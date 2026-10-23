#include "game_restart.h"
#include "knife_swap.h"
#include "skins.h"

#include "apply_internal.h"

#include <algorithm>
#include <atomic>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

// Weapon skins + knives, driven from the plugin's on_tick (the core's GameFrame hook).
//
// Why polling instead of events/detours: engine events are currently disabled on the live build
// (CGameEventManager_Init capture crashed) and there is no detour on GiveNamedItem yet. Walking the
// 64 player controllers each tick is cheap, runs on the game thread, and — because our hook runs
// after the original GameFrame but before the snapshot for that frame is sent — a weapon created
// this frame (buy, round-start give, pickup) gets its skin before any client ever sees it.
// Weapons we can only decorate later (loadout still loading) are re-networked with
// MarkEntityFullyChanged.

namespace skins {
namespace {

struct Offsets {
  bool ok = false;
  // Controller / pawn.
  int ctrl_steamId = -1;     // CBasePlayerController::m_steamID (uint64)
  int ctrl_playerPawn = -1;  // CCSPlayerController::m_hPlayerPawn (CHandle)
  int ent_teamNum = -1;      // CBaseEntity::m_iTeamNum (uint8)
  int ent_lifeState = -1;    // CBaseEntity::m_lifeState (uint8, 0 = alive)
  int pawn_weaponServices = -1;  // CBasePlayerPawn::m_pWeaponServices (ptr)
  int ws_myWeapons = -1;         // CPlayer_WeaponServices::m_hMyWeapons (CNetworkUtlVectorBase<CHandle>)
  // Weapon (CEconEntity).
  int econ_attrMgr = -1;     // m_AttributeManager (CAttributeContainer, embedded)
  int econ_fbPaint = -1;     // m_nFallbackPaintKit
  int econ_fbSeed = -1;      // m_nFallbackSeed
  int econ_fbWear = -1;      // m_flFallbackWear
  int econ_fbStatTrak = -1;  // m_nFallbackStatTrak
  int econ_xuidLow = -1;     // m_OriginalOwnerXuidLow
  int econ_xuidHigh = -1;    // m_OriginalOwnerXuidHigh
  int cont_item = -1;        // CAttributeContainer::m_Item (CEconItemView, embedded)
  // CEconItemView.
  int item_defIndex = -1;    // uint16
  int item_quality = -1;     // int32
  int item_id = -1;          // uint64
  int item_idHigh = -1;      // uint32
  int item_idLow = -1;       // uint32
  int item_accountId = -1;   // uint32
  int item_initialized = -1; // bool
  int item_attrList = -1;    // CAttributeList (embedded)
  int item_netAttrs = -1;    // CAttributeList (embedded)
  int item_customName = -1;  // char[161]
};

Offsets& Off() {
  static Offsets o;
  return o;
}

int F(const char* cls, const char* field) { return SchemaOffset(cls, field); }

bool ResolveOffsets() {
  auto& o = Off();
  if (o.ok) return true;
  o.ctrl_steamId = F("CBasePlayerController", "m_steamID");
  o.ctrl_playerPawn = F("CCSPlayerController", "m_hPlayerPawn");
  o.ent_teamNum = F("CBaseEntity", "m_iTeamNum");
  o.ent_lifeState = F("CBaseEntity", "m_lifeState");
  o.pawn_weaponServices = F("CBasePlayerPawn", "m_pWeaponServices");
  o.ws_myWeapons = F("CPlayer_WeaponServices", "m_hMyWeapons");
  o.econ_attrMgr = F("CEconEntity", "m_AttributeManager");
  o.econ_fbPaint = F("CEconEntity", "m_nFallbackPaintKit");
  o.econ_fbSeed = F("CEconEntity", "m_nFallbackSeed");
  o.econ_fbWear = F("CEconEntity", "m_flFallbackWear");
  o.econ_fbStatTrak = F("CEconEntity", "m_nFallbackStatTrak");
  o.econ_xuidLow = F("CEconEntity", "m_OriginalOwnerXuidLow");
  o.econ_xuidHigh = F("CEconEntity", "m_OriginalOwnerXuidHigh");
  o.cont_item = F("CAttributeContainer", "m_Item");
  o.item_defIndex = F("CEconItemView", "m_iItemDefinitionIndex");
  o.item_quality = F("CEconItemView", "m_iEntityQuality");
  o.item_id = F("CEconItemView", "m_iItemID");
  o.item_idHigh = F("CEconItemView", "m_iItemIDHigh");
  o.item_idLow = F("CEconItemView", "m_iItemIDLow");
  o.item_accountId = F("CEconItemView", "m_iAccountID");
  o.item_initialized = F("CEconItemView", "m_bInitialized");
  o.item_attrList = F("CEconItemView", "m_AttributeList");
  o.item_netAttrs = F("CEconItemView", "m_NetworkedDynamicAttributes");
  o.item_customName = F("CEconItemView", "m_szCustomName");

  const int required[] = {o.ctrl_steamId,  o.ctrl_playerPawn, o.ent_teamNum,  o.ent_lifeState, o.pawn_weaponServices,
                          o.ws_myWeapons,  o.econ_attrMgr,    o.econ_fbPaint, o.econ_fbSeed,   o.econ_fbWear,
                          o.cont_item,     o.item_defIndex,   o.item_idHigh,  o.item_idLow,    o.item_netAttrs,
                          o.item_attrList};
  o.ok = std::all_of(std::begin(required), std::end(required), [](int v) { return v >= 0; });
  return o.ok;
}

template <typename T>
T Rd(const void* base, int off) {
  T v{};
  std::memcpy(&v, static_cast<const unsigned char*>(base) + off, sizeof(T));
  return v;
}

template <typename T>
void Wr(void* base, int off, T v) {
  if (off < 0) return;
  std::memcpy(static_cast<unsigned char*>(base) + off, &v, sizeof(T));
}

float BitsAsFloat(uint32_t u) {
  float f = 0.0f;
  std::memcpy(&f, &u, sizeof(f));
  return f;
}

// Fake, per-server-unique item IDs. A non-zero item ID makes the client treat the item as a
// "real" econ item and read the paint attributes instead of rendering the stock weapon.
uint64_t NextItemId() {
  static uint64_t next = 0x10000;
  return ++next;
}

// The stock knives' item definitions: CT's "weapon_knife" and T's "weapon_knife_t".
constexpr int kStockKnifeCt = kStockKnifeCtDef;
constexpr int kStockKnifeT = kStockKnifeTDef;

bool IsKnifeDesigner(const char* dn) {
  if (!dn) return false;
  return std::strncmp(dn, "weapon_knife", 12) == 0 || std::strcmp(dn, "weapon_bayonet") == 0;
}

}  // namespace

namespace detail {

bool EconOffsetsReady() { return ResolveOffsets(); }
void ResetOffsets() { Off() = Offsets{}; }
int ItemDefIndexOffset() { return Off().item_defIndex; }
int ItemInitializedOffset() { return Off().item_initialized; }

// Writes paint/seed/wear/StatTrak/nametag onto an econ item view + the owning entity's fallback
// fields. Used for weapons (owner = the weapon entity) and gloves (owner = nullptr).
uint64_t ItemIdOf(void* itemView) { return Rd<uint64_t>(itemView, Off().item_id); }

uint64_t WritePaint(void* ownerEconEntity, void* itemView, uint64_t steamid64, const WeaponSkinEntry& skin) {
  auto& o = Off();
  const uint64_t itemId = NextItemId();
  Wr<uint64_t>(itemView, o.item_id, itemId);
  Wr<uint32_t>(itemView, o.item_idLow, static_cast<uint32_t>(itemId & 0xFFFFFFFFu));
  Wr<uint32_t>(itemView, o.item_idHigh, static_cast<uint32_t>(itemId >> 32));
  Wr<uint32_t>(itemView, o.item_accountId, static_cast<uint32_t>(steamid64 & 0xFFFFFFFFu));

  if (ownerEconEntity) {
    Wr<int32_t>(ownerEconEntity, o.econ_fbPaint, skin.paint_id);
    Wr<int32_t>(ownerEconEntity, o.econ_fbSeed, skin.seed);
    Wr<float>(ownerEconEntity, o.econ_fbWear, skin.wear);
    if (skin.stattrak_enabled) Wr<int32_t>(ownerEconEntity, o.econ_fbStatTrak, std::max(0, skin.stattrak_count));
  }

  void* lists[2] = {static_cast<unsigned char*>(itemView) + o.item_netAttrs,
                    static_cast<unsigned char*>(itemView) + o.item_attrList};
  for (void* list : lists) {
    AttrSetOrAddByName(list, "set item texture prefab", static_cast<float>(skin.paint_id));
    AttrSetOrAddByName(list, "set item texture seed", static_cast<float>(skin.seed));
    AttrSetOrAddByName(list, "set item texture wear", skin.wear);
    if (skin.stattrak_enabled) {
      AttrSetOrAddByName(list, "kill eater", BitsAsFloat(static_cast<uint32_t>(std::max(0, skin.stattrak_count))));
      AttrSetOrAddByName(list, "kill eater score type", 0.0f);
    }
  }

  if (o.item_customName >= 0 && !skin.nametag.empty()) {
    char* dst = reinterpret_cast<char*>(itemView) + o.item_customName;
    const size_t n = std::min<size_t>(skin.nametag.size(), 160);
    std::memcpy(dst, skin.nametag.data(), n);
    dst[n] = '\0';
  }
  return itemId;
}

void* EconItemViewOfWeapon(void* weapon) {
  auto& o = Off();
  return static_cast<unsigned char*>(weapon) + o.econ_attrMgr + o.cont_item;
}

}  // namespace detail

using detail::EconItemViewOfWeapon;
using detail::WritePaint;
using detail::ItemIdOf;

namespace {

enum class WeaponResult { kDone, kRetry };

// Set by ProcessWeapon when it swapped a knife to another knife (ChangeSubclass) this call.
bool g_lastSwappedKnife = false;
// Knife entities whose knife we swapped in (knife_swap.h): only those go back to stock.
KnifeSwapLedger g_knifeSwaps;
// What we last painted on each weapon (by entity handle): the same paint is not written again.
std::unordered_map<uint32_t, PaintStamp> g_weaponStamps;

// Paint kits with items_game `use_legacy_model 1` (CS:GO-era UVs). In CS2 they only render
// correctly on the weapon's legacy model: bodygroup "body" = 1 (what WeaponPaints does with
// AcceptInput("SetBodygroup", "body,1")). Regenerate with scripts/gen_legacy_paint_kits.py.
constexpr int kLegacyPaintKits[] = {
#include "legacy_paint_kits.inc"
};

bool IsLegacyPaintKit(int paint) {
  return std::binary_search(std::begin(kLegacyPaintKits), std::end(kLegacyPaintKits), paint);
}

// Returns true when done (applied or definitively impossible), false to retry on a later tick.
// m_CBodyComponent -> CBodyComponentSkeletonInstance::m_skeletonInstance -> m_modelState.m_MeshGroupMask.
// Written directly (what WeaponPaints does): unlike SetBodygroup it needs no loaded model, so a
// weapon that was just created or bought gets the legacy model before it is networked.
bool WriteMeshGroupMask(void* ent, uint64_t mask) {
  static const int kBody = SchemaOffset("CBaseEntity", "m_CBodyComponent");
  static const int kSkel = SchemaOffset("CBodyComponentSkeletonInstance", "m_skeletonInstance");
  static const int kState = SchemaOffset("CSkeletonInstance", "m_modelState");
  static const int kMask = SchemaOffset("CModelState", "m_MeshGroupMask");
  if (kBody < 0 || kSkel < 0 || kState < 0 || kMask < 0) return false;
  void* body = Rd<void*>(ent, kBody);
  if (!body) return false;
  Wr<uint64_t>(static_cast<unsigned char*>(body) + kSkel + kState, kMask, mask);
  return true;
}

bool ApplyLegacyBody(void* weapon, uint64_t steamid64) {
  const bool masked = WriteMeshGroupMask(weapon, 2);  // mesh group 1 = the legacy model
  const auto r = SetBodygroupByName(weapon, "body", 1);
  if (r == RU_BODYGROUP_NO_MODEL && !masked) return false;
  if (DebugOn()) {
    Log(RU_LOG_DEBUG, "legacy model for %s owner=%llu: mask %s, body=1 %s", EntityDesignerName(weapon),
          static_cast<unsigned long long>(steamid64), masked ? "2" : "unavailable", BodygroupResultName(r));
  }
  return true;
}

// set_player_paint (readyup.skins.v1): steamid64 -> the paint every new weapon gets.
std::unordered_map<uint64_t, WeaponSkinEntry> g_paintOverride;
std::unordered_map<uint64_t, std::string> g_overrideNameTag;  // set_player_name_tag
long long g_applied = 0;  // weapons painted / knives swapped this session (for skins_status)

// force: write the paint even when the weapon already has it (the planned repaint of a swapped
// knife a quarter second later, which the client needs).
WeaponResult ProcessWeapon(void* weapon, uint64_t steamid64, int team, bool late, bool* legacyPending,
                           bool force = false) {
  g_lastSwappedKnife = false;
  auto& o = Off();
  const bool isKnife = IsKnifeDesigner(EntityDesignerName(weapon));
  const auto ov = g_paintOverride.find(steamid64);
  // A per-player paint needs no loadout: paint it now, before the weapon is networked. Knives still
  // wait for it, since the knife model comes from the loadout.
  // paint_kit -1 ("the player's own skin") needs the loadout like no override at all.
  const bool ownPaint = ov != g_paintOverride.end() && ov->second.paint_id < 0;
  if ((ov == g_paintOverride.end() || isKnife || ownPaint) && !IsLoaded(steamid64)) return WeaponResult::kRetry;

  // Weapons picked up from someone else keep their original owner's look.
  const uint64_t xuid = (static_cast<uint64_t>(Rd<uint32_t>(weapon, o.econ_xuidHigh)) << 32) |
                        Rd<uint32_t>(weapon, o.econ_xuidLow);
  if (xuid != 0 && xuid != steamid64) return WeaponResult::kDone;
  if (xuid == 0) {
    Wr<uint32_t>(weapon, o.econ_xuidLow, static_cast<uint32_t>(steamid64 & 0xFFFFFFFFu));
    Wr<uint32_t>(weapon, o.econ_xuidHigh, static_cast<uint32_t>(steamid64 >> 32));
  }

  void* item = EconItemViewOfWeapon(weapon);
  int defindex = Rd<uint16_t>(item, o.item_defIndex);
  if (defindex <= 0) return WeaponResult::kDone;

  bool changed = false;
  bool swappedKnife = false;
  int paintUsed = 0;
  float wearUsed = 0.0f;
  const bool fromOverride = ov != g_paintOverride.end();
  const uint32_t handle = g_api->entity_handle_of(g_api->self, weapon);
  // One paint write, skipped when this weapon already shows exactly this paint from us.
  auto paintOnce = [&](const WeaponSkinEntry& paint) {
    if (!force && !swappedKnife && handle != 0xFFFFFFFFu) {
      const auto st = g_weaponStamps.find(handle);
      if (st != g_weaponStamps.end() &&
          StampMatches(st->second, ItemIdOf(item), defindex, paint.paint_id, paint.seed, paint.wear)) {
        return false;
      }
    }
    const uint64_t id = WritePaint(weapon, item, steamid64, paint);
    if (handle != 0xFFFFFFFFu) g_weaponStamps[handle] = PaintStamp{id, defindex, paint.paint_id, paint.seed, paint.wear};
    return true;
  };
  if (isKnife) {
    std::optional<int> wantDef;
    if (const auto want = FindKnifeClassname(steamid64, team)) wantDef = KnifeClassnameToDefindex(*want);
    switch (DecideKnife(wantDef, defindex, handle != 0xFFFFFFFFu && g_knifeSwaps.Swapped(handle, steamid64))) {
      case KnifeAction::Swap:
        // Same as the "ChangeSubclass" input / `subclass_change <defindex>`: swaps weapon VData
        // (model, animations) to the chosen knife.
        if (ChangeSubclass(weapon, std::to_string(*wantDef).c_str())) {
          Wr<uint16_t>(item, o.item_defIndex, static_cast<uint16_t>(*wantDef));
          defindex = *wantDef;
          changed = true;
          swappedKnife = true;
          g_lastSwappedKnife = true;
          if (handle != 0xFFFFFFFFu) g_knifeSwaps.MarkSwapped(handle, steamid64);
        } else if (DebugOn()) {
          Log(RU_LOG_DEBUG, "ChangeSubclass unavailable; keeping default knife for %llu",
                static_cast<unsigned long long>(steamid64));
        }
        break;
      case KnifeAction::RevertStock: {
        // No platform knife equipped any more, on a knife we swapped in: the stock knife, not the
        // last one swapped in (unequipping left the old knife in hand, NTLAN, 2026-10-16). A knife
        // the game gave (the player's Steam inventory knife) never gets here.
        const int stock = team == 2 ? kStockKnifeT : kStockKnifeCt;
        if (ChangeSubclass(weapon, std::to_string(stock).c_str())) {
          Wr<uint16_t>(item, o.item_defIndex, static_cast<uint16_t>(stock));
          Wr<int32_t>(item, o.item_quality, 0);
          defindex = stock;
          changed = true;
          g_knifeSwaps.Forget(handle);
        }
        break;
      }
      case KnifeAction::Keep: break;
    }
    // Quality 3 = "unusual" (the ★ prefix); non-default knives always carry it.
    if (wantDef) Wr<int32_t>(item, o.item_quality, 3);
  }

  if (ov != g_paintOverride.end()) {
    WeaponSkinEntry paint = ov->second;
    bool write = true;
    if (ownPaint) {  // the loadout's skin for this weapon, with the override's name tag
      const auto own = FindWeaponSkin(steamid64, team, defindex);
      paint = own ? *own : WeaponSkinEntry{};
      // No platform skin for it: the weapon keeps what the game gave (the player's Steam
      // inventory skin, or stock). Writing paint 0 replaced Steam skins with stock (2026-10-17).
      write = paint.paint_id > 0;
    }
    if (write) {
      const auto tag = g_overrideNameTag.find(steamid64);
      if (tag != g_overrideNameTag.end()) paint.nametag = tag->second;
      if (paintOnce(paint)) {
        paintUsed = paint.paint_id;
        wearUsed = paint.wear;
        changed = true;
        if (IsLegacyPaintKit(paint.paint_id) && !ApplyLegacyBody(weapon, steamid64)) *legacyPending = true;
      }
    }
  } else if (const auto skin = FindWeaponSkin(steamid64, team, defindex)) {
    if (skin->paint_id > 0 && paintOnce(*skin)) {
      paintUsed = skin->paint_id;
      wearUsed = skin->wear;
      changed = true;
      if (IsLegacyPaintKit(skin->paint_id) && !ApplyLegacyBody(weapon, steamid64)) *legacyPending = true;
    }
  }

  if (changed) ++g_applied;
  // A knife swapped to another knife (ChangeSubclass) is sent in full even on its first
  // tick: the client kept the new model unpainted otherwise (a side-specific karambit on
  // T showed vanilla while the server had painted it, 2026-10-15).
  if (changed && (late || swappedKnife)) MarkEntityFullyChanged(weapon);
  if (changed && DebugOn()) {
    Log(RU_LOG_DEBUG, "applied to %s def=%d owner=%llu paint=%d wear=%.2f%s%s", EntityDesignerName(weapon), defindex,
          static_cast<unsigned long long>(steamid64), paintUsed, static_cast<double>(wearUsed),
          fromOverride ? " (player paint)" : "", late ? " (late)" : swappedKnife ? " (knife swap)" : "");
  }
  return WeaponResult::kDone;
}

struct PlayerState {
  uint64_t steamid64 = 0;
  uint32_t pawnHandle = 0xFFFFFFFFu;
  bool wasAlive = false;
  int cosmeticsPass = -1;      // next index into kCosmeticPassTicks, -1 = none pending
  long long spawnTick = 0;
};

struct WeaponState {
  long long firstSeen = 0;
  long long lastSeen = 0;
  bool done = false;
  bool legacyPending = false;  // legacy "body" bodygroup still to set (model not loaded yet)
  // Painted by another plugin (readyup.skins.v1 paint_weapon): no loadout over it until handed back.
  bool external = false;
  bool externalLegacy = false;  // that paint used the legacy model (reset on hand back)
  // Held by a bot first. A bot has no SteamID, so its weapons carry no original owner and would
  // look new to the player who picks one up (a CT got their T-side AK skin on a bot's AK).
  bool botOwned = false;
  // A knife swapped to another knife is painted again here: the client rebuilds the model from
  // the new VData after the first paint and drew it vanilla (a T-side karambit, 2026-10-15).
  long long repaintAt = 0;
};

// Cosmetics (agent model, gloves, default_gloves bodygroup) are applied on these ticks after the
// spawn frame. The game can re-apply its own model/gloves shortly after spawning, and a freshly
// SetModel'd agent model may not be loaded (no bodygroups) on the first frames.
constexpr long long kCosmeticPassTicks[] = {0, 1, 8, 32};
constexpr int kCosmeticPassCount = static_cast<int>(sizeof(kCosmeticPassTicks) / sizeof(kCosmeticPassTicks[0]));

PlayerState g_players[65];
// player_spawn seen for the slot (RequestSpawnCosmetics): run the cosmetics passes again on the
// next alive tick. mp_restartgame and round starts respawn living players on the same pawn, so
// the dead -> alive / new pawn check alone misses them.
bool g_spawnRequested[65] = {};
// `.skins reload` (RequestReapply): at this tick (0 = none) the slot's cosmetics run again and
// the weapons it holds are repainted. A second after the request, so the re-read loadout is in.
long long g_reapplyAt[65] = {};
// Repaint what the player holds (no re-give): their first loadout arrived after they spawned.
std::atomic<bool> g_repaintNow[65] = {};
// The last round restart (round_prestart / round_start: mp_restartgame, a match going live, a
// new round). A re-give within kRestartQuietTicks of one is put off: weapons re-given while the
// restart deletes and recreates them crashed a server (WriteEnterPVS: GetEntServerClass failed,
// 2026-10-14, a loadout that arrived as the match went live).
long long g_roundChangeTick = -1000000;
constexpr long long kRestartQuietTicks = 96;
// This tick is inside the quiet window (set once per tick in the tick function).
bool g_restartQuietNow = false;
// CS2 has a round restart pending this tick (GameRestartPending).
bool g_restartPendingNow = false;
// Dev only (skins_debug_as, debug=1): a bot slot decorated with a real player's loadout, so the
// whole apply path can be exercised without a human client.
uint64_t g_debugAs[65] = {};
std::unordered_map<uint32_t, WeaponState> g_weapons;  // by entity handle (index + serial)
long long g_tick = 0;
long long g_activeSince = 0;  // first tick of the current map/session we saw entities
void* g_lastEntitySystemWorld = nullptr;

// Loadout loads are async; give up decorating a weapon/spawn after this many ticks (~10s @64).
constexpr long long kRetryTicks = 640;

bool DisabledByEnv() {  // READYUP_DISABLE_SKINS=1: keep the plugin loaded but idle
  static const bool disabled = [] {
    const char* v = std::getenv("READYUP_DISABLE_SKINS");
    return v && *v && std::strcmp(v, "0") != 0;
  }();
  return disabled;
}

// ---- refresh held weapons (player_give_item) --------------------------------------------------
// A paint written after a weapon was networked keeps the old look on clients. So `.skins reload`
// and midas (a new finish, a player given Midas) swap the held weapons for new ones: note them, remove them, and
// give fresh ones two ticks later (after the removal), which ProcessPlayer paints on first sight.
// Clip and reserve ammo carry over; the weapon in hand is given last. Grenades, C4 and weapons
// another player owned first are left alone.
struct PendingGive {
  std::string cls;
  int clip = -1;
  int reserve = -1;
};
struct PendingRefresh {
  uint64_t steamid64 = 0;
  long long at = 0;  // g_tick to give at (0 = none)
  std::vector<PendingGive> gives;
};
PendingRefresh g_refresh[64];
bool g_giveBroken = false;  // a give failed: no more refreshes this session (repaint instead)

bool CanRefresh() {
  return !g_giveBroken && g_api && RU_API_HAS(g_api, player_give_item) && g_api->player_give_item && RU_API_HAS(g_api, entity_remove) &&
         g_api->entity_remove;
}

bool SkipOnRefresh(const std::string& dn) {
  static const char* const kSkip[] = {"weapon_c4",       "weapon_hegrenade", "weapon_flashbang", "weapon_smokegrenade",
                                      "weapon_molotov",  "weapon_incgrenade", "weapon_decoy",    "weapon_tagrenade",
                                      "weapon_healthshot", "weapon_breachcharge", "weapon_shield", "weapon_snowball"};
  if (dn.compare(0, 7, "weapon_") != 0) return true;
  for (const char* s : kSkip) {
    if (dn == s) return true;
  }
  return false;
}

// The name `give` needs: a few items share their entity class with another weapon.
std::string GiveName(const std::string& dn, int defindex, int team) {
  if (IsKnifeDesigner(dn.c_str())) return team == 2 ? "weapon_knife_t" : "weapon_knife";
  switch (defindex) {
    case 23: return "weapon_mp5sd";
    case 60: return "weapon_m4a1_silencer";
    case 61: return "weapon_usp_silencer";
    case 63: return "weapon_cz75a";
    case 64: return "weapon_revolver";
    default: return dn;
  }
}

bool RefreshHeldWeapons(int slot) {
  if (slot < 0 || slot >= 64 || !CanRefresh()) return false;
  auto& o = Off();
  static const int kActive = SchemaOffset("CPlayer_WeaponServices", "m_hActiveWeapon");
  static const int kClip = SchemaOffset("CBasePlayerWeapon", "m_iClip1");
  static const int kReserve = SchemaOffset("CBasePlayerWeapon", "m_pReserveAmmo");
  void* controller = EntityByIndex(slot + 1);
  if (!controller) return false;
  const uint64_t steamid64 = Rd<uint64_t>(controller, o.ctrl_steamId);
  void* pawn = EntityFromHandle(Rd<uint32_t>(controller, o.ctrl_playerPawn));
  if (!pawn || Rd<uint8_t>(pawn, o.ent_lifeState) != 0) return false;
  const int team = Rd<uint8_t>(pawn, o.ent_teamNum);
  void* ws = Rd<void*>(pawn, o.pawn_weaponServices);
  if (!ws) return false;
  const int count = Rd<int32_t>(ws, o.ws_myWeapons);
  const uint32_t* handles = Rd<const uint32_t*>(ws, o.ws_myWeapons + 8);
  if (!handles || count <= 0 || count > 64) return false;
  const uint32_t active = kActive >= 0 ? Rd<uint32_t>(ws, kActive) : 0xFFFFFFFFu;

  PendingRefresh pr;
  pr.steamid64 = steamid64;
  std::vector<void*> remove;
  PendingGive inHand;
  bool haveInHand = false;
  for (int i = 0; i < count; ++i) {
    void* weapon = EntityFromHandle(handles[i]);
    if (!weapon) continue;
    const char* dnp = EntityDesignerName(weapon);
    if (!dnp) continue;
    const std::string dn = dnp;
    if (SkipOnRefresh(dn)) continue;
    const uint64_t xuid = (static_cast<uint64_t>(Rd<uint32_t>(weapon, o.econ_xuidHigh)) << 32) |
                          Rd<uint32_t>(weapon, o.econ_xuidLow);
    if (xuid != 0 && xuid != steamid64) continue;
    PendingGive g;
    g.cls = GiveName(dn, Rd<uint16_t>(EconItemViewOfWeapon(weapon), o.item_defIndex), team);
    if (kClip >= 0) g.clip = Rd<int32_t>(weapon, kClip);
    if (kReserve >= 0) g.reserve = Rd<int32_t>(weapon, kReserve);
    remove.push_back(weapon);
    if (handles[i] == active) {
      inHand = g;
      haveInHand = true;
    } else {
      pr.gives.push_back(g);
    }
  }
  if (haveInHand) pr.gives.push_back(inHand);
  if (pr.gives.empty()) return false;
  for (void* w : remove) g_api->entity_remove(g_api->self, w);
  pr.at = g_tick + 2;
  g_refresh[slot] = std::move(pr);
  if (DebugOn()) Log(RU_LOG_DEBUG, "refresh: slot %d, %zu weapon(s)", slot, g_refresh[slot].gives.size());
  return true;
}

// The player in this slot holds a knife now (a live entity in their weapon list). A refresh whose
// knife removal has not gone through, or a second refresh racing the first, would otherwise give
// another one: CS2 hands out a second knife rather than replacing it (two knives on one side,
// NTLAN 2026-10-16).
bool HoldsKnife(int slot) {
  auto& o = Off();
  void* controller = EntityByIndex(slot + 1);
  if (!controller) return false;
  void* pawn = EntityFromHandle(Rd<uint32_t>(controller, o.ctrl_playerPawn));
  if (!pawn) return false;
  void* ws = Rd<void*>(pawn, o.pawn_weaponServices);
  if (!ws) return false;
  const int count = Rd<int32_t>(ws, o.ws_myWeapons);
  const uint32_t* handles = Rd<const uint32_t*>(ws, o.ws_myWeapons + 8);
  if (!handles || count <= 0 || count > 64) return false;
  for (int i = 0; i < count; ++i) {
    void* weapon = EntityFromHandle(handles[i]);
    if (weapon && IsKnifeDesigner(EntityDesignerName(weapon))) return true;
  }
  return false;
}

// Before the player loop, so the new weapons are painted in the tick they are created. Waits for
// the loadout (up to ~2 s after a `.skins reload`) so they get it on first sight.
void RunPendingRefresh() {
  static const int kClip = SchemaOffset("CBasePlayerWeapon", "m_iClip1");
  static const int kReserve = SchemaOffset("CBasePlayerWeapon", "m_pReserveAmmo");
  for (int slot = 0; slot < 64; ++slot) {
    PendingRefresh& pr = g_refresh[slot];
    if (pr.at == 0 || g_tick < pr.at) continue;
    if (!IsLoaded(pr.steamid64) && g_tick < pr.at + 128) continue;
    const bool hasKnife = HoldsKnife(slot);
    for (const PendingGive& g : pr.gives) {
      if (hasKnife && IsKnifeDesigner(g.cls.c_str())) {
        if (DebugOn()) Log(RU_LOG_DEBUG, "refresh: slot %d still holds a knife; not giving another", slot);
        continue;
      }
      void* w = g_api->player_give_item(g_api->self, slot, g.cls.c_str());
      if (!w) {
        Log(RU_LOG_WARN, "refresh: could not give %s to slot %d; weapon refresh off until restart", g.cls.c_str(), slot);
        g_giveBroken = true;
        continue;
      }
      if (kClip >= 0 && g.clip >= 0) Wr<int32_t>(w, kClip, g.clip);
      if (kReserve >= 0 && g.reserve >= 0) Wr<int32_t>(w, kReserve, g.reserve);
    }
    pr = PendingRefresh{};
  }
}

void ProcessPlayer(int slot, void* controller) {
  auto& o = Off();
  PlayerState& ps = g_players[slot];

  uint64_t steamid64 = Rd<uint64_t>(controller, o.ctrl_steamId);
  if (steamid64 == 0 && g_debugAs[slot] != 0 && DebugOn()) steamid64 = g_debugAs[slot];
  if (steamid64 == 0) {  // bots / not yet authenticated
    ps = PlayerState{};
    // Remember what a bot holds: a player who picks it up gets it as it is (stock).
    if (void* botPawn = EntityFromHandle(Rd<uint32_t>(controller, o.ctrl_playerPawn))) {
      if (void* bws = Rd<void*>(botPawn, o.pawn_weaponServices)) {
        const int n = Rd<int32_t>(bws, o.ws_myWeapons);
        const uint32_t* hs = Rd<const uint32_t*>(bws, o.ws_myWeapons + 8);
        if (hs && n > 0 && n <= 64) {
          for (int i = 0; i < n; ++i) {
            WeaponState& st = g_weapons[hs[i]];
            if (st.firstSeen == 0) st.firstSeen = g_tick;
            st.lastSeen = g_tick;
            if (!st.done) st.botOwned = true;
          }
        }
      }
    }
    return;
  }
  if (ps.steamid64 != steamid64) ps = PlayerState{steamid64};

  // Prefetch roughly once a second; MaybeRefreshAsync is TTL-gated anyway.
  if (((g_tick + slot) & 63) == 0) MaybeRefreshAsync(steamid64);

  const uint32_t pawnHandle = Rd<uint32_t>(controller, o.ctrl_playerPawn);
  void* pawn = EntityFromHandle(pawnHandle);
  if (!pawn) {
    ps.wasAlive = false;
    return;
  }
  const int team = Rd<uint8_t>(pawn, o.ent_teamNum);
  const bool alive = Rd<uint8_t>(pawn, o.ent_lifeState) == 0;
  bool repaintHeld = false;
  bool refreshed = false;
  // Around a round restart no weapon is given or removed: one made or removed while CS2 deletes
  // and recreates the round's entities crashed servers and clients (2026-10-14, WriteEnterPVS /
  // CopyExistingEntity). Gun paint is untouched: the round's new weapons are painted on their first
  // tick as always (holding that too left a vanilla knife, 2026-10-14). Gloves and knife swaps wait
  // (below).
  const bool restartQuiet = g_restartQuietNow;
  if (g_reapplyAt[slot] != 0 && g_tick >= g_reapplyAt[slot] && alive && restartQuiet) {
    g_reapplyAt[slot] = g_tick + 8;
  }
  if (g_reapplyAt[slot] != 0 && g_tick >= g_reapplyAt[slot] && alive) {
    g_reapplyAt[slot] = 0;
    g_spawnRequested[slot] = true;
    // New weapons get the loadout when they are created; without player_give_item, repaint.
    refreshed = RefreshHeldWeapons(slot);
    repaintHeld = !refreshed;
  }
  if (team != 2 && team != 3) {
    ps.wasAlive = false;
    return;
  }
  // The player's first loadout came after their spawn (a server without a match: it is asked
  // for on connect). Their weapons are given again, as for an equip on the site: a knife swapped
  // in place on a weapon the client already drew stayed vanilla on the client (CT M9 on a tryout
  // server, 2026-10-15; a side switch, i.e. new weapons, fixed it). Around a round restart, or
  // without player_give_item, the held weapons are repainted instead.
  if (alive && g_repaintNow[slot].exchange(false)) {
    if (!restartQuiet && !refreshed) refreshed = RefreshHeldWeapons(slot);
    if (!refreshed) repaintHeld = true;
    g_spawnRequested[slot] = true;
    if (DebugOn()) Log(RU_LOG_DEBUG, "first loadout after spawn: %s slot %d", refreshed ? "re-giving" : "repainting", slot);
  }

  if (alive && (!ps.wasAlive || ps.pawnHandle != pawnHandle || g_spawnRequested[slot])) {
    // Fresh spawn (or a respawn on the same pawn): apply now and on the follow-up passes.
    ps.cosmeticsPass = 0;
    ps.spawnTick = g_tick;
    g_spawnRequested[slot] = false;
  }
  ps.wasAlive = alive;
  ps.pawnHandle = pawnHandle;
  if (!alive) return;

  // Gloves and the agent model rebuild things on the client (the glove counter, a full re-send of
  // the pawn). Done while CS2 restarts the round, the client crashed (CopyExistingEntity, a player
  // joining with gloves on, 2026-10-14): the passes start over once the restart has settled.
  // Only while the restart is pending: with no full re-send of the pawn any more (gloves the
  // WeaponPaints way), waiting the whole quiet window after it only showed stock gloves and knife
  // for a second after every restart.
  if (g_restartPendingNow && ps.cosmeticsPass >= 0) ps.spawnTick = g_tick;
  if (!g_restartPendingNow && ps.cosmeticsPass >= 0 && ps.cosmeticsPass < kCosmeticPassCount &&
      g_tick - ps.spawnTick >= kCosmeticPassTicks[ps.cosmeticsPass]) {
    // Anything first seen on our first active tick may already have been networked.
    const bool late = g_tick > ps.spawnTick + 1 || ps.spawnTick == g_activeSince;
    if (ApplySpawnCosmetics(pawn, steamid64, team, late)) {
      if (++ps.cosmeticsPass >= kCosmeticPassCount) ps.cosmeticsPass = -1;
    } else if (g_tick - ps.spawnTick > kRetryTicks) {
      ps.cosmeticsPass = -1;  // loadout never loaded — give up for this spawn
    }
  }

  if (refreshed) return;  // the new weapons arrive in two ticks
  void* ws = Rd<void*>(pawn, o.pawn_weaponServices);
  if (!ws) return;
  // CNetworkUtlVectorBase<CHandle<CBasePlayerWeapon>>: { int m_Size; <pad>; CHandle* m_pElements; }
  const int count = Rd<int32_t>(ws, o.ws_myWeapons);
  const uint32_t* handles = Rd<const uint32_t*>(ws, o.ws_myWeapons + 8);
  if (!handles || count <= 0 || count > 64) return;

  for (int i = 0; i < count; ++i) {
    const uint32_t h = handles[i];
    if (repaintHeld && !g_weapons[h].external) g_weapons[h] = WeaponState{};
    WeaponState& st = g_weapons[h];
    if (st.firstSeen == 0) st.firstSeen = g_tick;
    st.lastSeen = g_tick;
    if (st.done && st.repaintAt != 0 && g_tick >= st.repaintAt) {
      st.repaintAt = 0;
      if (void* again = EntityFromHandle(h)) {
        bool legacy = false;
        (void)ProcessWeapon(again, steamid64, team, /*late=*/true, &legacy, /*force=*/true);
        if (legacy) st.legacyPending = true;
      }
      continue;
    }
    if (st.done && !st.legacyPending) continue;
    if (st.botOwned) {  // picked up from a bot: keep its (stock) look
      st.done = true;
      continue;
    }
    void* weapon = EntityFromHandle(h);
    if (!weapon) continue;
    if (st.done) {
      // Legacy bodygroup retry (the weapon model wasn't loaded when we painted it).
      if (ApplyLegacyBody(weapon, steamid64)) {
        st.legacyPending = false;
        MarkEntityFullyChanged(weapon);
      } else if (g_tick - st.firstSeen > kRetryTicks) {
        st.legacyPending = false;
      }
      continue;
    }
    // A knife swap (ChangeSubclass) waits while the restart is pending; the restart hands out a new
    // knife, swapped on its first tick. No re-give: removing and giving weapons is what crashed
    // clients and servers (a re-give after each round start, 2026-10-14).
    if (g_restartPendingNow && IsKnifeDesigner(EntityDesignerName(weapon)) && FindKnifeClassname(steamid64, team)) {
      continue;
    }
    const bool late = g_tick > st.firstSeen || st.firstSeen == g_activeSince;
    if (ProcessWeapon(weapon, steamid64, team, late, &st.legacyPending) == WeaponResult::kDone ||
        g_tick - st.firstSeen > kRetryTicks) {
      st.done = true;
      if (g_lastSwappedKnife) st.repaintAt = g_tick + 16;  // about a quarter second
    }
  }
}

void PruneWeapons() {
  for (auto it = g_weapons.begin(); it != g_weapons.end();) {
    if (g_tick - it->second.lastSeen > 64 * 60) {
      g_knifeSwaps.Forget(it->first);
      g_weaponStamps.erase(it->first);
      it = g_weapons.erase(it);
    }
    else ++it;
  }
}

}  // namespace

namespace {
void RunWarmupRegive();
bool GameRestartPending();
}  // namespace

void GameFrameTick() {
  if (DisabledByEnv() || Inert()) return;  // Inert: ruleset valve (skins.h)
  ++g_tick;

  if (g_api->entity_system_status(g_api->self) != RU_ENTSYS_OK) return;
  if (!ResolveOffsets()) {
    static bool s_warned = false;
    if (!s_warned) {
      s_warned = true;
      Log(RU_LOG_WARN, "required schema fields missing; weapon skins disabled for this session");
    }
    return;
  }

  // New map => entity handles are reused from scratch; forget per-entity state.
  void* world = EntityByIndex(0);
  if (world != g_lastEntitySystemWorld) {
    g_lastEntitySystemWorld = world;
    g_activeSince = g_tick;
    g_weapons.clear();
    g_knifeSwaps.Clear();
    g_weaponStamps.clear();
    for (auto& p : g_players) p = PlayerState{};
    for (auto& r : g_reapplyAt) r = 0;
  }

  g_restartPendingNow = GameRestartPending();
  g_restartQuietNow = g_restartPendingNow || g_tick - g_roundChangeTick < kRestartQuietTicks;
  if (!g_restartQuietNow) RunPendingRefresh();
  RunWarmupRegive();
  for (int slot = 0; slot < 64; ++slot) {
    void* controller = EntityByIndex(slot + 1);
    if (!controller) {
      g_players[slot] = PlayerState{};
      continue;
    }
    const char* dn = EntityDesignerName(controller);
    if (!dn || std::strcmp(dn, "cs_player_controller") != 0) continue;
    ProcessPlayer(slot, controller);
  }

  if ((g_tick & 1023) == 0) PruneWeapons();
}

bool PaintWeaponExternal(uint32_t handle, uint64_t steamid64, int paintKit, float wear, int seed) {
  if (!ExternalPaintReady()) return false;
  void* weapon = EntityFromHandle(handle);
  if (!weapon) return false;
  auto& o = Off();
  void* item = EconItemViewOfWeapon(weapon);
  if (Rd<uint16_t>(item, o.item_defIndex) == 0) return false;
  WeaponState& st = g_weapons[handle];
  WeaponSkinEntry skin;
  if (paintKit > 0) {
    skin.paint_id = paintKit;
    skin.wear = std::min(1.0f, std::max(0.0f, wear));
    skin.seed = std::max(0, seed);
    WritePaint(weapon, item, steamid64, skin);
    if (st.firstSeen == 0) st.firstSeen = g_tick;
    st.lastSeen = g_tick;
    st.done = true;
    st.external = true;
    st.externalLegacy = IsLegacyPaintKit(paintKit);
    st.legacyPending = st.externalLegacy && !ApplyLegacyBody(weapon, steamid64);
  } else {
    // Hand back: stock paint now; processed again like a new weapon (the holder's loadout, if
    // any, on the next tick, re-networked because firstSeen < that tick).
    const bool wasLegacy = st.external && st.externalLegacy;
    WritePaint(weapon, item, steamid64, skin);
    if (wasLegacy) (void)SetBodygroupByName(weapon, "body", 0);
    st = WeaponState{};
    st.firstSeen = st.lastSeen = g_tick;
  }
  MarkEntityFullyChanged(weapon);
  if (DebugOn()) {
    Log(RU_LOG_DEBUG, "paint_weapon %s paint=%d owner=%llu", EntityDesignerName(weapon), paintKit,
        static_cast<unsigned long long>(steamid64));
  }
  return true;
}

bool SetPlayerPaintExternal(uint64_t steamid64, int paintKit, float wear, int seed) {
  if (steamid64 == 0) return false;
  if (paintKit == -1) {  // the player's own loadout paint, only the name tag (set_player_name_tag) added
    WeaponSkinEntry own;
    own.paint_id = -1;
    g_paintOverride[steamid64] = own;
    return true;
  }
  if (paintKit <= 0) {
    g_paintOverride.erase(steamid64);
    return true;
  }
  WeaponSkinEntry e;
  e.paint_id = paintKit;
  e.wear = std::min(1.0f, std::max(0.0f, wear));
  e.seed = std::max(0, seed);
  g_paintOverride[steamid64] = e;
  return true;
}

bool SetPlayerNameTagExternal(uint64_t steamid64, const char* nameTag) {
  if (steamid64 == 0) return false;
  std::string tag = nameTag ? nameTag : "";
  if (tag.size() > 40) tag.resize(40);
  if (tag.empty()) g_overrideNameTag.erase(steamid64);
  else g_overrideNameTag[steamid64] = tag;
  return true;
}

bool RefreshWeaponsExternal(int slot) { return !Inert() && ResolveOffsets() && RefreshHeldWeapons(slot); }

bool ExternalPaintReady() {
  return g_api && !DisabledByEnv() && !Inert() && g_api->entity_system_status(g_api->self) == RU_ENTSYS_OK &&
         ResolveOffsets();
}

void RequestSpawnCosmetics(int slot) {
  if (slot >= 0 && slot < 65) g_spawnRequested[slot] = true;
}

void NoteRoundChange() { g_roundChangeTick = g_tick; }

void RequestReapply(int slot) {
  if (slot >= 0 && slot < 65) g_reapplyAt[slot] = g_tick + 64;
}

// ---- warmup re-give (a new loadout from the platform) -----------------------------------------
std::mutex g_regiveMu;
std::unordered_set<uint64_t> g_regive;  // players whose loadout changed since the last frame

std::unordered_set<uint64_t> g_firstRepaint;  // guarded by g_regiveMu (filled off the game thread)

void QueueFirstLoadoutRepaint(uint64_t steamid64) {
  if (steamid64 == 0) return;
  std::lock_guard<std::mutex> lk(g_regiveMu);
  g_firstRepaint.insert(steamid64);
}

void QueueWarmupRegive(uint64_t steamid64) {
  if (steamid64 == 0) return;
  std::lock_guard<std::mutex> lk(g_regiveMu);
  g_regive.insert(steamid64);
}

namespace {
int g_offRulesPtr = -2, g_offWarmup = -2, g_offRestartTime = -2, g_offRoundStart = -2;
uint32_t g_rulesProxy = 0xFFFFFFFFu;
void* g_rulesPtr = nullptr;
long long g_rulesScanTick = -100000;

// CS2's own warmup (CCSGameRules::m_bWarmupPeriod): -1 unknown, 0 no, 1 yes.
int GameWarmup() {
  if (g_offRulesPtr == -2) {
    g_offRulesPtr = g_api->schema_offset(g_api->self, "CCSGameRulesProxy", "m_pGameRules");
    g_offWarmup = g_api->schema_offset(g_api->self, "CCSGameRules", "m_bWarmupPeriod");
    g_offRestartTime = g_api->schema_offset(g_api->self, "CCSGameRules", "m_flRestartRoundTime");
    g_offRoundStart = g_api->schema_offset(g_api->self, "CCSGameRules", "m_fRoundStartTime");
  }
  if (g_offRulesPtr < 0 || g_offWarmup < 0) return -1;
  void* proxy = g_rulesProxy != 0xFFFFFFFFu ? g_api->entity_from_handle(g_api->self, g_rulesProxy) : nullptr;
  if (!proxy) {
    g_rulesProxy = 0xFFFFFFFFu;
    if (g_tick - g_rulesScanTick < 640) return -1;  // look again in ~10 s
    g_rulesScanTick = g_tick;
    for (int i = 65; i < 4096 && !proxy; ++i) {
      void* e = g_api->entity_by_index(g_api->self, i);
      if (!e) continue;
      const char* cn = g_api->entity_classname(g_api->self, e);
      if (cn && std::strcmp(cn, "cs_gamerules") == 0) {
        proxy = e;
        g_rulesProxy = g_api->entity_handle_of(g_api->self, e);
      }
    }
    if (!proxy) return -1;
  }
  void* rules = nullptr;
  std::memcpy(&rules, static_cast<unsigned char*>(proxy) + g_offRulesPtr, sizeof(rules));
  if (!rules) return -1;
  g_rulesPtr = rules;
  return *(static_cast<unsigned char*>(rules) + g_offWarmup) ? 1 : 0;
}

// CS2 has a restart pending (mp_restartgame, a match going live), read from the round times of the
// game rules GameWarmup found (game_restart.h: not m_bGameRestart, which hud_steady rewrites).
bool GameRestartPending() {
  if (GameWarmup() < 0 || g_offRestartTime < 0 || g_offRoundStart < 0 || !g_rulesPtr) return false;
  float restartTime = 0.0f, roundStart = 0.0f;
  std::memcpy(&restartTime, static_cast<unsigned char*>(g_rulesPtr) + g_offRestartTime, sizeof(restartTime));
  std::memcpy(&roundStart, static_cast<unsigned char*>(g_rulesPtr) + g_offRoundStart, sizeof(roundStart));
  return RestartPending(restartTime, roundStart);
}

// Game thread: players with a new loadout get their weapons re-given, in warmup only.
void RunWarmupRegive() {
  std::unordered_set<uint64_t> due;
  std::unordered_set<uint64_t> first;
  {
    std::lock_guard<std::mutex> lk(g_regiveMu);
    due.swap(g_regive);
    first.swap(g_firstRepaint);
  }
  for (int slot = 0; slot < 64 && !first.empty(); ++slot) {
    if (g_players[slot].steamid64 != 0 && first.count(g_players[slot].steamid64)) g_repaintNow[slot] = true;
  }
  if (due.empty()) return;
  // CS2's own warmup, or Ready Up's (a match warmup runs with CS2's warmup ended): swap now.
  // Live (or unknown): the next spawn brings the new skins.
  if (GameWarmup() != 1 && !WarmupLike()) return;
  for (int slot = 0; slot < 64; ++slot) {
    if (g_players[slot].steamid64 != 0 && due.count(g_players[slot].steamid64)) {
      RequestReapply(slot);
      if (DebugOn()) Log(RU_LOG_DEBUG, "warmup: new loadout, re-giving slot %d", slot);
    }
  }
}
}  // namespace

bool SetDebugAs(int slot, uint64_t steamid64) {
  if (slot < 0 || slot >= 64) return false;
  g_debugAs[slot] = steamid64;
  g_players[slot] = PlayerState{};
  return true;
}

std::string ApplyStatus() {
  int players = 0;
  for (const auto& p : g_players) players += p.steamid64 != 0;
  return std::to_string(players) + " player(s) tracked, " + std::to_string(g_weapons.size()) + " weapon(s) seen, " +
         std::to_string(g_applied) + " applied" + (Off().ok ? "" : ", schema offsets not resolved yet") +
         (DisabledByEnv() ? ", DISABLED by READYUP_DISABLE_SKINS" : "");
}

}  // namespace skins

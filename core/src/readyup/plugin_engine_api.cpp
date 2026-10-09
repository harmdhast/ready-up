// Engine-facing members of the plugin API (v1.1): output, players, raw game event accessors,
// schema and entities, round-termination suppression, admin check. The loader
// (plugin_loader.cpp) owns registration bookkeeping; this file only adapts core services to
// the C ABI in core/include/readyup/plugin_api.h. Every member except is_admin is game thread
// only (detail::CheckGameThread).

#include "readyup/steam_ugc.h"
#include "readyup/admin_check.h"
#include "readyup/center_html.h"
#include "readyup/center_panel_owner.h"
#include "readyup/config.h"
#include "readyup/features.h"
#include "readyup/grenades.h"
#include "readyup/logging.h"
#include "readyup/license_status.h"
#include "readyup/plugin_loader.h"
#include "readyup/practice_engine.h"
#include "readyup/round_termination_hook.h"
#include "readyup/schema.h"
#include "readyup/sdk/igameevents.h"
#include "readyup/entity.h"
#include "readyup/slot_registry.h"
#include "readyup/status_feed.h"

#include "readyup/plugin_api.h"
#include "readyup/plugin_hooks.h"

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace readyup::plugins {
namespace {

using detail::CheckGameThread;
using sdk::CKV3MemberName;
using sdk::IGameEvent;

// ---- output ------------------------------------------------------------------------

// Who owns each player's center panel (game thread only; center_panel_owner.h).
CenterPanelOwners g_panels;

double PanelNow() {
  return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
}

uint64_t PanelOwnerId(ru_plugin* self) { return static_cast<uint64_t>(reinterpret_cast<uintptr_t>(self)); }

// 1 sent, -1 refused (a higher panel of another plugin is up), 0 failed.
int SendPanel(ru_plugin* self, int slot, const char* html, int seconds, int priority) {
  seconds = seconds > 0 ? seconds : 1;
  if (slot < 0 || slot >= CenterPanelOwners::kSlots) return 0;
  if (!g_panels.Claim(slot, PanelOwnerId(self), priority, seconds, PanelNow())) return -1;
  return PrintCenterHtmlToClientOnly(slot, html, seconds) ? 1 : 0;
}

int ApiCenterHtmlToSlotPrio(ru_plugin* self, int slot, const char* html, int seconds, int priority) {
  if (!CheckGameThread(self, "center_html_to_slot") || !html || !*html) return 0;
  return SendPanel(self, slot, html, seconds, priority);
}

int ApiCenterHtmlAllPrio(ru_plugin* self, const char* html, int seconds, int priority) {
  if (!CheckGameThread(self, "center_html_all") || !html || !*html) return 0;
  int sent = 0;
  for (const auto& h : ListHumans()) {
    if (h.slot >= 0 && SendPanel(self, h.slot, html, seconds, priority) == 1) ++sent;
  }
  return sent;
}

int ApiCenterHtmlToSlot(ru_plugin* self, int slot, const char* html, int seconds) {
  return ApiCenterHtmlToSlotPrio(self, slot, html, seconds, RU_HTML_PRIO_HUD) == 1 ? 1 : 0;
}

int ApiCenterHtmlAll(ru_plugin* self, const char* html, int seconds) {
  return ApiCenterHtmlAllPrio(self, html, seconds, RU_HTML_PRIO_HUD);
}

void ApiCenterHtmlRelease(ru_plugin* self, int slot) {
  if (!CheckGameThread(self, "center_html_release")) return;
  g_panels.Release(slot, PanelOwnerId(self));
}

// ---- players -----------------------------------------------------------------------

void Fill(ru_player* out, int slot, uint64_t steamid64, int team, bool bot, const std::string& name, int userid) {
  // Fill only what the caller's struct has room for (struct_size is the caller's sizeof).
  ru_player p{};
  p.struct_size = out->struct_size;
  p.slot = slot;
  p.steamid64 = steamid64;
  p.team = team;
  p.is_bot = bot ? 1 : 0;
  p.connected = 1;
  std::strncpy(p.name, name.c_str(), sizeof(p.name) - 1);
  p.userid = userid;  // v1.2
  const size_t n = out->struct_size < sizeof(p) ? out->struct_size : sizeof(p);
  std::memcpy(out, &p, n);
}

bool ValidOut(const ru_player* out) {
  return out && out->struct_size >= offsetof(ru_player, connected) + sizeof(out->connected);
}

int ApiGetPlayer(ru_plugin* self, int slot, ru_player* out) {
  if (!CheckGameThread(self, "get_player") || !ValidOut(out) || slot < 0) return 0;
  for (const auto& h : ListHumans()) {
    if (h.slot == slot) {
      Fill(out, h.slot, h.steamid64, h.team, false, h.name, h.userid);
      return 1;
    }
  }
  return 0;
}

int ApiGetPlayerBySteam(ru_plugin* self, uint64_t steamid64, ru_player* out) {
  if (!CheckGameThread(self, "get_player_by_steamid") || !ValidOut(out) || steamid64 == 0) return 0;
  for (const auto& h : ListHumans()) {
    if (h.steamid64 == steamid64) {
      Fill(out, h.slot, h.steamid64, h.team, false, h.name, h.userid);
      return 1;
    }
  }
  return 0;
}

int ApiForEachPlayer(ru_plugin* self, ru_player_fn fn, void* user) {
  if (!CheckGameThread(self, "for_each_player") || !fn) return 0;
  int n = 0;
  ru_player p{};
  p.struct_size = sizeof(p);
  for (const auto& h : ListHumans()) {
    Fill(&p, h.slot, h.steamid64, h.team, false, h.name, h.userid);
    ++n;
    if (!fn(user, &p)) return n;
  }
  for (const auto& b : ListBots()) {
    Fill(&p, -1, 0, b.team, true, b.name, b.userid);
    ++n;
    if (!fn(user, &p)) return n;
  }
  return n;
}

// ---- raw game events ---------------------------------------------------------------

IGameEvent* Ev(const ru_game_event* ev) { return reinterpret_cast<IGameEvent*>(const_cast<ru_game_event*>(ev)); }

int ApiEvInt(ru_plugin* self, const ru_game_event* ev, const char* key, int def) {
  if (!CheckGameThread(self, "ev_get_int") || !ev || !key) return def;
  return Ev(ev)->GetInt(CKV3MemberName(key), def);
}

double ApiEvFloat(ru_plugin* self, const ru_game_event* ev, const char* key, double def) {
  if (!CheckGameThread(self, "ev_get_float") || !ev || !key) return def;
  return Ev(ev)->GetFloat(CKV3MemberName(key), static_cast<float>(def));
}

uint64_t ApiEvUint64(ru_plugin* self, const ru_game_event* ev, const char* key, uint64_t def) {
  if (!CheckGameThread(self, "ev_get_uint64") || !ev || !key) return def;
  return Ev(ev)->GetUint64(CKV3MemberName(key), def);
}

const char* ApiEvString(ru_plugin* self, const ru_game_event* ev, const char* key, const char* def) {
  const char* fallback = def ? def : "";
  if (!CheckGameThread(self, "ev_get_string") || !ev || !key) return fallback;
  const char* s = Ev(ev)->GetString(CKV3MemberName(key), fallback);
  return s ? s : fallback;
}

int ApiEvPlayerSlot(ru_plugin* self, const ru_game_event* ev, const char* key) {
  if (!CheckGameThread(self, "ev_get_player_slot") || !ev || !key) return -1;
  const int slot = Ev(ev)->GetPlayerSlot(CKV3MemberName(key)).value;
  return slot >= 0 && slot < 64 ? slot : -1;
}

void* ApiEvController(ru_plugin* self, const ru_game_event* ev, const char* key) {
  if (!CheckGameThread(self, "ev_get_player_controller") || !ev || !key) return nullptr;
  return Ev(ev)->GetPlayerController(CKV3MemberName(key));
}

void* ApiEvPawn(ru_plugin* self, const ru_game_event* ev, const char* key) {
  if (!CheckGameThread(self, "ev_get_player_pawn") || !ev || !key) return nullptr;
  return Ev(ev)->GetPlayerPawn(CKV3MemberName(key));
}

// ---- schema and entities -----------------------------------------------------------

int ApiSchemaOffset(ru_plugin* self, const char* cls, const char* field) {
  if (!CheckGameThread(self, "schema_offset") || !cls || !field) return -1;
  const auto v = SchemaFindOffset("server", cls, field);
  return v ? *v : -1;
}

int ApiEntitySystemStatus(ru_plugin* self) {
  if (!CheckGameThread(self, "entity_system_status")) return RU_ENTSYS_PENDING;
  return entity::EntitySystemStatus(nullptr);
}

void* ApiEntityByIndex(ru_plugin* self, int index) {
  if (!CheckGameThread(self, "entity_by_index")) return nullptr;
  return entity::EntityByIndex(index);
}

void* ApiEntityFromHandle(ru_plugin* self, uint32_t handle) {
  if (!CheckGameThread(self, "entity_from_handle")) return nullptr;
  return entity::EntityFromHandle(handle);
}

uint32_t ApiEntityHandleOf(ru_plugin* self, void* ent) {
  if (!CheckGameThread(self, "entity_handle_of") || !ent) return 0xFFFFFFFFu;
  return entity::EntityHandleOf(ent);
}

const char* ApiEntityClassname(ru_plugin* self, void* ent) {
  if (!CheckGameThread(self, "entity_classname") || !ent) return nullptr;
  return entity::EntityDesignerName(ent);
}

int ApiEntityMarkChanged(ru_plugin* self, void* ent) {
  if (!CheckGameThread(self, "entity_mark_changed") || !ent) return 0;
  return entity::MarkEntityFullyChanged(ent) ? 1 : 0;
}

int ApiEconAttrSet(ru_plugin* self, void* list, const char* name, double value) {
  if (!CheckGameThread(self, "econ_attr_set_by_name") || !list || !name) return 0;
  return entity::AttrSetOrAddByName(list, name, static_cast<float>(value)) ? 1 : 0;
}

int ApiChangeSubclass(ru_plugin* self, void* ent, const char* subclass) {
  if (!CheckGameThread(self, "entity_change_subclass") || !ent || !subclass) return 0;
  return entity::ChangeSubclass(ent, subclass) ? 1 : 0;
}

int ApiSetModel(ru_plugin* self, void* ent, const char* model) {
  if (!CheckGameThread(self, "entity_set_model") || !ent || !model || !*model) return 0;
  return entity::SetModel(ent, model) ? 1 : 0;
}

int ApiSetAbsOrigin(ru_plugin* self, void* ent, const float* origin) {
  if (!CheckGameThread(self, "entity_set_abs_origin") || !ent || !origin) return 0;
  return entity::SetAbsOrigin(ent, origin) ? 1 : 0;
}

void* ApiGiveItem(ru_plugin* self, int slot, const char* classname) {
  if (!CheckGameThread(self, "player_give_item") || slot < 0 || slot >= 64 || !classname || !*classname) return nullptr;
  const auto pawnOff = SchemaFindOffset("server", "CCSPlayerController", "m_hPlayerPawn");
  const auto svcOff = SchemaFindOffset("server", "CBasePlayerPawn", "m_pItemServices");
  const auto lifeOff = SchemaFindOffset("server", "CBaseEntity", "m_lifeState");
  void* ctrl = entity::EntityByIndex(slot + 1);
  if (!ctrl || !pawnOff || !svcOff || !lifeOff) return nullptr;
  uint32_t pawnHandle = 0;
  std::memcpy(&pawnHandle, static_cast<const char*>(ctrl) + *pawnOff, sizeof(pawnHandle));
  void* pawn = entity::EntityFromHandle(pawnHandle);
  if (!pawn || static_cast<const unsigned char*>(pawn)[*lifeOff] != 0) return nullptr;  // not alive
  void* svc = nullptr;
  std::memcpy(&svc, static_cast<const char*>(pawn) + *svcOff, sizeof(svc));
  return svc ? entity::GiveNamedItem(svc, classname) : nullptr;
}

void* ApiSurfaceFunction(ru_plugin* self, const char* key) {
  if (!CheckGameThread(self, "surface_function") || !key) return nullptr;
  return hooks::SurfaceFunction(key);
}

int ApiHookFunction(ru_plugin* self, const char* key, void* detour, void** trampoline) {
  if (!CheckGameThread(self, "hook_function")) return 0;
  return hooks::HookFunction(detail::PluginId(self), detail::PluginNameOf(self), key, detour, trampoline, nullptr) ? 1 : 0;
}

int ApiHookVtable(ru_plugin* self, const char* key, const void* obj, void* fn, void** original) {
  if (!CheckGameThread(self, "hook_vtable")) return 0;
  return hooks::HookVtable(detail::PluginId(self), detail::PluginNameOf(self), key, obj, fn, original, nullptr) ? 1 : 0;
}

int ApiSetBodygroup(ru_plugin* self, void* ent, const char* group, int value) {
  if (!CheckGameThread(self, "entity_set_bodygroup_by_name") || !ent || !group) return RU_BODYGROUP_UNAVAILABLE;
  switch (entity::SetBodygroupByName(ent, group, value)) {
    case entity::BodygroupResult::kOk: return RU_BODYGROUP_OK;
    case entity::BodygroupResult::kNoModel: return RU_BODYGROUP_NO_MODEL;
    case entity::BodygroupResult::kNoGroup: return RU_BODYGROUP_NO_GROUP;
    default: return RU_BODYGROUP_UNAVAILABLE;
  }
}

// ---- match control / admins --------------------------------------------------------

int ApiSetRoundTermSuppressed(ru_plugin* self, int suppress) {
  if (!CheckGameThread(self, "set_round_termination_suppressed")) return 0;
  SetRoundTerminationSuppressed(suppress != 0);
  return RoundTerminationSuppressed() == (suppress != 0) ? 1 : 0;
}

// ---- v1.2 ---------------------------------------------------------------------------

int ApiFeatureState(ru_plugin* self, const char* name) {
  if (!CheckGameThread(self, "feature_state") || !name || !*name) return -1;
  return FeatureStateByName(name);
}

int ApiIsAdmin(ru_plugin* self, uint64_t steamid64) {
  if (!self || steamid64 == 0) return 0;
  return IsReadyUpAdmin(steamid64) ? 1 : 0;  // asks the plugin admin provider first
}

int ApiEntityRemove(ru_plugin* self, void* ent) {
  if (!CheckGameThread(self, "entity_remove") || !ent) return 0;
  return entity::RemoveEntity(ent) ? 1 : 0;
}

int ApiWorkshopDownloadProgress(ru_plugin* self, uint64_t id, uint64_t* downloaded, uint64_t* total) {
  if (!self) return 0;
  return steam_ugc::DownloadProgress(id, downloaded, total) ? 1 : 0;
}

int ApiLicensePlayerLine(ru_plugin* self, char* buf, uint32_t len) {
  if (!self) return -1;
  const std::string line = license::PlayerLineIfShown();
  if (line.empty()) return -1;
  if (buf && len > 0) std::snprintf(buf, len, "%s", line.c_str());
  return static_cast<int>(line.size());
}

// ---- v1.10 --------------------------------------------------------------------------

int ApiSetCoreSetting(ru_plugin* self, const char* key, const char* value) {
  if (!CheckGameThread(self, "set_core_setting") || !key) return 0;
  const std::string k = key;
  const std::string v = value ? value : "";
  if (k == "chat_prefix") return SetChatPrefixOverride(v) ? 1 : 0;
  if (k == "status_http_token") return status_feed::SetTokenOverride(v) ? 1 : 0;
  return 0;
}

// ---- v1.12 --------------------------------------------------------------------------

int ApiGrenadeSpawnAvailable(ru_plugin* self, uint32_t type) {
  if (!CheckGameThread(self, "grenade_spawn_available")) return 0;
  return grenades::Available(type) ? 1 : 0;
}

void* ApiGrenadeSpawn(ru_plugin* self, const ru_grenade_spawn* spec) {
  if (!CheckGameThread(self, "grenade_spawn")) return nullptr;
  auto refuse = [&](const std::string& why) -> void* {
    Print("plugin[%s]: grenade_spawn refused: %s\n", detail::PluginNameOf(self), why.c_str());
    return nullptr;
  };
  if (!spec || spec->struct_size < offsetof(ru_grenade_spawn, owner_slot) + sizeof(spec->owner_slot)) {
    return refuse("spec missing or struct_size too small");
  }
  grenades::SpawnRequest req;
  req.type = spec->type;
  req.origin = spec->origin;
  req.angles = spec->angles;
  req.velocity = spec->velocity;
  req.ang_velocity = spec->angular_velocity;
  if (spec->owner_slot >= 0) {
    // The thrower is the player's pawn (what CS2's own SpawnGrenadeProjectile takes); the team
    // comes from it too. A slot without a pawn is refused rather than silently ownerless.
    const auto pawnOff = SchemaFindOffset("server", "CCSPlayerController", "m_hPlayerPawn");
    const auto teamOff = SchemaFindOffset("server", "CBaseEntity", "m_iTeamNum");
    void* ctrl = spec->owner_slot < 64 ? entity::EntityByIndex(spec->owner_slot + 1) : nullptr;
    if (!ctrl || !pawnOff || !teamOff) return refuse("owner_slot " + std::to_string(spec->owner_slot) + " is not a player");
    uint32_t pawnHandle = 0;
    std::memcpy(&pawnHandle, static_cast<const char*>(ctrl) + *pawnOff, sizeof(pawnHandle));
    void* pawn = entity::EntityFromHandle(pawnHandle);
    if (!pawn) return refuse("owner_slot " + std::to_string(spec->owner_slot) + " has no pawn");
    req.owner_pawn = pawn;
    req.team = static_cast<const unsigned char*>(pawn)[*teamOff];
  }
  std::string why;
  void* ent = grenades::Spawn(req, &why);
  return ent ? ent : refuse(why);
}

// ---- v1.13 --------------------------------------------------------------------------

int ApiEngineFeatureAvailable(ru_plugin* self, const char* feature) {
  if (!CheckGameThread(self, "engine_feature_available")) return 0;
  return practice_engine::Available(feature) ? 1 : 0;
}

int ApiPlayerChangeTeam(ru_plugin* self, int slot, int team) {
  if (!CheckGameThread(self, "player_change_team")) return 0;
  std::string why;
  if (practice_engine::ChangeTeam(slot, team, &why)) return 1;
  Print("plugin[%s]: player_change_team(%d, %d) refused: %s\n", detail::PluginNameOf(self), slot, team, why.c_str());
  return 0;
}

int ApiPlayerTeleport(ru_plugin* self, int slot, const float* origin, const float* angles, const float* velocity) {
  if (!CheckGameThread(self, "player_teleport")) return 0;
  std::string why;
  if (practice_engine::Teleport(slot, origin, angles, velocity, &why)) return 1;
  Print("plugin[%s]: player_teleport(%d) refused: %s\n", detail::PluginNameOf(self), slot, why.c_str());
  return 0;
}

void* ApiEntityCreate(ru_plugin* self, const char* classname) {
  if (!CheckGameThread(self, "entity_create")) return nullptr;
  std::string why;
  if (void* e = practice_engine::Create(classname, &why)) return e;
  Print("plugin[%s]: entity_create refused: %s\n", detail::PluginNameOf(self), why.c_str());
  return nullptr;
}

int ApiEntitySpawn(ru_plugin* self, void* ent) {
  if (!CheckGameThread(self, "entity_spawn")) return 0;
  std::string why;
  if (practice_engine::Spawn(ent, &why)) return 1;
  Print("plugin[%s]: entity_spawn refused: %s\n", detail::PluginNameOf(self), why.c_str());
  return 0;
}

// ---- v1.14 --------------------------------------------------------------------------

int ApiPlayerRespawn(ru_plugin* self, int slot) {
  if (!CheckGameThread(self, "player_respawn")) return 0;
  std::string why;
  if (practice_engine::Respawn(slot, &why)) return 1;
  Print("plugin[%s]: player_respawn(%d) refused: %s\n", detail::PluginNameOf(self), slot, why.c_str());
  return 0;
}

}  // namespace

void detail::FillEngineApi(ru_api* a) {
  a->center_html_to_slot = &ApiCenterHtmlToSlot;
  a->center_html_all = &ApiCenterHtmlAll;
  a->get_player = &ApiGetPlayer;
  a->get_player_by_steamid = &ApiGetPlayerBySteam;
  a->for_each_player = &ApiForEachPlayer;
  a->ev_get_int = &ApiEvInt;
  a->ev_get_float = &ApiEvFloat;
  a->ev_get_uint64 = &ApiEvUint64;
  a->ev_get_string = &ApiEvString;
  a->ev_get_player_slot = &ApiEvPlayerSlot;
  a->ev_get_player_controller = &ApiEvController;
  a->ev_get_player_pawn = &ApiEvPawn;
  a->schema_offset = &ApiSchemaOffset;
  a->entity_system_status = &ApiEntitySystemStatus;
  a->entity_by_index = &ApiEntityByIndex;
  a->entity_from_handle = &ApiEntityFromHandle;
  a->entity_handle_of = &ApiEntityHandleOf;
  a->entity_classname = &ApiEntityClassname;
  a->entity_mark_changed = &ApiEntityMarkChanged;
  a->econ_attr_set_by_name = &ApiEconAttrSet;
  a->entity_change_subclass = &ApiChangeSubclass;
  a->entity_set_model = &ApiSetModel;
  a->entity_set_bodygroup_by_name = &ApiSetBodygroup;
  a->set_round_termination_suppressed = &ApiSetRoundTermSuppressed;
  a->is_admin = &ApiIsAdmin;
  a->feature_state = &ApiFeatureState;
  a->entity_set_abs_origin = &ApiSetAbsOrigin;  // v1.3
  a->workshop_download_progress = &ApiWorkshopDownloadProgress;  // v1.4
  a->entity_remove = &ApiEntityRemove;                            // v1.5
  a->center_html_to_slot_prio = &ApiCenterHtmlToSlotPrio;         // v1.6
  a->center_html_all_prio = &ApiCenterHtmlAllPrio;
  a->center_html_release = &ApiCenterHtmlRelease;
  a->player_give_item = &ApiGiveItem;                             // v1.7
  a->surface_function = &ApiSurfaceFunction;                      // v1.8
  a->hook_function = &ApiHookFunction;
  a->hook_vtable = &ApiHookVtable;
  a->license_player_line = &ApiLicensePlayerLine;                 // v1.9
  a->set_core_setting = &ApiSetCoreSetting;                       // v1.10
  a->grenade_spawn = &ApiGrenadeSpawn;                            // v1.12
  a->grenade_spawn_available = &ApiGrenadeSpawnAvailable;
  a->engine_feature_available = &ApiEngineFeatureAvailable;       // v1.13
  a->player_change_team = &ApiPlayerChangeTeam;
  a->player_teleport = &ApiPlayerTeleport;
  a->entity_create = &ApiEntityCreate;
  a->entity_spawn = &ApiEntitySpawn;
  a->player_respawn = &ApiPlayerRespawn;  // v1.14
}

}  // namespace readyup::plugins

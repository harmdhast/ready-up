#include "readyup/round_economy.h"

#include "readyup/config.h"
#include "readyup/host.h"
#include "readyup/logging.h"
#include "readyup/match_stats.h"
#include "readyup/players.h"
#include "readyup/plugin_api.h"

#include <cstdint>
#include <cstring>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

namespace readyup {
namespace {

struct Offsets {
  bool resolved = false;
  int moneyServices = -1;  // CCSPlayerController::m_pInGameMoneyServices
  int account = -1;        // CCSPlayerController_InGameMoneyServices::m_iAccount
  int pawnHandle = -1;     // CCSPlayerController::m_hPlayerPawn
  int armor = -1;          // CCSPlayerPawn::m_ArmorValue
  int itemServices = -1;   // CBasePlayerPawn::m_pItemServices
  int helmet = -1;         // CCSPlayer_ItemServices::m_bHasHelmet
  int defuser = -1;        // CCSPlayer_ItemServices::m_bHasDefuser
  int weaponServices = -1; // CBasePlayerPawn::m_pWeaponServices
  int myWeapons = -1;      // CPlayer_WeaponServices::m_hMyWeapons (networked CUtlVector<CHandle>)
};
Offsets g_off;

bool Resolve(const ru_api* a) {
  if (g_off.resolved) return g_off.moneyServices >= 0 && g_off.account >= 0;
  if (a->entity_system_status(a->self) != RU_ENTSYS_OK) return false;
  g_off.moneyServices = a->schema_offset(a->self, "CCSPlayerController", "m_pInGameMoneyServices");
  g_off.account = a->schema_offset(a->self, "CCSPlayerController_InGameMoneyServices", "m_iAccount");
  g_off.pawnHandle = a->schema_offset(a->self, "CCSPlayerController", "m_hPlayerPawn");
  g_off.armor = a->schema_offset(a->self, "CCSPlayerPawn", "m_ArmorValue");
  g_off.itemServices = a->schema_offset(a->self, "CBasePlayerPawn", "m_pItemServices");
  g_off.helmet = a->schema_offset(a->self, "CCSPlayer_ItemServices", "m_bHasHelmet");
  g_off.defuser = a->schema_offset(a->self, "CCSPlayer_ItemServices", "m_bHasDefuser");
  g_off.weaponServices = a->schema_offset(a->self, "CBasePlayerPawn", "m_pWeaponServices");
  g_off.myWeapons = a->schema_offset(a->self, "CPlayer_WeaponServices", "m_hMyWeapons");
  g_off.resolved = true;
  if (DebugEnabled()) {
    Debug("round-economy: offsets money=%d/%d pawn=%d armor=%d items=%d/%d/%d weapons=%d/%d\n",
          g_off.moneyServices, g_off.account, g_off.pawnHandle, g_off.armor, g_off.itemServices, g_off.helmet,
          g_off.defuser, g_off.weaponServices, g_off.myWeapons);
  }
  return g_off.moneyServices >= 0 && g_off.account >= 0;
}

template <typename T>
bool ReadAt(const void* base, int off, T* out) {
  if (!base || off < 0) return false;
  std::memcpy(out, static_cast<const unsigned char*>(base) + off, sizeof(T));
  return true;
}

// The stats model's id for a player: SteamID64, or a bot's pseudo id from its log userid.
uint64_t StatsId(const ru_player& p) {
  if (!p.is_bot && p.steamid64 != 0) return p.steamid64;
  if (p.is_bot && p.userid >= 0) return DevBotIdForUserid(p.userid);
  return 0;
}

// The controller of a player: humans by engine slot, bots by their log userid (CS2 logs the slot).
void* Controller(const ru_api* a, const ru_player& p) {
  const int slot = p.slot >= 0 ? p.slot : p.userid;
  if (slot < 0 || slot > 63) return nullptr;
  return a->entity_by_index(a->self, slot + 1);
}

int Money(void* ctrl) {
  void* ms = nullptr;
  int32_t acct = -1;
  if (!ReadAt(ctrl, g_off.moneyServices, &ms) || !ms || !ReadAt(ms, g_off.account, &acct)) return -1;
  return acct;
}

struct Collect {
  const ru_api* a;
  std::vector<ru_player> players;
};

int CollectPlayer(void* user, const ru_player* p) {
  auto* c = static_cast<Collect*>(user);
  if (p->connected && (p->team == 2 || p->team == 3)) c->players.push_back(*p);
  return 1;
}

std::vector<ru_player> Players(const ru_api* a) {
  Collect c{a, {}};
  a->for_each_player(a->self, CollectPlayer, &c);
  return c.players;
}

// Money at round_start, held until the freeze time ends: the stats model starts its round a
// moment after the event (match_events.cpp, deferred to the tick), which would clear it.
std::mutex g_mu;
std::unordered_map<uint64_t, int> g_startMoney;

}  // namespace

void RoundEconomyOnRoundStart() {
  const ru_api* a = host::Api();
  if (!a || !Resolve(a)) return;
  std::unordered_map<uint64_t, int> start;
  for (const auto& p : Players(a)) {
    const uint64_t id = StatsId(p);
    void* ctrl = Controller(a, p);
    if (!id || !ctrl) continue;
    const int money = Money(ctrl);
    if (money >= 0) start[id] = money;
  }
  std::lock_guard<std::mutex> lk(g_mu);
  g_startMoney.swap(start);
}

void RoundEconomyOnFreezeEnd() {
  const ru_api* a = host::Api();
  if (!a || !Resolve(a)) return;
  std::unordered_map<uint64_t, int> start;
  {
    std::lock_guard<std::mutex> lk(g_mu);
    start.swap(g_startMoney);
  }
  std::lock_guard<std::recursive_mutex> lk(stats::Mutex());
  for (const auto& p : Players(a)) {
    const uint64_t id = StatsId(p);
    void* ctrl = Controller(a, p);
    if (!id || !ctrl) continue;
    const int money = Money(ctrl);
    uint32_t pawnHandle = 0xFFFFFFFFu;
    void* pawn = ReadAt(ctrl, g_off.pawnHandle, &pawnHandle) ? a->entity_from_handle(a->self, pawnHandle) : nullptr;
    int32_t armor = -1;
    bool helmet = false, defuser = false;
    std::vector<std::string> loadout;
    if (pawn) {
      (void)ReadAt(pawn, g_off.armor, &armor);
      void* items = nullptr;
      if (ReadAt(pawn, g_off.itemServices, &items) && items) {
        (void)ReadAt(items, g_off.helmet, &helmet);
        (void)ReadAt(items, g_off.defuser, &defuser);
      }
      void* ws = nullptr;
      if (ReadAt(pawn, g_off.weaponServices, &ws) && ws && g_off.myWeapons >= 0) {
        // Networked CUtlVector: the element count, then a pointer to the elements.
        const unsigned char* vec = static_cast<const unsigned char*>(ws) + g_off.myWeapons;
        int32_t count = 0;
        uint32_t* elems = nullptr;
        std::memcpy(&count, vec, sizeof(count));
        std::memcpy(&elems, vec + 8, sizeof(elems));
        if (count > 0 && count <= 64 && elems) {
          for (int i = 0; i < count; ++i) {
            void* w = a->entity_from_handle(a->self, elems[i]);
            const char* cls = w ? a->entity_classname(a->self, w) : nullptr;
            if (!cls) continue;
            std::string name(cls);
            if (name.rfind("weapon_", 0) == 0) name = name.substr(7);
            if (name == "knife" || name == "knife_t" || name.rfind("knife", 0) == 0 || name == "bayonet") continue;
            loadout.push_back(name);
          }
        }
      }
    }
    if (auto it = start.find(id); it != start.end()) stats::Current().SetMoneyStart(id, it->second);
    stats::Current().SetLoadout(id, money, armor, helmet, defuser, loadout);
  }
}

}  // namespace readyup

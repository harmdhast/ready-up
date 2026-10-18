// Warmup weapon cleanup (see weapon_cleanup.h).
#include "readyup/weapon_cleanup.h"

#include "readyup/config.h"
#include "readyup/host.h"
#include "readyup/logging.h"
#include "readyup/modes.h"
#include "readyup/plugin_api.h"

#include <algorithm>
#include <atomic>

namespace readyup {
namespace {

// Networked entities (weapons are) sit below this index. A sweep walks kSlice indices per tick,
// (see g_limit: a sweep ends past the highest slot in use; every 8th covers the whole range).
constexpr int kMaxIndex = 16384;
constexpr int kSlice = 256;  // per call (16 calls a second): a 2048-slot map is swept in half a second
// Index 0 is the world, 1..64 the player controllers.
constexpr int kFirstIndex = 65;

int g_offOwner = -2;  // CBaseEntity::m_hOwnerEntity (-2 = not looked up)
bool g_unavailableLogged = false;
int g_cursor = kFirstIndex;
// A map uses a few thousand entity slots at most: a sweep ends a margin past the highest slot the
// last sweep found in use, not at kMaxIndex (2026-10-15: the full range was most of the match
// plugin's per-tick time).
int g_passMax = kFirstIndex;   // highest slot in use seen during this sweep
int g_limit = kMaxIndex;       // where this sweep ends
constexpr int kLimitMargin = 512;
constexpr int kMinLimit = 2048;
int g_passes = 0;  // every 8th sweep covers the whole range, so slots past the limit are found too
int g_removed = 0;  // this pass (debug line)
bool g_enabled = true;  // readyup.cfg warmup_weapon_cleanup
double g_nextCfg = 0;
DroppedWeaponTracker g_tracker;
// For `ru selftest` (any thread): 0 not checked yet, 1 ok, -1 no entity_remove, -2 no schema field.
std::atomic<int> g_state{0};
std::atomic<int> g_totalRemoved{0};

bool Resolve(const ru_api* a) {
  if (!RU_API_HAS(a, entity_remove) || !a->entity_remove) {
    if (!g_unavailableLogged) {
      Print("weapon-cleanup: core has no entity_remove (API 1.5); dropped weapons in warmup are left to "
            "weapon_auto_cleanup_time / weapon_max_before_cleanup\n");
      g_unavailableLogged = true;
    }
    g_state.store(-1);
    return false;
  }
  if (a->entity_system_status(a->self) != RU_ENTSYS_OK) return false;
  if (g_offOwner == -2) {
    g_offOwner = a->schema_offset(a->self, "CBaseEntity", "m_hOwnerEntity");
    if (g_offOwner < 0) Print("weapon-cleanup: schema field CBaseEntity::m_hOwnerEntity missing; no sweep\n");
  }
  g_state.store(g_offOwner >= 0 ? 1 : -2);
  return g_offOwner >= 0;
}

bool HasOwner(const ru_api* a, void* ent) {
  uint32_t h = 0xFFFFFFFFu;
  std::memcpy(&h, static_cast<unsigned char*>(ent) + g_offOwner, sizeof(h));
  return h != 0xFFFFFFFFu && a->entity_from_handle(a->self, h) != nullptr;
}

}  // namespace

void WeaponCleanupInstall(const ru_api*) {
  g_offOwner = -2;
  g_unavailableLogged = false;
  g_cursor = kFirstIndex;
  g_nextCfg = 0;
  g_tracker.Clear();
}

std::string WeaponCleanupStatus() {
  const std::string n = std::to_string(g_totalRemoved.load());
  switch (g_state.load()) {
    case 1: return "on (" + n + " removed since load)";
    case -1: return "unavailable: core has no entity_remove (API 1.5)";
    case -2: return "unavailable: no CBaseEntity::m_hOwnerEntity in schema";
    default: return "not checked yet (runs in idle / scrim warmup / match warmup)";
  }
}

void WeaponCleanupTick(double now) {
  // The mode every tick, readyup.cfg once a second (Cfg() copies the whole config).
  if (now >= g_nextCfg) {
    g_enabled = Cfg().warmup_weapon_cleanup;
    g_nextCfg = now + 1.0;
  }
  if (!WeaponCleanupActive(GetModeString(), GoLiveTriggered(), g_enabled)) {
    g_tracker.Clear();
    g_cursor = kFirstIndex;
    g_limit = kMaxIndex;
    g_passMax = kFirstIndex;
    return;
  }
  const ru_api* a = host::Api();
  if (!a || !Resolve(a)) return;
  const int end = std::min(g_limit, g_cursor + kSlice);
  for (int i = g_cursor; i < end; ++i) {
    void* ent = a->entity_by_index(a->self, i);
    if (!ent) continue;
    g_passMax = i;
    if (!WeaponCleanupClass(a->entity_classname(a->self, ent))) continue;
    const uint32_t h = a->entity_handle_of(a->self, ent);
    if (h == 0xFFFFFFFFu) continue;
    if (HasOwner(a, ent)) {
      g_tracker.Owned(h);
    } else if (g_tracker.Unowned(h, now) && a->entity_remove(a->self, ent) == 1) {
      g_tracker.Forget(h);
      ++g_removed;
      g_totalRemoved.fetch_add(1);
    }
  }
  g_cursor = end;
  if (g_cursor < g_limit) return;
  // Pass done.
  g_cursor = kFirstIndex;
  g_limit = (++g_passes & 7) == 0 ? kMaxIndex : std::min(kMaxIndex, std::max(kMinLimit, g_passMax + kLimitMargin));
  g_passMax = kFirstIndex;
  g_tracker.Prune(now);
  if (g_removed) Debug("weapon-cleanup: removed %d dropped weapon(s)\n", g_removed);
  g_removed = 0;
}

}  // namespace readyup

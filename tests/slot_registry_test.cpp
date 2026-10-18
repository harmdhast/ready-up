// Offline tests for core/src/readyup/slot_registry.h: a player who disconnected is not
// re-added by the lines CS2 logs right after the disconnect. ctest `slot_registry`.
#include "readyup/slot_registry.h"

#include <cstdio>

using namespace readyup;

static int g_failures = 0;

#define CHECK(cond)                                                                 \
  do {                                                                              \
    if (!(cond)) {                                                                  \
      std::fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #cond); \
      ++g_failures;                                                                 \
    }                                                                               \
  } while (0)

static int TeamOf(uint64_t sid) {
  for (const auto& h : ListHumans()) {
    if (h.steamid64 == sid) return h.team;
  }
  return -1;
}

// The engine in the 2026-10-17 case: slot 3 (BBro) is CT, slot 4 (EmilYO) T, slot 9 unknown.
static int EngineTeam(int slot) { return slot == 3 ? 3 : slot == 4 ? 2 : -1; }

static void TestReconcileAfterMapLoad() {
  const uint64_t bbro = 76561198000000003ull, emil = 76561198000000004ull, other = 76561198000000009ull;
  ObserveHuman(13, bbro, "BBro", 3);
  ObserveHuman(14, emil, "EmilYO", 2);
  ObserveHuman(19, other, "x", 3);
  CHECK(ObserveHumanTeamFromEvent(bbro, 3, 3));
  CHECK(ObserveHumanTeamFromEvent(emil, 4, 2));
  CHECK(ObserveHumanTeamFromEvent(other, 9, 3));
  // Map load: everyone "has no team"; the two who stay on their side send no player_team event.
  ResetHumanTeams();
  CHECK(TeamOf(bbro) == 0 && TeamOf(emil) == 0);
  CHECK(ReconcileHumanTeams(&EngineTeam) == 2);
  CHECK(TeamOf(bbro) == 3 && TeamOf(emil) == 2);
  CHECK(TeamOf(other) == 0);  // engine unknown: left alone
  CHECK(ReconcileHumanTeams(&EngineTeam) == 0);  // nothing more to do
  CHECK(ReconcileHumanTeams(nullptr) == 0);
  ForgetHuman(bbro);
  ForgetHuman(emil);
  ForgetHuman(other);
}

static bool Present(uint64_t sid) {
  for (const auto& h : ListHumans()) {
    if (h.steamid64 == sid) return true;
  }
  return false;
}

// What client_command_hook does with one player log line.
static void Line(uint64_t sid, int userid, bool leaving, bool connectLine) {
  if (!AcceptHumanLogLine(sid, leaving, connectLine)) return;
  if (leaving) ForgetHuman(sid);
  else ObserveHuman(userid, sid, "p", -1);
}

int main() {
  const uint64_t cola = 76561198854229724ull;
  const uint64_t emil = 76561198003830440ull;

  // Never seen: any header line adds them (the plugin loaded mid-match).
  Line(cola, 4, false, false);
  CHECK(Present(cola));

  // 2026-10-16: `disconnected (reason ...)` then `triggered "Dropped_The_Bomb"` and
  // `committed suicide` for the same header. The tail must not bring them back.
  Line(cola, 4, true, false);
  CHECK(!Present(cola));
  Line(cola, 4, false, false);
  Line(cola, 4, false, false);
  CHECK(!Present(cola));

  // Someone else takes the number: unaffected.
  Line(emil, 4, false, true);
  CHECK(Present(emil));
  CHECK(ListHumans().size() == 1);

  // They connect again: back, and later lines keep them.
  Line(cola, 7, false, true);
  CHECK(Present(cola));
  Line(cola, 7, false, false);
  CHECK(Present(cola));

  // A second disconnect works the same way.
  Line(cola, 7, true, false);
  Line(cola, 7, false, false);
  CHECK(!Present(cola));

  TestReconcileAfterMapLoad();

  if (g_failures == 0) std::printf("slot_registry: ok\n");
  return g_failures == 0 ? 0 : 1;
}

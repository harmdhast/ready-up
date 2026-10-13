// Offline tests for core/src/readyup/slot_registry.h: a disconnected human stays gone when the
// game logs more lines for them, and only an arrival brings them back. ctest `slot_registry`.
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

constexpr uint64_t kSteam = 76561197960265728ull + 76450349ull;
constexpr uint64_t kOther = 76561197960265728ull + 1234ull;

static void Leave(uint64_t steam) {
  ForgetHuman(steam);
  ForgetSlotIdentitiesForSteam(steam);
}

int main() {
  ObserveHumanArrival(kSteam);
  ObserveSlotIdentity(5, kSteam, "Harmdhast");
  ObserveHuman(5, kSteam, "Harmdhast", 2);
  CHECK(ListHumans().size() == 1);
  CHECK(ListSlotIdentities().size() == 1);

  Leave(kSteam);
  CHECK(ListHumans().empty());
  CHECK(IsHumanDeparted(kSteam));

  // `triggered "Dropped_The_Bomb"` right after the disconnect line
  ObserveHuman(5, kSteam, "Harmdhast", 2);
  CHECK(ListHumans().empty());
  CHECK(ObserveHumanTeamFromEvent(kSteam, 5, 2) == false);

  // another player is unaffected
  ObserveHuman(6, kOther, "Other", 3);
  CHECK(ListHumans().size() == 1 && ListHumans()[0].steamid64 == kOther);

  // arrival (`entered the game`, `STEAM USERID validated`, switch from Unassigned)
  ObserveHumanArrival(kSteam);
  CHECK(!IsHumanDeparted(kSteam));
  ObserveHuman(7, kSteam, "Harmdhast", 0);
  CHECK(ListHumans().size() == 2);

  // a map load forgets the departed set
  Leave(kOther);
  CHECK(IsHumanDeparted(kOther));
  ResetHumanTeams();
  CHECK(!IsHumanDeparted(kOther));

  // the departed set is bounded
  for (uint64_t i = 1; i <= 2000; ++i) ForgetHuman(kOther + i);
  CHECK(!IsHumanDeparted(kOther + 1));
  CHECK(IsHumanDeparted(kOther + 2000));

  if (g_failures) return 1;
  std::puts("slot_registry_test: ok");
  return 0;
}

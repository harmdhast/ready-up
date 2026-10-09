// Offline tests for the ready panel's player names (readyup/ready_hud_logic.h).
// ctest `match_ready_hud`.
#include "readyup/ready_hud_logic.h"

#include <cstdio>
#include <string>

using namespace readyup;

static int g_failures = 0;
static int g_checks = 0;

#define CHECK(cond)                                                                 \
  do {                                                                              \
    ++g_checks;                                                                     \
    if (!(cond)) {                                                                  \
      std::fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #cond); \
      ++g_failures;                                                                 \
    }                                                                               \
  } while (0)

static void TestDisplayName() {
  const uint64_t sid = 76561198000000123ULL;
  CHECK(ReadyHudDisplayName(sid, "Live", "Roster", "Seen") == "Live");
  // A player who is not connected shows the name from the match config.
  CHECK(ReadyHudDisplayName(sid, "", "Dummy CT1", "") == "Dummy CT1");
  CHECK(ReadyHudDisplayName(sid, "", "Dummy CT1", "Seen") == "Dummy CT1");
  CHECK(ReadyHudDisplayName(sid, "", "", "Seen") == "Seen");
  // No name known anywhere: the last 4 digits.
  CHECK(ReadyHudDisplayName(sid, "", "", "") == "#0123");
  CHECK(ReadyHudDisplayName(42, "", "", "") == "#42");
}

int main() {
  TestDisplayName();
  if (g_failures != 0) {
    std::fprintf(stderr, "%d of %d checks failed\n", g_failures, g_checks);
    return 1;
  }
  std::printf("ready_hud: %d checks passed\n", g_checks);
  return 0;
}

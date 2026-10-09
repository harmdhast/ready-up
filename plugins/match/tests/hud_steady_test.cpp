// Offline tests for the center panel blink workaround (readyup/hud_steady_logic.h).
// ctest `match_hud_steady`.
#include "readyup/hud_steady_logic.h"

#include <cstdio>

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

int main() {
  // No restart ever scheduled.
  CHECK(HudSteadyGameRestart(0.0f, 12.5f));
  // A restart that already happened (the round started after it).
  CHECK(HudSteadyGameRestart(30.0f, 31.0f));
  // A restart still pending (mp_restartgame countdown).
  CHECK(!HudSteadyGameRestart(45.0f, 31.0f));
  if (g_failures != 0) {
    std::fprintf(stderr, "%d of %d checks failed\n", g_failures, g_checks);
    return 1;
  }
  std::printf("hud_steady: %d checks passed\n", g_checks);
  return 0;
}

// Offline tests for plugins/skins/game_restart.h: a restart is pending while it is scheduled after
// the current round started. ctest `skins_game_restart`.
#include "game_restart.h"

#include <cstdio>

using namespace skins;

static int g_failures = 0;

#define CHECK(cond)                                                                 \
  do {                                                                              \
    if (!(cond)) {                                                                  \
      std::fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #cond); \
      ++g_failures;                                                                 \
    }                                                                               \
  } while (0)

int main() {
  // mp_restartgame 1 during a round that started at 100 s: pending until the new round starts.
  CHECK(RestartPending(131.0f, 100.0f));
  // The restart happened: the new round started at (or after) the scheduled time.
  CHECK(!RestartPending(131.0f, 131.0f));
  CHECK(!RestartPending(131.0f, 146.0f));
  // No restart ever scheduled.
  CHECK(!RestartPending(0.0f, 100.0f));
  // The same answer whatever hud_steady wrote into m_bGameRestart: the flag is not an input.

  if (g_failures) std::fprintf(stderr, "%d failure(s)\n", g_failures);
  return g_failures ? 1 : 0;
}

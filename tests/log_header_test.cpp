// Player headers of CS2 log lines (core/src/readyup/log_header.h).
#include "readyup/log_header.h"

#include <cstdio>

namespace ru = readyup;

static int g_failed = 0;
static void Check(bool ok, const char* what) {
  std::printf("%s %s\n", ok ? "PASS" : "FAIL", what);
  if (!ok) ++g_failed;
}

int main() {
  // Real CS2 1.41.9.0 headers: name<userid><steamid><team>, three <> groups.
  Check(ru::TeamFromHeader("Harmdhast<0><[U:1:76450349]><CT>") == 3, "human on CT");
  Check(ru::TeamFromHeader("alice<3><[U:1:1]><TERRORIST>") == 2, "human on T");
  Check(ru::TeamFromHeader("alice<3><[U:1:1]><Spectator>") == 1, "spectator");
  Check(ru::TeamFromHeader("alice<3><[U:1:1]><Unassigned>") == 0, "unassigned");
  Check(ru::TeamFromHeader("Harmdhast<0><[U:1:76450349]><>") == -1, "no team yet (connect lines): unknown");
  Check(ru::TeamFromHeader("Harmdhast<0><[U:1:76450349]>") == -1, "switch lines have no team group");
  Check(ru::TeamFromHeader("Jaques<1><BOT><TERRORIST>") == 2, "bot on T");
  Check(ru::IdGroupFromHeader("Jaques<1><BOT><TERRORIST>") == "BOT", "bot id group");
  Check(ru::TeamFromSwitchLine("L 10/09/2026 - 22:21:11: \"Harmdhast<0><[U:1:76450349]>\" switched from team "
                               "<Unassigned> to <CT>") == 3,
        "switch line");

  std::printf("log_header_test: %s\n", g_failed ? "FAIL" : "PASS");
  return g_failed ? 1 : 0;
}

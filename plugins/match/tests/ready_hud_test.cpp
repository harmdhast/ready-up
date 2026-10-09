// Offline tests for the ready panel's player names, team lines, header and count line (readyup/ready_hud_logic.h).
// ctest `match_ready_hud`.
#include "readyup/ready_hud_logic.h"

#include <cstdio>
#include <string>
#include <vector>

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

static bool Has(const std::string& s, const std::string& sub) { return s.find(sub) != std::string::npos; }

static void TestTeamLine() {
  const std::string waiting = ReadyHudTeamLine(true, "Navi", 1, 5, {"Dummy CT1", "<b>Me</b>"}, 2, 0);
  CHECK(Has(waiting, "<font color='#5EA8FF'><b>Navi</b></font>"));
  CHECK(Has(waiting, "<font color='#FFD23F'><b>1/5</b></font>"));
  CHECK(!Has(waiting, "#F87171"));
  CHECK(Has(waiting, "<font color='#8A8F98'>&#183; waiting: Dummy CT1, <b>Me</b></font>") && Has(waiting, "+2"));

  const std::string full = ReadyHudTeamLine(false, "T", 5, 5, {}, 0, 0);
  CHECK(Has(full, "<font color='#FF9D3B'><b>T</b></font>") && Has(full, "<font color='#7CD35A'><b>5/5</b></font>"));
  CHECK(Has(full, "&#183; ready") && !Has(full, "waiting"));

  CHECK(Has(ReadyHudTeamLine(true, "a&amp;b", 0, 5, {"A"}, 0, 0), "<b>a&amp;b</b>"));
  CHECK(Has(ReadyHudTeamLine(true, "CT", 0, 5, {"A"}, 0, 2), "&#10004;2 bots"));
  CHECK(Has(ReadyHudTeamLine(true, "CT", 0, 0, {}, 0, 0), "-"));
}

static void TestHeader() {
  const std::string h = HudHeader("Auto Tournament", kGold, "WARMUP");
  CHECK(h == "<font color='#FFD23F'><b>Auto Tournament</b></font> <font color='#8A8F98'>&#183;</font> "
             "<font color='#FFD23F'><b>WARMUP</b></font>");
  CHECK(HudHeader("", kOk, "KNIFE IN 3") == "<font color='#7CD35A'><b>KNIFE IN 3</b></font>");
  CHECK(Has(HudHeader("A<b>", kNo, "X"), "A&lt;b&gt;"));
  CHECK(!Has(HudHeader("Auto Tournament", kGold, "WARMUP"), "<img"));
}

static void TestCountLine() {
  CHECK(ReadyHudCountLine(3, 10) == "<font class='fontSize-l' color='#FFD23F'><b>3/10 READY</b></font>");
  CHECK(Has(ReadyHudCountLine(10, 10), "color='#7CD35A'"));
  CHECK(Has(ReadyHudCountLine(0, 0), "color='#FFD23F'"));
}

static void TestEscape() {
  CHECK(HudEscape("a<&>'\"", 40) == "a&lt;&amp;&gt;&#39;&quot;");
  // 16-byte cut never splits a UTF-8 sequence: nine "é" are 18 bytes.
  CHECK(HudEscape("\xC3\xA9\xC3\xA9\xC3\xA9\xC3\xA9\xC3\xA9\xC3\xA9\xC3\xA9\xC3\xA9\xC3\xA9", 16) ==
        "\xC3\xA9\xC3\xA9\xC3\xA9\xC3\xA9\xC3\xA9\xC3\xA9\xC3\xA9\xC3\xA9..");
}

static void TestViewerLine() {
  CHECK(ReadyHudViewerLine(ReadyViewer::NotReady, false) == "<font color='#F87171'>type <b>.r</b> to ready up</font>");
  CHECK(ReadyHudViewerLine(ReadyViewer::Ready, true) == "<font color='#7CD35A'><b>READY</b> &#183; .ur to cancel</font>");
  CHECK(Has(ReadyHudViewerLine(ReadyViewer::Absent, false), "spectating"));
  CHECK(Has(ReadyHudViewerLine(ReadyViewer::Absent, true), "join CT or T to play"));
}

static void TestTeamLabel() {
  CHECK(ReadyHudTeamLabel(true, "") == "CT");
  CHECK(ReadyHudTeamLabel(false, "") == "T");
  CHECK(ReadyHudTeamLabel(true, "<Navi>") == "&lt;Navi&gt;");
  CHECK(ReadyHudTeamLabel(true, "Twelve Monkeys Esports") == "Twelve Monkeys E..");
}

int main() {
  TestDisplayName();
  TestTeamLine();
  TestHeader();
  TestCountLine();
  TestEscape();
  TestViewerLine();
  TestTeamLabel();
  if (g_failures != 0) {
    std::fprintf(stderr, "%d of %d checks failed\n", g_failures, g_checks);
    return 1;
  }
  std::printf("ready_hud: %d checks passed\n", g_checks);
  return 0;
}

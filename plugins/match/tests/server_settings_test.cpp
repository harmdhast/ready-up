// Offline tests for readyup/server_settings.h: the table, value checks, chat toggles, where a value
// comes from (runtime > readyup.cfg > built-in), the persist hook, `.settings` lines and the
// hostname_format expansion; plus the ready gate with substitutes and playout (match_rules.h).
// ctest `match_server_settings`.
#include "readyup/match_rules.h"
#include "readyup/server_settings.h"

#include <cstdio>
#include <map>
#include <optional>
#include <string>
#include <vector>

using namespace readyup;
namespace st = readyup::settings;

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

static void TestTable() {
  // Every setting has a console name, a normalized built-in default and help; names are unique.
  std::map<std::string, int> seen;
  for (const auto& s : st::Table()) {
    CHECK(++seen[s.name] == 1);
    std::string v, err;
    CHECK(st::Normalize(s, s.builtin, &v, &err) && v == s.builtin);
    CHECK(s.help && *s.help);
    CHECK(st::ConsoleName(s) == std::string("ru_") + s.name);
    CHECK(st::Find(st::ConsoleName(s)) == &s && st::Find(s.name) == &s);
  }
  CHECK(st::Table().size() == 13);
  // Fleet server.config fields (docs/FLEET.md §7.5) that are server settings.
  CHECK(std::string(st::Find("scrim_when_idle")->builtin) == "1");
  CHECK(st::Find("chat_prefix") && st::Find("chat_prefix")->kind == st::Kind::Str);
  CHECK(st::Find("admin_chat_prefix") && std::string(st::Find("admin_chat_prefix")->cfgKey) == "admin_prefix");
  CHECK(st::Find("RU_Playout_Enabled_Default") != nullptr);
  CHECK(st::Find("playout") == nullptr && st::Find("") == nullptr);
  CHECK(st::FindChat(".playout") == st::Find("playout_enabled_default"));
  CHECK(st::FindChat(".READYREQUIRED") == st::Find("minimum_ready_required"));
  CHECK(st::FindChat(".roundknife") == st::Find("knife_enabled_default"));
  CHECK(st::FindChat(".rk") == st::Find("knife_enabled_default"));
  CHECK(st::FindChat(".whitelist") == st::Find("whitelist_enabled_default"));
  CHECK(st::FindChat(".ready") == nullptr);
  // The defaults keep today's behaviour: whitelist on, knife on, cvars reset.
  CHECK(std::string(st::Find("whitelist_enabled_default")->builtin) == "1");
  CHECK(std::string(st::Find("knife_enabled_default")->builtin) == "1");
  CHECK(st::Find("pause_after_restore") == nullptr);  // round_restore.h owns ru_pause_after_restore
  CHECK(std::string(st::Find("reset_cvars_on_series_end")->builtin) == "1");
  CHECK(std::string(st::Find("playout_enabled_default")->builtin) == "0");
  CHECK(std::string(st::Find("kick_when_no_match_loaded")->builtin) == "0");
}

static void TestNormalize() {
  const auto& b = *st::Find("playout_enabled_default");
  const auto& n = *st::Find("minimum_ready_required");
  const auto& h = *st::Find("hostname_format");
  std::string v, err;
  for (const char* on : {"1", "true", "ON", "yes", " on "}) CHECK(st::Normalize(b, on, &v, &err) && v == "1");
  for (const char* off : {"0", "false", "Off", "no"}) CHECK(st::Normalize(b, off, &v, &err) && v == "0");
  CHECK(!st::Normalize(b, "2", &v, &err) && err.find("0 or 1") != std::string::npos);
  CHECK(!st::Normalize(b, "", &v, &err));
  CHECK(st::Normalize(n, "4", &v, &err) && v == "4");
  CHECK(st::Normalize(n, "004", &v, &err) && v == "4");
  CHECK(st::Normalize(n, "0", &v, &err) && v == "0");
  CHECK(!st::Normalize(n, "-1", &v, &err) && err.find("0 to 32") != std::string::npos);
  CHECK(!st::Normalize(n, "33", &v, &err));
  CHECK(!st::Normalize(n, "4x", &v, &err));
  CHECK(!st::Normalize(n, "99999999999", &v, &err));
  CHECK(st::Normalize(h, "\"{TEAM1} vs {TEAM2}\"", &v, &err) && v == "{TEAM1} vs {TEAM2}");
  CHECK(st::Normalize(h, "", &v, &err) && v.empty());
  CHECK(!st::Normalize(h, "a\nb", &v, &err));
  CHECK(!st::Normalize(h, std::string(128, 'x'), &v, &err));
  CHECK(st::Normalize(h, std::string(127, 'x'), &v, &err));
}

static void TestToggle() {
  const auto& b = *st::Find("playout_enabled_default");
  const auto& n = *st::Find("minimum_ready_required");
  std::string v, err;
  CHECK(st::ToggleValue(b, "0", "", &v, &err) && v == "1");
  CHECK(st::ToggleValue(b, "1", "", &v, &err) && v == "0");
  CHECK(st::ToggleValue(b, "1", "on", &v, &err) && v == "1");
  CHECK(!st::ToggleValue(b, "1", "maybe", &v, &err));
  CHECK(!st::ToggleValue(n, "0", "", &v, &err) && err.find(".readyrequired <value>") != std::string::npos);
  CHECK(st::ToggleValue(n, "0", "3", &v, &err) && v == "3");
  CHECK(st::Display(b, "1") == "on" && st::Display(n, "3") == "3");
  CHECK(st::Display(*st::Find("hostname_format"), "") == "(empty)");
}

static std::vector<std::pair<std::string, std::optional<std::string>>> g_saved;
static void Save(const std::string& name, const std::optional<std::string>& value) { g_saved.emplace_back(name, value); }

static void TestStore() {
  st::Store s;
  s.SetPersistHook(&Save);
  // Built-in default.
  CHECK(s.Get("playout_enabled_default") == "0" && s.Source("playout_enabled_default") == "default");
  CHECK(s.Get("nope").empty() && s.Source("nope").empty());
  // readyup.cfg: the setting's own key, or its legacy key; bad values are ignored.
  s.SetFileValues({{"playout_enabled_default", "true"}, {"min_players_to_ready", "4"}, {"scrim_knife", "0"},
                   {"whitelist_enabled_default", "banana"}, {"welcome", "1"}});
  CHECK(s.Get("playout_enabled_default") == "1" && s.Source("playout_enabled_default") == "cfg");
  CHECK(s.Get("minimum_ready_required") == "4" && s.Source("minimum_ready_required") == "cfg");
  CHECK(s.Get("knife_enabled_default") == "0");
  CHECK(s.Get("whitelist_enabled_default") == "1" && s.Source("whitelist_enabled_default") == "default");
  // The setting's own key wins over the legacy key.
  s.SetFileValues({{"minimum_ready_required", "3"}, {"min_players_to_ready", "4"}});
  CHECK(s.Get("minimum_ready_required") == "3");
  // Runtime wins over the file, is saved, and survives a cfg reload.
  std::string err;
  g_saved.clear();
  CHECK(s.Set("ru_minimum_ready_required", "2", &err));
  CHECK(g_saved.size() == 1 && g_saved[0].first == "minimum_ready_required" && g_saved[0].second == std::string("2"));
  CHECK(s.Get("minimum_ready_required") == "2" && s.Source("minimum_ready_required") == "runtime");
  s.SetFileValues({{"minimum_ready_required", "5"}});
  CHECK(s.Get("minimum_ready_required") == "2");
  // A refused value changes nothing and saves nothing.
  CHECK(!s.Set("minimum_ready_required", "40", &err) && s.Get("minimum_ready_required") == "2" && g_saved.size() == 1);
  CHECK(!s.Set("unknown_thing", "1", &err) && err.find("unknown setting") != std::string::npos);
  // Clearing goes back to the file value and clears the saved one.
  CHECK(s.Clear("minimum_ready_required"));
  CHECK(g_saved.size() == 2 && !g_saved[1].second.has_value());
  CHECK(s.Get("minimum_ready_required") == "5" && s.Source("minimum_ready_required") == "cfg");
  // An empty string is saved quoted (the store reads "" as unset) and loads back empty.
  CHECK(s.Set("hostname_format", "", &err) && g_saved.back().second == std::string("\"\""));
  st::Store t;
  t.LoadRuntime("hostname_format", "\"\"");
  t.LoadRuntime("ru_playout_enabled_default", "1");
  t.LoadRuntime("minimum_ready_required", "junk");  // ignored
  CHECK(t.Source("hostname_format") == "runtime" && t.Get("hostname_format").empty());
  CHECK(t.Get("playout_enabled_default") == "1" && t.Source("minimum_ready_required") == "default");
  // `.settings` lines: table order, the source when not the default.
  const auto lines = st::ShowLines(t);
  CHECK(lines.size() == st::Table().size());
  CHECK(lines[0] == "minimum_ready_required = 0");
  CHECK(lines[1] == "playout_enabled_default = on (runtime)");
}

static void TestHostname() {
  st::HostnameVars v;
  v.team1 = "Alpha";
  v.team2 = "Bravo \"B\"; quit";
  v.matchId = "ko-r1-m3";
  v.map = "de_mirage";
  v.mapNumber = 2;
  v.team1Score = 7;
  v.team2Score = 5;
  v.team1Series = 1;
  CHECK(st::ExpandHostname("{TEAM1} vs {TEAM2}", v) == "Alpha vs Bravo B quit");
  CHECK(st::ExpandHostname("[{MATCH_ID}] {map} #{MAPNUMBER} {TEAM1_SCORE}-{TEAM2_SCORE} ({TEAM1_SERIES}-{TEAM2_SERIES})", v) ==
        "[ko-r1-m3] de_mirage #2 7-5 (1-0)");
  CHECK(st::ExpandHostname("{UNKNOWN} {TEAM1", v) == "{UNKNOWN} {TEAM1");
  CHECK(st::ExpandHostname("  x\\y  ", v) == "xy");
  CHECK(st::ExpandHostname(std::string(200, 'a'), v).size() == 127);
  // A UTF-8 character is never cut in half.
  std::string u;
  for (int i = 0; i < 70; ++i) u += "\xC3\xA6";  // æ, 2 bytes
  const std::string e = st::ExpandHostname(u, v);
  CHECK(e.size() == 126 && e.substr(124) == "\xC3\xA6");
}

static void TestReadyGate() {
  // Full team: players_per_team (0 = 5) capped at the roster.
  CHECK(FullTeamSize(5, 0) == 5 && FullTeamSize(7, 0) == 5 && FullTeamSize(2, 0) == 2 && FullTeamSize(3, 2) == 2);
  CHECK(FullTeamSize(0, 0) == 0);
  // Mixed 2v2 with one roster human per side: that human readies, the anonymous bot does not.
  CHECK(FullTeamSize(1, 2) == 1);
  CHECK(TeamReadyToGoLive(1, 1, 1, 0, 2));
  CHECK(!TeamReadyToGoLive(1, 1, 0, 0, 2));
  // Bot fill: a team waits for its connected humans only; nobody connected (a roster that never
  // joins, bots playing) is ready. 2026-10-12: a 5-player roster with nobody on held the match up.
  CHECK(BotFillTeamReady(0, 0));
  CHECK(BotFillTeamReady(2, 2));
  CHECK(!BotFillTeamReady(2, 1));
  CHECK(BotFillReadyTally(2, 1).ready == 1 && BotFillReadyTally(2, 1).total == 2);
  CHECK(BotFillReadyTally(0, 0).total == 0);
  // Nobody connected: not live on load, a bots-only match after the grace.
  CHECK(!BotFillEmptyMayGoLive(0));
  CHECK(!BotFillEmptyMayGoLive(kBotFillEmptyGraceSeconds - 1));
  CHECK(BotFillEmptyMayGoLive(kBotFillEmptyGraceSeconds));
  // Failover resume: live when everyone is back, or after the rejoin window once anyone is.
  // A resume waits for the whole roster (then the normal ready-up), however long it takes.
  CHECK(!ResumeMayGoLive(4, 0));
  CHECK(ResumeMayGoLive(4, 4));
  CHECK(!ResumeMayGoLive(4, 2));
  CHECK(!ResumeMayGoLive(4, 3));
  {
    // 6 and 5 on the rosters (one substitute), 5 per team: 10 needed, the substitute not counted.
    const int roster[2] = {6, 5};
    const int allBack[2] = {5, 5};
    const ResumeHeadcount h = ResumeCount(roster, allBack, 0);
    CHECK(h.needed == 10 && h.back == 10);
    CHECK(ResumeMayGoLive(h.needed, h.back));
    const int subToo[2] = {6, 4};
    const ResumeHeadcount h2 = ResumeCount(roster, subToo, 0);
    CHECK(h2.back == 9);
    CHECK(!ResumeMayGoLive(h2.needed, h2.back));
    // Wingman with a 3-player roster: 2 per team.
    const int wr[2] = {3, 2};
    const int wp[2] = {2, 2};
    CHECK(ResumeCount(wr, wp, 2).needed == 4 && ResumeCount(wr, wp, 2).back == 4);
  }
  // A roster of 6 (one substitute), rule 0: 5 ready players go live, the 6th does not hold it up.
  CHECK(TeamReadyToGoLive(6, 6, 5, 0, 0));
  CHECK(TeamReadyToGoLive(6, 5, 5, 0, 0));
  CHECK(!TeamReadyToGoLive(6, 6, 4, 0, 0));
  CHECK(!TeamReadyToGoLive(6, 4, 4, 0, 0));  // only 4 there: not a full team
  // Before: the full roster (6) had to be connected and ready. Five from six is enough now.
  CHECK(ForceReadyRequired(6, 0) == 5 && ForceReadyAllowed(5, 6, 0));
  // min 4 of 5: four there and ready is enough; the fifth, once connected, readies too.
  CHECK(TeamReadyToGoLive(5, 4, 4, 4, 0));
  CHECK(!TeamReadyToGoLive(5, 5, 4, 4, 0));
  CHECK(TeamReadyToGoLive(5, 5, 5, 4, 0));
  CHECK(TeamReadyNeeded(5, 5, 4, 0) == 5 && TeamReadyNeeded(5, 3, 4, 0) == 4);
  // Wingman (players_per_team 2) with a substitute.
  CHECK(TeamReadyToGoLive(3, 3, 2, 0, 2) && !TeamReadyToGoLive(3, 3, 1, 0, 2));
  // Empty roster (a side with nobody on the roster): never blocks.
  CHECK(TeamReadyToGoLive(0, 0, 0, 0, 0));
  // The ready count (M1 play-test: 5 + 1 substitute read 10/11): a substitute never raises the
  // total, and a ready substitute standing in for a starter counts up to the full team.
  {
    const ReadyTally t = TeamReadyTally(6, 5, 5);
    CHECK(t.ready == 5 && t.total == 5);
    const ReadyTally all = TeamReadyTally(6, 6, 5);  // everyone ready, substitute too
    CHECK(all.ready == 5 && all.total == 5);
    const ReadyTally some = TeamReadyTally(6, 3, 5);
    CHECK(some.ready == 3 && some.total == 5);
    const ReadyTally small = TeamReadyTally(2, 1, 0);  // a 2-player test roster: 1/2
    CHECK(small.ready == 1 && small.total == 2);
    const ReadyTally none = TeamReadyTally(0, 0, 0);
    CHECK(none.ready == 0 && none.total == 0);
  }
}

static void TestPlayout() {
  // MR12 (24): 13-3 is a clinch without playout, but playout plays all 24 rounds.
  CHECK(PlayoutRoundsLeft(24, true, 3, -1, 13, 3));
  CHECK(!PlayoutRoundsLeft(24, true, 3, -1, 16, 8));
  CHECK(!PlayoutRoundsLeft(24, false, 3, -1, 12, 12));
  // Overtime (MR3 blocks of 6): decided only at the end of a block.
  CHECK(PlayoutRoundsLeft(24, true, 3, -1, 16, 12));  // 4 rounds into OT1
  CHECK(!PlayoutRoundsLeft(24, true, 3, -1, 16, 14));  // OT1 over
  CHECK(PlayoutRoundsLeft(24, true, 3, -1, 16, 15));
  // Past the overtime cap the rounds are sudden death.
  CHECK(!PlayoutRoundsLeft(24, true, 3, 1, 16, 15));
  CHECK(PlayoutRoundsLeft(24, true, 3, 2, 16, 15));
}

int main() {
  TestTable();
  TestNormalize();
  TestToggle();
  TestStore();
  TestHostname();
  TestReadyGate();
  TestPlayout();
  std::printf("server_settings_test: %d checks, %d failures\n", g_checks, g_failures);
  return g_failures == 0 ? 0 : 1;
}

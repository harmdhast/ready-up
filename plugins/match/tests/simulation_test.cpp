// Offline tests for readyup/simulation_rules.h and readyup/wingman.h: timescale, roster identities,
// the bot -> identity assignment (kept across halftime), when to add the next bot, the setup /
// teardown commands, wingman's game mode, the match config fields (wingman defaults, valve refused,
// simulation + timescale, roster names) and cfg/ReadyUp/live_wingman.cfg.
// ctest `match_simulation`.
#include "readyup/match_config_parser.h"
#include "readyup/simulation_rules.h"
#include "readyup/weapon_cleanup.h"
#include "readyup/wingman.h"

#include <algorithm>
#include <cstdio>
#include <fstream>
#include <sstream>
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

static bool Has(const std::vector<std::string>& v, const std::string& s) {
  return std::find(v.begin(), v.end(), s) != v.end();
}

static void TestTimescale() {
  CHECK(sim::ClampTimescale(2.0) == 2.0);
  CHECK(sim::ClampTimescale(0.0) == 1.0 && sim::ClampTimescale(-3) == 1.0);
  CHECK(sim::ClampTimescale(0.01) == sim::kMinTimescale && sim::ClampTimescale(50) == sim::kMaxTimescale);
  CHECK(sim::TimescaleText(2.0) == "2" && sim::TimescaleText(1.5) == "1.5" && sim::TimescaleText(0.25) == "0.25");
  CHECK(sim::TimescaleCommands(1.0).empty());  // real time: sv_cheats is left alone
  const auto ts = sim::TimescaleCommands(3.0);
  CHECK(ts.size() == 2 && ts[0] == "sv_cheats 1" && ts[1] == "host_timescale 3");
  CHECK(Has(sim::RealTimeCommands(), "host_timescale 1") && Has(sim::RealTimeCommands(), "sv_cheats 0"));
}

static void TestIdentities() {
  std::vector<sim::Identity> roster = {
      {76561198000000003ull, "c", 1}, {76561198000000001ull, "a", 1}, {76561198000000011ull, "x", 2}, {5, "bad", 3}};
  auto ids = sim::PlanIdentities(roster, 5);
  CHECK(ids.size() == 3);
  CHECK(ids[0].name == "a" && ids[1].name == "c" && ids[2].name == "x");  // team1 then team2, by id
  // A team without a roster gets anonymous fillers.
  ids = sim::PlanIdentities({{76561198000000001ull, "a", 1}}, 2);
  CHECK(ids.size() == 3 && ids[1].steamid64 == 0 && ids[1].team == 2 && ids[2].team == 2);
  CHECK(sim::PlanIdentities({}, 5).size() == 10);

  CHECK(sim::SideOfTeam(1, true) == 3 && sim::SideOfTeam(2, true) == 2);
  CHECK(sim::SideOfTeam(1, false) == 2 && sim::SideOfTeam(2, false) == 3 && sim::SideOfTeam(0, true) == 0);
  int ct = 0, t = 0;
  sim::WantedPerSide(sim::PlanIdentities({{1, "a", 1}, {2, "b", 1}, {3, "c", 2}}, 5), false, &ct, &t);
  CHECK(ct == 1 && t == 2);  // team2 (1 player) on CT, team1 (2) on T
}

static void TestAssign() {
  // 2v2: team1 = a, b; team2 = x, y. team1 starts CT.
  const auto ids = sim::PlanIdentities({{1, "a", 1}, {2, "b", 1}, {11, "x", 2}, {12, "y", 2}}, 2);
  // Bots on CT take team1's identities, bots on T team2's; a bot on no side waits.
  auto m = sim::Assign({}, {{5, 3}, {3, 2}, {4, 0}, {6, 3}}, ids, true);
  CHECK(m.size() == 3);
  CHECK(m.count(3) && ids[static_cast<size_t>(m[3])].name == "x");
  CHECK(m.count(5) && ids[static_cast<size_t>(m[5])].name == "a");
  CHECK(m.count(6) && ids[static_cast<size_t>(m[6])].name == "b");
  CHECK(!m.count(4));
  // A third CT bot finds no free team1 identity.
  auto m2 = sim::Assign(m, {{5, 3}, {3, 2}, {6, 3}, {7, 3}}, ids, true);
  CHECK(m2.size() == 3 && !m2.count(7));
  // Halftime: sides swap, identities stay with their bots.
  auto m3 = sim::Assign(m2, {{5, 2}, {3, 3}, {6, 2}, {8, 3}}, ids, false);
  CHECK(m3[5] == m[5] && m3[3] == m[3] && m3[6] == m[6]);
  CHECK(m3.count(8) && ids[static_cast<size_t>(m3[8])].name == "y");  // team2 is CT now
  // A bot that left frees its identity for the next bot on that side.
  auto m4 = sim::Assign(m3, {{3, 3}, {6, 2}, {8, 3}, {9, 2}}, ids, false);
  CHECK(!m4.count(5) && m4.count(9) && ids[static_cast<size_t>(m4[9])].name == "a");
}

static void TestFeeder() {
  sim::BotFeeder f;
  int q = 0;
  // Nobody there, 2 CT + 2 T wanted: CT first.
  CHECK(f.Next(10.0, 0, 0, 0, 2, 2, 6, &q) == 3 && q == 1);
  // The bot is not there yet: wait.
  CHECK(f.Next(11.5, 0, 0, 0, 2, 2, 6, &q) == 0 && q == 0);
  // It joined but has no side yet: wait.
  CHECK(f.Next(12.0, 1, 0, 0, 2, 2, 6, &q) == 0);
  // On CT: the short side (T) is next, at least a second after the last add.
  CHECK(f.Next(12.5, 1, 1, 0, 2, 2, 6, &q) == 2 && q == 2);
  CHECK(f.Next(12.6, 2, 1, 1, 2, 2, 6, &q) == 0);  // too soon
  CHECK(f.Next(13.6, 2, 1, 1, 2, 2, 6, &q) == 3 && q == 3);
  CHECK(f.Next(14.7, 3, 2, 1, 2, 2, 6, &q) == 2 && q == 4);
  CHECK(f.Next(16.0, 4, 2, 2, 2, 2, 6, &q) == 0);  // full
  CHECK(f.QuotaSent() == 4);
  // A bot was kicked (3 left): wait kSettleSeconds for the engine, then add it again.
  CHECK(f.Next(20.0, 3, 2, 1, 2, 2, 6, &q) == 0);
  CHECK(f.Next(20.0 + sim::BotFeeder::kSettleSeconds - 0.5, 3, 2, 1, 2, 2, 6, &q) == 0);
  CHECK(f.Next(20.0 + sim::BotFeeder::kSettleSeconds + 0.1, 3, 2, 1, 2, 2, 6, &q) == 2 && q == 4);
  // Never above maxBots.
  sim::BotFeeder g;
  CHECK(g.Next(0.0, 6, 6, 0, 2, 2, 6, &q) == 0);
  // Reset (new map): starts from the bots there.
  f.Reset();
  CHECK(f.QuotaSent() == 0 && f.Next(100.0, 0, 0, 0, 1, 1, 4, &q) == 3 && q == 1);
  // Ready delays: 1.5 .. 3.5 s.
  for (int i = 0; i < 12; ++i) CHECK(sim::ReadyDelaySeconds(i) >= 1.5 && sim::ReadyDelaySeconds(i) < 3.5);
}

static void TestCommands() {
  const auto setup = sim::SetupCommands();
  for (const char* c : {"bot_quota 0", "bot_kick", "bot_quota_mode normal", "bot_join_after_player 0",
                        "mp_autoteambalance 0", "mp_limitteams 0", "mp_autokick 0", "bot_stop 0", "bot_freeze 0",
                        "bot_dont_shoot 0", "bot_ignore_enemies 0"}) {
    if (!Has(setup, c)) std::fprintf(stderr, "setup lacks `%s`\n", c);
    CHECK(Has(setup, c));
  }
  // The quota goes to 0 before the kick (else the engine adds the kicked bots again).
  CHECK(setup[0] == "bot_quota 0" && setup[1] == "bot_kick");
  const auto add = sim::AddBotCommands(3, 4);
  CHECK(add.size() == 2 && add[0] == "bot_join_team CT" && add[1] == "bot_quota 4");
  CHECK(sim::AddBotCommands(2, 1)[0] == "bot_join_team T");
  // Bots on the wrong side start the fill over; short or over on one side only does not.
  CHECK(sim::WrongSides(0, 10, 5, 5));      // all ten on T after an engine refill
  CHECK(sim::WrongSides(6, 4, 5, 5));
  CHECK(!sim::WrongSides(5, 5, 5, 5));
  CHECK(!sim::WrongSides(3, 2, 5, 5));      // still filling
  CHECK(!sim::WrongSides(6, 5, 5, 5));      // one too many: trimmed, not refilled
  const auto down = sim::TeardownCommands();
  CHECK(Has(down, "bot_quota 0") && Has(down, "bot_kick") && Has(down, "bot_join_team any") &&
        Has(down, "bot_join_after_player 1"));

  const auto w = wingman::GameModeCommands(true), c = wingman::GameModeCommands(false);
  CHECK(w.size() == 2 && w[0] == "game_type 0" && w[1] == "game_mode 2");
  CHECK(c.size() == 2 && c[0] == "game_type 0" && c[1] == "game_mode 1");
}

static std::optional<WebhookMatchContext> Parse(const std::string& json, std::string* err = nullptr) {
  std::string e;
  auto ctx = ParseWebhookMatchContextFromJson(json, &e);
  if (err) *err = e;
  return ctx;
}

static void TestParser() {
  const std::string teams =
      R"("team1":{"name":"A","players":{"76561198000000001":"alpha","76561198000000002":"bravo"}},)"
      R"("team2":{"name":"B","players":{"76561198000000011":"xray","76561198000000012":"yankee"}})";
  // Plain 5v5: nothing set.
  auto c = Parse(R"({"matchid":7,"maplist":["de_dust2"],)" + teams + "}");
  CHECK(c && !c->wingman && !c->simulation && c->simulation_timescale == 1.0 && c->maxRounds == 24);
  CHECK(c && c->roster_names.size() == 4 && c->roster_names[76561198000000011ull] == "xray");

  // Wingman: MR8 and MR2 overtime unless the config sets them.
  c = Parse(R"({"matchid":7,"wingman":true,"maplist":["de_shortdust"],)" + teams + "}");
  CHECK(c && c->wingman && c->maxRounds == wingman::kMaxRounds && c->overtimeSegments == wingman::kOvertimeHalf);
  CHECK(c && c->players_per_team == wingman::kPlayersPerTeam);
  c = Parse(R"({"matchid":7,"wingman":true,"players_per_team":3,"maplist":["de_shortdust"],)" + teams + "}");
  CHECK(c && c->players_per_team == 3);
  c = Parse(R"({"matchid":7,"maplist":["de_dust2"],)" + teams + "}");
  CHECK(c && c->players_per_team == 0);
  c = Parse(R"({"matchid":7,"wingman":true,"maxRounds":12,"overtimeSegments":3,"maplist":["de_shortdust"],)" + teams + "}");
  CHECK(c && c->maxRounds == 12 && c->overtimeSegments == 3);
  c = Parse(R"({"matchid":7,"wingman":"true","cvars":{"mp_maxrounds":20},"maplist":["de_shortdust"],)" + teams + "}");
  CHECK(c && c->wingman && c->maxRounds == 20);  // mp_maxrounds still wins over the default
  // Wingman + the valve ruleset (5v5) is refused.
  std::string err;
  c = Parse(R"({"matchid":7,"wingman":true,"ruleset":"valve","map_sides":["team1_ct"],"maplist":["de_shortdust"],)" +
                teams + "}",
            &err);
  CHECK(!c && err.find("wingman") != std::string::npos);

  // Simulation: flag + timescale (number or string, clamped).
  c = Parse(R"({"matchid":7,"simulation":true,"simulation_timescale":2.5,"maplist":["de_dust2"],)" + teams + "}");
  CHECK(c && c->simulation && c->simulation_timescale == 2.5);
  c = Parse(R"({"config":{"matchid":7,"simulation":1,"simulation_timescale":"40","maplist":["de_dust2"],)" + teams +
            "}}");
  CHECK(c && c->simulation && c->simulation_timescale == sim::kMaxTimescale);
  c = Parse(R"({"matchid":7,"simulation":false,"simulation_timescale":"x","maplist":["de_dust2"],)" + teams + "}");
  CHECK(c && !c->simulation && c->simulation_timescale == 1.0);
}

static std::string ReadCfg(const char* name) {
  std::ifstream f(std::string(READYUP_CFG_DIR) + "/" + name);
  std::stringstream ss;
  ss << f.rdbuf();
  return ss.str();
}

static bool CfgSets(const std::string& cfg, const std::string& cmd) {
  const size_t sp = cmd.find(' ');
  const std::string cvar = cmd.substr(0, sp), value = cmd.substr(sp + 1);
  std::istringstream in(cfg);
  std::string line;
  while (std::getline(in, line)) {
    std::istringstream ls(line);
    std::string k, v;
    if ((ls >> k >> v) && k == cvar && v == value) return true;
  }
  return false;
}

static void TestWingmanCfg() {
  const std::string cfgName = std::string(wingman::kLiveCfg).substr(std::string("ReadyUp/").size());
  const std::string w = ReadCfg(cfgName.c_str()), live = ReadCfg("live.cfg");
  CHECK(!w.empty() && !live.empty());
  for (const char* c : {"mp_maxrounds 16", "mp_maxmoney 8000", "mp_overtime_maxrounds 4", "mp_freezetime 10",
                        "mp_roundtime 1.5", "mp_roundtime_defuse 1.5", "cash_team_loser_bonus 2000",
                        "cash_team_bonus_shorthanded 1000", "mp_autoteambalance 0", "mp_limitteams 0",
                        "mp_halftime 1", "mp_match_can_clinch 1", "mp_overtime_enable 1"}) {
    if (!CfgSets(w, c)) std::fprintf(stderr, "live_wingman.cfg lacks `%s`\n", c);
    CHECK(CfgSets(w, c));
  }
  // What live.cfg puts back after warmup, the wingman cfg does too (weapon_cleanup.h).
  for (const char* l : kLiveDropCmds) {
    if (!CfgSets(w, l)) std::fprintf(stderr, "live_wingman.cfg lacks `%s`\n", l);
    CHECK(CfgSets(w, l));
  }
  CHECK(w.find("exec ReadyUp/live_wingman_override.cfg") != std::string::npos);
  CHECK(w.find("exec ReadyUp/live_override.cfg") == std::string::npos);
  CHECK(!ReadCfg("live_wingman_override.cfg").empty());
}

static void TestFeederTrim() {
  sim::BotFeeder f;
  int q = 0;
  // 12 bots for 10 roster players, all on a side: back to 10, then not again for a while.
  CHECK(f.Trim(100.0, 12, 6, 6, 5, 5, &q) && q == 10);
  CHECK(!f.Trim(100.0 + sim::BotFeeder::kSettleSeconds - 0.5, 12, 6, 6, 5, 5, &q));
  CHECK(f.Trim(100.0 + sim::BotFeeder::kSettleSeconds + 0.1, 11, 6, 5, 5, 5, &q) && q == 10);
  // Exactly the rosters, or fewer: nothing.
  sim::BotFeeder g;
  CHECK(!g.Trim(1.0, 10, 5, 5, 5, 5, &q));
  CHECK(!g.Trim(1.0, 8, 4, 4, 5, 5, &q));
  // A bot still on no side: wait for it.
  CHECK(!g.Trim(1.0, 11, 5, 5, 5, 5, &q));
}

static void TestHeldSeats() {
  std::map<uint64_t, double> live = {{1, 0}, {2, 0}};
  // Both on: nothing held.
  CHECK(sim::HeldSeats(&live, {1, 2}, 100, 300).empty());
  // 2 leaves: its seat is held, from when it left.
  CHECK(sim::HeldSeats(&live, {1}, 110, 300) == std::vector<uint64_t>{2});
  CHECK(live[2] == 110);
  CHECK(sim::HeldSeats(&live, {1}, 400, 300) == std::vector<uint64_t>{2});
  // Back in time: not missing any more.
  CHECK(sim::HeldSeats(&live, {1, 2}, 405, 300).empty());
  CHECK(live[2] == 0);
  // Gone past the hold: a bot may take the seat (and it is forgotten).
  CHECK(sim::HeldSeats(&live, {1}, 500, 300) == std::vector<uint64_t>{2});
  CHECK(sim::HeldSeats(&live, {1}, 801, 300).empty());
  CHECK(live.count(2) == 0);
  // No hold time: never held.
  std::map<uint64_t, double> none = {{3, 0}};
  CHECK(sim::HeldSeats(&none, {}, 10, 0).empty());
}

static void TestMixedFill() {
  CHECK(!Has(sim::FillSetupCommands(), "bot_kick"));
  CHECK(!Has(sim::FillTeardownCommands(), "bot_kick"));
  CHECK(Has(sim::FillSetupCommands(), "bot_quota 0"));
  const std::vector<sim::Bot> bots = {{10, 3}, {11, 2}};
  auto p = sim::PlanFill(2, 1, 1, bots);
  CHECK(p.ctWanted == 1 && p.tWanted == 1 && p.removeUserids.empty());
  p = sim::PlanFill(2, 2, 1, bots);
  CHECK(p.ctWanted == 0 && p.tWanted == 1 && p.removeUserids == std::vector<int>{10});
  p = sim::PlanFill(2, 1, 0, bots);
  CHECK(p.ctWanted == 1 && p.tWanted == 2 && p.removeUserids.empty());
  p = sim::PlanFill(2, 1, 2, {{10, 2}, {11, 3}}); // side swap
  CHECK(p.ctWanted == 1 && p.tWanted == 0 && p.removeUserids == std::vector<int>{10});
  p = sim::PlanFill(2, 2, 2, {{12, 1}, {13, 0}}); // spectator/GOTV and joining bot untouched
  CHECK(p.removeUserids.empty());
  std::string err;
  auto c = ParseWebhookMatchContextFromJson(R"({"matchid":7,"bot_fill":true,"simulation":true,"simulation_timescale":4,"players_per_team":2,"maplist":["de_dust2"],"team1":{"players":{"76561198000000001":"Sivert"}},"team2":{"players":{"76561198000000002":"Emil"}}})", &err);
  CHECK(c && c->bot_fill && !c->simulation && c->simulation_timescale == 1.0 && c->players_per_team == 2);
  CHECK(c && c->roster_team.size() == 2);
}

int main() {
  TestHeldSeats();
  TestMixedFill();
  TestTimescale();
  TestIdentities();
  TestAssign();
  TestFeeder();
  TestFeederTrim();
  TestCommands();
  TestParser();
  TestWingmanCfg();
  std::printf("match_simulation: %d checks, %d failures\n", g_checks, g_failures);
  return g_failures == 0 ? 0 : 1;
}

// Offline tests for the match stats model (match_stats.h), the map/series-end logic
// (match_end.h), demo naming and upload headers (demo_recorder.h) and the AT webhook payloads
// (at_payloads.h). No CS2 server needed:
//   cmake --build build && (cd build && ctest --output-on-failure)

#include "readyup/at_payloads.h"
#include "readyup/demo_recorder.h"
#include "readyup/match_end.h"
#include "readyup/match_stats.h"
#include "readyup/minijson.h"

#include <cstdio>
#include <initializer_list>
#include <optional>
#include <string>
#include <vector>

using namespace readyup;

static int g_failures = 0;

#define CHECK(cond)                                                    \
  do {                                                                 \
    if (!(cond)) {                                                     \
      std::fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #cond); \
      ++g_failures;                                                    \
    }                                                                  \
  } while (0)

#define CHECK_EQ(a, b)                                                                            \
  do {                                                                                            \
    const auto va = (a);                                                                          \
    const auto vb = (b);                                                                          \
    if (!(va == vb)) {                                                                            \
      std::fprintf(stderr, "%s:%d: CHECK_EQ failed: %s == %s (%lld vs %lld)\n", __FILE__, __LINE__, #a, #b, \
                   static_cast<long long>(va), static_cast<long long>(vb));                       \
      ++g_failures;                                                                               \
    }                                                                                             \
  } while (0)

static void CheckStr(const std::string& a, const std::string& b, const char* expr, int line) {
  if (a == b) return;
  std::fprintf(stderr, "%s:%d: CHECK_STR failed: %s: \"%s\" != \"%s\"\n", __FILE__, line, expr, a.c_str(),
               b.c_str());
  ++g_failures;
}
#define CHECK_STR(a, b) CheckStr((a), (b), #a, __LINE__)

static const stats::PlayerLine* FindPlayer(const stats::MapStats& m, uint64_t id) {
  for (const auto& p : m.players) {
    if (p.id == id) return &p;
  }
  return nullptr;
}

static bool ParsesAsJson(const std::string& s) {
  minijson::ParseError err;
  auto v = minijson::Parse(s, &err);
  if (!v) std::fprintf(stderr, "JSON parse error at %zu: %s\n", err.offset, err.msg.c_str());
  return v.has_value();
}

// team1 = A1 (1), A2 (2) starting CT; team2 = B1 (11), B2 (12) starting T.
constexpr uint64_t A1 = 76561198000000001ull, A2 = 76561198000000002ull;
constexpr uint64_t B1 = 76561198000000011ull, B2 = 76561198000000012ull;

static void ObserveAll(stats::StatsAccumulator& st, int team1Side) {
  const int team2Side = team1Side == 3 ? 2 : 3;
  st.ObservePlayer(A1, "A1", team1Side, 1);
  st.ObservePlayer(A2, "A2", team1Side, 1);
  st.ObservePlayer(B1, "B1", team2Side, 2);
  st.ObservePlayer(B2, "B2", team2Side, 2);
}

static void TestStatsRound1() {
  stats::StatsAccumulator st;
  st.BeginMap(/*team1IsCt=*/true);
  st.OnRoundStart();
  ObserveAll(st, 3);

  st.OnPlayerHurt(B1, A1, 150, 0, "ak47");       // overkill: counts 100
  st.OnPlayerDeath(B1, A1, 0, false, true, "ak47", 10.0);
  st.OnPlayerBlind(A1, B2, 2.0);                 // enemy flashed
  st.OnPlayerBlind(B1, B2, 1.0);                 // friendly flashed
  st.OnPlayerBlind(A2, B2, 0.0);                 // zero duration: ignored
  st.OnPlayerHurt(A2, B2, 100, 0, "glock");
  st.OnPlayerDeath(A2, B2, 0, false, false, "glock", 12.0);
  // A1 is now alone (1v1) and trades A2 within 5 s.
  st.OnPlayerHurt(B2, A1, 100, 0, "ak47");
  st.OnPlayerDeath(B2, A1, 0, false, false, "ak47", 14.0);
  st.OnRoundMvp(A1);
  st.SetScore(A1, 7);
  st.OnRoundEnd(3, 8);

  const auto m = st.Snapshot();
  CHECK_EQ(m.team1.score, 1);
  CHECK_EQ(m.team1.score_ct, 1);
  CHECK_EQ(m.team2.score, 0);
  CHECK_EQ(m.rounds.size(), 1u);
  CHECK_EQ(m.rounds[0].winner_team, 1);
  CHECK_EQ(m.rounds[0].winner_side, 3);
  CHECK_EQ(m.rounds[0].reason, 8);

  const auto* a1 = FindPlayer(m, A1);
  const auto* a2 = FindPlayer(m, A2);
  const auto* b1 = FindPlayer(m, B1);
  const auto* b2 = FindPlayer(m, B2);
  CHECK(a1 && a2 && b1 && b2);
  if (!(a1 && a2 && b1 && b2)) return;
  CHECK_EQ(a1->stats.kills, 2);
  CHECK_EQ(a1->stats.headshot_kills, 1);
  CHECK_EQ(a1->stats.damage, 200);
  CHECK_EQ(a1->stats.entry_kills_ct, 1);
  CHECK_EQ(a1->stats.trade_kills, 1);
  CHECK_EQ(a1->stats.multi_kills[1], 1);
  CHECK_EQ(a1->stats.clutches_won[0], 1);
  CHECK_EQ(a1->stats.kast_rounds, 1);
  CHECK_EQ(a1->stats.rounds_played, 1);
  CHECK_EQ(a1->stats.mvp, 1);
  CHECK_EQ(a1->stats.score, 7);
  CHECK_EQ(a2->stats.deaths, 1);
  CHECK_EQ(a2->stats.traded_deaths, 1);
  CHECK_EQ(a2->stats.kast_rounds, 1);  // traded
  CHECK_EQ(b1->stats.entry_deaths_t, 1);
  CHECK_EQ(b1->stats.kast_rounds, 0);
  CHECK_EQ(b2->stats.kills, 1);
  CHECK_EQ(b2->stats.enemies_flashed, 1);
  CHECK_EQ(b2->stats.friendlies_flashed, 1);
  CHECK_EQ(b2->stats.kast_rounds, 1);
  CHECK_EQ(b2->stats.clutches_won[1], 0);  // was last alive vs 2 but lost

  // Round 2: team damage, utility damage, flash assist, suicide, team kill.
  st.OnRoundStart();
  ObserveAll(st, 3);
  st.OnPlayerHurt(B1, B2, 50, 50, "ak47");           // team damage: not counted
  st.OnPlayerHurt(A1, B1, 40, 60, "hegrenade");
  st.OnPlayerDeath(A1, B1, B2, true, false, "ak47", 30.0);
  st.OnPlayerDeath(A2, 0, 0, false, false, "world", 31.0);
  st.OnPlayerDeath(B2, B1, 0, false, false, "ak47", 32.0);  // team kill
  st.OnRoundEnd(2, 9);

  const auto m2 = st.Snapshot();
  CHECK_EQ(m2.team2.score, 1);
  CHECK_EQ(m2.team2.score_t, 1);
  CHECK_EQ(m2.rounds.size(), 2u);
  const auto* b1r = FindPlayer(m2, B1);
  const auto* b2r = FindPlayer(m2, B2);
  const auto* a2r = FindPlayer(m2, A2);
  CHECK(b1r && b2r && a2r);
  if (!(b1r && b2r && a2r)) return;
  CHECK_EQ(b1r->stats.kills, 1);
  CHECK_EQ(b1r->stats.team_kills, 1);
  CHECK_EQ(b1r->stats.damage, 40);
  CHECK_EQ(b1r->stats.utility_damage, 40);
  CHECK_EQ(b2r->stats.flash_assists, 1);
  CHECK_EQ(b2r->stats.assists, 0);
  CHECK_EQ(b2r->stats.damage, 100);  // round 1 only; B2 hitting B1 in round 2 is team damage
  CHECK_EQ(a2r->stats.suicides, 1);
  CHECK_EQ(a2r->stats.kills, 0);
  CHECK_EQ(a2r->stats.rounds_played, 2);

  const std::string json = stats::ToJson(m2);
  CHECK(ParsesAsJson(json));
  CHECK(json.find("\"clutches_won\":[1,0,0,0,0]") != std::string::npos);
  CHECK(json.find("\"id\":\"76561198000000001\"") != std::string::npos);
}

static void TestStatsSidesAndGate() {
  stats::StatsAccumulator st;
  // Not live: nothing is recorded.
  st.ObservePlayer(A1, "A1", 3, 1);
  st.OnPlayerDeath(B1, A1, 0, false, false, "ak47", 1.0);
  st.OnRoundEnd(3, 8);
  CHECK_EQ(st.Snapshot().rounds.size(), 0u);
  CHECK_EQ(st.Team1Score(), 0);

  // team2 starts CT; after the swap team1 is CT.
  st.BeginMap(/*team1IsCt=*/false);
  st.OnRoundStart();
  st.OnRoundEnd(3, 7);
  CHECK_EQ(st.Team2Score(), 1);
  st.SetTeam1IsCt(true);
  st.OnRoundStart();
  st.OnRoundEnd(3, 7);
  CHECK_EQ(st.Team1Score(), 1);
  const auto m = st.Snapshot();
  CHECK_EQ(m.team1.score_ct, 1);
  CHECK_EQ(m.team2.score_ct, 1);
  // A player without a roster team gets it from the side it was first seen on.
  st.OnRoundStart();
  st.ObservePlayer(999, "bot", 2, 0, /*bot=*/true);
  const auto m2 = st.Snapshot();
  const auto* bot = FindPlayer(m2, 999);
  CHECK(bot && bot->team == 2 && bot->bot);
  st.EndMap();
  CHECK(!st.Live());
}

// Plugin reload: ToJson(MapStats) -> FromJson -> Restore continues the map with the same
// totals, sides and round list.
// Round economy (round_economy.h): money at the start and the buys go into the round's player
// rows and survive the JSON round trip; a row without them stays as it was.
static void TestStatsEconomy() {
  stats::StatsAccumulator st;
  st.BeginMap(/*team1IsCt=*/true);
  st.OnRoundStart();
  ObserveAll(st, 3);
  st.SetMoneyStart(A1, 4750);
  st.SetLoadout(A1, 150, 100, true, false, {"m4a1", "smokegrenade"});
  st.OnRoundEnd(3, 8);
  const auto m = st.Snapshot();
  CHECK(m.rounds.size() == 1);
  const stats::PlayerRound* a1 = nullptr;
  const stats::PlayerRound* b1 = nullptr;
  for (const auto& p : m.rounds[0].players) {
    if (p.id == A1) a1 = &p;
    if (p.id == B1) b1 = &p;
  }
  CHECK(a1 && a1->money_start == 4750 && a1->money_after_buy == 150 && a1->armor == 100 && a1->helmet &&
        !a1->defuser && a1->loadout.size() == 2 && a1->loadout[0] == "m4a1");
  CHECK(b1 && b1->money_start == -1);
  const std::string json = stats::ToJson(m);
  CHECK(json.find("\"money_start\":4750") != std::string::npos);
  CHECK(json.find("\"loadout\":[\"m4a1\",\"smokegrenade\"]") != std::string::npos);
  stats::MapStats back;
  CHECK(stats::FromJson(json, &back));
  const stats::PlayerRound* a1b = nullptr;
  for (const auto& p : back.rounds[0].players)
    if (p.id == A1) a1b = &p;
  CHECK(a1b && a1b->money_start == 4750 && a1b->loadout.size() == 2 && a1b->helmet);
}

static void TestStatsRestore() {
  stats::StatsAccumulator st;
  st.BeginMap(/*team1IsCt=*/true);
  st.OnRoundStart();
  ObserveAll(st, 3);
  st.OnPlayerDeath(B1, A1, A2, /*assistedFlash=*/false, /*headshot=*/true, "ak47", 1.0);
  st.OnPlayerHurt(B2, A2, 40, 60, "m4a1");
  st.OnRoundEnd(3, 8);
  st.SetTeam1IsCt(false);  // halftime
  const auto before = st.Snapshot();
  const std::string json = stats::ToJson(before);

  stats::MapStats parsed;
  CHECK(stats::FromJson(json, &parsed));
  stats::StatsAccumulator st2;
  st2.Restore(parsed);
  CHECK(st2.Live());
  CHECK(!st2.Team1IsCt());
  CHECK_EQ(st2.Team1Score(), 1);
  CHECK_EQ(st2.Team2Score(), 0);
  CHECK_STR(stats::ToJson(st2.Snapshot()), json);
  // The next round continues on top of the restored totals.
  st2.OnRoundStart();
  ObserveAll(st2, 2);
  st2.OnPlayerDeath(A1, B1, 0, false, false, "glock", 2.0);
  st2.OnRoundEnd(3, 8);  // team2 is CT now
  CHECK_EQ(st2.Team2Score(), 1);
  const auto* a1 = FindPlayer(st2.Snapshot(), A1);
  CHECK(a1 && a1->stats.kills == 1 && a1->stats.deaths == 1 && a1->stats.headshot_kills == 1);
  CHECK_EQ(st2.Snapshot().rounds.size(), 2u);
  CHECK(!stats::FromJson("[1,2]", &parsed));
}

static void TestMapEndPlan() {
  auto p = ComputeMapEndPlan(/*demo=*/false, /*upload=*/false, 10, 5, 10, 60);
  CHECK_EQ(p.restartDelay, 10);
  CHECK_EQ(p.tvFlushDelay, 0);
  CHECK_EQ(p.kickDelay, 14);
  p = ComputeMapEndPlan(true, false, 0, 5, 10, 60);
  CHECK_EQ(p.tvFlushDelay, 15);
  CHECK_EQ(p.restartDelay, 15);
  CHECK_EQ(p.kickDelay, 24);
  p = ComputeMapEndPlan(true, true, 10, 5, 10, 60);
  CHECK_EQ(p.tvFlushDelay, 25);
  CHECK_EQ(p.restartDelay, 35);
  CHECK_EQ(p.kickDelay, 94);
  p = ComputeMapEndPlan(true, true, 120, 5, 10, 60);
  CHECK_EQ(p.tvFlushDelay, 135);
}

static void TestKickDelayAfterSeriesEnd() {
  CHECK_EQ(KickDelayAfterSeriesEnd(10, 0), 9.0);
  CHECK_EQ(KickDelayAfterSeriesEnd(10, 5), 14.0);
  CHECK_EQ(KickDelayAfterSeriesEnd(10, 30), kMatchEndKickMaxS);
  CHECK_EQ(KickDelayAfterSeriesEnd(35, 30), kMatchEndKickMaxS);
  CHECK_EQ(KickDelayAfterSeriesEnd(135, 0), kMatchEndKickMaxS);
  CHECK_EQ(KickDelayAfterSeriesEnd(1, -5), 0.0);
}

static void TestSeriesOver() {
  // Bo1.
  CHECK(IsSeriesOver(1, RemainingMaps(1, 1, 1), 1, 0, true));
  // Bo3 after map 1 (1-0): continue.
  CHECK_EQ(RemainingMaps(3, 3, 1), 2);
  CHECK(!IsSeriesOver(3, RemainingMaps(3, 3, 1), 1, 0, true));
  // Bo3 2-0 after map 2: clinched; played out without clinch.
  CHECK(IsSeriesOver(3, RemainingMaps(3, 3, 2), 2, 0, true));
  CHECK(!IsSeriesOver(3, RemainingMaps(3, 3, 2), 2, 0, false));
  // Bo3 1-1 with a drawn map: no maps left after map 3.
  CHECK(IsSeriesOver(3, RemainingMaps(3, 3, 3), 1, 1, true));
  // Map list shorter than num_maps.
  CHECK_EQ(RemainingMaps(5, 2, 2), 0);
  // Bo2 1-1: over after map 2 (a draw).
  CHECK(IsSeriesOver(2, RemainingMaps(2, 2, 2), 1, 1, true));
}

static void TestDemoNaming() {
  demo::TokenValues v;
  v.time = "2026-09-25_12-00-00";
  v.matchid = 42;
  v.slug = "r1m1";
  v.map = "de_dust2";
  v.mapNumber = 2;
  v.team1 = "Team A";
  v.team2 = "B/C";
  v.team1Score = 13;
  v.team2Score = 11;
  CHECK_STR(demo::FormatDemoFileName("{TIME}_{MATCH_ID}_{MAP}_{TEAM1}_vs_{TEAM2}", v), std::string("2026-09-25_12-00-00_42_de_dust2_Team_A_vs_B_C"));
  CHECK_STR(demo::ExpandTokens("m{MAP_NUMBER}/i{MAP_INDEX}/{TEAM1_SCORE}-{TEAM2_SCORE}/{SLUG}", v), std::string("m2/i1/13-11/r1m1"));
  v.fileName = "x.dem";
  v.roundNumber = 24;
  CHECK_STR(demo::ExpandTokens("{FILENAME}:{ROUND_NUMBER}", v), std::string("x.dem:24"));
  CHECK_STR(demo::FormatDemoFileName("../{MAP}", v), std::string("_de_dust2"));

  std::vector<demo::DemoCandidate> files = {
      {"/csgo/ReadyUp/old_42_de_dust2.dem", 100},
      {"/csgo/ReadyUp/new_42_de_dust2.dem", 200},
      {"/csgo/ReadyUp/new_420_de_dust2.dem", 300},
      {"/csgo/ReadyUp/new_42_de_mirage.dem", 400},
  };
  CHECK_STR(demo::PickDemoForMatch(files, "missing.dem", 42, "de_dust2", 0), std::string("/csgo/ReadyUp/new_42_de_dust2.dem"));
  CHECK_STR(demo::PickDemoForMatch(files, "old_42_de_dust2.dem", 42, "de_dust2", 0), std::string("/csgo/ReadyUp/old_42_de_dust2.dem"));
  CHECK_STR(demo::PickDemoForMatch(files, "missing.dem", 42, "de_dust2", 250), std::string());

  demo::DemoEvent e;
  e.type = demo::DemoEventType::UploadFailed;
  e.fileName = "a\"b.dem";
  e.error = "HTTP 500";
  CHECK(ParsesAsJson(demo::ToJson(e)));
}

static void TestMatchFlowEventJson() {
  stats::StatsAccumulator st;
  st.BeginMap(true);
  st.OnRoundStart();
  ObserveAll(st, 3);
  st.OnPlayerDeath(B1, A1, 0, false, false, "ak47", 1.0);
  st.OnRoundEnd(3, 8);
  MatchFlowEvent e;
  e.type = MatchFlowEventType::MapResult;
  e.matchid = 7;
  e.slug = "s\"lug";
  e.winner = "team1";
  e.team1Score = 13;
  e.team2Score = 5;
  e.team1SeriesScore = 1;
  e.stats = st.Snapshot();
  const std::string j = ToJson(e);
  CHECK(ParsesAsJson(j));
  CHECK(j.find("\"type\":\"map_result\"") != std::string::npos);
  e.type = MatchFlowEventType::SeriesEnd;
  e.secondsUntilReset = 94;
  const std::string j2 = ToJson(e);
  CHECK(ParsesAsJson(j2));
  CHECK(j2.find("\"seconds_until_reset\":94") != std::string::npos);
}

// ---- AT payloads (at_payloads.h) and upload headers ------------------------------------------

using minijson::Value;

static std::optional<Value> ParseJson(const std::string& s) {
  minijson::ParseError err;
  auto v = minijson::Parse(s, &err);
  if (!v) std::fprintf(stderr, "JSON parse error at %zu: %s in %s\n", err.offset, err.msg.c_str(), s.c_str());
  return v;
}

// obj.a.b... as a number (-9999 when missing / not a number).
static long long Num(const Value* v, std::initializer_list<const char*> path) {
  for (const char* k : path) v = v ? v->get(k) : nullptr;
  return v && v->type == Value::Type::Number ? static_cast<long long>(v->num) : -9999;
}

static std::string Text(const Value* v, std::initializer_list<const char*> path) {
  for (const char* k : path) v = v ? v->get(k) : nullptr;
  if (!v) return "<missing>";
  if (v->type == Value::Type::Null) return "<null>";
  if (v->type == Value::Type::Bool) return v->b ? "<true>" : "<false>";
  return v->type == Value::Type::String ? v->str : "<not a string>";
}

// team1 / team2 player by steamid (string).
static const Value* AtPlayer(const Value& root, const char* team, uint64_t id) {
  const Value* ps = root.get(team) ? root.get(team)->get("players") : nullptr;
  if (!ps || ps->type != Value::Type::Array) return nullptr;
  for (const auto& p : ps->arr) {
    if (Text(&p, {"steamid"}) == std::to_string(id)) return &p;
  }
  return nullptr;
}

static size_t AtPlayerCount(const Value& root, const char* team) {
  const Value* ps = root.get(team) ? root.get(team)->get("players") : nullptr;
  return ps && ps->type == Value::Type::Array ? ps->arr.size() : 0;
}

// Round 1 (team1 CT wins, A1: entry, trade, 1v1 clutch, 2k), a bot on team2.
static stats::StatsAccumulator PlayedRound1() {
  stats::StatsAccumulator st;
  st.BeginMap(/*team1IsCt=*/true);
  st.OnRoundStart();
  ObserveAll(st, 3);
  st.ObservePlayer(900, "BOT Ada", 2, 2, /*bot=*/true);
  st.OnPlayerDeath(B1, A1, 0, false, true, "ak47", 10.0);
  st.OnPlayerDeath(A2, B2, 0, false, false, "glock", 12.0);
  st.OnPlayerDeath(B2, A1, 0, false, false, "ak47", 14.0);
  st.OnPlayerDeath(900, A1, 0, false, false, "knife", 15.0);
  st.OnRoundMvp(A1);
  st.OnRoundEnd(3, 8);
  return st;
}

static void TestAtPlayerStats() {
  stats::PlayerStats s;
  s.kills = 3;
  s.multi_kills = {1, 2, 3, 4, 5};
  s.clutches_won = {6, 7, 8, 9, 10};
  s.entry_kills_t = 11;
  s.entry_kills_ct = 12;
  s.entry_deaths_t = 13;
  s.entry_deaths_ct = 14;
  s.kast_rounds = 2;
  s.rounds_played = 3;
  s.mvp = 4;
  s.bomb_plants = 5;
  s.bomb_defuses = 6;
  const auto v = ParseJson(at::PlayerStatsJson(s));
  CHECK(v.has_value());
  if (!v) return;
  CHECK_EQ(Num(&*v, {"kills"}), 3);
  CHECK_EQ(Num(&*v, {"1k"}), 1);
  CHECK_EQ(Num(&*v, {"5k"}), 5);
  CHECK_EQ(Num(&*v, {"1v1"}), 6);
  CHECK_EQ(Num(&*v, {"1v5"}), 10);
  CHECK_EQ(Num(&*v, {"first_kills_t"}), 11);
  CHECK_EQ(Num(&*v, {"first_kills_ct"}), 12);
  CHECK_EQ(Num(&*v, {"first_deaths_t"}), 13);
  CHECK_EQ(Num(&*v, {"first_deaths_ct"}), 14);
  CHECK_EQ(Num(&*v, {"kast"}), 67);  // 2 of 3 rounds, in percent
  CHECK_EQ(Num(&*v, {"mvp"}), 4);
  CHECK_EQ(Num(&*v, {"bomb_plants"}), 5);
  CHECK_EQ(Num(&*v, {"bomb_defuses"}), 6);
  // Every AT PlayerStats field (MatchData.cs) is there.
  for (const char* k : {"kills", "deaths", "assists", "flash_assists", "team_kills", "suicides", "damage",
                        "utility_damage", "enemies_flashed", "friendlies_flashed", "knife_kills", "headshot_kills",
                        "rounds_played", "bomb_defuses", "bomb_plants", "1k", "2k", "3k", "4k", "5k", "1v1", "1v2",
                        "1v3", "1v4", "1v5", "first_kills_t", "first_kills_ct", "first_deaths_t", "first_deaths_ct",
                        "trade_kills", "kast", "score", "mvp"}) {
    if (Num(&*v, {k}) == -9999) {
      std::fprintf(stderr, "AT PlayerStats field missing: %s\n", k);
      ++g_failures;
    }
  }
  CHECK_EQ(at::KastPercent(stats::PlayerStats{}), 0);  // no rounds played
  s.kast_rounds = 1;
  s.rounds_played = 8;
  CHECK_EQ(at::KastPercent(s), 13);  // 12.5 rounds up
}

static void TestAtRoundEnd() {
  auto st = PlayedRound1();
  at::RoundEnd r;
  r.matchid = 42;
  r.map_number = 2;
  r.round_number = 1;
  r.round_time_ms = 51234;
  r.reason = 8;
  r.winner_side = 3;
  r.team1 = {"7", "Alpha \"A\"", 1};
  r.team2 = {"", "Bravo", 0};
  r.stats = st.Snapshot();
  const std::string json = at::RoundEndJson(r);
  const auto v = ParseJson(json);
  CHECK(v.has_value());
  if (!v) return;
  CHECK_STR(Text(&*v, {"event"}), std::string("round_end"));
  CHECK_EQ(Num(&*v, {"matchid"}), 42);
  CHECK_EQ(Num(&*v, {"map_number"}), 2);
  CHECK_EQ(Num(&*v, {"round_number"}), 1);
  CHECK_EQ(Num(&*v, {"round_time"}), 51234);
  CHECK_EQ(Num(&*v, {"reason"}), 8);
  CHECK_STR(Text(&*v, {"winner", "side"}), std::string("3"));
  CHECK_STR(Text(&*v, {"winner", "team"}), std::string("team1"));
  CHECK_STR(Text(&*v, {"team1", "id"}), std::string("7"));
  CHECK_STR(Text(&*v, {"team1", "name"}), std::string("Alpha \"A\""));
  CHECK_EQ(Num(&*v, {"team1", "series_score"}), 1);
  CHECK_EQ(Num(&*v, {"team1", "score"}), 1);
  CHECK_EQ(Num(&*v, {"team1", "score_ct"}), 1);
  CHECK_EQ(Num(&*v, {"team1", "score_t"}), 0);
  CHECK_EQ(Num(&*v, {"team2", "score"}), 0);
  CHECK_EQ(Num(&*v, {"team1_score"}), 1);  // flat fallback for the normalizer
  CHECK_EQ(AtPlayerCount(*v, "team1"), 2u);
  CHECK_EQ(AtPlayerCount(*v, "team2"), 2u);  // the bot is left out
  const Value* a1 = AtPlayer(*v, "team1", A1);
  CHECK(a1 != nullptr);
  if (a1) {
    CHECK_STR(Text(a1, {"name"}), std::string("A1"));
    CHECK_EQ(Num(a1, {"stats", "kills"}), 3);
    CHECK_EQ(Num(a1, {"stats", "headshot_kills"}), 1);
    CHECK_EQ(Num(a1, {"stats", "knife_kills"}), 1);
    CHECK_EQ(Num(a1, {"stats", "3k"}), 1);
    CHECK_EQ(Num(a1, {"stats", "first_kills_ct"}), 1);
    CHECK_EQ(Num(a1, {"stats", "trade_kills"}), 1);
    CHECK_EQ(Num(a1, {"stats", "1v2"}), 1);  // alone against B2 and the bot
    CHECK_EQ(Num(a1, {"stats", "kast"}), 100);
    CHECK_EQ(Num(a1, {"stats", "mvp"}), 1);
    CHECK_EQ(Num(a1, {"stats", "rounds_played"}), 1);
  }
  const Value* b1 = AtPlayer(*v, "team2", B1);
  CHECK(b1 && Num(b1, {"stats", "first_deaths_t"}) == 1 && Num(b1, {"stats", "kast"}) == 0);

  // Round 2 after halftime (team1 now T) won by T: team1 wins it on the T side.
  st.SetTeam1IsCt(false);
  st.OnRoundStart();
  ObserveAll(st, 2);
  st.OnRoundEnd(2, 9);
  r.stats = st.Snapshot();
  r.winner_side = 2;
  r.round_number = 2;
  const auto v2 = ParseJson(at::RoundEndJson(r));
  CHECK(v2.has_value());
  if (!v2) return;
  CHECK_STR(Text(&*v2, {"winner", "side"}), std::string("2"));
  CHECK_STR(Text(&*v2, {"winner", "team"}), std::string("team1"));
  CHECK_EQ(Num(&*v2, {"team1", "score"}), 2);
  CHECK_EQ(Num(&*v2, {"team1", "score_t"}), 1);

  // A drawn / unknown winner.
  r.winner_side = 0;
  const auto v3 = ParseJson(at::RoundEndJson(r));
  CHECK(v3 && Text(&*v3, {"winner", "side"}) == "0" && Text(&*v3, {"winner", "team"}) == "none");
}

static void TestAtMapResult() {
  auto st = PlayedRound1();
  st.EndMap();
  at::MapResult m;
  m.matchid = 42;
  m.map_number = 1;
  m.map_name = "de_mirage";
  m.team1_score = 13;
  m.team2_score = 7;
  m.winner = "team1";
  m.team1 = {"7", "Alpha", 1};
  m.team2 = {"8", "Bravo", 0};
  m.stats = st.Snapshot();
  const auto v = ParseJson(at::MapResultJson(m));
  CHECK(v.has_value());
  if (!v) return;
  CHECK_STR(Text(&*v, {"event"}), std::string("map_result"));
  CHECK_STR(Text(&*v, {"map_name"}), std::string("de_mirage"));
  CHECK_STR(Text(&*v, {"winner", "team"}), std::string("team1"));
  CHECK_STR(Text(&*v, {"winner", "side"}), std::string("3"));  // team1 ends the map on CT
  CHECK_EQ(Num(&*v, {"team1", "score"}), 13);                  // the decided score, not the model's
  CHECK_EQ(Num(&*v, {"team1", "score_ct"}), 1);
  CHECK_EQ(Num(&*v, {"team1", "series_score"}), 1);
  CHECK_STR(Text(&*v, {"team2", "id"}), std::string("8"));
  CHECK_EQ(Num(&*v, {"team2_score"}), 7);
  const Value* a2 = AtPlayer(*v, "team1", A2);
  CHECK(a2 && Num(a2, {"stats", "deaths"}) == 1 && Num(a2, {"stats", "kast"}) == 100);  // traded

  // team2 wins while on T; a draw has no winner.
  m.winner = "team2";
  const auto v2 = ParseJson(at::MapResultJson(m));
  CHECK(v2 && Text(&*v2, {"winner", "side"}) == "2" && Text(&*v2, {"winner", "team"}) == "team2");
  m.winner = "none";
  const auto v3 = ParseJson(at::MapResultJson(m));
  CHECK(v3 && Text(&*v3, {"winner", "side"}) == "0" && Text(&*v3, {"winner", "team"}) == "none");
  // No stats recorded (e.g. a forfeit before any round): empty player lists, still valid.
  m.stats = stats::MapStats{};
  const auto v4 = ParseJson(at::MapResultJson(m));
  CHECK(v4 && AtPlayerCount(*v4, "team1") == 0 && Num(&*v4, {"team1", "score"}) == 13);
}

// ru_match_stats: the round_end / map_result team blocks, so the platform's stats parsing
// (matchEventHandler: team1.players[].steamid + stats.kills / damage / rounds_played / kast / mvp)
// reads it unchanged.
static void TestAtMatchStats() {
  auto st = PlayedRound1();
  at::MatchStatsLine m;
  m.matchid = 42;
  m.map_number = 2;
  m.map_name = "de_mirage";
  m.team1 = {"7", "Alpha", 1};
  m.team2 = {"8", "Bravo", 0};
  m.stats = st.Snapshot();
  const auto v = ParseJson(at::MatchStatsJson(m));
  CHECK(v.has_value());
  if (!v) return;
  CHECK_STR(Text(&*v, {"event"}), std::string("match_stats"));
  CHECK_EQ(Num(&*v, {"matchid"}), 42);
  CHECK_EQ(Num(&*v, {"map_number"}), 2);
  CHECK_STR(Text(&*v, {"map_name"}), std::string("de_mirage"));
  CHECK_EQ(Num(&*v, {"round_number"}), 1);
  CHECK_STR(Text(&*v, {"team1", "name"}), std::string("Alpha"));
  CHECK_EQ(Num(&*v, {"team1", "series_score"}), 1);
  CHECK_EQ(Num(&*v, {"team1", "score"}) + Num(&*v, {"team2", "score"}), 1);  // one round played
  CHECK_EQ(Num(&*v, {"team1_score"}), Num(&*v, {"team1", "score"}));
  const Value* a2 = AtPlayer(*v, "team1", A2);
  CHECK(a2 && Num(a2, {"stats", "deaths"}) == 1 && Num(a2, {"stats", "rounds_played"}) == 1 &&
        Num(a2, {"stats", "kast"}) == 100);
  // No match loaded / nothing recorded: still one valid object, empty player lists.
  const auto e = ParseJson(at::MatchStatsJson(at::MatchStatsLine{}));
  CHECK(e && Num(&*e, {"matchid"}) == 0 && Num(&*e, {"round_number"}) == 0 && AtPlayerCount(*e, "team1") == 0 &&
        Text(&*e, {"live"}) == "<false>");
}

static void TestAtDemoEvents() {
  demo::DemoEvent e;
  e.matchid = 42;
  e.mapNumber = 2;
  e.fileName = "x.dem";
  e.type = demo::DemoEventType::RecordingStarted;
  auto out = at::DemoEventJsons(e);
  CHECK_EQ(out.size(), 1u);
  auto v = ParseJson(out.empty() ? "" : out[0]);
  CHECK(v && Text(&*v, {"event"}) == "demo_recording_start" && Num(&*v, {"matchid"}) == 42 &&
        Num(&*v, {"map_number"}) == 2 && Text(&*v, {"filename"}) == "x.dem");

  e.type = demo::DemoEventType::RecordingStopped;
  out = at::DemoEventJsons(e);
  CHECK(out.size() == 1 && out[0].find("\"event\":\"demo_recording_stop\"") != std::string::npos);

  e.type = demo::DemoEventType::UploadStarted;
  e.sizeMb = 123.456;
  out = at::DemoEventJsons(e);
  v = ParseJson(out.empty() ? "" : out[0]);
  CHECK(v && Text(&*v, {"event"}) == "demo_upload_start" && out[0].find("\"size_mb\":123.46") != std::string::npos);

  e.type = demo::DemoEventType::UploadSucceeded;
  e.httpStatus = 201;
  out = at::DemoEventJsons(e);
  CHECK_EQ(out.size(), 2u);
  if (out.size() == 2) {
    v = ParseJson(out[0]);
    CHECK(v && Text(&*v, {"event"}) == "demo_upload_success" && Text(&*v, {"status"}) == "201");
    v = ParseJson(out[1]);
    CHECK(v && Text(&*v, {"event"}) == "demo_upload_ended" && Text(&*v, {"success"}) == "<true>" &&
          Num(&*v, {"map_number"}) == 2);
  }

  e.type = demo::DemoEventType::UploadFailed;
  e.httpStatus = 413;
  e.error = "payload too large";
  out = at::DemoEventJsons(e);
  CHECK_EQ(out.size(), 2u);
  if (out.size() == 2) {
    v = ParseJson(out[0]);
    CHECK(v && Text(&*v, {"event"}) == "demo_upload_fail" && Text(&*v, {"status"}) == "413" &&
          Text(&*v, {"reason"}) == "payload too large");
    v = ParseJson(out[1]);
    CHECK(v && Text(&*v, {"event"}) == "demo_upload_ended" && Text(&*v, {"success"}) == "<false>");
  }
  e.httpStatus = 0;
  e.error = "Couldn't connect to server";
  out = at::DemoEventJsons(e);
  v = ParseJson(out.empty() ? "" : out[0]);
  CHECK(v && Text(&*v, {"status"}) == "no_response" && Text(&*v, {"reason"}) == "Couldn't connect to server");
  e.error = "file_not_found";
  e.sizeMb = 0;
  out = at::DemoEventJsons(e);
  v = ParseJson(out.empty() ? "" : out[0]);
  CHECK(v && Text(&*v, {"status"}) == "file_not_found" && Text(&*v, {"size_mb"}) == "<null>");
}

static bool HasLine(const std::vector<std::string>& ls, const std::string& l) {
  for (const auto& x : ls) {
    if (x == l) return true;
  }
  std::fprintf(stderr, "header line missing: %s\n", l.c_str());
  return false;
}

static void TestUploadHeaders() {
  demo::Settings s;
  demo::TokenValues v;
  v.matchid = 42;
  v.slug = "r1m1";
  v.mapNumber = 2;
  v.fileName = "x.dem";
  v.roundNumber = 24;
  auto hs = demo::UploadHeaderLines(s, v);
  CHECK_EQ(hs.size(), 8u);
  CHECK(HasLine(hs, "Auto-Tournament-FileName: x.dem"));
  CHECK(HasLine(hs, "Auto-Tournament-MatchId: 42"));
  CHECK(HasLine(hs, "Auto-Tournament-MapNumber: 2"));
  CHECK(HasLine(hs, "Auto-Tournament-RoundNumber: 24"));
  CHECK(HasLine(hs, "Get5-FileName: x.dem"));
  CHECK(HasLine(hs, "Get5-RoundNumber: 24"));

  // ru_demo_upload_header entries (tokens expanded) replace a built-in one of the same name.
  s.uploadHeaders = {{"auto-tournament-matchid", "{SLUG}"}, {"X-Extra", "m{MAP_NUMBER}"}};
  // Key without a value is not sent.
  s.headerKey = "X-Auto-Tournament-Token";
  hs = demo::UploadHeaderLines(s, v);
  CHECK_EQ(hs.size(), 9u);
  CHECK(HasLine(hs, "Auto-Tournament-MatchId: r1m1"));
  CHECK(HasLine(hs, "X-Extra: m2"));
  s.headerValue = "tok{SLUG}";  // the token is sent as is
  hs = demo::UploadHeaderLines(s, v);
  CHECK_EQ(hs.size(), 10u);
  CHECK(HasLine(hs, "X-Auto-Tournament-Token: tok{SLUG}"));
  // The key / value pair wins over an entry with the same name.
  s.uploadHeaders.push_back({"x-auto-tournament-token", "old"});
  hs = demo::UploadHeaderLines(s, v);
  CHECK_EQ(hs.size(), 10u);
  CHECK(HasLine(hs, "x-auto-tournament-token: tok{SLUG}"));
}

// Per match (rules.demo.upload): -1 = the server's upload URL, 0 = never, 1 = only with a URL.
static void TestUploadWanted() {
  demo::Settings s;
  s.uploadUrl = "https://at.example/api/demos";
  CHECK(demo::UploadWanted(s, -1) && demo::UploadWanted(s, 1) && !demo::UploadWanted(s, 0));
  demo::Settings none;
  CHECK(!demo::UploadWanted(none, -1) && !demo::UploadWanted(none, 1));
}

int main() {
  TestAtPlayerStats();
  TestAtRoundEnd();
  TestAtMapResult();
  TestAtMatchStats();
  TestAtDemoEvents();
  TestUploadHeaders();
  TestUploadWanted();
  TestStatsRound1();
  TestStatsSidesAndGate();
  TestStatsEconomy();
  TestStatsRestore();
  TestMapEndPlan();
  TestKickDelayAfterSeriesEnd();
  TestSeriesOver();
  TestDemoNaming();
  TestMatchFlowEventJson();
  if (g_failures) {
    std::fprintf(stderr, "match_flow_test: %d failure(s)\n", g_failures);
    return 1;
  }
  std::printf("match_flow_test: OK\n");
  return 0;
}

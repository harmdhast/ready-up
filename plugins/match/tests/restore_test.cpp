// Offline tests for round restore and crash recovery (readyup/round_restore_rules.h): backup file
// names and the `ru match backups` list, `.restore <round>` parsing and refusals, the
// pause_after_restore chain, the progress record and what a restarted server recovers.
// ctest `match_restore`.
#include "readyup/match_rules.h"
#include "readyup/match_stats.h"
#include "readyup/round_restore_rules.h"

#include <cstdio>
#include <string>
#include <vector>

using namespace readyup;
using namespace readyup::restore;

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

static void TestFileNames() {
  CHECK(BackupPrefix(4242, 2) == "readyup_backup_4242_map2_");
  CHECK(BackupPrefix(4242, 0) == "readyup_backup_4242_map1_");

  BackupInfo b;
  CHECK(ParseBackupFileName("readyup_backup_4242_map2_round07.txt", 4242, &b));
  CHECK(b.map_number == 2 && b.round == 8 && b.file == "readyup_backup_4242_map2_round07.txt");
  CHECK(ParseBackupFileName("readyup_backup_4242_map1_round00.txt", 4242, &b) && b.round == 1);
  CHECK(ParseBackupFileName("readyup_backup_4242_map1_round123.txt", 4242, &b) && b.round == 124);
  // What CS2 writes for BackupPrefix ("..._map1_" + "_round03.txt"): a double underscore.
  CHECK(ParseBackupFileName("readyup_backup_4242_map1__round03.txt", 4242, &b));
  CHECK(b.map_number == 1 && b.round == 4 && b.file == "readyup_backup_4242_map1__round03.txt");
  CHECK(ParseBackupFileName("readyup_backup_4242_map2__round00.txt", 4242, &b) && b.map_number == 2 && b.round == 1);
  CHECK(ParseBackupFileName(BackupPrefix(4242, 3) + "_round11.txt", 4242, &b) && b.map_number == 3 && b.round == 12);
  CHECK(!ParseBackupFileName("readyup_backup_4242_map1___round03.txt", 4242, &b));
  CHECK(!ParseBackupFileName("readyup_backup_4242_map__round03.txt", 4242, &b));
  // Another match (also one whose id starts the same), not a backup, a path, a temp file.
  CHECK(!ParseBackupFileName("readyup_backup_42_map1_round03.txt", 4242, &b));
  CHECK(!ParseBackupFileName("readyup_backup_42420_map1_round03.txt", 4242, &b));
  CHECK(!ParseBackupFileName("readyup_resume_4242_map1_round03.txt", 4242, &b));
  CHECK(!ParseBackupFileName("readyup_backup_4242_map1_round03.txt.tmp", 4242, &b));
  CHECK(!ParseBackupFileName("readyup_backup_4242_map1_roundx.txt", 4242, &b));
  CHECK(!ParseBackupFileName("readyup_backup_4242_map_round03.txt", 4242, &b));
  CHECK(!ParseBackupFileName("readyup_backup_4242_map0_round03.txt", 4242, &b));
  CHECK(!ParseBackupFileName("readyup_backup_4242_map1_round.txt", 4242, &b));
  CHECK(!ParseBackupFileName("../readyup_backup_4242_map1_round03.txt", 4242, &b));
}

static void TestList() {
  std::vector<BackupInfo> in = {
      {"readyup_backup_7_map2_round01.txt", 2, 2, 50, 10},
      {"readyup_backup_7_map1_round10.txt", 1, 11, 10, 10},
      {"readyup_backup_7_map1_round02.txt", 1, 3, 5, 10},
      {"old_copy", 1, 3, 1, 10},  // same map / round, older: dropped
      {"readyup_backup_7_map2_round00.txt", 2, 1, 40, 10},
  };
  const auto s = SortBackups(in);
  CHECK(s.size() == 4);
  CHECK(s[0].map_number == 1 && s[0].round == 3 && s[0].file == "readyup_backup_7_map1_round02.txt");
  CHECK(s[1].map_number == 1 && s[1].round == 11);
  CHECK(s[2].map_number == 2 && s[2].round == 1);
  CHECK(s[3].map_number == 2 && s[3].round == 2);
  const BackupInfo* l = LatestForMap(s, 1);
  CHECK(l && l->round == 11);
  CHECK(LatestForMap(s, 2) && LatestForMap(s, 2)->round == 2);
  CHECK(LatestForMap(s, 3) == nullptr);
  CHECK(BackupListLine(s[0], 1, 1) == "map 1 round 3 (1-1) readyup_backup_7_map1_round02.txt");
  CHECK(BackupListLine(s[0], -1, -1) == "map 1 round 3 readyup_backup_7_map1_round02.txt");

  stats::MapStats m;
  for (int i = 1; i <= 3; ++i) {
    stats::RoundSummary r;
    r.round_number = i;
    r.team1_score = i;  // team1 won every round
    r.team2_score = 0;
    m.rounds.push_back(r);
  }
  int a = -1, b = -1;
  CHECK(ScoreAtRoundStart(m, 1, &a, &b) && a == 0 && b == 0);
  CHECK(ScoreAtRoundStart(m, 3, &a, &b) && a == 2 && b == 0);
  CHECK(ScoreAtRoundStart(m, 4, &a, &b) && a == 3 && b == 0);
  CHECK(!ScoreAtRoundStart(m, 5, &a, &b));
}

static void TestRestoreCommand() {
  std::string err;
  CHECK(ParseRestoreRound("5", &err) == 5);
  CHECK(ParseRestoreRound("1", &err) == 1);
  CHECK(ParseRestoreRound("0", &err) == -1 && !err.empty());
  CHECK(ParseRestoreRound("", &err) == -1);
  CHECK(ParseRestoreRound("-3", &err) == -1);
  CHECK(ParseRestoreRound("4a", &err) == -1);
  CHECK(ParseRestoreRound("1000", &err) == -1);

  RestoreCheck c;
  CHECK(RestoreRefusal(c) == "no match loaded.");
  c.match_loaded = true;
  CHECK(RestoreRefusal(c) == "a restore needs a live map.");
  c.live = true;
  c.rounds_played = 7;
  c.round = 8;  // the round being played: restart it
  CHECK(RestoreRefusal(c).empty());
  c.round = 1;
  CHECK(RestoreRefusal(c).empty());
  c.round = 9;
  CHECK(RestoreRefusal(c) == "round 9 has not been played yet (this is round 8).");
  c.round = 0;
  CHECK(!RestoreRefusal(c).empty());
}

static void TestBackupFlagHint() {
  const std::string hint = BackupFlagHint();
  CHECK(hint.find("-allow_round_backup_files") != std::string::npos);
  CHECK(hint.find("csm 1.27.4+") != std::string::npos && hint.find("restart the server") != std::string::npos);
  const std::string missing = "no backup readyup_backup_4242_map1_round04.txt";
  CHECK(ExplainRestoreError(missing) == missing + ". " + hint);
  CHECK(ExplainRestoreError("no match loaded") == "no match loaded");
  CHECK(ExplainRestoreError("round must be >= 1") == "round must be >= 1");

  const auto has = [](const std::string& raw) { return CmdlineHasArg(raw, kRoundBackupFlag); };
  const std::string nul(1, '\0');
  CHECK(has("/srv/cs2" + nul + "-dedicated" + nul + "-allow_round_backup_files" + nul + "+map" + nul));
  CHECK(has("cs2" + nul + "-allow_round_backup_files"));  // last arg, no trailing NUL
  CHECK(!has("cs2" + nul + "-dedicated" + nul));
  CHECK(!has("cs2" + nul + "-allow_round_backup_files_x" + nul));
  CHECK(!has(""));
}

static void TestPauseAfterRestore() {
  CHECK(PauseAfterRestoreFor(-1, -1, -1) == 1);  // default on
  CHECK(PauseAfterRestoreFor(-1, -1, 0) == 0);   // readyup.cfg
  CHECK(PauseAfterRestoreFor(-1, 1, 0) == 1);    // the console setting beats readyup.cfg
  CHECK(PauseAfterRestoreFor(-1, 0, 1) == 0);
  CHECK(PauseAfterRestoreFor(1, 0, 0) == 1);     // the match config beats both
  CHECK(PauseAfterRestoreFor(0, 1, -1) == 0);
  // The rule chain (ru match rules) has it too.
  CHECK(BuiltinDefaultRules().pause_after_restore == 1);
  MatchRules m, base;
  base.pause_after_restore = 0;
  CHECK(ResolveRules(m, base).pause_after_restore == 0);
  m.pause_after_restore = 1;
  CHECK(ResolveRules(m, base).pause_after_restore == 1);
  CHECK(ResolveRules(MatchRules{}, MatchRules{}).pause_after_restore == 1);
}

static void TestProgress() {
  Progress p;
  p.map_number = 2;
  p.phase = "live";
  p.series_team1 = 1;
  p.series_team2 = 0;
  stats::StatsAccumulator acc;
  acc.BeginMap(true);
  acc.ObservePlayer(76561198000000001ull, "alice", 3, 1);
  acc.ObservePlayer(76561198000000002ull, "bob", 2, 2);
  acc.OnRoundStart();
  acc.OnPlayerDeath(76561198000000002ull, 76561198000000001ull, 0, false, true, "ak47", 1.0);
  acc.OnRoundEnd(3, 8);
  p.stats_json = stats::ToJson(acc.Snapshot());
  p.have_events = true;
  p.events.roundNumber = 1;
  p.events.lastMapNumber = 2;
  p.events.swapCount = 0;
  p.events.lastHalfStartTotal = -1;
  MatchEventsState::Totals t;
  t.steamid64 = 76561198000000001ull;
  t.name = "alice \"the\" first";
  t.team = 1;
  t.kills = 1;
  t.headshot_kills = 1;
  t.damage = 100;
  p.events.players.push_back(t);

  Progress q;
  CHECK(ProgressFromJson(ProgressToJson(p), &q));
  CHECK(q.map_number == 2 && q.phase == "live" && q.series_team1 == 1 && q.series_team2 == 0);
  CHECK(q.have_events && q.events.roundNumber == 1 && q.events.lastMapNumber == 2 && q.events.lastHalfStartTotal == -1);
  CHECK(q.events.players.size() == 1 && q.events.players[0].steamid64 == 76561198000000001ull &&
        q.events.players[0].name == "alice \"the\" first" && q.events.players[0].kills == 1 &&
        q.events.players[0].damage == 100);
  stats::MapStats m;
  CHECK(stats::FromJson(q.stats_json, &m));
  CHECK(m.team1.score == 1 && m.team2.score == 0 && m.rounds.size() == 1);
  bool alice = false;
  for (const auto& l : m.players) alice |= l.id == 76561198000000001ull && l.stats.kills == 1 && l.stats.headshot_kills == 1;
  CHECK(alice);

  Progress over;
  over.phase = "map_over";
  CHECK(ProgressFromJson(ProgressToJson(over), &q) && q.phase == "map_over" && !q.have_events && q.stats_json.empty());
  CHECK(!ProgressFromJson("", &q));
  CHECK(!ProgressFromJson("{\"v\":2,\"phase\":\"live\"}", &q));
  CHECK(!ProgressFromJson("{\"v\":1,\"phase\":\"sideways\"}", &q));
  CHECK(!ProgressFromJson("not json", &q));
}

static void TestPlanRecovery() {
  const std::vector<std::string> bo3 = {"de_mirage", "de_nuke", "de_inferno"};
  // Older state.json without a progress record: what recovery did before (map 1).
  RecoveryPlan r = PlanRecovery(nullptr, true, bo3, 3, true);
  CHECK(r.kind == RecoveryPlan::Kind::Live && r.map_number == 1 && r.map_entry == "de_mirage");
  r = PlanRecovery(nullptr, false, bo3, 3, true);
  CHECK(r.kind == RecoveryPlan::Kind::Warmup && r.map_number == 1);

  Progress p;
  p.map_number = 2;
  p.phase = "live";
  p.series_team1 = 1;
  r = PlanRecovery(&p, false, bo3, 3, true);
  CHECK(r.kind == RecoveryPlan::Kind::Live && r.map_number == 2 && r.map_entry == "de_nuke");
  p.phase = "warmup";
  r = PlanRecovery(&p, true, bo3, 3, true);
  CHECK(r.kind == RecoveryPlan::Kind::Warmup && r.map_number == 2);
  // Map 2 over at 1-1: map 3 in warmup.
  p.phase = "map_over";
  p.series_team2 = 1;
  r = PlanRecovery(&p, true, bo3, 3, true);
  CHECK(r.kind == RecoveryPlan::Kind::Warmup && r.map_number == 3 && r.map_entry == "de_inferno");
  // Map 2 over at 2-0: the series is over (clinched).
  p.series_team1 = 2;
  p.series_team2 = 0;
  r = PlanRecovery(&p, true, bo3, 3, true);
  CHECK(r.kind == RecoveryPlan::Kind::None);
  // ... unless every map is played (no clinch).
  r = PlanRecovery(&p, true, bo3, 3, false);
  CHECK(r.kind == RecoveryPlan::Kind::Warmup && r.map_number == 3);
  // Last map over: nothing to recover.
  p.map_number = 3;
  r = PlanRecovery(&p, true, bo3, 3, false);
  CHECK(r.kind == RecoveryPlan::Kind::None);
  // Bo1 finished.
  Progress bo1;
  bo1.phase = "map_over";
  bo1.series_team1 = 1;
  CHECK(PlanRecovery(&bo1, true, {"de_dust2"}, 1, true).kind == RecoveryPlan::Kind::None);
  // A map number outside the list.
  Progress bad;
  bad.map_number = 5;
  bad.phase = "live";
  CHECK(PlanRecovery(&bad, true, bo3, 3, true).kind == RecoveryPlan::Kind::None);
}

int main() {
  TestFileNames();
  TestList();
  TestRestoreCommand();
  TestBackupFlagHint();
  TestPauseAfterRestore();
  TestProgress();
  TestPlanRecovery();
  std::printf("match_restore: %d checks, %d failures\n", g_checks, g_failures);
  return g_failures ? 1 : 0;
}

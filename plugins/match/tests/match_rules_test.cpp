// Offline tests for readyup/match_rules.h: rule resolution, technical pause limits, the unpause
// rule, the auto-unpause countdown, the engine's auto 5v5 pause, the .forceready threshold, team
// name sanitizing and the team-left forfeit timer, warmup money, the warmup score reset and the warmup
// weapon cleanup.
// ctest `match_rules`.
#include "readyup/match_rules.h"
#include "readyup/warmup_money.h"
#include "readyup/warmup_score.h"
#include "readyup/weapon_cleanup.h"

#include <cstdio>
#include <fstream>
#include <sstream>
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

static void TestResolve() {
  const MatchRules d = BuiltinDefaultRules();
  CHECK(d.tech_pauses_per_team == 3 && d.tech_pause_max_seconds == 300 && d.both_teams_unpause == 1);
  CHECK(d.allow_force_ready == 1 && d.min_players_to_ready == 0 && d.forfeit_after_seconds == 240);

  // Nothing set anywhere: defaults.
  MatchRules r = ResolveRules(MatchRules{}, MatchRules{});
  CHECK(r.tech_pauses_per_team == 3 && r.forfeit_after_seconds == 240);

  // readyup.cfg (base) over defaults, the match over both; 0 is a value, not "unset".
  MatchRules cfg;
  cfg.tech_pauses_per_team = 5;
  cfg.forfeit_after_seconds = 0;
  cfg.both_teams_unpause = 0;
  MatchRules match;
  match.tech_pauses_per_team = 1;
  match.tech_pause_max_seconds = 0;
  r = ResolveRules(match, cfg);
  CHECK(r.tech_pauses_per_team == 1);
  CHECK(r.tech_pause_max_seconds == 0);
  CHECK(r.forfeit_after_seconds == 0);
  CHECK(r.both_teams_unpause == 0);
  CHECK(r.allow_force_ready == 1);
  // Booleans normalize to 0/1.
  match.allow_force_ready = 7;
  CHECK(ResolveRules(match, cfg).allow_force_ready == 1);
}

static void TestPauses() {
  // Limit 2: the third technical pause is refused.
  CHECK(TechPauseAllowed(0, 2));
  CHECK(TechPauseAllowed(1, 2));
  CHECK(!TechPauseAllowed(2, 2));
  CHECK(TechPausesLeft(1, 2) == 1);
  CHECK(TechPausesLeft(2, 2) == 0);
  CHECK(TechPausesLeft(5, 2) == 0);
  // 0 = unlimited.
  CHECK(TechPauseAllowed(100, 0));
  CHECK(TechPausesLeft(100, 0) == -1);

  // Auto-unpause countdown.
  CHECK(TechPauseSecondsLeft(0, 300) == 300);
  CHECK(TechPauseSecondsLeft(299, 300) == 1);
  CHECK(TechPauseSecondsLeft(300, 300) == 0);
  CHECK(TechPauseSecondsLeft(1000, 300) == 0);
  CHECK(TechPauseSecondsLeft(-5, 300) == 300);
  CHECK(TechPauseSecondsLeft(50, 0) == -1);  // no limit

  // Unpause: both teams, or the pausing team alone.
  CHECK(!UnpauseSatisfied(true, false, true, 1));
  CHECK(UnpauseSatisfied(true, true, true, 1));
  CHECK(UnpauseSatisfied(true, false, false, 1));
  CHECK(!UnpauseSatisfied(false, true, false, 1));  // the other team cannot end team1's pause
  CHECK(UnpauseSatisfied(false, true, false, 2));
  CHECK(UnpauseSatisfied(false, true, false, 0));  // no pausing team: either one
  CHECK(!UnpauseSatisfied(false, false, false, 0));
}

static void TestAuto5v5() {
  // Rule on, live, not paused: a side short of 5 players at a round start means the engine pauses.
  CHECK(!Auto5v5PauseExpected(true, true, false, 5, 5));
  CHECK(!Auto5v5PauseExpected(true, true, false, 6, 5));  // more than 5 is not "short"
  CHECK(Auto5v5PauseExpected(true, true, false, 4, 5));
  CHECK(Auto5v5PauseExpected(true, true, false, 5, 4));
  CHECK(Auto5v5PauseExpected(true, true, false, 0, 0));
  // Off, not live, or a pause already running: nothing to mark.
  CHECK(!Auto5v5PauseExpected(false, true, false, 4, 5));
  CHECK(!Auto5v5PauseExpected(true, false, false, 4, 5));
  CHECK(!Auto5v5PauseExpected(true, true, true, 4, 5));
  // The short side: the one with fewer players, CT on a tie; 0 when both are full.
  CHECK(Auto5v5ShortSide(5, 5) == 0);
  CHECK(Auto5v5ShortSide(4, 5) == 3);
  CHECK(Auto5v5ShortSide(5, 3) == 2);
  CHECK(Auto5v5ShortSide(4, 4) == 3);
  CHECK(Auto5v5ShortSide(2, 7) == 3);
  // Both teams resume it (it belongs to nobody): the pauser is ignored when both are required.
  CHECK(!UnpauseSatisfied(true, false, true, 1));
  CHECK(UnpauseSatisfied(true, true, true, 2));
}

static void TestForceReady() {
  // min 0 = the full roster.
  CHECK(ForceReadyRequired(5, 0) == 5);
  CHECK(!ForceReadyAllowed(4, 5, 0));
  CHECK(ForceReadyAllowed(5, 5, 0));
  // min 4 of 5.
  CHECK(ForceReadyRequired(5, 4) == 4);
  CHECK(ForceReadyAllowed(4, 5, 4));
  CHECK(!ForceReadyAllowed(3, 5, 4));
  // min above the roster: the roster.
  CHECK(ForceReadyRequired(2, 5) == 2);
  CHECK(ForceReadyAllowed(2, 2, 5));
  // Empty roster never qualifies.
  CHECK(!ForceReadyAllowed(0, 0, 0));
}

static void TestTeamNames() {
  CHECK(SanitizeTeamName("Team Alpha") == "Team Alpha");
  CHECK(SanitizeTeamName("  Bad\";quit\n ") == "Badquit");
  CHECK(SanitizeTeamName("") == "");
  CHECK(SanitizeTeamName("   ") == "");
  const std::string longName(40, 'x');
  CHECK(SanitizeTeamName(longName).size() == 32);
  // Cut on a UTF-8 boundary: 31 ASCII bytes + a 2-byte character must not be split.
  const std::string utf = std::string(31, 'a') + "\xC3\xA6" + "b";
  CHECK(SanitizeTeamName(utf) == std::string(31, 'a'));
}

static void TestForfeit() {
  using A = ForfeitTimer::Action;
  ForfeitTimer t;
  // Both present: nothing.
  auto r = t.Update(0, true, 240, true, true);
  CHECK(r.action == A::None && !t.Running());
  // Team2 leaves at t=10.
  r = t.Update(10, true, 240, true, false);
  CHECK(r.action == A::Started && r.absentTeam == 2 && r.secondsLeft == 240);
  CHECK(t.Running());
  r = t.Update(100.2, true, 240, true, false);
  CHECK(r.action == A::None && r.secondsLeft == 150);
  // Someone reconnects: cancelled.
  r = t.Update(120, true, 240, true, true);
  CHECK(r.action == A::Cancelled && r.absentTeam == 2 && !t.Running());
  // Leaves again: a fresh countdown.
  r = t.Update(130, true, 240, true, false);
  CHECK(r.action == A::Started && r.secondsLeft == 240);
  r = t.Update(369.9, true, 240, true, false);
  CHECK(r.action == A::None && r.secondsLeft == 1);
  r = t.Update(370, true, 240, true, false);
  CHECK(r.action == A::Expired && r.absentTeam == 2);
  // Fires once.
  r = t.Update(371, true, 240, true, false);
  CHECK(r.action == A::None && !t.Running());
  // The map ended (not live): reset without a "cancelled".
  r = t.Update(372, false, 240, true, false);
  CHECK(r.action == A::None);
  CHECK(!t.Running() && t.AbsentTeam() == 0);

  // Everyone gone (empty server) and rule off: no countdown.
  ForfeitTimer u;
  CHECK(u.Update(0, true, 240, false, false).action == A::None);
  CHECK(u.Update(0, true, 0, true, false).action == A::None);
  CHECK(u.Update(0, false, 240, true, false).action == A::None);
  // Team1 leaves, then the whole server empties: cancelled; team1 still gone later restarts.
  CHECK(u.Update(1, true, 60, false, true).action == A::Started);
  CHECK(u.Update(2, true, 60, false, false).action == A::Cancelled);
  r = u.Update(3, true, 60, false, true);
  CHECK(r.action == A::Started && r.absentTeam == 1);
  // The other team leaving instead restarts the countdown for it.
  r = u.Update(4, true, 60, true, false);
  CHECK(r.action == A::Started && r.absentTeam == 2 && r.secondsLeft == 60);
  // Leaving the live map while counting cancels it.
  CHECK(u.Update(5, false, 60, true, false).action == A::Cancelled);
}

// Whole cfg file (READYUP_CFG_DIR = cfg/ReadyUp), "" if missing.
static std::string ReadCfg(const char* name) {
  std::ifstream f(std::string(READYUP_CFG_DIR) + "/" + name);
  std::stringstream ss;
  ss << f.rdbuf();
  return ss.str();
}

// "cvar value" appears as a line of the cfg (columns may be padded with spaces).
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

static void TestWeaponCleanup() {
  // Sweep: idle, scrim warmup, match warmup; not once go-live is pending, not when off.
  CHECK(WeaponCleanupActive("idle", false, true) && WeaponCleanupActive("scrim_warmup", false, true));
  CHECK(WeaponCleanupActive("match_warmup", false, true));
  CHECK(!WeaponCleanupActive("match_warmup", true, true) && !WeaponCleanupActive("scrim_warmup", false, false));
  CHECK(!WeaponCleanupActive("match_live", false, true) && !WeaponCleanupActive("match_knife", false, true));
  CHECK(!WeaponCleanupActive("practice", false, true) && !WeaponCleanupActive("postgame", false, true));
  CHECK(!WeaponCleanupActive(nullptr, false, true));

  // Classes: every weapon, the C4 too (no bomb in warmup); no projectiles, players or items.
  CHECK(WeaponCleanupClass("weapon_ak47") && WeaponCleanupClass("weapon_hegrenade") && WeaponCleanupClass("weapon_knife"));
  CHECK(WeaponCleanupClass("weapon_taser") && WeaponCleanupClass("weapon_m4a1_silencer"));
  CHECK(WeaponCleanupClass("weapon_c4") && !WeaponCleanupClass("hegrenade_projectile") && !WeaponCleanupClass("planted_c4"));
  CHECK(!WeaponCleanupClass("cs_player_pawn") && !WeaponCleanupClass("weaponworldmodel") && !WeaponCleanupClass(nullptr));

  // Grace: removed only after kWeaponCleanupGraceSeconds unowned; a pickup starts it over.
  DroppedWeaponTracker t;
  const double g = kWeaponCleanupGraceSeconds;
  CHECK(!t.Unowned(100, 10.0));            // first seen on the ground
  CHECK(!t.Unowned(100, 10.0 + g - 0.1));  // not yet
  CHECK(t.Unowned(100, 10.0 + g));         // on the ground long enough
  CHECK(!t.Unowned(200, 11.0));
  t.Owned(200);                            // picked up
  CHECK(!t.Unowned(200, 11.0 + g));        // dropped again: grace starts over
  CHECK(t.Unowned(200, 11.0 + 2 * g));
  t.Forget(100);
  CHECK(!t.Unowned(100, 20.0));  // removed; a new entity with that handle starts over
  // Prune: handles not seen for a while (the entity is gone) are forgotten.
  CHECK(!t.Unowned(300, 30.0));
  t.Prune(30.0 + 2 * g + 1);
  CHECK(t.Size() == 0);
  CHECK(!t.Unowned(300, 40.0));

  // The warmup cvars stop death drops; the go-live list puts every one of them back.
  for (const char* w : kWarmupNoDropCmds) {
    const std::string cvar = std::string(w).substr(0, std::string(w).find(' '));
    bool restored = false;
    for (const char* l : kLiveDropCmds) restored = restored || std::string(l).rfind(cvar + " ", 0) == 0;
    CHECK(restored);
  }
  // No bomb in warmup / idle (idle.cfg repeats it for an older warmup.cfg), practice keeps it.
  CHECK(CfgSets(ReadCfg("idle.cfg"), "mp_give_player_c4 0") && CfgSets(ReadCfg("knife.cfg"), "mp_give_player_c4 0"));
  CHECK(CfgSets(ReadCfg("prac.cfg"), "mp_give_player_c4 \"1\""));
  // And the shipped cfgs agree: warmup.cfg sets the warmup values, live.cfg and esports_live.cfg
  // the go-live ones.
  const std::string warmup = ReadCfg("warmup.cfg"), live = ReadCfg("live.cfg"), esports = ReadCfg("esports_live.cfg");
  CHECK(!warmup.empty() && !live.empty() && !esports.empty());
  for (const char* w : kWarmupNoDropCmds) {
    if (!CfgSets(warmup, w)) std::fprintf(stderr, "warmup.cfg lacks `%s`\n", w);
    CHECK(CfgSets(warmup, w));
  }
  for (const char* l : kLiveDropCmds) {
    if (!CfgSets(live, l)) std::fprintf(stderr, "live.cfg lacks `%s`\n", l);
    if (!CfgSets(esports, l)) std::fprintf(stderr, "esports_live.cfg lacks `%s`\n", l);
    CHECK(CfgSets(live, l) && CfgSets(esports, l));
  }
}

static void TestSpectators() {
  CHECK(BuiltinDefaultRules().min_spectators_to_ready == 0);
  MatchRules m;
  m.min_spectators_to_ready = 2;
  CHECK(ResolveRules(m, MatchRules{}).min_spectators_to_ready == 2);
  CHECK(ResolveRules(MatchRules{}, MatchRules{}).min_spectators_to_ready == 0);
  CHECK(SpectatorsReadyToGoLive(0, 0));   // default: never holds the match up
  CHECK(!SpectatorsReadyToGoLive(2, 1));
  CHECK(SpectatorsReadyToGoLive(2, 2) && SpectatorsReadyToGoLive(2, 3));
  CHECK(SpecCommandAllowed(false) && !SpecCommandAllowed(true));  // .spec
}

int main() {
  TestResolve();
  TestSpectators();
  TestPauses();
  TestAuto5v5();
  TestForceReady();
  TestTeamNames();
  TestForfeit();
  // Warmup money top-up: scrim and match warmup only, and only when enabled.
  CHECK(WarmupMoneyActive("scrim_warmup", true) && WarmupMoneyActive("match_warmup", true));
  CHECK(!WarmupMoneyActive("scrim_warmup", false) && !WarmupMoneyActive("match_live", true));
  CHECK(!WarmupMoneyActive("practice", true) && !WarmupMoneyActive("idle", true) && !WarmupMoneyActive(nullptr, true));
  // Warmup score reset: scrim and match warmup, not the recovery gate of a restored match, not
  // once go-live is pending, not before the warmup's map has loaded.
  CHECK(WarmupScoreResetActive("scrim_warmup", false, false, true) && WarmupScoreResetActive("match_warmup", false, false, true));
  CHECK(!WarmupScoreResetActive("match_warmup", true, false, true) && !WarmupScoreResetActive("match_warmup", false, true, true));
  CHECK(!WarmupScoreResetActive("match_warmup", false, false, false) && !WarmupScoreResetActive("scrim_warmup", false, false, false));
  CHECK(!WarmupScoreResetActive("scrim_warmup", false, true, true) && !WarmupScoreResetActive("match_live", false, false, true));
  CHECK(!WarmupScoreResetActive("match_knife", false, false, true) && !WarmupScoreResetActive("practice", false, false, true));
  CHECK(!WarmupScoreResetActive("idle", false, false, true) && !WarmupScoreResetActive(nullptr, false, false, true));
  TestWeaponCleanup();
  std::printf("match_rules: %d checks, %d failures\n", g_checks, g_failures);
  return g_failures == 0 ? 0 : 1;
}

#pragma once

// Pause / ready / forfeit rules of a match and the pure logic behind them (no engine calls; unit
// tested by ctest `match_rules`). The engine side lives in match_features.h.
//
// Where a value comes from (first one set wins):
//   1. the match config: MAT JSON top-level keys `max_tech_pauses_per_team`, `tech_pause_max_seconds`,
//      `both_teams_unpause_required`, `allow_force_ready`, `min_players_to_ready`,
//      `min_spectators_to_ready`, `forfeit_after_seconds`; a fleet `match.assign` maps `rules.pause.technical_per_team`,
//      `rules.pause.technical_seconds`, `rules.pause.unpause`, `rules.ready.allow_force_ready`,
//      `rules.ready.min_per_team` and `rules.forfeit.team_absent_seconds` onto them
//      (fleet_state.cpp AssignToMatConfig),
//   2. readyup.cfg / match.cfg keys of the same names (scrims only have these),
//   3. the built-in defaults (BuiltinDefaultRules).
//
// Tactical timeouts (`.tac`) are CS2's own (timeout_ct_start / timeout_terrorist_start): their
// count and length are mp_team_timeout_max / mp_team_timeout_time (live.cfg, match cvars).

#include <string>

namespace readyup {

struct MatchRules {
  // -1 = not set (take the next source).
  int tech_pauses_per_team = -1;   // technical pauses per team per map; 0 = unlimited
  int tech_pause_max_seconds = -1; // auto-unpause after this long; 0 = no limit
  int both_teams_unpause = -1;     // 1: both teams type .unpause; 0: the pausing team alone
  int allow_force_ready = -1;      // `.forceready`
  int min_players_to_ready = -1;   // connected players a team needs for .forceready; 0 = full roster
  int min_spectators_to_ready = -1; // spectators (casters, match `spectators`) that must be ready before go-live; 0 = none
  int forfeit_after_seconds = -1;  // whole team disconnected while live -> forfeit; 0 = off
  // `.gg` surrender vote and `.stop` round-restore vote (votes.h). MAT keys gg_enabled,
  // gg_threshold (0.8 or 80), gg_min_score_diff, stop_command_available, stop_command_no_damage,
  // stop_vote_seconds; fleet rules.forfeit.gg_vote.{enabled,threshold,min_score_diff}.
  int gg_enabled = -1;              // .gg is a team vote that forfeits the map (else it only emits player_gg)
  int gg_threshold_pct = -1;        // share of the team's connected players that must vote, percent
  int gg_min_score_diff = -1;       // the team must trail by at least this many rounds
  int stop_command_available = -1;  // .stop: both teams restore the start of the current round
  int stop_command_no_damage = -1;  // .stop only while no player damaged an opponent this round
  int stop_vote_seconds = -1;       // the other team has this long to confirm a .stop
  // After a round restore (round_restore.h): stay paused (1) or go live 3 s later (0). MAT key
  // pause_after_restore; fleet rules.pause.pause_after_restore; between the match config and
  // readyup.cfg sits the console setting ru_pause_after_restore.
  int pause_after_restore = -1;
};

// gg_threshold as written in a config: a fraction (0.8) or a percent (80). -1 if invalid.
int GgThresholdPctFromText(const std::string& text);

MatchRules BuiltinDefaultRules();
// Every -1 in `match` is taken from `base`, then from BuiltinDefaultRules(). The result has no -1.
MatchRules ResolveRules(const MatchRules& match, const MatchRules& base);

// ---- pauses ----------------------------------------------------------------------------------

// A team with `used` technical pauses this map may pause again under `limit` (0 = unlimited).
bool TechPauseAllowed(int used, int limit);
// Technical pauses left (-1 = unlimited).
int TechPausesLeft(int used, int limit);
// Seconds until the automatic unpause (`elapsed` since the pause took effect), -1 = no limit.
int TechPauseSecondsLeft(int elapsedSeconds, int maxSeconds);

// team: 1 / 2 (0 = unknown). pauserTeam: the team that paused (0 = admin / console / unknown).
// Both required: both teams confirmed. Otherwise the pausing team alone is enough (any team when
// the pauser is not a team).
bool UnpauseSatisfied(bool team1Confirmed, bool team2Confirmed, bool bothRequired, int pauserTeam);

// sv_matchpause_auto_5v5: the engine pauses the match at freeze time when a side is short of
// kFullTeamPlayers. At a round start of a live map, with the rule on and no pause running, Ready Up
// marks that pause as type "auto_5v5". ct / t: players (humans + bots) on each side.
constexpr int kFullTeamPlayers = 5;
bool Auto5v5PauseExpected(bool ruleOn, bool liveMap, bool alreadyPaused, int ctPlayers, int tPlayers);
// The short side (3 CT / 2 T; the one with fewer players, CT on a tie), 0 when both are full.
int Auto5v5ShortSide(int ctPlayers, int tPlayers);

// ---- .forceready -----------------------------------------------------------------------------

// connected: roster players of the team on the server; rosterSize: the team's roster (substitutes
// included); minPlayers: rule `min_players_to_ready` / `ru_minimum_ready_required` (0 = a full
// team); playersPerTeam: the match's `players_per_team` (0 = kFullTeamPlayers). A full team is
// playersPerTeam capped at the roster, so a roster with substitutes (7 players, 5 per team) needs 5,
// not 7. Empty rosters never qualify.
int FullTeamSize(int rosterSize, int playersPerTeam = 0);
bool ForceReadyAllowed(int connected, int rosterSize, int minPlayers, int playersPerTeam = 0);
int ForceReadyRequired(int rosterSize, int minPlayers, int playersPerTeam = 0);

// The minPlayers the ready gate and .forceready use: the rule, or 0 (a full team) after a failover
// or a crash recovery, so every roster player is back before the match goes live again.
int ReadyMinPlayers(int minPlayersRule, bool fullRoster);

// ---- go-live ready gate ----------------------------------------------------------------------

// One team may go live when `ready` (its connected roster players who typed .ready) reaches
// max(ForceReadyRequired, min(connected, full team)): the minimum is always met, and every player
// needed for a full team who is connected has to be ready, but a connected substitute beyond the
// full team does not hold the match up. An empty roster is always ready.
bool TeamReadyToGoLive(int rosterSize, int connected, int ready, int minPlayers, int playersPerTeam = 0);
int TeamReadyNeeded(int rosterSize, int connected, int minPlayers, int playersPerTeam = 0);

// The "x/y ready" count players and the platform see, for one team: ready roster players up to a
// full team over the full team (FullTeamSize). Substitutes never raise the total (MatchZy Enhanced:
// only players_per_team per team take a slot and ready up), and a ready substitute standing in for a
// missing starter still counts, so a 5-player team with 1 substitute reads 5/5, not 5/6.
struct ReadyTally {
  int ready = 0;
  int total = 0;
};
ReadyTally TeamReadyTally(int rosterSize, int ready, int playersPerTeam = 0);

// Spectators of the match config (casters) who typed .ready: `required` (min_spectators_to_ready)
// of them are needed before go-live. 0 (the default) never holds the match up.
bool SpectatorsReadyToGoLive(int required, int readySpectators);

// `.spec`: a player moves themselves to spectator. Refused for a player on the roster of a loaded
// (non-scrim) match: the roster enforcement (modes.cpp MaybeForceRosterTeamsLocked) would put them
// straight back. Everyone else (idle, scrim warmup / live, non-roster players) may.
bool SpecCommandAllowed(bool onLoadedMatchRoster);

// ---- playout ---------------------------------------------------------------------------------

// Playout (`ru_playout_enabled_default`, match `playout`): every regulation round is played (no
// clinch at maxRounds/2 + 1) and an overtime is only decided at the end of its block. True while
// the map may not be decided yet at this score. Past the overtime cap (maxOvertimes >= 0) the
// rounds are sudden death and playout no longer holds anything back.
bool PlayoutRoundsLeft(int maxRounds, bool overtimeEnabled, int overtimeSegments, int maxOvertimes, int team1Score,
                       int team2Score);

// ---- team names ------------------------------------------------------------------------------

// Value for `mp_teamname_N "<value>"`: control bytes, quotes and ';' dropped, trimmed, at most
// 32 bytes (on a UTF-8 boundary).
std::string SanitizeTeamName(const std::string& in);

// ---- forfeit when a team leaves --------------------------------------------------------------

// One team with nobody connected for `limitSeconds` loses by forfeit. Fed once per tick.
class ForfeitTimer {
 public:
  enum class Action { None, Started, Cancelled, Expired };
  struct Result {
    Action action = Action::None;
    int absentTeam = 0;   // 1 / 2 while counting (and with Expired)
    int secondsLeft = 0;  // while counting
  };

  // now: seconds (monotonic). active: the map is live and the rule is on (limitSeconds > 0).
  // Both teams gone (empty server) or both present: no countdown.
  Result Update(double now, bool active, int limitSeconds, bool team1Present, bool team2Present);
  void Reset();

  bool Running() const { return absent_ != 0 && !fired_; }
  int AbsentTeam() const { return absent_; }
  int SecondsLeft(double now) const;

 private:
  int absent_ = 0;
  double deadline_ = 0;
  bool fired_ = false;
};

}  // namespace readyup

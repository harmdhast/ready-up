#pragma once

// Pause / ready / forfeit rules of a match and the pure logic behind them (no engine calls; unit
// tested by ctest `match_rules`). The engine side lives in match_features.h.
//
// Where a value comes from (first one set wins):
//   1. the match config: MAT JSON top-level keys `max_tech_pauses_per_team`, `tech_pause_max_seconds`,
//      `both_teams_unpause_required`, `allow_force_ready`, `team_ready`, `min_players_to_ready`,
//      `min_spectators_to_ready`, `forfeit_after_seconds`; a fleet `match.assign` maps `rules.pause.technical_per_team`,
//      `rules.pause.technical_seconds`, `rules.pause.unpause`, `rules.ready.allow_force_ready`, `rules.ready.team_ready`,
//      `rules.ready.min_per_team` and `rules.forfeit.team_absent_seconds` onto them
//      (fleet_state.cpp AssignToMatConfig),
//   2. readyup.cfg / match.cfg keys of the same names (scrims only have these),
//   3. the built-in defaults (BuiltinDefaultRules).
//
// Tactical timeouts (`.tac`) are CS2's own (timeout_ct_start / timeout_terrorist_start): their
// count and length are mp_team_timeout_max / mp_team_timeout_time (live.cfg, match cvars).

#include <string>
#include <vector>

namespace readyup {

struct MatchRules {
  // -1 = not set (take the next source).
  int tech_pauses_per_team = -1;   // technical pauses per team per map; 0 = unlimited
  int tech_pause_max_seconds = -1; // auto-unpause after this long; 0 = no limit
  // auto_5v5 (a side short: sv_matchpause_auto_5v5): unpaused once both sides are full again,
  // or after this long; 0 = no limit (both teams .unpause, or an admin).
  int disconnect_pause_max_seconds = -1;
  int both_teams_unpause = -1;     // 1: both teams type .unpause; 0: the pausing team alone
  int allow_force_ready = -1;      // `.forceready`
  // `.ready` by a rostered player readies their whole team (and `.unready` un-readies it), once
  // the team has the players .forceready needs. MAT key team_ready; fleet rules.ready.team_ready.
  int team_ready = -1;
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
// Chat when a rostered player leaves a live map: whether the match will pause for them
// (auto 5v5 pause on) and, with a limit, for how long. `maxSeconds` <= 0 = no limit.
// Chat when a map goes live: what is played and how long (2026-10-15, a live test: "so
// you see how long you will play for"). `maxOvertimes` -1 = as many as needed.
// One chat message per line: players scroll chat (Y), a long line wraps badly.
std::vector<std::string> GoLiveRulesLines(int mapNumber, int numMaps, const std::string& map, int maxRounds,
                                          bool overtime, int overtimeHalfRounds, int maxOvertimes,
                                          bool damageTiebreak);
std::string PlayerLeftChatLine(const std::string& name, const std::string& team, bool autoPause, int maxSeconds);
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
// What the freeze-time check does (match_features.cpp), polled from the round start until the
// freeze time ends. enginePaused: CS2's own pause state (match_status.h EngineMatchPaused: 1 / 0 /
// -1 unknown). The pause is only marked when the engine really paused: a stale team count marked
// one at every round start and reported "paused" to the platform (2026-10-17, server-2). Paused
// with both sides full is someone else's pause (Done). Unknown engine state: the counts decide,
// as before.
enum class Auto5v5Check { Done, Wait, Mark };
Auto5v5Check Auto5v5Decide(bool ruleOn, bool liveMap, bool alreadyPaused, int enginePaused, int ctPlayers,
                           int tPlayers);
// The short side (3 CT / 2 T; the one with fewer players, CT on a tie), 0 when both are full.
int Auto5v5ShortSide(int ctPlayers, int tPlayers);
// An auto_5v5 pause ends by itself (pure): "full" once both sides have 5 again (held for
// kAuto5v5FullHoldSeconds, so a player flickering in and out does not start the round),
// "timeout" after maxSeconds (> 0) of the pause, else "" (it waits).
constexpr double kAuto5v5FullHoldSeconds = 5.0;
std::string Auto5v5UnpauseReason(int ctPlayers, int tPlayers, double fullForSeconds, double pausedSeconds,
                                 int maxSeconds);

// An engine pause Ready Up did not start (match_features.cpp tick, knife round or live map).
// CS2 pauses at map load with sv_matchpause_auto_5v5 1 and nobody on a team yet; the pause
// survives warmup and the go-live restart, and nothing in Ready Up knew about it, so `.up` said
// "not paused" and the rounds never started (2026-10-17, a Bo3 final: an admin ran
// mp_unpause_match after 3.5 minutes).
//   enginePaused        CS2's pause state (1 / 0 / -1 unknown, match_status.h EngineMatchPaused)
//   readyUpPause        Ready Up has a pause recorded (pause_state.h)
//   playing             knife round or live map
//   goLiveWindow        just after Ready Up started the knife round / went live / resumed
//   autoRuleOn          sv_matchpause_auto_5v5 is on for the match
//   ct, t, fullTeam     people per side and a full side (players_per_team, 5 by default)
//   unmarkedForSeconds  how long the engine has been paused with no Ready Up pause
// At go-live with both sides full the pause is stale: Clear (mp_unpause_match). A side short:
// the engine pause is real, it is recorded (auto_5v5 with the rule on, else engine). Outside the
// window an unmarked engine pause held for kEnginePauseMarkHoldSeconds is recorded too, so the
// platform shows it and both teams `.unpause` it.
enum class EnginePauseAction { None, Clear, MarkAuto5v5, MarkEngine };
constexpr double kEnginePauseMarkHoldSeconds = 3.0;
EnginePauseAction DecideEnginePause(int enginePaused, bool readyUpPause, bool playing, bool goLiveWindow,
                                    bool autoRuleOn, int ct, int t, int fullTeam, double unmarkedForSeconds);

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

// Rule `team_ready`: what a rostered player's .ready / .unready in match warmup does.
//   Single:     only the player (rule off).
//   Team:       the whole team (connected roster players).
//   NotEnough:  .ready with the rule on, but the team lacks the players .forceready needs: say so
//               (ForceReadyRequired) and ready only the player.
// .unready with the rule on is always Team.
enum class TeamReadyAction { Single, Team, NotEnough };
TeamReadyAction DecideTeamReady(bool teamReadyRule, bool ready, int connected, int rosterSize, int minPlayers,
                                int playersPerTeam = 0);

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

// Rule `team_ready`, the warmup HUD's per-team line. here: connected roster players (capped at the
// full team, so substitutes never inflate it); total: the full team (FullTeamSize); ready: the team
// is ready when every full-team slot is filled by a ready connected player.
struct TeamHudState {
  int here = 0;
  int total = 0;
  bool allHere = false;
  bool ready = false;
};
TeamHudState TeamHudStatus(int rosterSize, int connected, int readyConnected, int playersPerTeam = 0);

// Bot fill (match config bot_fill): bots take every empty seat and are always ready, so a team
// waits only for the roster humans who are connected; a team with nobody connected is ready (its
// bots play). The "x/y ready" count is those humans: ready connected over connected.
bool BotFillTeamReady(int connected, int readyConnected);
ReadyTally BotFillReadyTally(int connected, int readyConnected);
// Bot fill with no human connected: the match waits this long for anyone to join before its bots
// play it alone (a bots-only bracket match), never going live the moment it loads.
constexpr double kBotFillEmptyGraceSeconds = 60.0;
// Bot fill: how long a match with players on its roster waits for one of them before its bots play.
constexpr double kBotFillRosterGraceSeconds = 300.0;
bool BotFillEmptyMayGoLive(double secondsEmpty);

// A failover resume restores a round, so it waits for every roster player to be back; then the
// normal ready-up applies (everyone .r), the match goes live and the backup round is restored.
bool ResumeMayGoLive(int rosterTotal, int presentTotal);
// "Every roster player is back" for ResumeMayGoLive, per team: a full team (FullTeamSize), so a
// substitute who was not playing does not hold the resume up for the rejoin window.
// needed / back: the totals to pass as ResumeMayGoLive's rosterTotal / presentTotal.
struct ResumeHeadcount {
  int needed = 0;
  int back = 0;
};
ResumeHeadcount ResumeCount(const int roster[2], const int present[2], int playersPerTeam = 0);

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

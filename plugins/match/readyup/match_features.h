#pragma once

// Match features on top of the ready / live flow (docs/PARITY.md §7 P1), engine side. The rules
// and the pure logic are in match_rules.h.
//
// Pauses (chat, roster players, live map only):
//   .tac               tactical timeout: CS2's own timeout_ct_start / timeout_terrorist_start for
//                      the caller's side (count and length: mp_team_timeout_max / _time). Ready Up
//                      shows it as a "tactical" pause and clears it at the next round_freeze_end.
//   .pause .p .tech    technical pause: mp_pause_match (takes effect at freeze time), at most
//                      max_tech_pauses_per_team per team per map; after tech_pause_max_seconds of
//                      pause the match unpauses by itself (countdown in the center HUD).
//   .unpause .up       both teams (both_teams_unpause_required=1) or the pausing team alone.
//                      An admin pause only ends with .forceunpause.
// Engine pauses Ready Up marks: "halftime" (mp_halftime_pausematch, esports.h) and "auto_5v5"
// (sv_matchpause_auto_5v5: a side short of 5 players at a round start). Both teams .unpause them,
// or an admin; neither is counted against a team. An engine pause Ready Up did not start (the
// map-load auto_5v5 pause, an admin's console mp_pause_match): at go-live (knife round, live,
// resume) with both sides full it is stale and cleared (mp_unpause_match); otherwise it is
// recorded ("auto_5v5" with a side short, else "engine"; the platform sees "engine" as technical)
// and both teams .unpause it (match_rules.h DecideEnginePause).
//   .forcepause .fp / .forceunpause .fup   admins (same as `.ru match pause` / `.ru match unpause`).
// .forceready          (allow_force_ready) readies the caller's whole team in warmup once it has
//                      min_players_to_ready connected (0 = the full roster).
// Forfeit              a team with nobody connected for forfeit_after_seconds while a map is live
//                      forfeits the map and the series (modes.h ForfeitCurrentMap). Countdown in
//                      chat and the HUD; cancelled when someone reconnects.
//
// Admin / console, on behalf of a team: `ru match tech team1|team2`, `ru match tac team1|team2` (same limits).
// Log lines (scripts/livetest): `pause: ...`, `forfeit: ...`.

#include "readyup/match_rules.h"
#include "readyup/webhook.h"

#include <cstdint>
#include <string>
#include <vector>

namespace readyup {

// The match's rules resolved against readyup.cfg and the built-in defaults. Any thread.
MatchRules EffectiveRules();

// Engine game events (synchronous, game thread): round_start, round_freeze_end, round_end,
// round_announce_warmup.
void MatchFeaturesOnGameEvent(const char* name);
// Between round_end and the next round_start (the round-over delay). A CS2 round backup loaded
// then never starts the next round (the game stays in RoundOver), so restores wait (fleet_bridge
// DoRestore). Any thread.
bool MatchFeaturesRoundOver();
// An admin ends an auto_5v5 pause: CS2 would pause again at once while a side is short, so the
// engine's auto pause goes off for the rest of the map (the admin chose to play on short). Call
// before mp_unpause_match. Game thread / any thread (queues commands).
void MatchFeaturesAdminUnpausing();
// Between round_start and round_freeze_end (freeze time). Any thread.
bool MatchFeaturesInFreeze();

// Game thread, every frame: tactical timeout end, technical pause auto-unpause, forfeit timer.
void MatchFeaturesTick();
// A player left / came back (core events): one chat line for a rostered player on a live map.
void MatchFeaturesOnPlayerLeft(uint64_t steamid64, const std::string& name);
void MatchFeaturesOnPlayerBack(uint64_t steamid64, const std::string& name);

// Pause commands for `team` (steamid64 = the player, 0 = console / admin on behalf of the team).
// Reply goes to chat (players) or the console. Game thread.
void MatchFeaturesTechPause(WebhookTeam team, uint64_t steamid64, const std::string& name);
void MatchFeaturesTacticalTimeout(WebhookTeam team, uint64_t steamid64, const std::string& name);
void MatchFeaturesUnpause(WebhookTeam team, uint64_t steamid64, const std::string& name);
// `.forceready` from a roster player (match) or a player on CT/T (scrim warmup: ctx empty).
void MatchFeaturesForceReady(uint64_t steamid64, const std::string& name);
// Rule team_ready (match_rules.h DecideTeamReady), match warmup only: a rostered player's .ready /
// .unready for their whole team. True = done here (chat line sent; *changed = the players whose
// state changed). False = not this rule's case: the caller readies only the player (after
// "needs N players connected" when the team is short).
bool MatchFeaturesTeamReady(uint64_t steamid64, const std::string& name, bool ready, std::vector<uint64_t>* changed);

// `.switch` (admin, no knife pick pending) / `ru match swap` / fleet cmd swap_teams: mp_swapteams in
// match warmup, and the map's side follows (team1_ct <-> team2_ct) so team1 / team2 stay who they
// are. False + *why outside warmup or without a match. Game thread.
bool MatchFeaturesSwapTeams(std::string* why);
// `.team1 <name>` / `ru match team1 <name>`: renames a team of a match loaded with `ru match load`
// (fleet matches are renamed by the platform, match.update rename_team); the in-game names follow.
bool MatchFeaturesRenameTeam(int team, const std::string& name, std::string* why);

// Center HUD while a map is live (ready_hud.cpp).
struct LiveHudInfo {
  bool paused = false;
  std::string type;            // "tactical" | "technical" | "admin" | "offline" | "halftime" | "auto_5v5" | "engine"
  std::string byTeam;          // pausing team's name (empty for admins)
  bool pendingFreeze = false;  // technical: waiting for freeze time
  int secondsLeft = -1;        // technical auto-unpause countdown (-1 = none)
  bool team1Confirmed = false, team2Confirmed = false;
  bool bothRequired = true;
  std::string team1, team2;  // team names
  bool forfeit = false;
  std::string forfeitTeam;  // the absent team
  int forfeitSecondsLeft = 0;
};
LiveHudInfo MatchFeaturesHud();

}  // namespace readyup

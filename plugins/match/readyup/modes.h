#pragma once

#include "readyup/status_snapshot.h"
#include "readyup/webhook.h"

#include <cstdint>
#include <string>
#include <utility>

namespace readyup {

// High-level Ready Up mode/state (independent of CS2 built-in warmup).
enum class ReadyUpMode {
  Idle = 0,
  Practice,
  MatchWarmup,
  MatchKnife,
  MatchLive,
  // Map decided: between the final round end and the next map's changelevel (series
  // continues) or the unload after the series-end kick delay (series over). Matches
  // and scrims. See match_end.h.
  Postgame,
  // No match loaded, humans on CT/T: CS2 warmup (paused timer) + ready-up.
  // When every human on CT/T is ready (and both sides are populated), the
  // scrim flow (scrim_flow.cpp) creates a scrim match context and goes live.
  ScrimWarmup,
  // Another plugin runs the server (readyup.match.v1 set_external_mode, e.g. the deathmatch
  // plugin): no scrim warmup, ready HUD, idle cfg, practice rules or round-termination
  // suppression, and CS2's own warmup is left alone. Left for idle by the plugin or `.ru mode
  // idle`; a match load takes over (OnMatchLoaded).
  External,
};

// Mode + ready-state controls (best-effort; in-memory).
ReadyUpMode GetMode();
const char* GetModeString();

void SetModeIdle();
void SetModePractice();
// External mode owned by the plugin mode `name` ([a-z0-9_], e.g. "deathmatch"). Only from idle,
// scrim warmup or External itself (false otherwise: a match is loaded, practice is on).
bool SetModeExternal(const std::string& name);
// The name given to SetModeExternal ("" when the mode is not External).
std::string ExternalModeName();
// Enter scrim warmup (only from Idle / no match loaded). Returns false if the
// current mode doesn't allow it. Warmup rules are applied from Tick().
bool SetModeScrimWarmup();
// Boot-time match recovery: enter match_warmup for an already-set match
// context without the OnMatchLoaded() resets (the boot default is idle now).
void SetModeMatchWarmupForRecovery();

// Scrim go-live (called right after the scrim match context was created and
// OnMatchLoaded() ran): exec ReadyUp/live.cfg, mp_warmup_end, mp_restartgame N.
// Mode stays match_warmup (start triggered) until the next Round_Start log
// line / event flips it to match_live via OnMatchRoundStarted().
bool ScrimGoLive(int restartSeconds);

// True once Ready Up issued the go-live restart and is waiting for Round_Start.
bool GoLiveTriggered();

// Recovery gate: used when the server rebooted mid-match and RU restored state.
// When enabled, RU will NOT apply warmup cvars or restart the match. Instead it
// stays paused until all roster players are connected + ready, then unpauses.
void SetRecoveryGate(bool enabled);
bool RecoveryGateEnabled();

// Scrim / match warmup (not the recovery gate, no go-live pending): the warmup rules go out again
// on the next Tick() (warmup_score.h: a round was scored, so they were lost).
void ReapplyWarmupRules();

// The map of this warmup is loaded (Tick saw the map change, or a round started in match warmup:
// also a reload of the same map). ModesAwaitMapLoad: a map load is on its way (match load); until
// then the warmup is still on the last map, with its score.
bool WarmupMapReady();
void ModesAwaitMapLoad();

// Full-roster gate: a match resumed after a failover (FLEET.md §11.3). Until it goes live, every
// roster player must be connected and ready: min_players_to_ready and autoready do not apply.
// The recovery gate implies it. A match load, the next map, a reset and idle clear it.
void SetFullRosterGate(bool enabled);
bool FullRosterGateEnabled();

// Called when a match config is loaded successfully (seeds roster + enters warmup).
void OnMatchLoaded();

// Maps already won in the series (a fleet failover resuming map N, FLEET.md §11.3): call after
// OnMatchLoaded, which starts the series at 0-0.
void ModesSetSeriesWins(int team1, int team2);
// Maps won so far in the series (team1, team2).
std::pair<int, int> ModesGetSeriesWins();

// Called when match becomes live (e.g. first observed round start).
void OnMatchRoundStarted();

// Called after a round_end score update is known (engine netvars or log-derived).
// This lets Ready Up detect map completion and perform end-of-map actions.
void OnMatchRoundEnded(int map_number, int team1_score, int team2_score, const std::string& map_name);

// The engine logged `Game Over: ... score <CT>:<T>` (match_log.cpp). Safety net: if 2 s later
// the map is still undecided in Ready Up (its own round tally can drift after restores, or its
// round limit differs from the server's), the map ends here with the engine's score.
void ModesOnEngineGameOver(int ctScore, int tScore);

// The engine logged `Started map` for the map it is already on (match_log.cpp; a new map name goes
// through the map change): CS2 ran the gamemode cfg again, so the next Tick re-applies the cfg
// baselines and the warmup rules over it.
void ModesOnMapLoaded();

// A human joined CT or T (engine player_team, or a team-lock move). In match warmup the round is one
// long round, so a player who joins mid-round stays dead until it ends: 0.5 s later the joiner is
// respawned alone (ru_api 1.14 player_respawn). Without it, or when it is refused, the warmup round
// is restarted so everyone spawns (debounced). Only in the match warmup itself: nothing once going
// live was triggered or in a recovered live map's gate. Game thread.
void ModesOnHumanJoinedTeam(int slot, uint64_t steamid, int team);

// Map/series end (match_end.cpp, game thread, outside the modes mutex):
// - ModesBeginNextMapWarmup: postgame -> match warmup for the next map of the series (not
//   WarmupMapReady until it loads).
// - ModesFinishSeriesResetToIdle: unload the match (context, persisted state, stats) and go idle.
void ModesBeginNextMapWarmup();
void ModesFinishSeriesResetToIdle();

// Forfeit (match_features.cpp: a whole team left for forfeit_after_seconds): `loser` forfeits the
// live map and the series; map_result + series_end go out through the normal map-end flow with the
// other team as winner, plus `match_forfeit` (webhook) / `forfeit` (fleet) with `reason`.
// False unless a map is live. Game thread.
bool ForfeitCurrentMap(WebhookTeam loser, const char* reason);

// Knife decider (match config `map_sides: "knife"`, or scrims with
// readyup.cfg `scrim_knife=1`). Log-driven; engine events are an optional
// second source (deduped by phase):
//
//   match_warmup --(everyone ready)--> match_knife/starting
//       (exec knife.cfg + overrides, mp_logdetail 3, mp_restartgame 1)
//   starting --(Round_Start >= 0.8s after the restart)--> running
//   running --(SFUI_Notice_* round end)--> pick
//       elimination notice (CTs_Win / Terrorists_Win): that side won;
//       anything else (time / draw): more alive, then more HP, then random.
//   pick --(.stay/.switch/.ct/.t by a winning-team player or admin, or
//           timeout -> stay)--> [mp_swapteams] live.cfg + mp_restartgame 1
//       -> match_warmup (go-live pending) --(Round_Start)--> match_live
enum class KnifePhase { None = 0, Starting, Running, Picking };

// Starts the knife round now (from match_warmup with a match context loaded).
bool StartKnifeRound();
KnifePhase GetKnifePhase();
// "starting" | "running" | "pick" | nullptr (no knife round in progress).
const char* KnifePhaseString();

// source: "log" | "event" (diagnostics only).
void KnifeOnRoundStart(int map_number, const char* source = "event");
// elimination: the notice/reason was a team elimination (CTs_Win/Terrorists_Win);
// otherwise the winner is decided by alive count / HP / random.
void KnifeOnRoundEnd(int map_number, int csWinnerTeamNum, bool elimination, const char* source,
                     const std::string& notice);

struct KnifeHudInfo {
  KnifePhase phase = KnifePhase::None;
  int winnerCs = 0;          // 2 = T, 3 = CT (knife-round side of the winner)
  std::string winnerName;    // team name (scrim: "CT"/"T")
  std::string reasonShort;   // "elimination" | "more alive" | "more HP" | "coin flip"
  int secondsLeft = 0;       // pick window
};
KnifeHudInfo KnifeHudSnapshot();

// Non-default `ru_warmup_message_html` text (empty when default/unset).
std::string WarmupHtmlCustom();

// Side pick command (any knife-winning roster player, or admin). Returns true on success.
// choice: "stay"|"switch"|"ct"|"t"
bool KnifeApplySideChoice(const std::string& choice,
                          uint64_t pickerSteamid64,
                          const std::string& pickerName,
                          bool isAdminOverride);

// True if the knife winner has been determined and RU is awaiting a side pick.
bool KnifeIsAwaitingPick();

// "team1" | "team2" | "unknown"
const char* KnifeWinnerTeamString();

// Admin controls (best-effort).
// - ForceStartMatch: applies live rules + restarts game, sets mode to match_live. Under the
//   valve ruleset it refuses (false) while GOTV is down unless `force` (esports.h).
// - RestartMatch: restarts game, returns to match_warmup (requires players to ready again)
// - EndMatchResetServer: resets server rules + restarts game; intended to be used
//   alongside WebhookEmitSeriesEnd + WebhookClearMatchContext by the caller.
bool ForceStartMatch(bool force = false);
bool RestartMatch();
bool EndMatchResetServer();

// Apply match cvars immediately (no restart). Best-effort.
void ApplyMatchCvarsNow();
// mp_teamname_1/2 + mp_teamflag_1/2 from the loaded match and the current map side (a rename, a swap).
void ApplyTeamNamesNow();

// Ready toggling for players.
bool ToggleReady(uint64_t steamid64);
bool IsReady(uint64_t steamid64);
// Sets a player's ready state; returns the previous state.
bool SetReady(uint64_t steamid64, bool ready);
// Drops a single player's ready state (e.g. on disconnect).
void ClearReady(uint64_t steamid64);
void ClearReadyStates();

// `ru_warmup_enable`: the match ready-up gate, not just the banner. Off: a loaded match
// gets no warmup rules, no ready panel/banner and no knife round, and goes match_live on
// the next Round_Start. Scrims (no match config) are not affected. Default on.
void SetWarmupEnabled(bool enabled);
bool WarmupEnabled();
void SetWarmupHtmlMessage(std::string html);
std::string WarmupHtmlMessage();

// Optional: exec cfg hooks per RU mode (inspired by MatchZy cfg structure).
// When enabled, Ready Up will `exec` these files at key transitions:
// - warmup rules apply:      exec ReadyUp/warmup.cfg
// - practice rules apply:    exec ReadyUp/prac.cfg
// - knife rules apply:       exec ReadyUp/knife.cfg
// - live rules apply:        exec ReadyUp/live.cfg
// - idle mode entered:       exec ReadyUp/idle.cfg
void SetCfgExecEnabled(bool enabled);
bool CfgExecEnabled();

// Warmup gameplay rule settings (pushed via RCON).
void SetWarmupRespawnEnabled(bool enabled);
bool WarmupRespawnEnabled();
void SetWarmupIgnoreWinConditions(bool enabled);
bool WarmupIgnoreWinConditions();
void SetWarmupRoundTimeMinutes(int minutes);
int WarmupRoundTimeMinutes();
void SetWarmupStartMoney(int amount);
int WarmupStartMoney();
void SetWarmupMaxMoney(int amount);
int WarmupMaxMoney();
void SetWarmupBuyAnywhereEnabled(bool enabled);
bool WarmupBuyAnywhereEnabled();
void SetWarmupInfiniteAmmoEnabled(bool enabled);
bool WarmupInfiniteAmmoEnabled();

// CS2's own warmup started (log `World triggered "Warmup_Start"` or the
// `round_announce_warmup` event). Ready Up emulates warmup (CS2's WARMUP text
// takes over the center panel and hides Ready Up's HTML), so while idle,
// scrim_warmup, match_warmup or knife it is ended right away
// (mp_warmup_pausetimer 0, mp_warmuptime 0, mp_warmup_end). Thread-safe.
void OnNativeWarmupStarted(const char* source);

// Tick from server thread (e.g. GameFrame hook).
void Tick();

// Plugin reload (reload_state.cpp): mode, ready states, warmup settings, lifecycle / knife /
// series bookkeeping.
status::Json ModesSnapshotJson();
void ModesRestoreJson(const status::Json& j);

}  // namespace readyup


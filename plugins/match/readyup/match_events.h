#pragma once

// The match half of what used to be the core's game_events.cpp: engine game events
// (ru_api subscribe_game_event) -> round lifecycle (warmup -> live, halftime / overtime side
// swaps, knife round end, round_end webhooks, map-end detection), per-player stats
// (match_stats.h) and damage totals for the damage tiebreak.
//
// Timing: stats events (player_death, player_hurt, ...) are handled synchronously inside the
// engine's dispatch. round_start / round_end are copied and handled on the same frame's tick,
// after the core delivered that frame's log lines: the log-derived scores (SFUI notices) the
// round end webhook reports are then up to date, whichever order the engine logged and fired
// them in.

#include "readyup/plugin_api.h"

#include <cstdint>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace readyup {

// Load: subscribes the raw engine events it needs.
void MatchEventsInstall(const ru_api* api);
// on_tick: runs the round events queued this frame (after the core delivered log lines).
void MatchEventsTick();

// True once engine events are delivered (ru_api feature_state "events_live"): they then drive
// the round lifecycle, otherwise log lines do. Any thread (cached per frame off the game thread).
bool GameEventsListenerInstalled();

// Team of a player slot (2 = T, 3 = CT): the controller's m_iTeamNum when engine events are
// live, else the log-derived team. Game thread.
std::optional<int> GetCsTeamNumForSlot(int slot);
// The engine's team of a player slot right now: the controller's m_iTeamNum (0 none, 1 spectators,
// 2 T, 3 CT), read as the byte it is. nullopt off the game thread, without a controller or before
// the schema is known. Not the core's cached team, which a map load resets to 0 (team_lock.h).
std::optional<int> EngineTeamForSlot(int slot);

// Total roster-team damage on the current map, {team1, team2} (player_hurt dmg_health).
std::pair<int, int> GetRosterTeamDamageTotals();

// Engine slot of a connected player (ru_api slot_for_steamid), or nullopt. Game thread.
std::optional<int> GameEventsSlotForSteam(uint64_t steamid64);

// `World triggered "Match_Start"` (mp_restartgame, warmup end) while engine events drive the round
// lifecycle: a fresh game, so the round counter, halftime / overtime bookkeeping and the event
// totals start over (match_log.cpp does the same for its log-derived counters). Game thread.
void MatchEventsOnMatchStart();
// Team1 on CT after `swapCount` side swaps of `mapNumber` (the map's start side, flipped per swap).
bool Team1IsCtWithSwapCount(int mapNumber, int swapCount);
// A failover resume of `mapNumber` is on `team1Ct` in the resumed round: the swap count of that map
// follows (now, and after every reset of it for the map) until MatchEventsClearResumeSide.
void MatchEventsSetResumeSide(int mapNumber, bool team1Ct);
void MatchEventsClearResumeSide();
// The swap count of `mapNumber` (0 while the counters belong to another map). Lock-free: safe
// under modes.cpp's mutex.
int MatchEventsSwapCountFor(int mapNumber);
// Round restore (fleet_bridge.cpp): mp_backup_restore_load_file ends the current round as a draw
// (reason 10) right away; round ends in the next `seconds` are not a played round (no stats, no
// webhook, no map-end check). Game thread.
void MatchEventsIgnoreRoundEndsFor(double seconds);

// Reload state (reload_state.cpp): the per-map round counters that are not in MatchState.
struct MatchEventsState {
  int roundNumber = 0;
  int lastMapNumber = 0;
  int swapCount = 0;
  int lastHalfStartTotal = -1;
  int lastOvertimeNumber = 0;
  // Event-accumulated totals (round_end webhook payload, damage tiebreak).
  struct Totals {
    uint64_t steamid64 = 0;
    std::string name;
    int team = 0;  // WebhookTeam
    int kills = 0, deaths = 0, assists = 0, headshot_kills = 0, damage = 0;
  };
  std::vector<Totals> players;
};
MatchEventsState MatchEventsSnapshot();
void MatchEventsRestore(const MatchEventsState& s);

}  // namespace readyup

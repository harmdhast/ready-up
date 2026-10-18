#pragma once

// readyup.match.v1 (core/include/readyup/match_iface.h): the match part of the core's local
// status endpoint. Moved from the core's status_feed.cpp: the summary fields, MatchState
// (docs/FLEET.md §9.1) and update_safe, plus the series / demo bookkeeping they need (fed by the
// match-flow and demo listeners).

#include "readyup/match_iface.h"
#include "readyup/status_snapshot.h"

#include <string>
#include <vector>

namespace readyup {

// Load: registers the match-flow / demo listeners.
void MatchStatusInstall();
// CS2's own pause state (the game rules' m_bMatchWaitingForResume / m_bGamePaused): 1 paused or
// waiting to pause, 0 not, -1 unknown (off the game thread, no game rules yet, schema missing).
// Game thread; a cached handle and two byte reads.
int EngineMatchPaused();

// readyup.match.v1 get_status (game thread).
int MatchStatusGet(ru_match_status* out);

// The MatchState of the loaded match / scrim as the status endpoint builds it from the match
// flow (null when none is loaded). fleet_bridge overlays it on the platform's config. Game thread.
status::Json MatchStatusStateJson();

// A fleet failover resuming map N (FLEET.md §11.3): the series score and the results of the maps
// played before it, for the match `matchid` (the one just loaded). Game thread.
struct SeededMapResult {
  int map_number = 0;
  int team1 = 0, team2 = 0;
  std::string winner;  // team1 | team2 | none
};
void MatchStatusSeedSeries(unsigned long long matchid, int team1, int team2, const std::vector<SeededMapResult>& maps);

// Plugin reload (reload_state.cpp): series score / map results / demo states.
status::Json MatchStatusSnapshotJson();
void MatchStatusRestoreJson(const status::Json& j);

}  // namespace readyup

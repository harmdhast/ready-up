#pragma once

// Map end and series end.
//
// When modes.cpp decides a map (OnMatchRoundEnded), it switches to mode `postgame`
// (state: line `mode=postgame`, heartbeat `postgame`) and calls MatchEndOnMapComplete().
// Postgame lasts until 3a (the changelevel) or 3b (the unload); matches and scrims both
// pass through it:
//   1. A MapResult event (below) with the map score, the series score after the map
//      and the final per-player stats (match_stats.h).
//   2. Restart delay: 10 s without a demo; with a demo tv_delay + 15 s, plus 10 s
//      when tv_delay > 0. mp_match_restart_delay is raised above it so the engine does
//      not act first. The demo stops at the GOTV flush (tv_delay + 15 s) and uploads
//      (demo_recorder.h).
//   3a. Series continues: `changelevel <next map>` at restart_delay - 1 and the next
//       map starts in match warmup.
//   3b. Series over (no maps left, or a team clinched when clinch_series is on): a
//       SeriesEnd event. For matches, every human is kicked after
//       (restart_delay - 1) + ru_series_end_kick_delay_{no_demo|demo_no_upload|demo_upload}
//       (30 s each by default, so players can read the scoreboard), but never later than
//       kMatchEndKickMaxS (20 s) after the series end. The kick takes every human slot, admins
//       and slots without a SteamID included. The match is unloaded (ServerReset event, mode
//       postgame -> idle) at restart_delay + the configured delay + 1 s, after the demo flush. Scrims are not kicked; they stay in postgame until
//       restart_delay - 1 (9 s without a demo), then unload.
//
// Internal events are delivered to listeners (AddMatchFlowListener) on the game thread.
// The transport that turns them into platform messages is designed separately; the
// existing webhooks (map_result / series_end) are still sent as before.
//
// Console / RCON:
//   ru_series_end_kick_delay_no_demo <s>        default 30
//   ru_series_end_kick_delay_demo_no_upload <s> default 30
//   ru_series_end_kick_delay_demo_upload <s>    default 30
//   ru_match_stats [matchid]                    current map stats as one JSON line (at_payloads.h)
// plus the demo settings in demo_recorder.h.

#include "readyup/match_stats.h"
#include "readyup/status_snapshot.h"

#include <functional>
#include <string>
#include <vector>

namespace readyup {

// ---------------------------------------------------------------------------- pure logic

// Longest a human may stay on the server after a match ends, is unassigned or moves.
constexpr double kMatchEndKickMaxS = 20;

struct MapEndPlan {
  int restartDelay = 10;  // seconds until the next map / the series-end window starts
  int tvFlushDelay = 0;   // seconds until tv_stoprecord (0 = no demo)
  int kickDelay = 0;      // series end: seconds until players are kicked
};

MapEndPlan ComputeMapEndPlan(bool demoRecording, bool hasUploadUrl, int tvDelay, int kickExtraNoDemo,
                             int kickExtraDemoNoUpload, int kickExtraDemoUpload);
// Seconds after the series end until every human is kicked.
double KickDelayAfterSeriesEnd(int restartDelay, int configuredKickDelay);
// Maps left after mapNumber (1-based) in a series of numMaps (capped by the map list).
int RemainingMaps(int numMaps, int maplistCount, int mapNumber);
// Over when no maps are left, or (canClinch) a team has won more than half of numMaps.
bool IsSeriesOver(int numMaps, int remainingMaps, int team1SeriesScore, int team2SeriesScore, bool canClinch);

// ---------------------------------------------------------------------------- events

enum class MatchFlowEventType { MapResult, SeriesEnd, ServerReset };

struct MatchFlowEvent {
  MatchFlowEventType type = MatchFlowEventType::MapResult;
  unsigned long long matchid = 0;
  std::string slug;
  bool scrim = false;
  int mapNumber = 1;           // 1-based (MapResult)
  std::string mapName;         // MapResult
  std::string winner;          // "team1" | "team2" | "none" (map winner / series winner)
  std::string team1Name;
  std::string team2Name;
  int team1Score = 0;          // map score (MapResult)
  int team2Score = 0;
  int team1SeriesScore = 0;    // after this map
  int team2SeriesScore = 0;
  bool seriesOver = false;     // MapResult: this map ended the series
  std::string nextMap;         // MapResult when the series continues
  int secondsUntilReset = 0;   // SeriesEnd: until players are kicked / the match is unloaded
  bool demoRecorded = false;
  bool demoUploadConfigured = false;
  stats::MapStats stats;       // MapResult
  bool decidedByDamage = false;  // MapResult: tied after the overtimes, won on total damage
  int team1Damage = 0, team2Damage = 0;
};

const char* MatchFlowEventTypeName(MatchFlowEventType t);
std::string ToJson(const MatchFlowEvent& e);

using MatchFlowListener = std::function<void(const MatchFlowEvent&)>;
void AddMatchFlowListener(MatchFlowListener fn);

// ---------------------------------------------------------------------------- flow

void SetSeriesEndKickDelays(int noDemo, int demoNoUpload, int demoUpload);  // <0 keeps a value
void GetSeriesEndKickDelays(int* noDemo, int* demoNoUpload, int* demoUpload);

struct MapEndInput {
  int mapNumber = 1;         // 1-based
  std::string mapName;
  std::string winner;        // map winner: "team1" | "team2" | "none"
  int team1Score = 0;
  int team2Score = 0;
  int team1SeriesScore = 0;  // after this map
  int team2SeriesScore = 0;
  bool seriesOver = false;
  std::string nextMap;       // when the series continues
  // Series over: the winner when it is not the series score's (a forfeit); empty = by score.
  std::string seriesWinner;
  // A map tied after its overtimes and won on total damage (damage tiebreak): both teams' damage.
  bool decidedByDamage = false;
  int team1Damage = 0, team2Damage = 0;
};

// Game thread, called with the modes mutex held: sends the MapResult event and only
// schedules the rest (nothing calls back into modes synchronously).
MapEndPlan MatchEndOnMapComplete(const MapEndInput& in);

// Drops pending changelevel / kick / reset work (admin end, restart, new match).
void MatchEndCancelPending();
bool MatchEndPending();

// Settings commands (this file + demo_recorder.h). Returns true if consumed.
bool MatchFlowHandleConsoleLine(const std::string& line);

// Plugin reload (reload_state.cpp): kick delays and the pending postgame step (next map,
// series-end kick, match unload), rescheduled with the time it had left.
status::Json MatchEndSnapshotJson();
void MatchEndRestoreJson(const status::Json& j);

}  // namespace readyup

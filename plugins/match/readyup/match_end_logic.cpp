#include "readyup/match_end.h"

#include <algorithm>

// Pure parts of match_end.h (no engine or server state): unit-tested by
// tests/match_flow_test.cpp.

namespace readyup {

namespace {
constexpr int kFlushMargin = 15;      // GOTV needs tv_delay + this to write the last ticks
constexpr int kTvDelayMargin = 10;    // extra restart delay when tv_delay > 0
constexpr int kNoDemoRestartDelay = 10;
}  // namespace

MapEndPlan ComputeMapEndPlan(bool demoRecording, bool hasUploadUrl, int tvDelay, int kickExtraNoDemo,
                             int kickExtraDemoNoUpload, int kickExtraDemoUpload) {
  MapEndPlan p;
  tvDelay = std::max(0, tvDelay);
  if (!demoRecording) {
    p.restartDelay = kNoDemoRestartDelay;
    p.kickDelay = (p.restartDelay - 1) + std::max(0, kickExtraNoDemo);
    return p;
  }
  p.tvFlushDelay = tvDelay + kFlushMargin;
  p.restartDelay = tvDelay > 0 ? p.tvFlushDelay + kTvDelayMargin : p.tvFlushDelay;
  p.kickDelay = (p.restartDelay - 1) + std::max(0, hasUploadUrl ? kickExtraDemoUpload : kickExtraDemoNoUpload);
  return p;
}

double KickDelayAfterSeriesEnd(int restartDelay, int configuredKickDelay) {
  const double d = (restartDelay - 1) + std::max(0, configuredKickDelay);
  return std::min(kMatchEndKickMaxS, std::max(0.0, d));
}

int RemainingMaps(int numMaps, int maplistCount, int mapNumber) {
  const int total = maplistCount > 0 ? std::min(numMaps, maplistCount) : numMaps;
  return std::max(0, total - mapNumber);
}

bool IsSeriesOver(int numMaps, int remainingMaps, int team1SeriesScore, int team2SeriesScore, bool canClinch) {
  if (remainingMaps <= 0) return true;
  if (!canClinch) return false;
  const int mapsToWin = (numMaps / 2) + 1;
  return team1SeriesScore >= mapsToWin || team2SeriesScore >= mapsToWin;
}

const char* MatchFlowEventTypeName(MatchFlowEventType t) {
  switch (t) {
    case MatchFlowEventType::MapResult: return "map_result";
    case MatchFlowEventType::SeriesEnd: return "series_end";
    case MatchFlowEventType::ServerReset: return "server_reset";
  }
  return "unknown";
}

std::string ToJson(const MatchFlowEvent& e) {
  using stats::JsonEscape;
  std::string j = std::string("{\"type\":\"") + MatchFlowEventTypeName(e.type) + "\"" +
                  ",\"matchid\":\"" + std::to_string(e.matchid) + "\"" + ",\"slug\":\"" + JsonEscape(e.slug) + "\"" +
                  ",\"scrim\":" + (e.scrim ? "true" : "false") + ",\"winner\":\"" + JsonEscape(e.winner) + "\"" +
                  ",\"team1_name\":\"" + JsonEscape(e.team1Name) + "\"" + ",\"team2_name\":\"" +
                  JsonEscape(e.team2Name) + "\"" + ",\"team1_series_score\":" + std::to_string(e.team1SeriesScore) +
                  ",\"team2_series_score\":" + std::to_string(e.team2SeriesScore);
  if (e.type == MatchFlowEventType::MapResult) {
    j += ",\"map_number\":" + std::to_string(e.mapNumber) + ",\"map_name\":\"" + JsonEscape(e.mapName) + "\"" +
         ",\"team1_score\":" + std::to_string(e.team1Score) + ",\"team2_score\":" + std::to_string(e.team2Score) +
         ",\"series_over\":" + (e.seriesOver ? "true" : "false") + ",\"next_map\":\"" + JsonEscape(e.nextMap) + "\"" +
         ",\"demo_recorded\":" + (e.demoRecorded ? "true" : "false") +
         ",\"demo_upload_configured\":" + (e.demoUploadConfigured ? "true" : "false") +
         ",\"stats\":" + stats::ToJson(e.stats);
  } else if (e.type == MatchFlowEventType::SeriesEnd) {
    j += ",\"seconds_until_reset\":" + std::to_string(e.secondsUntilReset);
  }
  return j + "}";
}

}  // namespace readyup

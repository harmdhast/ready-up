#include "readyup/demo_recorder.h"

#include "readyup/match_stats.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <utility>

// Pure helpers of the demo recorder (no engine or server state): unit-tested by
// tests/match_flow_test.cpp.

namespace readyup::demo {
namespace {

std::string BaseName(const std::string& p) {
  const size_t s = p.find_last_of('/');
  return s == std::string::npos ? p : p.substr(s + 1);
}

void ReplaceAll(std::string& s, const std::string& from, const std::string& to) {
  size_t pos = 0;
  while ((pos = s.find(from, pos)) != std::string::npos) {
    s.replace(pos, from.size(), to);
    pos += to.size();
  }
}

}  // namespace

// ------------------------------------------------------------------------------ events

const char* EventTypeName(DemoEventType t) {
  switch (t) {
    case DemoEventType::RecordingStarted: return "recording_started";
    case DemoEventType::RecordingStopped: return "recording_stopped";
    case DemoEventType::UploadStarted: return "upload_started";
    case DemoEventType::UploadSucceeded: return "upload_succeeded";
    case DemoEventType::UploadFailed: return "upload_failed";
  }
  return "unknown";
}

std::string ToJson(const DemoEvent& e) {
  char size[32];
  std::snprintf(size, sizeof(size), "%.2f", e.sizeMb);
  return std::string("{\"type\":\"") + EventTypeName(e.type) + "\",\"matchid\":" + std::to_string(e.matchid) +
         ",\"map_number\":" + std::to_string(e.mapNumber) + ",\"file_name\":\"" + stats::JsonEscape(e.fileName) +
         "\",\"path\":\"" + stats::JsonEscape(e.path) + "\",\"size_mb\":" + size +
         ",\"http_status\":" + std::to_string(e.httpStatus) + ",\"error\":\"" + stats::JsonEscape(e.error) +
         "\",\"attempts\":" + std::to_string(e.attempts) + "}";
}

// ------------------------------------------------------------------------------ helpers

std::string ExpandTokens(const std::string& format, const TokenValues& v) {
  std::string out = format;
  ReplaceAll(out, "{TIME}", v.time);
  ReplaceAll(out, "{MATCH_ID}", std::to_string(v.matchid));
  ReplaceAll(out, "{SLUG}", v.slug);
  ReplaceAll(out, "{MAP_NUMBER}", std::to_string(v.mapNumber));
  ReplaceAll(out, "{MAP_INDEX}", std::to_string(std::max(0, v.mapNumber - 1)));
  ReplaceAll(out, "{MAP}", v.map);
  ReplaceAll(out, "{TEAM1_SCORE}", std::to_string(v.team1Score));
  ReplaceAll(out, "{TEAM2_SCORE}", std::to_string(v.team2Score));
  ReplaceAll(out, "{TEAM1}", v.team1);
  ReplaceAll(out, "{TEAM2}", v.team2);
  ReplaceAll(out, "{FILENAME}", v.fileName);
  ReplaceAll(out, "{ROUND_NUMBER}", std::to_string(v.roundNumber));
  return out;
}

std::string FormatDemoFileName(const std::string& format, const TokenValues& v) {
  std::string out = ExpandTokens(format, v);
  // Only what the fleet stores (fleet_demo.h IsDemoFileName): a team called "Team!" would otherwise lose its demo.
  for (char& c : out) {
    const unsigned char u = static_cast<unsigned char>(c);
    if (!std::isalnum(u) && c != '.' && c != '-') c = '_';
  }
  while (!out.empty() && out.front() == '.') out.erase(out.begin());
  if (out.empty()) out = "readyup_demo";
  return out;
}

bool UploadWanted(const Settings& s, int matchUpload) { return !s.uploadUrl.empty() && matchUpload != 0; }

std::vector<std::string> UploadHeaderLines(const Settings& s, const TokenValues& v) {
  std::vector<std::pair<std::string, std::string>> hs;
  auto lower = [](std::string x) {
    for (char& c : x) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return x;
  };
  auto set = [&](const std::string& name, const std::string& value) {
    const std::string key = lower(name);
    for (auto& h : hs) {
      if (lower(h.first) == key) {
        h.second = value;
        return;
      }
    }
    hs.emplace_back(name, value);
  };
  const std::string matchId = std::to_string(v.matchid);
  const std::string mapNumber = std::to_string(v.mapNumber);
  const std::string round = std::to_string(v.roundNumber);
  for (const char* prefix : {"Auto-Tournament-", "Get5-"}) {
    const std::string p = prefix;
    set(p + "FileName", v.fileName);
    set(p + "MatchId", matchId);
    set(p + "MapNumber", mapNumber);
    set(p + "RoundNumber", round);
  }
  for (const auto& h : s.uploadHeaders) set(h.first, ExpandTokens(h.second, v));
  if (!s.headerKey.empty() && !s.headerValue.empty()) set(s.headerKey, s.headerValue);
  std::vector<std::string> out;
  out.reserve(hs.size());
  for (const auto& h : hs) out.push_back(h.first + ": " + h.second);
  return out;
}

std::string PickDemoForMatch(const std::vector<DemoCandidate>& files, const std::string& expectedFileName,
                             long long matchid, const std::string& mapName, long long notBeforeEpoch) {
  for (const auto& f : files) {
    if (BaseName(f.path) == expectedFileName) return f.path;
  }
  const std::string idToken = "_" + std::to_string(matchid) + "_";
  const DemoCandidate* best = nullptr;
  for (const auto& f : files) {
    const std::string n = BaseName(f.path);
    if (n.size() < 4 || n.compare(n.size() - 4, 4, ".dem") != 0) continue;
    if (("_" + n).find(idToken) == std::string::npos) continue;
    if (!mapName.empty() && n.find(mapName) == std::string::npos) continue;
    if (notBeforeEpoch > 0 && f.mtimeEpoch < notBeforeEpoch) continue;
    if (!best || f.mtimeEpoch > best->mtimeEpoch) best = &f;
  }
  return best ? best->path : std::string();
}

}  // namespace readyup::demo

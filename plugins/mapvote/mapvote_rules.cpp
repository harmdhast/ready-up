#include "mapvote_rules.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>

namespace mapvote {

std::vector<std::string> DefaultPool(const std::string& currentMap) {
  const bool defuse = currentMap.rfind("de_", 0) == 0 || currentMap.rfind("cs_", 0) == 0;
  if (!defuse) return {};
  // Valve's Active Duty pool (csm's maps.json, 2026-10).
  return {"de_dust2", "de_inferno", "de_mirage", "de_nuke", "de_overpass", "de_train", "de_vertigo"};
}

namespace {

std::string Trim(const std::string& s) {
  size_t a = 0, b = s.size();
  while (a < b && std::isspace(static_cast<unsigned char>(s[a]))) ++a;
  while (b > a && std::isspace(static_cast<unsigned char>(s[b - 1]))) --b;
  return s.substr(a, b - a);
}

std::string Lower(std::string s) {
  for (auto& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  return s;
}

bool ParseInt(const std::string& v, int lo, int hi, int* out) {
  char* end = nullptr;
  const long n = std::strtol(v.c_str(), &end, 10);
  if (v.empty() || !end || *end != '\0' || n < lo || n > hi) return false;
  *out = static_cast<int>(n);
  return true;
}

}  // namespace

std::vector<std::string> ParseMapList(const std::string& text) {
  std::vector<std::string> out;
  std::set<std::string> seen;
  std::string cur;
  auto flush = [&] {
    const std::string t = Trim(cur);
    cur.clear();
    if (t.empty() || t[0] == '#' || (t.size() > 1 && t[0] == '/' && t[1] == '/')) return;
    if (seen.insert(Lower(t)).second) out.push_back(t);
  };
  for (char c : text) {
    if (c == ',' || c == '\n' || c == '\r') flush();
    else cur += c;
  }
  flush();
  return out;
}

std::string MapDisplayName(const std::string& entry) {
  if (entry.rfind("workshop/", 0) == 0) {
    const std::string rest = entry.substr(9);
    const size_t slash = rest.find('/');
    if (slash != std::string::npos && slash + 1 < rest.size()) return rest.substr(slash + 1);
    return "workshop " + rest;
  }
  return entry;
}

std::string FindMap(const std::vector<std::string>& pool, const std::string& typed, std::vector<std::string>* ambiguous) {
  const std::string t = Lower(Trim(typed));
  if (t.empty()) return {};
  for (const auto& e : pool) {
    if (Lower(MapDisplayName(e)) == t || Lower(e) == t) return e;
  }
  std::vector<std::string> hits;
  for (const auto& e : pool) {
    if (Lower(MapDisplayName(e)).find(t) != std::string::npos) hits.push_back(e);
  }
  if (hits.size() == 1) return hits[0];
  if (ambiguous) {
    ambiguous->clear();
    for (size_t i = 0; i < hits.size() && i < 5; ++i) ambiguous->push_back(MapDisplayName(hits[i]));
  }
  return {};
}

int RtvNeeded(int humans, double ratio) {
  if (humans <= 0) return 1;
  const int n = static_cast<int>(std::ceil(ratio * humans - 1e-9));
  return std::max(1, std::min(n, humans));
}

std::vector<std::string> BuildBallot(const std::vector<std::string>& pool, const std::vector<std::string>& nominations,
                                     const std::string& currentMap, const std::vector<std::string>& recent, int count,
                                     const std::function<int(int)>& rng) {
  std::vector<std::string> out;
  std::set<std::string> used;
  const std::string cur = Lower(currentMap);
  auto isCurrent = [&](const std::string& e) { return !cur.empty() && (Lower(e) == cur || Lower(MapDisplayName(e)) == cur); };
  for (const auto& n : nominations) {
    if (static_cast<int>(out.size()) >= count) break;
    if (isCurrent(n) || !used.insert(Lower(n)).second) continue;
    out.push_back(n);
  }
  std::set<std::string> recentSet;
  for (const auto& r : recent) {
    recentSet.insert(Lower(r));
    recentSet.insert(Lower(MapDisplayName(r)));
  }
  std::vector<std::string> rest;
  for (const auto& e : pool) {
    if (isCurrent(e) || used.count(Lower(e)) || recentSet.count(Lower(e)) || recentSet.count(Lower(MapDisplayName(e)))) continue;
    rest.push_back(e);
  }
  // A small pool: recent maps may come back rather than leaving the ballot short.
  if (static_cast<int>(out.size() + rest.size()) < count) {
    for (const auto& e : pool) {
      if (isCurrent(e) || used.count(Lower(e))) continue;
      if (std::find(rest.begin(), rest.end(), e) == rest.end()) rest.push_back(e);
    }
  }
  while (static_cast<int>(out.size()) < count && !rest.empty()) {
    const int i = rng(static_cast<int>(rest.size()));
    out.push_back(rest[static_cast<size_t>(i)]);
    used.insert(Lower(rest[static_cast<size_t>(i)]));
    rest.erase(rest.begin() + i);
  }
  return out;
}

int Winner(int optionCount, const std::map<uint64_t, int>& votes, int extendIndex, const std::function<int(int)>& rng) {
  if (optionCount <= 0) return -1;
  std::vector<int> tally(static_cast<size_t>(optionCount), 0);
  for (const auto& kv : votes) {
    if (kv.second >= 0 && kv.second < optionCount) ++tally[static_cast<size_t>(kv.second)];
  }
  const int best = *std::max_element(tally.begin(), tally.end());
  std::vector<int> top;
  if (best == 0) {
    for (int i = 0; i < optionCount; ++i) {
      if (i != extendIndex) top.push_back(i);
    }
    if (top.empty()) return extendIndex;
  } else {
    for (int i = 0; i < optionCount; ++i) {
      if (tally[static_cast<size_t>(i)] == best) top.push_back(i);
    }
  }
  return top[static_cast<size_t>(rng(static_cast<int>(top.size())))];
}

int SecondsLeft(const Settings& s, double mapStartedAt, int extends, double now) {
  if (s.mapTimeMinutes <= 0) return -1;
  const double total = (s.mapTimeMinutes + static_cast<double>(extends) * s.extendMinutes) * 60.0;
  return std::max(0, static_cast<int>(std::ceil(total - (now - mapStartedAt))));
}

std::string FormatClock(int seconds) {
  if (seconds < 0) seconds = 0;
  char buf[32];
  std::snprintf(buf, sizeof(buf), "%d:%02d", seconds / 60, seconds % 60);
  return buf;
}

Settings LoadSettings(const std::function<bool(const char*, std::string*)>& get, std::vector<std::string>* warnings) {
  Settings s;
  std::string v;
  auto warn = [&](const char* key) {
    if (warnings) warnings->push_back(std::string("bad value for ") + key + ": " + v);
  };
  auto intKey = [&](const char* key, int lo, int hi, int* out) {
    if (!get(key, &v)) return;
    v = Trim(v);
    if (!ParseInt(v, lo, hi, out)) warn(key);
  };
  if (get("enabled", &v)) s.enabled = Trim(v) != "0";
  if (get("maps", &v)) s.maps = ParseMapList(v);
  if (get("rtv_ratio", &v)) {
    char* end = nullptr;
    const double d = std::strtod(Trim(v).c_str(), &end);
    if (end && *end == '\0' && d > 0 && d <= 1) s.rtvRatio = d;
    else warn("rtv_ratio");
  }
  intKey("rtv_min_players", 1, 64, &s.rtvMinPlayers);
  intKey("rtv_delay_seconds", 0, 3600, &s.rtvDelaySeconds);
  intKey("vote_options", 2, 8, &s.voteOptions);
  intKey("vote_seconds", 5, 120, &s.voteSeconds);
  intKey("map_time_minutes", 0, 600, &s.mapTimeMinutes);
  intKey("vote_before_end_seconds", 10, 600, &s.voteBeforeEndSeconds);
  intKey("extend_minutes", 1, 120, &s.extendMinutes);
  intKey("max_extends", 0, 10, &s.maxExtends);
  intKey("recent_maps_excluded", 0, 20, &s.recentMapsExcluded);
  return s;
}

}  // namespace mapvote

#include "community_rules.h"

#include "readyup/status_snapshot.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>

namespace community {

using readyup::status::Json;

int64_t Points(const PlayerStats& s) {
  const int64_t p = s.kills * 2 + s.headshots + s.assists - s.deaths;
  return p < 0 ? 0 : p;
}

std::string Kdr(const PlayerStats& s) {
  char buf[32];
  if (s.deaths <= 0) std::snprintf(buf, sizeof(buf), "%lld.00", static_cast<long long>(s.kills));
  else std::snprintf(buf, sizeof(buf), "%.2f", static_cast<double>(s.kills) / static_cast<double>(s.deaths));
  return buf;
}

std::vector<Ranked> Ranking(const std::map<uint64_t, PlayerStats>& all, int64_t minKills) {
  std::vector<Ranked> out;
  for (const auto& kv : all) {
    if (kv.second.kills < minKills) continue;
    out.push_back({kv.first, &kv.second, Points(kv.second)});
  }
  std::sort(out.begin(), out.end(), [](const Ranked& a, const Ranked& b) {
    if (a.points != b.points) return a.points > b.points;
    if (a.stats->kills != b.stats->kills) return a.stats->kills > b.stats->kills;
    if (a.stats->deaths != b.stats->deaths) return a.stats->deaths < b.stats->deaths;
    return a.steamid64 < b.steamid64;
  });
  return out;
}

int RankOf(const std::vector<Ranked>& ranking, uint64_t steamid64) {
  for (size_t i = 0; i < ranking.size(); ++i) {
    if (ranking[i].steamid64 == steamid64) return static_cast<int>(i) + 1;
  }
  return 0;
}

std::string Serialize(const std::map<uint64_t, PlayerStats>& all) {
  Json root = Json::Object();
  Json& players = root["players"];
  players = Json::Object();
  for (const auto& kv : all) {
    Json p = Json::Object();
    p["name"] = kv.second.name;
    p["kills"] = static_cast<long long>(kv.second.kills);
    p["deaths"] = static_cast<long long>(kv.second.deaths);
    p["headshots"] = static_cast<long long>(kv.second.headshots);
    p["assists"] = static_cast<long long>(kv.second.assists);
    p["last_seen"] = static_cast<long long>(kv.second.lastSeen);
    players[std::to_string(kv.first)] = p;
  }
  return root.Dump();
}

bool Deserialize(const std::string& text, std::map<uint64_t, PlayerStats>* out) {
  Json root;
  if (!Json::Parse(text, &root) || !root.IsObject()) return false;
  const Json* players = root.Find("players");
  if (!players || !players->IsObject()) return false;
  out->clear();
  for (const auto& kv : players->Members()) {
    char* end = nullptr;
    const unsigned long long id = std::strtoull(kv.first.c_str(), &end, 10);
    if (!end || *end != '\0' || id == 0 || !kv.second.IsObject()) continue;
    PlayerStats s;
    auto num = [&](const char* k) -> int64_t {
      const Json* v = kv.second.Find(k);
      return v ? v->AsInt() : 0;
    };
    if (const Json* n = kv.second.Find("name")) s.name = n->AsString();
    s.kills = num("kills");
    s.deaths = num("deaths");
    s.headshots = num("headshots");
    s.assists = num("assists");
    s.lastSeen = num("last_seen");
    (*out)[id] = s;
  }
  return true;
}

int FindByName(const std::vector<std::pair<int, std::string>>& names, const std::string& typed, bool* ambiguous) {
  auto lower = [](std::string s) {
    for (auto& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
  };
  const std::string t = lower(typed);
  if (ambiguous) *ambiguous = false;
  if (t.empty()) return -1;
  for (const auto& n : names) {
    if (lower(n.second) == t) return n.first;
  }
  int found = -1, count = 0;
  for (const auto& n : names) {
    if (lower(n.second).find(t) != std::string::npos) {
      found = n.first;
      ++count;
    }
  }
  if (count == 1) return found;
  if (count > 1 && ambiguous) *ambiguous = true;
  return -1;
}

}  // namespace community

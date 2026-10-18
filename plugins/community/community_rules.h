// readyup-community: the rules (pure, no engine; ctest `community_rules`). See community_plugin.cpp.
#pragma once

#include <cstdint>
#include <map>
#include <string>
#include <vector>

namespace community {

struct PlayerStats {
  std::string name;
  int64_t kills = 0, deaths = 0, headshots = 0, assists = 0;
  int64_t lastSeen = 0;  // unix seconds
};

// Points: 2 per kill, 1 per headshot, 1 per assist, -1 per death; never below 0.
int64_t Points(const PlayerStats& s);

// Kills per death to two decimals ("1.25"; deaths 0 -> the kill count).
std::string Kdr(const PlayerStats& s);

struct Ranked {
  uint64_t steamid64 = 0;
  const PlayerStats* stats = nullptr;
  int64_t points = 0;
};

// Every player with at least `minKills` kills, best first: points, then kills, then fewer deaths,
// then steamid (stable).
std::vector<Ranked> Ranking(const std::map<uint64_t, PlayerStats>& all, int64_t minKills);

// 1-based rank of `steamid64` in `ranking`, 0 when not ranked.
int RankOf(const std::vector<Ranked>& ranking, uint64_t steamid64);

// stats.json <-> the map (players: {"<steamid64>": {name, kills, deaths, headshots, assists, last_seen}}).
std::string Serialize(const std::map<uint64_t, PlayerStats>& all);
bool Deserialize(const std::string& text, std::map<uint64_t, PlayerStats>* out);

// Name lookup for admin commands: exact (case-insensitive) first, then a unique substring.
// `names` is (key, display name). Returns the key or -1; `ambiguous` set when several match.
int FindByName(const std::vector<std::pair<int, std::string>>& names, const std::string& typed, bool* ambiguous);

}  // namespace community

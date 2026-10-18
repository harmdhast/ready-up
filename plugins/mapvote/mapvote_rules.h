// readyup-mapvote: the rules (pure, no engine; ctest `mapvote_rules`). See mapvote_plugin.cpp.
#pragma once

#include <cstdint>
#include <functional>
#include <map>
#include <set>
#include <string>
#include <vector>

namespace mapvote {

struct Settings {
  bool enabled = true;
  std::vector<std::string> maps;  // the pool: map names or workshop entries (workshop/<id>[/<name>])
  double rtvRatio = 0.6;          // share of the human players needed for .rtv
  int rtvMinPlayers = 1;          // .rtv needs at least this many humans on the server
  int rtvDelaySeconds = 60;       // no .rtv in the first seconds of a map
  int voteOptions = 6;            // maps on the ballot (nominations first), plus "Extend" when allowed
  int voteSeconds = 20;
  int mapTimeMinutes = 30;        // 0 = no time limit (votes only by .rtv)
  int voteBeforeEndSeconds = 120; // the end-of-map vote starts this long before the time runs out
  int extendMinutes = 10;
  int maxExtends = 1;
  int recentMapsExcluded = 3;     // the last N maps are not on the ballot
};

// The pool when mapvote.cfg sets none: CS2's Active Duty maps while a defuse or hostage map (de_,
// cs_) is on, nothing otherwise (a surf, kz or aim server needs its own list).
std::vector<std::string> DefaultPool(const std::string& currentMap);

// Parses `maps` (comma or newline separated). Blank entries and duplicates are dropped.
std::vector<std::string> ParseMapList(const std::string& text);

// Display name of a map entry: "de_dust2", "workshop/3070/surf_utopia" -> "surf_utopia",
// "workshop/3070" -> "workshop 3070".
std::string MapDisplayName(const std::string& entry);

// Finds a pool entry by what a player typed (exact display name first, then a unique substring).
// Empty when none or ambiguous; `ambiguous` lists the candidates (at most 5).
std::string FindMap(const std::vector<std::string>& pool, const std::string& typed, std::vector<std::string>* ambiguous);

// Votes needed for .rtv: ceil(ratio * humans), at least 1.
int RtvNeeded(int humans, double ratio);

// The ballot: nominations (in order, not the current map), then random pool maps that are not the
// current map, not recent and not already on it; up to `count`. `rng(n)` returns [0, n).
std::vector<std::string> BuildBallot(const std::vector<std::string>& pool, const std::vector<std::string>& nominations,
                                     const std::string& currentMap, const std::vector<std::string>& recent, int count,
                                     const std::function<int(int)>& rng);

// Winner of a vote: options are 0..n-1 (the extend option, when offered, is index n-1 and named in
// `extendIndex`, else -1). Most votes wins; a tie is broken by `rng`; no votes at all -> a random
// map option (never extend). Returns the winning index, -1 when there are no options.
int Winner(int optionCount, const std::map<uint64_t, int>& votes, int extendIndex, const std::function<int(int)>& rng);

// Seconds left on the map, -1 when there is no time limit.
int SecondsLeft(const Settings& s, double mapStartedAt, int extends, double now);

// "12:05" / "0:42".
std::string FormatClock(int seconds);

// Settings from key/value lookups (config_get). Unknown keys are ignored; bad values keep the
// default and add a warning.
Settings LoadSettings(const std::function<bool(const char*, std::string*)>& get, std::vector<std::string>* warnings);

}  // namespace mapvote

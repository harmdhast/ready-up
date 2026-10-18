// Offline tests for plugins/mapvote/mapvote_rules.h. ctest `mapvote_rules`.
#include "mapvote_rules.h"

#include <cstdio>
#include <map>
#include <string>

using namespace mapvote;

static int g_failures = 0;

#define CHECK(cond)                                                                 \
  do {                                                                              \
    if (!(cond)) {                                                                  \
      std::fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #cond); \
      ++g_failures;                                                                 \
    }                                                                               \
  } while (0)

static int First(int) { return 0; }

int main() {
  // Map lists: commas and lines, blanks, comments and duplicates dropped.
  const auto pool = ParseMapList("de_dust2, de_mirage\nworkshop/3070/surf_utopia\n\n# comment\nDE_DUST2\nworkshop/123");
  CHECK(pool.size() == 4);
  CHECK(pool[2] == "workshop/3070/surf_utopia");
  CHECK(MapDisplayName("workshop/3070/surf_utopia") == "surf_utopia");
  CHECK(MapDisplayName("workshop/123") == "workshop 123");
  CHECK(MapDisplayName("de_dust2") == "de_dust2");

  // Lookup: exact first, then a unique part; ambiguous lists candidates.
  std::vector<std::string> amb;
  CHECK(FindMap(pool, "surf_utopia", &amb) == "workshop/3070/surf_utopia");
  CHECK(FindMap(pool, "utopia", &amb) == "workshop/3070/surf_utopia");
  CHECK(FindMap(pool, "de_", &amb).empty() && amb.size() == 2);
  CHECK(FindMap(pool, "nuke", &amb).empty());

  // RTV: ceil(ratio * humans), at least 1, at most everyone.
  CHECK(RtvNeeded(1, 0.6) == 1);
  CHECK(RtvNeeded(5, 0.6) == 3);
  CHECK(RtvNeeded(10, 0.6) == 6);
  CHECK(RtvNeeded(0, 0.6) == 1);
  CHECK(RtvNeeded(3, 1.0) == 3);

  // Ballot: nominations first, never the current map, recent maps left out while others remain.
  const std::vector<std::string> p2 = {"a", "b", "c", "d", "e"};
  auto b = BuildBallot(p2, {"d", "a", "d"}, "a", {"b"}, 3, &First);
  CHECK(b.size() == 3 && b[0] == "d" && b[1] == "c" && b[2] == "e");
  // Small pool: recent maps come back rather than a short ballot.
  b = BuildBallot({"a", "b", "c"}, {}, "a", {"b", "c"}, 2, &First);
  CHECK(b.size() == 2);
  // Only the current map: an empty ballot.
  CHECK(BuildBallot({"a"}, {}, "a", {}, 3, &First).empty());

  // Winner: most votes; nobody voted -> a map, never extend.
  std::map<uint64_t, int> votes = {{1, 1}, {2, 1}, {3, 0}};
  CHECK(Winner(3, votes, 2, &First) == 1);
  CHECK(Winner(3, {}, 2, &First) == 0);
  CHECK(Winner(3, {{1, 2}}, 2, &First) == 2);  // extend can win with votes
  CHECK(Winner(0, {}, -1, &First) == -1);
  CHECK(Winner(3, {{1, 7}}, -1, &First) == 0);  // out-of-range votes do not count

  // Clock.
  Settings s;
  s.mapTimeMinutes = 30;
  s.extendMinutes = 10;
  CHECK(SecondsLeft(s, 100.0, 0, 100.0) == 1800);
  CHECK(SecondsLeft(s, 100.0, 1, 100.0) == 2400);
  CHECK(SecondsLeft(s, 0.0, 0, 5000.0) == 0);
  s.mapTimeMinutes = 0;
  CHECK(SecondsLeft(s, 0.0, 0, 10.0) == -1);
  CHECK(FormatClock(725) == "12:05");
  CHECK(FormatClock(42) == "0:42");

  // Settings: values, defaults on bad input.
  // No pool set: the Active Duty maps on a defuse map, nothing on a surf map.
  CHECK(mapvote::DefaultPool("de_dust2").size() == 7);
  CHECK(mapvote::DefaultPool("cs_office").size() == 7);
  CHECK(mapvote::DefaultPool("surf_utopia_njv").empty());
  CHECK(mapvote::DefaultPool("").empty());

  std::map<std::string, std::string> cfg = {{"maps", "a,b"}, {"rtv_ratio", "0.5"}, {"vote_options", "99"}, {"enabled", "0"}};
  std::vector<std::string> warnings;
  const Settings loaded = LoadSettings(
      [&](const char* k, std::string* out) {
        auto it = cfg.find(k);
        if (it == cfg.end()) return false;
        *out = it->second;
        return true;
      },
      &warnings);
  CHECK(loaded.maps.size() == 2);
  CHECK(loaded.rtvRatio == 0.5);
  CHECK(loaded.voteOptions == 6 && warnings.size() == 1);
  CHECK(!loaded.enabled);

  if (g_failures) std::fprintf(stderr, "%d failure(s)\n", g_failures);
  else std::printf("mapvote_rules: ok\n");
  return g_failures ? 1 : 0;
}

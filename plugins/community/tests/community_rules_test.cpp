// Offline tests for plugins/community/community_rules.h. ctest `community_rules`.
#include "community_rules.h"

#include <cstdio>

using namespace community;

static int g_failures = 0;

#define CHECK(cond)                                                                 \
  do {                                                                              \
    if (!(cond)) {                                                                  \
      std::fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #cond); \
      ++g_failures;                                                                 \
    }                                                                               \
  } while (0)

int main() {
  PlayerStats a{"alice", 20, 10, 5, 2, 0};
  PlayerStats b{"bob", 20, 5, 0, 0, 0};
  PlayerStats c{"carol", 3, 0, 0, 0, 0};
  PlayerStats d{"dave", 0, 9, 0, 0, 0};
  CHECK(Points(a) == 40 + 5 + 2 - 10);
  CHECK(Points(d) == 0);  // never negative
  CHECK(Kdr(a) == "2.00");
  CHECK(Kdr(c) == "3.00");  // no deaths: the kill count

  std::map<uint64_t, PlayerStats> all = {{1, a}, {2, b}, {3, c}, {4, d}};
  auto r = Ranking(all, 10);
  CHECK(r.size() == 2);
  CHECK(r[0].steamid64 == 1);  // alice 37 points, bob 35
  CHECK(RankOf(r, 1) == 1 && RankOf(r, 2) == 2);
  CHECK(RankOf(r, 3) == 0);  // under min kills
  CHECK(Ranking(all, 0).size() == 4);

  // Round trip.
  const std::string text = Serialize(all);
  std::map<uint64_t, PlayerStats> back;
  CHECK(Deserialize(text, &back));
  CHECK(back.size() == 4 && back[1].name == "alice" && back[1].headshots == 5 && back[4].deaths == 9);
  CHECK(!Deserialize("not json", &back));

  // Names.
  bool amb = false;
  std::vector<std::pair<int, std::string>> names = {{0, "Simpert"}, {1, "Sim"}, {2, "Bob"}};
  CHECK(FindByName(names, "sim", &amb) == 1);  // exact beats substring
  CHECK(FindByName(names, "bo", &amb) == 2);
  CHECK(FindByName(names, "s", &amb) == -1 && amb);
  CHECK(FindByName(names, "zed", &amb) == -1 && !amb);

  if (g_failures) std::fprintf(stderr, "%d failure(s)\n", g_failures);
  else std::printf("community_rules: ok\n");
  return g_failures ? 1 : 0;
}

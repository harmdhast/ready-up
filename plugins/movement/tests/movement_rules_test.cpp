// Offline tests for plugins/movement/movement_rules.h. ctest `movement_rules`.
#include "movement_rules.h"

#include <cstdio>

using namespace movement;

static int g_failures = 0;

#define CHECK(cond)                                                                 \
  do {                                                                              \
    if (!(cond)) {                                                                  \
      std::fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #cond); \
      ++g_failures;                                                                 \
    }                                                                               \
  } while (0)

int main() {
  CHECK(ModeForMap("surf_utopia_njv") == Mode::Surf);
  CHECK(ModeForMap("workshop/3070/surf_mesa") == Mode::Surf);
  CHECK(ModeForMap("bhop_arcane") == Mode::Bhop);
  CHECK(ModeForMap("kz_grotto") == Mode::Kz);
  CHECK(ModeForMap("de_mirage") == Mode::Off);
  bool ok = false;
  CHECK(ParseMode("KZ", &ok) == Mode::Kz && ok);
  CHECK(ParseMode("nope", &ok) == Mode::Off && !ok);
  CHECK(!ModeCvars(Mode::Surf).empty() && ModeCvars(Mode::Off).empty());

  CHECK(ClassifyZone("map_start") == ZoneKind::Start);
  CHECK(ClassifyZone("Timer_EndZone") == ZoneKind::End);
  CHECK(ClassifyZone("climb_startbutton") == ZoneKind::Start);
  CHECK(ClassifyZone("s2_start") == ZoneKind::None);
  CHECK(ClassifyZone("") == ZoneKind::None);
  int course = -1;
  CHECK(ClassifyZone("map_start", &course) == ZoneKind::Start && course == 0);
  CHECK(ClassifyZone("b1_start", &course) == ZoneKind::Start && course == 1);
  CHECK(ClassifyZone("B2_End", &course) == ZoneKind::End && course == 2);
  CHECK(ClassifyZone("bonus_start", &course) == ZoneKind::Start && course == 1);
  CHECK(ClassifyZone("bonus3_endzone", &course) == ZoneKind::End && course == 3);
  CHECK(ClassifyZone("timer_bonus1_startzone", &course) == ZoneKind::Start && course == 1);
  CHECK(ClassifyZone("b1_stage2", &course) == ZoneKind::None);
  CHECK(ClassifyZone("button_1", &course) == ZoneKind::None);
  CHECK(CourseKey("surf", 0) == "surf" && CourseKey("surf", 2) == "surf.b2");
  CHECK(ParseCourse("") == 0 && ParseCourse("b2") == 2 && ParseCourse("bonus1") == 1 && ParseCourse("3") == 3);
  CHECK(ParseCourse("x") == -1 && ParseCourse("b") == -1);
  CHECK(ParseFinishChat("SELF", &ok) == FinishChat::Self && ok);
  CHECK(ParseFinishChat("nope", &ok) == FinishChat::Records && !ok);
  CHECK(FinishToAll(FinishChat::Records, true) && !FinishToAll(FinishChat::Records, false));
  CHECK(!FinishToAll(FinishChat::Self, true) && FinishToAll(FinishChat::All, false));
  {
    const std::string j = RecordPayload("surf_mesa", "surf", 1, 76561198000000001ull, "Ann", 62345, 0, 1790000000);
    CHECK(j.find("\"steamid64\":\"76561198000000001\"") != std::string::npos);
    CHECK(j.find("\"course\":1") != std::string::npos && j.find("\"time_ms\":62345") != std::string::npos);
  }

  Box b{{0, 0, 0}, {100, 100, 50}};
  CHECK(Inside(b, {50, 50, 10}, 0));
  CHECK(!Inside(b, {150, 50, 10}, 0));
  CHECK(Inside(b, {101, 50, 10}, 2));

  CHECK(FormatTime(62345) == "1:02.345");
  CHECK(FormatTime(12345) == "12.345 s");

  Records r;
  auto f = AddRun(&r, "surf_a", "surf", 1, "alice", 60000, 100);
  CHECK(f.personalBest && f.serverRecord && f.rank == 1 && f.previousMs == 0);
  f = AddRun(&r, "surf_a", "surf", 2, "bob", 55000, 200);
  CHECK(f.serverRecord && f.rank == 1);
  f = AddRun(&r, "surf_a", "surf", 1, "alice", 58000, 300);
  CHECK(f.personalBest && !f.serverRecord && f.rank == 2 && f.previousMs == 60000);
  f = AddRun(&r, "surf_a", "surf", 1, "alice", 70000, 400);
  CHECK(!f.personalBest && f.previousMs == 58000);
  auto top = Top(r, "surf_a", "surf", 10);
  CHECK(top.size() == 2 && top[0].steamid64 == 2 && top[1].rec.ms == 58000);

  Records back;
  CHECK(Deserialize(Serialize(r), &back));
  CHECK(Top(back, "surf_a", "surf", 10).size() == 2);

  if (g_failures) std::fprintf(stderr, "%d failure(s)\n", g_failures);
  else std::printf("movement_rules: ok\n");
  return g_failures ? 1 : 0;
}

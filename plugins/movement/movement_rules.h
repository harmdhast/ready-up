// readyup-movement: the rules (pure, no engine; ctest `movement_rules`). See movement_plugin.cpp.
#pragma once

#include <cstdint>
#include <map>
#include <string>
#include <vector>

namespace movement {

enum class Mode { Off, Surf, Bhop, Kz };

const char* ModeName(Mode m);
// "auto": from the map name (surf_ / bhop_ / kz_, xc_, bkz_, kzpro_); else the named mode.
Mode ParseMode(const std::string& s, bool* ok);
Mode ModeForMap(const std::string& map);

// Who sees a finished run in chat (movement.cfg finish_chat).
enum class FinishChat { Records, Self, All };
// "records" / "self" / "all"; anything else = Records and *ok = false.
FinishChat ParseFinishChat(const std::string& s, bool* ok);
// Whether everyone sees this finish (else only the runner).
bool FinishToAll(FinishChat c, bool serverRecord);

// The cvars a mode sets (applied on every map start of that mode).
std::vector<std::string> ModeCvars(Mode m);

// A trigger or button that marks a run's start or end, from its targetname.
enum class ZoneKind { None, Start, End };
// Start: map_start, timer_startzone, start_zone, zone_start, s1_start, climb_startbutton,
// startzone, ...; End: map_end, timer_endzone, end_zone, climb_endbutton, endzone, ... These are
// the map run (*course = 0). Bonus courses: b1_start / b1_end, bonus1_start / bonus1_end,
// bonus_start / bonus_end (= 1), timer_bonus2_startzone, ... (*course = N, 1..99). Stage triggers
// (s2_start, ...) are not a run: None.
ZoneKind ClassifyZone(const std::string& targetname, int* course = nullptr);

// The records key for a mode and course: "surf" for the map run, "surf.b1" for bonus 1.
std::string CourseKey(const std::string& mode, int course);
// "map" / "bonus 1".
std::string CourseName(int course);
// A player's `.wr b2` / `.wr 2` / `.wr bonus2` argument: the course (0 = the map run), -1 = not a course.
int ParseCourse(const std::string& arg);

struct Vec3 {
  float x = 0, y = 0, z = 0;
};
struct Box {
  Vec3 mins, maxs;  // world space
};
// A player standing on a zone's floor counts: the box is grown by `pad` (units) on every side.
bool Inside(const Box& b, const Vec3& p, float pad);
Vec3 Center(const Box& b);

// "1:02.345" / "12.345".
std::string FormatTime(int64_t ms);

struct Record {
  std::string name;
  int64_t ms = 0;
  int64_t date = 0;  // unix seconds
};
// records[map][mode][steamid64]
using Records = std::map<std::string, std::map<std::string, std::map<uint64_t, Record>>>;

struct Finish {
  bool personalBest = false;
  bool serverRecord = false;
  int64_t previousMs = 0;  // the player's old best, 0 = first finish
  int rank = 0;            // 1-based place on the map after this run
};
// A finished run: keeps the best time per player. `now` is unix seconds.
Finish AddRun(Records* r, const std::string& map, const std::string& mode, uint64_t steamid64, const std::string& name,
              int64_t ms, int64_t now);

struct Ranked {
  uint64_t steamid64 = 0;
  Record rec;
};
std::vector<Ranked> Top(const Records& r, const std::string& map, const std::string& mode, size_t n);

// The `movement.record` fleet payload (protocol/v1/messages/movement.record.json) for a finished run.
std::string RecordPayload(const std::string& map, const std::string& mode, int course, uint64_t steamid64,
                          const std::string& name, int64_t ms, int teleports, int64_t finishedAt);

std::string Serialize(const Records& r);
bool Deserialize(const std::string& text, Records* out);

}  // namespace movement

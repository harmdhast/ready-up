#include "movement_rules.h"

#include "readyup/status_snapshot.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>

namespace movement {

using readyup::status::Json;

namespace {
std::string Lower(std::string s) {
  for (auto& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  return s;
}
bool StartsWith(const std::string& s, const char* p) { return s.rfind(p, 0) == 0; }
}  // namespace

const char* ModeName(Mode m) {
  switch (m) {
    case Mode::Surf: return "surf";
    case Mode::Bhop: return "bhop";
    case Mode::Kz: return "kz";
    default: return "off";
  }
}

Mode ParseMode(const std::string& s, bool* ok) {
  const std::string v = Lower(s);
  if (ok) *ok = true;
  if (v == "surf") return Mode::Surf;
  if (v == "bhop") return Mode::Bhop;
  if (v == "kz" || v == "climb") return Mode::Kz;
  if (v == "off" || v == "0") return Mode::Off;
  if (ok) *ok = false;
  return Mode::Off;
}

Mode ModeForMap(const std::string& map) {
  // Workshop maps arrive as their plain name; a path ("workshop/123/surf_x") counts by its last part.
  std::string m = Lower(map);
  const size_t slash = m.rfind('/');
  if (slash != std::string::npos) m = m.substr(slash + 1);
  if (StartsWith(m, "surf_")) return Mode::Surf;
  if (StartsWith(m, "bhop_")) return Mode::Bhop;
  if (StartsWith(m, "kz_") || StartsWith(m, "xc_") || StartsWith(m, "bkz_") || StartsWith(m, "kzpro_") ||
      StartsWith(m, "skz_") || StartsWith(m, "vnl_")) {
    return Mode::Kz;
  }
  return Mode::Off;
}

FinishChat ParseFinishChat(const std::string& s, bool* ok) {
  const std::string v = Lower(s);
  *ok = true;
  if (v == "self") return FinishChat::Self;
  if (v == "all") return FinishChat::All;
  if (v != "records") *ok = false;
  return FinishChat::Records;
}

bool FinishToAll(FinishChat c, bool serverRecord) {
  return c == FinishChat::All || (c == FinishChat::Records && serverRecord);
}

std::vector<std::string> ModeCvars(Mode m) {
  // Shared: nobody dies of the map, rounds never end, no stamina, players pass through each other.
  std::vector<std::string> c = {
      "mp_ignore_round_win_conditions 1", "mp_respawn_on_death_ct 1", "mp_respawn_on_death_t 1",
      "mp_roundtime 60", "mp_roundtime_defuse 60", "mp_roundtime_hostage 60", "mp_freezetime 0",
      "mp_warmup_end", "sv_falldamage_scale 0", "sv_staminajumpcost 0", "sv_staminalandcost 0",
      "sv_staminamax 0", "mp_solid_teammates 0", "mp_friendlyfire 0", "mp_autokick 0",
      "mp_death_drop_gun 0", "sv_enablebunnyhopping 1", "sv_maxvelocity 3500", "mp_free_armor 1",
      "mp_buy_anywhere 1", "mp_buytime 99999", "bot_quota 0", "bot_kick",
      // A player who joins a team mid-round spawns (the round is an hour long), not next round.
      "mp_join_grace_time 3600",
  };
  switch (m) {
    case Mode::Surf:
      c.insert(c.end(), {"sv_airaccelerate 150", "sv_accelerate 10", "sv_friction 4", "sv_autobunnyhopping 1"});
      break;
    case Mode::Bhop:
      c.insert(c.end(), {"sv_airaccelerate 1000", "sv_accelerate 10", "sv_friction 4", "sv_autobunnyhopping 1"});
      break;
    case Mode::Kz:
      c.insert(c.end(), {"sv_airaccelerate 100", "sv_accelerate 6.5", "sv_friction 5.2", "sv_autobunnyhopping 0"});
      break;
    default:
      return {};
  }
  return c;
}

namespace {

// "b1_start" / "bonus1_start" / "bonus_start" / "timer_bonus1_startzone": the bonus number and the
// rest after it ("start", "startzone", ...); 0 when the name is no bonus.
int BonusPrefix(const std::string& n, std::string* rest) {
  std::string s = n;
  if (s.rfind("timer_", 0) == 0) s = s.substr(6);
  size_t i = 0;
  if (s.rfind("bonus", 0) == 0) i = 5;
  else if (s.size() > 1 && s[0] == 'b' && std::isdigit(static_cast<unsigned char>(s[1]))) i = 1;
  else return 0;
  int num = 0;
  size_t j = i;
  while (j < s.size() && std::isdigit(static_cast<unsigned char>(s[j])) && j - i < 2) num = num * 10 + (s[j++] - '0');
  if (j == i) num = 1;  // bonus_start
  if (j >= s.size() || s[j] != '_' || num < 1) return 0;
  *rest = s.substr(j + 1);
  return num;
}

}  // namespace

ZoneKind ClassifyZone(const std::string& targetname, int* course) {
  if (course) *course = 0;
  const std::string n = Lower(targetname);
  if (n.empty()) return ZoneKind::None;
  static const char* const kStart[] = {"map_start", "timer_startzone", "start_zone", "zone_start", "startzone",
                                       "s1_start", "climb_startbutton", "climb_startzone", "timer_start"};
  static const char* const kEnd[] = {"map_end", "timer_endzone", "end_zone", "zone_end", "endzone",
                                     "climb_endbutton", "climb_endzone", "timer_end"};
  for (const char* s : kStart) {
    if (n == s) return ZoneKind::Start;
  }
  for (const char* s : kEnd) {
    if (n == s) return ZoneKind::End;
  }
  std::string rest;
  const int bonus = BonusPrefix(n, &rest);
  if (bonus > 0) {
    ZoneKind k = ZoneKind::None;
    if (rest == "start" || rest == "startzone" || rest == "start_zone" || rest == "zone_start") k = ZoneKind::Start;
    if (rest == "end" || rest == "endzone" || rest == "end_zone" || rest == "zone_end") k = ZoneKind::End;
    if (k != ZoneKind::None && course) *course = bonus;
    return k;
  }
  return ZoneKind::None;
}

std::string CourseKey(const std::string& mode, int course) {
  return course > 0 ? mode + ".b" + std::to_string(course) : mode;
}

std::string CourseName(int course) { return course > 0 ? "bonus " + std::to_string(course) : "map"; }

int ParseCourse(const std::string& arg) {
  std::string a = Lower(arg);
  if (a.empty() || a == "map" || a == "main") return 0;
  if (a.rfind("bonus", 0) == 0) a = a.substr(5);
  else if (a[0] == 'b') a = a.substr(1);
  if (a.empty() || a.size() > 2) return -1;
  for (char c : a) {
    if (!std::isdigit(static_cast<unsigned char>(c))) return -1;
  }
  const int n = std::stoi(a);
  return n >= 1 ? n : -1;
}

bool Inside(const Box& b, const Vec3& p, float pad) {
  return p.x >= b.mins.x - pad && p.x <= b.maxs.x + pad && p.y >= b.mins.y - pad && p.y <= b.maxs.y + pad &&
         p.z >= b.mins.z - pad && p.z <= b.maxs.z + pad;
}

Vec3 Center(const Box& b) {
  return {(b.mins.x + b.maxs.x) / 2, (b.mins.y + b.maxs.y) / 2, b.mins.z + 2.0f};
}

std::string FormatTime(int64_t ms) {
  if (ms < 0) ms = 0;
  const int64_t m = ms / 60000, s = (ms / 1000) % 60, r = ms % 1000;
  char buf[32];
  if (m > 0) std::snprintf(buf, sizeof(buf), "%lld:%02lld.%03lld", static_cast<long long>(m), static_cast<long long>(s), static_cast<long long>(r));
  else std::snprintf(buf, sizeof(buf), "%lld.%03lld s", static_cast<long long>(s), static_cast<long long>(r));
  return buf;
}

std::vector<Ranked> Top(const Records& r, const std::string& map, const std::string& mode, size_t n) {
  std::vector<Ranked> out;
  auto m = r.find(map);
  if (m == r.end()) return out;
  auto md = m->second.find(mode);
  if (md == m->second.end()) return out;
  for (const auto& kv : md->second) out.push_back({kv.first, kv.second});
  std::sort(out.begin(), out.end(), [](const Ranked& a, const Ranked& b) {
    if (a.rec.ms != b.rec.ms) return a.rec.ms < b.rec.ms;
    if (a.rec.date != b.rec.date) return a.rec.date < b.rec.date;  // the first to set a time keeps the place
    return a.steamid64 < b.steamid64;
  });
  if (out.size() > n) out.resize(n);
  return out;
}

Finish AddRun(Records* r, const std::string& map, const std::string& mode, uint64_t steamid64, const std::string& name,
              int64_t ms, int64_t now) {
  Finish f;
  auto& times = (*r)[map][mode];
  const auto best = Top(*r, map, mode, 1);
  const int64_t serverBest = best.empty() ? 0 : best[0].rec.ms;
  auto it = times.find(steamid64);
  if (it != times.end()) f.previousMs = it->second.ms;
  if (it == times.end() || ms < it->second.ms) {
    f.personalBest = true;
    times[steamid64] = Record{name, ms, now};
  } else {
    it->second.name = name;
  }
  f.serverRecord = f.personalBest && (serverBest == 0 || ms < serverBest);
  const auto all = Top(*r, map, mode, times.size());
  for (size_t i = 0; i < all.size(); ++i) {
    if (all[i].steamid64 == steamid64) f.rank = static_cast<int>(i) + 1;
  }
  return f;
}

std::string RecordPayload(const std::string& map, const std::string& mode, int course, uint64_t steamid64,
                          const std::string& name, int64_t ms, int teleports, int64_t finishedAt) {
  Json p = Json::Object();
  p["map"] = map;
  p["mode"] = mode;
  p["course"] = course;
  p["steamid64"] = std::to_string(steamid64);
  std::string n = name;
  if (n.size() > 64) {
    size_t cut = 64;
    while (cut > 0 && (static_cast<unsigned char>(n[cut]) & 0xC0) == 0x80) --cut;  // whole UTF-8 characters
    n.resize(cut);
  }
  p["name"] = n;
  p["time_ms"] = ms;
  p["teleports"] = teleports;
  p["finished_at"] = finishedAt;
  return p.Dump();
}

std::string Serialize(const Records& r) {
  Json root = Json::Object();
  Json& maps = root["maps"];
  maps = Json::Object();
  for (const auto& m : r) {
    Json modes = Json::Object();
    for (const auto& md : m.second) {
      Json players = Json::Object();
      for (const auto& p : md.second) {
        Json rec = Json::Object();
        rec["name"] = p.second.name;
        rec["ms"] = static_cast<long long>(p.second.ms);
        rec["date"] = static_cast<long long>(p.second.date);
        players[std::to_string(p.first)] = rec;
      }
      modes[md.first] = players;
    }
    maps[m.first] = modes;
  }
  return root.Dump();
}

bool Deserialize(const std::string& text, Records* out) {
  Json root;
  if (!Json::Parse(text, &root) || !root.IsObject()) return false;
  const Json* maps = root.Find("maps");
  if (!maps || !maps->IsObject()) return false;
  out->clear();
  for (const auto& m : maps->Members()) {
    if (!m.second.IsObject()) continue;
    for (const auto& md : m.second.Members()) {
      if (!md.second.IsObject()) continue;
      for (const auto& p : md.second.Members()) {
        char* end = nullptr;
        const unsigned long long id = std::strtoull(p.first.c_str(), &end, 10);
        if (!end || *end != '\0' || id == 0 || !p.second.IsObject()) continue;
        Record rec;
        if (const Json* n = p.second.Find("name")) rec.name = n->AsString();
        if (const Json* v = p.second.Find("ms")) rec.ms = v->AsInt();
        if (const Json* v = p.second.Find("date")) rec.date = v->AsInt();
        if (rec.ms > 0) (*out)[m.first][md.first][id] = rec;
      }
    }
  }
  return true;
}

}  // namespace movement

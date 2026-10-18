// Round restore / crash recovery pure logic (see round_restore_rules.h).
#include "readyup/round_restore_rules.h"

#include "readyup/match_end.h"
#include "readyup/status_snapshot.h"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <map>
#include <utility>

namespace readyup::restore {
namespace {

using status::Json;

bool AllDigits(const std::string& s, size_t from, size_t to) {
  if (from >= to) return false;
  for (size_t i = from; i < to; ++i) {
    if (!std::isdigit(static_cast<unsigned char>(s[i]))) return false;
  }
  return true;
}

int Int(const Json* o, const char* k, int def = 0) {
  const Json* v = o ? o->Find(k) : nullptr;
  return v ? static_cast<int>(v->AsInt()) : def;
}
std::string Str(const Json* o, const char* k) {
  const Json* v = o ? o->Find(k) : nullptr;
  return v ? v->AsString() : std::string();
}

}  // namespace

std::string BackupPrefix(uint64_t matchid, int mapNumber) {
  return "readyup_backup_" + std::to_string(static_cast<unsigned long long>(matchid)) + "_map" +
         std::to_string(std::max(1, mapNumber)) + "_";
}

bool ParseBackupFileName(const std::string& file, uint64_t matchid, BackupInfo* out) {
  const std::string head = "readyup_backup_" + std::to_string(static_cast<unsigned long long>(matchid)) + "_map";
  if (file.size() <= head.size() + 4 || file.compare(0, head.size(), head) != 0) return false;
  if (file.compare(file.size() - 4, 4, ".txt") != 0) return false;
  const size_t roundTag = file.find("_round", head.size());
  if (roundTag == std::string::npos) return false;
  // CS2 appends "_round<NN>.txt" to mp_backup_round_file, and BackupPrefix already ends with '_':
  // current builds write "..._map1__round03.txt"; "..._map1_round03.txt" is accepted too.
  size_t mapEnd = roundTag;
  if (mapEnd > head.size() && file[mapEnd - 1] == '_') --mapEnd;
  if (!AllDigits(file, head.size(), mapEnd) || mapEnd - head.size() > 3) return false;
  const size_t roundStart = roundTag + 6;
  const size_t roundEnd = file.size() - 4;
  if (!AllDigits(file, roundStart, roundEnd) || roundEnd - roundStart > 3) return false;
  const int map = std::atoi(file.substr(head.size(), mapEnd - head.size()).c_str());
  const int played = std::atoi(file.substr(roundStart, roundEnd - roundStart).c_str());
  if (map < 1) return false;
  if (out) {
    out->file = file;
    out->map_number = map;
    out->round = played + 1;
  }
  return true;
}

std::vector<BackupInfo> SortBackups(std::vector<BackupInfo> in) {
  std::map<std::pair<int, int>, BackupInfo> best;
  for (auto& b : in) {
    const auto key = std::make_pair(b.map_number, b.round);
    auto it = best.find(key);
    if (it == best.end() || b.mtime > it->second.mtime) best[key] = std::move(b);
  }
  std::vector<BackupInfo> out;
  out.reserve(best.size());
  for (auto& kv : best) out.push_back(std::move(kv.second));
  return out;
}

const BackupInfo* LatestForMap(const std::vector<BackupInfo>& sorted, int mapNumber) {
  const BackupInfo* best = nullptr;
  for (const auto& b : sorted) {
    if (b.map_number == mapNumber && (!best || b.round > best->round)) best = &b;
  }
  return best;
}

std::string BackupListLine(const BackupInfo& b, int score1, int score2) {
  std::string l = "map " + std::to_string(b.map_number) + " round " + std::to_string(b.round);
  if (score1 >= 0 && score2 >= 0) l += " (" + std::to_string(score1) + "-" + std::to_string(score2) + ")";
  return l + " " + b.file;
}

bool ScoreAtRoundStart(const stats::MapStats& m, int round, int* team1, int* team2) {
  if (round == 1) {
    *team1 = *team2 = 0;
    return true;
  }
  for (const auto& r : m.rounds) {
    if (r.round_number == round - 1) {
      *team1 = r.team1_score;
      *team2 = r.team2_score;
      return true;
    }
  }
  return false;
}

int ParseRestoreRound(const std::string& arg, std::string* err) {
  if (arg.empty() || arg.size() > 3 || !AllDigits(arg, 0, arg.size()) || std::atoi(arg.c_str()) < 1) {
    if (err) *err = "usage: .restore <round> (the round to play again, 1 = the first; .ru match backups lists them)";
    return -1;
  }
  return std::atoi(arg.c_str());
}

const char* BackupFlagHint() {
  return "CS2 writes round backups only when started with -allow_round_backup_files (csm 1.27.4+ adds it; "
         "restart the server)";
}

std::string ExplainRestoreError(const std::string& err) {
  if (err.rfind("no backup ", 0) != 0) return err;
  return err + ". " + BackupFlagHint();
}

bool CmdlineHasArg(const std::string& raw, const std::string& arg) {
  size_t i = 0;
  while (i <= raw.size()) {
    size_t e = raw.find('\0', i);
    if (e == std::string::npos) e = raw.size();
    if (raw.compare(i, e - i, arg) == 0 && e - i == arg.size()) return true;
    i = e + 1;
  }
  return false;
}

std::string RestoreRefusal(const RestoreCheck& c) {
  if (!c.match_loaded) return "no match loaded.";
  if (!c.live) return "a restore needs a live map.";
  if (c.round < 1) return "the round must be 1 or more.";
  const int current = std::max(0, c.rounds_played) + 1;
  if (c.round > current) {
    return "round " + std::to_string(c.round) + " has not been played yet (this is round " + std::to_string(current) +
           ").";
  }
  return {};
}

int PauseAfterRestoreFor(int matchValue, int consoleValue, int cfgValue) {
  if (matchValue >= 0) return matchValue ? 1 : 0;
  if (consoleValue >= 0) return consoleValue ? 1 : 0;
  if (cfgValue >= 0) return cfgValue ? 1 : 0;
  return 1;
}

std::string ProgressToJson(const Progress& p) {
  Json j = Json::Object();
  j["v"] = 1;
  j["map_number"] = p.map_number;
  j["phase"] = p.phase;
  j["series_team1"] = p.series_team1;
  j["series_team2"] = p.series_team2;
  if (!p.stats_json.empty()) j["stats"] = p.stats_json;
  if (p.have_events) {
    Json e = Json::Object();
    e["round"] = p.events.roundNumber;
    e["last_map_number"] = p.events.lastMapNumber;
    e["swaps"] = p.events.swapCount;
    e["last_half_start_total"] = p.events.lastHalfStartTotal;
    e["last_overtime"] = p.events.lastOvertimeNumber;
    Json players = Json::Array();
    for (const auto& t : p.events.players) {
      Json x = Json::Object();
      x["id"] = std::to_string(static_cast<unsigned long long>(t.steamid64));
      x["name"] = t.name;
      x["team"] = t.team;
      x["k"] = t.kills;
      x["d"] = t.deaths;
      x["a"] = t.assists;
      x["hs"] = t.headshot_kills;
      x["dmg"] = t.damage;
      players.Push(std::move(x));
    }
    e["players"] = std::move(players);
    j["events"] = std::move(e);
  }
  return j.Dump();
}

bool ProgressFromJson(const std::string& json, Progress* out) {
  Json j;
  if (json.empty() || !Json::Parse(json, &j) || !j.IsObject() || Int(&j, "v") != 1) return false;
  Progress p;
  p.map_number = std::max(1, Int(&j, "map_number", 1));
  p.phase = Str(&j, "phase");
  if (p.phase != "warmup" && p.phase != "live" && p.phase != "map_over") return false;
  p.series_team1 = std::max(0, Int(&j, "series_team1"));
  p.series_team2 = std::max(0, Int(&j, "series_team2"));
  p.stats_json = Str(&j, "stats");
  if (const Json* e = j.Find("events"); e && e->IsObject()) {
    p.have_events = true;
    p.events.roundNumber = Int(e, "round");
    p.events.lastMapNumber = Int(e, "last_map_number");
    p.events.swapCount = Int(e, "swaps");
    p.events.lastHalfStartTotal = Int(e, "last_half_start_total", -1);
    p.events.lastOvertimeNumber = Int(e, "last_overtime");
    if (const Json* ps = e->Find("players")) {
      for (const auto& x : ps->Items()) {
        MatchEventsState::Totals t;
        t.steamid64 = std::strtoull(Str(&x, "id").c_str(), nullptr, 10);
        t.name = Str(&x, "name");
        t.team = Int(&x, "team");
        t.kills = Int(&x, "k");
        t.deaths = Int(&x, "d");
        t.assists = Int(&x, "a");
        t.headshot_kills = Int(&x, "hs");
        t.damage = Int(&x, "dmg");
        p.events.players.push_back(std::move(t));
      }
    }
  }
  if (out) *out = std::move(p);
  return true;
}

RecoveryPlan PlanRecovery(const Progress* p, bool legacyLiveFlag, const std::vector<std::string>& maplist, int numMaps,
                          bool clinchSeries) {
  RecoveryPlan plan;
  const int total = numMaps > 0 ? numMaps : static_cast<int>(maplist.size());
  auto at = [&](int mapNumber, RecoveryPlan::Kind kind, std::string why) {
    if (mapNumber < 1 || static_cast<size_t>(mapNumber) > maplist.size() || mapNumber > std::max(1, total)) {
      plan.kind = RecoveryPlan::Kind::None;
      plan.why = "map " + std::to_string(mapNumber) + " is not in the map list";
      return plan;
    }
    plan.kind = kind;
    plan.map_number = mapNumber;
    plan.map_entry = maplist[static_cast<size_t>(mapNumber - 1)];
    plan.why = std::move(why);
    return plan;
  };
  if (!p) {
    return at(1, legacyLiveFlag ? RecoveryPlan::Kind::Live : RecoveryPlan::Kind::Warmup, "no progress record");
  }
  if (p->phase == "map_over") {
    const int remaining = RemainingMaps(total, static_cast<int>(maplist.size()), p->map_number);
    if (IsSeriesOver(total, remaining, p->series_team1, p->series_team2, clinchSeries)) {
      plan.kind = RecoveryPlan::Kind::None;
      plan.why = "the series was over";
      return plan;
    }
    return at(p->map_number + 1, RecoveryPlan::Kind::Warmup, "map " + std::to_string(p->map_number) + " was over");
  }
  if (p->phase == "live") return at(p->map_number, RecoveryPlan::Kind::Live, "the map was live");
  return at(p->map_number, RecoveryPlan::Kind::Warmup, "the map was in warmup");
}

}  // namespace readyup::restore

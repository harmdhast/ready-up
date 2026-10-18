// Loadout cache for readyup-skins (docs/json-contract.md). No database (docs/FLEET.md D13):
//
//   standalone  loadouts.json in the plugin data dir (csgo/readyup/plugins/skins/), written by
//               whatever manages skins (a web tool, scripts/seed-dev-skins.py, the Postgres
//               migration); re-read when it changes. StatTrak counters go to stattrak.json next
//               to it, so the plugin never rewrites the file someone else owns.
//   fleet mode  skins.loadout / skins.invalidate from the platform over readyup.fleet.v1 (D6),
//               cached per player in memory; StatTrak kills go back as skins.stattrak.
//
// File I/O runs on one worker thread that the plugin starts in load and joins in unload, so a
// reload never leaves a thread running in unmapped code. Lookups from the game thread only read
// the in-memory cache.
#include "skins.h"

#include "readyup/fleet_iface.h"
#include "readyup/json_store.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdlib>
#include <deque>
#include <functional>
#include <map>
#include <mutex>
#include <thread>
#include <unordered_map>
#include <utility>
#include <vector>

namespace skins {
namespace {

using Clock = std::chrono::steady_clock;
using readyup::json_store::Json;
namespace store = readyup::json_store;
constexpr std::chrono::seconds kTtl{45};
constexpr int kLoadoutsVersion = 1;
constexpr int kStatTrakVersion = 1;

struct CacheEntry {
  Loadout loadout;
  Clock::time_point loaded_at{};
  bool loading = false;
};

std::mutex g_mu;  // guards g_cache, g_fleet, g_fleetRev
std::unordered_map<uint64_t, CacheEntry> g_cache;
std::unordered_map<uint64_t, Loadout> g_fleet;  // fleet mode: last skins.loadout per player
std::unordered_map<uint64_t, int64_t> g_fleetRev;
std::unordered_map<uint64_t, std::string> g_fleetItems;  // the last items received (to tell a change)
std::atomic<bool> g_fleetMode{false};

// ---- worker ---------------------------------------------------------------------------
std::mutex g_qmu;
std::condition_variable g_qcv;
std::deque<std::function<void()>> g_jobs;
bool g_stop = false;
std::thread g_worker;
bool g_running = false;
std::string g_status = "not started";

// Worker thread only.
std::string g_loadoutsPath, g_stattrakPath;
std::unordered_map<uint64_t, Loadout> g_file;  // loadouts.json
int64_t g_fileMtime = -1;
std::map<std::string, int> g_counters;  // stattrak.json: "<steamid64>/<team>/<defindex>" -> kills
bool g_countersLoaded = false;
bool g_countersDirty = false;
std::atomic<bool> g_savePending{false};

// Fleet StatTrak increments waiting for the next flush ("<steamid64>/<defindex>" -> kills).
std::mutex g_stMu;
std::map<std::pair<uint64_t, int>, int> g_pendingKills;

void Post(std::function<void()> job) {
  {
    std::lock_guard<std::mutex> lk(g_qmu);
    if (!g_running || g_stop || g_jobs.size() >= 4096) return;
    g_jobs.push_back(std::move(job));
  }
  g_qcv.notify_one();
}

void WorkerMain() {
  for (;;) {
    std::function<void()> job;
    {
      std::unique_lock<std::mutex> lk(g_qmu);
      g_qcv.wait(lk, [] { return g_stop || !g_jobs.empty(); });
      if (g_jobs.empty()) return;  // stopping; queued jobs ran first (local file I/O only, no network)
      job = std::move(g_jobs.front());
      g_jobs.pop_front();
    }
    try {
      job();
    } catch (...) {
      Log(RU_LOG_ERROR, "loadout worker: job threw; ignored");
    }
  }
}

// ---- JSON helpers ----------------------------------------------------------------------
int IntOf(const Json& o, const char* key, int def = 0) {
  const Json* v = o.Find(key);
  if (!v) return def;
  if (v->type() == Json::Type::Int || v->type() == Json::Type::Double) return static_cast<int>(v->AsInt());
  if (v->type() == Json::Type::String) return std::atoi(v->AsString().c_str());
  return def;
}
double NumOf(const Json& o, const char* key, double def) {
  const Json* v = o.Find(key);
  if (!v) return def;
  if (v->type() == Json::Type::Int || v->type() == Json::Type::Double) return v->AsDouble();
  if (v->type() == Json::Type::String) return std::strtod(v->AsString().c_str(), nullptr);
  return def;
}
std::string StrOf(const Json& o, const char* key) {
  const Json* v = o.Find(key);
  return v && v->type() == Json::Type::String ? v->AsString() : std::string();
}
bool BoolOf(const Json& o, const char* key) {
  const Json* v = o.Find(key);
  if (!v) return false;
  if (v->type() == Json::Type::Bool || v->type() == Json::Type::Int) return v->AsInt() != 0;
  return v->type() == Json::Type::String && (v->AsString() == "true" || v->AsString() == "1");
}
const std::vector<Json>& ArrOf(const Json& o, const char* key) {
  static const std::vector<Json> kEmpty;
  const Json* v = o.Find(key);
  return v && v->type() == Json::Type::Array ? v->Items() : kEmpty;
}
uint64_t SteamOf(const Json* v) {
  if (!v) return 0;
  if (v->type() == Json::Type::String) return std::strtoull(v->AsString().c_str(), nullptr, 10);
  if (v->type() == Json::Type::Int && v->AsInt() > 0) return static_cast<uint64_t>(v->AsInt());
  return 0;
}

std::string CounterKey(uint64_t sid, int team, int defindex) {
  return std::to_string(sid) + "/" + std::to_string(team) + "/" + std::to_string(defindex);
}

// One player's rows in loadouts.json (docs/json-contract.md).
Loadout ParseFilePlayer(const Json& p) {
  Loadout l;
  for (const Json& r : ArrOf(p, "weapon_skins")) {
    WeaponSkinEntry e;
    e.weapon_team = IntOf(r, "weapon_team");
    e.weapon_defindex = IntOf(r, "weapon_defindex");
    e.paint_id = IntOf(r, "paint_id");
    e.wear = static_cast<float>(NumOf(r, "wear", 0.000001));
    e.seed = IntOf(r, "seed");
    e.nametag = StrOf(r, "nametag");
    e.stattrak_enabled = BoolOf(r, "stattrak_enabled");
    e.stattrak_count = IntOf(r, "stattrak_count");
    if (e.weapon_defindex > 0) l.by_team[e.weapon_team].skins_by_defindex[e.weapon_defindex] = e;
  }
  for (const Json& r : ArrOf(p, "weapon_knives")) {
    const std::string cls = StrOf(r, "knife_classname");
    if (!cls.empty()) l.by_team[IntOf(r, "weapon_team")].knife_classname = cls;
  }
  for (const Json& r : ArrOf(p, "weapon_gloves")) {
    const int def = IntOf(r, "glove_defindex");
    if (def > 0) l.by_team[IntOf(r, "weapon_team")].glove_defindex = def;
  }
  if (const Json* a = p.Find("weapon_agents"); a && a->IsObject()) {
    l.agents = WeaponAgentEntry{StrOf(*a, "agent_ct"), StrOf(*a, "agent_t")};
  }
  return l;
}

std::optional<std::string> KnifeDefindexToClassname(int defindex);

// skins.loadout.items (docs/FLEET.md §7.6).
Loadout ParseFleetItems(const Json& items) {
  Loadout l;
  for (const Json& r : ArrOf(items, "paints")) {
    WeaponSkinEntry e;
    e.weapon_team = IntOf(r, "team");
    e.weapon_defindex = IntOf(r, "defindex");
    e.paint_id = IntOf(r, "paint");
    e.wear = static_cast<float>(NumOf(r, "wear", 0.000001));
    e.seed = IntOf(r, "seed");
    e.nametag = StrOf(r, "nametag");
    e.stattrak_enabled = r.Find("stattrak") != nullptr && !r.Find("stattrak")->IsNull();
    e.stattrak_count = IntOf(r, "stattrak");
    if (e.weapon_defindex > 0) l.by_team[e.weapon_team].skins_by_defindex[e.weapon_defindex] = e;
  }
  for (const Json& r : ArrOf(items, "knife")) {
    if (auto cls = KnifeDefindexToClassname(IntOf(r, "defindex"))) l.by_team[IntOf(r, "team")].knife_classname = *cls;
  }
  for (const Json& r : ArrOf(items, "gloves")) {
    const int def = IntOf(r, "defindex");
    if (def > 0) l.by_team[IntOf(r, "team")].glove_defindex = def;
  }
  for (const Json& r : ArrOf(items, "agents")) {
    if (!l.agents) l.agents = WeaponAgentEntry{};
    const int team = IntOf(r, "team");
    if (team == 3) l.agents->agent_ct = StrOf(r, "model");
    if (team == 2) l.agents->agent_t = StrOf(r, "model");
  }
  return l;
}

// ---- files (worker thread) -------------------------------------------------------------
bool LoadDoc(const std::string& path, int version, Json* doc) {
  std::string note;
  switch (store::Load(path, version, doc, &note)) {
    case store::LoadResult::Ok:
      return true;
    case store::LoadResult::Corrupt:
      Log(RU_LOG_WARN, "%s", note.c_str());
      return false;
    case store::LoadResult::Missing:
      return false;
  }
  return false;
}

void ReloadFileIfChanged() {
  const int64_t m = store::MtimeNs(g_loadoutsPath);
  if (m == g_fileMtime) return;
  g_fileMtime = m;
  g_file.clear();
  Json doc;
  if (m != 0 && LoadDoc(g_loadoutsPath, kLoadoutsVersion, &doc)) {
    if (const Json* players = doc.Find("players"); players && players->IsObject()) {
      for (const auto& kv : players->Members()) {
        const uint64_t sid = std::strtoull(kv.first.c_str(), nullptr, 10);
        if (sid != 0 && kv.second.IsObject()) g_file[sid] = ParseFilePlayer(kv.second);
      }
    }
    Log(RU_LOG_INFO, "loadouts: %zu player(s) from %s", g_file.size(), g_loadoutsPath.c_str());
  }
  g_fileMtime = store::MtimeNs(g_loadoutsPath);  // a corrupt file was moved aside
}

void LoadCounters() {
  if (g_countersLoaded) return;
  g_countersLoaded = true;
  Json doc;
  if (!LoadDoc(g_stattrakPath, kStatTrakVersion, &doc)) return;
  if (const Json* c = doc.Find("counters"); c && c->IsObject()) {
    for (const auto& kv : c->Members()) {
      if (kv.second.type() == Json::Type::Int) g_counters[kv.first] = static_cast<int>(kv.second.AsInt());
    }
  }
}

void SaveCounters() {
  Json c = Json::Object();
  for (const auto& kv : g_counters) c[kv.first] = kv.second;
  Json doc = Json::Object();
  doc["counters"] = std::move(c);
  store::FileLock lock(g_stattrakPath);
  std::string err;
  if (!store::Save(g_stattrakPath, doc, kStatTrakVersion, &err)) Log(RU_LOG_WARN, "stattrak: save failed: %s", err.c_str());
}

void ApplyCounters(uint64_t sid, Loadout& l) {
  for (auto& team : l.by_team) {
    for (auto& kv : team.second.skins_by_defindex) {
      auto it = g_counters.find(CounterKey(sid, kv.second.weapon_team, kv.first));
      if (it != g_counters.end()) kv.second.stattrak_count = it->second;
    }
  }
}

void RefreshJob(uint64_t sid) {
  Loadout l;
  bool fleet = g_fleetMode.load();
  if (!fleet) {
    ReloadFileIfChanged();
    LoadCounters();
    auto it = g_file.find(sid);
    if (it != g_file.end()) l = it->second;
    ApplyCounters(sid, l);
  }
  std::lock_guard<std::mutex> lk(g_mu);
  CacheEntry& e = g_cache[sid];
  e.loading = false;
  if (fleet) {
    auto it = g_fleet.find(sid);
    if (it != g_fleet.end()) l = it->second;
  }
  e.loadout = std::move(l);
  e.loaded_at = Clock::now();
}

const TeamLoadout* TeamLocked(const Loadout& l, int team) {
  auto it = l.by_team.find(team);
  return it == l.by_team.end() ? nullptr : &it->second;
}

// Team-specific rows (2=T, 3=CT) win; team 0 ("both") is the per-field fallback.
std::optional<TeamLoadout> FindTeamLocked(const Loadout& l, int weapon_team) {
  const TeamLoadout* specific = TeamLocked(l, weapon_team);
  const TeamLoadout* both = weapon_team != 0 ? TeamLocked(l, 0) : nullptr;
  if (!specific && !both) return std::nullopt;
  TeamLoadout out = both ? *both : TeamLoadout{};
  if (specific) {
    for (const auto& kv : specific->skins_by_defindex) out.skins_by_defindex[kv.first] = kv.second;
    if (specific->knife_classname) out.knife_classname = specific->knife_classname;
    if (specific->glove_defindex) out.glove_defindex = specific->glove_defindex;
  }
  return out;
}

bool IsStale(const CacheEntry& e, Clock::time_point now) {
  return e.loaded_at.time_since_epoch().count() == 0 || (now - e.loaded_at) > kTtl;
}

const std::unordered_map<std::string, int>& Knives() {
  // Item defindexes from items_game.txt (CS2 1.41.8.3). Default knives map to nullopt (no change).
  static const std::unordered_map<std::string, int> kKnives = {
      {"weapon_bayonet", 500},
      {"weapon_knife_css", 503},
      {"weapon_knife_flip", 505},
      {"weapon_knife_gut", 506},
      {"weapon_knife_karambit", 507},
      {"weapon_knife_m9_bayonet", 508},
      {"weapon_knife_tactical", 509},
      {"weapon_knife_falchion", 512},
      {"weapon_knife_survival_bowie", 514},
      {"weapon_knife_butterfly", 515},
      {"weapon_knife_push", 516},
      {"weapon_knife_cord", 517},
      {"weapon_knife_canis", 518},
      {"weapon_knife_ursus", 519},
      {"weapon_knife_gypsy_jackknife", 520},
      {"weapon_knife_outdoor", 521},
      {"weapon_knife_stiletto", 522},
      {"weapon_knife_widowmaker", 523},
      {"weapon_knife_skeleton", 525},
      {"weapon_knife_kukri", 526},
  };
  return kKnives;
}

std::optional<std::string> KnifeDefindexToClassname(int defindex) {
  for (const auto& kv : Knives()) {
    if (kv.second == defindex) return kv.first;
  }
  return std::nullopt;
}

// ---- fleet link (game thread) ----------------------------------------------------------
uint64_t g_fleetInstance = 0;
std::vector<uint64_t> g_handlerIds;
double g_lastFlush = 0;

const ru_fleet_v1* Fleet() {
  if (!g_api) return nullptr;
  auto* f = static_cast<const ru_fleet_v1*>(g_api->get_interface(g_api->self, RU_FLEET_IFACE_NAME, 1));
  if (!f || f->struct_size < offsetof(ru_fleet_v1, add_capability) + sizeof(f->add_capability)) return nullptr;
  return f;
}

Json Payload(const ru_fleet_msg* m) {
  Json p;
  if (!m->payload_json || !Json::Parse(m->payload_json, &p) || !p.IsObject()) return Json::Object();
  return p;
}

void OnFleetMessage(void*, const ru_fleet_msg* m) {
  if (!m || !m->type) return;
  try {
    const std::string type = m->type;
    const Json p = Payload(m);
    const uint64_t sid = SteamOf(p.Find("steamid64"));
    if (sid == 0) return;
    if (type == "skins.loadout") {
      const Json* items = p.Find("items");
      const int64_t rev = p.Find("rev") ? p.Find("rev")->AsInt() : 0;
      Loadout l = items && items->IsObject() ? ParseFleetItems(*items) : Loadout{};
      std::lock_guard<std::mutex> lk(g_mu);
      auto r = g_fleetRev.find(sid);
      if (r != g_fleetRev.end() && rev < r->second) return;  // replay of an older loadout
      // The first loadout of a player (the platform sends it when they join) is not a change: the
      // weapons they spawn with get it anyway. Re-giving on it removed and re-created their weapons
      // right as Ready Up restarted the round for the join, and their client crashed
      // (CopyExistingEntity / OnPreserveEntity: missing client entity, 2026-10-14).
      // The platform stamps every send with a new rev (a rejoin too): compare the items.
      const std::string itemsKey = items ? readyup::json_store::Pretty(*items) : std::string();
      const auto prev = g_fleetItems.find(sid);
      const bool changed = prev != g_fleetItems.end() && prev->second != itemsKey;
      const bool firstLoadout = prev == g_fleetItems.end();
      g_fleetItems[sid] = itemsKey;
      g_fleetRev[sid] = rev;
      g_fleet[sid] = l;
      CacheEntry& e = g_cache[sid];
      e.loadout = std::move(l);
      e.loaded_at = Clock::now();
      SKINS_DEBUG("fleet: skins.loadout %llu rev %lld", static_cast<unsigned long long>(sid), static_cast<long long>(rev));
      if (changed) QueueWarmupRegive(sid);  // warmup: the held weapons get the new skins now
      // The first one may come after the player spawned (asked for on connect): paint what they
      // hold, or the first life on a server without a match was all stock (#339).
      if (firstLoadout) QueueFirstLoadoutRepaint(sid);
    } else if (type == "skins.invalidate") {
      std::lock_guard<std::mutex> lk(g_mu);
      g_fleet.erase(sid);
      g_fleetRev.erase(sid);
      g_fleetItems.erase(sid);
      g_cache.erase(sid);
    }
  } catch (...) {
    Log(RU_LOG_ERROR, "fleet: handling %s threw", m->type);
  }
}

void FlushStatTrak(const ru_fleet_v1* f) {
  std::map<std::pair<uint64_t, int>, int> kills;
  {
    std::lock_guard<std::mutex> lk(g_stMu);
    kills.swap(g_pendingKills);
  }
  if (kills.empty() || !f) return;
  Json incs = Json::Array();
  for (const auto& kv : kills) {
    Json o = Json::Object();
    o["steamid64"] = std::to_string(kv.first.first);
    o["defindex"] = kv.first.second;
    o["kills"] = kv.second;
    incs.Push(std::move(o));
  }
  Json p = Json::Object();
  p["increments"] = std::move(incs);
  if (!f->send_event("skins.stattrak", p.Dump().c_str(), 0, RU_FLEET_RELIABLE)) {
    Log(RU_LOG_WARN, "fleet: skins.stattrak not queued (%zu weapon(s))", kills.size());
  }
}

}  // namespace

bool LoadoutStart() {
  const char* dir = g_api->data_dir(g_api->self);
  if (!dir || !*dir) {
    g_status = "no plugin data dir";
    return false;
  }
  g_loadoutsPath = std::string(dir) + "/loadouts.json";
  g_stattrakPath = std::string(dir) + "/stattrak.json";
  g_fileMtime = -1;
  g_countersLoaded = false;
  {
    std::lock_guard<std::mutex> lk(g_qmu);
    g_stop = false;
    g_jobs.clear();
    g_running = true;
  }
  g_worker = std::thread(WorkerMain);
  g_status = "json " + g_loadoutsPath;
  Post([] {
    ReloadFileIfChanged();
    LoadCounters();
  });
  return true;
}

void LoadoutStop() {
  if (const ru_fleet_v1* f = Fleet(); f && f->instance_id() == g_fleetInstance) {
    FlushStatTrak(f);
    for (uint64_t id : g_handlerIds) f->unregister_handler(id);
  }
  g_handlerIds.clear();
  g_fleetInstance = 0;
  {
    std::lock_guard<std::mutex> lk(g_qmu);
    g_stop = true;  // the worker finishes what is queued (a pending StatTrak save) and leaves
  }
  g_qcv.notify_all();
  if (g_worker.joinable()) g_worker.join();
  {
    std::lock_guard<std::mutex> lk(g_qmu);
    g_running = false;
  }
  std::lock_guard<std::mutex> lk(g_mu);
  g_cache.clear();
  g_fleet.clear();
  g_fleetRev.clear();
}

void FleetTick(double now) {
  const ru_fleet_v1* f = Fleet();
  const bool fleet = f && f->connection_state() != RU_FLEET_LINK_STANDALONE;
  if (fleet != g_fleetMode.exchange(fleet)) {
    Log(RU_LOG_INFO, "loadouts from %s", fleet ? "the platform (skins.loadout)" : g_loadoutsPath.c_str());
    std::lock_guard<std::mutex> lk(g_mu);
    g_cache.clear();  // refetch everyone from the new source
  }
  if (!f) {
    g_fleetInstance = 0;
    g_handlerIds.clear();
    return;
  }
  if (f->instance_id() != g_fleetInstance) {
    g_fleetInstance = f->instance_id();
    g_handlerIds.clear();  // a new fleet.so image has no registrations
    for (const char* type : {"skins.loadout", "skins.invalidate"}) {
      if (const uint64_t id = f->register_handler(type, &OnFleetMessage, nullptr)) g_handlerIds.push_back(id);
    }
    f->add_capability("skins.v1");
  }
  if (now - g_lastFlush >= 10.0) {
    g_lastFlush = now;
    if (fleet) FlushStatTrak(f);
  }
}

std::string LoadoutStatus() {
  size_t cached = 0, fleetPlayers = 0;
  {
    std::lock_guard<std::mutex> lk(g_mu);
    cached = g_cache.size();
    fleetPlayers = g_fleet.size();
  }
  size_t queued = 0;
  {
    std::lock_guard<std::mutex> lk(g_qmu);
    queued = g_jobs.size();
  }
  const std::string src = g_fleetMode.load()
                              ? "platform (" + std::to_string(fleetPlayers) + " loadout(s) received)"
                              : g_status;
  return src + ", " + std::to_string(cached) + " player(s) cached, " + std::to_string(queued) + " job(s) queued";
}

void MaybeRefreshAsync(uint64_t steamid64) {
  if (steamid64 == 0) return;
  const auto now = Clock::now();
  {
    std::lock_guard<std::mutex> lk(g_mu);
    CacheEntry& e = g_cache[steamid64];
    if (e.loading || !IsStale(e, now)) return;
    e.loading = true;
  }
  Post([steamid64] { RefreshJob(steamid64); });
}

void Invalidate(uint64_t steamid64) {
  if (steamid64 == 0) return;
  std::lock_guard<std::mutex> lk(g_mu);
  auto it = g_cache.find(steamid64);
  if (it == g_cache.end()) return;
  // Keep serving the old loadout until the refetch lands; just force the next refresh.
  it->second.loaded_at = Clock::time_point{} + std::chrono::nanoseconds(1);
}

bool IsLoaded(uint64_t steamid64) {
  if (steamid64 == 0) return false;
  std::lock_guard<std::mutex> lk(g_mu);
  auto it = g_cache.find(steamid64);
  return it != g_cache.end() && it->second.loaded_at.time_since_epoch().count() != 0;
}

std::optional<WeaponSkinEntry> FindWeaponSkin(uint64_t steamid64, int weapon_team, int weapon_defindex) {
  if (steamid64 == 0 || weapon_defindex <= 0) return std::nullopt;
  MaybeRefreshAsync(steamid64);
  std::lock_guard<std::mutex> lk(g_mu);
  auto it = g_cache.find(steamid64);
  if (it == g_cache.end()) return std::nullopt;
  const auto tl = FindTeamLocked(it->second.loadout, weapon_team);
  if (!tl) return std::nullopt;
  auto ws = tl->skins_by_defindex.find(weapon_defindex);
  if (ws == tl->skins_by_defindex.end()) return std::nullopt;
  return ws->second;
}

std::optional<std::string> FindKnifeClassname(uint64_t steamid64, int weapon_team) {
  if (steamid64 == 0) return std::nullopt;
  MaybeRefreshAsync(steamid64);
  std::lock_guard<std::mutex> lk(g_mu);
  auto it = g_cache.find(steamid64);
  if (it == g_cache.end() || it->second.loaded_at.time_since_epoch().count() == 0) return std::nullopt;
  const auto tl = FindTeamLocked(it->second.loadout, weapon_team);
  // No knife row: stock knife (no default, not even for admins).
  if (tl && tl->knife_classname && !tl->knife_classname->empty()) return *tl->knife_classname;
  return std::nullopt;
}

std::optional<int> FindGloveDefindex(uint64_t steamid64, int weapon_team) {
  if (steamid64 == 0) return std::nullopt;
  MaybeRefreshAsync(steamid64);
  std::lock_guard<std::mutex> lk(g_mu);
  auto it = g_cache.find(steamid64);
  if (it == g_cache.end()) return std::nullopt;
  const auto tl = FindTeamLocked(it->second.loadout, weapon_team);
  if (!tl || !tl->glove_defindex || *tl->glove_defindex <= 0) return std::nullopt;
  return *tl->glove_defindex;
}

std::optional<WeaponAgentEntry> FindAgents(uint64_t steamid64) {
  if (steamid64 == 0) return std::nullopt;
  MaybeRefreshAsync(steamid64);
  std::lock_guard<std::mutex> lk(g_mu);
  auto it = g_cache.find(steamid64);
  if (it == g_cache.end() || !it->second.loadout.agents) return std::nullopt;
  const auto& a = *it->second.loadout.agents;
  if (a.agent_ct.empty() && a.agent_t.empty()) return std::nullopt;
  return a;
}

std::optional<int> KnifeClassnameToDefindex(const std::string& classname) {
  auto it = Knives().find(classname);
  if (it == Knives().end()) return std::nullopt;
  return it->second;
}

void IncrementStatTrakAsync(uint64_t steamid64, int weapon_team, int weapon_defindex) {
  // The cache first, so the next weapon applied shows the new count right away.
  {
    std::lock_guard<std::mutex> lk(g_mu);
    auto it = g_cache.find(steamid64);
    if (it != g_cache.end()) {
      auto team = it->second.loadout.by_team.find(weapon_team);
      if (team != it->second.loadout.by_team.end()) {
        auto ws = team->second.skins_by_defindex.find(weapon_defindex);
        if (ws != team->second.skins_by_defindex.end()) ++ws->second.stattrak_count;
      }
    }
  }
  if (g_fleetMode.load()) {  // the platform stores counters (skins.stattrak, flushed every 10 s)
    {
      std::lock_guard<std::mutex> lk(g_mu);
      auto p = g_fleet.find(steamid64);
      if (p != g_fleet.end()) {
        auto team = p->second.by_team.find(weapon_team);
        if (team != p->second.by_team.end()) {
          auto ws = team->second.skins_by_defindex.find(weapon_defindex);
          if (ws != team->second.skins_by_defindex.end()) ++ws->second.stattrak_count;
        }
      }
    }
    std::lock_guard<std::mutex> lk(g_stMu);
    ++g_pendingKills[{steamid64, weapon_defindex}];
    return;
  }
  Post([steamid64, weapon_team, weapon_defindex] {
    ReloadFileIfChanged();
    LoadCounters();
    const std::string key = CounterKey(steamid64, weapon_team, weapon_defindex);
    auto it = g_counters.find(key);
    if (it == g_counters.end()) {
      int base = 0;
      auto p = g_file.find(steamid64);
      if (p != g_file.end()) {
        auto team = p->second.by_team.find(weapon_team);
        if (team != p->second.by_team.end()) {
          auto ws = team->second.skins_by_defindex.find(weapon_defindex);
          if (ws != team->second.skins_by_defindex.end()) base = ws->second.stattrak_count;
        }
      }
      it = g_counters.emplace(key, base).first;
    }
    ++it->second;
    g_countersDirty = true;
    if (!g_savePending.exchange(true)) {  // one save for a burst of kills
      Post([] {
        g_savePending.store(false);
        if (!g_countersDirty) return;
        g_countersDirty = false;
        SaveCounters();
      });
    }
  });
}

}  // namespace skins

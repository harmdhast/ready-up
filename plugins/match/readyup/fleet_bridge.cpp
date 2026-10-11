// The match side of the fleet link. See fleet_bridge.h and docs/FLEET.md §7-§13.
#include "readyup/fleet_bridge.h"

#include "readyup/fleet_iface.h"

#include "readyup/admin_check.h"
#include "readyup/backup_files.h"
#include "readyup/cs2_update_check.h"
#include "readyup/cs2_version.h"
#include "readyup/demo_recorder.h"
#include "readyup/engine.h"
#include "readyup/esports.h"
#include "readyup/fleet_state.h"
#include "readyup/local_store.h"
#include "readyup/log_relay.h"
#include "readyup/logging.h"
#include "readyup/map_names.h"
#include "readyup/match_config_parser.h"
#include "readyup/match_console.h"
#include "readyup/match_end.h"
#include "readyup/match_events.h"
#include "readyup/match_features.h"
#include "readyup/match_log.h"
#include "readyup/match_recovery.h"
#include "readyup/match_settings.h"
#include "readyup/match_signals.h"
#include "readyup/match_state.h"
#include "readyup/match_stats.h"
#include "readyup/match_status.h"
#include "readyup/modes.h"
#include "readyup/pause_state.h"
#include "readyup/persisted_match_state.h"
#include "readyup/persisted_settings.h"
#include "readyup/players.h"
#include "readyup/round_restore.h"
#include "readyup/scrim_flow.h"
#include "readyup/http_client.h"
#include "readyup/webhook.h"
#include "readyup/workers.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <set>
#include <sstream>
#include <string>
#include <vector>

namespace readyup::fleet_bridge {
namespace {

using status::Json;
namespace fs = fleetstate;

const char* const kHandOverMessage = "This server was assigned a tournament match. Thanks for playing!";
constexpr double kHandOverDelayS = 5.0;       // D16: kick non-roster players 5 s after the notice
constexpr double kPollIntervalS = 0.25;       // state diff cadence without signals
constexpr double kPeriodicSnapshotS = 60.0;   // §8.2
constexpr double kLoadTimeoutS = 120.0;       // "loading" ends at the latest after this
constexpr size_t kBackupPartRaw = 384 * 1024; // raw bytes per event.backup part (512 KB base64, §12.3)
constexpr size_t kBackupMaxRaw = 4u << 20;    // larger files are not forwarded
constexpr size_t kExecOutputMax = 8192;       // §7.4
constexpr double kExecCaptureS = 0.5;

const ru_api* g_api = nullptr;

// ---- fleet.so -------------------------------------------------------------------------------
uint64_t g_fleetInstance = 0;
std::vector<uint64_t> g_handlerIds;
double g_lastIdlePublish = -1e9;
std::string g_lastPublishedAvail;

// ---- the assignment (game thread) -----------------------------------------------------------
fs::Assignment g_asg;
Json g_config;  // match.assign config (+ match.update ops)
fs::Fence g_fence;
fs::LiveStream g_stream;
bool g_handOverPending = false;
double g_handOverAt = 0;
bool g_loading = false;
double g_loadStarted = 0;
std::string g_loadFromMap;
unsigned g_mapStarts = 0;      // RU_EVENT_MAP_START seen (OnMapStart)
unsigned g_loadMapStarts = 0;  // g_mapStarts when the load began
bool g_restoring = false;
bool g_seriesOver = false;   // series_end seen for this assignment
bool g_serverReset = false;  // the match flow unloaded the finished match (ServerReset)
int g_latestBackupRound = -1;
Json g_lastLive;  // the match flow's last view of the assigned match (kept once it is unloaded)
bool g_pauseSeen = false;
std::string g_phaseReason;
std::string g_unpauseBy;
double g_lastPoll = -1e9;
double g_lastPeriodic = -1e9;
double g_now = 0;
LogRelay g_logRelay;

// Deferred work (game thread).
struct PendingKick {
  double at = 0;
  std::string message;
};
std::vector<PendingKick> g_kicks;
struct ExecCapture {
  std::string ref, auditId;
  long long epoch = 0;
  double until = 0;
  std::string output;
};
std::vector<ExecCapture> g_execs;
double g_backupScanAt = -1;

// Failover resume (§11.3) of this assignment: map N loads, warmup until everyone is ready, the
// go-live, then the chosen backup is restored (DoRestore) and the match waits paused.
fs::ResumePlan g_resume;
bool g_resumeActive = false;           // from the assign until the backup was restored
double g_resumeRestoreAt = -1;         // restore time (1 s after the go-live)
bool g_resumeRestored = false;         // RunResumeRestore ran
double g_resumeUnpauseAt = -1;         // rules.pause.pause_after_restore = false
bool g_resumeSnapshotPending = false;  // state.snapshot (restored) after match_restored went out

// Sent backup files as "name|size|mtime" (worker thread + game thread): CS2 rewrites a round's
// file when the round is played again after a restore, and the new version is sent too.
std::mutex g_backupMu;
std::set<std::string> g_sentBackups;

std::string BackupKey(const std::filesystem::path& p) {
  std::error_code ec;
  const auto size = std::filesystem::file_size(p, ec);
  const auto mtime = std::filesystem::last_write_time(p, ec).time_since_epoch().count();
  return p.filename().string() + "|" + std::to_string(ec ? 0 : size) + "|" + std::to_string(mtime);
}

long long UnixMs() {
  return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch())
      .count();
}

// ---------------------------------------------------------------------------- fleet access

const ru_fleet_v1* Fleet() {
  if (!g_api) return nullptr;
  auto* f = static_cast<const ru_fleet_v1*>(g_api->get_interface(g_api->self, RU_FLEET_IFACE_NAME, 1));
  if (!f || f->struct_size < offsetof(ru_fleet_v1, add_capability) + sizeof(f->add_capability)) return nullptr;
  return f;
}

// A member appended after step 1 is there only when the provider's struct is big enough.
#define FLEET_HAS(f, member)   ((f) && (f)->struct_size >= offsetof(ru_fleet_v1, member) + sizeof(static_cast<ru_fleet_v1*>(nullptr)->member))

bool FleetActive(const ru_fleet_v1* f) {
  return f && f->connection_state() != RU_FLEET_LINK_STANDALONE;
}

bool Send(const std::string& type, const Json& payload, long long epoch, bool reliable, const std::string& ref = {}) {
  const ru_fleet_v1* f = Fleet();
  if (!FleetActive(f)) return false;
  const std::string body = payload.Dump();
  const uint32_t flags = reliable ? static_cast<uint32_t>(RU_FLEET_RELIABLE) : 0u;
  if (!ref.empty() && FLEET_HAS(f, send_reply)) {
    return f->send_reply(type.c_str(), body.c_str(), epoch, flags, ref.c_str()) == 1;
  }
  return f->send_event(type.c_str(), body.c_str(), epoch, flags) == 1;
}

bool SendSnapshot(const char* reason, bool withStats) {
  const ru_fleet_v1* f = Fleet();
  if (!FleetActive(f) || !FLEET_HAS(f, send_snapshot)) return false;
  std::string extra;
  if (withStats && g_asg.active) {
    std::lock_guard<std::recursive_mutex> lk(stats::Mutex());
    extra = std::string("{\"map_stats\":") + stats::ToJson(stats::Current().Snapshot()) + "}";
  }
  return f->send_snapshot(reason, extra.empty() ? nullptr : extra.c_str()) == 1;
}

// ---------------------------------------------------------------------------- replies

struct Result {
  std::string status = "ok";  // ok | rejected | failed | expired
  std::string code, message;
  long long rev = -1;
  std::string output;
  bool haveOutput = false;
  bool deferred = false;  // exec: the reply comes after the output capture
};

Result Ok() { return Result{}; }
Result Rejected(const std::string& code, const std::string& message) {
  Result r;
  r.status = "rejected";
  r.code = code;
  r.message = message;
  return r;
}
Result Failed(const std::string& code, const std::string& message) {
  Result r = Rejected(code, message);
  r.status = "failed";
  return r;
}

void Reply(const std::string& ref, long long epoch, const Result& r, const std::string& auditId = {}) {
  Json p = Json::Object();
  p["status"] = r.status;
  if (!r.code.empty()) {
    Json e = Json::Object();
    e["code"] = r.code;
    e["message"] = r.message.empty() ? r.code : r.message;
    p["error"] = std::move(e);
  }
  if (r.rev >= 0) p["rev"] = r.rev;
  if (r.haveOutput) p["output"] = r.output;
  if (!auditId.empty()) p["audit_id"] = auditId;
  Send("cmd.result", p, epoch > 0 ? epoch : 0, true, ref);
}

// ---------------------------------------------------------------------------- helpers

std::string Str(const Json& o, const char* key, const std::string& def = {}) {
  const Json* v = o.Find(key);
  return v && v->type() == Json::Type::String ? v->AsString() : def;
}
long long Int(const Json& o, const char* key, long long def = 0) {
  const Json* v = o.Find(key);
  if (!v || (v->type() != Json::Type::Int && v->type() != Json::Type::Double)) return def;
  return v->AsInt();
}
bool Bool(const Json& o, const char* key, bool def = false) {
  const Json* v = o.Find(key);
  return v && v->type() == Json::Type::Bool ? v->AsBool() : def;
}
const Json* Path(const Json& o, std::initializer_list<const char*> keys) {
  const Json* cur = &o;
  for (const char* k : keys) {
    if (!cur || !cur->IsObject()) return nullptr;
    cur = cur->Find(k);
  }
  return cur;
}
Json Copy(const Json* v) { return v ? *v : Json(); }

bool CtxIsOurs(const std::optional<WebhookMatchContext>& ctx) {
  return g_asg.active && ctx && ctx->matchid == fs::NumericMatchId(g_asg.match_id);
}

// A real match that did not come from the platform (`ru match load`): new assignments are busy.
bool LocalMatchActive() {
  const auto ctx = WebhookGetMatchContext();
  return ctx && ctx->slug != "scrim" && !CtxIsOurs(ctx);
}

// The loaded match (e.g. recovered from state.json after a crash) is `matchId`'s.
bool LocalMatchIs(const std::string& matchId) {
  const auto ctx = WebhookGetMatchContext();
  return ctx && ctx->slug != "scrim" && ctx->matchid == fs::NumericMatchId(matchId);
}

// hello.admins_rev / state.snapshot.admins_rev: the admins.set rev cached in fleet-admins.json.
void PublishAdminsRev() {
  const ru_fleet_v1* f = Fleet();
  if (!FLEET_HAS(f, set_admins_rev)) return;
  int64_t rev = -1;
  (void)local_store::FleetAdmins(&rev);
  f->set_admins_rev(rev);
}

std::string ServerId() {
  const ru_fleet_v1* f = Fleet();
  if (!f) return {};
  ru_fleet_status st{};
  st.struct_size = sizeof(st);
  if (f->get_status(&st) != 1) return {};
  return st.server_id;
}

bool UpdateBlocked() {
  const ru_fleet_v1* f = Fleet();
  if (!f) return false;
  ru_fleet_status st{};
  st.struct_size = sizeof(st);
  if (f->get_status(&st) != 1) return false;
  return st.update_blocked != 0;
}

// Humans connected right now.
void KickHumans(const std::string& message, bool (*keep)(uint64_t)) {
  for (const auto& h : ListHumans()) {
    if (h.userid < 0 || h.steamid64 == 0) continue;
    if (keep && keep(h.steamid64)) continue;
    std::string m = message;
    for (char& c : m) {
      if (c == '"' || c == ';' || c == '\n' || c == '\r') c = ' ';
    }
    const std::string cmd = "kickid " + std::to_string(h.userid) + " \"" + m + "\"";
    (void)EnqueueServerCommand(cmd.c_str());
  }
}

bool KeepOnHandOver(uint64_t sid) {
  return fs::InAssignedMatch(g_config, sid) || IsReadyUpAdmin(sid);
}

void SetPassword(const std::string& pw) {
  // ValidPassword() already refused quotes / `;` / spaces.
  const std::string cmd = "sv_password \"" + pw + "\"";
  (void)EnqueueServerCommand(cmd.c_str());
}

// ---------------------------------------------------------------------------- MatchState

// Platform-owned part of MatchState (§9.2) from the assign config, server-owned fields at their
// idle values.
Json ConfigState() {
  Json st = Json::Object();
  st["match_id"] = g_asg.match_id;
  st["epoch"] = g_asg.epoch;
  const std::string sid = ServerId();
  if (!sid.empty()) st["server_id"] = sid;
  st["config_rev"] = g_asg.config_rev;
  st["live_rev"] = 0;
  st["phase"] = "loading";

  Json series = Json::Object();
  series["num_maps"] = Int(g_config, "num_maps", 1);
  series["current_map"] = 1;
  Json zero = Json::Object();
  zero["team1"] = 0;
  zero["team2"] = 0;
  series["score"] = zero;
  Json maps = Json::Object();
  if (const Json* ml = g_config.Find("maps")) {
    int n = 0;
    for (const auto& m : ml->Items()) {
      ++n;
      Json mj = Json::Object();
      mj["name"] = Str(m, "name");
      mapnames::MapRef ref;
      if (!Str(m, "workshop_id").empty()) mj["workshop_id"] = Str(m, "workshop_id");
      else if (mapnames::ParseEntry(Str(m, "name"), &ref) && !ref.workshop_id.empty()) mj["workshop_id"] = ref.workshop_id;
      mj["sides"] = Str(m, "sides", "knife");
      mj["status"] = "pending";
      mj["score"] = zero;
      maps[std::to_string(n)] = std::move(mj);
    }
  }
  if (g_resumeActive) {
    // Failover resume: the platform's series state until the match flow reports its own.
    series["current_map"] = g_resume.map_number;
    series["score"]["team1"] = g_resume.series_team1;
    series["score"]["team2"] = g_resume.series_team2;
    for (const auto& r : g_resume.maps_done) {
      const std::string k = std::to_string(r.map_number);
      if (!maps.Find(k)) continue;
      maps[k]["status"] = "done";
      maps[k]["score"]["team1"] = r.team1;
      maps[k]["score"]["team2"] = r.team2;
      maps[k]["winner"] = r.winner;
    }
  }
  series["maps"] = std::move(maps);
  st["series"] = std::move(series);

  int maxPlayers = 0;
  Json teams = Json::Object();
  Json spectators = Json::Object();
  for (const char* key : {"team1", "team2"}) {
    const Json* src = g_config.Find(key);
    Json t = Json::Object();
    const std::string id = src ? Str(*src, "id") : std::string();
    t["id"] = id.empty() ? std::string(key) : id;
    t["name"] = src ? Str(*src, "name") : std::string(key);
    if (src && !Str(*src, "tag").empty()) t["tag"] = Str(*src, "tag");
    t["score"] = 0;
    t["score_ct"] = 0;
    t["score_t"] = 0;
    Json players = Json::Object();
    int n = 0;
    if (src) {
      if (const Json* ps = src->Find("players")) {
        for (const auto& p : ps->Items()) {
          Json pj = Json::Object();
          pj["name"] = Str(p, "name");
          pj["role"] = Str(p, "role", "player");
          pj["connected"] = false;
          pj["ready"] = false;
          if (Str(p, "role", "player") == "player") ++n;
          players[Str(p, "steamid64")] = std::move(pj);
        }
      }
    }
    maxPlayers = std::max(maxPlayers, n);
    t["players"] = std::move(players);
    teams[key] = std::move(t);
  }
  st["teams"] = std::move(teams);
  if (const Json* sp = g_config.Find("spectators")) {
    for (const auto& s : sp->Items()) {
      Json sj = Json::Object();
      sj["name"] = "";
      sj["connected"] = false;
      spectators[s.AsString()] = std::move(sj);
    }
  }
  st["spectators"] = std::move(spectators);

  Json ready = Json::Object();
  const long long minPer = Int(Copy(Path(g_config, {"rules", "ready"})), "min_per_team", 0);
  ready["required_per_team"] = minPer > 0 ? minPer : maxPlayers;
  st["ready"] = std::move(ready);
  Json knife = Json::Object();
  knife["status"] = "none";
  st["knife"] = std::move(knife);
  Json pause = Json::Object();
  pause["active"] = false;
  Json un = Json::Object();
  un["team1"] = false;
  un["team2"] = false;
  pause["unpause"] = std::move(un);
  Json used = Json::Object();
  for (int t = 0; t < 2; ++t) {
    Json u = Json::Object();
    const WebhookTeam wt = t == 0 ? WebhookTeam::Team1 : WebhookTeam::Team2;
    u["tactical"] = PauseStateUsed(wt, "tactical");
    u["technical"] = PauseStateUsed(wt, "technical");
    used[t == 0 ? "team1" : "team2"] = std::move(u);
  }
  pause["used"] = std::move(used);
  st["pause"] = std::move(pause);
  Json round = Json::Object();
  round["number"] = 0;
  st["round"] = std::move(round);
  Json backups = Json::Object();
  if (g_latestBackupRound >= 0) backups["latest_round"] = g_latestBackupRound;
  st["backups"] = std::move(backups);
  const Json* rules = g_config.Find("rules");
  st["rules"] = rules && rules->IsObject() ? *rules : Json::Object();
  return st;
}

// Server-owned fields from the match flow's view of the loaded match (match_status.cpp).
void Overlay(Json* st, const Json& live) {
  Json& s = *st;
  if (const Json* v = Path(live, {"series", "current_map"})) s["series"]["current_map"] = *v;
  if (const Json* v = Path(live, {"series", "score"})) s["series"]["score"] = *v;
  if (const Json* lm = Path(live, {"series", "maps"})) {
    Json& maps = s["series"]["maps"];
    for (const auto& kv : lm->Members()) {
      if (!maps.Find(kv.first)) continue;
      Json& m = maps[kv.first];
      for (const char* k : {"status", "score", "winner", "demo"}) {
        if (const Json* v = kv.second.Find(k)) m[k] = *v;
      }
    }
  }
  for (const char* team : {"team1", "team2"}) {
    const Json* lt = Path(live, {"teams", team});
    if (!lt) continue;
    Json& t = s["teams"][team];
    for (const char* k : {"side", "score", "score_ct", "score_t"}) {
      if (const Json* v = lt->Find(k)) t[k] = *v;
    }
    const Json* lp = lt->Find("players");
    Json& players = t["players"];
    for (const auto& kv : players.Members()) {
      const Json* p = lp ? lp->Find(kv.first) : nullptr;
      if (!p) continue;
      Json& dst = players[kv.first];
      for (const char* k : {"connected", "ready"}) {
        if (const Json* v = p->Find(k)) dst[k] = *v;
      }
      if (Str(dst, "name").empty() && !Str(*p, "name").empty()) dst["name"] = Str(*p, "name");
    }
  }
  if (const Json* ls = live.Find("spectators")) {
    Json& specs = s["spectators"];
    for (const auto& kv : ls->Members()) {
      if (!specs.Find(kv.first)) continue;
      Json& dst = specs[kv.first];
      if (const Json* v = kv.second.Find("connected")) dst["connected"] = *v;
      if (!Str(kv.second, "name").empty()) dst["name"] = Str(kv.second, "name");
    }
  }
  if (const Json* r = live.Find("ready")) {
    for (const char* k : {"ready", "total"}) {
      if (const Json* v = r->Find(k)) s["ready"][k] = *v;
    }
  }
  if (const Json* k = live.Find("knife")) s["knife"] = *k;
  if (const Json* p = live.Find("pause")) {
    for (const char* k : {"active", "started_at", "unpause"}) {
      if (const Json* v = p->Find(k)) s["pause"][k] = *v;
    }
  }
  if (const Json* r = live.Find("round")) s["round"] = *r;
  // Ruleset + effective rules (ruleset.h): what the server enforces, "differs from Valve".
  for (const char* k : {"ruleset", "effective_rules"}) {
    if (const Json* v = live.Find(k)) s[k] = *v;
  }
}

Json BuildState() {
  Json st = ConfigState();
  const auto ctx = WebhookGetMatchContext();
  std::string phase;
  if (CtxIsOurs(ctx)) {
    Json live = MatchStatusStateJson();
    if (!live.IsNull()) {
      // Go-live pending (after the side pick / all ready): the restart into live runs (§9.5).
      if (Str(live, "phase") == "warmup" && GoLiveTriggered()) live["phase"] = "live";
      // Map scores from the stats model (engine round winners through the current sides) while it
      // records: the log-derived MatchState score is CT/T based and lags after a side swap.
      {
        std::lock_guard<std::recursive_mutex> lk(stats::Mutex());
        auto& acc = stats::Current();
        if (acc.Live()) {
          const int mapNo = static_cast<int>(Int(Copy(live.Find("series")), "current_map", 1));
          live["teams"]["team1"]["score"] = acc.Team1Score();
          live["teams"]["team2"]["score"] = acc.Team2Score();
          Json& m = live["series"]["maps"][std::to_string(mapNo)];
          if (Str(m, "status") != "done") {
            m["score"]["team1"] = acc.Team1Score();
            m["score"]["team2"] = acc.Team2Score();
          }
        }
      }
      g_lastLive = live;
      Overlay(&st, live);
      phase = Str(live, "phase");
    }
  } else if (!g_lastLive.IsNull()) {
    // Unloaded (series end, end_match): keep the final scores and map results; nothing is
    // paused, knifed or connected-and-ready any more.
    Json last = g_lastLive;
    last["pause"]["active"] = false;
    last["pause"]["unpause"]["team1"] = false;
    last["pause"]["unpause"]["team2"] = false;
    last["knife"] = Json::Object();
    last["knife"]["status"] = "none";
    Overlay(&st, last);
  }
  // Pause kind (who / why) and the per-team counters.
  const PauseSnapshot ps = PauseStateGet();
  if (ps.paused && CtxIsOurs(ctx)) {
    g_pauseSeen = true;
    // The protocol's pause types have no "restore" (DoRestoreNow): the platform sees technical.
    st["pause"]["type"] = ps.type.empty() ? std::string("admin") : ps.type == "restore" ? std::string("technical") : ps.type;
    if (!ps.by.empty()) st["pause"]["by"] = ps.by;
  } else {
    g_pauseSeen = false;
  }
  if (g_restoring) phase = "restoring";
  else if (g_handOverPending || g_loading) phase = "loading";
  else if (phase.empty() || phase == "idle" || phase == "practice") phase = g_seriesOver ? "series_end" : "loading";
  st["phase"] = phase;
  return st;
}

// server.drain (FLEET.md §14.3): finish the current series, take nothing new, report "draining"
// until server.undrain. Held in memory only: a restart (the update the drain was for) clears it.
bool g_draining = false;

std::string Availability() {
  if (g_draining) return "draining";
  if (!g_asg.active) return LocalMatchActive() ? "busy" : "available";
  return g_serverReset ? "available" : "busy";
}

void PublishState(const Json& st) {
  const ru_fleet_v1* f = Fleet();
  if (!f) return;
  const std::string avail = Availability();
  const std::string body = st.IsNull() ? std::string("null") : st.Dump();
  f->publish_state(body.c_str(), avail.c_str());
  if (avail != g_lastPublishedAvail && !g_lastPublishedAvail.empty() && FleetActive(f)) {
    Json p = Json::Object();
    p["availability"] = avail;
    p["reason"] = avail == "draining" ? "drain" : g_asg.active ? (g_serverReset ? "series_end" : "assigned") : "idle";
    Send("server.availability", p, 0, true);
  }
  g_lastPublishedAvail = avail;
}

// ---------------------------------------------------------------------------- events

struct Ev {
  std::string type;
  Json data;
  int round = -1;
  int mapNumber = -1;
};

std::string TeamOfSid(uint64_t sid) {
  const std::string t = fs::TeamOf(g_config, sid);
  return t.empty() ? "none" : t;
}

// Semantic changes between the last state sent and the new one (§8.1): phase, ready, pause.
void Derive(const Json& prev, const Json& cur, std::vector<Ev>* out) {
  if (prev.IsNull()) return;
  const std::string pPhase = Str(prev, "phase"), cPhase = Str(cur, "phase");
  if (pPhase != cPhase) {
    Ev e{"phase", Json::Object()};
    e.data["from"] = pPhase;
    e.data["to"] = cPhase;
    e.data["reason"] = g_phaseReason.empty() ? std::string("flow") : g_phaseReason;
    out->push_back(std::move(e));
  }
  g_phaseReason.clear();

  auto readyCount = [&](const Json& st, const char* team) {
    int n = 0;
    if (const Json* ps = Path(st, {"teams", team, "players"})) {
      for (const auto& kv : ps->Members()) n += Bool(kv.second, "ready") ? 1 : 0;
    }
    return n;
  };
  for (const char* team : {"team1", "team2"}) {
    const Json* cp = Path(cur, {"teams", team, "players"});
    const Json* pp = Path(prev, {"teams", team, "players"});
    if (!cp || !pp) continue;
    for (const auto& kv : cp->Members()) {
      const Json* was = pp->Find(kv.first);
      if (!was) continue;
      const bool r0 = Bool(*was, "ready"), r1 = Bool(kv.second, "ready");
      if (r0 == r1) continue;
      Ev e{r1 ? "player_ready" : "player_unready", Json::Object()};
      e.data["steamid64"] = kv.first;
      e.data["team"] = team;
      e.data["ready_team1"] = readyCount(cur, "team1");
      e.data["ready_team2"] = readyCount(cur, "team2");
      e.data["required"] = Int(Copy(cur.Find("ready")), "required_per_team", 0);
      out->push_back(std::move(e));
    }
  }

  const Json* pp = prev.Find("pause");
  const Json* cp = cur.Find("pause");
  if (pp && cp) {
    const bool a0 = Bool(*pp, "active"), a1 = Bool(*cp, "active");
    if (!a0 && a1) {
      Ev e{"pause", Json::Object()};
      e.data["action"] = "paused";
      e.data["type"] = Str(*cp, "type", "admin");
      e.data["by"] = Str(*cp, "by", "unknown");
      const PauseSnapshot ps = PauseStateGet();
      if (ps.team == WebhookTeam::Team1) e.data["team"] = "team1";
      if (ps.team == WebhookTeam::Team2) e.data["team"] = "team2";
      out->push_back(std::move(e));
    } else if (a0 && !a1) {
      Ev e{"pause", Json::Object()};
      e.data["action"] = "unpaused";
      e.data["type"] = Str(*pp, "type", "admin");
      e.data["by"] = g_unpauseBy.empty() ? std::string("players") : g_unpauseBy;
      const long long started = Int(*pp, "started_at", 0);
      if (started > 0) e.data["duration_s"] = std::max<long long>(0, (UnixMs() - started) / 1000);
      out->push_back(std::move(e));
    } else if (a1) {
      for (const char* team : {"team1", "team2"}) {
        const bool u0 = Bool(Copy(pp->Find("unpause")), team), u1 = Bool(Copy(cp->Find("unpause")), team);
        if (!u0 && u1) {
          Ev e{"pause", Json::Object()};
          e.data["action"] = "unpause_requested";
          e.data["type"] = Str(*cp, "type", "admin");
          e.data["by"] = team;
          e.data["team"] = team;
          out->push_back(std::move(e));
        }
      }
    }
    if (a0 != a1) g_unpauseBy.clear();
  }
}

void EmitAll(const Json& st, std::vector<Ev>& evs) {
  const long long epoch = g_asg.epoch;
  if (evs.empty()) {
    Json patch;
    long long rev = 0;
    if (!g_stream.Advance(st, false, &patch, &rev)) return;
    Json p = Json::Object();
    p["match_id"] = g_asg.match_id;
    p["rev"] = rev;
    p["patch"] = std::move(patch);
    Send("state.patch", p, epoch, true);
    return;
  }
  const long long curMap = Int(Copy(st.Find("series")), "current_map", 1);
  const long long curRound = Int(Copy(st.Find("round")), "number", 0);
  for (auto& e : evs) {
    Json patch;
    long long rev = 0;
    (void)g_stream.Advance(st, true, &patch, &rev);
    Json p = Json::Object();
    p["match_id"] = g_asg.match_id;
    p["map_number"] = e.mapNumber > 0 ? static_cast<long long>(e.mapNumber) : std::max<long long>(1, curMap);
    const long long round = e.round >= 0 ? e.round : curRound;
    if (round > 0) p["round"] = round;
    p["rev"] = rev;
    p["patch"] = std::move(patch);
    p["data"] = std::move(e.data);
    Send("event." + e.type, p, epoch, true);
  }
}

// ---------------------------------------------------------------------------- round backups

std::string BackupPrefixFor(const std::string& matchId, int mapNumber) {
  return "readyup_backup_" + std::to_string(fs::NumericMatchId(matchId)) + "_map" +
         std::to_string(std::max(1, mapNumber)) + "_";
}
std::string BackupPrefix(int mapNumber) { return BackupPrefixFor(g_asg.match_id, mapNumber); }

// Where CS2 writes and reads its round backups (backup_files.h). Any thread.
std::vector<std::string> BackupDirs() { return backup_files::BackupDirs(); }

// Worker thread: forwards backup files with `prefix` that were not sent yet.
void ScanBackups(std::string prefix, int mapNumber, int team1, int team2) {
  struct Found {
    std::string name, path, key;
    int round;
  };
  std::vector<Found> found;
  for (const auto& dir : BackupDirs()) {
    std::error_code ec;
    for (const auto& it : std::filesystem::directory_iterator(dir, ec)) {
      if (ec) break;
      const std::string fn = it.path().filename().string();
      if (fn.compare(0, prefix.size(), prefix) != 0 || it.path().extension() != ".txt") continue;
      const int r = fs::BackupRoundFromName(fn);
      if (r < 0) continue;
      const std::string key = BackupKey(it.path());
      std::lock_guard<std::mutex> lk(g_backupMu);
      if (g_sentBackups.count(key)) continue;
      found.push_back(Found{fn, it.path().string(), key, r});
    }
  }
  if (found.empty()) return;
  std::sort(found.begin(), found.end(), [](const Found& a, const Found& b) { return a.round < b.round; });
  for (const auto& f : found) {
    std::ifstream in(f.path, std::ios::binary);
    if (!in) continue;
    std::ostringstream ss;
    ss << in.rdbuf();
    const std::string data = ss.str();
    {
      std::lock_guard<std::mutex> lk(g_backupMu);
      g_sentBackups.insert(f.key);
    }
    if (data.empty() || data.size() > kBackupMaxRaw) {
      Print("fleet: round backup %s not forwarded (%zu bytes)\n", f.name.c_str(), data.size());
      continue;
    }
    const std::string sha = fs::Sha256Hex(data);
    const size_t parts = (data.size() + kBackupPartRaw - 1) / kBackupPartRaw;
    // CS2 names the backup written at the start of round N+1 "...roundN.txt" (rounds played).
    const int round = f.round + 1;
    for (size_t i = 0; i < parts; ++i) {
      Json b = Json::Object();
      b["map_number"] = mapNumber;
      b["round"] = round;
      b["file"] = f.name;
      b["size"] = static_cast<long long>(data.size());
      b["sha256"] = sha;
      Json sc = Json::Object();
      // Only the newest file was written at this round start; older unsent ones use their own
      // round's score from the stats model when the bridge resolves it (0-0 otherwise).
      sc["team1"] = f.round == found.back().round ? team1 : 0;
      sc["team2"] = f.round == found.back().round ? team2 : 0;
      b["score"] = std::move(sc);
      b["encoding"] = "base64";
      b["data"] = fs::Base64Encode(data.substr(i * kBackupPartRaw, kBackupPartRaw));
      if (parts > 1) {
        b["part"] = static_cast<long long>(i + 1);
        b["parts"] = static_cast<long long>(parts);
      }
      signals::Emit("backup", std::move(b), round, mapNumber);
    }
    Print("fleet: round backup %s (round %d, %zu bytes, %zu part(s)) queued\n", f.name.c_str(), round, data.size(),
          parts);
  }
}

void ScheduleBackupScan(double delay) {
  if (g_backupScanAt < 0 || g_now + delay < g_backupScanAt) g_backupScanAt = g_now + delay;
}

void RunBackupScan() {
  const auto ms = MatchStateGet();
  const int mapNumber = ms.map_number <= 0 ? 1 : ms.map_number;
  const std::string prefix = BackupPrefix(mapNumber);
  int t1 = ms.team1_score, t2 = ms.team2_score;
  {
    std::lock_guard<std::recursive_mutex> lk(stats::Mutex());
    if (stats::Current().Live()) {
      t1 = stats::Current().Team1Score();
      t2 = stats::Current().Team2Score();
    }
  }
  workers::Spawn("fleet-backup", [prefix, mapNumber, t1, t2] { ScanBackups(prefix, mapNumber, t1, t2); });
}

// Finds a local backup file for `round` (1-based: the round it starts) on `mapNumber`; *dir = its
// directory.
std::string FindLocalBackup(int mapNumber, int round, std::string* dirOut, const std::string& prefixIn = {}) {
  const std::string prefix = prefixIn.empty() ? BackupPrefix(mapNumber) : prefixIn;
  for (const auto& dir : BackupDirs()) {
    std::error_code ec;
    for (const auto& it : std::filesystem::directory_iterator(dir, ec)) {
      if (ec) break;
      const std::string fn = it.path().filename().string();
      if (fn.compare(0, prefix.size(), prefix) == 0 && fs::BackupRoundFromName(fn) + 1 == round) {
        if (dirOut) *dirOut = dir;
        return fn;
      }
    }
  }
  return {};
}

// The directory a platform-sent backup is written to: where this server's backups are.
std::string RestoreDir() {
  // Where CS2 wrote its own round backups (its write path); else the first that exists.
  const auto dirs = BackupDirs();
  for (const auto& dir : dirs) {
    std::error_code ec;
    for (const auto& it : std::filesystem::directory_iterator(dir, ec)) {
      if (ec) break;
      if (it.path().filename().string().rfind("readyup_backup_", 0) == 0) return dir;
    }
  }
  for (const auto& dir : dirs) {
    std::error_code ec;
    if (std::filesystem::is_directory(dir, ec)) return dir;
  }
  return GetCsgoDirFromModuleDir();
}

// Writes a backup file where CS2 loads it from (temp + rename) and marks it as sent (it came from
// the platform or is a copy).
Result WriteBackupFile(const std::string& name, const std::string& raw) {
  const std::string dir = RestoreDir();
  const std::string tmp = dir + "/." + name + ".tmp";
  {
    std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
    out.write(raw.data(), static_cast<std::streamsize>(raw.size()));
    if (!out) return Failed("io", "cannot write the backup file");
  }
  std::error_code ec;
  std::filesystem::rename(tmp, dir + "/" + name, ec);
  if (ec) return Failed("io", "cannot move the backup file into place: " + ec.message());
  const std::string key = BackupKey(dir + "/" + name);
  std::lock_guard<std::mutex> lk(g_backupMu);
  g_sentBackups.insert(key);
  return Ok();
}

// A resume's backup (inline, a named local file, or this match's own file for map / round) as
// readyup_resume_<id>_map<N>_round<NN>.txt: a name CS2's own round backups never overwrite and the
// backup scan never forwards. Sets plan->file / sha256.
Result PrepareResumeBackup(const std::string& matchId, fs::ResumePlan* plan) {
  char nn[16];
  std::snprintf(nn, sizeof(nn), "%02d", std::max(0, plan->round - 1));
  const std::string dest = "readyup_resume_" + std::to_string(fs::NumericMatchId(matchId)) + "_map" +
                           std::to_string(plan->map_number) + "_round" + nn + ".txt";
  std::string raw;
  if (plan->inline_backup) {
    raw = std::move(plan->raw);
  } else {
    std::string dir, name = plan->file;
    if (name.empty()) {
      name = FindLocalBackup(plan->map_number, plan->round, &dir, BackupPrefixFor(matchId, plan->map_number));
    } else {
      for (const auto& d : BackupDirs()) {
        std::error_code ec;
        if (std::filesystem::is_regular_file(d + "/" + name, ec)) {
          dir = d;
          break;
        }
      }
    }
    if (name.empty() || dir.empty()) {
      return Rejected("no_backup", "no local backup for map " + std::to_string(plan->map_number) + " round " +
                                       std::to_string(plan->round) + "; send it inline");
    }
    std::ifstream in(dir + "/" + name, std::ios::binary);
    std::ostringstream ss;
    ss << in.rdbuf();
    raw = ss.str();
    if (raw.empty()) return Rejected("no_backup", "the local backup " + name + " is empty");
    const std::string sha = fs::Sha256Hex(raw);
    if (!plan->sha256.empty() && sha != plan->sha256) return Rejected("checksum", "backup_ref sha256 mismatch");
    plan->sha256 = sha;
  }
  plan->raw.clear();
  Result r = WriteBackupFile(dest, raw);
  if (r.status != "ok") return r;
  plan->file = dest;
  return Ok();
}

// The restore itself (cmd restore_round, a resume): loads `file` (mp_backup_restore_load_file,
// autopaused), voids rounds >= `round` in the stats model, the round counters and the scores
// (`scoreT1` / `scoreT2` >= 0: the backup's score wins over the stats), pauses and emits
// rounds_voided + match_restored (`extra` members added to its data), then round_restore.h
// AfterRestore (backup_loaded webhook, pause_after_restore). True when the match unpauses by
// itself in 3 s.
bool DoRestoreNow(int mapNumber, int round, const std::string& file, const std::string& sha, const std::string& by,
                  const std::string& reason, const Json& extra, int scoreT1, int scoreT2);

// A restore asked for between round_end and the next round_start: CS2 loads the backup but never
// starts the next round (the server stays in RoundOver until a map change; seen in the NTLAN trial
// run with `.restore` / cmd restore_round / a vote right after a round). It waits for round_start
// (match_features.h MatchFeaturesRoundOver) and runs from Tick. The newest one wins.
struct DeferredRestore {
  bool active = false;
  bool needRoundEnd = false;  // an admin restore asked mid-round: first the round ends
  int mapNumber = 0, round = 0, scoreT1 = -1, scoreT2 = -1;
  std::string file, sha, by, reason;
  Json extra;
};
DeferredRestore g_deferredRestore;

void RunDeferredRestore() {
  if (!g_deferredRestore.active) return;
  if (g_deferredRestore.needRoundEnd) {
    if (MatchFeaturesRoundOver()) g_deferredRestore.needRoundEnd = false;
    return;
  }
  if (MatchFeaturesRoundOver()) return;
  DeferredRestore d = std::move(g_deferredRestore);
  g_deferredRestore = DeferredRestore{};
  Print("fleet: next round started; running the restore of map %d round %d\n", d.mapNumber, d.round);
  (void)DoRestoreNow(d.mapNumber, d.round, d.file, d.sha, d.by, d.reason, d.extra, d.scoreT1, d.scoreT2);
}

bool DoRestore(int mapNumber, int round, const std::string& file, const std::string& sha, const std::string& by,
               const std::string& reason, const Json& extra, int scoreT1 = -1, int scoreT2 = -1) {
  // An admin restore during a live round (past freeze time) gives money back but not weapons,
  // armor or utility: CS2 applies the backup's per-player Items only when the restore lands at a
  // round start (live test; a failover resume, restored 1 s into a fresh round, got them all).
  // So it waits for this round to end and runs at the next round start, like a resume.
  if (reason != "resume" && !MatchFeaturesRoundOver() && !MatchFeaturesInFreeze()) {
    g_deferredRestore = DeferredRestore{true, true, mapNumber, round, scoreT1, scoreT2, file, sha, by, reason, extra};
    Print("fleet: restore of map %d round %d waits for this round to end (weapons come back at a round start)\n",
          mapNumber, round);
    SendToChat(("Ready Up: round " + std::to_string(round) +
                " is restored at the start of the next round (with everyone's weapons).")
                   .c_str());
    return false;
  }
  if (MatchFeaturesRoundOver()) {
    g_deferredRestore = DeferredRestore{true, false, mapNumber, round, scoreT1, scoreT2, file, sha, by, reason, extra};
    Print("fleet: restore of map %d round %d waits for the next round start (round over)\n", mapNumber, round);
    SendToChat(("Ready Up: round " + std::to_string(round) + " is restored when the next round starts.").c_str());
    return false;
  }
  return DoRestoreNow(mapNumber, round, file, sha, by, reason, extra, scoreT1, scoreT2);
}

bool DoRestoreNow(int mapNumber, int round, const std::string& file, const std::string& sha, const std::string& by,
                  const std::string& reason, const Json& extra, int scoreT1, int scoreT2) {
  g_restoring = true;
  MatchEventsIgnoreRoundEndsFor(5.0);  // the reload ends the current round as a draw
  (void)EnqueueServerCommand(("mp_backup_restore_load_file " + file).c_str());
  // mp_backup_restore_load_autopause 1 is meant to pause the restored round, but it does not
  // always (NTLAN trial run: a failover resume loaded at go-live played on with no pause while
  // Ready Up reported "paused" until the map ended). Pause explicitly; every way out of it
  // (admin unpause, pause_after_restore 0, resume) sends mp_unpause_match.
  (void)EnqueueServerCommand("mp_pause_match");
  // A "restore" pause, not an admin one: both teams lift it with .unpause (match_features.cpp),
  // so a LAN does not wait for an admin after every restore. An admin can still .fup.
  if (!PauseStateGet().paused) PauseStateOnPaused("restore", by);
  // Ready Up state: rounds >= `round` are voided (stats, round counter, scores).
  int t1 = 0, t2 = 0;
  {
    std::lock_guard<std::recursive_mutex> lk(stats::Mutex());
    stats::MapStats snap = stats::Current().Snapshot();
    stats::RewindTo(&snap, round);
    if (scoreT1 >= 0 && scoreT2 >= 0) {
      snap.team1.score = scoreT1;
      snap.team2.score = scoreT2;
    }
    stats::Current().Restore(snap);
    t1 = snap.team1.score;
    t2 = snap.team2.score;
  }
  MatchStateSetRound(round - 1);
  MatchStateSetScore(t1, t2);
  MatchEventsState es = MatchEventsSnapshot();
  es.roundNumber = round - 1;
  MatchEventsRestore(es);
  MatchLogState ls = MatchLogSnapshot();
  ls.roundNumber = round - 1;
  ls.team1Score = t1;
  ls.team2Score = t2;
  MatchLogRestore(ls);
  {
    Json d = Json::Object();
    d["from_round"] = round;
    d["reason"] = reason;
    signals::Emit("rounds_voided", std::move(d), round, mapNumber);
    Json r = Json::Object();
    r["map_number"] = mapNumber;
    r["round"] = round;
    r["backup_sha256"] = sha;
    r["file"] = file;
    if (extra.IsObject()) {
      for (const auto& kv : extra.Members()) r[kv.first] = kv.second;
    }
    signals::Emit("match_restored", std::move(r), round, mapNumber);
  }
  Print("fleet: restored map %d round %d from %s (sha256 %s, %s)\n", mapNumber, round, file.c_str(), sha.c_str(),
        reason.c_str());
  return round_restore::AfterRestore(mapNumber, round, file, reason);
}

// ---------------------------------------------------------------------------- loading

void BeginLoad() {
  g_handOverPending = false;
  std::vector<std::string> dropped;
  const Json mat = fs::AssignToMatConfig(g_asg.match_id, g_config, &dropped);
  for (const auto& d : dropped) Print("fleet: match %s: cvar %s ignored (engine cvars only)\n", g_asg.match_id.c_str(), d.c_str());
  const std::string matJson = mat.Dump();
  std::string err;
  auto ctx = ParseWebhookMatchContextFromJson(matJson, &err);
  if (!ctx) {
    Print("fleet: match %s: cannot build the match context: %s\n", g_asg.match_id.c_str(), err.c_str());
    return;
  }
  SetPassword(Str(g_config, "password"));
  const char* cm = g_api ? g_api->current_map(g_api->self) : nullptr;
  g_loadFromMap = cm ? cm : "";
  g_loadMapStarts = g_mapStarts;
  ScrimSetAutoEnabled(false);
  // Map 1, or the map a failover resumes (workshop maps load with host_workshop_map).
  const int first = g_resumeActive ? g_resume.map_number : 1;
  ApplyLoadedMatch(*ctx, matJson, first);
  if (g_resumeActive) {
    // Series state from the platform: maps won and the results of the maps before this one.
    ModesSetSeriesWins(g_resume.series_team1, g_resume.series_team2);
    std::vector<SeededMapResult> done;
    for (const auto& m : g_resume.maps_done) done.push_back(SeededMapResult{m.map_number, m.team1, m.team2, m.winner});
    MatchStatusSeedSeries(ctx->matchid, g_resume.series_team1, g_resume.series_team2, done);
    Print("fleet: match %s resumes on map %d %s (series %d-%d, from epoch %lld)\n", g_asg.match_id.c_str(),
          g_resume.map_number,
          g_resume.round >= 1 ? ("at round " + std::to_string(g_resume.round) + " from " + g_resume.file).c_str()
                              : "from warmup",
          g_resume.series_team1, g_resume.series_team2, g_resume.from_epoch);
  }
  g_loading = true;
  g_loadStarted = g_now;
  g_phaseReason = "assign";
  Print("fleet: match %s epoch %lld loaded (%s vs %s, %zu map(s))\n", g_asg.match_id.c_str(), g_asg.epoch,
        ctx->team1_name.c_str(), ctx->team2_name.c_str(), ctx->maplist.size());
}

void CheckLoaded() {
  if (!g_loading) return;
  const char* cm = g_api ? g_api->current_map(g_api->self) : nullptr;
  const std::string cur = cm ? cm : "";
  // The load always changes map (ApplyLoadedMatch), also onto the map the server is on: done once
  // a map started after it (or the map name changed, after a plugin reload lost the count).
  const bool changed = (!cur.empty() && cur != g_loadFromMap) || g_mapStarts != g_loadMapStarts;
  if (changed || g_now - g_loadStarted > kLoadTimeoutS) {
    // The engine's map start runs before the match flow's warmup; give it a tick to settle.
    if (g_now - g_loadStarted > 1.0) g_loading = false;
  }
}

void ClearAssignment() {
  g_fence.Retire(g_asg.match_id, g_asg.epoch);
  g_asg = fs::Assignment{};
  g_config = Json();
  g_stream.Reset(Json(), 0);
  g_handOverPending = g_loading = g_restoring = g_seriesOver = g_serverReset = false;
  g_latestBackupRound = -1;
  g_lastLive = Json();
  g_pauseSeen = false;
  g_execs.clear();
  g_resume = fs::ResumePlan{};
  g_resumeActive = g_resumeSnapshotPending = g_resumeRestored = false;
  g_resumeRestoreAt = g_resumeUnpauseAt = -1;
  g_backupScanAt = -1;
  signals::SetEnabled(false);
  {
    std::lock_guard<std::mutex> lk(g_backupMu);
    g_sentBackups.clear();
  }
  ScrimSetAutoEnabled(true);
}

// ---------------------------------------------------------------------------- handlers

Json ParsePayload(const ru_fleet_msg* m) {
  Json p;
  if (!m->payload_json || !Json::Parse(m->payload_json, &p) || !p.IsObject()) return Json::Object();
  return p;
}

long long MsgEpoch(const ru_fleet_msg* m, const Json& p) {
  if (m->epoch > 0) return m->epoch;
  return Int(p, "epoch", 0);
}

void OnAssign(const ru_fleet_msg* m) {
  const Json p = ParsePayload(m);
  const std::string ref = m->id ? m->id : "";
  std::string err;
  const long long epoch = MsgEpoch(m, p);
  if (!fs::ValidateAssign(p, &err)) {
    Reply(ref, epoch, Rejected("invalid_config", err));
    return;
  }
  const std::string mid = Str(p, "match_id");
  // Failover resume (§11.3): checked (and an inline backup verified) before anything changes.
  fs::ResumePlan resume;
  if (const Json* rs = p.Find("resume")) {
    std::string code;
    if (!fs::ParseResume(*rs, *p.Find("config"), epoch, &resume, &code, &err)) {
      Reply(ref, epoch, Rejected(code, err));
      return;
    }
  }
  // A finished match the platform did not unassign yet (series over, match unloaded) does not
  // block the next one. Neither does this match's own copy recovered from state.json after a
  // crash when the platform resumes it.
  fs::Assignment cur = g_asg;
  if (cur.active && g_serverReset && cur.match_id != mid) cur.active = false;
  const bool localBusy = LocalMatchActive() && !(resume.present && LocalMatchIs(mid));
  const fs::Verdict v = g_fence.CheckAssign(cur, mid, epoch, localBusy);
  if (v == fs::Verdict::Duplicate) {
    Reply(ref, epoch, Ok());
    return;
  }
  if (v != fs::Verdict::Ok) {
    Reply(ref, epoch,
          Rejected(fs::VerdictCode(v), v == fs::Verdict::Busy
                                           ? "this server has another match (" + (g_asg.active ? g_asg.match_id : std::string("local")) + ")"
                                           : "epoch " + std::to_string(epoch) + " is older than this server's"));
    return;
  }
  if (g_draining) {
    Reply(ref, epoch, Rejected("busy", "this server is draining: it takes no new match"));
    return;
  }
  if (resume.present && resume.round >= 1) {
    const Result r = PrepareResumeBackup(mid, &resume);
    if (r.status != "ok") {
      Reply(ref, epoch, r);
      return;
    }
  }
  const bool sameMatch = g_asg.active && g_asg.match_id == mid;
  if (g_asg.active && !sameMatch) ClearAssignment();
  g_asg.active = true;
  g_asg.match_id = mid;
  g_asg.epoch = epoch;
  g_asg.config_rev = std::max<long long>(1, Int(p, "config_rev", 1));
  g_config = *p.Find("config");
  if (resume.present && !resume.sides.empty()) {
    // The sides the knife round decided (platform's state / resume.sides) replace "knife".
    Json& maps = g_config["maps"];
    Json fixed = Json::Array();
    int n = 0;
    for (const auto& m : maps.Items()) {
      Json c = m;
      if (++n == resume.map_number) c["sides"] = resume.sides;
      fixed.Push(std::move(c));
    }
    maps = std::move(fixed);
  }
  g_fence.Retire(mid, epoch);
  signals::SetEnabled(true);
  Reply(ref, epoch, Ok());
  Print("fleet: assigned match %s epoch %lld (config_rev %lld)%s\n", mid.c_str(), epoch, g_asg.config_rev,
        resume.present ? " with a failover resume" : "");

  if (sameMatch && CtxIsOurs(WebhookGetMatchContext()) && !resume.present) {
    // Same match, newer epoch (§11.4 re-issue): keep playing, adopt the new epoch and config.
    SetPassword(Str(g_config, "password"));
  } else {
    g_seriesOver = g_serverReset = false;
    g_latestBackupRound = -1;
    g_lastLive = Json();
    g_resume = resume;
    g_resumeActive = resume.present;
    g_resumeRestoreAt = g_resumeUnpauseAt = -1;
    g_resumeSnapshotPending = g_resumeRestored = false;
    g_restoring = resume.present && resume.round >= 1;
    if (resume.present && LocalMatchIs(mid)) {
      // This match is loaded already (still running here, or recovered after a crash): the
      // resume reloads it from the platform's state.
      WebhookClearMatchContext();
      (void)EndMatchResetServer();
    }
    // D16: a scrim / pickup ends unreported; everyone not in the match is kicked after 5 s.
    const auto ctx = WebhookGetMatchContext();
    const ReadyUpMode mode = GetMode();
    // A plugin mode (deathmatch) ends like a scrim; the match load then restores competitive.
    const bool scrim = mode == ReadyUpMode::ScrimWarmup || mode == ReadyUpMode::Practice ||
                       mode == ReadyUpMode::External || (ctx && ctx->slug == "scrim");
    bool outsiders = false;
    for (const auto& h : ListHumans()) outsiders = outsiders || (h.steamid64 && !KeepOnHandOver(h.steamid64));
    ScrimSetAutoEnabled(false);
    if (scrim || outsiders) {
      SendToChat(kHandOverMessage);
      if (scrim) {
        if (ctx) WebhookClearMatchContext();
        (void)EndMatchResetServer();
      }
      g_handOverPending = true;
      g_handOverAt = g_now + kHandOverDelayS;
      Print("fleet: hand-over: %s, loading in %.0f s\n", scrim ? "scrim ended" : "non-roster players connected",
            kHandOverDelayS);
    } else {
      BeginLoad();
    }
  }
  const Json st = BuildState();
  g_stream.Reset(st, 0);
  PublishState(g_stream.State());
  SendSnapshot("assign", false);
}

void OnUpdate(const ru_fleet_msg* m) {
  const Json p = ParsePayload(m);
  const std::string ref = m->id ? m->id : "";
  const long long epoch = MsgEpoch(m, p);
  const fs::Verdict v = g_fence.CheckScoped(g_asg, Str(p, "match_id"), epoch);
  if (v != fs::Verdict::Ok) {
    Reply(ref, epoch, Rejected(fs::VerdictCode(v), "match.update for a match / epoch this server does not run"));
    return;
  }
  const long long base = Int(p, "base_config_rev", -1);
  const long long next = Int(p, "config_rev", -1);
  if (!fs::ConfigCas(base, g_asg.config_rev)) {
    Result r = Rejected("conflict", "base_config_rev " + std::to_string(base) + " != server config_rev " +
                                        std::to_string(g_asg.config_rev));
    r.rev = g_asg.config_rev;
    Reply(ref, epoch, r);
    SendSnapshot("request", false);  // §9.3: conflict + the current state
    return;
  }
  if (next <= base) {
    Reply(ref, epoch, Rejected("invalid_update", "config_rev must be greater than base_config_rev"));
    return;
  }
  Json cfg = g_config;
  std::string err;
  bool pwChanged = false;
  const Json* ops = p.Find("ops");
  if (!ops || !fs::ApplyUpdateOps(&cfg, *ops, &err, &pwChanged)) {
    Reply(ref, epoch, Rejected("invalid_update", err.empty() ? "no ops" : err));
    return;
  }
  g_config = std::move(cfg);
  g_asg.config_rev = next;
  // The loaded match context follows the new roster / names / rules; maps and sides the match
  // flow already decided (knife picks) stay.
  const Json mat = fs::AssignToMatConfig(g_asg.match_id, g_config, nullptr);
  auto fresh = ParseWebhookMatchContextFromJson(mat.Dump(), &err);
  auto cur = WebhookGetMatchContext();
  if (fresh && CtxIsOurs(cur)) {
    fresh->maplist = cur->maplist;
    fresh->map_sides = cur->map_sides;
    fresh->num_maps = cur->num_maps;
    WebhookSetMatchContext(*fresh);
    persisted_match_state::PersistActiveMatchJson(mat.Dump());
  }
  if (pwChanged) SetPassword(Str(g_config, "password"));
  Result r = Ok();
  r.rev = g_asg.config_rev;
  Reply(ref, epoch, r);
  Print("fleet: match %s config_rev %lld -> %lld (%zu op(s))\n", g_asg.match_id.c_str(), base, next,
        ops->Items().size());
}

// After a restart the link holds no assignment, but the match plugin runs (or is about to recover,
// match_recovery.h) its own copy of a match from state.json. A match.unassign for that match means
// the platform ended it or moved it to another server while this one was down (failover:
// `superseded`): drop the copy, so this server does not play on with a match that goes on
// elsewhere. True when there was a copy.
bool DropRecoveredCopy(const std::string& mid, long long epoch, const std::string& reason) {
  bool running = false, saved = false;
  if (LocalMatchIs(mid)) {
    WebhookClearMatchContext();
    (void)EndMatchResetServer();  // stops its demo and clears the match in state.json
    running = true;
  } else if (const auto json = persisted_match_state::GetActiveMatchJson()) {
    std::string err;
    const auto ctx = ParseWebhookMatchContextFromJson(*json, &err);
    if (ctx && ctx->matchid == fs::NumericMatchId(mid)) {
      persisted_match_state::ClearActiveMatch();  // not recovered yet: now there is nothing to recover
      saved = true;
    }
  }
  if (!running && !saved) return false;
  match_recovery::Cancel();
  g_fence.Retire(mid, epoch);
  SetPassword("");
  Print("fleet: match %s epoch %lld was unassigned (%s) while this server was down; dropped the copy %s\n",
        mid.c_str(), epoch, reason.c_str(), running ? "it recovered after the restart" : "it would have recovered");
  return true;
}

void OnUnassign(const ru_fleet_msg* m) {
  const Json p = ParsePayload(m);
  const std::string ref = m->id ? m->id : "";
  const long long epoch = MsgEpoch(m, p);
  const std::string mid = Str(p, "match_id");
  if (!g_asg.active && epoch > 0 && epoch == g_fence.Retired(mid)) {
    Reply(ref, epoch, Ok());  // replayed: already unassigned
    return;
  }
  const fs::Verdict v = g_fence.CheckScoped(g_asg, mid, epoch);
  if (v != fs::Verdict::Ok) {
    if (!g_asg.active && v == fs::Verdict::NotAssigned && epoch > 0 &&
        DropRecoveredCopy(mid, epoch, Str(p, "reason", "ended"))) {
      Reply(ref, epoch, Ok());
      PublishState(Json());
      SendSnapshot("request", false);
      return;
    }
    Reply(ref, epoch, Rejected(fs::VerdictCode(v), "match.unassign for a match / epoch this server does not run"));
    return;
  }
  const std::string reason = Str(p, "reason", "ended");
  const std::string kickMsg = Str(p, "kick_message");
  const auto ctx = WebhookGetMatchContext();
  if (CtxIsOurs(ctx)) {
    WebhookClearMatchContext();
    (void)EndMatchResetServer();
  }
  SetPassword("");
  if (!kickMsg.empty()) {
    KickHumans(kickMsg, nullptr);
  } else if (!ListHumans().empty()) {
    int noDemo = 5, d1 = 0, d2 = 0;
    GetSeriesEndKickDelays(&noDemo, &d1, &d2);
    g_kicks.push_back(PendingKick{g_now + std::max(1, noDemo), "Match ended. Thanks for playing!"});
  }
  Reply(ref, epoch, Ok());
  Print("fleet: unassigned match %s epoch %lld (%s)\n", mid.c_str(), epoch, reason.c_str());
  ClearAssignment();
  PublishState(Json());
  SendSnapshot("request", false);
}

// ---- cmd --------------------------------------------------------------------------------------

bool IsLiveMode() { return GetMode() == ReadyUpMode::MatchLive; }
bool IsWarmupMode() { return GetMode() == ReadyUpMode::MatchWarmup && !GoLiveTriggered() && !KnifeIsAwaitingPick(); }

Result CmdPause(const Json& args, const std::string& by) {
  if (!IsLiveMode()) return Rejected("bad_phase", "pause needs a live match");
  if (PauseStateGet().paused) return Rejected("already_paused", "the match is already paused");
  const std::string type = Str(args, "type", "admin");
  if (type != "admin" && type != "technical") return Rejected("bad_args", "type must be admin or technical");
  if (!EnqueueServerCommand("mp_pause_match")) return Failed("engine", "mp_pause_match unavailable");
  PauseStateOnPaused(type.c_str(), by);
  g_phaseReason = "cmd:pause";
  SendToChat(type == "technical" ? "Ready Up: technical pause (tournament admin)." : "Ready Up: paused by a tournament admin.");
  return Ok();
}

Result CmdUnpause(const std::string& by) {
  if (!PauseStateGet().paused) return Rejected("not_paused", "the match is not paused");
  if (!EnqueueServerCommand("mp_unpause_match")) return Failed("engine", "mp_unpause_match unavailable");
  PauseStateOnUnpaused();
  g_unpauseBy = by;
  g_phaseReason = "cmd:unpause";
  SendToChat("Ready Up: unpaused by a tournament admin.");
  return Ok();
}

Result CmdForceReady(const Json& args) {
  if (!IsWarmupMode()) return Rejected("bad_phase", "force_ready works in warmup only");
  const auto ctx = WebhookGetMatchContext();
  if (!ctx) return Rejected("bad_phase", "no match loaded");
  const std::string team = Str(args, "team");
  if (!team.empty() && team != "team1" && team != "team2") return Rejected("bad_args", "team must be team1 or team2");
  int n = 0;
  for (const auto& h : ListHumans()) {
    const auto it = ctx->roster_team.find(h.steamid64);
    if (it == ctx->roster_team.end()) continue;
    if (!team.empty() && (it->second == WebhookTeam::Team1 ? "team1" : "team2") != team) continue;
    if (!SetReady(h.steamid64, true)) ++n;
  }
  ScrimNoteReadyChanged();
  SendToChat("Ready Up: players readied by a tournament admin.");
  Result r = Ok();
  r.message = std::to_string(n) + " player(s) set ready";
  return r;
}

Result CmdRestore(const Json& args, const std::string& by) {
  const auto ctx = WebhookGetMatchContext();
  if (!CtxIsOurs(ctx)) return Rejected("bad_phase", "no match loaded");
  if (g_resumeActive) return Rejected("bad_phase", "the failover resume has not restored its backup yet");
  const ReadyUpMode mode = GetMode();
  if (mode != ReadyUpMode::MatchLive && mode != ReadyUpMode::MatchWarmup) {
    return Rejected("bad_phase", "restore needs warmup or a live map");
  }
  const auto ms = MatchStateGet();
  const int mapNumber = static_cast<int>(Int(args, "map_number", ms.map_number));
  const int round = static_cast<int>(Int(args, "round", 0));
  if (mapNumber != ms.map_number) return Rejected("bad_phase", "the backup is for map " + std::to_string(mapNumber) +
                                                                  ", the server is on map " + std::to_string(ms.map_number));
  if (round < 1) return Rejected("bad_args", "round must be >= 1");
  std::string file, sha;
  if (const Json* b = args.Find("backup"); b && b->IsObject()) {
    if (Int(*b, "parts", 1) > 1) return Rejected("unsupported", "multi-part inline backups are not accepted yet");
    file = Str(*b, "file");
    if (!fs::SafeBackupFileName(file)) return Rejected("bad_args", "backup.file is not a plain *.txt name");
    std::string raw;
    if (Str(*b, "encoding", "base64") != "base64" || !fs::Base64Decode(Str(*b, "data"), &raw)) {
      return Rejected("bad_args", "backup.data is not base64");
    }
    sha = fs::Sha256Hex(raw);
    if (sha != Str(*b, "sha256")) return Rejected("checksum", "backup sha256 mismatch");
    if (Int(*b, "size", static_cast<long long>(raw.size())) != static_cast<long long>(raw.size())) {
      return Rejected("checksum", "backup size mismatch");
    }
    const Result w = WriteBackupFile(file, raw);  // it came from the platform
    if (w.status != "ok") return w;
  } else {
    std::string dir;
    file = FindLocalBackup(mapNumber, round, &dir);
    if (file.empty()) return Rejected("no_backup", "no local backup for map " + std::to_string(mapNumber) + " round " +
                                                       std::to_string(round) + "; send it inline");
    std::ifstream in(dir + "/" + file, std::ios::binary);
    std::ostringstream ss;
    ss << in.rdbuf();
    sha = fs::Sha256Hex(ss.str());
  }
  const bool autoUnpause = DoRestore(mapNumber, round, file, sha, by, "restore", Json::Object());
  g_phaseReason = "cmd:restore_round";
  SendToChat(("Ready Up: round " + std::to_string(round) + " restored by a tournament admin. " +
              (autoUnpause ? "Live in 3 seconds." : "Unpause when ready."))
                 .c_str());
  return Ok();
}

// The resume's go-live happened: seed the stats model with the platform's view of the map (or
// the backup's score), then restore the backup like restore_round.
void RunResumeRestore() {
  {
    std::lock_guard<std::recursive_mutex> lk(stats::Mutex());
    stats::MapStats seed;
    const bool have = !g_resume.map_stats.IsNull() && stats::FromJson(g_resume.map_stats.Dump(), &seed);
    if (!have) {
      seed = stats::Current().Snapshot();
      seed.rounds.clear();
      seed.team1 = stats::TeamLine{};
      seed.team2 = stats::TeamLine{};
      if (g_resume.score_team1 >= 0) {
        seed.team1.score = g_resume.score_team1;
        seed.team2.score = g_resume.score_team2;
      }
    }
    seed.live = true;
    if (!g_resume.team1_side.empty()) seed.team1_is_ct = g_resume.team1_side == "ct";
    stats::Current().Restore(seed);
  }
  Json extra = Json::Object();
  extra["resume"] = true;
  if (g_resume.from_epoch > 0) extra["from_epoch"] = g_resume.from_epoch;
  DoRestore(g_resume.map_number, g_resume.round, g_resume.file, g_resume.sha256, "platform:resume", "resume", extra,
            g_resume.score_team1, g_resume.score_team2);
  g_phaseReason = "resume";
  // Everyone readied up in warmup to get here, so the resume goes live by itself: `.r` is the
  // confirmation, no `.unpause` after it (live test). pause_after_restore still holds an admin
  // restore_round (a restore pause both teams lift).
  if (true) {
    g_resumeUnpauseAt = g_now + 3.0;
    SendToChat(("Ready Up: match moved here and restored at round " + std::to_string(g_resume.round) +
                ". Live in 3 seconds.").c_str());
  } else {
    SendToChat(("Ready Up: match moved here and restored at round " + std::to_string(g_resume.round) +
                ". Unpause when ready.").c_str());
  }
}

Result CmdEnd(const Json& args) {
  const auto ctx = WebhookGetMatchContext();
  if (!CtxIsOurs(ctx)) return Rejected("bad_phase", "no match loaded");
  const std::string winner = Str(args, "winner", "none");
  if (winner != "none" && winner != "team1" && winner != "team2") return Rejected("bad_args", "winner must be team1 or team2");
  const std::string reason = Str(args, "reason", "admin");
  Json d = Json::Object();
  d["type"] = "series_end";
  d["winner"] = winner;
  d["reason"] = reason;
  d["forced"] = true;
  d["team1_series_score"] = Int(Copy(Path(g_stream.State(), {"series", "score"})), "team1", 0);
  d["team2_series_score"] = Int(Copy(Path(g_stream.State(), {"series", "score"})), "team2", 0);
  d["seconds_until_reset"] = 0;
  signals::Emit("series_end", std::move(d));
  WebhookEmitSeriesEnd(0, 0, winner.c_str(), 0);
  WebhookClearMatchContext();
  (void)EndMatchResetServer();
  g_seriesOver = true;
  g_phaseReason = "cmd:end_match";
  SendToChat("Ready Up: the match was ended by a tournament admin.");
  return Ok();
}

Result CmdChangeMap(const Json& args) {
  if (!IsWarmupMode()) return Rejected("bad_phase", "change_map works before the map goes live only");
  auto ctx = WebhookGetMatchContext();
  if (!CtxIsOurs(ctx)) return Rejected("bad_phase", "no match loaded");
  // A map entry: a name, a workshop id ("123", "ws:123", "workshop/123[/name]") or name +
  // workshop_id (map_names.h).
  std::string entry;
  const long long n = Int(args, "map_number", 0);
  if (n > 0) {
    const Json* maps = g_config.Find("maps");
    if (!maps || n > static_cast<long long>(maps->Items().size())) return Rejected("bad_args", "no such map_number");
    const Json& m = maps->Items()[static_cast<size_t>(n - 1)];
    entry = mapnames::MakeEntry(Str(m, "name"), Str(m, "workshop_id"));
  } else {
    const std::string name = Str(args, "name");
    const std::string ws = Str(args, "workshop_id");
    if ((!name.empty() && !fs::SafeMapName(name)) || (!ws.empty() && !fs::SafeWorkshopId(ws))) {
      return Rejected("bad_args", "bad map");
    }
    entry = mapnames::MakeEntry(name, ws);
    if (entry.empty()) return Rejected("bad_args", "name or workshop_id needed");
    const auto ms = MatchStateGet();
    const size_t idx = static_cast<size_t>(std::max(1, ms.map_number) - 1);
    if (idx < ctx->maplist.size()) {
      ctx->maplist[idx] = entry;
      WebhookSetMatchContext(*ctx);
    }
  }
  if (!LoadMapEntry(entry)) return Rejected("bad_args", "bad map");
  g_phaseReason = "cmd:change_map";
  return Ok();
}

Result CmdSwapTeams() {
  if (!IsWarmupMode()) return Rejected("bad_phase", "swap_teams works in warmup only");
  const auto ctx = WebhookGetMatchContext();
  if (!CtxIsOurs(ctx)) return Rejected("bad_phase", "no match loaded");
  std::string why;
  if (!MatchFeaturesSwapTeams(&why)) return Rejected("bad_phase", why);  // the same as `.switch`
  return Ok();
}

Result CmdKick(const Json& args) {
  const std::string sid = Str(args, "steamid64");
  std::string msg = Str(args, "message", "Kicked by a tournament admin");
  for (const auto& h : ListHumans()) {
    if (std::to_string(h.steamid64) != sid || h.userid < 0) continue;
    for (char& c : msg) {
      if (c == '"' || c == ';' || c == '\n' || c == '\r') c = ' ';
    }
    (void)EnqueueServerCommand(("kickid " + std::to_string(h.userid) + " \"" + msg + "\"").c_str());
    return Ok();
  }
  return Rejected("not_connected", "no connected player " + sid);
}

// A JSON scalar as the text a console setting takes (true -> "1").
bool SettingText(const Json& v, std::string* out) {
  switch (v.type()) {
    case Json::Type::Bool: *out = v.AsBool() ? "1" : "0"; return true;
    case Json::Type::Int: *out = std::to_string(v.AsInt()); return true;
    case Json::Type::String: *out = v.AsString(); return true;
    default: return false;
  }
}

// `settings.set` {settings: {<name>: value, ...}}: server settings (match_settings.h), all or none;
// an empty object only reads them. cmd.result.output: the settings after the change.
Result CmdSettingsSet(const Json& args, const std::string& by) {
  const Json* set = args.Find("settings");
  if (set && !set->IsObject()) return Rejected("bad_args", "settings must be an object");
  std::vector<std::pair<std::string, std::string>> todo;
  if (set) {
    for (const auto& kv : set->Members()) {
      std::string v, err;
      if (!SettingText(kv.second, &v)) return Rejected("bad_args", kv.first + ": a boolean, number or string");
      if (!match_settings::Validate(kv.first, v, &err)) return Rejected("bad_args", err);
      todo.emplace_back(kv.first, v);
    }
  }
  for (const auto& t : todo) {
    std::string reply;
    if (!match_settings::Set(t.first, t.second, by, &reply)) return Failed("engine", reply);
  }
  Result r = Ok();
  for (const auto& l : match_settings::ShowLines()) r.output += l + "\n";
  r.haveOutput = true;
  return r;
}

// server.config {rev, settings} (FLEET.md §7.5): fleetstate::PlanServerConfig says what each field
// sets. Server settings and console settings are saved in state.json like a console change (a
// later console / chat change wins until the next server.config). offline_pause_minutes and
// status_http.token are fleet.so's (fleet_plugin.cpp).
void OnServerConfig(const ru_fleet_msg* m) {
  const Json p = ParsePayload(m);
  const Json* st = p.Find("settings");
  if (!st || !st->IsObject()) {
    Print("fleet: server.config ignored (no settings)\n");
    return;
  }
  const long long rev = Int(p, "rev", 0);
  const std::string by = "platform:server.config rev " + std::to_string(rev);
  const fs::ServerConfigPlan plan = fs::PlanServerConfig(*st);
  int n = 0;
  for (const auto& kv : plan.settings) {
    std::string reply;
    if (match_settings::Set(kv.first, kv.second, by, &reply)) ++n;
    else Print("fleet: server.config %s: %s\n", kv.first.c_str(), reply.c_str());
  }
  for (const auto& kv : plan.console) {
    if (persisted_settings::ApplyConsoleSetting(kv.first, kv.second)) ++n;
    else Print("fleet: server.config %s: value refused\n", kv.first.c_str());
  }
  for (const auto& f : plan.skipped) Print("fleet: server.config %s: wrong type, skipped\n", f.c_str());
  Print("fleet: server.config rev %lld: %d setting(s) applied\n", rev, n);
}

void OnDrain(bool drain, const ru_fleet_msg* m) {
  const Json p = ParsePayload(m);
  const std::string reason = Str(p, "reason");
  if (g_draining == drain) return;
  g_draining = drain;
  Print("fleet: %s%s%s\n", drain ? "server.drain" : "server.undrain", reason.empty() ? "" : ": ", reason.c_str());
  PublishState(g_asg.active && g_stream.HasState() ? g_stream.State() : Json());  // sends server.availability
}

// ---- CS2 build check: Steam's UpToDateCheck on a worker thread, `server.cs2_update_required
// {required_build}` once per required version (cs2_update_check.h).
constexpr double kCs2FirstCheckS = 60.0;
constexpr double kCs2CheckEveryS = 1800.0;
std::atomic<bool> g_cs2Running{false};
std::mutex g_cs2Mu;
cs2update::Answer g_cs2Answer;
bool g_cs2AnswerNew = false;
double g_nextCs2Check = 0;
long long g_cs2Reported = 0;

void CheckCs2Update(double now, bool active) {
  cs2update::Answer a;
  bool have = false;
  {
    std::lock_guard<std::mutex> lk(g_cs2Mu);
    if (g_cs2AnswerNew) {
      a = g_cs2Answer;
      have = true;
      g_cs2AnswerNew = false;
    }
  }
  if (have && active && cs2update::ShouldReport(a, g_cs2Reported)) {
    g_cs2Reported = a.required;
    Json p = Json::Object();
    p["required_build"] = static_cast<int>(a.required);
    Send("server.cs2_update_required", p, 0, true);
    Print("fleet: CS2 update required (build %lld)\n", a.required);
  }
  if (!active || g_cs2Running.load()) return;
  if (g_nextCs2Check == 0) g_nextCs2Check = now + kCs2FirstCheckS;
  if (now < g_nextCs2Check) return;
  g_nextCs2Check = now + kCs2CheckEveryS;
  const Cs2VersionSnapshot snap = GetCs2VersionSnapshot();
  if (!snap.version_string || snap.version_string->rfind("Patchversion", 0) != 0) return;  // no PatchVersion in steam.inf
  const long long cur = cs2update::PatchVersionNumber(*snap.version_string);
  if (cur <= 0) return;
  g_cs2Running = true;
  if (!workers::Spawn("cs2-update-check", [cur]() {
        const HttpResponse r = HttpGet(cs2update::CheckUrl(cur), std::nullopt);
        if (r.status == 200) {
          const cs2update::Answer ans = cs2update::ParseAnswer(r.body);
          std::lock_guard<std::mutex> lk(g_cs2Mu);
          g_cs2Answer = ans;
          g_cs2AnswerNew = true;
        }
        g_cs2Running = false;
      })) {
    g_cs2Running = false;
  }
}

void OnCmd(const ru_fleet_msg* m) {
  const Json p = ParsePayload(m);
  const std::string ref = m->id ? m->id : "";
  const long long epoch = MsgEpoch(m, p);
  const std::string name = Str(p, "name");
  const Json args = p.Find("args") && p.Find("args")->IsObject() ? *p.Find("args") : Json::Object();
  const Json issued = p.Find("issued_by") && p.Find("issued_by")->IsObject() ? *p.Find("issued_by") : Json::Object();
  const std::string auditId = Str(p, "audit_id");
  const std::string by = "platform:" + Str(issued, "user_id", "?");
  const long long expires = Int(p, "expires_at", 0);
  if (expires > 0 && UnixMs() > expires) {
    Result r;
    r.status = "expired";
    Reply(ref, epoch, r, auditId);
    return;
  }
  static const std::set<std::string> kMatchCmds = {"pause",      "unpause",   "force_ready", "start",
                                                   "restore_round", "restart_round", "restart_map", "end_match",
                                                   "end",        "change_map", "swap_teams"};
  const std::string mid = Str(p, "match_id");
  if (kMatchCmds.count(name) || !mid.empty()) {
    if (mid.empty()) {
      Reply(ref, epoch, Rejected("bad_request", name + " needs match_id and epoch"), auditId);
      return;
    }
    const fs::Verdict v = g_fence.CheckScoped(g_asg, mid, epoch);
    if (v != fs::Verdict::Ok) {
      Reply(ref, epoch, Rejected(fs::VerdictCode(v), "cmd for a match / epoch this server does not run"), auditId);
      return;
    }
  }
  Print("fleet: cmd %s by %s (%s)%s%s\n", name.c_str(), Str(issued, "name", "?").c_str(), by.c_str(),
        auditId.empty() ? "" : " audit ", auditId.c_str());
  Result r;
  if (name == "pause") {
    r = CmdPause(args, by);
  } else if (name == "unpause") {
    r = CmdUnpause(by);
  } else if (name == "force_ready") {
    r = CmdForceReady(args);
  } else if (name == "start") {
    // args.force: also under the valve ruleset without GOTV (esports.h).
    const bool force = Bool(args, "force");
    if (!IsWarmupMode()) r = Rejected("bad_phase", "start works in warmup only");
    else if (ForceStartMatch(force)) r = Ok();
    else if (!force && LiveCfgRequired() && EsportsGotvState() == GotvState::Down)
      r = Rejected("gotv_off", "GOTV is off on this map (valve ruleset): reload the map, or start with {\"force\": true}");
    else r = Failed("engine", "force start failed");
    if (r.status == "ok") g_phaseReason = "cmd:start";
  } else if (name == "restore_round" || name == "restart_round") {
    r = CmdRestore(args, by);
  } else if (name == "restart_map") {
    if (!CtxIsOurs(WebhookGetMatchContext())) {
      r = Rejected("bad_phase", "no match loaded");
    } else {
      (void)RestartMatch();
      Json d = Json::Object();
      d["from_round"] = 1;
      d["reason"] = "restart_map";
      signals::Emit("rounds_voided", std::move(d), 1);
      g_phaseReason = "cmd:restart_map";
      r = Ok();
    }
  } else if (name == "end_match" || name == "end") {
    r = CmdEnd(args);
  } else if (name == "change_map") {
    r = CmdChangeMap(args);
  } else if (name == "swap_teams") {
    r = CmdSwapTeams();
  } else if (name == "kick") {
    r = CmdKick(args);
  } else if (name == "settings.set") {
    r = CmdSettingsSet(args, by);
  } else if (name == "snapshot_now") {
    r = SendSnapshot("request", true) ? Ok() : Failed("offline", "not connected");
  } else if (name == "exec") {
    std::string err;
    if (!Bool(issued, "root")) {
      r = Rejected("forbidden", "exec is for root admins only (D10)");
    } else if (!fs::ValidateExec(Str(args, "command"), &err)) {
      r = Rejected("invalid_command", err);
    } else {
      const std::string cmd = Str(args, "command");
      Print("fleet: exec by %s (%s, audit %s): %s\n", Str(issued, "name", "?").c_str(), by.c_str(),
            auditId.empty() ? "-" : auditId.c_str(), cmd.c_str());
      if (!EnqueueServerCommand(cmd.c_str())) {
        r = Failed("engine", "the command could not be queued");
      } else {
        g_execs.push_back(ExecCapture{ref, auditId, epoch, g_now + kExecCaptureS, {}});
        return;  // cmd.result with the captured output from Tick()
      }
    }
  } else {
    r = Rejected("unknown_command", "unknown cmd \"" + name + "\"");
  }
  Reply(ref, epoch, r, auditId);
}

void OnOfflineTimeout(const ru_fleet_msg*) {
  if (!g_asg.active) return;
  const auto ctx = WebhookGetMatchContext();
  if (!CtxIsOurs(ctx) || !IsLiveMode() || PauseStateGet().paused) {
    Print("fleet: offline timer fired; no live match to pause\n");
    return;
  }
  if (!EnqueueServerCommand("mp_pause_match")) return;
  PauseStateOnPaused("offline", "platform-link");
  g_phaseReason = "offline";
  SendToChat("Paused: server lost contact with the tournament platform");
  Print("fleet: auto-paused match %s (platform offline, D12)\n", g_asg.match_id.c_str());
}

void OnConnection(const ru_fleet_msg* m) {
  Json p = ParsePayload(m);
  if (Str(p, "state") != "online") {
    g_logRelay.Stop();
    return;
  }
  // §8.2: the platform gets the current state on every connect (hello carries it too; this one
  // also has map_stats).
  if (g_asg.active) SendSnapshot("hello", true);
}

// admins.set {rev, admins: [{steamid64, name}]}: the whole fleet-wide list (D5), replaces the
// previous one; cached in fleet-admins.json (local_store.h) for offline boots.
void OnAdminsSet(const ru_fleet_msg* m) {
  const Json p = ParsePayload(m);
  const long long rev = Int(p, "rev", -1);
  const Json* arr = p.Find("admins");
  if (rev < 0 || !arr || arr->type() != Json::Type::Array) {
    Print("fleet: admins.set ignored (no rev / admins)\n");
    return;
  }
  std::vector<local_store::Admin> admins;
  for (const Json& a : arr->Items()) {
    if (!a.IsObject()) continue;
    const Json* sid = a.Find("steamid64");
    uint64_t id = 0;
    if (sid && sid->type() == Json::Type::String) id = std::strtoull(sid->AsString().c_str(), nullptr, 10);
    else if (sid && sid->type() == Json::Type::Int && sid->AsInt() > 0) id = static_cast<uint64_t>(sid->AsInt());
    if (id != 0) admins.push_back(local_store::Admin{id, Str(a, "name")});
  }
  const size_t n = admins.size();
  if (local_store::SetFleetAdmins(rev, std::move(admins))) {
    PublishAdminsRev();
    Print("fleet: admins.set rev %lld: %zu admin(s)\n", rev, n);
  } else {
    Print("fleet: admins.set rev %lld is older than the stored list; kept it\n", rev);
  }
}

void OnLogsStart(const ru_fleet_msg* m) { g_logRelay.Start(g_now, Bool(ParsePayload(m), "backlog")); }

void SendLogBatches() {
  while (auto b = g_logRelay.Take(g_now)) {
    Json lines = Json::Array();
    for (const auto& l : b->lines) lines.Push(l);
    Json p = Json::Object();
    p["lines"] = std::move(lines);
    p["backlog"] = b->backlog;
    p["dropped"] = b->dropped;
    Send("logs.lines", p, 0, false);
  }
}

void OnMessage(void*, const ru_fleet_msg* m) {
  if (!m || !m->type) return;
  try {
    const std::string t = m->type;
    if (t == "match.assign") OnAssign(m);
    else if (t == "match.update") OnUpdate(m);
    else if (t == "match.unassign") OnUnassign(m);
    else if (t == "cmd") OnCmd(m);
    else if (t == "local.offline_timeout") OnOfflineTimeout(m);
    else if (t == "local.connection") OnConnection(m);
    else if (t == "admins.set") OnAdminsSet(m);
    else if (t == "server.config") OnServerConfig(m);
    else if (t == "server.drain") OnDrain(true, m);
    else if (t == "server.undrain") OnDrain(false, m);
    else if (t == "logs.start") OnLogsStart(m);
    else if (t == "logs.stop") g_logRelay.Stop();
  } catch (const std::exception& e) {
    Print("fleet: handling %s threw: %s\n", m->type, e.what());
  }
}

void EnsureHandlers() {
  const ru_fleet_v1* f = Fleet();
  local_store::SetFleetMode(FleetActive(f));
  if (!f) {
    g_fleetInstance = 0;
    g_handlerIds.clear();
    return;
  }
  const uint64_t inst = f->instance_id();
  if (inst == g_fleetInstance) return;
  g_fleetInstance = inst;
  g_handlerIds.clear();  // a new fleet.so image has no registrations
  for (const char* type : {"match.assign", "match.update", "match.unassign", "cmd", "local.offline_timeout",
                           "local.connection", "admins.set", "server.config", "server.drain",
                           "server.undrain", "logs.start", "logs.stop"}) {
    const uint64_t id = f->register_handler(type, &OnMessage, nullptr);
    if (id) g_handlerIds.push_back(id);
  }
  f->add_capability("match.v1");
  f->add_capability("match.backup.v1");
  f->add_capability("match.resume.v1");
  f->add_capability("maps.workshop.v1");
  f->add_capability("mode.wingman.v1");
  f->add_capability("mode.simulation.v1");
  f->add_capability("logs.v1");
  PublishAdminsRev();
  g_lastPublishedAvail.clear();
  if (g_asg.active) {
    PublishState(g_stream.State());
    SendSnapshot("hello", true);
  }
}

// ---------------------------------------------------------------------------- listeners

void OnFlow(const MatchFlowEvent& e) {
  if (!signals::Enabled() || e.scrim) return;
  // Called with the modes mutex held: only queue (match_signals.h).
  Json d;
  if (e.type == MatchFlowEventType::ServerReset) {
    signals::Emit("server_reset", Json::Object());
    return;
  }
  if (!Json::Parse(ToJson(e), &d)) return;
  signals::Emit(e.type == MatchFlowEventType::MapResult ? "map_result" : "series_end", std::move(d), -1,
                e.type == MatchFlowEventType::MapResult ? e.mapNumber : -1);
}

void OnDemo(const demo::DemoEvent& e) {
  if (!signals::Enabled()) return;  // any thread
  Json d;
  if (!Json::Parse(demo::ToJson(e), &d)) return;
  signals::Emit("demo", std::move(d), -1, e.mapNumber);
}

}  // namespace

// ---------------------------------------------------------------------------- public

// Demo streaming (FLEET.md §12.2): the GOTV demo of a platform match goes to fleet.so, which
// streams it while it records. Anything else (no fleet link, not enrolled, a local match, an
// older fleet.so) returns false and the demo is handled as without a platform.
static bool StreamDemoBegin(const demo::RecordingInfo& info, const std::string& absPath, const std::string&) {
  const ru_fleet_v1* f = Fleet();
  if (!FleetActive(f) || !FLEET_HAS(f, demo_stream_begin) || !f->demo_stream_begin) return false;
  if (!g_asg.active || info.matchid != static_cast<long long>(fs::NumericMatchId(g_asg.match_id))) return false;
  Json spec = Json::Object();
  spec["match_id"] = g_asg.match_id;
  spec["epoch"] = g_asg.epoch;
  spec["map_number"] = info.mapNumber;
  spec["path"] = absPath;
  spec["started_at"] = static_cast<long long>(std::time(nullptr)) * 1000;
  return f->demo_stream_begin(spec.Dump().c_str()) == 1;
}

static void StreamDemoEnd(const std::string& absPath) {
  const ru_fleet_v1* f = Fleet();
  if (f && FLEET_HAS(f, demo_stream_end) && f->demo_stream_end) (void)f->demo_stream_end(absPath.c_str());
}

void Install(const ru_api* api) {
  g_api = api;
  AddMatchFlowListener(&OnFlow);
  demo::AddListener(&OnDemo);
  demo::SetStreamHooks(demo::StreamHooks{&StreamDemoBegin, &StreamDemoEnd});
}

void Uninstall() {
  demo::SetStreamHooks(demo::StreamHooks{});
  const ru_fleet_v1* f = Fleet();
  if (f && f->instance_id() == g_fleetInstance) {
    for (uint64_t id : g_handlerIds) f->unregister_handler(id);
  }
  g_handlerIds.clear();
  g_fleetInstance = 0;
  signals::SetEnabled(false);
  g_api = nullptr;
}

void OnMapStart() { ++g_mapStarts; }

void OnCoreEvent(const ru_event* e) {
  if (!g_asg.active || !e || !e->steamid64 || IsDevBotId(e->steamid64)) return;
  const std::string sid = std::to_string(e->steamid64);
  if (e->type == RU_EVENT_PLAYER_CONNECT || e->type == RU_EVENT_PLAYER_DISCONNECT) {
    Json d = Json::Object();
    d["steamid64"] = sid;
    d["name"] = e->name ? e->name : "";
    d["team"] = TeamOfSid(e->steamid64);
    if (e->type == RU_EVENT_PLAYER_DISCONNECT) d["reason"] = std::to_string(e->reason);
    signals::Emit(e->type == RU_EVENT_PLAYER_CONNECT ? "player_connect" : "player_disconnect", std::move(d));
  } else if (e->type == RU_EVENT_PLAYER_TEAM) {
    Json d = Json::Object();
    d["steamid64"] = sid;
    d["team"] = TeamOfSid(e->steamid64);
    d["side"] = e->team == 3 ? "ct" : e->team == 2 ? "t" : e->team == 1 ? "spectator" : "none";
    signals::Emit("player_team", std::move(d));
  }
}

void OnLogLine(const char* line) {
  if (!line) return;
  g_logRelay.Push(line, g_now);
  if (g_execs.empty()) return;
  for (auto& x : g_execs) {
    if (x.output.size() >= kExecOutputMax) continue;
    x.output.append(line);
    if (x.output.empty() || x.output.back() != '\n') x.output.push_back('\n');
    if (x.output.size() > kExecOutputMax) x.output.resize(kExecOutputMax);
  }
}

void Tick(double now) {
  g_now = now;
  EnsureHandlers();
  RunDeferredRestore();
  const ru_fleet_v1* f = Fleet();
  CheckCs2Update(now, FleetActive(f));
  SendLogBatches();
  if (!g_asg.active) {
    (void)signals::Take();
    if (f && now - g_lastIdlePublish >= 1.0) {
      g_lastIdlePublish = now;
      if (g_lastPublishedAvail != Availability()) PublishState(Json());
    }
    // Pending kicks after an unassign still run.
    for (auto it = g_kicks.begin(); it != g_kicks.end();) {
      if (now < it->at) {
        ++it;
        continue;
      }
      KickHumans(it->message, [](uint64_t sid) { return IsReadyUpAdmin(sid); });
      it = g_kicks.erase(it);
    }
    return;
  }

  // Deferred work.
  if (g_handOverPending && now >= g_handOverAt) {
    KickHumans(kHandOverMessage, &KeepOnHandOver);
    BeginLoad();
  }
  CheckLoaded();
  for (auto it = g_execs.begin(); it != g_execs.end();) {
    if (now < it->until) {
      ++it;
      continue;
    }
    Result r = Ok();
    r.output = it->output;
    r.haveOutput = true;
    Reply(it->ref, it->epoch, r, it->auditId);
    it = g_execs.erase(it);
  }
  if (g_backupScanAt >= 0 && now >= g_backupScanAt) {
    g_backupScanAt = -1;
    RunBackupScan();
  }
  if (g_resumeActive && !g_loading && !g_handOverPending) {
    // The resumed map is map N whatever map tracking derived from the map list.
    const auto ms = MatchStateGet();
    if (ms.map_number != g_resume.map_number && CtxIsOurs(WebhookGetMatchContext())) {
      MatchLogState ls = MatchLogSnapshot();
      ls.mapNumber = g_resume.map_number;
      MatchLogRestore(ls);
      MatchStateSetMap(g_resume.map_number, ms.current_map);
    }
    if (g_resume.round < 1) {
      // Restart of the map from warmup: nothing to restore; done once the map left warmup.
      if (GetMode() != ReadyUpMode::MatchWarmup) g_resumeActive = false;
    } else if (!g_resumeRestored && IsLiveMode() && g_resumeRestoreAt < 0) {
      g_resumeRestoreAt = now + 1.0;  // the go-live round started: restore the backup into it
    } else if (g_resumeRestoreAt >= 0 && now >= g_resumeRestoreAt) {
      g_resumeRestoreAt = -1;
      g_resumeRestored = true;
      RunResumeRestore();
    }
  }
  if (g_resumeUnpauseAt >= 0 && now >= g_resumeUnpauseAt) {
    g_resumeUnpauseAt = -1;
    if (PauseStateGet().paused && EnqueueServerCommand("mp_unpause_match")) {
      PauseStateOnUnpaused();
      g_unpauseBy = "resume";
      g_phaseReason = "resume";
    }
  }

  std::vector<signals::Signal> sigs = signals::Take();
  if (sigs.empty() && now - g_lastPoll < kPollIntervalS) return;
  g_lastPoll = now;

  std::vector<Ev> evs;
  for (auto& s : sigs) {
    if (s.type == "server_reset") {
      g_serverReset = true;
      continue;
    }
    // Resume: the go-live round before the backup is restored is not reported.
    if (g_resumeActive && g_restoring &&
        (s.type == "round_start" || s.type == "round_end" || s.type == "backup" || s.type == "halftime")) {
      continue;
    }
    if (s.type == "backup") {
      g_latestBackupRound = std::max(g_latestBackupRound, s.round);
    } else if (s.type == "series_end") {
      g_seriesOver = true;
    } else if (s.type == "round_start") {
      ScheduleBackupScan(1.5);  // CS2 (re)writes the backup of the rounds played at the round start
      std::lock_guard<std::recursive_mutex> lk(stats::Mutex());
      if (stats::Current().Live()) {
        s.data["score"]["team1"] = stats::Current().Team1Score();
        s.data["score"]["team2"] = stats::Current().Team2Score();
      }
    } else if (s.type == "match_restored") {
      g_restoring = false;
      if (g_resumeActive) {
        g_resumeActive = false;
        g_resumeSnapshotPending = true;
      }
    } else if (s.type == "knife_result") {
      const KnifeHudInfo k = KnifeHudSnapshot();
      s.data["reason"] = k.reasonShort == "more alive"  ? "alive"
                         : k.reasonShort == "more HP"   ? "hp"
                         : k.reasonShort == "coin flip" ? "coin"
                                                        : "elimination";
    }
    evs.push_back(Ev{s.type, std::move(s.data), s.round, s.map_number});
  }
  const Json st = BuildState();
  Derive(g_stream.State(), st, &evs);
  EmitAll(st, evs);
  PublishState(g_stream.State());
  if (g_resumeSnapshotPending) {
    g_resumeSnapshotPending = false;
    SendSnapshot("restored", true);  // §11.3: the platform's state after the resume
  }

  const std::string phase = Str(st, "phase");
  const bool running = phase != "series_end" && phase != "loading";
  if (running && now - g_lastPeriodic >= kPeriodicSnapshotS) {
    g_lastPeriodic = now;
    SendSnapshot("periodic", true);
  }
}

bool Assigned() { return g_asg.active; }

bool RestoreRoundFromLocalBackup(int round, const std::string& by, const std::string& reason, std::string* err,
                                 bool* autoUnpause) {
  const auto ctx = WebhookGetMatchContext();
  if (!ctx) {
    if (err) *err = "no match loaded";
    return false;
  }
  if (round < 1) {
    if (err) *err = "round must be >= 1";
    return false;
  }
  const int mapNumber = std::max(1, MatchStateGet().map_number);
  const std::string prefix = "readyup_backup_" + std::to_string(static_cast<unsigned long long>(ctx->matchid)) + "_map" +
                             std::to_string(mapNumber) + "_";
  std::string dir;
  const std::string file = FindLocalBackup(mapNumber, round, &dir, prefix);
  if (file.empty()) {
    if (err) *err = "no backup " + prefix + "round" + (round - 1 < 10 ? "0" : "") + std::to_string(round - 1) + ".txt";
    return false;
  }
  std::ifstream in(dir + "/" + file, std::ios::binary);
  std::ostringstream ss;
  ss << in.rdbuf();
  const bool au = DoRestore(mapNumber, round, file, fs::Sha256Hex(ss.str()), by, reason, Json::Object());
  if (autoUnpause) *autoUnpause = au;
  if (g_asg.active) g_phaseReason = "vote:" + reason;
  else g_restoring = false;  // only the fleet link reads (and clears) it
  return true;
}

bool CurrentState(Json* out) {
  if (!g_asg.active || !g_stream.HasState()) return false;
  if (out) *out = g_stream.State();
  return true;
}

bool UpdateSafe() {
  if (!g_asg.active) return true;
  return g_serverReset;
}

bool DemosUnconfirmed() { return UpdateBlocked(); }

Json SnapshotJson() {
  Json j = Json::Object();
  j["active"] = g_asg.active;
  j["match_id"] = g_asg.match_id;
  j["epoch"] = g_asg.epoch;
  j["config_rev"] = g_asg.config_rev;
  j["config"] = g_config;
  j["fence"] = g_fence.ToJson();
  j["live_rev"] = g_stream.Rev();
  j["state"] = g_stream.State();
  j["loading"] = g_loading || g_handOverPending;
  j["series_over"] = g_seriesOver;
  j["server_reset"] = g_serverReset;
  j["latest_backup_round"] = g_latestBackupRound;
  j["last_live"] = g_lastLive;
  Json sent = Json::Array();
  {
    std::lock_guard<std::mutex> lk(g_backupMu);
    for (const auto& s : g_sentBackups) sent.Push(s);
  }
  j["sent_backups"] = std::move(sent);
  j["resume_active"] = g_resumeActive;
  j["resume_restored"] = g_resumeRestored;
  j["restoring"] = g_restoring;
  if (g_resumeActive) j["resume"] = fs::ResumeToJson(g_resume);
  return j;
}

void RestoreJson(const Json& j) {
  if (const Json* f = j.Find("fence")) g_fence.FromJson(*f);
  if (!Bool(j, "active")) return;
  g_asg.active = true;
  g_asg.match_id = Str(j, "match_id");
  g_asg.epoch = Int(j, "epoch", 1);
  g_asg.config_rev = Int(j, "config_rev", 1);
  g_config = j.Find("config") ? *j.Find("config") : Json::Object();
  const Json* st = j.Find("state");
  g_stream.Reset(st ? *st : Json(), Int(j, "live_rev", 0));
  g_loading = Bool(j, "loading");
  g_loadStarted = 0;
  g_seriesOver = Bool(j, "series_over");
  g_serverReset = Bool(j, "server_reset");
  g_latestBackupRound = static_cast<int>(Int(j, "latest_backup_round", -1));
  g_lastLive = j.Find("last_live") ? *j.Find("last_live") : Json();
  if (const Json* s = j.Find("sent_backups")) {
    std::lock_guard<std::mutex> lk(g_backupMu);
    for (const auto& v : s->Items()) g_sentBackups.insert(v.AsString());
  }
  g_resumeActive = Bool(j, "resume_active");
  g_resumeRestored = Bool(j, "resume_restored");
  g_restoring = Bool(j, "restoring");
  if (const Json* r = j.Find("resume")) g_resume = fs::ResumeFromJson(*r);
  g_fleetInstance = 0;  // this image registers its own handlers
  signals::SetEnabled(true);
  ScrimSetAutoEnabled(false);
  Print("fleet: kept assignment %s epoch %lld (live_rev %lld) across the reload\n", g_asg.match_id.c_str(),
        g_asg.epoch, g_stream.Rev());
}

std::string FleetServerId() { return ServerId(); }

}  // namespace readyup::fleet_bridge

// The match part of the local status endpoint (see match_status.h). Moved from the core's
// status_feed.cpp Collect(); the core keeps the HTTP server, versions, platform, selftest and
// health, and merges what this returns.
#include "readyup/match_status.h"

#include "readyup/host.h"
#include "readyup/map_names.h"
#include "readyup/demo_recorder.h"
#include "readyup/esports.h"
#include "readyup/fleet_bridge.h"
#include "readyup/match_end.h"
#include "readyup/match_rules.h"
#include "readyup/match_state.h"
#include "readyup/match_stats.h"
#include "readyup/modes.h"
#include "readyup/pause_state.h"
#include "readyup/player_registry.h"
#include "readyup/players.h"
#include "readyup/scrim_flow.h"
#include "readyup/webhook.h"

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <map>
#include <mutex>
#include <string>
#include <unordered_map>

namespace readyup {
namespace {

using status::Json;

long long UnixMs() {
  return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch())
      .count();
}

// Demo state per map (recording events: game thread; upload events: upload thread).
struct DemoInfo {
  std::string file;
  std::string state;  // recording | stopped | uploading | stored | failed
};
std::mutex g_demoMu;
long long g_demoMatchId = 0;
std::map<int, DemoInfo> g_demos;
int g_uploadsPending = 0;

// Series / map results (match flow listener, game thread).
struct MapResult {
  int team1 = 0, team2 = 0;
  std::string winner;
};
unsigned long long g_seriesMatchId = 0;
int g_seriesT1 = 0, g_seriesT2 = 0;
bool g_seriesOver = false;
std::map<int, MapResult> g_mapResults;

// Game thread.
bool g_wasPaused = false;
long long g_pauseStartedMs = 0;
std::string g_summary, g_state, g_mode;  // get_status buffers

struct PhaseInfo {
  const char* mode = "idle";   // idle | scrim | match | practice
  const char* phase = "idle";  // FLEET.md §9.1 phases + idle / practice / warmup (scrim)
};

PhaseInfo ComputePhase(ReadyUpMode m, bool haveCtx, bool scrim, bool paused, bool seriesOver) {
  PhaseInfo p;
  switch (m) {
    case ReadyUpMode::Idle: p.mode = "idle"; p.phase = "idle"; break;
    case ReadyUpMode::Practice: p.mode = "practice"; p.phase = "practice"; break;
    case ReadyUpMode::ScrimWarmup: p.mode = "scrim"; p.phase = "warmup"; break;
    // Another plugin runs the server; the summary's phase is that plugin mode's name.
    case ReadyUpMode::External: p.mode = "external"; p.phase = "external"; break;
    case ReadyUpMode::MatchWarmup: p.phase = "warmup"; break;
    case ReadyUpMode::MatchKnife: p.phase = GetKnifePhase() == KnifePhase::Picking ? "side_pick" : "knife"; break;
    case ReadyUpMode::MatchLive: p.phase = paused ? "paused" : "live"; break;
    case ReadyUpMode::Postgame: p.phase = seriesOver ? "series_end" : "map_end"; break;
  }
  if (m != ReadyUpMode::Idle && m != ReadyUpMode::Practice && m != ReadyUpMode::ScrimWarmup &&
      m != ReadyUpMode::External) {
    p.mode = (!haveCtx || scrim) ? "scrim" : "match";
  }
  // Knife side pick can outlive the match_knife mode briefly.
  if (m == ReadyUpMode::MatchWarmup && GetKnifePhase() == KnifePhase::Picking) p.phase = "side_pick";
  return p;
}

Json SideOf(bool known, bool ct) {
  if (!known) return Json();
  return Json(ct ? "ct" : "t");
}

void OnFlowEvent(const MatchFlowEvent& e) {
  switch (e.type) {
    case MatchFlowEventType::MapResult:
      g_seriesMatchId = e.matchid;
      g_seriesT1 = e.team1SeriesScore;
      g_seriesT2 = e.team2SeriesScore;
      g_seriesOver = e.seriesOver;
      g_mapResults[e.mapNumber] = MapResult{e.team1Score, e.team2Score, e.winner};
      break;
    case MatchFlowEventType::SeriesEnd: g_seriesOver = true; break;
    case MatchFlowEventType::ServerReset:
      g_seriesT1 = g_seriesT2 = 0;
      g_seriesOver = false;
      g_mapResults.clear();
      break;
  }
}

void OnDemoEvent(const demo::DemoEvent& e) {
  std::lock_guard<std::mutex> lk(g_demoMu);
  if (e.matchid != g_demoMatchId) {
    g_demoMatchId = e.matchid;
    g_demos.clear();
  }
  DemoInfo& d = g_demos[e.mapNumber];
  if (!e.fileName.empty()) d.file = e.fileName;
  switch (e.type) {
    case demo::DemoEventType::RecordingStarted: d.state = "recording"; break;
    case demo::DemoEventType::RecordingStopped: d.state = "stopped"; break;
    case demo::DemoEventType::UploadStarted:
      d.state = "uploading";
      ++g_uploadsPending;
      break;
    case demo::DemoEventType::UploadSucceeded:
      d.state = "stored";
      g_uploadsPending = std::max(0, g_uploadsPending - 1);
      break;
    case demo::DemoEventType::UploadFailed:
      d.state = "failed";
      g_uploadsPending = std::max(0, g_uploadsPending - 1);
      break;
  }
}

}  // namespace

void MatchStatusInstall() {
  AddMatchFlowListener(&OnFlowEvent);
  demo::AddListener(&OnDemoEvent);
}

namespace {

// ---- engine reads for the status (game thread) -----------------------------------------------

int g_offPing = -2;      // CCSPlayerController.m_iPing
int g_offRulesPtr = -2;  // CCSGameRulesProxy.m_pGameRules
int g_offWarmup = -2;    // CCSGameRules.m_bWarmupPeriod
int g_offFreeze = -2;    // CCSGameRules.m_bFreezePeriod
int g_offWaitResume = -2;  // CCSGameRules.m_bMatchWaitingForResume (mp_pause_match, auto 5v5)
int g_offGamePaused = -2;  // CCSGameRules.m_bGamePaused
uint32_t g_rulesProxy = 0xFFFFFFFFu;
long long g_rulesScanAtMs = 0;
// When each roster player was first seen connected (unix ms); dropped when they leave.
std::unordered_map<uint64_t, long long> g_connectedAtMs;

// The player's ping in ms from their controller, or -1 when unknown.
int PingOfSlot(int slot) {
  const ru_api* a = host::Api();
  if (!a || slot < 0 || slot >= 64 || a->entity_system_status(a->self) != RU_ENTSYS_OK) return -1;
  if (g_offPing == -2) g_offPing = a->schema_offset(a->self, "CCSPlayerController", "m_iPing");
  if (g_offPing < 0) return -1;
  void* ent = a->entity_by_index(a->self, slot + 1);
  if (!ent) return -1;
  const char* cls = a->entity_classname(a->self, ent);
  if (!cls || std::strcmp(cls, "cs_player_controller") != 0) return -1;
  uint32_t v = 0;
  std::memcpy(&v, static_cast<unsigned char*>(ent) + g_offPing, sizeof(v));
  return v > 5000 ? -1 : static_cast<int>(v);
}

// CS2's own game rules flags (not Ready Up's phase). The game rules proxy is found once per map
// (at most one scan every 10 s while missing) and kept by handle.
struct GameFlags {
  int warmup = -1;  // 1 in CS2's warmup, 0 not, -1 unknown
  int freeze = -1;  // 1 in freeze time
  int paused = -1;  // 1 while the match is paused or waits to pause (m_bMatchWaitingForResume / m_bGamePaused)
};

GameFlags ReadGameFlags(long long nowMs) {
  GameFlags out;
  const ru_api* a = host::Api();
  if (!a || a->entity_system_status(a->self) != RU_ENTSYS_OK) return out;
  if (g_offRulesPtr == -2) {
    g_offRulesPtr = a->schema_offset(a->self, "CCSGameRulesProxy", "m_pGameRules");
    g_offWarmup = a->schema_offset(a->self, "CCSGameRules", "m_bWarmupPeriod");
    g_offFreeze = a->schema_offset(a->self, "CCSGameRules", "m_bFreezePeriod");
    g_offWaitResume = a->schema_offset(a->self, "CCSGameRules", "m_bMatchWaitingForResume");
    g_offGamePaused = a->schema_offset(a->self, "CCSGameRules", "m_bGamePaused");
  }
  if (g_offRulesPtr < 0) return out;
  void* proxy = g_rulesProxy != 0xFFFFFFFFu ? a->entity_from_handle(a->self, g_rulesProxy) : nullptr;
  if (!proxy) {
    g_rulesProxy = 0xFFFFFFFFu;
    if (nowMs - g_rulesScanAtMs < 10000) return out;
    g_rulesScanAtMs = nowMs;
    for (int i = 65; i < 4096 && !proxy; ++i) {
      void* e = a->entity_by_index(a->self, i);
      if (!e) continue;
      const char* cn = a->entity_classname(a->self, e);
      if (cn && std::strcmp(cn, "cs_gamerules") == 0) {
        proxy = e;
        g_rulesProxy = a->entity_handle_of(a->self, e);
      }
    }
    if (!proxy) return out;
  }
  void* rules = nullptr;
  std::memcpy(&rules, static_cast<unsigned char*>(proxy) + g_offRulesPtr, sizeof(rules));
  if (!rules) return out;
  auto flag = [&](int off) {
    if (off < 0) return -1;
    bool v = false;
    std::memcpy(&v, static_cast<unsigned char*>(rules) + off, sizeof(v));
    return v ? 1 : 0;
  };
  out.warmup = flag(g_offWarmup);
  out.freeze = flag(g_offFreeze);
  const int wait = flag(g_offWaitResume), gp = flag(g_offGamePaused);
  out.paused = wait == 1 || gp == 1 ? 1 : (wait == 0 || gp == 0) ? 0 : -1;
  return out;
}

// The summary fields, the MatchState of the loaded match / scrim (null when none) and
// update_safe. Game thread.
void Collect(Json* sOut, Json* stOut, bool* safeOut) {
  const ReadyUpMode mode = GetMode();
  const auto ctx = WebhookGetMatchContext();
  const bool scrim = ctx && ctx->slug == "scrim";
  const auto ms = MatchStateGet();
  const auto pause = PauseStateGet();
  const bool paused = ctx && pause.paused;
  const long long nowMs = UnixMs();
  if (paused && !g_wasPaused) g_pauseStartedMs = nowMs - 1000LL * PauseStatePauseDurationSeconds();
  g_wasPaused = paused;

  // A new / different match resets the series bookkeeping.
  if (!ctx || ctx->matchid != g_seriesMatchId) {
    if (!ctx || g_seriesMatchId != 0) {
      g_seriesT1 = g_seriesT2 = 0;
      g_seriesOver = false;
      g_mapResults.clear();
    }
    g_seriesMatchId = ctx ? ctx->matchid : 0;
  }
  const PhaseInfo ph = ComputePhase(mode, ctx.has_value(), scrim, paused, g_seriesOver);

  const auto humans = ListHumans();
  std::unordered_map<uint64_t, const HumanIdentity*> humanBy;
  for (const auto& h : humans) humanBy[h.steamid64] = &h;

  int ready = 0, readyTotal = 0;
  if (ctx) {
    // Per team, ready players up to a full team over the full team: substitutes never count toward
    // the players the match waits for (match_rules.h TeamReadyTally).
    int teamRoster[2] = {0, 0}, teamReady[2] = {0, 0}, teamPresent[2] = {0, 0}, teamPresentReady[2] = {0, 0};
    for (const auto& kv : ctx->roster_team) {
      if (!kv.first) continue;
      const int t = kv.second == WebhookTeam::Team2 ? 1 : 0;
      ++teamRoster[t];
      const bool ready = IsReady(kv.first);
      if (ready) ++teamReady[t];
      if (humanBy.count(kv.first)) {
        ++teamPresent[t];
        if (ready) ++teamPresentReady[t];
      }
    }
    for (int t = 0; t < 2; ++t) {
      // Bot fill: the connected humans only (match_rules.h BotFillReadyTally).
      const ReadyTally tally = ctx->bot_fill
                                   ? BotFillReadyTally(teamPresent[t], teamPresentReady[t])
                                   : TeamReadyTally(teamRoster[t], teamReady[t], ctx->players_per_team);
      ready += tally.ready;
      readyTotal += tally.total;
    }
  } else {
    for (const auto& h : humans) {
      if (h.team != 2 && h.team != 3) continue;
      ++readyTotal;
      if (IsReady(h.steamid64)) ++ready;
    }
  }

  int uploadsPending = 0;
  std::map<int, DemoInfo> demos;
  {
    std::lock_guard<std::mutex> lk(g_demoMu);
    uploadsPending = g_uploadsPending;
    if (ctx && static_cast<unsigned long long>(g_demoMatchId) == ctx->matchid) demos = g_demos;
  }

  // update_safe (docs/FLEET.md §17.2): false from loading to series_end of a real match and while
  // a demo upload is pending; scrims, practice and idle are safe.
  const bool matchActive = ctx && !scrim && mode != ReadyUpMode::Idle && mode != ReadyUpMode::Practice &&
                           mode != ReadyUpMode::ScrimWarmup && mode != ReadyUpMode::External;
  bool safe = !matchActive || (mode == ReadyUpMode::Postgame && g_seriesOver && !MatchEndPending());
  if (uploadsPending > 0) safe = false;

  // ---- summary
  Json s = Json::Object();
  s["mode"] = ph.mode;
  s["ru_mode"] = GetModeString();
  s["phase"] = ph.phase;
  if (mode == ReadyUpMode::External) {
    // readyup.match.v1 set_external_mode: the plugin mode's name ("deathmatch").
    const std::string ext = ExternalModeName();
    if (!ext.empty()) s["phase"] = ext;
  }
  s["map"] = ms.current_map;
  s["map_number"] = ms.map_number;
  s["num_maps"] = ctx ? ctx->num_maps : 0;
  s["round"] = ms.round_number;
  Json score = Json::Object();
  score["team1"] = ms.team1_score;
  score["team2"] = ms.team2_score;
  s["score"] = std::move(score);
  Json series = Json::Object();
  series["team1"] = g_seriesT1;
  series["team2"] = g_seriesT2;
  s["series_score"] = std::move(series);
  Json players = Json::Object();
  players["connected"] = static_cast<int>(humans.size());
  players["expected"] = ctx ? static_cast<int>(ctx->roster_team.size()) : 0;
  s["players"] = std::move(players);
  // CS2's own warmup flag, in every mode (idle too): csm's status table and debugging.
  const GameFlags game = ReadGameFlags(nowMs);
  if (game.warmup >= 0) s["game_warmup"] = game.warmup == 1;
  if (game.freeze >= 0) s["game_freeze"] = game.freeze == 1;
  Json rdy = Json::Object();
  rdy["ready"] = ready;
  rdy["total"] = readyTotal;
  s["ready"] = std::move(rdy);
  if (ctx) {
    s["match_id"] = std::to_string(ctx->matchid);
    s["slug"] = ctx->slug;
    Json teams = Json::Object();
    teams["team1"] = ctx->team1_name;
    teams["team2"] = ctx->team2_name;
    s["teams"] = std::move(teams);
    s["paused"] = paused;
    s["ruleset"] = RulesetName(CurrentEffectiveRules().ruleset);
  }
  if (const char* kp = KnifePhaseString()) s["knife"] = kp;
  const int countdown = ScrimCountdownSecondsLeft();
  if (countdown >= 0) s["countdown_s"] = countdown;
  if (uploadsPending > 0) s["demo_uploads_pending"] = uploadsPending;

  // ---- MatchState (docs/FLEET.md §9.1) while a match / scrim context is loaded.
  Json st;
  if (ctx) {
    st = Json::Object();
    st["match_id"] = std::to_string(ctx->matchid);
    st["slug"] = ctx->slug;
    st["scrim"] = scrim;
    st["phase"] = ph.phase;

    // Sides of team1 on the current map.
    bool sideKnown = false, team1Ct = true;
    stats::TeamLine t1line, t2line;
    {
      std::lock_guard<std::recursive_mutex> lk(stats::Mutex());
      auto& acc = stats::Current();
      t1line = acc.Team1Line();
      t2line = acc.Team2Line();
      if (acc.Live()) {
        sideKnown = true;
        team1Ct = acc.Team1IsCt();
      }
    }
    const int mapIdx = ms.map_number - 1;
    if (!sideKnown && mapIdx >= 0 && static_cast<size_t>(mapIdx) < ctx->map_sides.size()) {
      const std::string& ms1 = ctx->map_sides[static_cast<size_t>(mapIdx)];
      if (ms1 == "team1_ct" || ms1 == "team2_ct") {
        sideKnown = true;
        team1Ct = ms1 == "team1_ct";
      }
    }

    Json ser = Json::Object();
    ser["num_maps"] = ctx->num_maps;
    ser["current_map"] = ms.map_number;
    Json sscore = Json::Object();
    sscore["team1"] = g_seriesT1;
    sscore["team2"] = g_seriesT2;
    ser["score"] = std::move(sscore);
    Json maps = Json::Object();
    const bool curDone = mode == ReadyUpMode::Postgame;
    for (size_t i = 0; i < ctx->maplist.size(); ++i) {
      const int n = static_cast<int>(i) + 1;
      Json m = Json::Object();
      m["name"] = mapnames::DisplayName(ctx->maplist[i]);
      mapnames::MapRef ref;
      if (mapnames::ParseEntry(ctx->maplist[i], &ref) && !ref.workshop_id.empty()) m["workshop_id"] = ref.workshop_id;
      m["sides"] = i < ctx->map_sides.size() ? ctx->map_sides[i] : std::string("knife");
      const auto res = g_mapResults.find(n);
      const char* status = "pending";
      if (res != g_mapResults.end() || n < ms.map_number || (n == ms.map_number && curDone)) status = "done";
      else if (n == ms.map_number && mode != ReadyUpMode::MatchWarmup) status = "live";
      m["status"] = status;
      Json msc = Json::Object();
      if (res != g_mapResults.end()) {
        msc["team1"] = res->second.team1;
        msc["team2"] = res->second.team2;
        m["score"] = std::move(msc);
        m["winner"] = res->second.winner;
      } else if (n == ms.map_number) {
        msc["team1"] = ms.team1_score;
        msc["team2"] = ms.team2_score;
        m["score"] = std::move(msc);
      }
      const auto d = demos.find(n);
      if (d != demos.end()) {
        Json dj = Json::Object();
        dj["file"] = d->second.file;
        dj["state"] = d->second.state;
        m["demo"] = std::move(dj);
      }
      maps[std::to_string(n)] = std::move(m);
    }
    ser["maps"] = std::move(maps);
    st["series"] = std::move(ser);

    // Names for disconnected roster players.
    std::unordered_map<uint64_t, std::string> seen;
    for (const auto& p : ListObservedPlayers()) seen[p.steamid64] = p.name;

    auto team = [&](WebhookTeam which, const std::string& name, const stats::TeamLine& line, int mapScore,
                    bool isTeam1) {
      Json t = Json::Object();
      t["name"] = name;
      t["side"] = SideOf(sideKnown, isTeam1 ? team1Ct : !team1Ct);
      t["score"] = mapScore;
      t["score_ct"] = line.score_ct;
      t["score_t"] = line.score_t;
      Json pl = Json::Object();
      for (const auto& kv : ctx->roster_team) {
        if (kv.second != which || !kv.first) continue;
        Json pj = Json::Object();
        const auto h = humanBy.find(kv.first);
        std::string nm = h != humanBy.end() ? h->second->name : std::string();
        if (nm.empty()) {
          const auto o = seen.find(kv.first);
          if (o != seen.end()) nm = o->second;
        }
        pj["name"] = nm;
        pj["role"] = ctx->substitutes.count(kv.first) ? "sub" : "player";
        pj["connected"] = h != humanBy.end();
        pj["ready"] = IsReady(kv.first);
        if (h != humanBy.end()) {
          const int ping = PingOfSlot(h->second->slot);
          if (ping >= 0) pj["ping"] = ping;
          auto at = g_connectedAtMs.find(kv.first);
          if (at == g_connectedAtMs.end()) at = g_connectedAtMs.emplace(kv.first, nowMs).first;
          pj["connected_at"] = at->second;
        } else {
          g_connectedAtMs.erase(kv.first);
        }
        pl[std::to_string(kv.first)] = std::move(pj);
      }
      t["players"] = std::move(pl);
      return t;
    };
    Json teams = Json::Object();
    teams["team1"] = team(WebhookTeam::Team1, ctx->team1_name, t1line, ms.team1_score, true);
    teams["team2"] = team(WebhookTeam::Team2, ctx->team2_name, t2line, ms.team2_score, false);
    st["teams"] = std::move(teams);

    Json specs = Json::Object();
    for (uint64_t sid : ctx->spectators) {
      Json sj = Json::Object();
      const auto h = humanBy.find(sid);
      sj["name"] = h != humanBy.end() ? h->second->name : (seen.count(sid) ? seen[sid] : std::string());
      sj["connected"] = h != humanBy.end();
      specs[std::to_string(sid)] = std::move(sj);
    }
    st["spectators"] = std::move(specs);

    int c1 = 0, c2 = 0;
    for (const auto& kv : ctx->roster_team) {
      if (kv.second == WebhookTeam::Team1) ++c1;
      else if (kv.second == WebhookTeam::Team2) ++c2;
    }
    Json rj = Json::Object();
    rj["required_per_team"] = std::max(FullTeamSize(c1, ctx->players_per_team), FullTeamSize(c2, ctx->players_per_team));
    rj["ready"] = ready;
    rj["total"] = readyTotal;
    st["ready"] = std::move(rj);

    Json kj = Json::Object();
    const KnifePhase kp = GetKnifePhase();
    kj["status"] = kp == KnifePhase::Picking                                        ? "picking"
                   : (kp == KnifePhase::Starting || kp == KnifePhase::Running) ? "running"
                                                                                   : "none";
    if (kp == KnifePhase::Picking) kj["winner"] = KnifeWinnerTeamString();
    st["knife"] = std::move(kj);

    Json pj = Json::Object();
    pj["active"] = paused;
    if (paused) pj["started_at"] = g_pauseStartedMs;
    Json un = Json::Object();
    un["team1"] = paused && pause.team1_ready_to_unpause;
    un["team2"] = paused && pause.team2_ready_to_unpause;
    pj["unpause"] = std::move(un);
    st["pause"] = std::move(pj);

    Json round = Json::Object();
    round["number"] = ms.round_number;
    st["round"] = std::move(round);

    // CS2's own view, next to Ready Up's phase: a map stuck in CS2's warmup shows up as a
    // mismatch on the platform (FLEET.md §6, `game`).
    if (game.warmup >= 0 || game.freeze >= 0) {
      Json gj = Json::Object();
      if (game.warmup >= 0) gj["warmup"] = game.warmup == 1;
      if (game.freeze >= 0) gj["freeze_period"] = game.freeze == 1;
      st["game"] = std::move(gj);
    }

    // Ruleset preset + overrides, and the keys that differ from the preset (ruleset.h).
    const EffectiveRuleSet er = CurrentEffectiveRules();
    st["ruleset"] = RulesetName(er.ruleset);
    st["effective_rules"] = EffectiveRulesJson(er);
  }

  *sOut = std::move(s);
  *stOut = ctx ? std::move(st) : Json();
  *safeOut = safe;
}

}  // namespace

status::Json MatchStatusStateJson() {
  Json s, st;
  bool safe = true;
  Collect(&s, &st, &safe);
  return st;
}

int MatchStatusGet(ru_match_status* out) {
  if (!out || out->struct_size < sizeof(uint32_t) + sizeof(int32_t)) return 0;
  Json s, st;
  bool safe = true;
  Collect(&s, &st, &safe);
  // A platform-assigned match (docs/FLEET.md §9): /status shows the same MatchState the fleet
  // link streams (platform match_id, epoch, config_rev, live_rev, rules, ...).
  Json fleetState;
  if (fleet_bridge::CurrentState(&fleetState)) {
    st = std::move(fleetState);
    if (!fleet_bridge::UpdateSafe()) safe = false;
  }
  g_summary = s.Dump();
  g_state = st.IsNull() ? std::string() : st.Dump();
  g_mode = GetModeString();

  // Fill only what the caller's struct has room for.
  ru_match_status r{};
  r.struct_size = out->struct_size;
  r.update_safe = safe ? 1 : 0;
  r.summary_json = g_summary.c_str();
  r.state_json = st.IsNull() ? nullptr : g_state.c_str();
  r.ru_mode = g_mode.c_str();
  const size_t n = out->struct_size < sizeof(r) ? out->struct_size : sizeof(r);
  std::memcpy(out, &r, n);
  return 1;
}

void MatchStatusSeedSeries(unsigned long long matchid, int team1, int team2, const std::vector<SeededMapResult>& maps) {
  g_seriesMatchId = matchid;
  g_seriesT1 = std::max(0, team1);
  g_seriesT2 = std::max(0, team2);
  g_seriesOver = false;
  g_mapResults.clear();
  for (const auto& m : maps) {
    if (m.map_number >= 1) g_mapResults[m.map_number] = MapResult{m.team1, m.team2, m.winner};
  }
}

Json MatchStatusSnapshotJson() {
  Json j = Json::Object();
  j["series_matchid"] = static_cast<long long>(g_seriesMatchId);
  j["series_t1"] = g_seriesT1;
  j["series_t2"] = g_seriesT2;
  j["series_over"] = g_seriesOver;
  Json maps = Json::Object();
  for (const auto& kv : g_mapResults) {
    Json m = Json::Object();
    m["team1"] = kv.second.team1;
    m["team2"] = kv.second.team2;
    m["winner"] = kv.second.winner;
    maps[std::to_string(kv.first)] = std::move(m);
  }
  j["map_results"] = std::move(maps);
  std::lock_guard<std::mutex> lk(g_demoMu);
  j["demo_matchid"] = g_demoMatchId;
  j["uploads_pending"] = g_uploadsPending;
  Json demos = Json::Object();
  for (const auto& kv : g_demos) {
    Json d = Json::Object();
    d["file"] = kv.second.file;
    d["state"] = kv.second.state;
    demos[std::to_string(kv.first)] = std::move(d);
  }
  j["demos"] = std::move(demos);
  return j;
}

void MatchStatusRestoreJson(const Json& j) {
  auto i = [&](const Json* o, const char* k) {
    const Json* v = o ? o->Find(k) : nullptr;
    return v ? v->AsInt() : 0;
  };
  g_seriesMatchId = static_cast<unsigned long long>(i(&j, "series_matchid"));
  g_seriesT1 = static_cast<int>(i(&j, "series_t1"));
  g_seriesT2 = static_cast<int>(i(&j, "series_t2"));
  if (const Json* v = j.Find("series_over")) g_seriesOver = v->AsBool();
  g_mapResults.clear();
  if (const Json* maps = j.Find("map_results")) {
    for (const auto& kv : maps->Members()) {
      MapResult r;
      r.team1 = static_cast<int>(i(&kv.second, "team1"));
      r.team2 = static_cast<int>(i(&kv.second, "team2"));
      if (const Json* w = kv.second.Find("winner")) r.winner = w->AsString();
      g_mapResults[std::atoi(kv.first.c_str())] = r;
    }
  }
  std::lock_guard<std::mutex> lk(g_demoMu);
  g_demoMatchId = i(&j, "demo_matchid");
  g_uploadsPending = static_cast<int>(i(&j, "uploads_pending"));
  g_demos.clear();
  if (const Json* demos = j.Find("demos")) {
    for (const auto& kv : demos->Members()) {
      DemoInfo d;
      if (const Json* f = kv.second.Find("file")) d.file = f->AsString();
      if (const Json* s = kv.second.Find("state")) d.state = s->AsString();
      g_demos[std::atoi(kv.first.c_str())] = d;
    }
  }
}

int EngineMatchPaused() {
  if (!host::OnGameThread()) return -1;
  return ReadGameFlags(UnixMs()).paused;
}

}  // namespace readyup

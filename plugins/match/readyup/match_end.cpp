#include "readyup/match_end.h"

#include "readyup/card_html.h"
#include "readyup/result_card.h"

#include "readyup/at_payloads.h"

#include "readyup/engine.h"
#include "readyup/demo_recorder.h"
#include "readyup/game_timers.h"
#include "readyup/host.h"
#include "readyup/logging.h"
#include "readyup/match_console.h"
#include "readyup/match_state.h"
#include "readyup/modes.h"
#include "readyup/players.h"
#include "readyup/webhook.h"

#include <algorithm>
#include <atomic>
#include <cstdlib>
#include <mutex>

namespace readyup {
namespace {

constexpr double kResetAfterKickSeconds = 2.0;

std::mutex g_mu;
int g_kickNoDemo = 90;
int g_kickDemoNoUpload = 90;
int g_kickDemoUpload = 90;
std::vector<MatchFlowListener> g_listeners;
std::atomic<unsigned> g_generation{0};
std::atomic<int> g_pending{0};

// The step scheduled after a map ended (so a plugin reload in postgame can schedule it again).
struct Plan {
  enum Kind { None = 0, NextMap, Reset, KickThenReset };
  Kind kind = None;
  double due = 0;  // host::NowSeconds()
  std::string next;        // NextMap
  std::string kickReason;  // KickThenReset
  double resetAfterKick = kResetAfterKickSeconds;  // KickThenReset: seconds from the kick to the unload
  MatchFlowEvent base;     // Reset / KickThenReset: the ServerReset event
};
Plan g_plan;  // guarded by g_mu

void SetPlan(Plan p) {
  std::lock_guard<std::mutex> lk(g_mu);
  g_plan = std::move(p);
}

void Emit(const MatchFlowEvent& e) {
  std::vector<MatchFlowListener> ls;
  {
    std::lock_guard<std::mutex> lk(g_mu);
    ls = g_listeners;
  }
  // One line per event (the full stats are in `ru_match_stats` / the listener).
  Print("match-flow: %s matchid=%llu map=%d %s %d-%d series %d-%d%s\n", MatchFlowEventTypeName(e.type), e.matchid,
        e.mapNumber, e.winner.c_str(), e.team1Score, e.team2Score, e.team1SeriesScore, e.team2SeriesScore,
        e.type == MatchFlowEventType::SeriesEnd ? (" reset_in=" + std::to_string(e.secondsUntilReset) + "s").c_str()
                                                : "");
  for (auto& fn : ls) fn(e);
}

// Every human slot, admins and slots without a SteamID included (never bots or GOTV).
void KickAllHumans(const std::string& reason) {
  std::string r = reason;
  std::replace(r.begin(), r.end(), '"', '\'');
  int kicked = 0;
  for (const auto& h : ListHumans()) {
    if (h.userid < 0) continue;
    const std::string cmd = "kickid " + std::to_string(h.userid) + " \"" + r + "\"";
    if (EnqueueServerCommand(cmd.c_str())) ++kicked;
  }
  Print("series-end: kicked %d player(s)\n", kicked);
}

std::vector<std::string> SplitArgs(const std::string& line) {
  std::vector<std::string> out;
  std::string cur;
  bool inQ = false;
  bool any = false;
  for (char c : line) {
    if (c == '"') {
      inQ = !inQ;
      any = true;
      continue;
    }
    if (!inQ && (c == ' ' || c == '\t' || c == '\r' || c == '\n')) {
      if (any || !cur.empty()) out.push_back(cur);
      cur.clear();
      any = false;
      continue;
    }
    cur.push_back(c);
  }
  if (any || !cur.empty()) out.push_back(cur);
  return out;
}

void ScheduleNextMap(double delay, std::string next, unsigned gen) {
  g_pending.fetch_add(1);
  SetPlan(Plan{Plan::NextMap, host::NowSeconds() + delay, next, {}, kResetAfterKickSeconds, {}});
  ScheduleOnGameThread(delay, [next, gen]() {
    g_pending.fetch_sub(1);
    if (g_generation.load() != gen) return;
    SetPlan(Plan{});
    ModesBeginNextMapWarmup();
    if (!next.empty()) (void)LoadMapEntry(next);  // changelevel, or host_workshop_map for a workshop map
  });
}

void ScheduleReset(double delay, MatchFlowEvent base, unsigned gen) {
  g_pending.fetch_add(1);
  SetPlan(Plan{Plan::Reset, host::NowSeconds() + delay, {}, {}, kResetAfterKickSeconds, base});
  ScheduleOnGameThread(delay, [base, gen]() {
    g_pending.fetch_sub(1);
    if (g_generation.load() != gen) return;
    SetPlan(Plan{});
    MatchFlowEvent ev = base;
    ev.type = MatchFlowEventType::ServerReset;
    Emit(ev);
    ModesFinishSeriesResetToIdle();
  });
}

// Kicks everyone after `delay`, then unloads the match `resetAfterKick` later.
void ScheduleKickThenReset(double delay, double resetAfterKick, std::string kickReason, MatchFlowEvent base,
                           unsigned gen) {
  g_pending.fetch_add(1);
  SetPlan(Plan{Plan::KickThenReset, host::NowSeconds() + delay, {}, kickReason, resetAfterKick, base});
  ScheduleOnGameThread(delay, [kickReason, resetAfterKick, base, gen]() {
    g_pending.fetch_sub(1);
    if (g_generation.load() != gen) return;
    KickAllHumans(kickReason);
    ScheduleReset(resetAfterKick, base, gen);
  });
}

}  // namespace

void AddMatchFlowListener(MatchFlowListener fn) {
  if (!fn) return;
  std::lock_guard<std::mutex> lk(g_mu);
  g_listeners.push_back(std::move(fn));
}

void SetSeriesEndKickDelays(int noDemo, int demoNoUpload, int demoUpload) {
  std::lock_guard<std::mutex> lk(g_mu);
  if (noDemo >= 0) g_kickNoDemo = noDemo;
  if (demoNoUpload >= 0) g_kickDemoNoUpload = demoNoUpload;
  if (demoUpload >= 0) g_kickDemoUpload = demoUpload;
}

void GetSeriesEndKickDelays(int* noDemo, int* demoNoUpload, int* demoUpload) {
  std::lock_guard<std::mutex> lk(g_mu);
  if (noDemo) *noDemo = g_kickNoDemo;
  if (demoNoUpload) *demoNoUpload = g_kickDemoNoUpload;
  if (demoUpload) *demoUpload = g_kickDemoUpload;
}

MapEndPlan MatchEndOnMapComplete(const MapEndInput& in) {
  int kNo = 0, kNoUp = 0, kUp = 0;
  GetSeriesEndKickDelays(&kNo, &kNoUp, &kUp);
  const bool recording = demo::IsRecording();
  const auto uploadCtx = WebhookGetMatchContext();
  // Uploaded over HTTP, or streamed to the platform over the fleet link (FLEET.md §12.2).
  const bool hasUpload = demo::CurrentRecordingStreamed() ||
                         demo::UploadWanted(demo::Get(), uploadCtx ? uploadCtx->demo_upload : -1);
  const int tvDelay = demo::TvDelaySeconds();
  const MapEndPlan plan = ComputeMapEndPlan(recording, hasUpload, tvDelay, kNo, kNoUp, kUp);
  const unsigned gen = g_generation.load();

  MatchFlowEvent base;
  if (auto ctx = WebhookGetMatchContext()) {
    base.matchid = ctx->matchid;
    base.slug = ctx->slug;
    base.scrim = ctx->slug == "scrim";
    base.team1Name = ctx->team1_name;
    base.team2Name = ctx->team2_name;
  }
  base.team1SeriesScore = in.team1SeriesScore;
  base.team2SeriesScore = in.team2SeriesScore;
  base.demoRecorded = recording;
  base.demoUploadConfigured = hasUpload;

  MatchFlowEvent mapEv = base;
  mapEv.type = MatchFlowEventType::MapResult;
  mapEv.mapNumber = in.mapNumber;
  mapEv.mapName = in.mapName;
  mapEv.winner = in.winner;
  mapEv.team1Score = in.team1Score;
  mapEv.team2Score = in.team2Score;
  mapEv.seriesOver = in.seriesOver;
  mapEv.nextMap = in.nextMap;
  mapEv.decidedByDamage = in.decidedByDamage;
  mapEv.team1Damage = in.team1Damage;
  mapEv.team2Damage = in.team2Damage;
  {
    std::lock_guard<std::recursive_mutex> lk(stats::Mutex());
    stats::Current().EndMap();
    mapEv.stats = stats::Current().Snapshot();
  }
  Emit(mapEv);

  // Keep the engine from restarting / changing level before this flow is done with the map.
  const std::string restartCmd = "mp_match_restart_delay " + std::to_string(plan.restartDelay + 1);
  (void)EnqueueServerCommand(restartCmd.c_str());
  if (recording) {
    (void)demo::StopAfterDelayAndUpload(std::max(0.0, plan.tvFlushDelay - 0.5), in.team1Score + in.team2Score,
                                        in.team1Score, in.team2Score);
  }
  Print("map-end: map %d done; demo=%d upload=%d tv_delay=%d restart_delay=%ds flush=%ds series_over=%d\n",
        in.mapNumber, recording ? 1 : 0, hasUpload ? 1 : 0, tvDelay, plan.restartDelay, plan.tvFlushDelay,
        in.seriesOver ? 1 : 0);

  ResultCardInfo card;
  card.team1 = base.team1Name;
  card.team2 = base.team2Name;
  card.decidedByDamage = in.decidedByDamage;
  card.team1Damage = in.team1Damage;
  card.team2Damage = in.team2Damage;
  if (!in.seriesOver) {
    card.winner = in.winner == "team1" || in.winner == "team2" ? in.winner : "";
    card.team1Score = in.team1Score;
    card.team2Score = in.team2Score;
    card.mapNumber = in.mapNumber;
    card.mapName = in.mapName;
    card.nextMap = in.nextMap;
    ResultCardShow(MapResultCardHtml(card), std::max(1, plan.restartDelay - 1), "map result");
    ScheduleNextMap(std::max(1, plan.restartDelay - 1), in.nextMap, gen);
    return plan;
  }

  // Series over.
  std::string seriesWinner = "none";
  if (in.team1SeriesScore > in.team2SeriesScore) seriesWinner = "team1";
  else if (in.team2SeriesScore > in.team1SeriesScore) seriesWinner = "team2";
  if (!in.seriesWinner.empty()) seriesWinner = in.seriesWinner;
  const std::string winnerName = seriesWinner == "team1" ? base.team1Name : seriesWinner == "team2" ? base.team2Name : "";
  const int resetIn = base.scrim ? std::max(1, plan.restartDelay - 1) : plan.kickDelay;
  const double kickIn = KickDelayAfterSeriesEnd(plan.restartDelay, plan.kickDelay - (plan.restartDelay - 1));

  MatchFlowEvent seriesEv = base;
  seriesEv.type = MatchFlowEventType::SeriesEnd;
  seriesEv.mapNumber = in.mapNumber;
  seriesEv.winner = seriesWinner;
  seriesEv.secondsUntilReset = resetIn;
  Emit(seriesEv);
  // Existing webhook (shape unchanged); time_until_restore now carries the real delay.
  WebhookEmitSeriesEnd(in.team1SeriesScore, in.team2SeriesScore, seriesWinner.c_str(), resetIn);

  card.winner = seriesWinner == "none" ? "" : seriesWinner;
  card.team1Score = in.team1SeriesScore;
  card.team2Score = in.team2SeriesScore;
  card.forfeit = !winnerName.empty() && !in.seriesWinner.empty();
  ResultCardShow(SeriesResultCardHtml(card), resetIn, "series result");

  if (!winnerName.empty() && !in.seriesWinner.empty()) {
    SendToChat(("Ready Up: " + winnerName + " won the series by forfeit.").c_str());
  } else if (!winnerName.empty()) {
    SendToChat(("Ready Up: " + winnerName + " won the series " +
                std::to_string(std::max(in.team1SeriesScore, in.team2SeriesScore)) + "-" +
                std::to_string(std::min(in.team1SeriesScore, in.team2SeriesScore)) + ".")
                   .c_str());
  } else {
    SendToChat("Ready Up: the series ended in a draw.");
  }

  // Where to go now (a live test, 2026-10-15): the platform's match page has the next match.
  if (!base.scrim) SendToChat("Ready Up: go back to the platform to view your next match.");

  if (base.scrim) {
    // Pickup games: nobody is kicked, the scrim just unloads.
    ScheduleReset(resetIn, base, gen);
    return plan;
  }

  SendToChat(("Ready Up: all players will be disconnected in " + std::to_string(static_cast<int>(kickIn)) +
              "s to prepare the server for the next match.")
                 .c_str());
  const std::string kickReason = winnerName.empty() ? std::string("Series over. Thanks for playing!")
                                                    : winnerName + " won the match";
  ScheduleKickThenReset(kickIn, resetIn + kResetAfterKickSeconds - kickIn, kickReason, base, gen);
  return plan;
}

void MatchEndCancelPending() {
  g_generation.fetch_add(1);
  SetPlan(Plan{});
}

status::Json MatchEndSnapshotJson() {
  status::Json j = status::Json::Object();
  int a = 0, b = 0, c = 0;
  GetSeriesEndKickDelays(&a, &b, &c);
  j["kick_no_demo"] = a;
  j["kick_demo_no_upload"] = b;
  j["kick_demo_upload"] = c;
  std::lock_guard<std::mutex> lk(g_mu);
  if (g_plan.kind != Plan::None) {
    status::Json p = status::Json::Object();
    p["kind"] = static_cast<int>(g_plan.kind);
    p["due"] = g_plan.due;
    p["next"] = g_plan.next;
    p["kick_reason"] = g_plan.kickReason;
    p["reset_after_kick"] = g_plan.resetAfterKick;
    status::Json e = status::Json::Object();
    e["matchid"] = static_cast<long long>(g_plan.base.matchid);
    e["slug"] = g_plan.base.slug;
    e["scrim"] = g_plan.base.scrim;
    e["map_number"] = g_plan.base.mapNumber;
    e["team1_name"] = g_plan.base.team1Name;
    e["team2_name"] = g_plan.base.team2Name;
    e["team1_series_score"] = g_plan.base.team1SeriesScore;
    e["team2_series_score"] = g_plan.base.team2SeriesScore;
    e["demo_recorded"] = g_plan.base.demoRecorded;
    e["demo_upload_configured"] = g_plan.base.demoUploadConfigured;
    p["base"] = std::move(e);
    j["plan"] = std::move(p);
  }
  return j;
}

void MatchEndRestoreJson(const status::Json& j) {
  auto i = [&](const status::Json* o, const char* k, int def) {
    const auto* v = o ? o->Find(k) : nullptr;
    return v ? static_cast<int>(v->AsInt()) : def;
  };
  auto s = [&](const status::Json* o, const char* k) {
    const auto* v = o ? o->Find(k) : nullptr;
    return v ? v->AsString() : std::string();
  };
  SetSeriesEndKickDelays(i(&j, "kick_no_demo", -1), i(&j, "kick_demo_no_upload", -1), i(&j, "kick_demo_upload", -1));
  const status::Json* p = j.Find("plan");
  if (!p) return;
  const auto kind = static_cast<Plan::Kind>(i(p, "kind", 0));
  const auto* dueV = p->Find("due");
  const double left = std::max(0.0, (dueV ? dueV->AsDouble() : 0.0) - host::NowSeconds());
  MatchFlowEvent base;
  const status::Json* e = p->Find("base");
  if (const auto* v = e ? e->Find("matchid") : nullptr) base.matchid = static_cast<unsigned long long>(v->AsInt());
  base.slug = s(e, "slug");
  if (const auto* v = e ? e->Find("scrim") : nullptr) base.scrim = v->AsBool();
  base.mapNumber = i(e, "map_number", 1);
  base.team1Name = s(e, "team1_name");
  base.team2Name = s(e, "team2_name");
  base.team1SeriesScore = i(e, "team1_series_score", 0);
  base.team2SeriesScore = i(e, "team2_series_score", 0);
  if (const auto* v = e ? e->Find("demo_recorded") : nullptr) base.demoRecorded = v->AsBool();
  if (const auto* v = e ? e->Find("demo_upload_configured") : nullptr) base.demoUploadConfigured = v->AsBool();
  const unsigned gen = g_generation.load();
  const char* what = kind == Plan::NextMap ? "next map" : kind == Plan::Reset ? "match unload" : "series-end kick";
  Print("map-end: rescheduling the %s in %.1fs (plugin reloaded in postgame)\n", what, left);
  if (kind == Plan::NextMap) ScheduleNextMap(left, s(p, "next"), gen);
  else if (kind == Plan::Reset) ScheduleReset(left, base, gen);
  else if (kind == Plan::KickThenReset) {
    const auto* r = p->Find("reset_after_kick");
    ScheduleKickThenReset(left, r ? r->AsDouble() : kResetAfterKickSeconds, s(p, "kick_reason"), base, gen);
  }
}

bool MatchEndPending() { return g_pending.load() > 0; }

bool MatchFlowHandleConsoleLine(const std::string& line) {
  const auto args = SplitArgs(line);
  if (args.empty()) return false;
  if (demo::HandleConsoleLine(args)) return true;
  const std::string& cmd = args[0];
  if (cmd == "ru_series_end_kick_delay_no_demo" || cmd == "ru_series_end_kick_delay_demo_no_upload" ||
      cmd == "ru_series_end_kick_delay_demo_upload") {
    const int v = args.size() > 1 ? std::max(0, std::atoi(args[1].c_str())) : -1;
    if (cmd == "ru_series_end_kick_delay_no_demo") SetSeriesEndKickDelays(v, -1, -1);
    else if (cmd == "ru_series_end_kick_delay_demo_no_upload") SetSeriesEndKickDelays(-1, v, -1);
    else SetSeriesEndKickDelays(-1, -1, v);
    int a = 0, b = 0, c = 0;
    GetSeriesEndKickDelays(&a, &b, &c);
    Print("series end kick delays: no_demo=%d demo_no_upload=%d demo_upload=%d\n", a, b, c);
    return true;
  }
  if (cmd == "ru_match_stats") {
    // `ru_match_stats [matchid]` (the old plugin's get_match_stats <matchId>). Ready Up keeps no
    // stats database: only the loaded match's current map is known, so another id gets ME's
    // "No stats found" reply.
    const auto ctx = WebhookGetMatchContext();
    const unsigned long long loaded = ctx ? static_cast<unsigned long long>(ctx->matchid) : 0ull;
    if (args.size() > 1) {
      const std::string& a = args[1];
      if (a.empty() || a.find_first_not_of("0123456789") != std::string::npos) {
        PrintLine("Invalid match ID. Must be a number.");
        return true;
      }
      if (std::strtoull(a.c_str(), nullptr, 10) != loaded || !ctx) {
        Print("No stats found for match ID %s\n", a.c_str());
        return true;
      }
    }
    at::MatchStatsLine line;
    {
      std::lock_guard<std::recursive_mutex> lk(stats::Mutex());
      line.stats = stats::Current().Snapshot();
    }
    const auto ms = MatchStateGet();
    const auto series = ModesGetSeriesWins();
    line.matchid = static_cast<long long>(loaded);
    line.map_number = ms.map_number > 0 ? ms.map_number : 1;
    line.map_name = ms.current_map;
    if (ctx) {
      line.team1 = {ctx->team1_id, ctx->team1_name, std::max(0, series.first)};
      line.team2 = {ctx->team2_id, ctx->team2_name, std::max(0, series.second)};
    }
    // One line: consumers take everything after the first '{' (the core keeps long plugin
    // log lines whole).
    PrintLine(("match_stats " + at::MatchStatsJson(line)).c_str());
    return true;
  }
  return false;
}

}  // namespace readyup

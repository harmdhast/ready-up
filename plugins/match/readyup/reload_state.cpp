// Match state across `ru plugin reload match` (see reload_state.h).
#include "readyup/reload_state.h"

#include "readyup/config.h"
#include "readyup/cvar_snapshot.h"
#include "readyup/demo_recorder.h"
#include "readyup/fleet_bridge.h"
#include "readyup/host.h"
#include "readyup/knife_tracker.h"
#include "readyup/logging.h"
#include "readyup/mat_admins.h"
#include "readyup/match_end.h"
#include "readyup/match_events.h"
#include "readyup/match_log.h"
#include "readyup/match_state.h"
#include "readyup/match_stats.h"
#include "readyup/match_status.h"
#include "readyup/match_token.h"
#include "readyup/modes.h"
#include "readyup/pause_state.h"
#include "readyup/scrim_flow.h"
#include "readyup/status_snapshot.h"
#include "readyup/webhook.h"

#include <cstdlib>
#include <string>
#include <vector>

namespace readyup {
namespace {

using status::Json;

constexpr const char* kStashKey = "state";
constexpr int kVersion = 1;
constexpr size_t kMaxBytes = (1u << 20) - 4096;  // ru_api stash blobs are capped at 1 MiB

std::string U64(uint64_t v) { return std::to_string(static_cast<unsigned long long>(v)); }
uint64_t ToU64(const Json* v) { return v ? std::strtoull(v->AsString().c_str(), nullptr, 10) : 0; }
int Int(const Json* o, const char* k, int def = 0) {
  const Json* v = o ? o->Find(k) : nullptr;
  return v ? static_cast<int>(v->AsInt()) : def;
}
bool Bool(const Json* o, const char* k, bool def = false) {
  const Json* v = o ? o->Find(k) : nullptr;
  return v ? v->AsBool() : def;
}
std::string Str(const Json* o, const char* k) {
  const Json* v = o ? o->Find(k) : nullptr;
  return v ? v->AsString() : std::string();
}

Json ContextToJson(const WebhookMatchContext& c) {
  Json j = Json::Object();
  j["matchid"] = U64(c.matchid);
  j["slug"] = c.slug;
  j["num_maps"] = c.num_maps;
  j["team1_name"] = c.team1_name;
  j["team2_name"] = c.team2_name;
  j["team1_id"] = c.team1_id;
  j["team2_id"] = c.team2_id;
  j["team1_captain"] = U64(c.team1_captain_steamid64);
  j["team2_captain"] = U64(c.team2_captain_steamid64);
  j["knife_decision_seconds"] = c.knifeDecisionSeconds;
  j["max_rounds"] = c.maxRounds;
  j["overtime_enabled"] = c.overtime_enabled;
  j["overtime_segments"] = c.overtimeSegments;
  j["max_overtimes"] = c.maxOvertimes;
  j["damage_tiebreak"] = c.damageTiebreakEnabled;
  j["sudden_death_on_damage_tie"] = c.suddenDeathOnDamageTie;
  j["clinch_series"] = c.clinch_series;
  j["players_per_team"] = c.players_per_team;
  j["playout"] = c.playout;
  j["whitelist"] = c.whitelist;
  j["autoready"] = c.autoready;
  j["roster_names_rule"] = c.roster_names_rule;
  Json sides = Json::Array();
  for (const auto& s : c.map_sides) sides.Push(s);
  j["map_sides"] = std::move(sides);
  Json maps = Json::Array();
  for (const auto& m : c.maplist) maps.Push(m);
  j["maplist"] = std::move(maps);
  Json specs = Json::Array();
  for (uint64_t s : c.spectators) specs.Push(U64(s));
  j["spectators"] = std::move(specs);
  Json admins = Json::Array();
  for (uint64_t s : c.admins) admins.Push(U64(s));
  j["admins"] = std::move(admins);
  Json cvars = Json::Object();
  for (const auto& kv : c.cvars) cvars[kv.first] = kv.second;
  j["cvars"] = std::move(cvars);
  Json roster = Json::Object();
  for (const auto& kv : c.roster_team) roster[U64(kv.first)] = static_cast<int>(kv.second);
  j["roster"] = std::move(roster);
  j["team1_flag"] = c.team1_flag;
  j["team2_flag"] = c.team2_flag;
  Json rules = Json::Object();
  rules["tech_pauses_per_team"] = c.rules.tech_pauses_per_team;
  rules["tech_pause_max_seconds"] = c.rules.tech_pause_max_seconds;
  rules["both_teams_unpause"] = c.rules.both_teams_unpause;
  rules["allow_force_ready"] = c.rules.allow_force_ready;
  rules["team_ready"] = c.rules.team_ready;
  rules["min_players_to_ready"] = c.rules.min_players_to_ready;
  rules["min_spectators_to_ready"] = c.rules.min_spectators_to_ready;
  rules["forfeit_after_seconds"] = c.rules.forfeit_after_seconds;
  rules["gg_enabled"] = c.rules.gg_enabled;
  rules["gg_threshold_pct"] = c.rules.gg_threshold_pct;
  rules["gg_min_score_diff"] = c.rules.gg_min_score_diff;
  rules["stop_command_available"] = c.rules.stop_command_available;
  rules["stop_command_no_damage"] = c.rules.stop_command_no_damage;
  rules["stop_vote_seconds"] = c.rules.stop_vote_seconds;
  rules["pause_after_restore"] = c.rules.pause_after_restore;
  j["rules"] = std::move(rules);
  j["ruleset"] = c.ruleset;
  j["overrides"] = c.overrides_json;
  Json coaches = Json::Array();
  for (uint64_t s : c.coaches) coaches.Push(U64(s));
  j["coaches"] = std::move(coaches);
  Json coachTeam = Json::Object();
  for (const auto& kv : c.coach_team) coachTeam[std::to_string(kv.first)] = static_cast<long long>(kv.second);
  j["coach_team"] = std::move(coachTeam);
  j["coaches_per_team"] = c.coaches_per_team;
  Json subs = Json::Array();
  for (uint64_t s : c.substitutes) subs.Push(U64(s));
  j["substitutes"] = std::move(subs);
  Json names = Json::Object();
  for (const auto& kv : c.roster_names) names[U64(kv.first)] = kv.second;
  j["roster_names"] = std::move(names);
  j["wingman"] = c.wingman;
  j["bot_fill"] = c.bot_fill;
  j["simulation"] = c.simulation;
  j["simulation_timescale"] = c.simulation_timescale;
  return j;
}

WebhookMatchContext ContextFromJson(const Json& j) {
  WebhookMatchContext c;
  c.matchid = ToU64(j.Find("matchid"));
  c.slug = Str(&j, "slug");
  c.num_maps = Int(&j, "num_maps");
  c.team1_name = Str(&j, "team1_name");
  c.team2_name = Str(&j, "team2_name");
  c.team1_id = Str(&j, "team1_id");
  c.team2_id = Str(&j, "team2_id");
  c.team1_captain_steamid64 = ToU64(j.Find("team1_captain"));
  c.team2_captain_steamid64 = ToU64(j.Find("team2_captain"));
  c.knifeDecisionSeconds = Int(&j, "knife_decision_seconds", c.knifeDecisionSeconds);
  c.maxRounds = Int(&j, "max_rounds", c.maxRounds);
  c.overtime_enabled = Bool(&j, "overtime_enabled");
  c.overtimeSegments = Int(&j, "overtime_segments", c.overtimeSegments);
  c.maxOvertimes = Int(&j, "max_overtimes", c.maxOvertimes);
  c.damageTiebreakEnabled = Bool(&j, "damage_tiebreak");
  c.suddenDeathOnDamageTie = Bool(&j, "sudden_death_on_damage_tie", true);
  c.clinch_series = Bool(&j, "clinch_series", true);
  c.players_per_team = Int(&j, "players_per_team", 0);
  c.playout = Int(&j, "playout", -1);
  c.whitelist = Int(&j, "whitelist", -1);
  c.autoready = Int(&j, "autoready", -1);
  c.roster_names_rule = Int(&j, "roster_names_rule", -1);
  if (const Json* v = j.Find("map_sides")) for (const auto& s : v->Items()) c.map_sides.push_back(s.AsString());
  if (const Json* v = j.Find("maplist")) for (const auto& s : v->Items()) c.maplist.push_back(s.AsString());
  if (const Json* v = j.Find("spectators")) for (const auto& s : v->Items()) c.spectators.insert(ToU64(&s));
  if (const Json* v = j.Find("admins")) for (const auto& s : v->Items()) c.admins.insert(ToU64(&s));
  if (const Json* v = j.Find("cvars")) {
    for (const auto& kv : v->Members()) c.cvars[kv.first] = kv.second.AsString();
  }
  if (const Json* v = j.Find("roster")) {
    for (const auto& kv : v->Members()) {
      c.roster_team[std::strtoull(kv.first.c_str(), nullptr, 10)] = static_cast<WebhookTeam>(kv.second.AsInt());
    }
  }
  c.team1_flag = Str(&j, "team1_flag");
  c.team2_flag = Str(&j, "team2_flag");
  if (const Json* r = j.Find("rules")) {
    c.rules.tech_pauses_per_team = Int(r, "tech_pauses_per_team", -1);
    c.rules.tech_pause_max_seconds = Int(r, "tech_pause_max_seconds", -1);
    c.rules.both_teams_unpause = Int(r, "both_teams_unpause", -1);
    c.rules.allow_force_ready = Int(r, "allow_force_ready", -1);
    c.rules.team_ready = Int(r, "team_ready", -1);
    c.rules.min_players_to_ready = Int(r, "min_players_to_ready", -1);
    c.rules.min_spectators_to_ready = Int(r, "min_spectators_to_ready", -1);
    c.rules.forfeit_after_seconds = Int(r, "forfeit_after_seconds", -1);
    c.rules.gg_enabled = Int(r, "gg_enabled", -1);
    c.rules.gg_threshold_pct = Int(r, "gg_threshold_pct", -1);
    c.rules.gg_min_score_diff = Int(r, "gg_min_score_diff", -1);
    c.rules.stop_command_available = Int(r, "stop_command_available", -1);
    c.rules.stop_command_no_damage = Int(r, "stop_command_no_damage", -1);
    c.rules.stop_vote_seconds = Int(r, "stop_vote_seconds", -1);
    c.rules.pause_after_restore = Int(r, "pause_after_restore", -1);
  }
  c.ruleset = Str(&j, "ruleset");
  c.overrides_json = Str(&j, "overrides");
  if (const Json* v = j.Find("coaches")) for (const auto& s : v->Items()) c.coaches.insert(ToU64(&s));
  if (const Json* v = j.Find("coach_team")) {
    for (const auto& kv : v->Members()) {
      c.coach_team[std::strtoull(kv.first.c_str(), nullptr, 10)] = static_cast<WebhookTeam>(kv.second.AsInt());
    }
  }
  c.coaches_per_team = Int(&j, "coaches_per_team", c.coaches_per_team);
  if (const Json* v = j.Find("substitutes")) for (const auto& s : v->Items()) c.substitutes.insert(ToU64(&s));
  if (const Json* v = j.Find("roster_names")) {
    for (const auto& kv : v->Members()) c.roster_names[std::strtoull(kv.first.c_str(), nullptr, 10)] = kv.second.AsString();
  }
  c.wingman = Bool(&j, "wingman");
  c.bot_fill = Bool(&j, "bot_fill");
  c.simulation = Bool(&j, "simulation");
  if (const Json* v = j.Find("simulation_timescale")) c.simulation_timescale = v->AsDouble();
  return c;
}

Json Build(const std::vector<std::string>& pendingEvents, size_t firstEvent) {
  Json j = Json::Object();
  j["v"] = kVersion;

  // Settings + match context (webhook.h, match_token.h, mat_admins.h, config.h).
  Json wh = Json::Object();
  wh["url"] = WebhookBaseUrl();
  wh["heartbeat_url"] = WebhookHeartbeatUrl();
  wh["heartbeat_status"] = WebhookHeartbeatStatusString();
  if (auto ctx = WebhookGetMatchContext()) wh["context"] = ContextToJson(*ctx);
  j["cvar_snapshot"] = cvar_snapshot::ReloadJson();  // pre-match cvar values (cvar_snapshot.h)
  Json pending = Json::Array();
  for (size_t i = firstEvent; i < pendingEvents.size(); ++i) pending.Push(pendingEvents[i]);
  wh["pending"] = std::move(pending);
  j["webhook"] = std::move(wh);
  if (auto tok = GetMatchTokenCopy()) j["match_token"] = *tok;
  Json adm = Json::Object();
  adm["url"] = mat_admins::AdminsUrl();
  adm["refresh_seconds"] = mat_admins::RefreshSeconds();
  j["mat_admins"] = std::move(adm);
  j["dev_bots_scrim_override"] = DevBotsScrimOverride();

  j["modes"] = ModesSnapshotJson();
  Json scrim = Json::Object();
  scrim["auto"] = ScrimAutoToggle();
  scrim["last_map"] = ScrimLastMap();
  j["scrim"] = std::move(scrim);

  PauseSnapshot ps;
  long long pauseStart = 0;
  PauseStateSave(&ps, &pauseStart);
  Json pause = Json::Object();
  pause["paused"] = ps.paused;
  pause["team1"] = ps.team1_ready_to_unpause;
  pause["team2"] = ps.team2_ready_to_unpause;
  pause["type"] = ps.type;
  pause["by"] = ps.by;
  pause["team"] = static_cast<int>(ps.team);
  pause["start"] = pauseStart;
  int used[4] = {0, 0, 0, 0};
  PauseStateUsageSave(used);
  Json usedJ = Json::Array();
  for (int u : used) usedJ.Push(u);
  pause["used"] = std::move(usedJ);
  j["pause"] = std::move(pause);

  const auto ms = MatchStateGet();
  Json mst = Json::Object();
  mst["map_number"] = ms.map_number;
  mst["round"] = ms.round_number;
  mst["team1"] = ms.team1_score;
  mst["team2"] = ms.team2_score;
  mst["map"] = ms.current_map;
  j["match_state"] = std::move(mst);

  const auto lg = MatchLogSnapshot();
  Json log = Json::Object();
  log["map_number"] = lg.mapNumber;
  log["round"] = lg.roundNumber;
  log["team1"] = lg.team1Score;
  log["team2"] = lg.team2Score;
  log["map"] = lg.currentMap;
  j["match_log"] = std::move(log);

  const auto ev = MatchEventsSnapshot();
  Json evj = Json::Object();
  evj["round"] = ev.roundNumber;
  evj["last_map_number"] = ev.lastMapNumber;
  evj["swaps"] = ev.swapCount;
  evj["last_half_start_total"] = ev.lastHalfStartTotal;
  evj["last_overtime"] = ev.lastOvertimeNumber;
  Json totals = Json::Array();
  for (const auto& t : ev.players) {
    Json p = Json::Object();
    p["id"] = U64(t.steamid64);
    p["name"] = t.name;
    p["team"] = t.team;
    p["k"] = t.kills;
    p["d"] = t.deaths;
    p["a"] = t.assists;
    p["hs"] = t.headshot_kills;
    p["dmg"] = t.damage;
    totals.Push(std::move(p));
  }
  evj["players"] = std::move(totals);
  j["match_events"] = std::move(evj);

  {
    std::lock_guard<std::recursive_mutex> lk(stats::Mutex());
    j["stats"] = stats::ToJson(stats::Current().Snapshot());  // string: parsed back by stats::FromJson
  }

  Json knife = Json::Array();
  for (const auto& m : KnifeTrackerSave()) {
    Json k = Json::Object();
    k["userid"] = m.userid;
    k["team"] = m.team;
    k["dead"] = m.dead;
    k["health"] = m.health;
    knife.Push(std::move(k));
  }
  j["knife_tracker"] = std::move(knife);
  j["knife_tracker_active"] = KnifeTrackerActive();

  j["demo"] = demo::SnapshotJson();
  j["match_end"] = MatchEndSnapshotJson();
  j["status"] = MatchStatusSnapshotJson();
  j["fleet"] = fleet_bridge::SnapshotJson();
  return j;
}

}  // namespace

void ReloadStateSave() {
  const ru_api* a = host::Api();
  if (!a) return;
  const std::vector<std::string> pending = WebhookTakePending();
  size_t first = 0;
  std::string doc = Build(pending, first).Dump();
  // Too big for one stash blob (1 MiB): drop the oldest undelivered webhook events.
  while (doc.size() > kMaxBytes && first < pending.size()) {
    size_t over = doc.size() - kMaxBytes;
    while (first < pending.size() && over > 0) {
      const size_t sz = pending[first++].size() + 8;
      over = sz >= over ? 0 : over - sz;
    }
    doc = Build(pending, first).Dump();
  }
  if (first > 0) Print("match: reload state too large; dropped %zu undelivered webhook event(s)\n", first);
  if (doc.size() > kMaxBytes || !a->stash_put(a->self, kStashKey, doc.data(), static_cast<uint32_t>(doc.size()))) {
    Print("match: could not keep the match state for the next load (%zu bytes)\n", doc.size());
    return;
  }
  Debug("match: kept %zu bytes of match state for the next load\n", doc.size());
}

bool ReloadStateRestore() {
  const ru_api* a = host::Api();
  if (!a) return false;
  const int n = a->stash_get(a->self, kStashKey, nullptr, 0);
  if (n <= 0) return false;
  std::string doc(static_cast<size_t>(n), '\0');
  a->stash_get(a->self, kStashKey, &doc[0], static_cast<uint32_t>(doc.size()));
  a->stash_put(a->self, kStashKey, nullptr, 0);  // consumed: a later cold load must not reuse it
  Json j;
  std::string err;
  if (!Json::Parse(doc, &j, &err) || Int(&j, "v") != kVersion) {
    Print("match: ignoring the previous image's state (%s)\n", err.empty() ? "version mismatch" : err.c_str());
    return false;
  }

  // Settings first (the sender starts with the right URL / token).
  const Json* wh = j.Find("webhook");
  WebhookConfigure(Str(wh, "url"));
  WebhookConfigureHeartbeatUrl(Str(wh, "heartbeat_url"));
  if (const Json* tok = j.Find("match_token")) SetMatchToken(tok->AsString());
  // Before the context: the names already read (or asked) are not read again, which would now
  // return the match's own values.
  if (const Json* cs = j.Find("cvar_snapshot")) cvar_snapshot::RestoreReloadJson(cs->AsString());
  if (const Json* ctx = wh ? wh->Find("context") : nullptr) WebhookSetMatchContext(ContextFromJson(*ctx));
  WebhookSetHeartbeatStatus(Str(wh, "heartbeat_status").c_str());
  if (const Json* p = wh ? wh->Find("pending") : nullptr) {
    std::vector<std::string> events;
    for (const auto& e : p->Items()) events.push_back(e.AsString());
    WebhookRestorePending(std::move(events));
  }
  const Json* adm = j.Find("mat_admins");
  if (!Str(adm, "url").empty()) mat_admins::ConfigureAdminsUrl(Str(adm, "url"));
  if (Int(adm, "refresh_seconds") > 0) mat_admins::ConfigureRefreshSeconds(Int(adm, "refresh_seconds"));
  SetDevBotsScrimOverride(Int(&j, "dev_bots_scrim_override", -1));
  if (!WebhookBaseUrl().empty() || !WebhookHeartbeatUrl().empty()) WebhookStartSenderThread();

  if (const Json* m = j.Find("modes")) ModesRestoreJson(*m);
  const Json* scrim = j.Find("scrim");
  ScrimRestore(Bool(scrim, "auto", true), Str(scrim, "last_map"));

  const Json* pause = j.Find("pause");
  PauseSnapshot ps;
  ps.paused = Bool(pause, "paused");
  ps.team1_ready_to_unpause = Bool(pause, "team1");
  ps.team2_ready_to_unpause = Bool(pause, "team2");
  ps.type = Str(pause, "type");
  ps.by = Str(pause, "by");
  ps.team = static_cast<WebhookTeam>(Int(pause, "team"));
  const Json* ptart = pause ? pause->Find("start") : nullptr;
  PauseStateRestore(ps, ptart ? ptart->AsInt() : 0);
  if (const Json* u = pause ? pause->Find("used") : nullptr) {
    int used[4] = {0, 0, 0, 0};
    size_t i = 0;
    for (const auto& v : u->Items()) {
      if (i < 4) used[i++] = static_cast<int>(v.AsInt());
    }
    PauseStateUsageRestore(used);
  }

  const Json* mst = j.Find("match_state");
  MatchStateSetMap(Int(mst, "map_number", 1), Str(mst, "map"));
  MatchStateSetRound(Int(mst, "round"));
  MatchStateSetScore(Int(mst, "team1"), Int(mst, "team2"));

  const Json* log = j.Find("match_log");
  MatchLogRestore(MatchLogState{Int(log, "map_number", 1), Int(log, "round"), Int(log, "team1"), Int(log, "team2"),
                                Str(log, "map")});

  const Json* evj = j.Find("match_events");
  MatchEventsState ev;
  ev.roundNumber = Int(evj, "round");
  ev.lastMapNumber = Int(evj, "last_map_number");
  ev.swapCount = Int(evj, "swaps");
  ev.lastHalfStartTotal = Int(evj, "last_half_start_total", -1);
  ev.lastOvertimeNumber = Int(evj, "last_overtime");
  if (const Json* ps2 = evj ? evj->Find("players") : nullptr) {
    for (const auto& p : ps2->Items()) {
      MatchEventsState::Totals t;
      t.steamid64 = ToU64(p.Find("id"));
      t.name = Str(&p, "name");
      t.team = Int(&p, "team");
      t.kills = Int(&p, "k");
      t.deaths = Int(&p, "d");
      t.assists = Int(&p, "a");
      t.headshot_kills = Int(&p, "hs");
      t.damage = Int(&p, "dmg");
      ev.players.push_back(std::move(t));
    }
  }
  MatchEventsRestore(ev);

  stats::MapStats mstats;
  if (stats::FromJson(Str(&j, "stats"), &mstats)) {
    std::lock_guard<std::recursive_mutex> lk(stats::Mutex());
    stats::Current().Restore(mstats);
  }

  std::vector<KnifeTrackerMember> members;
  if (const Json* kt = j.Find("knife_tracker")) {
    for (const auto& k : kt->Items()) {
      members.push_back(KnifeTrackerMember{Int(&k, "userid", -1), Int(&k, "team"), Bool(&k, "dead"), Int(&k, "health", 100)});
    }
  }
  KnifeTrackerRestore(Bool(&j, "knife_tracker_active"), members);

  if (const Json* d = j.Find("demo")) demo::RestoreJson(*d);
  if (const Json* me = j.Find("match_end")) MatchEndRestoreJson(*me);
  if (const Json* st = j.Find("status")) MatchStatusRestoreJson(*st);
  if (const Json* fl = j.Find("fleet")) fleet_bridge::RestoreJson(*fl);

  const auto ctx = WebhookGetMatchContext();
  Print("match: restored the previous image's state (mode=%s match=%s round=%d score=%d-%d)\n", GetModeString(),
        ctx ? (ctx->slug.empty() ? U64(ctx->matchid).c_str() : ctx->slug.c_str()) : "none",
        MatchStateGet().round_number, MatchStateGet().team1_score, MatchStateGet().team2_score);
  return true;
}

}  // namespace readyup

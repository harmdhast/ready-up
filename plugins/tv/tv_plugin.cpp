// readyup-tv ("broadcast"): owns the server's GOTV settings so they hold for a whole series.
// Design: docs/design/tv-plugin.md. Pure logic in tv_rules.h (ctest `tv_rules`); the profile and
// its validator in libs/readyup/broadcast_profile.h (shared with the match plugin).
//
//   ru tv [status]                       GOTV state
//   ru tv set <key> <value|default>      server setting (saved)
//   ru tv match <key> <value|default>    this match only
//   ru tv caster add|remove <steamid64>  | list | clear
//   ru tv reapply | help
//
// Chat: `.ru tv ...` and `.tv ...`, admins only. Keys: delay_seconds, allow_insecure_observers,
// max_clients, relay_text_chat, relay_voice. allow_insecure_observers sets tv_secure_bypass and
// sv_deltaticks_enforce together (GOTV only; the game port stays VAC-secured).
//
// Never reads tv_password with cvar_query (the engine would print it) and never logs or sends it.
#include "tv_rules.h"

#include "readyup/broadcast_profile.h"
#include "readyup/fleet_iface.h"
#include "readyup/match_iface.h"
#include "readyup/minijson.h"
#include "readyup/plugin_api.h"
#include "readyup/selftest_iface.h"
#include "readyup/tv_iface.h"
#include "readyup/whitelist_iface.h"

#include <algorithm>
#include <cstddef>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <map>
#include <optional>
#include <set>
#include <sstream>
#include <string>
#include <vector>

#ifndef TV_VERSION
#define TV_VERSION "dev"
#endif

namespace tv {
namespace {

namespace bc = readyup::broadcast;
using readyup::minijson::Value;

const ru_api* g_api = nullptr;
std::string g_dir;
double g_now = 0;

// Config (tv.cfg) and runtime settings (settings.json).
bool g_enabled = true;
int g_clientsPollSeconds = 60;
Profile g_cfgProfile;
std::set<uint64_t> g_cfgCasters;
Settings g_settings;

MatchTracker g_tracker;
Applier g_applier;
Observed g_observed;
LaunchInfo g_launch;
std::optional<bool> g_passwordObserved;  // an observed `tv_password` console line: set / empty
std::optional<MatchProfile> g_restore;   // active.json, handed to the tracker on the first frame

// Scheduling (seconds on the plugin clock, g_now).
std::vector<std::pair<double, bool>> g_applyAt;  // (when, force)
double g_verifyAt = -1;
double g_lastVerify = -1e9;
double g_lastTvStatus = -1e9;
double g_tvStatusUntil = -1;
TvStatus g_tvStatusPartial;
double g_lastHousekeeping = -1e9;
double g_lastGuestPush = -1e9;
double g_lastDriftReapply = -1e9;
double g_driftCheckAt = -1;
std::set<std::string> g_baselineAsked;  // cvar_query in flight
std::set<std::string> g_mismatchLogged;
std::string g_lastMode;
// match.update set_rules with rules.broadcast: applied once the match plugin reports that config_rev
// (it may refuse the update: compare-and-set).
struct PendingUpdate {
  std::string matchId;
  int64_t configRev = 0;
  Profile profile;
  double since = 0;
};
std::optional<PendingUpdate> g_update;
std::vector<ru_handle> g_observers;

// Fleet link.
uint64_t g_fleetInstance = 0;
std::vector<uint64_t> g_fleetHandlers;

// tvState.
std::string g_stateJson;
uint64_t g_stateRev = 1;

// ---- helpers --------------------------------------------------------------------------------

std::string ReadFile(const std::string& path) {
  std::ifstream f(path);
  if (!f) return {};
  std::stringstream ss;
  ss << f.rdbuf();
  return ss.str();
}

bool WriteFile(const std::string& path, const std::string& body) {
  if (path.empty()) return false;
  const std::string tmp = path + ".tmp";
  {
    std::ofstream f(tmp, std::ios::trunc);
    if (!f) return false;
    f << body;
    if (!f) return false;
  }
  return std::rename(tmp.c_str(), path.c_str()) == 0;
}

std::string Cfg(const char* key) {
  char buf[2048];
  const int n = g_api->config_get(g_api->self, key, buf, sizeof(buf));
  return n < 0 ? std::string() : std::string(buf, static_cast<size_t>(std::min<int>(n, sizeof(buf) - 1)));
}
bool CfgSet(const char* key) {
  char buf[4];
  return g_api->config_get(g_api->self, key, buf, sizeof(buf)) >= 0;
}

void LoadConfig() {
  const std::string en = Cfg("enabled");
  g_enabled = !(en == "0" || en == "false" || en == "off" || en == "no");
  g_clientsPollSeconds = 60;
  if (CfgSet("clients_poll_seconds")) g_clientsPollSeconds = std::max(0, std::min(3600, std::atoi(Cfg("clients_poll_seconds").c_str())));
  std::map<std::string, std::string> kv;
  for (Key k : bc::AllKeys()) {
    if (CfgSet(bc::KeyName(k))) kv[bc::KeyName(k)] = Cfg(bc::KeyName(k));
  }
  std::vector<std::string> errors;
  g_cfgProfile = ProfileFromConfig(kv, &errors);
  for (const auto& e : errors) ru_logf(g_api, RU_LOG_WARN, "tv.cfg: %s (ignored)", e.c_str());
  int bad = 0;
  g_cfgCasters = ParseSteamIdList(Cfg("casters"), &bad);
  if (bad) ru_logf(g_api, RU_LOG_WARN, "tv.cfg: casters: %d entr%s not a SteamID64 (ignored)", bad, bad == 1 ? "y is" : "ies are");
}

std::string SettingsPath() { return g_dir.empty() ? std::string() : g_dir + "/settings.json"; }
std::string ActivePath() { return g_dir.empty() ? std::string() : g_dir + "/active.json"; }

bool SaveSettings() { return WriteFile(SettingsPath(), SettingsJson(g_settings)); }

void PersistActive() {
  if (g_dir.empty()) return;
  if (g_tracker.active) (void)WriteFile(ActivePath(), MatchProfileJson(*g_tracker.active));
  else std::remove(ActivePath().c_str());
}

void StashApplier() {
  const std::string j = ApplierJson(g_applier);
  g_api->stash_put(g_api->self, "applier", j.data(), static_cast<uint32_t>(j.size()));
}

const std::set<uint64_t>& Casters() { return g_settings.castersSet ? g_settings.casters : g_cfgCasters; }

Profile ServerProfile() { return bc::Overlay(g_cfgProfile, g_settings.server); }

Profile Effective(int* floor) {
  Profile eff = ServerProfile();
  int f = 0;
  if (g_tracker.active) {
    // Server < the match's own cvars < rules.broadcast < the admin's match override.
    eff = bc::Overlay(eff, bc::ProfileFromMatchCvars(g_tracker.active->matchCvars));
    eff = bc::Overlay(bc::Overlay(eff, g_tracker.active->profile), g_tracker.active->override_);
    f = g_tracker.active->delayFloor;
  }
  if (floor) *floor = f;
  return eff;
}

std::optional<bool> PasswordSet() {
  if (g_passwordObserved) return g_passwordObserved;
  if (g_tracker.active && g_tracker.active->passwordSet) return true;
  return g_launch.passwordSet;
}

const ru_whitelist_v1* Whitelist() {
  return static_cast<const ru_whitelist_v1*>(g_api->get_interface(g_api->self, RU_WHITELIST_IFACE_NAME, 1));
}

void ScheduleApply(double in, bool force) { g_applyAt.emplace_back(g_now + in, force); }

// ---- applying -------------------------------------------------------------------------------

void Apply(bool force);

void OnBaseline(void*, const char* name, const char* value) {
  try {
    if (!g_api || !name) return;
    g_baselineAsked.erase(name);
    RecordBaseline(g_applier, name, value);
    if (!value) ru_logf(g_api, RU_LOG_DEBUG, "tv: %s: no value read (it will not be restored)", name);
    StashApplier();
    if (g_baselineAsked.empty()) Apply(false);
  } catch (...) {
  }
}

void OnObservedValue(void*, const char* name, const char* value) {
  try {
    if (!g_api || !name) return;
    if (std::strstr(name, "password")) return;
    g_observed.cvars[name] = value ? value : "";
  } catch (...) {
  }
}

bool HaveCvarQuery() { return RU_API_HAS(g_api, cvar_query) && g_api->cvar_query; }

// Reads the owned cvars (+ tv_enable, tv_port) back for the status / tvState. Never tv_password.
void Verify() {
  g_lastVerify = g_now;
  if (!HaveCvarQuery()) return;
  g_driftCheckAt = g_now + 1.5;  // the answers come a frame or two later
  std::vector<std::string> names = bc::OwnedCvars();
  names.push_back("tv_enable");
  if (g_launch.tvPort <= 0) names.push_back("tv_port");
  for (const auto& n : names) (void)g_api->cvar_query(g_api->self, n.c_str(), &OnObservedValue, nullptr);
}

void Apply(bool force) {
  if (!g_api || !g_enabled) return;
  int floor = 0;
  const Profile eff = Effective(&floor);
  const auto desired = bc::DesiredCvars(eff, floor);
  // Owned cvars the match's config.cvars set (the platform sends tv_secure_bypass 1 +
  // sv_deltaticks_enforce 0 with allow_insecure_observers): the match plugin already applied them,
  // so the engine's value now is the match's, not a baseline. No baseline: tv_secure_bypass goes
  // to 0 afterwards, sv_deltaticks_enforce is the match plugin's to restore (its cvar snapshot).
  if (g_tracker.active) {
    for (const auto& kv : g_tracker.active->matchCvars) {
      if (!g_applier.applied.count(kv.first) && !g_applier.baseline.count(kv.first) && !g_applier.baselineUnknown.count(kv.first)) {
        RecordBaseline(g_applier, kv.first, nullptr);
      }
    }
  }
  const Plan plan = PlanApply(g_applier, desired, force);
  for (const auto& n : plan.needBaseline) {
    if (g_baselineAsked.count(n)) continue;
    if (!HaveCvarQuery()) {
      RecordBaseline(g_applier, n, nullptr);  // core older than 1.11: nothing to restore to
      continue;
    }
    if (g_api->cvar_query(g_api->self, n.c_str(), &OnBaseline, nullptr) == 1) g_baselineAsked.insert(n);
    // Refused (no command buffer yet, early boot): the next trigger asks again.
  }
  if (!HaveCvarQuery() && !plan.needBaseline.empty()) {
    StashApplier();
    Apply(force);
    return;
  }
  std::string sent;
  for (const auto& c : plan.commands) {
    if (g_api->server_command(g_api->self, c.c_str()) == 1) sent += (sent.empty() ? "" : ", ") + c;
  }
  for (const auto& n : plan.notes) ru_logf(g_api, RU_LOG_INFO, "tv: %s", n.c_str());
  if (!plan.commands.empty()) {
    // Repeated forced re-applies are routine (map loads): only say so with debug=1.
    ru_logf(g_api, force ? RU_LOG_DEBUG : RU_LOG_INFO, "tv: applied %s", sent.c_str());
    g_verifyAt = g_now + 3.0;
    g_driftCheckAt = -1;  // what was read before this is stale
  }
  StashApplier();
}

// After a verify: an owned cvar the engine reports differently from what tv.so set (a cfg the
// re-apply triggers missed) is set again, at most every 30 s.
void CheckDrift() {
  bool drift = false;
  for (const auto& kv : g_applier.applied) {
    auto it = g_observed.cvars.find(kv.first);
    if (it == g_observed.cvars.end() || it->second.empty()) continue;
    if (std::atof(it->second.c_str()) == std::atof(kv.second.c_str())) {
      g_mismatchLogged.erase(kv.first);
      continue;
    }
    drift = true;
    if (g_mismatchLogged.insert(kv.first).second) {
      ru_logf(g_api, RU_LOG_WARN, "tv: %s is %s, Ready Up set %s: setting it again", kv.first.c_str(), it->second.c_str(),
              kv.second.c_str());
    }
  }
  if (drift && g_now - g_lastDriftReapply >= 30.0) {
    g_lastDriftReapply = g_now;
    ScheduleApply(0.5, true);
  }
}

// ---- the match ------------------------------------------------------------------------------

const ru_match_v1* Match() {
  return static_cast<const ru_match_v1*>(g_api->get_interface(g_api->self, RU_MATCH_IFACE_NAME, 1));
}

bool MatchMode(const std::string& m) {
  return m == "match_warmup" || m == "match_knife" || m == "match_live" || m == "postgame";
}

// The platform match id the match plugin has loaded ("" = none): MatchState.match_id of a fleet
// match, else the status summary's slug (the platform id too, AssignToMatConfig).
std::string LoadedMatchId(const std::string& mode, int64_t* configRev = nullptr) {
  if (configRev) *configRev = -1;
  if (!MatchMode(mode)) return {};
  const ru_match_v1* m = Match();
  if (!m || !m->get_status) return {};
  ru_match_status st{};
  st.struct_size = sizeof(st);
  if (m->get_status(&st) != 1) return {};
  readyup::minijson::ParseError pe;
  for (const char* j : {st.state_json, st.summary_json}) {
    if (!j || !*j) continue;
    const auto v = readyup::minijson::Parse(j, &pe);
    if (!v) continue;
    const Value* id = v->get(j == st.state_json ? "match_id" : "slug");
    if (j == st.state_json && configRev) {
      if (const Value* r = v->get("config_rev"); r && r->type == Value::Type::Number) *configRev = static_cast<int64_t>(r->num);
    }
    if (id && id->type == Value::Type::String && !id->str.empty()) return id->str;
  }
  return {};
}

std::string CurrentMode() {
  const ru_match_v1* m = Match();
  if (m && RU_API_HAS(m, mode) && m->mode) {
    const char* s = m->mode();
    return s ? s : "";
  }
  return {};
}

void OnTrackEvent(TrackEvent ev) {
  if (ev == TrackEvent::Activated) {
    ru_logf(g_api, RU_LOG_INFO, "tv: match %s: broadcast profile %s (GOTV delay minimum %d s)",
            g_tracker.active->matchId.c_str(), bc::ProfileJson(g_tracker.active->profile).c_str(),
            g_tracker.active->delayFloor);
    PersistActive();
    Apply(true);
  } else if (ev == TrackEvent::Deactivated) {
    ru_logf(g_api, RU_LOG_INFO, "tv: the match is gone: back to the server's broadcast settings");
    PersistActive();
    Apply(true);
  } else if (ev == TrackEvent::Dropped) {
    ru_logf(g_api, RU_LOG_INFO, "tv: an assigned match never loaded: its broadcast profile is dropped");
  }
}

// ---- fleet ----------------------------------------------------------------------------------

const ru_fleet_v1* Fleet() {
  return static_cast<const ru_fleet_v1*>(g_api->get_interface(g_api->self, RU_FLEET_IFACE_NAME, 1));
}

template <typename T>
bool FleetHas(const ru_fleet_v1* f, size_t off) {
  return f && f->struct_size >= off + sizeof(T);
}

void OnAssign(void*, const ru_fleet_msg* msg) {
  try {
    if (!msg || !msg->payload_json) return;
    readyup::minijson::ParseError pe;
    const auto v = readyup::minijson::Parse(msg->payload_json, &pe);
    if (!v || v->type != Value::Type::Object) return;
    const Value* id = v->get("match_id");
    const Value* cfg = v->get("config");
    if (!id || id->type != Value::Type::String || !cfg || cfg->type != Value::Type::Object) return;
    std::string err;
    if (!bc::ValidateAssignConfig(*cfg, &err)) {
      ru_logf(g_api, RU_LOG_WARN, "tv: match %s: %s (the match plugin refuses the assign)", id->str.c_str(), err.c_str());
      return;
    }
    MatchProfile p;
    p.matchId = id->str;
    (void)bc::ProfileFromAssign(*cfg, &p.profile, nullptr);
    p.delayFloor = bc::DelayFloor(*cfg);
    p.passwordSet = bc::PasswordInAssign(*cfg);
    p.matchCvars = bc::OwnedCvarsInAssign(*cfg);
    const bool same = g_tracker.active && g_tracker.active->matchId == p.matchId;
    g_tracker.Assign(std::move(p), g_now);
    if (same) {
      PersistActive();
      Apply(true);
    }
  } catch (...) {
  }
}

void OnUnassign(void*, const ru_fleet_msg* msg) {
  try {
    std::string id;
    if (msg && msg->payload_json) {
      readyup::minijson::ParseError pe;
      const auto v = readyup::minijson::Parse(msg->payload_json, &pe);
      if (v && v->get("match_id") && v->get("match_id")->type == Value::Type::String) id = v->get("match_id")->str;
    }
    const bool wasActive = g_tracker.active.has_value();
    if (g_tracker.Unassign(id) && wasActive && !g_tracker.active) {
      ru_logf(g_api, RU_LOG_INFO, "tv: match unassigned: back to the server's broadcast settings");
      PersistActive();
      Apply(true);
    }
  } catch (...) {
  }
}

void OnUpdate(void*, const ru_fleet_msg* msg) {
  try {
    if (!msg || !msg->payload_json) return;
    readyup::minijson::ParseError pe;
    const auto v = readyup::minijson::Parse(msg->payload_json, &pe);
    if (!v || v->type != Value::Type::Object) return;
    const Value* id = v->get("match_id");
    const Value* rev = v->get("config_rev");
    const Value* ops = v->get("ops");
    if (!id || id->type != Value::Type::String || !rev || rev->type != Value::Type::Number || !ops ||
        ops->type != Value::Type::Array) {
      return;
    }
    MatchProfile* target = g_tracker.active && g_tracker.active->matchId == id->str ? &*g_tracker.active
                           : g_tracker.pending && g_tracker.pending->matchId == id->str ? &*g_tracker.pending
                                                                                         : nullptr;
    if (!target) return;
    Profile next = target->profile;
    bool any = false;
    for (const auto& op : ops->arr) {
      const Value* name = op.get("op");
      const Value* rules = op.get("rules");
      if (!name || name->type != Value::Type::String || name->str != "set_rules" || !rules) continue;
      bool touched = false;
      std::string err;
      if (!ApplyBroadcastPatch(*rules, &next, &touched, &err)) {
        ru_logf(g_api, RU_LOG_WARN, "tv: match.update for %s: %s (broadcast unchanged)", id->str.c_str(), err.c_str());
        return;
      }
      any = any || touched;
    }
    if (!any) return;
    if (const std::string c = bc::ConflictWithMatchCvars(next, target->delayFloor, target->matchCvars); !c.empty()) {
      ru_logf(g_api, RU_LOG_WARN, "tv: match.update for %s: rules.broadcast disagrees with the match's cvars on %s (broadcast unchanged)",
              id->str.c_str(), c.c_str());
      return;
    }
    const bool isActive = g_tracker.active.has_value() && target == &*g_tracker.active;
    if (!isActive) {
      target->profile = next;  // not loaded yet: the update rides along
      return;
    }
    g_update = PendingUpdate{id->str, static_cast<int64_t>(rev->num), next, g_now};
  } catch (...) {
  }
}

// The match plugin took the update (its config_rev reached it): the new broadcast profile applies.
void MaybeApplyUpdate(int64_t loadedRev) {
  if (!g_update) return;
  if (!g_tracker.active || g_tracker.active->matchId != g_update->matchId || g_now - g_update->since > 30.0) {
    g_update.reset();
    return;
  }
  if (loadedRev < g_update->configRev) return;
  g_tracker.active->profile = g_update->profile;
  g_update.reset();
  ru_logf(g_api, RU_LOG_INFO, "tv: match %s: broadcast profile updated to %s", g_tracker.active->matchId.c_str(),
          bc::ProfileJson(g_tracker.active->profile).c_str());
  PersistActive();
  Apply(true);
}

void FleetLink() {
  const ru_fleet_v1* f = Fleet();
  if (!f || !FleetHas<decltype(f->instance_id)>(f, offsetof(ru_fleet_v1, instance_id)) || !f->instance_id) {
    g_fleetInstance = 0;
    g_fleetHandlers.clear();
    return;
  }
  const uint64_t inst = f->instance_id();
  if (inst == g_fleetInstance) return;
  g_fleetInstance = inst;
  g_fleetHandlers.clear();  // fleet.so reloaded: its registrations are gone
  if (!FleetHas<decltype(f->register_handler)>(f, offsetof(ru_fleet_v1, register_handler))) return;
  for (const auto& t : {std::make_pair("match.assign", &OnAssign), std::make_pair("match.update", &OnUpdate),
                        std::make_pair("match.unassign", &OnUnassign)}) {
    if (const uint64_t h = f->register_handler(t.first, t.second, nullptr)) g_fleetHandlers.push_back(h);
  }
  if (FleetHas<decltype(f->add_capability)>(f, offsetof(ru_fleet_v1, add_capability)) && f->add_capability) {
    f->add_capability("tv.v1");
  }
}

bool FleetOnline() {
  const ru_fleet_v1* f = Fleet();
  return f && FleetHas<decltype(f->connection_state)>(f, offsetof(ru_fleet_v1, connection_state)) && f->connection_state &&
         f->connection_state() == RU_FLEET_LINK_ONLINE;
}

// ---- state ----------------------------------------------------------------------------------

View BuildView() {
  View v;
  v.available = g_enabled;
  v.port = g_launch.tvPort;
  v.observed = g_observed;
  int floor = 0;
  v.effective = Effective(&floor);
  v.delayFloor = floor;
  (void)bc::DesiredCvars(v.effective, floor, &v.effectiveDelay);
  if (g_tracker.active) {
    v.source = "match";
    v.matchId = g_tracker.active->matchId;
  }
  v.passwordSet = PasswordSet();
  v.casters = Casters().size();
  v.serverInsecure = g_launch.insecure;
  v.whitelistLoaded = Whitelist() != nullptr;
  return v;
}

void RefreshState() {
  const std::string j = StateJson(BuildView());
  if (j != g_stateJson) {
    g_stateJson = j;
    ++g_stateRev;
  }
}

// ---- casters --------------------------------------------------------------------------------

void PushGuests() {
  const ru_whitelist_v1* w = Whitelist();
  if (!w || !RU_API_HAS(w, set_guests) || !w->set_guests) return;
  const auto& c = Casters();
  std::vector<uint64_t> ids(c.begin(), c.end());
  (void)w->set_guests("tv", ids.data(), static_cast<uint32_t>(ids.size()));
}

// ---- tv_status (client count) ----------------------------------------------------------------

void AskTvStatus() {
  g_lastTvStatus = g_now;
  g_tvStatusPartial = TvStatus{};
  if (g_api->server_command(g_api->self, "tv_status") == 1) g_tvStatusUntil = g_now + 2.0;
}

void OnLogLine(void*, const char* line) {
  try {
    if (!line || g_tvStatusUntil < 0 || g_now > g_tvStatusUntil) return;
    std::string l = line;
    // "L 10/17/2026 - 20:00:00: ..." when it comes from the log file.
    if (l.size() > 25 && l[0] == 'L' && l[1] == ' ' && l.compare(13, 3, " - ") == 0) {
      const size_t c = l.find(": ", 16);
      if (c != std::string::npos) l = l.substr(c + 2);
    }
    if (!ParseTvStatusLine(l, &g_tvStatusPartial)) return;
    if (g_tvStatusPartial.active && !*g_tvStatusPartial.active) g_observed.clients = 0;
    if (g_tvStatusPartial.clients) g_observed.clients = g_tvStatusPartial.clients;
  } catch (...) {
  }
}

// ---- commands -------------------------------------------------------------------------------

void Reply(const ru_command_ctx* ctx, const std::string& msg) {
  if (ctx->is_console) ru_logf(g_api, RU_LOG_INFO, "%s", msg.c_str());
  else if (ctx->slot >= 0) g_api->chat_to_slot(g_api->self, ctx->slot, ("Ready Up tv: " + msg).c_str());
}

void Help(const ru_command_ctx* ctx) {
  for (const char* l : {".ru tv [status]: GOTV port, delay, clients, insecure observers, profile",
                        ".ru tv set <key> <value|default>: server setting (saved). Keys: delay_seconds, "
                        "allow_insecure_observers, max_clients, relay_text_chat, relay_voice",
                        ".ru tv match <key> <value|default>: this match only",
                        ".ru tv caster add|remove <steamid64> | list | clear: casters may join as spectators",
                        ".ru tv reapply: set every GOTV value again now (.tv works too)"}) {
    Reply(ctx, l);
  }
}

void Status(const ru_command_ctx* ctx) {
  for (const auto& l : StatusLines(BuildView())) Reply(ctx, l);
  // Fresh engine values for the next look (and the tvState).
  Verify();
  AskTvStatus();
}

// args: what follows "tv" (ru tv ... / .ru tv ... / .tv ...).
void RunTv(const ru_command_ctx* ctx, const std::vector<std::string>& args) {
  const std::string sub = args.empty() ? "status" : args[0];
  if (sub == "help") return Help(ctx);
  if (!ctx->is_console && g_api->is_admin(g_api->self, ctx->steamid64) != 1) return Reply(ctx, "not authorized");
  if (sub == "status") return Status(ctx);
  if (sub == "reapply") {
    Apply(true);
    return Reply(ctx, g_enabled ? "GOTV settings sent again" : "off (tv.cfg enabled=0)");
  }
  if (sub == "set" || sub == "match") {
    Key k;
    if (args.size() < 3 || !bc::ParseKeyName(args[1], &k)) {
      return Reply(ctx, "usage: .ru tv " + sub + " <delay_seconds|allow_insecure_observers|max_clients|relay_text_chat|relay_voice> <value|default>");
    }
    std::string err;
    if (sub == "match") {
      if (!g_tracker.active) return Reply(ctx, "no platform match is loaded (use .ru tv set for the server setting)");
      Profile o = g_tracker.active->override_;
      if (!bc::SetFromText(&o, k, args[2], &err)) return Reply(ctx, err);
      if (k == Key::Delay && o.delay_seconds && *o.delay_seconds < g_tracker.active->delayFloor) {
        return Reply(ctx, "the match needs a GOTV delay of at least " + std::to_string(g_tracker.active->delayFloor) + " s");
      }
      const Profile merged = bc::Overlay(bc::Overlay(bc::Overlay(ServerProfile(), bc::ProfileFromMatchCvars(g_tracker.active->matchCvars)), g_tracker.active->profile), o);
      if (const std::string c = bc::ConflictWithMatchCvars(merged, g_tracker.active->delayFloor, g_tracker.active->matchCvars);
          !c.empty()) {
        return Reply(ctx, "the match's own cvars set " + c + ": change it on the platform");
      }
      g_tracker.active->override_ = o;
      PersistActive();
    } else {
      Settings next = g_settings;
      if (!bc::SetFromText(&next.server, k, args[2], &err)) return Reply(ctx, err);
      g_settings = next;
      if (!SaveSettings()) Reply(ctx, "could not save " + SettingsPath());
    }
    Apply(false);
    RefreshState();
    int floor = 0;
    const Profile eff = Effective(&floor);
    std::string now = bc::ValueText(eff, k);
    if (k == Key::Delay && eff.delay_seconds && *eff.delay_seconds < floor) now = std::to_string(floor) + " (the match minimum)";
    ru_logf(g_api, RU_LOG_INFO, "tv: %s %s %s by %s", sub == "match" ? "match" : "server", bc::KeyName(k), args[2].c_str(),
            ctx->name ? ctx->name : "?");
    return Reply(ctx, std::string(bc::KeyName(k)) + " now " + (now.empty() ? "not managed (engine / cfg value)" : now));
  }
  if (sub == "caster" || sub == "casters") {
    const std::string verb = args.size() >= 2 ? args[1] : "list";
    Settings next = g_settings;
    if (!next.castersSet) {
      next.casters = g_cfgCasters;
      next.castersSet = true;
    }
    if (verb == "list") {
      std::string l;
      for (uint64_t id : Casters()) l += (l.empty() ? "" : " ") + std::to_string(id);
      return Reply(ctx, std::to_string(Casters().size()) + " caster(s)" + (l.empty() ? "" : ": " + l) +
                            (Whitelist() ? "" : " (whitelist.so is not loaded: they are only let in as a match's spectators)"));
    }
    if (verb == "clear") {
      next.casters.clear();
    } else if (verb == "add" || verb == "remove") {
      const uint64_t id = args.size() >= 3 ? ParseSteamId64(args[2]) : 0;
      if (!id) return Reply(ctx, "usage: .ru tv caster " + verb + " <steamid64>");
      if (verb == "add") {
        if (next.casters.size() >= 256) return Reply(ctx, "at most 256 casters");
        next.casters.insert(id);
      } else {
        next.casters.erase(id);
      }
    } else {
      return Reply(ctx, "usage: .ru tv caster add|remove <steamid64> | list | clear");
    }
    g_settings = next;
    if (!SaveSettings()) Reply(ctx, "could not save " + SettingsPath());
    PushGuests();
    RefreshState();
    return Reply(ctx, std::to_string(Casters().size()) + " caster(s)");
  }
  Reply(ctx, "unknown command. Type .ru tv help for the list.");
}

void OnRu(void*, const ru_command_ctx* ctx) {
  try {
    std::vector<std::string> args;
    for (int i = 2; i < ctx->argc; ++i) args.emplace_back(ctx->argv[i]);  // argv: ru tv ...
    RunTv(ctx, args);
  } catch (...) {
  }
}

void OnChatTv(void*, const ru_command_ctx* ctx) {
  try {
    std::vector<std::string> args;
    for (int i = 1; i < ctx->argc; ++i) args.emplace_back(ctx->argv[i]);  // argv: .tv ...
    RunTv(ctx, args);
  } catch (...) {
  }
}

// An owned cvar (or tv_password) typed on the console / exec'd. tv_password: only whether it is
// empty is kept. Another value for a cvar tv.so manages: set it again in a second.
void OnObserve(void*, const ru_command_ctx* ctx) {
  try {
    if (!ctx || ctx->argc < 1) return;
    const std::string name = ctx->argv[0];
    if (name == "tv_password") {
      if (ctx->argc < 2) return;  // a read (prints the value; not ours)
      const std::string v = ctx->argv[1];
      g_passwordObserved = !(v.empty() || v == "\"\"");
      return;
    }
    if (ctx->argc < 2) return;  // a read (cvar_query)
    auto it = g_applier.applied.find(name);
    if (it == g_applier.applied.end()) return;
    if (std::atof(ctx->argv[1]) == std::atof(it->second.c_str())) return;
    ScheduleApply(1.0, true);
  } catch (...) {
  }
}

// ---- events ---------------------------------------------------------------------------------

void OnMapStart(void*, const ru_event*) {
  try {
    LoadConfig();
    // After server.cfg, the game mode cfgs and the match plugin's after-cfg commands.
    ScheduleApply(0.0, true);
    ScheduleApply(2.0, true);
    ScheduleApply(8.0, true);
  } catch (...) {
  }
}

void OnMatchStart(void*, const ru_event*) {
  try {
    ScheduleApply(1.0, true);
  } catch (...) {
  }
}

void OnFrame(void*, const ru_tick_info* t) {
  try {
    g_now = t->now;
    // First frame: a match profile from before a reload / restart goes back first, so the first
    // apply does not drop to the server profile for a second in the middle of a match.
    if (g_restore) {
      g_tracker.Assign(std::move(*g_restore), g_now, 900.0);
      g_restore.reset();
      const TrackEvent ev = g_tracker.Observe(LoadedMatchId(CurrentMode()), g_now);
      if (ev == TrackEvent::Activated) {
        ru_logf(g_api, RU_LOG_INFO, "tv: match %s: broadcast profile restored", g_tracker.active->matchId.c_str());
      }
    }
    // Due re-applies (several due at once collapse into one).
    bool due = false, force = false;
    for (auto it = g_applyAt.begin(); it != g_applyAt.end();) {
      if (it->first <= g_now) {
        due = true;
        force = force || it->second;
        it = g_applyAt.erase(it);
      } else {
        ++it;
      }
    }
    if (due) Apply(force);
    if (g_verifyAt >= 0 && g_now >= g_verifyAt) {
      g_verifyAt = -1;
      Verify();
    }
    if (g_driftCheckAt >= 0 && g_now >= g_driftCheckAt) {
      g_driftCheckAt = -1;
      CheckDrift();
    }
    if (g_tvStatusUntil >= 0 && g_now > g_tvStatusUntil) g_tvStatusUntil = -1;

    if (g_now - g_lastHousekeeping < 1.0) return;
    g_lastHousekeeping = g_now;
    FleetLink();
    const std::string mode = CurrentMode();
    if (mode != g_lastMode) {
      // The match plugin execs a cfg on a phase change (warmup, knife, live).
      if (!g_lastMode.empty() || !mode.empty()) {
        ScheduleApply(1.0, true);
        ScheduleApply(5.0, true);
      }
      g_lastMode = mode;
    }
    int64_t loadedRev = -1;
    OnTrackEvent(g_tracker.Observe(LoadedMatchId(mode, &loadedRev), g_now));
    MaybeApplyUpdate(loadedRev);
    if (g_now - g_lastGuestPush >= 5.0) {
      g_lastGuestPush = g_now;
      PushGuests();
    }
    if (g_clientsPollSeconds > 0 && g_now - g_lastTvStatus >= g_clientsPollSeconds && FleetOnline()) AskTvStatus();
    if (g_now - g_lastVerify >= 300.0 && FleetOnline()) Verify();
    RefreshState();
  } catch (...) {
  }
}

// ---- readyup.tv.v1 --------------------------------------------------------------------------

void Copy(char* dst, uint32_t len, const std::string& s) {
  if (!dst || len == 0) return;
  const size_t n = std::min<size_t>(s.size(), len - 1);
  std::memcpy(dst, s.data(), n);
  dst[n] = '\0';
}

uint64_t IfaceStateRev() { return g_stateRev; }

int IfaceStateJson(char* buf, uint32_t len) {
  if (g_stateJson.empty()) RefreshState();
  Copy(buf, len, g_stateJson);
  return static_cast<int>(g_stateJson.size());
}

int IfaceTvSet(const char* argsJson, char* code, uint32_t codeLen, char* message, uint32_t messageLen) {
  auto refuse = [&](const char* c, const std::string& m) {
    Copy(code, codeLen, c);
    Copy(message, messageLen, m);
    return 0;
  };
  try {
    readyup::minijson::ParseError pe;
    const auto v = readyup::minijson::Parse(argsJson ? argsJson : "", &pe);
    if (!v || v->type != Value::Type::Object) return refuse("bad_args", "args must be an object");
    for (const auto& kv : v->obj) {
      if (kv.first != "scope" && kv.first != "settings" && kv.first != "casters") return refuse("bad_args", "unknown arg \"" + kv.first.substr(0, 32) + "\"");
    }
    const Value* scope = v->get("scope");
    if (!scope || scope->type != Value::Type::String || (scope->str != "server" && scope->str != "match")) {
      return refuse("bad_args", "scope must be \"server\" or \"match\"");
    }
    const bool match = scope->str == "match";
    const Value* settings = v->get("settings");
    const Value* casters = v->get("casters");
    if (settings && settings->type == Value::Type::Null) settings = nullptr;
    if (casters && casters->type == Value::Type::Null) casters = nullptr;
    if (!settings && !casters) return refuse("bad_args", "nothing to set (settings or casters)");
    Profile set;
    std::vector<Key> cleared;
    std::string err;
    if (settings && !bc::ParseProfile(*settings, &set, &err, &cleared)) return refuse("bad_args", "settings: " + err);
    std::set<uint64_t> ids;
    if (casters) {
      if (match) return refuse("bad_args", "casters are a server setting (scope \"server\")");
      if (casters->type != Value::Type::Array) return refuse("bad_args", "casters must be an array of SteamID64 strings");
      if (casters->arr.size() > 256) return refuse("bad_args", "casters: at most 256");
      for (const auto& c : casters->arr) {
        const uint64_t id = c.type == Value::Type::String ? ParseSteamId64(c.str) : 0;
        if (!id) return refuse("bad_args", "casters: not a SteamID64 string");
        ids.insert(id);
      }
    }
    if (match) {
      if (!g_tracker.active) return refuse("bad_phase", "no platform match is loaded on this server");
      Profile o = bc::Overlay(g_tracker.active->override_, set);
      for (Key k : cleared) o.Clear(k);
      if (o.delay_seconds && *o.delay_seconds < g_tracker.active->delayFloor) {
        return refuse("bad_args", "delay_seconds below the match's GOTV delay (" + std::to_string(g_tracker.active->delayFloor) + " s)");
      }
      const Profile merged = bc::Overlay(bc::Overlay(bc::Overlay(ServerProfile(), bc::ProfileFromMatchCvars(g_tracker.active->matchCvars)), g_tracker.active->profile), o);
      if (const std::string c = bc::ConflictWithMatchCvars(merged, g_tracker.active->delayFloor, g_tracker.active->matchCvars);
          !c.empty()) {
        return refuse("bad_args", "the match's own cvars set " + c + " (config.cvars): change the match instead");
      }
      g_tracker.active->override_ = o;
      PersistActive();
    } else {
      Settings next = g_settings;
      next.server = bc::Overlay(next.server, set);
      for (Key k : cleared) next.server.Clear(k);
      if (casters) {
        next.casters = ids;
        next.castersSet = true;
      }
      const Settings prev = g_settings;
      g_settings = next;
      if (!SaveSettings()) {
        g_settings = prev;
        return refuse("io", "settings.json could not be saved");
      }
      if (casters) PushGuests();
    }
    ru_logf(g_api, RU_LOG_INFO, "tv: tv.set (%s) %s%s", scope->str.c_str(), bc::ProfileJson(set).c_str(),
            casters ? (", " + std::to_string(ids.size()) + " caster(s)").c_str() : "");
    Apply(false);
    RefreshState();
    return 1;
  } catch (...) {
    return refuse("bad_args", "internal error");
  }
}

const ru_tv_v1 g_iface = {sizeof(ru_tv_v1), &IfaceStateRev, &IfaceStateJson, &IfaceTvSet};

void RunSelftest(ru_selftest_add_fn add, void* ctx) {
  int floor = 0;
  const Profile eff = Effective(&floor);
  std::string d = std::string(g_enabled ? "on" : "off (tv.cfg enabled=0)") + ", " +
                  (g_tracker.active ? "match " + g_tracker.active->matchId : std::string("server profile")) + " " +
                  bc::ProfileJson(eff) + ", " + std::to_string(Casters().size()) + " caster(s)";
  add(ctx, "INFO", "tv", d.c_str());
  if (!Casters().empty() && !Whitelist()) add(ctx, "WARN", "tv casters", "whitelist.so is not loaded: casters are not let in");
}
const ru_selftest_iface_v1 g_selftestIface = {sizeof(ru_selftest_iface_v1), &RunSelftest};

LaunchInfo ReadLaunch() {
  std::string cmd = ReadFile("/proc/self/cmdline");
  std::vector<std::string> args;
  size_t s = 0;
  for (size_t i = 0; i <= cmd.size(); ++i) {
    if (i == cmd.size() || cmd[i] == '\0') {
      if (i > s) args.push_back(cmd.substr(s, i - s));
      s = i + 1;
    }
  }
  return ParseLaunchArgs(args);
}

}  // namespace
}  // namespace tv

using namespace tv;

extern "C" {

READYUP_PLUGIN_EXPORT const ru_plugin_info* readyup_plugin_info(void) {
  static const ru_plugin_info info = {
      sizeof(ru_plugin_info),
      (1u << 16) | 11u,  // needs API 1.11 (cvar_query: the cvars' values before tv.so changes them)
      "tv",
      TV_VERSION,
      "Ready Up",
      "GOTV broadcast settings that hold for a whole series (delay, insecure observers, casters)",
  };
  return &info;
}

READYUP_PLUGIN_EXPORT int readyup_plugin_load(const ru_api* api, uint32_t core_api_version) {
  if (RU_API_VERSION_MAJOR(core_api_version) != 1 || !RU_API_HAS(api, cvar_query)) return 1;
  g_api = api;
  g_now = 0;
  g_applyAt.clear();
  g_baselineAsked.clear();
  g_mismatchLogged.clear();
  g_observers.clear();
  g_fleetInstance = 0;
  g_fleetHandlers.clear();
  g_lastMode.clear();
  g_tracker = MatchTracker{};
  g_observed = Observed{};
  g_passwordObserved.reset();
  g_lastHousekeeping = g_lastGuestPush = g_lastTvStatus = g_lastVerify = g_lastDriftReapply = -1e9;
  g_verifyAt = g_tvStatusUntil = -1;
  const char* dir = api->data_dir(api->self);
  g_dir = dir && *dir ? dir : "";
  g_launch = ReadLaunch();
  LoadConfig();
  bool ok = true;
  g_settings = ParseSettings(ReadFile(SettingsPath()), &ok);
  if (!ok) ru_logf(api, RU_LOG_WARN, "%s is not valid; using tv.cfg only", SettingsPath().c_str());
  // What this process already did to the engine (a `ru plugin reload tv`).
  g_applier = Applier{};
  {
    std::string buf(16384, '\0');
    const int n = api->stash_get(api->self, "applier", buf.data(), static_cast<uint32_t>(buf.size()));
    if (n > 0 && n <= static_cast<int>(buf.size())) (void)ParseApplier(buf.substr(0, static_cast<size_t>(n)), &g_applier);
  }
  // The match profile of a match that was loaded before a restart / reload: it waits for the match
  // plugin to report that match again.
  g_restore.reset();
  g_update.reset();
  g_driftCheckAt = -1;
  MatchProfile saved;
  if (ParseMatchProfile(ReadFile(ActivePath()), &saved)) g_restore = std::move(saved);

  if (!api->register_ru_subcommand(api->self, "tv", &OnRu, nullptr)) return 2;
  if (!api->register_chat_command(api->self, ".tv", &OnChatTv, nullptr)) {
    ru_logf(api, RU_LOG_WARN, "tv: .tv is taken by another plugin; use .ru tv");
  }
  for (const auto& n : bc::OwnedCvars()) {
    if (ru_handle h = api->register_console_command_ex(api->self, n.c_str(), RU_CMD_OBSERVE, &OnObserve, nullptr)) {
      g_observers.push_back(h);
    }
  }
  api->register_console_command_ex(api->self, "tv_password", RU_CMD_OBSERVE, &OnObserve, nullptr);
  api->on_frame(api->self, &OnFrame, nullptr);
  api->subscribe(api->self, RU_EVENT_MAP_START, &OnMapStart, nullptr);
  api->subscribe(api->self, RU_EVENT_MATCH_START, &OnMatchStart, nullptr);
  api->subscribe_log_line(api->self, &OnLogLine, nullptr);
  api->provide_interface(api->self, RU_TV_IFACE_NAME, RU_TV_IFACE_VERSION, const_cast<ru_tv_v1*>(&g_iface));
  api->provide_interface(api->self, RU_SELFTEST_IFACE_PREFIX "tv", RU_SELFTEST_IFACE_VERSION,
                         const_cast<ru_selftest_iface_v1*>(&g_selftestIface));
  ScheduleApply(0.0, true);
  RefreshState();
  ru_logf(api, RU_LOG_INFO, "loaded " TV_VERSION ": %s, server profile %s, %zu caster(s)%s", g_enabled ? "on" : "off (tv.cfg enabled=0)",
          bc::ProfileJson(ServerProfile()).c_str(), Casters().size(), g_launch.insecure ? "; this server runs -insecure" : "");
  return 0;
}

READYUP_PLUGIN_EXPORT void readyup_plugin_unload(void) {
  if (!g_api) return;
  try {
    // fleet.so cannot see us unload: drop our handlers. Same for whitelist guests.
    if (const ru_fleet_v1* f = Fleet(); f && f->instance_id && f->instance_id() == g_fleetInstance &&
                                       FleetHas<decltype(f->unregister_handler)>(f, offsetof(ru_fleet_v1, unregister_handler))) {
      for (uint64_t h : g_fleetHandlers) f->unregister_handler(h);
    }
    if (const ru_whitelist_v1* w = Whitelist(); w && RU_API_HAS(w, set_guests) && w->set_guests) w->set_guests("tv", nullptr, 0);
    // Insecure observers never outlive the plugin (`ru plugin disable tv`): tv_secure_bypass and
    // sv_deltaticks_enforce go back to their baselines. The rest stays (changing tv_delay mid-match
    // would disturb the broadcast); a reload sets everything again on its first frame.
    std::vector<std::pair<std::string, std::string>> keep;
    for (const auto& kv : g_applier.applied) {
      if (kv.first != "tv_secure_bypass" && kv.first != "sv_deltaticks_enforce") keep.push_back(kv);
    }
    const Plan p = PlanApply(g_applier, keep, false);
    for (const auto& c : p.commands) g_api->server_command(g_api->self, c.c_str());
    StashApplier();
  } catch (...) {
  }
  ru_logf(g_api, RU_LOG_INFO, "unloaded");
  g_api = nullptr;
}

}  // extern "C"

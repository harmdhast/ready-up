#include "readyup/fleet_state.h"

#include "readyup/broadcast_profile.h"
#include "readyup/map_names.h"
#include "readyup/ruleset.h"

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <cstdlib>
#include <cstring>

namespace readyup::fleetstate {

// ---------------------------------------------------------------------------- state stream

Json StripNulls(const Json& v) {
  if (v.type() == Json::Type::Object) {
    Json out = Json::Object();
    for (const auto& kv : v.Members()) {
      if (kv.second.IsNull()) continue;
      out[kv.first] = StripNulls(kv.second);
    }
    return out;
  }
  if (v.type() == Json::Type::Array) {
    Json out = Json::Array();
    for (const auto& it : v.Items()) out.Push(StripNulls(it));
    return out;
  }
  return v;
}

void LiveStream::Reset(const Json& state, long long rev) {
  rev_ = rev;
  if (state.IsNull()) {
    state_ = Json();
    return;
  }
  state_ = StripNulls(state);
  state_["live_rev"] = rev_;
}

bool LiveStream::Advance(const Json& next, bool force, Json* patch, long long* rev) {
  Json n = StripNulls(next);
  if (n.IsObject()) n["live_rev"] = rev_;  // compare without a live_rev change
  Json diff;
  const bool changed = MergeDiff(state_, n, &diff);
  if (!changed && !force) return false;
  ++rev_;
  if (n.IsObject()) n["live_rev"] = rev_;
  Json p;
  if (!MergeDiff(state_, n, &p)) p = Json::Object();
  if (!p.IsObject()) {
    // Whole-state replacement (null -> object or object -> null): send it as is; a platform
    // applying RFC 7386 to an object target with a non-object patch replaces the target.
    p = n;
  }
  state_ = std::move(n);
  if (patch) *patch = std::move(p);
  if (rev) *rev = rev_;
  return true;
}

bool LiveStream::BumpRevOnly(Json* patch, long long* rev) {
  if (!state_.IsObject()) return false;
  ++rev_;
  state_["live_rev"] = rev_;
  if (patch) {
    *patch = Json::Object();
    (*patch)["live_rev"] = rev_;
  }
  if (rev) *rev = rev_;
  return true;
}

// ---------------------------------------------------------------------------- fencing

const char* VerdictCode(Verdict v) {
  switch (v) {
    case Verdict::Ok: return "ok";
    case Verdict::Duplicate: return "duplicate";
    case Verdict::StaleEpoch: return "stale_epoch";
    case Verdict::Busy: return "busy";
    case Verdict::NotAssigned: return "not_assigned";
  }
  return "rejected";
}

Verdict Fence::CheckAssign(const Assignment& cur, const std::string& matchId, long long epoch,
                           bool localMatchActive) const {
  if (epoch < 1 || matchId.empty()) return Verdict::StaleEpoch;
  if (epoch < Retired(matchId)) return Verdict::StaleEpoch;
  if (cur.active) {
    if (cur.match_id == matchId) {
      if (epoch == cur.epoch) return Verdict::Duplicate;
      if (epoch < cur.epoch) return Verdict::StaleEpoch;
      return Verdict::Ok;  // re-assignment of the same match with a newer epoch (restore / move back)
    }
    return Verdict::Busy;
  }
  if (epoch == Retired(matchId)) return Verdict::StaleEpoch;  // that assignment already ended here
  if (localMatchActive) return Verdict::Busy;
  return Verdict::Ok;
}

Verdict Fence::CheckScoped(const Assignment& cur, const std::string& matchId, long long epoch) const {
  if (!cur.active || cur.match_id != matchId) {
    if (!matchId.empty() && epoch > 0 && epoch <= Retired(matchId)) return Verdict::StaleEpoch;
    return Verdict::NotAssigned;
  }
  if (epoch < cur.epoch) return Verdict::StaleEpoch;
  if (epoch > cur.epoch) return Verdict::NotAssigned;  // an assignment this server never got
  return Verdict::Ok;
}

void Fence::Retire(const std::string& matchId, long long epoch) {
  if (matchId.empty()) return;
  auto it = retired_.find(matchId);
  if (it != retired_.end()) {
    it->second = std::max(it->second, epoch);
    return;
  }
  retired_[matchId] = epoch;
  order_.push_back(matchId);
  while (order_.size() > 64) {
    retired_.erase(order_.front());
    order_.erase(order_.begin());
  }
}

long long Fence::Retired(const std::string& matchId) const {
  const auto it = retired_.find(matchId);
  return it == retired_.end() ? 0 : it->second;
}

Json Fence::ToJson() const {
  Json a = Json::Array();
  for (const auto& id : order_) {
    Json e = Json::Object();
    e["match_id"] = id;
    e["epoch"] = retired_.at(id);
    a.Push(std::move(e));
  }
  return a;
}

void Fence::FromJson(const Json& j) {
  retired_.clear();
  order_.clear();
  for (const auto& e : j.Items()) {
    const Json* id = e.Find("match_id");
    const Json* ep = e.Find("epoch");
    if (id && ep) Retire(id->AsString(), ep->AsInt());
  }
}

// ---------------------------------------------------------------------------- assign / update

namespace {

bool IsDigits(const std::string& s, size_t maxLen) {
  if (s.empty() || s.size() > maxLen) return false;
  for (unsigned char c : s) {
    if (!std::isdigit(c)) return false;
  }
  return true;
}

const Json* Obj(const Json& o, const char* key) {
  const Json* v = o.Find(key);
  return v && v->type() == Json::Type::Object ? v : nullptr;
}
const Json* Arr(const Json& o, const char* key) {
  const Json* v = o.Find(key);
  return v && v->type() == Json::Type::Array ? v : nullptr;
}
std::string Str(const Json& o, const char* key, const std::string& def = {}) {
  const Json* v = o.Find(key);
  return v && v->type() == Json::Type::String ? v->AsString() : def;
}
bool IsInt(const Json* v) { return v && v->type() == Json::Type::Int; }
long long Int(const Json& o, const char* key, long long def) {
  const Json* v = o.Find(key);
  if (!v) return def;
  if (v->type() == Json::Type::Int || v->type() == Json::Type::Double) return v->AsInt();
  return def;
}
bool Bool(const Json& o, const char* key, bool def) {
  const Json* v = o.Find(key);
  return v && v->type() == Json::Type::Bool ? v->AsBool() : def;
}

bool ValidSteam(const Json& v) { return v.type() == Json::Type::String && IsDigits(v.AsString(), 20); }

bool ValidTeam(const Json* t, const char* which, std::string* err) {
  auto fail = [&](const std::string& why) {
    if (err) *err = std::string(which) + ": " + why;
    return false;
  };
  if (!t) return fail("missing");
  const std::string name = Str(*t, "name");
  if (name.empty() || name.size() > 64) return fail("name must be 1..64 bytes");
  const Json* players = Arr(*t, "players");
  if (!players) return fail("players must be an array");
  if (players->Items().size() > 32) return fail("more than 32 players");
  for (const auto& p : players->Items()) {
    const Json* sid = p.Find("steamid64");
    if (!sid || !ValidSteam(*sid)) return fail("player without a valid steamid64");
    const std::string role = Str(p, "role", "player");
    if (role != "player" && role != "sub" && role != "coach") return fail("bad role " + role);
  }
  if (const Json* cap = t->Find("captain"); cap && !ValidSteam(*cap)) return fail("bad captain");
  return true;
}

// Removes a SteamID64 from every roster list of an assign config. True if it was there.
bool RemoveEverywhere(Json* config, const std::string& sid) {
  bool found = false;
  for (const char* team : {"team1", "team2"}) {
    Json* t = const_cast<Json*>(Obj(*config, team));
    if (!t) continue;
    const Json* players = Arr(*t, "players");
    if (!players) continue;
    Json kept = Json::Array();
    for (const auto& p : players->Items()) {
      const Json* s = p.Find("steamid64");
      if (s && s->AsString() == sid) {
        found = true;
        continue;
      }
      kept.Push(p);
    }
    (*t)["players"] = std::move(kept);
  }
  if (const Json* specs = Arr(*config, "spectators")) {
    Json kept = Json::Array();
    for (const auto& s : specs->Items()) {
      if (s.AsString() == sid) {
        found = true;
        continue;
      }
      kept.Push(s);
    }
    (*config)["spectators"] = std::move(kept);
  }
  return found;
}

// rules.ruleset / rules.overrides (ruleset.h), and what the ruleset refuses (knife sides under valve).
bool ValidateRuleset(const Json& cfg, std::string* err) {
  auto fail = [&](const std::string& why) {
    if (err) *err = why;
    return false;
  };
  Ruleset rs = ServerRuleset();
  RuleMap overrides;
  if (const Json* rules = Obj(cfg, "rules")) {
    if (const Json* r = rules->Find("ruleset")) {
      if (r->type() != Json::Type::String || !ParseRuleset(r->AsString(), &rs)) {
        return fail("rules.ruleset must be \"default\" or \"valve\"");
      }
    }
    if (const Json* o = rules->Find("overrides")) {
      std::string e;
      if (!ParseOverrides(*o, &overrides, &e)) return fail("rules.overrides: " + e);
    }
  }
  RulesInput in;
  in.ruleset = rs;
  in.overrides = std::move(overrides);
  std::vector<std::string> sides;
  if (const Json* maps = Arr(cfg, "maps")) {
    for (const auto& m : maps->Items()) sides.push_back(Str(m, "sides", "knife"));
  }
  std::string why;
  if (!CheckMapSides(ResolveEffective(in), sides, &why)) return fail(why);
  return true;
}

}  // namespace

bool ValidateAssign(const Json& payload, std::string* err) {
  auto fail = [&](const std::string& why) {
    if (err) *err = why;
    return false;
  };
  const std::string mid = Str(payload, "match_id");
  if (mid.empty() || mid.size() > 64) return fail("match_id must be 1..64 bytes");
  for (unsigned char c : mid) {
    if (!(std::isalnum(c) || c == '_' || c == '-' || c == '.' || c == ':')) return fail("match_id has a bad character");
  }
  if (!IsInt(payload.Find("epoch")) || payload.Find("epoch")->AsInt() < 1) return fail("epoch must be an integer >= 1");
  const Json* cfg = Obj(payload, "config");
  if (!cfg) return fail("config must be an object");
  if (const Json* id = cfg->Find("id"); id && (!IsInt(id) || id->AsInt() < 1 || id->AsInt() >= (1ll << 52)))
    return fail("id must be an integer 1..2^52-1");
  const long long numMaps = Int(*cfg, "num_maps", 0);
  if (numMaps < 1 || numMaps > 9) return fail("num_maps must be 1..9");
  const Json* maps = Arr(*cfg, "maps");
  if (!maps || maps->Items().empty() || maps->Items().size() > 9) return fail("maps must list 1..9 maps");
  if (static_cast<long long>(maps->Items().size()) < numMaps) return fail("fewer maps than num_maps");
  for (const auto& m : maps->Items()) {
    const std::string name = Str(m, "name");
    const std::string ws = Str(m, "workshop_id");
    if (!SafeMapName(name)) return fail("bad map name \"" + name + "\"");
    if (!ws.empty() && !SafeWorkshopId(ws)) return fail("bad workshop_id");
    const std::string sides = Str(m, "sides", "knife");
    if (sides != "team1_ct" && sides != "team2_ct" && sides != "knife") return fail("bad sides \"" + sides + "\"");
  }
  if (!ValidTeam(Obj(*cfg, "team1"), "team1", err)) return false;
  if (!ValidTeam(Obj(*cfg, "team2"), "team2", err)) return false;
  for (const char* list : {"spectators", "admins"}) {
    if (const Json* a = cfg->Find(list)) {
      if (a->type() != Json::Type::Array) return fail(std::string(list) + " must be an array");
      for (const auto& s : a->Items()) {
        if (!ValidSteam(s)) return fail(std::string(list) + ": bad steamid64");
      }
    }
  }
  const Json* pw = cfg->Find("password");
  if (!pw || pw->type() != Json::Type::String) return fail("password must be a string");
  if (!ValidPassword(pw->AsString())) return fail("password has characters that cannot go into sv_password");
  if (const Json* r = cfg->Find("rules"); r && r->type() != Json::Type::Object) return fail("rules must be an object");
  if (const Json* c = cfg->Find("cvars"); c && c->type() != Json::Type::Object) return fail("cvars must be an object");
  if (const Json* rules = Obj(*cfg, "rules")) {
    if (const Json* fill = rules->Find("bot_fill")) {
      if (fill->type() != Json::Type::Object || !IsInt(fill->Find("players_per_team")) ||
          Int(*fill, "players_per_team", 0) < 1 || Int(*fill, "players_per_team", 0) > 32)
        return fail("bot_fill.players_per_team must be an integer 1..32");
    }
  }
  if (!ValidateRuleset(*cfg, err)) return false;
  // rules.broadcast (the tv plugin's GOTV profile, docs/design/tv-plugin.md): the same validator
  // tv.so uses, so a bad block refuses the whole assign instead of being half applied.
  if (std::string why; !broadcast::ValidateAssignConfigText(cfg->Dump(), &why)) return fail(why);
  return true;
}

unsigned long long NumericMatchId(const std::string& matchId) {
  if (IsDigits(matchId, 15)) {
    const unsigned long long v = std::strtoull(matchId.c_str(), nullptr, 10);
    if (v != 0) return v;
  }
  uint64_t h = 1469598103934665603ull;
  for (unsigned char c : matchId) {
    h ^= c;
    h *= 1099511628211ull;
  }
  h &= (1ull << 52) - 1;  // exact in a double (the MAT parser reads numbers as doubles)
  return h ? h : 1;
}

unsigned long long AssignMatchId(const std::string& matchId, const Json& config) {
  const long long id = Int(config, "id", 0);
  return id > 0 ? static_cast<unsigned long long>(id) : NumericMatchId(matchId);
}

Json AssignToMatConfig(const std::string& matchId, const Json& config, std::vector<std::string>* dropped) {
  Json cfg = Json::Object();
  cfg["matchid"] = static_cast<long long>(AssignMatchId(matchId, config));
  cfg["num_maps"] = Int(config, "num_maps", 1);
  Json maplist = Json::Array();
  Json sides = Json::Array();
  if (const Json* maps = Arr(config, "maps")) {
    for (const auto& m : maps->Items()) {
      // Workshop maps: "workshop/<id>/<name>" (host_workshop_map; map_names.h).
      maplist.Push(mapnames::MakeEntry(Str(m, "name"), Str(m, "workshop_id")));
      sides.Push(Str(m, "sides", "knife"));
    }
  }
  cfg["maplist"] = std::move(maplist);
  cfg["map_sides"] = std::move(sides);

  Json spectators = Json::Object();
  Json specPlayers = Json::Object();
  Json coaches = Json::Array();
  // Starting players per team (role "player"); set when a team has substitutes (role "sub") so
  // the ready gate waits for a full team, not the whole roster (match_rules.h).
  long long starters = 0;
  bool subs = false;
  auto team = [&](const char* key) {
    Json t = Json::Object();
    const Json* src = Obj(config, key);
    if (!src) return t;
    t["name"] = Str(*src, "name");
    if (!Str(*src, "tag").empty()) t["tag"] = Str(*src, "tag");
    if (!Str(*src, "flag").empty()) t["flag"] = Str(*src, "flag");
    Json players = Json::Object();
    Json teamCoaches = Json::Object();
    Json teamSubs = Json::Object();
    long long teamStarters = 0;
    if (const Json* ps = Arr(*src, "players")) {
      for (const auto& p : ps->Items()) {
        const std::string sid = Str(p, "steamid64");
        const std::string role = Str(p, "role", "player");
        // Coaches are not ready-gated players: whitelisted spectators who coach this team
        // (coach.h), listed in the team's `coaches` (get5 shape) and the flat `coaches`.
        if (role == "coach") {
          specPlayers[sid] = Str(p, "name");
          coaches.Push(sid);
          teamCoaches[sid] = Str(p, "name");
        }
        else players[sid] = Str(p, "name");
        if (role == "sub") {
          subs = true;
          teamSubs[sid] = Str(p, "name");
        } else if (role != "coach") {
          ++teamStarters;
        }
      }
    }
    starters = std::max(starters, teamStarters);
    t["players"] = std::move(players);
    if (!teamCoaches.Members().empty()) t["coaches"] = std::move(teamCoaches);
    // Substitutes stay in `players` (whitelisted, may stand in) and are named here, so the ready
    // count and the live state know them (match_config_parser.cpp `substitutes`).
    if (!teamSubs.Members().empty()) t["substitutes"] = std::move(teamSubs);
    if (!Str(*src, "captain").empty()) t["captain_steamid64"] = Str(*src, "captain");
    return t;
  };
  cfg["team1"] = team("team1");
  cfg["team2"] = team("team2");
  if (subs && starters > 0) cfg["players_per_team"] = starters;
  if (const Json* specs = Arr(config, "spectators")) {
    for (const auto& s : specs->Items()) {
      if (!specPlayers.Find(s.AsString())) specPlayers[s.AsString()] = "";
    }
  }
  spectators["players"] = std::move(specPlayers);
  cfg["spectators"] = std::move(spectators);
  cfg["coaches"] = std::move(coaches);
  Json admins = Json::Array();
  if (const Json* a = Arr(config, "admins")) {
    for (const auto& s : a->Items()) admins.Push(s.AsString());
  }
  cfg["admins"] = std::move(admins);

  // rules (§7.1) -> the MAT metadata fields.
  const Json empty = Json::Object();
  const Json* rules = Obj(config, "rules");
  const Json& r = rules ? *rules : empty;
  const long long maxRounds = Int(r, "max_rounds", 24);
  cfg["maxRounds"] = maxRounds;
  const Json* ot = Obj(r, "overtime");
  const bool otOn = ot ? Bool(*ot, "enabled", true) : true;
  const long long otHalf = ot ? Int(*ot, "rounds_per_half", 3) : 3;
  cfg["overtimeMode"] = otOn ? "enabled" : "disabled";
  cfg["overtimeSegments"] = otHalf;
  if (ot && Int(*ot, "max_overtimes", -1) >= 0) cfg["maxOvertimes"] = Int(*ot, "max_overtimes", -1);
  if (const Json* tb = Obj(r, "tiebreak")) {
    cfg["damageTiebreak"] = Bool(*tb, "damage", false);
    cfg["damageTiebreakSuddenDeath"] = Bool(*tb, "sudden_death_on_tie", true);
  }
  if (const Json* k = Obj(r, "knife")) cfg["knifeDecisionSeconds"] = Int(*k, "side_pick_seconds", 60);
  cfg["clinch_series"] = Bool(r, "clinch_series", true);
  // Wingman (wingman.h) and simulation (simulation.h): rules.wingman, rules.simulation.timescale.
  if (Bool(r, "wingman", false)) cfg["wingman"] = true;
  // GOTV demo of this match (demo_recorder.h): rules.demo.record / .upload win over the server's
  // ru_demo_recording_enabled and upload target; absent = the server's settings.
  if (const Json* d = Obj(r, "demo")) {
    if (const Json* v = d->Find("record"); v && v->type() == Json::Type::Bool) cfg["demo_record"] = v->AsBool();
    if (const Json* v = d->Find("upload"); v && v->type() == Json::Type::Bool) cfg["demo_upload"] = v->AsBool();
  }
  if (const Json* sm = Obj(r, "simulation")) {
    cfg["simulation"] = true;
    if (const Json* ts = sm->Find("timescale"); ts && (ts->type() == Json::Type::Double || ts->type() == Json::Type::Int)) {
      cfg["simulation_timescale"] = ts->type() == Json::Type::Int ? static_cast<double>(ts->AsInt()) : ts->AsDouble();
    }
  }
  if (const Json* fill = Obj(r, "bot_fill")) {
    cfg["bot_fill"] = true;
    cfg["players_per_team"] = Int(*fill, "players_per_team", 5);
    cfg["simulation"] = false;
  }
  // Ruleset + overrides (ruleset.h; validated in ValidateAssign).
  if (const Json* rs = r.Find("ruleset")) cfg["ruleset"] = *rs;
  if (const Json* ov = r.Find("overrides")) cfg["overrides"] = *ov;
  // Pause / ready / forfeit rules (match_rules.h). Absent fields stay unset (server defaults).
  const Json* pause = Obj(r, "pause");
  if (pause && pause->Find("technical_per_team")) cfg["max_tech_pauses_per_team"] = Int(*pause, "technical_per_team", 0);
  if (pause && pause->Find("technical_seconds")) cfg["tech_pause_max_seconds"] = Int(*pause, "technical_seconds", 0);
  if (pause && !Str(*pause, "unpause").empty()) cfg["both_teams_unpause_required"] = Str(*pause, "unpause") != "caller_team";
  if (pause && pause->Find("pause_after_restore")) cfg["pause_after_restore"] = Bool(*pause, "pause_after_restore", true);
  if (const Json* ready = Obj(r, "ready")) {
    if (ready->Find("allow_force_ready")) cfg["allow_force_ready"] = Bool(*ready, "allow_force_ready", true);
    if (ready->Find("team_ready")) cfg["team_ready"] = Bool(*ready, "team_ready", false);
    if (ready->Find("min_per_team")) cfg["min_players_to_ready"] = Int(*ready, "min_per_team", 0);
    if (ready->Find("autoready")) cfg["autoready"] = Bool(*ready, "autoready", false);
  }
  if (const Json* st = Obj(r, "stop")) {
    if (st->Find("enabled")) cfg["stop_command_available"] = Bool(*st, "enabled", false);
    if (st->Find("no_damage")) cfg["stop_command_no_damage"] = Bool(*st, "no_damage", false);
  }
  // Server settings the match sets for itself (server_settings.h); absent = the server's value.
  if (r.Find("playout")) cfg["playout"] = Bool(r, "playout", false);
  if (r.Find("whitelist")) cfg["whitelist"] = Bool(r, "whitelist", true);
  // In-game names from the roster (roster_names.h); absent = on when players have names.
  if (r.Find("roster_names")) cfg["roster_names"] = Bool(r, "roster_names", true);
  if (const Json* ff = Obj(r, "forfeit"); ff && ff->Find("team_absent_seconds")) {
    cfg["forfeit_after_seconds"] = Int(*ff, "team_absent_seconds", 240);
  }
  if (const Json* ff = Obj(r, "forfeit")) {
    if (const Json* gg = Obj(*ff, "gg_vote")) {
      if (gg->Find("enabled")) cfg["gg_enabled"] = Bool(*gg, "enabled", false);
      if (const Json* t = gg->Find("threshold"); t && (t->type() == Json::Type::Double || t->type() == Json::Type::Int)) {
        cfg["gg_threshold"] = t->type() == Json::Type::Int ? static_cast<double>(t->AsInt()) : t->AsDouble();
      }
      if (gg->Find("min_score_diff")) cfg["gg_min_score_diff"] = Int(*gg, "min_score_diff", 8);
    }
  }

  Json cvars = Json::Object();
  cvars["mp_maxrounds"] = std::to_string(maxRounds);
  cvars["mp_overtime_enable"] = otOn ? "1" : "0";
  cvars["mp_overtime_maxrounds"] = std::to_string(otHalf * 2);
  // Tactical timeouts are CS2's own: their count and length are engine cvars.
  if (pause && pause->Find("tactical_per_team")) cvars["mp_team_timeout_max"] = std::to_string(Int(*pause, "tactical_per_team", 3));
  if (pause && pause->Find("tactical_seconds")) cvars["mp_team_timeout_time"] = std::to_string(Int(*pause, "tactical_seconds", 30));
  if (const Json* c = Obj(config, "cvars")) {
    for (const auto& kv : c->Members()) {
      const std::string& k = kv.first;
      const bool engine = k.rfind("mp_", 0) == 0 || k.rfind("sv_", 0) == 0 || k.rfind("tv_", 0) == 0 ||
                          k.rfind("bot_", 0) == 0;
      bool safeName = !k.empty() && k.size() <= 64;
      for (unsigned char ch : k) {
        if (!(std::islower(ch) || std::isdigit(ch) || ch == '_')) safeName = false;
      }
      std::string v;
      if (kv.second.type() == Json::Type::String) v = kv.second.AsString();
      else if (kv.second.type() == Json::Type::Int) v = std::to_string(kv.second.AsInt());
      else if (kv.second.type() == Json::Type::Double) v = kv.second.Dump();
      else if (kv.second.type() == Json::Type::Bool) v = kv.second.AsBool() ? "1" : "0";
      bool safeValue = v.size() <= 128;
      for (unsigned char ch : v) {
        if (ch < 0x20 || ch == ';' || ch == '"') safeValue = false;
      }
      if (!engine || !safeName || !safeValue || k == "sv_password" || k == "rcon_password" || k == "sv_cheats") {
        if (dropped) dropped->push_back(k);
        continue;
      }
      cvars[k] = v;
    }
  }
  cfg["cvars"] = std::move(cvars);

  Json wrapper = Json::Object();
  wrapper["slug"] = matchId;
  wrapper["config"] = std::move(cfg);
  return wrapper;
}

// ---------------------------------------------------------------------------- server.config

ServerConfigPlan PlanServerConfig(const Json& st) {
  ServerConfigPlan plan;
  // A JSON scalar as the text a setting takes (true -> "1"); false for another type.
  auto text = [](const Json& v, std::string* out) {
    switch (v.type()) {
      case Json::Type::Bool: *out = v.AsBool() ? "1" : "0"; return true;
      case Json::Type::Int: *out = std::to_string(v.AsInt()); return true;
      case Json::Type::String: *out = v.AsString(); return true;
      default: return false;
    }
  };
  auto add = [&](std::vector<std::pair<std::string, std::string>>* to, const std::string& name, const Json* v,
                 const char* field) {
    if (!v || v->IsNull()) return;
    std::string t;
    if (!text(*v, &t)) {
      plan.skipped.push_back(field);
      return;
    }
    to->emplace_back(name, t);
  };
  if (!st.IsObject()) return plan;
  add(&plan.settings, "hostname_format", st.Find("hostname_format"), "hostname_format");
  add(&plan.settings, "knife_enabled_default", st.Find("scrim_knife"), "scrim_knife");
  add(&plan.settings, "scrim_when_idle", st.Find("scrim_when_idle"), "scrim_when_idle");
  add(&plan.settings, "chat_prefix", st.Find("chat_prefix"), "chat_prefix");
  add(&plan.settings, "admin_chat_prefix", st.Find("admin_chat_prefix"), "admin_chat_prefix");
  if (const Json* k = Obj(st, "series_end_kick_delay")) {
    add(&plan.settings, "series_end_kick_delay_no_demo", k->Find("no_demo"), "series_end_kick_delay.no_demo");
    add(&plan.settings, "series_end_kick_delay_demo_no_upload", k->Find("demo_no_upload"),
        "series_end_kick_delay.demo_no_upload");
    add(&plan.settings, "series_end_kick_delay_demo_upload", k->Find("demo_upload"), "series_end_kick_delay.demo_upload");
  }
  if (const Json* w = Obj(st, "warmup")) {
    add(&plan.console, "ru_warmup_message_html", w->Find("message_html"), "warmup.message_html");
    add(&plan.console, "ru_warmup_respawn", w->Find("respawn"), "warmup.respawn");
    add(&plan.console, "ru_warmup_startmoney", w->Find("money"), "warmup.money");
  }
  if (const Json* d = Obj(st, "demo")) {
    add(&plan.console, "ru_demo_path", d->Find("path"), "demo.path");
    add(&plan.console, "ru_demo_name_format", d->Find("name_format"), "demo.name_format");
  }
  return plan;
}

// ---------------------------------------------------------------------------- failover resume

namespace {

bool IsSha256Hex(const std::string& s) {
  if (s.size() != 64) return false;
  for (unsigned char c : s) {
    if (!(std::isdigit(c) || (c >= 'a' && c <= 'f'))) return false;
  }
  return true;
}

// {team1, team2} with non-negative integers; false when malformed.
bool ScorePair(const Json* o, int* t1, int* t2) {
  if (!o || !o->IsObject()) return false;
  const Json* a = o->Find("team1");
  const Json* b = o->Find("team2");
  if (!IsInt(a) || !IsInt(b) || a->AsInt() < 0 || b->AsInt() < 0) return false;
  *t1 = static_cast<int>(a->AsInt());
  *t2 = static_cast<int>(b->AsInt());
  return true;
}

const Json* PathObj(const Json* o, std::initializer_list<const char*> keys) {
  for (const char* k : keys) {
    if (!o || !o->IsObject()) return nullptr;
    o = o->Find(k);
  }
  return o && o->IsObject() ? o : nullptr;
}

}  // namespace

bool ParseResume(const Json& resume, const Json& config, long long epoch, ResumePlan* out, std::string* code,
                 std::string* err) {
  auto fail = [&](const char* c, const std::string& why) {
    if (code) *code = c;
    if (err) *err = "resume: " + why;
    return false;
  };
  if (!resume.IsObject()) return fail("invalid_config", "must be an object");
  ResumePlan p;
  p.present = true;
  if (const Json* fe = resume.Find("from_epoch")) {
    if (!IsInt(fe) || fe->AsInt() < 1) return fail("invalid_config", "from_epoch must be an integer >= 1");
    p.from_epoch = fe->AsInt();
    if (epoch > 0 && p.from_epoch >= epoch) return fail("invalid_config", "from_epoch must be lower than epoch");
  }
  const Json* maps = Arr(config, "maps");
  const long long mapCount =
      std::min<long long>(Int(config, "num_maps", 1), maps ? static_cast<long long>(maps->Items().size()) : 0);
  const Json* mn = resume.Find("map_number");
  if (!IsInt(mn) || mn->AsInt() < 1 || mn->AsInt() > mapCount) {
    return fail("invalid_config", "map_number must be 1..num_maps");
  }
  p.map_number = static_cast<int>(mn->AsInt());
  const Json* state = Obj(resume, "state");
  if (resume.Find("state") && !state) return fail("invalid_config", "state must be a MatchState object");

  const Json* backup = Obj(resume, "backup");
  const Json* ref = Obj(resume, "backup_ref");
  if (resume.Find("backup") && !backup) return fail("invalid_config", "backup must be an InlineBackup object");
  if (resume.Find("backup_ref") && !ref) return fail("invalid_config", "backup_ref must be an object");
  if (backup && ref) return fail("invalid_config", "backup and backup_ref exclude each other");
  long long round = -1;
  if (const Json* r = resume.Find("round")) {
    if (!IsInt(r) || r->AsInt() < 0 || r->AsInt() > 999) return fail("invalid_config", "round must be 0..999");
    round = r->AsInt();
  }
  if (backup) {
    if (Int(*backup, "parts", 1) > 1) {
      return fail("unsupported", "multi-part inline backups are not accepted; send the whole file as one part");
    }
    p.file = Str(*backup, "file");
    if (!SafeBackupFileName(p.file)) return fail("invalid_config", "backup.file is not a plain *.txt name");
    if (Str(*backup, "encoding", "base64") != "base64" || !Base64Decode(Str(*backup, "data"), &p.raw) ||
        p.raw.empty()) {
      return fail("invalid_config", "backup.data is not base64");
    }
    p.sha256 = Sha256Hex(p.raw);
    if (p.sha256 != Str(*backup, "sha256")) return fail("checksum", "backup sha256 mismatch");
    if (Int(*backup, "size", static_cast<long long>(p.raw.size())) != static_cast<long long>(p.raw.size())) {
      return fail("checksum", "backup size mismatch");
    }
    if (Int(*backup, "map_number", p.map_number) != p.map_number) {
      return fail("invalid_config", "backup.map_number is not map_number");
    }
    const long long br = Int(*backup, "round", 0);
    if (br < 1) return fail("invalid_config", "backup.round must be >= 1");
    if (round >= 0 && round != br) return fail("invalid_config", "round is not backup.round");
    round = br;
    p.inline_backup = true;
    int t1 = 0, t2 = 0;
    if (ScorePair(backup->Find("score"), &t1, &t2)) {
      p.score_team1 = t1;
      p.score_team2 = t2;
    }
  } else if (ref) {
    p.file = Str(*ref, "file");
    if (!p.file.empty() && !SafeBackupFileName(p.file)) {
      return fail("invalid_config", "backup_ref.file is not a plain *.txt name");
    }
    p.sha256 = Str(*ref, "sha256");
    if (!p.sha256.empty() && !IsSha256Hex(p.sha256)) return fail("invalid_config", "backup_ref.sha256 is not a sha256");
    if (round < 1) return fail("invalid_config", "backup_ref needs round >= 1");
  }
  p.round = static_cast<int>(std::max<long long>(0, round));

  // Series state: maps won and the results of the maps before map_number.
  const Json* sscore = resume.Find("series_score");
  if (!sscore && state) sscore = PathObj(state, {"series", "score"});
  if (sscore && !ScorePair(sscore, &p.series_team1, &p.series_team2)) {
    return fail("invalid_config", "series_score must be {team1, team2} (integers >= 0)");
  }
  const Json* done = Obj(resume, "maps");
  const bool fromState = !done;
  if (!done && state) done = PathObj(state, {"series", "maps"});
  if (done) {
    for (const auto& kv : done->Members()) {
      const int n = std::atoi(kv.first.c_str());
      if (n < 1 || n >= p.map_number || !kv.second.IsObject()) continue;
      if (fromState && Str(kv.second, "status") != "done") continue;
      ResumeMapResult r;
      r.map_number = n;
      if (!ScorePair(kv.second.Find("score"), &r.team1, &r.team2)) r.team1 = r.team2 = 0;
      r.winner = Str(kv.second, "winner", r.team1 > r.team2 ? "team1" : r.team2 > r.team1 ? "team2" : "none");
      if (r.winner != "team1" && r.winner != "team2" && r.winner != "none") {
        return fail("invalid_config", "bad winner for map " + kv.first);
      }
      p.maps_done.push_back(r);
    }
  }

  // Starting sides of the resumed map: given, from the platform's state, or the config's.
  p.sides = Str(resume, "sides");
  if (p.sides.empty() && state) {
    const std::string key = std::to_string(p.map_number);
    if (const Json* m = PathObj(state, {"series", "maps", key.c_str()})) p.sides = Str(*m, "sides");
  }
  if (p.sides == "knife") p.sides.clear();
  if (!p.sides.empty() && p.sides != "team1_ct" && p.sides != "team2_ct") {
    return fail("invalid_config", "sides must be team1_ct or team2_ct");
  }
  const std::string cfgSides =
      maps ? Str(maps->Items()[static_cast<size_t>(p.map_number - 1)], "sides", "knife") : std::string("knife");
  if (p.round >= 1 && p.sides.empty() && cfgSides == "knife") {
    return fail("invalid_config", "the resumed map's sides are still \"knife\": send the sides the knife round decided");
  }

  // Map score at the start of the round, team1's side then, the platform's stats.
  if (const Json* sc = resume.Find("score")) {
    if (!ScorePair(sc, &p.score_team1, &p.score_team2)) return fail("invalid_config", "score must be {team1, team2}");
  } else if (p.score_team1 < 0 && state) {
    const Json* a = PathObj(state, {"teams", "team1"});
    const Json* b = PathObj(state, {"teams", "team2"});
    if (a && b && IsInt(a->Find("score")) && IsInt(b->Find("score"))) {
      p.score_team1 = static_cast<int>(a->Find("score")->AsInt());
      p.score_team2 = static_cast<int>(b->Find("score")->AsInt());
    }
  }
  if (const Json* t1 = PathObj(state, {"teams", "team1"})) {
    const std::string side = Str(*t1, "side");
    if (side == "ct" || side == "t") p.team1_side = side;
  }
  std::string roundSides = Str(resume, "round_sides");
  if (roundSides.empty() && backup) roundSides = Str(*backup, "round_sides");
  if (!roundSides.empty()) {
    if (roundSides != "team1_ct" && roundSides != "team2_ct") {
      return fail("invalid_config", "round_sides must be team1_ct or team2_ct");
    }
    p.team1_side = roundSides == "team1_ct" ? "ct" : "t";
  }
  if (const Json* ms = resume.Find("map_stats")) {
    if (!ms->IsObject()) return fail("invalid_config", "map_stats must be a MapStats object");
    p.map_stats = *ms;
  }
  if (const Json* pz = PathObj(&config, {"rules", "pause"})) p.pause_after_restore = Bool(*pz, "pause_after_restore", true);
  if (out) *out = std::move(p);
  return true;
}

Json ResumeToJson(const ResumePlan& p) {
  Json j = Json::Object();
  j["present"] = p.present;
  j["from_epoch"] = p.from_epoch;
  j["map_number"] = p.map_number;
  j["round"] = p.round;
  j["inline_backup"] = p.inline_backup;
  j["file"] = p.file;
  j["sha256"] = p.sha256;
  j["series_team1"] = p.series_team1;
  j["series_team2"] = p.series_team2;
  Json done = Json::Array();
  for (const auto& m : p.maps_done) {
    Json r = Json::Object();
    r["map_number"] = m.map_number;
    r["team1"] = m.team1;
    r["team2"] = m.team2;
    r["winner"] = m.winner;
    done.Push(std::move(r));
  }
  j["maps_done"] = std::move(done);
  j["sides"] = p.sides;
  j["score_team1"] = p.score_team1;
  j["score_team2"] = p.score_team2;
  j["team1_side"] = p.team1_side;
  j["map_stats"] = p.map_stats;
  j["pause_after_restore"] = p.pause_after_restore;
  return j;
}

ResumePlan ResumeFromJson(const Json& j) {
  ResumePlan p;
  if (!j.IsObject()) return p;
  p.present = Bool(j, "present", false);
  p.from_epoch = Int(j, "from_epoch", 0);
  p.map_number = static_cast<int>(Int(j, "map_number", 1));
  p.round = static_cast<int>(Int(j, "round", 0));
  p.inline_backup = Bool(j, "inline_backup", false);
  p.file = Str(j, "file");
  p.sha256 = Str(j, "sha256");
  p.series_team1 = static_cast<int>(Int(j, "series_team1", 0));
  p.series_team2 = static_cast<int>(Int(j, "series_team2", 0));
  if (const Json* done = Arr(j, "maps_done")) {
    for (const auto& m : done->Items()) {
      ResumeMapResult r;
      r.map_number = static_cast<int>(Int(m, "map_number", 0));
      r.team1 = static_cast<int>(Int(m, "team1", 0));
      r.team2 = static_cast<int>(Int(m, "team2", 0));
      r.winner = Str(m, "winner", "none");
      p.maps_done.push_back(r);
    }
  }
  p.sides = Str(j, "sides");
  p.score_team1 = static_cast<int>(Int(j, "score_team1", -1));
  p.score_team2 = static_cast<int>(Int(j, "score_team2", -1));
  p.team1_side = Str(j, "team1_side");
  if (const Json* ms = Obj(j, "map_stats")) p.map_stats = *ms;
  p.pause_after_restore = Bool(j, "pause_after_restore", true);
  return p;
}

bool ApplyUpdateOps(Json* config, const Json& ops, std::string* err, bool* passwordChanged) {
  auto fail = [&](const std::string& why) {
    if (err) *err = why;
    return false;
  };
  if (!config || !config->IsObject()) return fail("no config");
  if (ops.type() != Json::Type::Array || ops.Items().empty()) return fail("ops must be a non-empty array");
  Json c = *config;
  bool pw = false;
  for (const auto& op : ops.Items()) {
    const std::string kind = Str(op, "op");
    if (kind == "add_player") {
      const Json* sid = op.Find("steamid64");
      if (!sid || !ValidSteam(*sid)) return fail("add_player: bad steamid64");
      const std::string team = Str(op, "team");
      const std::string role = Str(op, "role", "player");
      if (role != "player" && role != "sub" && role != "coach") return fail("add_player: bad role");
      RemoveEverywhere(&c, sid->AsString());
      if (team == "spectator") {
        Json specs = c.Find("spectators") ? *c.Find("spectators") : Json::Array();
        specs.Push(sid->AsString());
        c["spectators"] = std::move(specs);
      } else if (team == "team1" || team == "team2") {
        Json& t = c[team];
        Json players = t.Find("players") ? *t.Find("players") : Json::Array();
        Json p = Json::Object();
        p["steamid64"] = sid->AsString();
        p["name"] = Str(op, "name");
        p["role"] = role;
        players.Push(std::move(p));
        t["players"] = std::move(players);
      } else {
        return fail("add_player: team must be team1, team2 or spectator");
      }
    } else if (kind == "remove_player") {
      const Json* sid = op.Find("steamid64");
      if (!sid || !ValidSteam(*sid)) return fail("remove_player: bad steamid64");
      if (!RemoveEverywhere(&c, sid->AsString())) return fail("remove_player: " + sid->AsString() + " is not in the match");
    } else if (kind == "rename_team") {
      const std::string team = Str(op, "team");
      const std::string name = Str(op, "name");
      if (team != "team1" && team != "team2") return fail("rename_team: team must be team1 or team2");
      if (name.empty() || name.size() > 64) return fail("rename_team: name must be 1..64 bytes");
      c[team]["name"] = name;
    } else if (kind == "set_password") {
      const Json* p = op.Find("password");
      if (!p || p->type() != Json::Type::String || !ValidPassword(p->AsString())) return fail("set_password: bad password");
      c["password"] = p->AsString();
      pw = true;
    } else if (kind == "set_rules") {
      const Json* r = Obj(op, "rules");
      if (!r) return fail("set_rules: rules must be an object");
      const Json* cur = c.Find("rules");
      c["rules"] = MergePatchApply(cur ? *cur : Json::Object(), *r);
      if (!ValidateRuleset(c, err)) return false;
    } else {
      return fail("unknown op \"" + kind + "\"");
    }
  }
  *config = std::move(c);
  if (passwordChanged) *passwordChanged = pw;
  return true;
}

bool InAssignedMatch(const Json& config, uint64_t steamid64) {
  if (!steamid64) return false;
  if (!TeamOf(config, steamid64).empty()) return true;
  if (const Json* a = Arr(config, "admins")) {
    const std::string sid = std::to_string(steamid64);
    for (const auto& s : a->Items()) {
      if (s.AsString() == sid) return true;
    }
  }
  return false;
}

std::string TeamOf(const Json& config, uint64_t steamid64) {
  const std::string sid = std::to_string(steamid64);
  for (const char* team : {"team1", "team2"}) {
    const Json* t = Obj(config, team);
    const Json* ps = t ? Arr(*t, "players") : nullptr;
    if (!ps) continue;
    for (const auto& p : ps->Items()) {
      if (Str(p, "steamid64") == sid) return Str(p, "role", "player") == "coach" ? "spectator" : team;
    }
  }
  if (const Json* s = Arr(config, "spectators")) {
    for (const auto& e : s->Items()) {
      if (e.AsString() == sid) return "spectator";
    }
  }
  return {};
}

// ---------------------------------------------------------------------------- codecs

namespace {

constexpr uint32_t kK[64] = {
    0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
    0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
    0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
    0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
    0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
    0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
    0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
    0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2};

inline uint32_t Rotr(uint32_t x, int n) { return (x >> n) | (x << (32 - n)); }

}  // namespace

std::string Sha256Hex(const std::string& data) {
  uint32_t h[8] = {0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a, 0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19};
  std::string msg = data;
  const uint64_t bits = static_cast<uint64_t>(data.size()) * 8;
  msg.push_back(static_cast<char>(0x80));
  while (msg.size() % 64 != 56) msg.push_back('\0');
  for (int i = 7; i >= 0; --i) msg.push_back(static_cast<char>((bits >> (i * 8)) & 0xff));
  for (size_t off = 0; off < msg.size(); off += 64) {
    uint32_t w[64];
    for (int i = 0; i < 16; ++i) {
      const auto* p = reinterpret_cast<const unsigned char*>(msg.data() + off + static_cast<size_t>(i) * 4);
      w[i] = (uint32_t{p[0]} << 24) | (uint32_t{p[1]} << 16) | (uint32_t{p[2]} << 8) | uint32_t{p[3]};
    }
    for (int i = 16; i < 64; ++i) {
      const uint32_t s0 = Rotr(w[i - 15], 7) ^ Rotr(w[i - 15], 18) ^ (w[i - 15] >> 3);
      const uint32_t s1 = Rotr(w[i - 2], 17) ^ Rotr(w[i - 2], 19) ^ (w[i - 2] >> 10);
      w[i] = w[i - 16] + s0 + w[i - 7] + s1;
    }
    uint32_t a = h[0], b = h[1], c = h[2], d = h[3], e = h[4], f = h[5], g = h[6], hh = h[7];
    for (int i = 0; i < 64; ++i) {
      const uint32_t S1 = Rotr(e, 6) ^ Rotr(e, 11) ^ Rotr(e, 25);
      const uint32_t ch = (e & f) ^ (~e & g);
      const uint32_t t1 = hh + S1 + ch + kK[i] + w[i];
      const uint32_t S0 = Rotr(a, 2) ^ Rotr(a, 13) ^ Rotr(a, 22);
      const uint32_t maj = (a & b) ^ (a & c) ^ (b & c);
      const uint32_t t2 = S0 + maj;
      hh = g;
      g = f;
      f = e;
      e = d + t1;
      d = c;
      c = b;
      b = a;
      a = t1 + t2;
    }
    h[0] += a;
    h[1] += b;
    h[2] += c;
    h[3] += d;
    h[4] += e;
    h[5] += f;
    h[6] += g;
    h[7] += hh;
  }
  static const char* kHex = "0123456789abcdef";
  std::string out;
  out.reserve(64);
  for (uint32_t v : h) {
    for (int i = 28; i >= 0; i -= 4) out.push_back(kHex[(v >> i) & 0xf]);
  }
  return out;
}

namespace {
const char kB64[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
}

std::string Base64Encode(const std::string& data) {
  std::string out;
  out.reserve((data.size() + 2) / 3 * 4);
  size_t i = 0;
  const auto* d = reinterpret_cast<const unsigned char*>(data.data());
  for (; i + 2 < data.size(); i += 3) {
    const uint32_t v = (uint32_t{d[i]} << 16) | (uint32_t{d[i + 1]} << 8) | d[i + 2];
    out.push_back(kB64[(v >> 18) & 63]);
    out.push_back(kB64[(v >> 12) & 63]);
    out.push_back(kB64[(v >> 6) & 63]);
    out.push_back(kB64[v & 63]);
  }
  if (i < data.size()) {
    uint32_t v = uint32_t{d[i]} << 16;
    if (i + 1 < data.size()) v |= uint32_t{d[i + 1]} << 8;
    out.push_back(kB64[(v >> 18) & 63]);
    out.push_back(kB64[(v >> 12) & 63]);
    out.push_back(i + 1 < data.size() ? kB64[(v >> 6) & 63] : '=');
    out.push_back('=');
  }
  return out;
}

bool Base64Decode(const std::string& text, std::string* out) {
  if (!out) return false;
  out->clear();
  int val = 0, bits = -8;
  size_t pad = 0;
  for (unsigned char c : text) {
    if (c == '\n' || c == '\r') continue;
    if (c == '=') {
      ++pad;
      continue;
    }
    if (pad) return false;  // data after padding
    const char* p = std::strchr(kB64, c);
    if (!p || c == '\0') return false;
    val = (val << 6) | static_cast<int>(p - kB64);
    bits += 6;
    if (bits >= 0) {
      out->push_back(static_cast<char>((val >> bits) & 0xff));
      bits -= 8;
    }
    val &= 0xffff;
  }
  return pad <= 2;
}

// ---------------------------------------------------------------------------- validators

bool ValidateExec(const std::string& command, std::string* err) {
  auto fail = [&](const std::string& why) {
    if (err) *err = why;
    return false;
  };
  if (command.empty()) return fail("empty command");
  if (command.size() > 512) return fail("longer than 512 bytes");
  for (unsigned char c : command) {
    if (c == '\n' || c == '\r' || c == '\0') return fail("more than one line");
  }
  // Every `;`-separated part (the console splits there, quotes or not, for this check).
  size_t start = 0;
  while (start <= command.size()) {
    size_t end = command.find(';', start);
    if (end == std::string::npos) end = command.size();
    std::string part = command.substr(start, end - start);
    std::string l;
    for (unsigned char c : part) l.push_back(static_cast<char>(std::tolower(c)));
    const size_t b = l.find_first_not_of(" \t\"");
    l = b == std::string::npos ? std::string() : l.substr(b);
    auto startsWord = [&](const char* w) {
      const size_t n = std::strlen(w);
      return l.compare(0, n, w) == 0 && (l.size() == n || l[n] == ' ' || l[n] == '\t' || l[n] == '"');
    };
    if (startsWord("fleet") || (startsWord("ru") && l.find("fleet") != std::string::npos) ||
        l.rfind("ru_fleet", 0) == 0) {
      return fail("the fleet link's own commands are not allowed over exec");
    }
    start = end + 1;
  }
  return true;
}

bool ValidPassword(const std::string& password) {
  if (password.size() > 64) return false;
  for (unsigned char c : password) {
    if (c <= 0x20 || c >= 0x7f || c == '"' || c == ';' || c == '\\' || c == '\'') return false;
  }
  return true;
}

bool SafeMapName(const std::string& name) { return mapnames::ValidEntry(name); }

bool SafeWorkshopId(const std::string& id) { return IsDigits(id, 20); }

int BackupRoundFromName(const std::string& fileName) {
  const size_t p = fileName.rfind("round");
  if (p == std::string::npos) return -1;
  size_t i = p + 5;
  if (i >= fileName.size() || !std::isdigit(static_cast<unsigned char>(fileName[i]))) return -1;
  int n = 0;
  while (i < fileName.size() && std::isdigit(static_cast<unsigned char>(fileName[i])) && n < 100000) {
    n = n * 10 + (fileName[i] - '0');
    ++i;
  }
  return n;
}

long long FileTimeUnixMs(std::filesystem::file_time_type t) {
  using namespace std::chrono;
  const auto sys = system_clock::now() + duration_cast<system_clock::duration>(t - std::filesystem::file_time_type::clock::now());
  return duration_cast<milliseconds>(sys.time_since_epoch()).count();
}

bool LiveSessionBackup(const std::string& fileName, const std::string& prefix, long long mtimeMs,
                       long long liveSinceMs) {
  if (liveSinceMs < 0 || fileName.compare(0, prefix.size(), prefix) != 0) return false;
  if (fileName.size() < 4 || fileName.compare(fileName.size() - 4, 4, ".txt") != 0) return false;
  return BackupRoundFromName(fileName) >= 0 && mtimeMs >= liveSinceMs - kLiveBackupSlackMs;
}

bool SafeBackupFileName(const std::string& name) {
  if (name.size() < 5 || name.size() > 128) return false;
  if (name.compare(name.size() - 4, 4, ".txt") != 0) return false;
  if (name[0] == '.') return false;
  for (unsigned char c : name) {
    if (!(std::isalnum(c) || c == '_' || c == '-' || c == '.')) return false;
  }
  return true;
}

namespace {
constexpr double kPartsTtlS = 120.0;
constexpr size_t kPartsMaxFiles = 4;
constexpr long long kPartsMaxBytes = 16LL * 1024 * 1024;
}  // namespace

void BackupParts::Expire(double now) {
  for (auto it = files_.begin(); it != files_.end();) {
    if (now - it->second.last > kPartsTtlS) it = files_.erase(it);
    else ++it;
  }
}

bool BackupParts::Add(const Json& backup, double now, std::string* why) {
  Expire(now);
  const Json* file = backup.Find("file");
  const Json* size = backup.Find("size");
  const Json* sha = backup.Find("sha256");
  const Json* data = backup.Find("data");
  const Json* part = backup.Find("part");
  const Json* parts = backup.Find("parts");
  const std::string name = file ? file->AsString() : "";
  const std::string hash = sha ? sha->AsString() : "";
  const long long total = size ? size->AsInt() : 0;
  const long long n = parts ? parts->AsInt() : 1;
  const long long k = part ? part->AsInt() : 1;
  if (!SafeBackupFileName(name) || hash.size() != 64 || total < 1 || total > 4 * 1024 * 1024 || n < 1 || n > 64 ||
      k < 1 || k > n || !data) {
    *why = "bad backup part";
    return false;
  }
  std::string raw;
  if (!Base64Decode(data->AsString(), &raw) || raw.empty()) {
    *why = "bad base64";
    return false;
  }
  long long held = 0;
  for (const auto& kv : files_)
    for (const auto& p : kv.second.got) held += static_cast<long long>(p.second.size());
  // A part sent again replaces its earlier copy: that copy does not count against the cap.
  if (const auto prev = files_.find(hash); prev != files_.end()) {
    if (const auto old = prev->second.got.find(static_cast<int>(k)); old != prev->second.got.end())
      held -= static_cast<long long>(old->second.size());
  }
  if (held + static_cast<long long>(raw.size()) > kPartsMaxBytes) {
    *why = "too much backup data waiting";
    return false;
  }
  auto it = files_.find(hash);
  if (it == files_.end()) {
    if (files_.size() >= kPartsMaxFiles) {
      auto oldest = files_.begin();
      for (auto j = files_.begin(); j != files_.end(); ++j)
        if (j->second.last < oldest->second.last) oldest = j;
      files_.erase(oldest);
    }
    it = files_.emplace(hash, File{name, total, static_cast<int>(n), {}, now}).first;
  }
  File& f = it->second;
  if (f.parts != n || f.size != total) {
    *why = "parts disagree on the file";
    return false;
  }
  f.got[static_cast<int>(k)] = std::move(raw);
  f.last = now;
  return true;
}

bool BackupParts::Take(const std::string& sha256, double now, std::string* raw) {
  Expire(now);
  auto it = files_.find(sha256);
  if (it == files_.end() || static_cast<int>(it->second.got.size()) != it->second.parts) return false;
  std::string whole;
  for (const auto& p : it->second.got) whole += p.second;
  const bool ok = static_cast<long long>(whole.size()) == it->second.size && Sha256Hex(whole) == sha256;
  files_.erase(it);
  if (!ok) return false;
  *raw = std::move(whole);
  return true;
}

AvailabilityInfo ComputeAvailability(const AvailabilityInputs& in) {
  if (in.draining) return {"draining", "draining", in.drainReason, ""};
  if (!in.assigned) {
    if (in.localMatch) return {"busy", "local_match", in.localSlug, ""};
    return {"available", "", "", "idle"};
  }
  if (in.serverReset) return {"available", "", "", "series_end"};
  if (in.seriesOver) return {"busy", "awaiting_reset", in.matchId, ""};
  return {"busy", "assigned", in.matchId, ""};
}

Json AvailabilityNotice(const AvailabilityInfo& a) {
  Json p = Json::Object();
  p["availability"] = a.availability;
  p["reason"] = a.reason.empty() ? a.legacyReason : a.reason;
  p["detail"] = a.detail.empty() ? Json() : Json(a.detail);
  return p;
}

ResetOutcome PlanReset(bool assigned, const std::string& asgMatchId, bool localMatch, const std::string& localSlug,
                       const std::string& pendingRecoverySlug) {
  if (assigned) return {"assignment", asgMatchId};
  if (localMatch) return {"local", localSlug};
  if (!pendingRecoverySlug.empty()) return {"local", pendingRecoverySlug};
  return {"none", ""};
}

Json ResetResultJson(const ResetOutcome& o) {
  Json p = Json::Object();
  p["ended"] = o.ended;
  p["match_id"] = o.matchId.empty() ? Json() : Json(o.matchId);
  return p;
}

const char* CsTeamName(int csTeam) {
  switch (csTeam) {
    case 1:
      return "spectator";
    case 2:
      return "t";
    case 3:
      return "ct";
    default:
      return "none";
  }
}

Json PlayersJson(const std::vector<std::tuple<uint64_t, std::string, int>>& humans) {
  Json out = Json::Array();
  for (const auto& [sid, name, team] : humans) {
    if (sid == 0 || out.Items().size() >= 64) continue;
    Json p = Json::Object();
    p["steamid64"] = std::to_string(sid);
    p["name"] = name.substr(0, 128);
    p["team"] = CsTeamName(team);
    out.Push(std::move(p));
  }
  return out;
}

bool ValidateAddTime(const Json& args, const std::string& phase, bool roundOver, bool inFreeze, int* seconds,
                     std::string* code, std::string* why) {
  const Json* v = args.Find("seconds");
  if (!v || v->type() != Json::Type::Int || v->AsInt() < 1 || v->AsInt() > 600) {
    *code = "bad_args";
    *why = "seconds must be a whole number from 1 to 600";
    return false;
  }
  if (phase != "live" && phase != "overtime") {
    *code = "bad_phase";
    *why = "add_time works during a live round only";
    return false;
  }
  if (roundOver || inFreeze) {
    *code = "bad_phase";
    *why = "no round in progress";
    return false;
  }
  *seconds = static_cast<int>(v->AsInt());
  return true;
}

}  // namespace readyup::fleetstate

#include "readyup/match_config_parser.h"

#include "readyup/minijson.h"
#include "readyup/ruleset.h"
#include "readyup/simulation_rules.h"
#include "readyup/status_snapshot.h"
#include "readyup/steamid.h"
#include "readyup/wingman.h"

#include <algorithm>
#include <cmath>
#include <optional>
#include <string>

namespace readyup {

namespace {

// "ruleset" / "overrides" (ruleset.h), validated here so a bad value refuses the load, and what
// the ruleset changes in the context (knife sides, overtime, coaches).
bool ApplyRulesetToContext(const std::string& json, WebhookMatchContext* ctx, std::string* errOut) {
  using status::Json;
  auto fail = [&](const std::string& why) {
    if (errOut) *errOut = why;
    return false;
  };
  Json root;
  if (!Json::Parse(json, &root) || !root.IsObject()) return true;  // nothing more to read
  const Json* cfg = &root;
  if (const Json* c = root.Find("config"); c && c->IsObject()) cfg = c;

  Ruleset rs = ServerRuleset();
  if (const Json* r = cfg->Find("ruleset")) {
    if (r->type() != Json::Type::String || !ParseRuleset(r->AsString(), &rs)) {
      return fail("ruleset must be \"default\" or \"valve\"");
    }
    ctx->ruleset = RulesetName(rs);
  }
  // Wingman is 2v2; the valve ruleset is 5v5 CS Major rules. A match that asks for both is
  // refused; readyup.cfg's valve preset gives way to the default ruleset for a wingman match.
  if (ctx->wingman && rs == Ruleset::Valve) {
    if (cfg->Find("ruleset")) return fail("wingman needs ruleset \"default\" (the valve ruleset is 5v5)");
    rs = Ruleset::Default;
    ctx->ruleset = RulesetName(rs);
    ctx->ruleset_notes.push_back("wingman: ruleset default for this match (readyup.cfg sets valve, which is 5v5)");
  }
  RuleMap overrides;
  if (const Json* o = cfg->Find("overrides")) {
    std::string err;
    if (!ParseOverrides(*o, &overrides, &err)) return fail(err);
    ctx->overrides_json = OverridesToText(overrides);
  }

  RulesInput in;
  in.ruleset = rs;
  in.overrides = overrides;
  in.match = ctx->rules;
  for (const auto& kv : ctx->cvars) in.cvars[kv.first] = kv.second;
  const EffectiveRuleSet e = ResolveEffective(in);

  // Scrims keep their knife round (scrim_knife); matches need allow_knife for "knife" sides.
  std::string err;
  if (ctx->slug != "scrim" && !CheckMapSides(e, ctx->map_sides, &err)) return fail(err);

  if (rs == Ruleset::Valve) {
    // Overtime is the engine's (mp_overtime_limit, unlimited MR3 by default): Ready Up's own
    // overtime cap and damage tiebreak do not run.
    if (ctx->maxOvertimes >= 0 || ctx->damageTiebreakEnabled) {
      ctx->ruleset_notes.push_back(
          "maxOvertimes / damageTiebreak ignored (ruleset valve: overtime is the engine's, mp_overtime_limit)");
      ctx->maxOvertimes = -1;
      ctx->damageTiebreakEnabled = false;
    }
  }
  if (rs == Ruleset::Valve || overrides.count("overtime.enabled")) ctx->overtime_enabled = e.Bool("overtime.enabled", true);
  if (rs == Ruleset::Valve || overrides.count("overtime.maxrounds")) {
    ctx->overtimeSegments = std::max(1, static_cast<int>(e.Int("overtime.maxrounds", 6) / 2));
  }

  if (!ctx->coaches.empty()) {
    if (CoachesAdmitted(e)) {
      for (uint64_t sid : ctx->coaches) ctx->spectators.insert(sid);
    } else {
      for (uint64_t sid : ctx->coaches) ctx->spectators.erase(sid);
      ctx->ruleset_notes.push_back(std::to_string(ctx->coaches.size()) +
                                   " coach(es) not admitted (online match: lan false, coaches_online false)");
    }
  }
  return true;
}

}  // namespace

std::optional<WebhookMatchContext> ParseWebhookMatchContextFromJson(const std::string& json, std::string* errOut) {
  using namespace readyup::minijson;
  ParseError perr;
  auto rootOpt = Parse(json, &perr);
  if (!rootOpt) {
    if (errOut) {
      *errOut = perr.msg.empty() ? "json parse error" : perr.msg;
      *errOut += " at offset " + std::to_string(perr.offset);
    }
    return std::nullopt;
  }
  const Value& root = *rootOpt;

  // Accept either:
  // - MatchResponse wrapper: { id, slug, serverId, config: { ... MatchConfig ... }, ... }
  // - Raw MatchConfig object: { matchid, team1, team2, ... }
  const Value* cfg = &root;
  std::string slug;
  if (const Value* c = root.get("config"); IsObject(c)) {
    cfg = c;
    if (auto s = AsString(root.get("slug"))) slug = *s;
  }

  WebhookMatchContext ctx;
  ctx.slug = std::move(slug);

  // matchid
  if (auto mid = AsInt(cfg->get("matchid"))) ctx.matchid = static_cast<uint64_t>(*mid);
  if (ctx.matchid == 0) {
    if (auto id = AsInt(root.get("id"))) ctx.matchid = static_cast<uint64_t>(*id);
  }
  if (ctx.matchid == 0) {
    if (errOut) *errOut = "missing matchid";
    return std::nullopt;
  }

  if (auto nm = AsInt(cfg->get("num_maps"))) ctx.num_maps = static_cast<int>(*nm);
  if (const Value* cs = cfg->get("clinch_series"); cs && cs->type == Value::Type::Bool) ctx.clinch_series = cs->b;

  // cvars (object)
  if (const Value* cvars = cfg->get("cvars"); IsObject(cvars)) {
    auto fmtNum = [](double x) {
      // Prefer integer rendering when possible.
      const double r = std::round(x);
      if (std::fabs(x - r) < 1e-9) return std::to_string(static_cast<int64_t>(r));
      // Fallback: trim trailing zeros from a fixed-ish representation.
      std::string s = std::to_string(x);
      while (!s.empty() && s.back() == '0') s.pop_back();
      if (!s.empty() && s.back() == '.') s.pop_back();
      return s.empty() ? std::string("0") : s;
    };

    for (const auto& kv : cvars->obj) {
      const std::string& key = kv.first;
      const Value& v = kv.second;
      if (key.empty()) continue;
      if (v.type == Value::Type::String) {
        if (!v.str.empty()) ctx.cvars[key] = v.str;
      } else if (v.type == Value::Type::Number) {
        ctx.cvars[key] = fmtNum(v.num);
      } else if (v.type == Value::Type::Bool) {
        ctx.cvars[key] = v.b ? "1" : "0";
      }
    }
  }

  // Round limit metadata (preferred over deriving from cvars).
  // maxRounds: allow number or string.
  auto parsePositiveInt = [](const Value* v) -> std::optional<int> {
    if (!v) return std::nullopt;
    if (auto i = AsInt(v)) {
      if (*i > 0 && *i < 10000) return static_cast<int>(*i);
      return std::nullopt;
    }
    if (auto s = AsString(v)) {
      if (s->empty()) return std::nullopt;
      try {
        const int n = std::stoi(*s);
        if (n > 0 && n < 10000) return n;
      } catch (...) {
      }
    }
    return std::nullopt;
  };

  auto parseNonNegativeInt = [&](const Value* v) -> std::optional<int> {
    if (!v) return std::nullopt;
    if (auto i = AsInt(v)) {
      if (*i >= 0 && *i < 10000) return static_cast<int>(*i);
      return std::nullopt;
    }
    if (auto s = AsString(v)) {
      if (s->empty()) return std::nullopt;
      try {
        const int n = std::stoi(*s);
        if (n >= 0 && n < 10000) return n;
      } catch (...) {
      }
    }
    return std::nullopt;
  };

  auto parseBool = [](const Value* v) -> std::optional<bool> {
    if (!v) return std::nullopt;
    if (v->type == Value::Type::Bool) return v->b;
    if (v->type == Value::Type::Number) return v->num != 0.0;
    if (v->type == Value::Type::String) {
      const std::string& s = v->str;
      if (s == "1" || s == "true" || s == "TRUE" || s == "yes" || s == "YES") return true;
      if (s == "0" || s == "false" || s == "FALSE" || s == "no" || s == "NO") return false;
    }
    return std::nullopt;
  };

  bool maxRoundsExplicit = false;
  if (auto mr = parsePositiveInt(cfg->get("maxRounds"))) {
    ctx.maxRounds = *mr;
    maxRoundsExplicit = true;
  }

  // overtimeMode: "enabled" | "disabled"
  if (auto om = AsString(cfg->get("overtimeMode"))) {
    if (*om == "enabled") ctx.overtime_enabled = true;
    else ctx.overtime_enabled = false;
  }

  bool overtimeSegmentsExplicit = false;
  if (auto seg = parsePositiveInt(cfg->get("overtimeSegments"))) {
    // keep within a sane range; MatchZy commonly uses 3.
    ctx.overtimeSegments = std::max(1, std::min(60, *seg));
    overtimeSegmentsExplicit = true;
  }

  // Wingman (wingman.h) and simulation (simulation_rules.h).
  if (auto w = parseBool(cfg->get("wingman"))) ctx.wingman = *w;
  // Demo recording / upload for this match (demo_recorder.h); absent = the server's settings.
  if (auto d = parseBool(cfg->get("demo_record"))) ctx.demo_record = *d ? 1 : 0;
  if (auto d = parseBool(cfg->get("demo_upload"))) ctx.demo_upload = *d ? 1 : 0;
  if (auto sm = parseBool(cfg->get("simulation"))) ctx.simulation = *sm;
  if (const Value* ts = cfg->get("simulation_timescale")) {
    if (ts->type == Value::Type::Number) {
      ctx.simulation_timescale = sim::ClampTimescale(ts->num);
    } else if (auto tss = AsString(ts)) {
      try {
        ctx.simulation_timescale = sim::ClampTimescale(std::stod(*tss));
      } catch (...) {
      }
    }
  }
  if (auto fill = parseBool(cfg->get("bot_fill"))) ctx.bot_fill = *fill;
  // Mixed matches always run in real time and retain human roster identities.
  if (ctx.bot_fill) { ctx.simulation = false; ctx.simulation_timescale = 1.0; }
  if (ctx.wingman) {
    // live_wingman.cfg's MR8 / MR2 overtime unless the config says otherwise.
    if (!maxRoundsExplicit) ctx.maxRounds = wingman::kMaxRounds;
    if (!overtimeSegmentsExplicit) ctx.overtimeSegments = wingman::kOvertimeHalf;
  }

  // Tie-break configuration.
  if (auto mo = parseNonNegativeInt(cfg->get("maxOvertimes"))) {
    ctx.maxOvertimes = *mo;
  }
  if (auto dt = parseBool(cfg->get("damageTiebreak"))) {
    ctx.damageTiebreakEnabled = *dt;
  }
  if (auto sd = parseBool(cfg->get("damageTiebreakSuddenDeath"))) {
    ctx.suddenDeathOnDamageTie = *sd;
  }

  // Knife side-pick timeout (seconds).
  if (auto sec = parsePositiveInt(cfg->get("knifeDecisionSeconds"))) {
    ctx.knifeDecisionSeconds = std::max(5, std::min(300, *sec));
  } else {
    // Keep a sane clamp for defaults as well.
    ctx.knifeDecisionSeconds = std::max(5, std::min(300, ctx.knifeDecisionSeconds));
  }

  // Pause / ready / forfeit rules (match_rules.h). Unset keys stay -1.
  auto ruleInt = [&](const char* key, int* out) {
    if (auto n = parseNonNegativeInt(cfg->get(key))) *out = *n;
  };
  auto ruleBool = [&](const char* key, int* out) {
    if (auto b = parseBool(cfg->get(key))) *out = *b ? 1 : 0;
  };
  ruleInt("max_tech_pauses_per_team", &ctx.rules.tech_pauses_per_team);
  ruleInt("tech_pause_max_seconds", &ctx.rules.tech_pause_max_seconds);
  ruleInt("disconnect_pause_max_seconds", &ctx.rules.disconnect_pause_max_seconds);
  ruleBool("both_teams_unpause_required", &ctx.rules.both_teams_unpause);
  ruleBool("allow_force_ready", &ctx.rules.allow_force_ready);
  ruleBool("team_ready", &ctx.rules.team_ready);
  ruleInt("min_players_to_ready", &ctx.rules.min_players_to_ready);
  ruleInt("min_spectators_to_ready", &ctx.rules.min_spectators_to_ready);
  ruleInt("forfeit_after_seconds", &ctx.rules.forfeit_after_seconds);
  ruleBool("gg_enabled", &ctx.rules.gg_enabled);
  if (const Value* g = cfg->get("gg_threshold")) {
    const int pct = g->type == Value::Type::Number   ? GgThresholdPctFromText(std::to_string(g->num))
                    : g->type == Value::Type::String ? GgThresholdPctFromText(g->str)
                                                     : -1;
    if (pct > 0) ctx.rules.gg_threshold_pct = pct;
  }
  ruleInt("gg_min_score_diff", &ctx.rules.gg_min_score_diff);
  ruleBool("stop_command_available", &ctx.rules.stop_command_available);
  ruleBool("stop_command_no_damage", &ctx.rules.stop_command_no_damage);
  ruleInt("stop_vote_seconds", &ctx.rules.stop_vote_seconds);
  ruleBool("pause_after_restore", &ctx.rules.pause_after_restore);
  // Server settings the match may set for itself (server_settings.h).
  if (auto n = parseNonNegativeInt(cfg->get("players_per_team"))) ctx.players_per_team = std::min(*n, 32);
  // Wingman is 2v2: a full team (ready gate, simulation bots) is 2 unless the config says otherwise.
  if (ctx.wingman && ctx.players_per_team == 0) ctx.players_per_team = wingman::kPlayersPerTeam;
  ruleBool("playout", &ctx.playout);
  ruleBool("whitelist", &ctx.whitelist);
  ruleBool("autoready", &ctx.autoready);
  ruleBool("roster_names", &ctx.roster_names_rule);

  // The server plays the cvars' mp_maxrounds (they go out after every cfg), so the map end is
  // decided on it too, also when maxRounds is given: a standalone match said maxRounds 8 with
  // mp_maxrounds 4, CS2 ended the map at 3-0 and Ready Up waited for a 5th win forever.
  if (ctx.maxRounds <= 0) ctx.maxRounds = 24;
  {
    auto it = ctx.cvars.find("mp_maxrounds");
    if (it != ctx.cvars.end()) {
      try {
        const int n = std::stoi(it->second);
        if (n > 0 && n < 10000) ctx.maxRounds = n;
      } catch (...) {
      }
    }
  }

  // map_sides (array of strings)
  if (const Value* sides = cfg->get("map_sides"); sides && sides->type == Value::Type::Array) {
    for (const auto& v : sides->arr) {
      if (v.type == Value::Type::String && !v.str.empty()) {
        ctx.map_sides.push_back(v.str);
      }
    }
  }

  // maplist (array of strings)
  if (const Value* maps = cfg->get("maplist"); maps && maps->type == Value::Type::Array) {
    for (const auto& v : maps->arr) {
      if (v.type == Value::Type::String && !v.str.empty()) {
        ctx.maplist.push_back(v.str);
      }
    }
  }

  // spectators whitelist
  if (const Value* spec = cfg->get("spectators"); IsObject(spec)) {
    const Value* players = spec->get("players");
    if (IsObject(players)) {
      for (const auto& kv : players->obj) {
        const uint64_t sid = ParseSteamId64Loose(kv.first);
        if (!sid) continue;
        ctx.spectators.insert(sid);
      }
    }
  }

  // admins (array of SteamID64 values)
  if (const Value* admins = cfg->get("admins"); admins && admins->type == Value::Type::Array) {
    for (const auto& v : admins->arr) {
      uint64_t sid = 0;
      if (v.type == Value::Type::String) sid = ParseSteamId64Loose(v.str);
      else if (v.type == Value::Type::Number) sid = static_cast<uint64_t>(v.num);
      if (sid) ctx.admins.insert(sid);
    }
  }

  auto readTeam = [&](const char* key, std::string& nameOut, WebhookTeam teamTag) {
    const Value* team = cfg->get(key);
    if (!IsObject(team)) return;
    if (auto n = AsString(team->get("name"))) nameOut = *n;
    std::string& idOut = teamTag == WebhookTeam::Team1 ? ctx.team1_id : ctx.team2_id;
    if (auto id = AsString(team->get("id"))) idOut = *id;
    else if (auto idn = AsInt(team->get("id"))) idOut = std::to_string(*idn);
    if (auto f = AsString(team->get("flag"))) {
      (teamTag == WebhookTeam::Team1 ? ctx.team1_flag : ctx.team2_flag) = *f;
    }
    if (auto cap = AsString(team->get("captain_steamid64"))) {
      const uint64_t sid = ParseSteamId64Loose(*cap);
      if (sid != 0) {
        if (teamTag == WebhookTeam::Team1) ctx.team1_captain_steamid64 = sid;
        if (teamTag == WebhookTeam::Team2) ctx.team2_captain_steamid64 = sid;
      }
    } else if (auto capn = AsInt(team->get("captain_steamid64"))) {
      const uint64_t sid = static_cast<uint64_t>(*capn);
      if (sid != 0) {
        if (teamTag == WebhookTeam::Team1) ctx.team1_captain_steamid64 = sid;
        if (teamTag == WebhookTeam::Team2) ctx.team2_captain_steamid64 = sid;
      }
    }
    // coaches: {steamid64: name} (get5) or [steamid64]. Read before the players return early.
    if (const Value* coaches = team->get("coaches")) {
      auto add = [&](uint64_t sid) {
        if (sid) ctx.coach_team[sid] = teamTag;
      };
      if (IsObject(coaches)) {
        for (const auto& kv : coaches->obj) {
          const uint64_t sid = ParseSteamId64Loose(kv.first);
          add(sid);
          // Their name in game too (roster_names.h).
          if (sid && kv.second.type == Value::Type::String && !kv.second.str.empty()) ctx.roster_names[sid] = kv.second.str;
        }
      } else if (coaches->type == Value::Type::Array) {
        for (const auto& v : coaches->arr) {
          if (v.type == Value::Type::String) add(ParseSteamId64Loose(v.str));
        }
      }
    }
    // substitutes: {steamid64: name} or [steamid64]. On the roster (whitelisted, may stand in), never
    // part of the full team (players_per_team below).
    if (const Value* subs = team->get("substitutes")) {
      auto add = [&](uint64_t sid, const std::string& name) {
        if (!sid) return;
        ctx.substitutes.insert(sid);
        ctx.roster_team[sid] = teamTag;
        if (!name.empty()) ctx.roster_names[sid] = name;
      };
      if (IsObject(subs)) {
        for (const auto& kv : subs->obj) {
          add(ParseSteamId64Loose(kv.first), kv.second.type == Value::Type::String ? kv.second.str : std::string());
        }
      } else if (subs->type == Value::Type::Array) {
        for (const auto& v : subs->arr) {
          if (v.type == Value::Type::String) add(ParseSteamId64Loose(v.str), std::string());
        }
      }
    }
    const Value* players = team->get("players");
    if (!IsObject(players)) return;
    for (const auto& kv : players->obj) {
      const uint64_t sid = ParseSteamId64Loose(kv.first);
      if (!sid) continue;
      ctx.roster_team[sid] = teamTag;
      if (kv.second.type == Value::Type::String && !kv.second.str.empty()) ctx.roster_names[sid] = kv.second.str;
    }
  };

  readTeam("team1", ctx.team1_name, WebhookTeam::Team1);
  readTeam("team2", ctx.team2_name, WebhookTeam::Team2);
  // A config with substitutes and no players_per_team: a full team is the starters (the larger
  // side), so the substitutes never count toward the players who must ready up.
  if (!ctx.substitutes.empty() && ctx.players_per_team == 0) {
    int starters[2] = {0, 0};
    for (const auto& kv : ctx.roster_team) {
      if (ctx.substitutes.count(kv.first)) continue;
      ++starters[kv.second == WebhookTeam::Team2 ? 1 : 0];
    }
    ctx.players_per_team = std::min(32, std::max(starters[0], starters[1]));
  }

  // coaches (array of SteamID64 values): spectators unless the ruleset keeps them out.
  if (const Value* coaches = cfg->get("coaches"); coaches && coaches->type == Value::Type::Array) {
    for (const auto& v : coaches->arr) {
      const uint64_t sid = v.type == Value::Type::String ? ParseSteamId64Loose(v.str) : 0;
      if (sid) ctx.coaches.insert(sid);
    }
  }
  // A player on a roster is a player, not a coach.
  for (auto it = ctx.coach_team.begin(); it != ctx.coach_team.end();) {
    if (ctx.roster_team.count(it->first)) {
      it = ctx.coach_team.erase(it);
    } else {
      ctx.coaches.insert(it->first);
      ++it;
    }
  }
  if (auto n = AsInt(cfg->get("coaches_per_team"))) ctx.coaches_per_team = static_cast<int>(*n);

  if (!ApplyRulesetToContext(json, &ctx, errOut)) return std::nullopt;
  return ctx;
}

}  // namespace readyup


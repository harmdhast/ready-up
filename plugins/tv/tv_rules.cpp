#include "tv_rules.h"

#include "readyup/minijson.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace tv {

namespace bc = readyup::broadcast;
using readyup::minijson::Value;

namespace {

std::string Quote(const std::string& s) {
  std::string out = "\"";
  for (unsigned char c : s) {
    if (c == '"' || c == '\\') {
      out += '\\';
      out += static_cast<char>(c);
    } else if (c < 0x20) {
      char b[8];
      std::snprintf(b, sizeof(b), "\\u%04x", c);
      out += b;
    } else {
      out += static_cast<char>(c);
    }
  }
  return out + "\"";
}

std::optional<Value> ParseObj(const std::string& json) {
  readyup::minijson::ParseError pe;
  auto v = readyup::minijson::Parse(json, &pe);
  if (!v || v->type != Value::Type::Object) return std::nullopt;
  return v;
}

bool NumericValue(const std::string& v) {
  if (v.empty() || v.size() > 24) return false;
  for (unsigned char c : v) {
    if (!(std::isdigit(c) || c == '-' || c == '.')) return false;
  }
  return true;
}

std::string Trim(const std::string& s) {
  size_t a = 0, b = s.size();
  while (a < b && std::isspace(static_cast<unsigned char>(s[a]))) ++a;
  while (b > a && std::isspace(static_cast<unsigned char>(s[b - 1]))) --b;
  return s.substr(a, b - a);
}

std::string Lower(std::string s) {
  for (auto& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  return s;
}

// First integer after `word` (case-insensitive) in `line`, e.g. "spectators 2" -> 2.
std::optional<int> IntAfter(const std::string& lower, const char* word) {
  const size_t at = lower.find(word);
  if (at == std::string::npos) return std::nullopt;
  size_t i = at + std::strlen(word);
  while (i < lower.size() && (lower[i] == ' ' || lower[i] == ':' || lower[i] == '=' || lower[i] == '\t')) ++i;
  if (i >= lower.size() || !std::isdigit(static_cast<unsigned char>(lower[i]))) return std::nullopt;
  return std::atoi(lower.c_str() + i);
}

std::optional<int> CvarInt(const Observed& o, const char* name) {
  auto it = o.cvars.find(name);
  if (it == o.cvars.end() || it->second.empty()) return std::nullopt;
  char* end = nullptr;
  const double d = std::strtod(it->second.c_str(), &end);
  if (end == it->second.c_str()) return std::nullopt;
  return static_cast<int>(d);
}

std::string JInt(const std::optional<int>& v) { return v ? std::to_string(*v) : "null"; }
std::string JBool(const std::optional<int>& v) { return v ? (*v != 0 ? "true" : "false") : "null"; }
std::string JBool(const std::optional<bool>& v) { return v ? (*v ? "true" : "false") : "null"; }

std::string OnOff(const std::optional<int>& v) { return v ? (*v ? "on" : "off") : "?"; }

}  // namespace

// ---- applying -------------------------------------------------------------------------------

Plan PlanApply(Applier& a, const std::vector<std::pair<std::string, std::string>>& desired, bool force) {
  Plan p;
  std::map<std::string, std::string> next;
  for (const auto& kv : desired) {
    const auto have = a.applied.find(kv.first);
    if (have == a.applied.end() && !a.baseline.count(kv.first) && !a.baselineUnknown.count(kv.first)) {
      p.needBaseline.push_back(kv.first);
      continue;
    }
    if (force || have == a.applied.end() || have->second != kv.second) p.commands.push_back(kv.first + " " + kv.second);
    next[kv.first] = kv.second;
  }
  for (const auto& kv : a.applied) {
    if (next.count(kv.first)) continue;
    if (std::find(p.needBaseline.begin(), p.needBaseline.end(), kv.first) != p.needBaseline.end()) continue;
    const auto b = a.baseline.find(kv.first);
    if (b != a.baseline.end()) {
      if (b->second != kv.second || force) p.commands.push_back(kv.first + " " + b->second);
    } else if (kv.first == "tv_secure_bypass") {
      p.commands.push_back("tv_secure_bypass 0");  // the safe value (CS2 default)
    } else if (kv.first == "sv_deltaticks_enforce") {
      p.commands.push_back("sv_deltaticks_enforce 2");  // CS2 default (help sv_deltaticks_enforce, 1.41.9.1)
    } else {
      p.notes.push_back(kv.first + " left at " + kv.second + " (its value before Ready Up changed it is not known)");
    }
  }
  a.applied = std::move(next);
  return p;
}

void RecordBaseline(Applier& a, const std::string& cvar, const char* value) {
  if (cvar.find("password") != std::string::npos) return;
  const std::string v = value ? Trim(value) : std::string();
  // "105.000000" from a float cvar: keep the integer text the cvars take.
  std::string clean = v;
  if (NumericValue(v) && v.find('.') != std::string::npos) {
    clean = std::to_string(static_cast<long long>(std::strtod(v.c_str(), nullptr)));
  }
  if (!NumericValue(clean)) {
    a.baselineUnknown.insert(cvar);
    return;
  }
  a.baseline[cvar] = clean;
  a.baselineUnknown.erase(cvar);
}

std::string ApplierJson(const Applier& a) {
  auto map = [](const std::map<std::string, std::string>& m) {
    std::string o = "{";
    for (const auto& kv : m) o += (o.size() > 1 ? "," : "") + Quote(kv.first) + ":" + Quote(kv.second);
    return o + "}";
  };
  std::string u = "[";
  for (const auto& n : a.baselineUnknown) u += (u.size() > 1 ? "," : "") + Quote(n);
  u += "]";
  return "{\"baseline\":" + map(a.baseline) + ",\"unknown\":" + u + ",\"applied\":" + map(a.applied) + "}";
}

bool ParseApplier(const std::string& json, Applier* out) {
  const auto v = ParseObj(json);
  if (!v) return false;
  Applier a;
  auto map = [](const Value* o, std::map<std::string, std::string>* m) {
    if (!o || o->type != Value::Type::Object) return;
    for (const auto& kv : o->obj) {
      if (kv.second.type == Value::Type::String && NumericValue(kv.second.str) && kv.first.find("password") == std::string::npos) {
        (*m)[kv.first] = kv.second.str;
      }
    }
  };
  map(v->get("baseline"), &a.baseline);
  map(v->get("applied"), &a.applied);
  if (const Value* u = v->get("unknown"); u && u->type == Value::Type::Array) {
    for (const auto& n : u->arr) {
      if (n.type == Value::Type::String) a.baselineUnknown.insert(n.str);
    }
  }
  if (out) *out = std::move(a);
  return true;
}

// ---- the active match ---------------------------------------------------------------------

void MatchTracker::Assign(MatchProfile p, double now, double ttl) {
  if (active && active->matchId == p.matchId) {
    // The same match again (a resume, a re-sent assign): new rules, the admin's override stays.
    p.override_ = active->override_;
    active = std::move(p);
    pending.reset();
    mismatchSince = -1;
    return;
  }
  pending = std::move(p);
  pendingSince = now;
  pendingTtl = ttl;
}

bool MatchTracker::Unassign(const std::string& matchId) {
  bool ended = false;
  if (active && (matchId.empty() || active->matchId == matchId)) {
    active.reset();
    mismatchSince = -1;
    ended = true;
  }
  if (pending && (matchId.empty() || pending->matchId == matchId)) {
    pending.reset();
    ended = true;
  }
  return ended;
}

TrackEvent MatchTracker::Observe(const std::string& loaded, double now) {
  if (pending && !loaded.empty() && loaded == pending->matchId) {
    active = std::move(pending);
    pending.reset();
    mismatchSince = -1;
    return TrackEvent::Activated;
  }
  TrackEvent ev = TrackEvent::None;
  if (pending && now - pendingSince > pendingTtl) {
    pending.reset();
    ev = TrackEvent::Dropped;
  }
  if (!active) return ev;
  if (loaded == active->matchId) {
    mismatchSince = -1;
    return ev;
  }
  if (mismatchSince < 0) {
    mismatchSince = now;
    return ev;
  }
  if (now - mismatchSince >= kGraceSeconds) {
    active.reset();
    mismatchSince = -1;
    return TrackEvent::Deactivated;
  }
  return ev;
}

bool ApplyBroadcastPatch(const Value& rulesPatch, Profile* p, bool* touched, std::string* err) {
  if (touched) *touched = false;
  if (rulesPatch.type != Value::Type::Object) return true;
  const Value* b = rulesPatch.get("broadcast");
  if (!b) return true;
  if (touched) *touched = true;
  if (b->type == Value::Type::Null) {
    *p = Profile{};
    return true;
  }
  Profile set;
  std::vector<Key> cleared;
  std::string e;
  if (!bc::ParseProfile(*b, &set, &e, &cleared)) {
    if (err) *err = "rules.broadcast: " + e;
    return false;
  }
  Profile next = bc::Overlay(*p, set);
  for (Key k : cleared) next.Clear(k);
  *p = next;
  return true;
}

std::string MatchProfileJson(const MatchProfile& m) {
  return "{\"version\":1,\"match_id\":" + Quote(m.matchId) + ",\"profile\":" + bc::ProfileJson(m.profile) +
         ",\"override\":" + bc::ProfileJson(m.override_) + ",\"delay_floor\":" + std::to_string(m.delayFloor) +
         ",\"password_set\":" + (m.passwordSet ? "true" : "false") + ",\"match_cvars\":" + [&] {
           std::string o = "{";
           for (const auto& kv : m.matchCvars) o += (o.size() > 1 ? "," : "") + Quote(kv.first) + ":" + Quote(kv.second);
           return o + "}";
         }() + "}";
}

bool ParseMatchProfile(const std::string& json, MatchProfile* out) {
  const auto v = ParseObj(json);
  if (!v) return false;
  MatchProfile m;
  const Value* id = v->get("match_id");
  if (!id || id->type != Value::Type::String || id->str.empty() || id->str.size() > 64) return false;
  m.matchId = id->str;
  if (const Value* p = v->get("profile"); p && !bc::ParseProfile(*p, &m.profile, nullptr)) return false;
  if (const Value* p = v->get("override"); p && !bc::ParseProfile(*p, &m.override_, nullptr)) return false;
  if (const Value* f = v->get("delay_floor"); f && f->type == Value::Type::Number && f->num >= 0 &&
                                              f->num <= bc::kMaxDelaySeconds) {
    m.delayFloor = static_cast<int>(f->num);
  }
  if (const Value* pw = v->get("password_set"); pw && pw->type == Value::Type::Bool) m.passwordSet = pw->b;
  if (const Value* mc = v->get("match_cvars"); mc && mc->type == Value::Type::Object) {
    for (const auto& name : bc::OwnedCvars()) {
      const Value* x = mc->get(name.c_str());
      if (x && x->type == Value::Type::String && NumericValue(x->str)) m.matchCvars.emplace_back(name, x->str);
    }
  }
  if (out) *out = std::move(m);
  return true;
}

// ---- settings -----------------------------------------------------------------------------

uint64_t ParseSteamId64(const std::string& raw) {
  const std::string s = Trim(raw);
  if (s.size() != 17 || s.compare(0, 7, "7656119") != 0) return 0;
  for (unsigned char c : s) {
    if (!std::isdigit(c)) return 0;
  }
  return std::strtoull(s.c_str(), nullptr, 10);
}

std::set<uint64_t> ParseSteamIdList(const std::string& s, int* bad) {
  std::set<uint64_t> out;
  int nbad = 0;
  size_t start = 0;
  for (size_t i = 0; i <= s.size(); ++i) {
    if (i == s.size() || s[i] == ',' || s[i] == ' ' || s[i] == ';') {
      const std::string tok = Trim(s.substr(start, i - start));
      if (!tok.empty()) {
        if (const uint64_t id = ParseSteamId64(tok)) out.insert(id);
        else ++nbad;
      }
      start = i + 1;
    }
  }
  if (bad) *bad = nbad;
  return out;
}

Settings ParseSettings(const std::string& json, bool* ok) {
  Settings s;
  if (ok) *ok = true;
  if (Trim(json).empty()) return s;
  const auto v = ParseObj(json);
  if (!v) {
    if (ok) *ok = false;
    return s;
  }
  if (const Value* p = v->get("server")) {
    Profile prof;
    if (bc::ParseProfile(*p, &prof, nullptr)) s.server = prof;
    else if (ok) *ok = false;
  }
  if (const Value* c = v->get("casters"); c && c->type == Value::Type::Array) {
    s.castersSet = true;
    for (const auto& x : c->arr) {
      if (const uint64_t id = x.type == Value::Type::String ? ParseSteamId64(x.str) : 0) s.casters.insert(id);
    }
  }
  return s;
}

std::string SettingsJson(const Settings& s) {
  std::string out = "{\n  \"version\": 1,\n  \"server\": " + bc::ProfileJson(s.server);
  if (s.castersSet) {
    out += ",\n  \"casters\": [";
    bool first = true;
    for (uint64_t id : s.casters) {
      out += (first ? "\"" : ", \"") + std::to_string(id) + "\"";
      first = false;
    }
    out += "]";
  }
  return out + "\n}\n";
}

Profile ProfileFromConfig(const std::map<std::string, std::string>& kv, std::vector<std::string>* errors) {
  Profile p;
  for (Key k : bc::AllKeys()) {
    auto it = kv.find(bc::KeyName(k));
    if (it == kv.end() || Trim(it->second).empty()) continue;
    std::string err;
    const std::string v = Lower(Trim(it->second));
    if (v == "default" || v == "-" || !bc::SetFromText(&p, k, v, &err)) {
      if (errors && v != "default" && v != "-") errors->push_back(err);
    }
  }
  return p;
}

// ---- the engine's view --------------------------------------------------------------------

LaunchInfo ParseLaunchArgs(const std::vector<std::string>& args) {
  LaunchInfo li;
  for (size_t i = 0; i < args.size(); ++i) {
    const std::string& a = args[i];
    if (a == "-insecure") li.insecure = true;
    if (i + 1 >= args.size()) continue;
    if (a == "+tv_port" || a == "-tv_port") li.tvPort = std::atoi(args[i + 1].c_str());
    if (a == "+tv_password") {
      const std::string& v = args[i + 1];
      // The next token is the value unless it is the next option (an empty password).
      li.passwordSet = !v.empty() && v[0] != '+' && v[0] != '-' && v != "\"\"";
    }
  }
  return li;
}

bool ParseTvStatusLine(const std::string& raw, TvStatus* st) {
  std::string line = Trim(raw);
  // "GOTV[0]: ..." / "GOTV[1]: ...": instance 0 only.
  const std::string lower0 = Lower(line);
  if (lower0.rfind("gotv[", 0) == 0) {
    if (lower0.rfind("gotv[0]", 0) != 0) return false;
    const size_t colon = line.find(':');
    line = colon == std::string::npos ? std::string() : Trim(line.substr(colon + 1));
  }
  const std::string lower = Lower(line);
  bool hit = false;
  if (lower.find("not active") != std::string::npos &&
      (lower.find("sourcetv") != std::string::npos || lower.find("gotv") != std::string::npos ||
       lower.find("cstv") != std::string::npos || lower0.rfind("gotv[0]", 0) == 0)) {
    st->active = false;
    return true;
  }
  if (lower.rfind("total slots", 0) == 0) return true;  // relays' spectators: not this server's load
  if (lower.find("slots") != std::string::npos && lower.find("spectators") != std::string::npos) {
    if (auto n = IntAfter(lower, "slots")) st->slots = *n;
    if (auto n = IntAfter(lower, "spectators")) st->clients = *n;
    st->active = true;
    hit = true;
  }
  if ((lower.find("master") != std::string::npos || lower.find("sourcetv") != std::string::npos ||
       lower.find("gotv") != std::string::npos) &&
      lower.find("delay") != std::string::npos) {
    if (auto n = IntAfter(lower, "delay")) {
      st->delay = *n;
      st->active = true;
      hit = true;
    }
  }
  if (lower.rfind("local ip", 0) == 0) {
    const size_t colon = lower.find(':', 8);
    if (colon != std::string::npos && colon + 1 < lower.size() && std::isdigit(static_cast<unsigned char>(lower[colon + 1]))) {
      st->port = std::atoi(lower.c_str() + colon + 1);
      hit = true;
    }
  }
  return hit;
}

std::string StateJson(const View& v) {
  const Observed& o = v.observed;
  std::optional<int> port;
  if (v.port > 0) port = v.port;
  else if (auto p = CvarInt(o, "tv_port"); p && *p > 0) port = p;
  std::string out = "{";
  out += "\"available\":" + std::string(v.available ? "true" : "false");
  out += ",\"port\":" + JInt(port);
  out += ",\"enabled\":" + JBool(CvarInt(o, "tv_enable"));
  out += ",\"delay_seconds\":" + JInt(CvarInt(o, "tv_delay"));
  out += ",\"max_clients\":" + JInt(CvarInt(o, "tv_maxclients"));
  out += ",\"clients\":" + JInt(o.clients);
  out += ",\"secure_bypass\":" + JBool(CvarInt(o, "tv_secure_bypass"));
  out += ",\"deltaticks_enforce\":" + JBool(CvarInt(o, "sv_deltaticks_enforce"));
  out += ",\"relay_text_chat\":" + JBool(CvarInt(o, "tv_relaytextchat"));
  out += ",\"relay_voice\":" + JBool(CvarInt(o, "tv_relayvoice"));
  out += ",\"password_set\":" + JBool(v.passwordSet);
  out += ",\"profile\":{\"source\":" + Quote(v.source);
  if (v.source == "match" && !v.matchId.empty()) out += ",\"match_id\":" + Quote(v.matchId);
  out += ",\"settings\":" + bc::ProfileJson(v.effective) + "}";
  out += ",\"delay_floor\":" + std::to_string(v.delayFloor);
  out += ",\"casters\":" + std::to_string(v.casters);
  out += ",\"server_insecure\":" + std::string(v.serverInsecure ? "true" : "false");
  return out + "}";
}

std::vector<std::string> StatusLines(const View& v) {
  std::vector<std::string> out;
  const Observed& o = v.observed;
  if (!v.available) out.push_back("off (tv.cfg enabled=0): Ready Up does not touch the GOTV settings");
  std::optional<int> port;
  if (v.port > 0) port = v.port;
  else port = CvarInt(o, "tv_port");
  const auto enabled = CvarInt(o, "tv_enable");
  std::string l = "GOTV " + std::string(!enabled ? "?" : (*enabled ? "on" : "off")) + ", port " +
                  (port && *port > 0 ? std::to_string(*port) : std::string("?"));
  const auto delay = CvarInt(o, "tv_delay");
  l += ", delay " + (delay ? std::to_string(*delay) + " s" : std::string("?"));
  if (v.effectiveDelay >= 0) {
    l += " (Ready Up sets " + std::to_string(v.effectiveDelay) + ")";
  }
  if (v.delayFloor > 0) l += ", match minimum " + std::to_string(v.delayFloor) + " s";
  const auto maxc = CvarInt(o, "tv_maxclients");
  l += ", clients " + (o.clients ? std::to_string(*o.clients) : std::string("?")) + "/" +
       (maxc ? std::to_string(*maxc) : std::string("?"));
  out.push_back(l);
  const auto bypass = CvarInt(o, "tv_secure_bypass");
  const auto delta = CvarInt(o, "sv_deltaticks_enforce");
  out.push_back("insecure observers (HLAE / HUD tools) on GOTV: " +
                std::string(!bypass ? "?" : (*bypass ? "allowed" : "not allowed")) + " (tv_secure_bypass " +
                (bypass ? std::to_string(*bypass) : "?") + ", sv_deltaticks_enforce " +
                (delta ? std::to_string(*delta) : "?") + "); the game port stays VAC-secured");
  out.push_back("relay: text chat " + OnOff(CvarInt(o, "tv_relaytextchat")) + ", voice " +
                OnOff(CvarInt(o, "tv_relayvoice")) + "; password " +
                (!v.passwordSet ? "unknown" : (*v.passwordSet ? "set" : "not set")));
  out.push_back("profile: " + (v.source == "match" ? "match " + v.matchId : std::string("server defaults")) + " " +
                bc::ProfileJson(v.effective));
  std::string c = "casters: " + std::to_string(v.casters);
  if (v.casters > 0 && !v.whitelistLoaded) c += " (whitelist.so is not loaded: casters are not let in outside a match's spectators)";
  out.push_back(c);
  if (v.serverInsecure) out.push_back("warning: this server itself runs -insecure (not VAC-secured)");
  return out;
}

}  // namespace tv

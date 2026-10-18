#include "readyup/broadcast_profile.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdlib>

namespace readyup::broadcast {

using minijson::Value;

namespace {

bool Fail(std::string* err, const std::string& why) {
  if (err) *err = why;
  return false;
}

std::string Lower(std::string s) {
  for (auto& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  return s;
}

bool IsIntKey(Key k) { return k == Key::Delay || k == Key::MaxClients; }
int MaxOf(Key k) { return k == Key::Delay ? kMaxDelaySeconds : kMaxClients; }

bool SetInt(Profile* p, Key k, int v) {
  if (k == Key::Delay) p->delay_seconds = v;
  else if (k == Key::MaxClients) p->max_clients = v;
  else return false;
  return true;
}
bool SetBool(Profile* p, Key k, bool v) {
  if (k == Key::Insecure) p->allow_insecure_observers = v;
  else if (k == Key::TextChat) p->relay_text_chat = v;
  else if (k == Key::Voice) p->relay_voice = v;
  else return false;
  return true;
}

// A JSON number that is an integer in [0, max].
bool IntIn(const Value& v, int max, int* out) {
  if (v.type != Value::Type::Number || !std::isfinite(v.num) || v.num != std::floor(v.num)) return false;
  if (v.num < 0 || v.num > max) return false;
  *out = static_cast<int>(v.num);
  return true;
}

// A cvar value in a match config (string / number / bool) as the text the match plugin applies.
bool CvarText(const Value& v, std::string* out) {
  if (v.type == Value::Type::String) *out = v.str;
  else if (v.type == Value::Type::Bool) *out = v.b ? "1" : "0";
  else if (v.type == Value::Type::Number && std::isfinite(v.num) && v.num == std::floor(v.num)) {
    *out = std::to_string(static_cast<long long>(v.num));
  } else {
    return false;
  }
  return true;
}

// "105", "105.000000" -> 105; anything else -1.
int CvarInt(const std::string& s) {
  if (s.empty()) return -1;
  char* end = nullptr;
  const double d = std::strtod(s.c_str(), &end);
  if (end == s.c_str() || !std::isfinite(d) || d < 0 || d > 1e6) return -1;
  for (const char* c = end; *c; ++c) {
    if (!std::isspace(static_cast<unsigned char>(*c))) return -1;
  }
  return static_cast<int>(d);
}

}  // namespace

const std::vector<Key>& AllKeys() {
  static const std::vector<Key> k = {Key::Delay, Key::Insecure, Key::MaxClients, Key::TextChat, Key::Voice};
  return k;
}

const char* KeyName(Key k) {
  switch (k) {
    case Key::Delay: return "delay_seconds";
    case Key::Insecure: return "allow_insecure_observers";
    case Key::MaxClients: return "max_clients";
    case Key::TextChat: return "relay_text_chat";
    case Key::Voice: return "relay_voice";
  }
  return "?";
}

bool ParseKeyName(const std::string& raw, Key* out) {
  const std::string s = Lower(raw);
  struct A {
    const char* name;
    Key k;
  };
  static const A kAliases[] = {{"delay_seconds", Key::Delay},     {"delay", Key::Delay},
                               {"allow_insecure_observers", Key::Insecure}, {"insecure", Key::Insecure},
                               {"max_clients", Key::MaxClients}, {"maxclients", Key::MaxClients},
                               {"relay_text_chat", Key::TextChat}, {"textchat", Key::TextChat},
                               {"relay_voice", Key::Voice},       {"voice", Key::Voice}};
  for (const auto& a : kAliases) {
    if (s == a.name) {
      if (out) *out = a.k;
      return true;
    }
  }
  return false;
}

bool Profile::Empty() const {
  return !delay_seconds && !allow_insecure_observers && !max_clients && !relay_text_chat && !relay_voice;
}

bool Profile::Has(Key k) const {
  switch (k) {
    case Key::Delay: return delay_seconds.has_value();
    case Key::Insecure: return allow_insecure_observers.has_value();
    case Key::MaxClients: return max_clients.has_value();
    case Key::TextChat: return relay_text_chat.has_value();
    case Key::Voice: return relay_voice.has_value();
  }
  return false;
}

void Profile::Clear(Key k) {
  switch (k) {
    case Key::Delay: delay_seconds.reset(); break;
    case Key::Insecure: allow_insecure_observers.reset(); break;
    case Key::MaxClients: max_clients.reset(); break;
    case Key::TextChat: relay_text_chat.reset(); break;
    case Key::Voice: relay_voice.reset(); break;
  }
}

bool Profile::operator==(const Profile& o) const {
  return delay_seconds == o.delay_seconds && allow_insecure_observers == o.allow_insecure_observers &&
         max_clients == o.max_clients && relay_text_chat == o.relay_text_chat && relay_voice == o.relay_voice;
}

bool ParseProfile(const Value& obj, Profile* out, std::string* err, std::vector<Key>* cleared) {
  if (obj.type != Value::Type::Object) return Fail(err, "must be an object");
  Profile p;
  std::vector<Key> nulls;
  // Sorted, so the error for several bad keys is always the same one.
  std::vector<std::string> names;
  for (const auto& kv : obj.obj) names.push_back(kv.first);
  std::sort(names.begin(), names.end());
  for (const auto& name : names) {
    const Value& v = obj.obj.at(name);
    Key k;
    if (!ParseKeyName(name, &k) || name != KeyName(k)) return Fail(err, "unknown key \"" + name.substr(0, 40) + "\"");
    if (v.type == Value::Type::Null) {
      if (!cleared) return Fail(err, name + " must not be null");
      nulls.push_back(k);
      continue;
    }
    if (IsIntKey(k)) {
      int n = 0;
      if (!IntIn(v, MaxOf(k), &n)) return Fail(err, name + " must be an integer 0.." + std::to_string(MaxOf(k)));
      SetInt(&p, k, n);
    } else {
      if (v.type != Value::Type::Bool) return Fail(err, name + " must be a boolean");
      SetBool(&p, k, v.b);
    }
  }
  if (out) *out = p;
  if (cleared) *cleared = std::move(nulls);
  return true;
}

bool ParseProfileText(const std::string& json, Profile* out, std::string* err, std::vector<Key>* cleared) {
  minijson::ParseError pe;
  const auto v = minijson::Parse(json, &pe);
  if (!v) return Fail(err, "not JSON: " + pe.msg);
  return ParseProfile(*v, out, err, cleared);
}

bool SetFromText(Profile* p, Key k, const std::string& raw, std::string* err) {
  const std::string v = Lower(raw);
  if (v == "default" || v == "-") {
    p->Clear(k);
    return true;
  }
  if (IsIntKey(k)) {
    if (v.empty() || v.size() > 4) return Fail(err, std::string(KeyName(k)) + ": an integer 0.." + std::to_string(MaxOf(k)));
    for (unsigned char c : v) {
      if (!std::isdigit(c)) return Fail(err, std::string(KeyName(k)) + ": an integer 0.." + std::to_string(MaxOf(k)));
    }
    const int n = std::atoi(v.c_str());
    if (n > MaxOf(k)) return Fail(err, std::string(KeyName(k)) + ": an integer 0.." + std::to_string(MaxOf(k)));
    SetInt(p, k, n);
    return true;
  }
  if (v == "1" || v == "on" || v == "true" || v == "yes") return SetBool(p, k, true);
  if (v == "0" || v == "off" || v == "false" || v == "no") return SetBool(p, k, false);
  return Fail(err, std::string(KeyName(k)) + ": on or off");
}

std::string ValueText(const Profile& p, Key k) {
  switch (k) {
    case Key::Delay: return p.delay_seconds ? std::to_string(*p.delay_seconds) : std::string();
    case Key::MaxClients: return p.max_clients ? std::to_string(*p.max_clients) : std::string();
    case Key::Insecure: return p.allow_insecure_observers ? (*p.allow_insecure_observers ? "1" : "0") : "";
    case Key::TextChat: return p.relay_text_chat ? (*p.relay_text_chat ? "1" : "0") : "";
    case Key::Voice: return p.relay_voice ? (*p.relay_voice ? "1" : "0") : "";
  }
  return {};
}

Profile Overlay(const Profile& base, const Profile& over) {
  Profile p = base;
  if (over.delay_seconds) p.delay_seconds = over.delay_seconds;
  if (over.allow_insecure_observers) p.allow_insecure_observers = over.allow_insecure_observers;
  if (over.max_clients) p.max_clients = over.max_clients;
  if (over.relay_text_chat) p.relay_text_chat = over.relay_text_chat;
  if (over.relay_voice) p.relay_voice = over.relay_voice;
  return p;
}

std::string ProfileJson(const Profile& p) {
  std::string out = "{";
  for (Key k : AllKeys()) {
    if (!p.Has(k)) continue;
    if (out.size() > 1) out += ",";
    out += "\"";
    out += KeyName(k);
    out += "\":";
    const std::string v = ValueText(p, k);
    out += IsIntKey(k) ? v : (v == "1" ? "true" : "false");
  }
  return out + "}";
}

int DelayFloor(const Value& config) {
  int floor = 0;
  const Value* rules = config.get("rules");
  if (rules && rules->type == Value::Type::Object) {
    const Value* rs = rules->get("ruleset");
    const Value* ov = rules->get("overrides");
    const Value* od = ov && ov->type == Value::Type::Object ? ov->get("tv_delay") : nullptr;
    int n = 0;
    if (od && IntIn(*od, kMaxDelaySeconds, &n)) floor = std::max(floor, n);
    else if (rs && rs->type == Value::Type::String && rs->str == "valve") floor = std::max(floor, kValveDelaySeconds);
  }
  if (const Value* cv = config.get("cvars"); cv && cv->type == Value::Type::Object) {
    if (const Value* d = cv->get("tv_delay")) {
      std::string t;
      if (CvarText(*d, &t)) floor = std::max(floor, std::min(CvarInt(t), kMaxDelaySeconds));
    }
  }
  return std::max(floor, 0);
}

bool ProfileFromAssign(const Value& config, Profile* out, std::string* err) {
  Profile p;
  const Value* rules = config.get("rules");
  const Value* b = rules && rules->type == Value::Type::Object ? rules->get("broadcast") : nullptr;
  if (b && b->type != Value::Type::Null) {
    std::string e;
    if (!ParseProfile(*b, &p, &e)) return Fail(err, "rules.broadcast: " + e);
  }
  if (out) *out = p;
  return true;
}

bool PasswordInAssign(const Value& config) {
  const Value* cv = config.get("cvars");
  const Value* pw = cv && cv->type == Value::Type::Object ? cv->get("tv_password") : nullptr;
  return pw && pw->type == Value::Type::String && !pw->str.empty();
}

const std::vector<std::string>& OwnedCvars() {
  static const std::vector<std::string> v = {"tv_delay",      "tv_secure_bypass", "sv_deltaticks_enforce",
                                             "tv_maxclients", "tv_relaytextchat", "tv_relayvoice"};
  return v;
}

std::vector<std::pair<std::string, std::string>> DesiredCvars(const Profile& eff, int delayFloor, int* effectiveDelay) {
  std::vector<std::pair<std::string, std::string>> out;
  if (effectiveDelay) *effectiveDelay = -1;
  if (eff.delay_seconds) {
    const int d = std::max(*eff.delay_seconds, std::max(delayFloor, 0));
    if (effectiveDelay) *effectiveDelay = d;
    out.emplace_back("tv_delay", std::to_string(d));
  }
  if (eff.allow_insecure_observers) {
    out.emplace_back("tv_secure_bypass", *eff.allow_insecure_observers ? "1" : "0");
    if (*eff.allow_insecure_observers) out.emplace_back("sv_deltaticks_enforce", "0");
  }
  if (eff.max_clients) out.emplace_back("tv_maxclients", std::to_string(*eff.max_clients));
  if (eff.relay_text_chat) out.emplace_back("tv_relaytextchat", *eff.relay_text_chat ? "1" : "0");
  if (eff.relay_voice) out.emplace_back("tv_relayvoice", *eff.relay_voice ? "1" : "0");
  return out;
}

bool ValidateAssignConfig(const Value& config, std::string* err) {
  Profile p;
  if (!ProfileFromAssign(config, &p, err)) return false;
  if (p.Empty()) return true;
  const int floor = DelayFloor(config);
  if (p.delay_seconds && *p.delay_seconds < floor) {
    return Fail(err, "rules.broadcast.delay_seconds " + std::to_string(*p.delay_seconds) +
                         " is below the match's own GOTV delay (" + std::to_string(floor) + ")");
  }
  const Value* cv = config.get("cvars");
  if (!cv || cv->type != Value::Type::Object) return true;
  // The cvars the profile sets, against the same cvar in config.cvars (tv_delay: the floor covers it).
  for (const auto& kv : DesiredCvars(p, 0)) {
    if (kv.first == "tv_delay") continue;
    const Value* v = cv->get(kv.first.c_str());
    std::string t;
    if (!v || !CvarText(*v, &t)) continue;
    if (CvarInt(t) != CvarInt(kv.second)) {
      return Fail(err, "rules.broadcast and cvars." + kv.first + " disagree (" + kv.second + " vs " + t.substr(0, 16) + ")");
    }
  }
  return true;
}

std::vector<std::pair<std::string, std::string>> OwnedCvarsInAssign(const Value& config) {
  std::vector<std::pair<std::string, std::string>> out;
  const Value* cv = config.get("cvars");
  if (!cv || cv->type != Value::Type::Object) return out;
  for (const auto& name : OwnedCvars()) {
    const Value* v = cv->get(name.c_str());
    std::string t;
    if (v && CvarText(*v, &t) && CvarInt(t) >= 0) out.emplace_back(name, std::to_string(CvarInt(t)));
  }
  return out;
}

Profile ProfileFromMatchCvars(const std::vector<std::pair<std::string, std::string>>& matchCvars) {
  Profile p;
  for (const auto& kv : matchCvars) {
    const int n = CvarInt(kv.second);
    if (n < 0) continue;
    if (kv.first == "tv_secure_bypass") p.allow_insecure_observers = n != 0;
    else if (kv.first == "tv_maxclients" && n <= kMaxClients) p.max_clients = n;
    else if (kv.first == "tv_relaytextchat") p.relay_text_chat = n != 0;
    else if (kv.first == "tv_relayvoice") p.relay_voice = n != 0;
  }
  return p;
}

std::string ConflictWithMatchCvars(const Profile& eff, int delayFloor,
                                   const std::vector<std::pair<std::string, std::string>>& matchCvars) {
  for (const auto& kv : DesiredCvars(eff, delayFloor)) {
    if (kv.first == "tv_delay") continue;  // the floor covers it
    for (const auto& m : matchCvars) {
      if (m.first == kv.first && CvarInt(m.second) != CvarInt(kv.second)) return kv.first;
    }
  }
  return {};
}

bool ValidateAssignConfigText(const std::string& configJson, std::string* err) {
  minijson::ParseError pe;
  const auto v = minijson::Parse(configJson, &pe);
  if (!v || v->type != Value::Type::Object) return Fail(err, "config is not a JSON object");
  return ValidateAssignConfig(*v, err);
}

}  // namespace readyup::broadcast

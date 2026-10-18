#include "fleet_cmds.h"

#include <algorithm>
#include <cctype>
#include <cstdlib>

namespace fleet::cmds {

namespace {
bool Fail(std::string* err, const std::string& why) {
  if (err) *err = why;
  return false;
}
bool ValidPluginName(const std::string& n) {
  if (n.empty() || n.size() > 32) return false;
  for (unsigned char c : n) {
    if (!(std::islower(c) || std::isdigit(c) || c == '_' || c == '-')) return false;
  }
  return true;
}
}  // namespace

bool OwnedByFleet(const std::string& name) {
  return name == "plugins.set" || name == "whitelist.set" || name == "practice.set" || name == "say" ||
         name == "tv.set";
}

bool ParsePluginsSet(const json::Value& args, std::vector<std::string>* enable, std::vector<std::string>* disable,
                     std::string* err) {
  std::vector<std::string> on, off;
  for (const char* key : {"enable", "disable"}) {
    const json::Value* list = args.Get(key);
    if (!list || list->IsNull()) continue;
    if (!list->IsArr()) return Fail(err, std::string(key) + " must be an array of plugin names");
    if (list->a.size() > 16) return Fail(err, std::string(key) + ": at most 16 plugins");
    auto& out = std::string(key) == "enable" ? on : off;
    for (const json::Value& v : list->a) {
      if (!v.IsStr() || !ValidPluginName(v.s)) return Fail(err, std::string(key) + ": not a plugin name (want [a-z0-9_-], 1..32)");
      out.push_back(v.s);
    }
  }
  if (on.empty() && off.empty()) return Fail(err, "nothing to enable or disable");
  for (const auto& n : off) {
    if (n == "fleet") return Fail(err, "cannot disable fleet over the fleet link (the link runs in it)");
    if (std::find(on.begin(), on.end(), n) != on.end()) return Fail(err, n + " is in both enable and disable");
  }
  if (enable) *enable = std::move(on);
  if (disable) *disable = std::move(off);
  return true;
}

bool ParseWhitelistSet(const json::Value& args, bool* enabled, std::vector<uint64_t>* steamids, std::string* err) {
  const json::Value* e = args.Get("enabled");
  if (!e || e->t != json::Value::T::Bool) return Fail(err, "enabled (boolean) is required");
  std::vector<uint64_t> ids;
  if (const json::Value* list = args.Get("steamids"); list && !list->IsNull()) {
    if (!list->IsArr()) return Fail(err, "steamids must be an array of SteamID64 strings");
    if (list->a.size() > 1000) return Fail(err, "steamids: at most 1000");
    for (const json::Value& v : list->a) {
      const std::string s = v.IsStr() ? v.s : std::string();
      bool ok = s.size() == 17 && s.compare(0, 7, "7656119") == 0;
      for (unsigned char c : s) ok = ok && std::isdigit(c);
      if (!ok) return Fail(err, "steamids: \"" + s.substr(0, 32) + "\" is not a SteamID64 string");
      ids.push_back(std::strtoull(s.c_str(), nullptr, 10));
    }
  }
  if (enabled) *enabled = e->b;
  if (steamids) *steamids = std::move(ids);
  return true;
}

bool ParsePracticeSet(const json::Value& args, int* on, int* always, std::string* err, bool* hasHomeMap,
                      std::string* homeMap, bool* hasHomeCvars, std::vector<std::string>* homeCvars) {
  bool hasCvars = false;
  std::vector<std::string> cvars;
  if (const json::Value* v = args.Get("home_cvars"); v && !v->IsNull()) {
    if (!v->IsArr()) return Fail(err, "home_cvars must be an array of strings");
    if (v->a.size() > 24) return Fail(err, "home_cvars: at most 24");
    for (const json::Value& c : v->a) {
      if (!c.IsStr() || c.s.size() > 96) return Fail(err, "home_cvars: each a \"sv_/mp_<name> <number>\" string");
      cvars.push_back(c.s);
    }
    hasCvars = true;
  }
  int o = -1, a = -1;
  bool hasHome = false;
  std::string home;
  if (const json::Value* v = args.Get("home_map"); v && !v->IsNull()) {
    if (!v->IsStr()) return Fail(err, "home_map must be a string");
    if (v->s.size() > 200) return Fail(err, "home_map: at most 200 characters");
    for (unsigned char c : v->s) {
      if (c <= 0x20 || c == 0x7f || c == '"' || c == ';') return Fail(err, "home_map: not a map name, workshop id or link");
    }
    hasHome = true;
    home = v->s;
  }
  if (const json::Value* v = args.Get("on"); v && !v->IsNull()) {
    if (v->t != json::Value::T::Bool) return Fail(err, "on must be a boolean");
    o = v->b ? 1 : 0;
  }
  if (const json::Value* v = args.Get("always"); v && !v->IsNull()) {
    if (v->t != json::Value::T::Bool) return Fail(err, "always must be a boolean");
    a = v->b ? 1 : 0;
  }
  if (o < 0 && a < 0 && !hasHome && !hasCvars) return Fail(err, "on, always (boolean), home_map or home_cvars is required");
  if (hasHomeCvars) *hasHomeCvars = hasCvars;
  if (homeCvars) *homeCvars = cvars;
  if (on) *on = o;
  if (always) *always = a;
  if (hasHomeMap) *hasHomeMap = hasHome;
  if (homeMap) *homeMap = home;
  return true;
}

std::string SayCenterHtml(const std::string& text) {
  std::string out;
  for (char c : text) {
    switch (c) {
      case '<': out += "&lt;"; break;
      case '>': out += "&gt;"; break;
      case '&': out += "&amp;"; break;
      case '"': out += "&quot;"; break;
      default: out.push_back(c);
    }
  }
  return out;
}

int SayCenterSeconds(int64_t asked) {
  if (asked <= 0) return 0;
  return asked > 15 ? 15 : static_cast<int>(asked);
}

namespace {
// The longest prefix of `s` of at most `maxBytes` that does not split a UTF-8 character.
size_t Utf8Cut(const std::string& s, size_t maxBytes) {
  if (s.size() <= maxBytes) return s.size();
  size_t cut = maxBytes;
  while (cut > 0 && (static_cast<unsigned char>(s[cut]) & 0xC0) == 0x80) --cut;
  return cut;
}
}  // namespace

std::string SanitizeSay(const std::string& text, size_t maxBytes) {
  std::string s;
  for (unsigned char c : text) {
    if (c < 0x20 || c == 0x7f) continue;
    s.push_back(static_cast<char>(c));
  }
  s.resize(Utf8Cut(s, maxBytes));
  return s;
}

std::vector<std::string> SplitSay(const std::string& text) {
  std::string rest = SanitizeSay(text, kSayLineBytes * kSayMaxLines);
  std::vector<std::string> out;
  auto trim = [](std::string& s) {
    const size_t a = s.find_first_not_of(' ');
    if (a == std::string::npos) {
      s.clear();
      return;
    }
    s = s.substr(a, s.find_last_not_of(' ') - a + 1);
  };
  trim(rest);
  while (!rest.empty() && out.size() < kSayMaxLines) {
    size_t cut = Utf8Cut(rest, kSayLineBytes);
    if (cut < rest.size()) {
      // Break at the last space in the line, unless that leaves it very short (one long word).
      const size_t sp = rest.rfind(' ', cut);
      if (sp != std::string::npos && sp >= kSayLineBytes / 2) cut = sp;
    }
    std::string line = rest.substr(0, cut);
    rest = rest.substr(cut);
    trim(line);
    trim(rest);
    if (!line.empty()) out.push_back(std::move(line));
  }
  return out;
}

std::string AdminSayLine(const std::string& from, const std::string& text) {
  std::string line = " \x07[ADMIN]\x01 ";
  if (!from.empty()) line += "\x10" + from + "\x01: ";
  return line + text;
}

int SayTeam(const std::string& team) {
  if (team.empty()) return 0;
  if (team == "team1") return 1;
  if (team == "team2") return 2;
  return -1;
}

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

std::string ResultPayload(const Result& r, const std::string& auditId) {
  json::Value p = json::Value::Object();
  p.Set("status", json::Value::Str(r.status));
  if (!r.code.empty()) {
    json::Value e = json::Value::Object();
    e.Set("code", json::Value::Str(r.code));
    e.Set("message", json::Value::Str(r.message.empty() ? r.code : r.message));
    p.Set("error", std::move(e));
  }
  if (!auditId.empty()) p.Set("audit_id", json::Value::Str(auditId));
  return json::Dump(p);
}

}  // namespace fleet::cmds

#include "readyup/server_settings.h"

#include <algorithm>
#include <cctype>
#include <cstdlib>

namespace readyup::settings {
namespace {

std::string Lower(std::string s) {
  for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  return s;
}

std::string Trim(const std::string& s) {
  const auto b = s.find_first_not_of(" \t\r\n");
  if (b == std::string::npos) return {};
  return s.substr(b, s.find_last_not_of(" \t\r\n") - b + 1);
}

// Cuts to at most `n` bytes without splitting a UTF-8 sequence.
std::string CutUtf8(std::string s, size_t n) {
  if (s.size() <= n) return s;
  size_t cut = n;
  while (cut > 0 && (static_cast<unsigned char>(s[cut]) & 0xC0) == 0x80) --cut;
  s.resize(cut);
  return s;
}

}  // namespace

const std::vector<SettingInfo>& Table() {
  static const std::vector<SettingInfo> k = {
      {"minimum_ready_required", Kind::Int, "0", 0, 32, "min_players_to_ready", ".readyrequired",
       "players a team needs ready to go live (0 = a full team: players_per_team, else 5)"},
      {"playout_enabled_default", Kind::Bool, "0", 0, 1, nullptr, ".playout",
       "play every round of the map, no clinch (mp_match_can_clinch 0)"},
      {"autoready_enabled", Kind::Bool, "0", 0, 1, nullptr, nullptr,
       "match roster players are READY as soon as they join their team"},
      {"knife_enabled_default", Kind::Bool, "1", 0, 1, "scrim_knife", ".roundknife",
       "knife round for scrims and for match maps the config gives no side"},
      {"reset_cvars_on_series_end", Kind::Bool, "1", 0, 1, nullptr, nullptr,
       "reset the warmup / team-name cvars when a series ends"},
      {"use_pause_command_for_tactical_pause", Kind::Bool, "0", 0, 1, nullptr, nullptr,
       ".pause / .p call a tactical timeout (0: a technical pause)"},
      {"hostname_format", Kind::Str, "", 0, 127, nullptr, nullptr,
       "hostname while a match is loaded: {TEAM1} {TEAM2} {MATCH_ID} {MAP} {MAPNUMBER} {TEAM1_SCORE} "
       "{TEAM2_SCORE} {TEAM1_SERIES} {TEAM2_SERIES} (empty = off)"},
      {"kick_when_no_match_loaded", Kind::Bool, "0", 0, 1, nullptr, nullptr,
       "kick non-admins while no match is loaded (not in practice)"},
      {"whitelist_enabled_default", Kind::Bool, "1", 0, 1, nullptr, ".whitelist",
       "only the roster, spectators and admins may join a loaded match"},
      {"scrim_when_idle", Kind::Bool, "1", 0, 1, nullptr, nullptr,
       "players joining an idle server start a scrim warmup (0: idle until .ru mode scrim or a match)"},
      {"chat_prefix", Kind::Str, "", 0, 64, nullptr, nullptr,
       "Ready Up's chat prefix, <Color> tokens (empty = readyup.cfg chat_prefix)"},
      {"admin_chat_prefix", Kind::Str, "", 0, 64, "admin_prefix", nullptr,
       "chat name prefix of admins, <Color> tokens (empty = [Admin])"},
      {"roster_names_enabled", Kind::Bool, "1", 0, 1, nullptr, nullptr,
       "roster players play under their match roster names (match `roster_names` wins)"},
  };
  return k;
}

const SettingInfo* Find(const std::string& name) {
  std::string n = Lower(Trim(name));
  if (n.rfind("ru_", 0) == 0) n.erase(0, 3);
  for (const auto& s : Table()) {
    if (n == s.name) return &s;
  }
  return nullptr;
}

const SettingInfo* FindChat(const std::string& chatCommand) {
  std::string c = Lower(chatCommand);
  if (c == ".rk") c = ".roundknife";  // MatchZy / Get5 short name
  for (const auto& s : Table()) {
    if (s.chat && c == s.chat) return &s;
  }
  return nullptr;
}

std::string ConsoleName(const SettingInfo& s) { return std::string("ru_") + s.name; }
std::string PersistKey(const SettingInfo& s) { return ConsoleName(s); }

bool Normalize(const SettingInfo& s, const std::string& in, std::string* out, std::string* err) {
  std::string v = Trim(in);
  if (v.size() >= 2 && v.front() == '"' && v.back() == '"') v = v.substr(1, v.size() - 2);
  switch (s.kind) {
    case Kind::Bool: {
      const std::string l = Lower(v);
      if (l == "1" || l == "true" || l == "on" || l == "yes") *out = "1";
      else if (l == "0" || l == "false" || l == "off" || l == "no") *out = "0";
      else {
        if (err) *err = std::string(s.name) + " takes 0 or 1 (on / off)";
        return false;
      }
      return true;
    }
    case Kind::Int: {
      bool digits = !v.empty() && v.size() <= 9;
      for (unsigned char c : v) digits = digits && std::isdigit(c) != 0;
      const long long n = digits ? std::atoll(v.c_str()) : -1;
      if (!digits || n < s.min || n > s.max) {
        if (err) *err = std::string(s.name) + " takes a number from " + std::to_string(s.min) + " to " + std::to_string(s.max);
        return false;
      }
      *out = std::to_string(n);
      return true;
    }
    case Kind::Str: {
      for (unsigned char c : v) {
        if (c < 0x20 || c == 0x7F) {
          if (err) *err = std::string(s.name) + ": no control characters";
          return false;
        }
      }
      if (v.size() > static_cast<size_t>(s.max)) {
        if (err) *err = std::string(s.name) + ": at most " + std::to_string(s.max) + " bytes";
        return false;
      }
      *out = v;
      return true;
    }
  }
  return false;
}

bool ToggleValue(const SettingInfo& s, const std::string& current, const std::string& arg, std::string* out,
                 std::string* err) {
  if (Trim(arg).empty()) {
    if (s.kind != Kind::Bool) {
      if (err) *err = std::string("usage: ") + (s.chat ? s.chat : s.name) + " <value>";
      return false;
    }
    *out = current == "1" ? "0" : "1";
    return true;
  }
  return Normalize(s, arg, out, err);
}

std::string Display(const SettingInfo& s, const std::string& value) {
  switch (s.kind) {
    case Kind::Bool: return value == "1" ? "on" : "off";
    case Kind::Int: return value;
    case Kind::Str: return value.empty() ? "(empty)" : "\"" + value + "\"";
  }
  return value;
}

// ---- Store -----------------------------------------------------------------------------------

bool Store::Set(const std::string& name, const std::string& value, std::string* err) {
  const SettingInfo* s = Find(name);
  if (!s) {
    if (err) *err = "unknown setting \"" + name.substr(0, 48) + "\"";
    return false;
  }
  std::string v;
  if (!Normalize(*s, value, &v, err)) return false;
  PersistFn fn = nullptr;
  {
    std::lock_guard<std::mutex> lk(mu_);
    runtime_[s->name] = v;
    fn = persist_;
  }
  // The store reads "" as "not set": an empty string is saved quoted (persisted_settings.cpp).
  if (fn) fn(s->name, v.empty() ? std::string("\"\"") : v);
  return true;
}

bool Store::Clear(const std::string& name) {
  const SettingInfo* s = Find(name);
  if (!s) return false;
  PersistFn fn = nullptr;
  {
    std::lock_guard<std::mutex> lk(mu_);
    runtime_.erase(s->name);
    fn = persist_;
  }
  if (fn) fn(s->name, std::nullopt);
  return true;
}

void Store::LoadRuntime(const std::string& name, const std::string& value) {
  const SettingInfo* s = Find(name);
  if (!s) return;
  std::string v;
  if (!Normalize(*s, value == "\"\"" ? std::string() : value, &v, nullptr)) return;
  std::lock_guard<std::mutex> lk(mu_);
  runtime_[s->name] = v;
}

void Store::SetFileValues(const std::map<std::string, std::string>& values) {
  std::map<std::string, std::string> f;
  for (const auto& s : Table()) {
    auto it = values.find(s.name);
    if (it == values.end() && s.cfgKey) it = values.find(s.cfgKey);
    if (it == values.end()) continue;
    std::string v;
    if (Normalize(s, it->second, &v, nullptr)) f[s.name] = v;
  }
  std::lock_guard<std::mutex> lk(mu_);
  file_ = std::move(f);
}

void Store::Reset() {
  std::lock_guard<std::mutex> lk(mu_);
  runtime_.clear();
  file_.clear();
  persist_ = nullptr;
}

std::string Store::Get(const std::string& name) const {
  const SettingInfo* s = Find(name);
  if (!s) return {};
  std::lock_guard<std::mutex> lk(mu_);
  if (auto it = runtime_.find(s->name); it != runtime_.end()) return it->second;
  if (auto it = file_.find(s->name); it != file_.end()) return it->second;
  return s->builtin;
}

std::string Store::Source(const std::string& name) const {
  const SettingInfo* s = Find(name);
  if (!s) return {};
  std::lock_guard<std::mutex> lk(mu_);
  if (runtime_.count(s->name)) return "runtime";
  if (file_.count(s->name)) return "cfg";
  return "default";
}

std::optional<std::string> Store::Runtime(const std::string& name) const {
  const SettingInfo* s = Find(name);
  if (!s) return std::nullopt;
  std::lock_guard<std::mutex> lk(mu_);
  if (auto it = runtime_.find(s->name); it != runtime_.end()) return it->second;
  return std::nullopt;
}

void Store::SetPersistHook(PersistFn fn) {
  std::lock_guard<std::mutex> lk(mu_);
  persist_ = fn;
}

Store& Global() {
  static Store s;
  return s;
}

bool Bool(const char* name) { return Global().Get(name) == "1"; }
int Int(const char* name) { return std::atoi(Global().Get(name).c_str()); }
std::string Str(const char* name) { return Global().Get(name); }

std::vector<std::string> ShowLines(const Store& store) {
  std::vector<std::string> out;
  for (const auto& s : Table()) {
    const std::string src = store.Source(s.name);
    out.push_back(std::string(s.name) + " = " + Display(s, store.Get(s.name)) +
                  (src == "default" ? std::string() : " (" + src + ")"));
  }
  return out;
}

// ---- hostname --------------------------------------------------------------------------------

std::string ExpandHostname(const std::string& format, const HostnameVars& v) {
  const std::pair<const char*, std::string> tokens[] = {
      {"{TEAM1_SCORE}", std::to_string(v.team1Score)}, {"{TEAM2_SCORE}", std::to_string(v.team2Score)},
      {"{TEAM1_SERIES}", std::to_string(v.team1Series)}, {"{TEAM2_SERIES}", std::to_string(v.team2Series)},
      {"{TEAM1}", v.team1},       {"{TEAM2}", v.team2},
      {"{MATCH_ID}", v.matchId},  {"{MAPNUMBER}", std::to_string(v.mapNumber)},
      {"{MAP}", v.map},
  };
  std::string out;
  size_t i = 0;
  while (i < format.size()) {
    bool hit = false;
    if (format[i] == '{') {
      for (const auto& t : tokens) {
        const size_t n = std::char_traits<char>::length(t.first);
        if (i + n <= format.size() && Lower(format.substr(i, n)) == Lower(t.first)) {
          out += t.second;
          i += n;
          hit = true;
          break;
        }
      }
    }
    if (!hit) out += format[i++];
  }
  std::string clean;
  for (char c : out) {
    const unsigned char u = static_cast<unsigned char>(c);
    if (u < 0x20 || u == 0x7F || c == '"' || c == ';' || c == '\\') continue;
    clean += c;
  }
  return CutUtf8(Trim(clean), 127);
}

}  // namespace readyup::settings

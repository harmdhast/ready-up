#include "readyup/config.h"

#include "readyup/chat_colors.h"
#include "readyup/engine.h"
#include "readyup/host.h"
#include "readyup/logging.h"
#include "readyup/ruleset.h"
#include "readyup/server_settings.h"

#include <sys/stat.h>

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cstdlib>
#include <fstream>
#include <mutex>
#include <string>
#include <vector>

namespace readyup {
namespace {

struct CfgState {
  std::mutex mu;
  ReadyUpCfg cfg;
  bool loaded = false;
  std::string sig;  // mtimes/sizes of the sources at the last load
  double lastCheck = 0;
};

CfgState& State() {
  static CfgState st;
  return st;
}

std::string Trim(std::string s) {
  while (!s.empty() && (s.back() == '\r' || s.back() == '\n' || s.back() == ' ' || s.back() == '\t')) s.pop_back();
  size_t i = 0;
  while (i < s.size() && (s[i] == ' ' || s[i] == '\t')) i++;
  if (i) s.erase(0, i);
  return s;
}

std::string Lower(std::string s) {
  for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  return s;
}

std::string Unquote(std::string s) {
  if (s.size() < 2) return s;
  const char q = s.front();
  if ((q == '"' || q == '\'') && s.back() == q) {
    s.erase(s.begin());
    s.pop_back();
  }
  return s;
}

bool ParseBool(const std::string& v, bool defaultValue) {
  if (v.empty()) return defaultValue;
  const char c = v[0];
  if (c == '0' || c == 'n' || c == 'N' || c == 'f' || c == 'F') return false;
  if (c == '1' || c == 'y' || c == 'Y' || c == 't' || c == 'T') return true;
  return defaultValue;
}

int ParseInt(const std::string& v, int defaultValue) {
  if (v.empty()) return defaultValue;
  const long long x = std::strtoll(v.c_str(), nullptr, 10);
  if (x <= 0 || x > 65535) return defaultValue;
  return static_cast<int>(x);
}

// -1 (unset) for an empty or non-numeric value, else 0..100000.
int RuleInt(const std::string& v) {
  if (v.empty() || !std::isdigit(static_cast<unsigned char>(v[0]))) return -1;
  return static_cast<int>(std::min(100000LL, std::strtoll(v.c_str(), nullptr, 10)));
}

std::string ExpandChatColorTokens(std::string s) {
  struct Tok {
    const char* token;
    char value;
  };
  static constexpr Tok kToks[] = {
      {"<Default>", ChatColors::Default},     {"<White>", ChatColors::White},
      {"<DarkRed>", ChatColors::DarkRed},     {"<Green>", ChatColors::Green},
      {"<LightYellow>", ChatColors::LightYellow}, {"<LightBlue>", ChatColors::LightBlue},
      {"<Olive>", ChatColors::Olive},         {"<Lime>", ChatColors::Lime},
      {"<Red>", ChatColors::Red},             {"<LightPurple>", ChatColors::LightPurple},
      {"<Purple>", ChatColors::Purple},       {"<Grey>", ChatColors::Grey},
      {"<Yellow>", ChatColors::Yellow},       {"<Gold>", ChatColors::Gold},
      {"<Silver>", ChatColors::Silver},       {"<Blue>", ChatColors::Blue},
      {"<DarkBlue>", ChatColors::DarkBlue},   {"<BlueGrey>", ChatColors::BlueGrey},
      {"<Magenta>", ChatColors::Magenta},     {"<LightRed>", ChatColors::LightRed},
      {"<Orange>", ChatColors::Orange},
  };
  for (const auto& t : kToks) {
    const std::string needle = t.token;
    size_t pos = 0;
    while ((pos = s.find(needle, pos)) != std::string::npos) {
      s.replace(pos, needle.size(), std::string(1, t.value));
      pos += 1;
    }
  }
  return s;
}

ReadyUpCfg DefaultCfg() {
  ReadyUpCfg c;
  c.admin_prefix = std::string(1, ChatColors::DarkRed) + "[Admin]" + std::string(1, ChatColors::Default);
  c.captain_prefix_team1 = std::string(1, ChatColors::LightBlue) + "[CAP]" + std::string(1, ChatColors::Default);
  c.captain_prefix_team2 = std::string(1, ChatColors::Orange) + "[CAP]" + std::string(1, ChatColors::Default);
  return c;
}

void Apply(ReadyUpCfg* out, const std::string& key, const std::string& val) {
  // Server settings (server_settings.h) keep the raw text; the store validates it. The legacy keys
  // (min_players_to_ready, scrim_knife) are read below as well.
  if (settings::Find(key) && key.rfind("ru_", 0) != 0) out->settings[key] = val;
  for (const auto& s : settings::Table()) {
    if (s.cfgKey && key == s.cfgKey) out->settings[key] = val;
  }
  if (key == "welcome") out->welcome = ParseBool(val, out->welcome);
  else if (key == "welcome_show_seconds") out->welcome_show_seconds = std::clamp(std::atoi(val.c_str()), 2, 60);
  else if (key == "welcome_round_delay_ms") out->welcome_round_delay_ms = std::clamp(std::atoi(val.c_str()), 0, 30000);
  else if (key == "golive_card_seconds") out->golive_card_seconds = std::clamp(std::atoi(val.c_str()), 0, 30);
  else if (key == "admin_call_cooldown_s") out->admin_call_cooldown_s = std::clamp(std::atoi(val.c_str()), 0, 3600);
  else if (key == "ready_hud") out->ready_hud = ParseBool(val, out->ready_hud);
  else if (key == "hud_steady") out->hud_steady = ParseBool(val, out->hud_steady);
  else if (key == "hud_tick_ms") out->hud_tick_ms = std::max(0, std::atoi(val.c_str()));
  else if (key == "hud_resend_ms") out->hud_resend_ms = std::max(0, std::atoi(val.c_str()));
  else if (key == "hud_knife_hold_s") out->hud_knife_hold_s = std::clamp(std::atoi(val.c_str()), 0, 300);
  else if (key == "hud_duration_s") out->hud_duration_s = std::clamp(std::atoi(val.c_str()), 1, 10);
  else if (key == "hud_brand") out->hud_brand = val;
  else if (key == "hud_logo_url") out->hud_logo_url = val;
  else if (key == "chat_debug") out->chat_debug = ParseBool(val, out->chat_debug);
  else if (key == "admin_prefix") out->admin_prefix = ExpandChatColorTokens(val);
  else if (key == "captain_prefix_team1") out->captain_prefix_team1 = ExpandChatColorTokens(val);
  else if (key == "captain_prefix_team2") out->captain_prefix_team2 = ExpandChatColorTokens(val);
  else if (key == "consume_ready_chat") out->consume_ready_chat = ParseBool(val, out->consume_ready_chat);
  else if (key == "dev_bots_ready") out->dev_bots_ready = ParseBool(val, out->dev_bots_ready);
  else if (key == "dev_bots_scrim") out->dev_bots_scrim = ParseBool(val, out->dev_bots_scrim);
  else if (key == "warmup_money") out->warmup_money = ParseBool(val, out->warmup_money);
  else if (key == "warmup_weapon_cleanup") out->warmup_weapon_cleanup = ParseBool(val, out->warmup_weapon_cleanup);
  else if (key == "knife_pick_seconds") out->knife_pick_seconds = ParseInt(val, out->knife_pick_seconds);
  else if (key == "idle_map_refresh_hours") {
    const int h = RuleInt(val);  // 0 = off; anything unreadable keeps the default
    if (h >= 0) out->idle_map_refresh_hours = std::min(h, 720);
  }
  else if (key == "backup_keep_hours" || key == "demo_keep_hours") {
    const int h = RuleInt(val);  // 0 = keep forever; anything unreadable keeps the default
    if (h >= 0) (key == "backup_keep_hours" ? out->backup_keep_hours : out->demo_keep_hours) = std::min(h, 87600);
  }
  // Rules: 0 is a real value (unlimited / off), so no ParseInt here.
  else if (key == "max_tech_pauses_per_team") out->rules.tech_pauses_per_team = RuleInt(val);
  else if (key == "tech_pause_max_seconds") out->rules.tech_pause_max_seconds = RuleInt(val);
  else if (key == "both_teams_unpause_required") out->rules.both_teams_unpause = ParseBool(val, true) ? 1 : 0;
  else if (key == "allow_force_ready") out->rules.allow_force_ready = ParseBool(val, true) ? 1 : 0;
  else if (key == "min_players_to_ready") out->rules.min_players_to_ready = RuleInt(val);
  else if (key == "forfeit_after_seconds") out->rules.forfeit_after_seconds = RuleInt(val);
  else if (key == "gg_enabled") out->rules.gg_enabled = ParseBool(val, false) ? 1 : 0;
  else if (key == "gg_threshold") out->rules.gg_threshold_pct = GgThresholdPctFromText(val);
  else if (key == "gg_min_score_diff") out->rules.gg_min_score_diff = RuleInt(val);
  else if (key == "stop_command_available") out->rules.stop_command_available = ParseBool(val, false) ? 1 : 0;
  else if (key == "stop_command_no_damage") out->rules.stop_command_no_damage = ParseBool(val, false) ? 1 : 0;
  else if (key == "stop_vote_seconds") out->rules.stop_vote_seconds = RuleInt(val);
  else if (key == "pause_after_restore") out->rules.pause_after_restore = ParseBool(val, true) ? 1 : 0;
  else if (key == "damage_report") out->damage_report = ParseBool(val, out->damage_report);
  else if (key == "ruleset") out->ruleset = Lower(val);
  else if (key == "default_model_ct" || key == "default_model_t") {
    // A model path ("agents/models/.../x.vmdl"); anything else keeps the built-in default.
    const bool ok = val.size() > 5 && val.compare(val.size() - 5, 5, ".vmdl") == 0 &&
                    val.find_first_of("\"; \t") == std::string::npos;
    if (ok) (key == "default_model_ct" ? out->default_model_ct : out->default_model_t) = val;
  }
}

// `sections`: which parts of the file count ("" = top level, "match" = [match]).
bool ReadFile(const std::string& path, const std::vector<std::string>& sections, ReadyUpCfg* out) {
  std::ifstream f(path);
  if (!f.good()) return false;
  std::string line, cur;
  while (std::getline(f, line)) {
    line = Trim(line);
    if (line.empty() || line[0] == '#' || line.rfind("//", 0) == 0) continue;
    if (line.front() == '[' && line.back() == ']') {
      cur = Lower(Trim(line.substr(1, line.size() - 2)));
      continue;
    }
    if (std::find(sections.begin(), sections.end(), cur) == sections.end()) continue;
    const size_t eq = line.find('=');
    if (eq == std::string::npos) continue;
    Apply(out, Lower(Trim(line.substr(0, eq))), Unquote(Trim(line.substr(eq + 1))));
  }
  return true;
}

std::string CorePath() {
  const std::string d = GetThisModuleDir();
  return d.empty() ? std::string() : d + "/readyup.cfg";
}

std::string MatchCfgPath() {
  const std::string c = GetCsgoDirFromModuleDir();
  return c.empty() ? std::string() : c + "/cfg/ReadyUp/match.cfg";
}

std::string Signature() {
  std::string sig;
  for (const std::string& p : {CorePath(), MatchCfgPath()}) {
    struct stat st {};
    if (!p.empty() && stat(p.c_str(), &st) == 0) {
      sig += std::to_string(static_cast<long long>(st.st_mtim.tv_sec)) + "." +
             std::to_string(static_cast<long long>(st.st_mtim.tv_nsec)) + ":" +
             std::to_string(static_cast<long long>(st.st_size));
    }
    sig += ";";
  }
  return sig;
}

// Must hold st.mu.
bool LoadLocked(CfgState& st, std::string* err) {
  ReadyUpCfg c = DefaultCfg();
  const std::string core = CorePath();
  // Top level first (legacy location), then [match] in the same file.
  const bool coreOk = !core.empty() && ReadFile(core, {"", "match"}, &c);
  const std::string m = MatchCfgPath();
  if (!m.empty()) (void)ReadFile(m, {"", "match"}, &c);
  Ruleset rs = Ruleset::Default;
  if (!ParseRuleset(c.ruleset, &rs)) {
    Print("config: ruleset=\"%s\" is not default|valve; using default\n", c.ruleset.c_str());
    c.ruleset = "default";
  }
  SetServerRuleset(rs);
  settings::Global().SetFileValues(c.settings);
  st.cfg = std::move(c);
  st.loaded = true;
  st.sig = Signature();
  if (!coreOk && err) *err = "could not open: " + (core.empty() ? std::string("readyup.cfg") : core);
  return coreOk;
}

void EnsureLoaded(CfgState& st) {
  if (!st.loaded) (void)LoadLocked(st, nullptr);
}

bool EnvBool(const char* name, bool* out) {
  const char* v = std::getenv(name);
  if (!v || !*v) return false;
  *out = !(v[0] == '0' || v[0] == 'n' || v[0] == 'N' || v[0] == 'f' || v[0] == 'F');
  return true;
}

std::atomic<int> g_devBotsScrimOverride{-1};

}  // namespace

ReadyUpCfg Cfg() {
  auto& st = State();
  std::lock_guard<std::mutex> lk(st.mu);
  EnsureLoaded(st);
  return st.cfg;
}

bool ReloadCfg(std::string* err) {
  auto& st = State();
  bool ok = false;
  {
    std::lock_guard<std::mutex> lk(st.mu);
    ok = LoadLocked(st, err);
  }
  (void)DevBotsReadyEnabled();  // log a flip
  (void)DevBotsScrimEnabled();
  return ok;
}

bool MaybeReloadCfg() {
  auto& st = State();
  const double now = host::NowSeconds();
  {
    std::lock_guard<std::mutex> lk(st.mu);
    if (st.loaded && now - st.lastCheck < 1.0) return false;
    st.lastCheck = now;
    if (st.loaded && Signature() == st.sig) return false;
  }
  (void)ReloadCfg(nullptr);
  Debug("match: settings (re)loaded from readyup.cfg / cfg/ReadyUp/match.cfg\n");
  return true;
}

bool DebugEnabled() {
  const ru_api* a = host::Api();
  if (a) return a->debug_enabled(a->self) != 0;
  bool on = false;
  return EnvBool("READYUP_DEBUG", &on) && on;
}

bool ChatDebugEnabled() {
  if (!DebugEnabled()) return false;
  bool on = false;
  if (EnvBool("READYUP_CHAT_DEBUG", &on)) return on;
  return Cfg().chat_debug;
}

std::string AdminPrefix() {
  // The server setting admin_chat_prefix (runtime: console / chat / fleet server.config; file:
  // this file's admin_prefix key), else the built-in [Admin].
  const std::string set = settings::Str("admin_chat_prefix");
  if (!set.empty()) return ExpandChatColorTokens(set);
  auto c = Cfg();
  return c.admin_prefix.empty() ? DefaultCfg().admin_prefix : c.admin_prefix;
}

std::string CaptainPrefixTeam1() {
  auto c = Cfg();
  return c.captain_prefix_team1.empty() ? DefaultCfg().captain_prefix_team1 : c.captain_prefix_team1;
}

std::string CaptainPrefixTeam2() {
  auto c = Cfg();
  return c.captain_prefix_team2.empty() ? DefaultCfg().captain_prefix_team2 : c.captain_prefix_team2;
}

bool ConsumeReadyChat() { return Cfg().consume_ready_chat; }

bool DevBotsReadyEnabled() {
  bool on = false;
  if (!EnvBool("READYUP_DEV_BOTS_READY", &on)) on = Cfg().dev_bots_ready;
  static std::atomic<int> s_last{-1};
  const int cur = on ? 1 : 0;
  const int prev = s_last.exchange(cur);
  if (prev != cur) {
    if (on) PrintLine("WARNING: dev_bots_ready is ON — bots count as ready (debug only; do not use for real matches).");
    else if (prev == 1) PrintLine("dev_bots_ready is OFF.");
  }
  return on;
}

void SetDevBotsScrimOverride(int v) {
  g_devBotsScrimOverride.store(v < 0 ? -1 : (v ? 1 : 0));
  (void)DevBotsScrimEnabled();  // logs the flip
}

int DevBotsScrimOverride() { return g_devBotsScrimOverride.load(); }

bool DevBotsScrimEnabled() {
  bool on = false;
  const int ov = g_devBotsScrimOverride.load();
  if (ov >= 0) on = ov == 1;
  else if (!EnvBool("READYUP_DEV_BOTS_SCRIM", &on)) on = Cfg().dev_bots_scrim;
  static std::atomic<int> s_last{-1};
  const int cur = on ? 1 : 0;
  const int prev = s_last.exchange(cur);
  if (prev != cur) {
    if (on) {
      PrintLine("WARNING: dev_bots_scrim is ON — a scrim starts and runs with only bots on CT and T "
                "(testing only; never on a real server).");
    } else if (prev == 1) {
      PrintLine("dev_bots_scrim is OFF.");
    }
  }
  return on;
}

}  // namespace readyup

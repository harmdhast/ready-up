#include "readyup/match_settings.h"

#include "readyup/admin_check.h"
#include "readyup/engine.h"
#include "readyup/esports.h"
#include "readyup/fleet_bridge.h"
#include "readyup/host.h"
#include "readyup/local_store.h"
#include "readyup/logging.h"
#include "readyup/match_console.h"
#include "readyup/match_end.h"
#include "readyup/match_router.h"
#include "readyup/match_state.h"
#include "readyup/modes.h"
#include "readyup/players.h"
#include "readyup/round_restore.h"
#include "readyup/server_settings.h"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <mutex>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace readyup::match_settings {
namespace {

using settings::Global;
using settings::SettingInfo;

std::string Trim(const std::string& s) {
  const auto b = s.find_first_not_of(" \t\r\n");
  if (b == std::string::npos) return {};
  return s.substr(b, s.find_last_not_of(" \t\r\n") - b + 1);
}

std::string Lower(std::string s) {
  for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  return s;
}

void Persist(const std::string& name, const std::optional<std::string>& value) {
  local_store::SetSetting("ru_" + name, value);
}

// The console settings of match_end.h reachable through `.ru settings set` / fleet as well.
const char* const kKickDelays[] = {"series_end_kick_delay_no_demo", "series_end_kick_delay_demo_no_upload",
                                   "series_end_kick_delay_demo_upload"};

int KickDelayIndex(const std::string& name) {
  std::string n = Lower(Trim(name));
  if (n.rfind("ru_", 0) == 0) n.erase(0, 3);
  for (int i = 0; i < 3; ++i) {
    if (n == kKickDelays[i]) return i;
  }
  return -1;
}

int KickDelay(int i) {
  int k[3] = {0, 0, 0};
  GetSeriesEndKickDelays(&k[0], &k[1], &k[2]);
  return k[i];
}

std::string Describe(const SettingInfo& s) {
  const std::string src = Global().Source(s.name);
  return std::string(s.name) + " = " + settings::Display(s, Global().Get(s.name)) +
         (src == "default" ? std::string() : " (" + src + ")");
}

// Effects that cannot wait for the next go-live.
void OnChanged(const SettingInfo& s) {
  const std::string n = s.name;
  if (n == "playout_enabled_default" && GetMode() == ReadyUpMode::MatchLive) {
    const auto ctx = WebhookGetMatchContext();
    if (ctx && !ctx->cvars.count("mp_match_can_clinch")) {
      (void)EnqueueServerCommand(PlayoutOn(&*ctx) ? "mp_match_can_clinch 0" : "mp_match_can_clinch 1");
    }
  }
}

// ---- tick state (game thread) ----------------------------------------------------------------

double g_nextTick = 0;
std::unordered_set<uint64_t> g_autoReadied;
std::string g_autoKey;  // "<matchid>:<map>:<mode>": auto-ready starts over when it changes
std::unordered_map<uint64_t, double> g_lastKick;
std::mutex g_hostMu;
std::string g_baseHostname;  // the operator's (seen on the console)
std::string g_lastHostname;  // the last one Ready Up set ("" = none)

void AutoReadyTick() {
  const auto ctx = WebhookGetMatchContext();
  const ReadyUpMode mode = GetMode();
  const bool on = ctx && ctx->slug != "scrim" && AutoreadyOn(*ctx) && mode == ReadyUpMode::MatchWarmup &&
                  !GoLiveTriggered() && !KnifeIsAwaitingPick() && !FullRosterGateEnabled();
  const std::string key = ctx ? std::to_string(ctx->matchid) + ":" + std::to_string(MatchStateGet().map_number) + ":" +
                                    GetModeString()
                              : std::string();
  if (key != g_autoKey) {
    g_autoKey = key;
    g_autoReadied.clear();
  }
  if (!on) return;
  std::unordered_set<uint64_t> present;
  for (const auto& h : ListHumans()) {
    if (h.steamid64 == 0) continue;
    present.insert(h.steamid64);
    if (h.team != 2 && h.team != 3) continue;  // on their side first (team enforcement moves them)
    if (!ctx->roster_team.count(h.steamid64) || g_autoReadied.count(h.steamid64)) continue;
    g_autoReadied.insert(h.steamid64);  // once per connection: a player's own .unready sticks
    if (!IsReady(h.steamid64)) {
      Print("settings: autoready: %s is ready\n", h.name.c_str());
      MatchChatCommand(h.steamid64, h.name, ".ready");
    }
  }
  for (auto it = g_autoReadied.begin(); it != g_autoReadied.end();) {
    it = present.count(*it) ? std::next(it) : g_autoReadied.erase(it);
  }
}

void NoMatchKickTick(double now) {
  if (!settings::Bool("kick_when_no_match_loaded")) return;
  const auto ctx = WebhookGetMatchContext();
  if (ctx && ctx->slug != "scrim") return;
  const ReadyUpMode mode = GetMode();
  if (mode == ReadyUpMode::Practice || mode == ReadyUpMode::External) return;  // an admin chose it
  if (fleet_bridge::Assigned()) return;  // a fleet match is loading
  for (const auto& h : ListHumans()) {
    if (h.steamid64 == 0 || h.userid < 0 || IsReadyUpAdmin(h.steamid64)) continue;
    auto it = g_lastKick.find(h.steamid64);
    if (it != g_lastKick.end() && now - it->second < 5.0) continue;
    g_lastKick[h.steamid64] = now;
    Print("settings: kick_when_no_match_loaded: kicking %s\n", h.name.c_str());
    (void)EnqueueServerCommand(("kickid " + std::to_string(h.userid) + " \"No match is loaded on this server\"").c_str());
  }
}

void HostnameTick() {
  const std::string fmt = settings::Str("hostname_format");
  const auto ctx = WebhookGetMatchContext();
  std::string want;
  if (!fmt.empty() && ctx && ctx->slug != "scrim") {
    const auto ms = MatchStateGet();
    settings::HostnameVars v;
    v.team1 = ctx->team1_name;
    v.team2 = ctx->team2_name;
    v.matchId = ctx->slug.empty() ? std::to_string(ctx->matchid) : ctx->slug;
    v.map = ms.current_map;
    v.mapNumber = ms.map_number;
    v.team1Score = ms.team1_score;
    v.team2Score = ms.team2_score;
    const auto series = ModesGetSeriesWins();
    v.team1Series = series.first;
    v.team2Series = series.second;
    want = settings::ExpandHostname(fmt, v);
  }
  std::string cmd;
  {
    std::lock_guard<std::mutex> lk(g_hostMu);
    if (!want.empty()) {
      if (want == g_lastHostname) return;
      g_lastHostname = want;
      cmd = "hostname \"" + want + "\"";
    } else if (!g_lastHostname.empty()) {
      // The match unloaded (or the format was cleared): the operator's hostname again.
      g_lastHostname.clear();
      if (!g_baseHostname.empty()) cmd = "hostname \"" + g_baseHostname + "\"";
    }
  }
  if (!cmd.empty()) {
    Debug("settings: %s\n", cmd.c_str());
    (void)EnqueueServerCommand(cmd.c_str());
  }
}

// chat_prefix: the core owns the chat prefix (readyup.cfg); a value set here (console, chat,
// fleet server.config, match.cfg) is pushed to it with ru_api set_core_setting, "" hands it back.
std::string g_pushedChatPrefix;
bool g_chatPrefixPushed = false;

void ChatPrefixTick() {
  const std::string want = Global().Get("chat_prefix");
  if (g_chatPrefixPushed && want == g_pushedChatPrefix) return;
  const ru_api* a = host::Api();
  if (!a || !RU_API_HAS(a, set_core_setting) || !a->set_core_setting) return;
  g_chatPrefixPushed = true;
  g_pushedChatPrefix = want;
  if (!a->set_core_setting(a->self, "chat_prefix", want.c_str())) {
    Print("settings: the core refused chat_prefix \"%s\"\n", want.c_str());
  }
}

}  // namespace

void Install() {
  Global().SetPersistHook(&Persist);
  int n = 0;
  for (const auto& s : settings::Table()) {
    if (auto v = local_store::GetSetting(settings::PersistKey(s))) {
      Global().LoadRuntime(s.name, *v);
      ++n;
    }
  }
  if (n > 0) Print("match: restored %d server setting(s) from state.json\n", n);
  g_autoReadied.clear();
  g_autoKey.clear();
  g_lastKick.clear();
  g_chatPrefixPushed = false;
  g_pushedChatPrefix.clear();
}

const std::vector<std::string>& ConsoleCommands() {
  static const std::vector<std::string> k = [] {
    std::vector<std::string> v;
    for (const auto& s : settings::Table()) v.push_back(settings::ConsoleName(s));
    return v;
  }();
  return k;
}

bool ConsoleCommand(const std::string& line) {
  const std::string t = Trim(line);
  const size_t sp = t.find_first_of(" \t");
  const std::string cmd = t.substr(0, sp);
  if (cmd.rfind("ru_", 0) != 0) return false;
  const SettingInfo* s = settings::Find(cmd);
  if (!s) return false;
  const std::string arg = sp == std::string::npos ? std::string() : Trim(t.substr(sp));
  std::string reply;
  if (arg.empty()) {
    Print("%s (%s)\n", Describe(*s).c_str(), s->help);
    return true;
  }
  if (Lower(arg) == "default") (void)SetDefault(s->name, "console", &reply);
  else (void)Set(s->name, arg, "console", &reply);
  PrintLine(reply.c_str());
  return true;
}

bool Validate(const std::string& name, const std::string& value, std::string* err) {
  if (const int k = KickDelayIndex(name); k >= 0) {
    const std::string v = Trim(value);
    bool digits = !v.empty() && v.size() <= 5;
    for (unsigned char c : v) digits = digits && std::isdigit(c) != 0;
    if (!digits || std::atoi(v.c_str()) > 3600) {
      if (err) *err = std::string(kKickDelays[k]) + " takes seconds from 0 to 3600";
      return false;
    }
    return true;
  }
  const SettingInfo* s = settings::Find(name);
  if (!s) {
    if (err) *err = "unknown setting \"" + Trim(name).substr(0, 48) + "\" (.ru settings show)";
    return false;
  }
  std::string v;
  return settings::Normalize(*s, value, &v, err);
}

bool Set(const std::string& name, const std::string& value, const std::string& by, std::string* reply) {
  std::string err;
  if (!Validate(name, value, &err)) {
    if (reply) *reply = err;
    return false;
  }
  if (const int k = KickDelayIndex(name); k >= 0) {
    const std::string v = Trim(value);
    // Through the console setting: saved in state.json like `ru_series_end_kick_delay_* <s>`.
    (void)MatchConsoleCommand("ru_" + std::string(kKickDelays[k]) + " " + v);
    if (reply) *reply = std::string(kKickDelays[k]) + " = " + std::to_string(KickDelay(k));
    Print("settings: %s set %s %s\n", by.c_str(), kKickDelays[k], v.c_str());
    return true;
  }
  const SettingInfo* s = settings::Find(name);
  if (!s || !Global().Set(s->name, value, &err)) {
    if (reply) *reply = err;
    return false;
  }
  Print("settings: %s set %s = %s\n", by.c_str(), s->name, Global().Get(s->name).c_str());
  OnChanged(*s);
  if (reply) *reply = Describe(*s);
  return true;
}

bool SetDefault(const std::string& name, const std::string& by, std::string* reply) {
  if (const int k = KickDelayIndex(name); k >= 0) {
    (void)MatchConsoleCommand("ru_" + std::string(kKickDelays[k]) + " default");
    if (reply) *reply = std::string(kKickDelays[k]) + " = " + std::to_string(KickDelay(k)) + " (default)";
    return true;
  }
  const SettingInfo* s = settings::Find(name);
  if (!s) {
    if (reply) *reply = "unknown setting \"" + Trim(name).substr(0, 48) + "\" (.ru settings show)";
    return false;
  }
  (void)Global().Clear(s->name);
  Print("settings: %s reset %s\n", by.c_str(), s->name);
  OnChanged(*s);
  if (reply) *reply = Describe(*s);
  return true;
}

std::vector<std::string> ShowLines() {
  std::vector<std::string> out = settings::ShowLines(Global());
  // The round restore's own setting (round_restore.h, ru_pause_after_restore), read, not copied.
  const int pauseConsole = round_restore::ConsolePauseAfterRestore();
  out.push_back(std::string("pause_after_restore = ") + (round_restore::PauseAfterRestore() ? "on" : "off") +
                (pauseConsole >= 0 ? " (runtime)" : ""));
  out.push_back("series_end_kick_delay: no_demo=" + std::to_string(KickDelay(0)) + " demo_no_upload=" +
                std::to_string(KickDelay(1)) + " demo_upload=" + std::to_string(KickDelay(2)) + " (seconds)");
  const auto ctx = WebhookGetMatchContext();
  if (ctx && ctx->slug != "scrim") {
    std::string m;
    auto add = [&](const std::string& part) { m += (m.empty() ? "" : ", ") + part; };
    if (ctx->rules.min_players_to_ready >= 0) add("minimum ready " + std::to_string(ctx->rules.min_players_to_ready));
    if (ctx->players_per_team > 0) add("players per team " + std::to_string(ctx->players_per_team));
    if (ctx->playout >= 0) add(std::string("playout ") + (ctx->playout ? "on" : "off"));
    if (ctx->whitelist >= 0) add(std::string("whitelist ") + (ctx->whitelist ? "on" : "off"));
    if (ctx->autoready >= 0) add(std::string("autoready ") + (ctx->autoready ? "on" : "off"));
    if (!m.empty()) out.push_back("this match sets: " + m);
  }
  return out;
}

bool PlayoutOn(const WebhookMatchContext* ctx) {
  if (ctx && ctx->playout >= 0) return ctx->playout != 0;
  return settings::Bool("playout_enabled_default");
}
bool WhitelistOn(const WebhookMatchContext& ctx) {
  if (ctx.whitelist >= 0) return ctx.whitelist != 0;
  return settings::Bool("whitelist_enabled_default");
}
bool AutoreadyOn(const WebhookMatchContext& ctx) {
  if (ctx.autoready >= 0) return ctx.autoready != 0;
  return settings::Bool("autoready_enabled");
}
bool ResetCvarsOnSeriesEnd() { return settings::Bool("reset_cvars_on_series_end"); }
bool PauseCommandIsTactical() { return settings::Bool("use_pause_command_for_tactical_pause"); }

bool KnifeDefault(const WebhookMatchContext* ctx) {
  if (!settings::Bool("knife_enabled_default")) return false;
  return EffectiveRulesFor(ctx).Bool("allow_knife", true);
}

void FillMissingSides(WebhookMatchContext* ctx) {
  if (!ctx || ctx->map_sides.size() >= ctx->maplist.size()) return;
  const std::string side = KnifeDefault(ctx) ? "knife" : "team1_ct";
  Print("match: %zu map(s) without a side in map_sides: %s (knife_enabled_default)\n",
        ctx->maplist.size() - ctx->map_sides.size(), side.c_str());
  ctx->map_sides.resize(ctx->maplist.size(), side);
}

void ObserveHostname(const std::string& value) {
  std::string v = Trim(value);
  if (v.size() >= 2 && v.front() == '"' && v.back() == '"') v = v.substr(1, v.size() - 2);
  for (char& c : v) {
    if (c == '"' || c == ';' || static_cast<unsigned char>(c) < 0x20) c = ' ';
  }
  std::lock_guard<std::mutex> lk(g_hostMu);
  if (v.empty() || v == g_lastHostname) return;  // Ready Up's own `hostname` line
  g_baseHostname = v;
}

void Tick(double now) {
  if (now < g_nextTick) return;
  g_nextTick = now + 0.5;
  AutoReadyTick();
  NoMatchKickTick(now);
  HostnameTick();
  ChatPrefixTick();
}

}  // namespace readyup::match_settings

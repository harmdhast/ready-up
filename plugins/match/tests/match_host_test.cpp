// match.so inside the real plugin host (core/src/readyup/plugin_loader.cpp with stubbed core
// functions and a fake engine API, like tools/plugin_host_test.cpp). No CS2 server:
//   1. load: commands, `ru` subcommands, console settings, readyup.match.v1, selftest lines
//   2. scrim warmup from the fake player registry, `.r` from chat
//   3. `ru plugin reload match` keeps mode, ready states and runtime settings
//   4. `ru match load <url>` (served by this test), reload again: the loaded match survives
//   5. unload removes everything, lifts round-termination suppression, unmaps the image
//   build/plugins/match/match_host_test <match.so>
#include "readyup/plugin_loader.h"

#include "readyup/match_iface.h"
#include "readyup/plugin_api.h"

#include <arpa/inet.h>
#include <dlfcn.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdarg>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <mutex>
#include <optional>
#include <fstream>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

static std::mutex g_logMu;
static std::vector<std::string> g_log;
static std::vector<std::string> g_chat;
static std::vector<std::string> g_cmds;

static void Capture(const char* fmt, va_list ap) {
  char stack[4096];
  va_list ap2;
  va_copy(ap2, ap);
  const int n = std::vsnprintf(stack, sizeof(stack), fmt, ap);
  std::string s;
  if (n >= static_cast<int>(sizeof(stack))) {
    s.resize(static_cast<size_t>(n) + 1);
    std::vsnprintf(&s[0], s.size(), fmt, ap2);
    s.resize(static_cast<size_t>(n));
  } else {
    s = stack;
  }
  va_end(ap2);
  while (!s.empty() && s.back() == '\n') s.pop_back();
  std::printf("  | %s\n", s.substr(0, 300).c_str());
  std::lock_guard<std::mutex> lk(g_logMu);
  g_log.push_back(s);
}

// ---- stubs for the core functions plugin_loader.cpp uses -----------------------------------
namespace readyup {
void Print(const char* fmt, ...) {
  va_list ap;
  va_start(ap, fmt);
  Capture(fmt, ap);
  va_end(ap);
}
void Debug(const char* fmt, ...) {
  va_list ap;
  va_start(ap, fmt);
  Capture(fmt, ap);
  va_end(ap);
}
void PrintLine(const char* msg) { Print("%s", msg); }
bool DebugEnabled() { return false; }
void SendToChat(const char* msg) {
  std::lock_guard<std::mutex> lk(g_logMu);
  g_chat.push_back(msg);
}
void SendRawToChat(const char* msg) { SendToChat(msg); }
bool ClientPrintChat(int, const char* msg) {
  SendToChat(msg);
  return true;
}
bool SendToSlotChat(int slot, const char* msg) { return ClientPrintChat(slot, msg); }
bool EnqueueServerCommand(const char* c) {
  std::lock_guard<std::mutex> lk(g_logMu);
  g_cmds.push_back(c);
  return true;
}
std::optional<int> GameEventsSlotForSteam(unsigned long long steamid64) {
  if (steamid64 == 76561198000000001ull) return 2;
  return std::nullopt;
}
static std::string g_csgoDir;
static std::string g_moduleDir;
static std::string g_lastCenterAll;  // essentials: the workshop download bar
static bool g_showLicense = false;    // core: readyup_show_license 1, valid key
std::string GetCsgoDirFromModuleDir() { return g_csgoDir; }
std::string GetThisModuleDir() { return g_moduleDir; }
bool IsCoreChatCommand(const std::string& t) { return t == ".ru"; }
}  // namespace readyup

// ---- fake engine API ---------------------------------------------------------------------
struct FakePlayer {
  int slot, userid;
  uint64_t steamid64;
  int team;
  bool bot;
  const char* name;
};
// player_change_team reports success but leaves the team as is (a full team, a refused move).
static bool g_refuseTeamChange = false;
// engine_feature_available("respawn"); the slots player_respawn was called for.
static bool g_respawnAvailable = true;
static std::vector<int> g_respawned;
static int g_respawnRc = 1;  // player_respawn: 1 respawned, 2 already alive, 0 refused
static std::vector<FakePlayer> g_players = {
    {2, 2, 76561198000000001ull, 3, false, "alice"},
};
static std::atomic<int> g_suppressed{-1};
// One CS2 team entity (cs_team_manager, CTeam::m_iScore) at entity index kTeamIndex. The entity
// system is up only while g_teamsUp is set.
struct FakeTeam {
  unsigned char pad[0x40];
  int32_t score;
};
static FakeTeam g_team{};
static std::atomic<bool> g_teamsUp{false};
constexpr int kTeamIndex = 70;

static void FillPlayer(ru_player* out, const FakePlayer& p) {
  ru_player r{};
  r.struct_size = out->struct_size;
  r.slot = p.slot;
  r.steamid64 = p.steamid64;
  r.team = p.team;
  r.is_bot = p.bot ? 1 : 0;
  r.connected = 1;
  std::snprintf(r.name, sizeof(r.name), "%s", p.name);
  r.userid = p.userid;
  std::memcpy(out, &r, out->struct_size < sizeof(r) ? out->struct_size : sizeof(r));
}

namespace readyup::plugins::detail {
void FillEngineApi(ru_api* a) {
  a->center_html_to_slot = [](ru_plugin*, int, const char*, int) { return 1; };
  a->center_html_all = [](ru_plugin*, const char* html, int) {
    g_lastCenterAll = html ? html : "";
    return 0;
  };
  a->get_player = [](ru_plugin*, int slot, ru_player* out) {
    for (const auto& p : g_players) {
      if (p.slot == slot) {
        FillPlayer(out, p);
        return 1;
      }
    }
    return 0;
  };
  a->get_player_by_steamid = [](ru_plugin*, uint64_t sid, ru_player* out) {
    for (const auto& p : g_players) {
      if (p.steamid64 == sid) {
        FillPlayer(out, p);
        return 1;
      }
    }
    return 0;
  };
  a->for_each_player = [](ru_plugin*, ru_player_fn fn, void* user) {
    int n = 0;
    ru_player r{};
    r.struct_size = sizeof(r);
    for (const auto& p : g_players) {
      FillPlayer(&r, p);
      ++n;
      if (!fn(user, &r)) break;
    }
    return n;
  };
  a->player_change_team = [](ru_plugin*, int slot, int team) {
    for (auto& p : g_players) {
      if (p.slot == slot) {
        if (!g_refuseTeamChange) p.team = team;
        return 1;
      }
    }
    return 0;
  };
  a->engine_feature_available = [](ru_plugin*, const char* f) {
    return std::strcmp(f, "respawn") == 0 && g_respawnAvailable ? 1 : 0;
  };
  a->player_respawn = [](ru_plugin*, int slot) {
    g_respawned.push_back(slot);
    return g_respawnRc;
  };
  a->ev_get_int = [](ru_plugin*, const ru_game_event*, const char*, int def) { return def; };
  a->ev_get_float = [](ru_plugin*, const ru_game_event*, const char*, double def) { return def; };
  a->ev_get_uint64 = [](ru_plugin*, const ru_game_event*, const char*, uint64_t def) { return def; };
  a->ev_get_string = [](ru_plugin*, const ru_game_event*, const char*, const char* def) { return def ? def : ""; };
  a->ev_get_player_slot = [](ru_plugin*, const ru_game_event*, const char*) { return -1; };
  a->ev_get_player_controller = [](ru_plugin*, const ru_game_event*, const char*) -> void* { return nullptr; };
  a->ev_get_player_pawn = [](ru_plugin*, const ru_game_event*, const char*) -> void* { return nullptr; };
  a->schema_offset = [](ru_plugin*, const char* cls, const char* field) {
    const std::string f = std::string(cls) + "." + field;
    if (f == "CTeam.m_iScore") return static_cast<int>(offsetof(FakeTeam, score));
    // warmup_money / weapon_cleanup / coach: present (no entity they look at exists).
    if (f == "CCSPlayerController.m_pInGameMoneyServices" || f == "CCSPlayerController_InGameMoneyServices.m_iAccount" ||
        f == "CBaseEntity.m_hOwnerEntity" || f == "CCSPlayerController.m_iTeamNum" ||
        f == "CCSPlayerController.m_iCoachingTeam") {
      return 0;
    }
    return -1;
  };
  a->entity_system_status = [](ru_plugin*) {
    return static_cast<int>(g_teamsUp.load() ? RU_ENTSYS_OK : RU_ENTSYS_PENDING);
  };
  a->entity_by_index = [](ru_plugin*, int i) -> void* { return g_teamsUp.load() && i == kTeamIndex ? &g_team : nullptr; };
  a->entity_from_handle = [](ru_plugin*, uint32_t h) -> void* {
    return g_teamsUp.load() && h == kTeamIndex ? &g_team : nullptr;
  };
  a->entity_handle_of = [](ru_plugin*, void* e) { return e == &g_team ? static_cast<uint32_t>(kTeamIndex) : 0xFFFFFFFFu; };
  a->entity_classname = [](ru_plugin*, void* e) -> const char* { return e == &g_team ? "cs_team_manager" : nullptr; };
  a->entity_mark_changed = [](ru_plugin*, void*) { return 0; };
  a->econ_attr_set_by_name = [](ru_plugin*, void*, const char*, double) { return 0; };
  a->entity_change_subclass = [](ru_plugin*, void*, const char*) { return 0; };
  a->entity_set_model = [](ru_plugin*, void*, const char*) { return 0; };
  a->entity_set_bodygroup_by_name = [](ru_plugin*, void*, const char*, int) { return static_cast<int>(RU_BODYGROUP_UNAVAILABLE); };
  a->entity_set_abs_origin = [](ru_plugin*, void*, const float*) { return 0; };
  a->entity_remove = [](ru_plugin*, void*) { return 0; };
  a->workshop_download_progress = [](ru_plugin*, uint64_t id, uint64_t* done, uint64_t* total) {
    if (id != 3793104017ull) return 0;  // "installed": no download info
    *done = 50ull << 20;
    *total = 200ull << 20;
    return 1;
  };
  a->set_round_termination_suppressed = [](ru_plugin*, int s) {
    g_suppressed.store(s);
    return 1;
  };
  // Like the core: ask every plugin's admin provider (match: match config / MAT / fleet;
  // essentials: admins.json).
  a->is_admin = [](ru_plugin*, uint64_t sid) { return readyup::plugins::PluginAdminVerdict(sid) == 1 ? 1 : 0; };
  a->feature_state = [](ru_plugin*, const char* name) {
    const std::string n = name;
    if (n == "events_live" || n == "fn:UTIL_ClientPrintAll") return 0;
    return 1;  // match_flow, knife, ready_hud, welcome_html, pauses, ...
  };
  a->license_player_line = [](ru_plugin*, char* buf, uint32_t len) {
    if (!readyup::g_showLicense) return -1;
    return std::snprintf(buf, len, "%s", "Licensed to Example LAN AS");
  };
}
}  // namespace readyup::plugins::detail

namespace rp = readyup::plugins;

// ---- helpers --------------------------------------------------------------------------------
static int g_failed = 0;
static void Check(bool ok, const char* what) {
  std::printf("%s %s\n", ok ? "PASS" : "FAIL", what);
  if (!ok) ++g_failed;
}
static bool Logged(const std::string& needle) {
  std::lock_guard<std::mutex> lk(g_logMu);
  for (const auto& l : g_log) {
    if (l.find(needle) != std::string::npos) return true;
  }
  return false;
}
static int CountLogged(const std::string& needle) {
  std::lock_guard<std::mutex> lk(g_logMu);
  int n = 0;
  for (const auto& l : g_log) {
    if (l.find(needle) != std::string::npos) ++n;
  }
  return n;
}
static void ClearLog() {
  std::lock_guard<std::mutex> lk(g_logMu);
  g_log.clear();
  g_chat.clear();
}
static bool FramesUntil(const std::function<bool()>& pred, int ms) {
  const auto end = std::chrono::steady_clock::now() + std::chrono::milliseconds(ms);
  while (std::chrono::steady_clock::now() < end) {
    rp::Frame(true);
    if (pred()) return true;
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }
  return pred();
}
static std::string Summary() {
  const auto* m = static_cast<const ru_match_v1*>(rp::CoreGetInterface(RU_MATCH_IFACE_NAME, RU_MATCH_IFACE_VERSION));
  if (!m) return "(none)";
  ru_match_status st{};
  st.struct_size = sizeof(st);
  if (m->get_status(&st) != 1) return "(error)";
  return std::string(st.summary_json) + (st.state_json ? std::string(" STATE ") + st.state_json : std::string());
}
static bool Has(const std::string& hay, const std::string& needle) { return hay.find(needle) != std::string::npos; }
static bool Sent(const std::string& cmd) {
  std::lock_guard<std::mutex> lk(g_logMu);
  for (const auto& c : g_cmds) {
    if (c == cmd) return true;
  }
  return false;
}
static bool Chatted(const std::string& needle) {
  std::lock_guard<std::mutex> lk(g_logMu);
  for (const auto& c : g_chat) {
    if (c.find(needle) != std::string::npos) return true;
  }
  return false;
}
// Index of the first command equal to `cmd`, -1 if none.
static int SentAt(const std::string& cmd) {
  std::lock_guard<std::mutex> lk(g_logMu);
  for (size_t i = 0; i < g_cmds.size(); ++i) {
    if (g_cmds[i] == cmd) return static_cast<int>(i);
  }
  return -1;
}
static int CountPrefix(const std::string& prefix) {
  std::lock_guard<std::mutex> lk(g_logMu);
  int n = 0;
  for (const auto& c : g_cmds) {
    if (c.compare(0, prefix.size(), prefix) == 0) ++n;
  }
  return n;
}
static void ClearCmds() {
  std::lock_guard<std::mutex> lk(g_logMu);
  g_cmds.clear();
}

// One-shot HTTP server for `ru match load`.
static int ServeOnce(const std::string& body, std::thread* th) {
  const int fd = socket(AF_INET, SOCK_STREAM, 0);
  sockaddr_in a{};
  a.sin_family = AF_INET;
  a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  a.sin_port = 0;
  if (bind(fd, reinterpret_cast<sockaddr*>(&a), sizeof(a)) != 0 || listen(fd, 1) != 0) return -1;
  socklen_t len = sizeof(a);
  getsockname(fd, reinterpret_cast<sockaddr*>(&a), &len);
  *th = std::thread([fd, body] {
    const int c = accept(fd, nullptr, nullptr);
    if (c >= 0) {
      char buf[4096];
      (void)!read(c, buf, sizeof(buf));
      const std::string resp = "HTTP/1.1 200 OK\r\nContent-Type: application/json\r\nContent-Length: " +
                               std::to_string(body.size()) + "\r\nConnection: close\r\n\r\n" + body;
      (void)!write(c, resp.data(), resp.size());
      close(c);
    }
    close(fd);
  });
  return ntohs(a.sin_port);
}

int main(int argc, char** argv) {
  if (argc < 2) {
    std::fprintf(stderr, "usage: %s <match.so>\n", argv[0]);
    return 2;
  }
  char tmpl[] = "/tmp/ru-match-host.XXXXXX";
  const char* root = mkdtemp(tmpl);
  if (!root) return 2;
  const std::string csgo = std::string(root) + "/csgo";
  const std::string pluginsDir = csgo + "/readyup/plugins";
  readyup::g_csgoDir = csgo;
  readyup::g_moduleDir = csgo + "/readyup/bin/linuxsteamrt64";
  // argv[2] (optional): practice.so, loaded next to match.so (readyup.practice.v1 <-> set_practice).
  const bool withPractice = argc >= 3;
  // argv[3] (optional): essentials.so (admins.json, `ru admins`, `ru map`).
  const bool withEssentials = argc >= 4;
  // argv[4] (optional): deathmatch.so (readyup.match.v1 set_external_mode, essentials default maps).
  const bool withDeathmatch = argc >= 5;
  if (std::system(("mkdir -p '" + pluginsDir + "' '" + readyup::g_moduleDir + "' && cp '" + argv[1] + "' '" +
                   pluginsDir + "/match.so'" + (withPractice ? " && cp '" + std::string(argv[2]) + "' '" + pluginsDir + "/practice.so'" : "") +
                   (withEssentials ? " && cp '" + std::string(argv[3]) + "' '" + pluginsDir + "/essentials.so'" : "") +
                   (withDeathmatch ? " && cp '" + std::string(argv[4]) + "' '" + pluginsDir + "/deathmatch.so'" : ""))
                      .c_str()) != 0) {
    return 2;
  }
  setenv("READYUP_PLUGINS_DIR", pluginsDir.c_str(), 1);
  if (withEssentials) {
    // An older install: the admins list in the match plugin's folder, no essentials folder yet.
    std::system(("rm -rf '" + pluginsDir + "/essentials' && mkdir -p '" + pluginsDir + "/match'").c_str());
    FILE* old = std::fopen((pluginsDir + "/match/admins.json").c_str(), "w");
    std::fputs(R"({"version": 1, "admins": [{"steamid64": "76561198000000077", "name": "owner"}]})", old);
    std::fclose(old);
  }
  {
    FILE* cfg = std::fopen((readyup::g_moduleDir + "/readyup.cfg").c_str(), "w");
    std::fputs("debug=0\nwelcome=0\nconsume_ready_chat=1\n[match]\nknife_pick_seconds=33\n", cfg);
    std::fclose(cfg);
  }

  std::puts("-- load");
  rp::LifecycleEvent map;
  map.type = RU_EVENT_MAP_START;
  map.map = "de_test";
  rp::PostEvent(map);  // the core's current map before the plugin loads
  rp::Frame(false);
  Check(Logged("plugin: loaded match"), "match.so loaded");
  if (withEssentials) {
    std::ifstream copied(pluginsDir + "/essentials/admins.json");
    std::stringstream text;
    text << copied.rdbuf();
    Check(text.str().find("76561198000000077") != std::string::npos,
          "essentials: the core made its folder, and the old match/admins.json was copied into it");
  }
  uint32_t flags = 0;
  Check(rp::ChatCommandOwned(".r", &flags) && (flags & RU_CMD_HIDE), ".r owned, hidden (consume_ready_chat=1)");
  Check(rp::ChatCommandOwned(".pause", &flags) && flags == 0, ".pause owned, visible");
  Check(rp::TryDispatchRu(true, 0, "Console", "ru match state"), "`ru match state` is a match command");
  rp::Frame(true);
  Check(Logged("state: mode=idle") && !Logged("plugin[match]: state:"), "`ru match state` answered, log lines untagged");
  {
    // scrim_auto=1 and alice on CT: the same frame may already have moved idle -> scrim_warmup.
    const std::string s0 = Summary();
    const bool ok = Has(s0, "\"map\":\"de_test\"") &&
                    (Has(s0, "\"ru_mode\":\"idle\"") || Has(s0, "\"ru_mode\":\"scrim_warmup\""));
    Check(ok, "readyup.match.v1: status on the core's current map");
    if (!ok) std::printf("  summary: %s\n", s0.c_str());
  }
  {
    // v1.4 map_stats: nothing recording before a map goes live; fn is not called.
    const auto* m = static_cast<const ru_match_v1*>(rp::CoreGetInterface(RU_MATCH_IFACE_NAME, RU_MATCH_IFACE_VERSION));
    ru_match_map_info info{};
    info.struct_size = sizeof(info);
    info.live = info.rounds = -1;
    int calls = 0;
    const bool has = RU_API_HAS(m, map_stats) && m->map_stats;
    const int rc = has ? m->map_stats(&info, [](void* u, const ru_match_player_stats*) { ++*static_cast<int*>(u); }, &calls) : 0;
    Check(rc == 1 && info.struct_size == sizeof(info) && info.live == 0 && info.rounds == 0 && info.half >= 1 && calls == 0,
          "readyup.match.v1 map_stats: not live before going live");
  }
  {
    bool db = false, hud = false;
    for (const auto& c : rp::RunPluginSelftests()) {
      if (c.plugin != "match") continue;
      db |= c.name == "store";
      hud |= c.name == "ready HUD";
    }
    Check(db && hud, "selftest lines from readyup.selftest.match");
  }
  Check(rp::TryDispatchConsole("ru_warmup_roundtime_minutes 7"), "console setting owned by match");
  rp::TryDispatchConsole("ru_series_end_kick_delay_no_demo 9");
  rp::Frame(true);
  Check(Logged("warmup roundtime minutes: set"), "console setting applied");

  std::puts("-- scrim warmup + .r");
  Check(FramesUntil([] { return Logged("state: mode=scrim_warmup"); }, 3000), "human on CT -> scrim_warmup");
  Check(g_suppressed.load() == 0, "no round-termination suppression in scrim warmup");
  // A score in scrim warmup: the scrim warmup rules go out again, and they restart the game themselves.
  Check(FramesUntil([] { return Sent("mp_roundtime 60"); }, 2000), "scrim warmup rules applied");
  ClearCmds();
  g_teamsUp = true;
  g_team.score = 2;
  Check(FramesUntil([] { return Sent("mp_roundtime 60"); }, 2000), "scrim warmup, a team has a score: warmup rules sent again");
  FramesUntil([] { return false; }, 300);
  Check(CountPrefix("mp_restartgame") == 1, "scrim warmup, a team has a score: one mp_restartgame (the rules')");
  g_team.score = 0;
  g_teamsUp = false;
  // A new map: its `Loading map` is the map change, its `Started map` the same load.
  ClearCmds();
  rp::PostLogLine("L 01/01/2026 - 00:00:01: Loading map \"de_other\"\n");
  FramesUntil([] { return false; }, 1000);
  rp::PostLogLine("L 01/01/2026 - 00:00:01: Started map \"de_other\" (CRC \"1\")\n");
  FramesUntil([] { return false; }, 1500);
  Check(CountPrefix("mp_roundtime 60") == 1, "new map in scrim warmup: the warmup rules go out once");
  rp::PostLogLine("L 01/01/2026 - 00:00:01: Loading map \"de_test\"\n");
  rp::PostLogLine("L 01/01/2026 - 00:00:01: Started map \"de_test\" (CRC \"1\")\n");
  rp::Frame(true);
  Check(rp::TryDispatchChat(76561198000000001ull, "alice", ".r", 2), ".r dispatched");
  rp::Frame(true);
  Check(Has(Summary(), "\"ready\":{\"ready\":1,\"total\":1}"), "alice is ready (1/1)");

  std::puts("-- .admin: private answer, console log, per-player cooldown");
  ClearLog();
  Check(rp::TryDispatchChat(76561198000000001ull, "alice", ".admin  smoke  bug on B ", 2), ".admin dispatched");
  rp::Frame(true);
  Check(Chatted("admins notified."), ".admin: the caller is told admins were notified");
  Check(Logged("admin-call: alice (76561198000000001, CT): smoke bug on B ["), ".admin: console log with the cleaned message");
  ClearLog();
  rp::TryDispatchChat(76561198000000001ull, "alice", ".admin again", 2);
  rp::Frame(true);
  Check(Chatted("you can call again in") && !Logged("admin-call:"), ".admin within the cooldown: refused");

  std::puts("-- reload keeps mode, ready states, settings");
  ClearLog();
  rp::HandlePluginCommand({"reload", "match"}, false);
  rp::Frame(true);
  Check(Logged("plugin: reloaded match"), "reloaded");
  Check(!Logged("still mapped"), "old image unmapped (every worker thread joined)");
  Check(Logged("restored the previous image's state (mode=scrim_warmup"), "state restored after reload");
  Check(Has(Summary(), "\"ru_mode\":\"scrim_warmup\"") && Has(Summary(), "\"ready\":{\"ready\":1,\"total\":1}"),
        "mode + ready survive the reload");
  rp::TryDispatchConsole("ru_warmup_roundtime_minutes");
  rp::TryDispatchConsole("ru_series_end_kick_delay_no_demo");
  rp::Frame(true);
  Check(Logged("warmup roundtime minutes: 7"), "ru_warmup_* survives the reload");
  Check(Logged("no_demo=9"), "series-end kick delay survives the reload");

  std::puts("-- main commands + subcommands: help, unknown, admin-only (the console always may)");
  ClearLog();
  ClearCmds();
  Check(!rp::TryDispatchRu(true, 0, "Console", "ru restart"), "old flat `ru restart` is not a match command");
  Check(!rp::TryDispatchRu(true, 0, "Console", "ru idle"), "old flat `ru idle` is not a match command");
  if (withEssentials) {
    rp::TryDispatchRu(false, 76561198000000001ull, "alice", ".ru map", 2);
    rp::TryDispatchRu(false, 76561198000000001ull, "alice", ".ru map nope", 2);
    rp::Frame(true);
    Check(Chatted(".ru map change <name|workshop id|link> [force]: change map (admin)"), ".ru map: its subcommands, to the sender");
    Check(Chatted("unknown command. Type .ru help map"), ".ru map <unknown>: points at .ru help map");
    for (const char* c : {".ru map change de_other", ".ru map reload", ".ru map restart", ".ru match load http://127.0.0.1:1/x",
                          ".ru mode idle"}) {
      rp::TryDispatchRu(false, 76561198000000001ull, "alice", c, 2);
    }
    rp::Frame(true);
    Check(Chatted("not authorized") && !Sent("changelevel de_other") && !Sent("changelevel de_test") &&
              !Sent("mp_restartgame 1") && !Logged("match-load[") && !Logged("state: mode=idle"),
          "non-admin: map change / reload / restart, match load, mode idle refused");
    Check(rp::TryDispatchRu(true, 0, "Console", "ru map change 3084291314"), "`ru map` is the essentials plugin's");
    rp::TryDispatchRu(true, 0, "Console", "ru map change de_x;quit");
    rp::TryDispatchRu(true, 0, "Console", "ru map change https://steamcommunity.com/sharedfiles/filedetails/?id=3793104017&searchtext=x");
    rp::TryDispatchRu(true, 0, "Console", "ru map reload");
    rp::TryDispatchRu(true, 0, "Console", "ru map restart");
    rp::Frame(true);
    Check(Sent("host_workshop_map 3084291314"), "console: ru map change <workshop id> -> host_workshop_map");
    Check(Logged("not a map name, workshop id or workshop link") && !Sent("changelevel de_x;quit"), "console: bad map name refused");
    Check(Sent("host_workshop_map 3793104017"), "console: a pasted Workshop link loads its id");
    Check(Sent("changelevel de_test"), "console: ru map reload -> changelevel to the current map");
    Check(Sent("mp_restartgame 1"), "console: ru map restart -> mp_restartgame 1");
    rp::Frame(true);
    Check(readyup::g_lastCenterAll.find("25.0% &#183; 50.0 / 200.0 MB") != std::string::npos,
          "workshop map change: download progress bar in the center panel");
    rp::TryDispatchRu(true, 0, "Console", "ru admins add 76561198000000001");
    rp::Frame(true);
    ClearLog();
    ClearCmds();
    rp::TryDispatchRu(false, 76561198000000001ull, "alice", ".ru map change de_other", 2);
    rp::TryDispatchChat(76561198000000001ull, "alice", ".help", 2);
    rp::Frame(true);
    Check(Sent("changelevel de_other"), "admin: .ru map change de_other -> changelevel");
    Check(Chatted("Ready Up admin: .ru help"), "admin: .help points at .ru help");
    Check(!Chatted("Licensed to"), ".help: no license line by default");
    readyup::g_showLicense = true;
    ClearLog();
    rp::TryDispatchChat(76561198000000001ull, "alice", ".help", 2);
    rp::Frame(true);
    Check(Chatted("Licensed to Example LAN AS"), ".help: readyup_show_license 1 adds \"Licensed to <licensee>\"");
    readyup::g_showLicense = false;
    rp::TryDispatchRu(true, 0, "Console", "ru admins remove 76561198000000001");
    rp::Frame(true);
  }

  if (withPractice) {
    std::puts("-- practice.so next to match.so: .ru mode practice goes through readyup.practice.v1");
    ClearLog();
    ClearCmds();
    uint32_t pf = 0;
    Check(rp::ChatCommandOwned(".prac", &pf) && rp::ChatCommandOwned(".rethrow", &pf), ".prac / .rethrow owned by practice.so");
    rp::TryDispatchRu(true, 0, "Console", "ru mode practice");
    rp::Frame(true);
    Check(Sent("exec ReadyUp/prac.cfg") && Sent("mp_restartgame 1"), "practice on: prac.cfg + respawn everyone");
    Check(Has(Summary(), "\"ru_mode\":\"practice\""), "match flow mode is practice");
    rp::TryDispatchChat(76561198000000001ull, "alice", ".prac", 2);
    rp::Frame(true);
    Check(Chatted("not authorized") && Has(Summary(), "\"ru_mode\":\"practice\""), ".prac from a non-admin refused");
    ClearCmds();
    rp::TryDispatchRu(true, 0, "Console", "ru practice off");
    rp::Frame(true);
    Check(Sent("exec ReadyUp/idle.cfg") && Sent("mp_restartgame 1"), "practice off: idle.cfg + respawn everyone");
    Check(!Has(Summary(), "\"ru_mode\":\"practice\""), "match flow left practice");
  }

  if (withDeathmatch && withEssentials) {
    std::puts("-- deathmatch.so: the match flow steps aside (set_external_mode), default map from essentials");
    ClearLog();
    ClearCmds();
    rp::TryDispatchRu(true, 0, "Console", "ru map default tdm de_dm");
    rp::TryDispatchRu(true, 0, "Console", "ru dm tdm");
    rp::Frame(true);
    Check(Sent("game_type 1") && Sent("game_mode 2") && Sent("changelevel de_dm") &&
              SentAt("game_mode 2") < SentAt("changelevel de_dm"),
          "ru dm tdm: deathmatch game mode, then the default map for tdm (essentials default_maps.json)");
    Check(Has(Summary(), "\"ru_mode\":\"external\"") && Has(Summary(), "\"phase\":\"deathmatch\""),
          "match flow in external mode (status phase deathmatch)");
    ClearCmds();
    map.map = "de_dm";
    rp::PostEvent(map);
    rp::Frame(true);
    rp::Frame(true);
    Check(Sent("mp_teammates_are_enemies 0") && Sent("mp_dm_teammode 1") && Sent("mp_ignore_round_win_conditions 1"),
          "map start: team deathmatch rules");
    Check(!Logged("state: mode=scrim_warmup") && Has(Summary(), "\"ru_mode\":\"external\""), "no scrim warmup over deathmatch");
    Check(g_suppressed.load() == 0, "no round-termination suppression in external mode");
    rp::TryDispatchChat(76561198000000001ull, "alice", ".r", 2);
    rp::Frame(true);
    Check(Chatted("ready-up is not used in deathmatch mode"), ".r: ready-up is not used in deathmatch mode");
    if (withPractice) {
      rp::TryDispatchRu(true, 0, "Console", "ru practice on");
      rp::Frame(true);
      Check(Logged("practice mode refused: another plugin's mode is on"), "practice refused while deathmatch is on");
    }
    rp::TryDispatchRu(true, 0, "Console", "ru dm status");
    rp::Frame(true);
    Check(Logged("Team deathmatch on de_dm, first to 100"), "ru dm status");
    rp::TryDispatchRu(false, 76561198000000001ull, "alice", ".ru dm ffa", 2);
    rp::Frame(true);
    Check(Chatted("not authorized") && !Sent("mp_teammates_are_enemies 1"), "non-admin .ru dm ffa refused");
    ClearCmds();
    rp::TryDispatchRu(true, 0, "Console", "ru dm ffa");  // no ffa default: stays on this map
    rp::Frame(true);
    Check(Sent("mp_teammates_are_enemies 1") && Sent("mp_restartgame 1") && !Sent("changelevel de_dm") && !Sent("game_type 1"),
          "ru dm ffa on a deathmatch map: rules switched + game restart, no map load");
    ClearLog();
    rp::HandlePluginCommand({"reload", "deathmatch"}, false);
    rp::Frame(true);
    Check(Logged("resumed ffa after a reload"), "ru plugin reload deathmatch keeps the mode");
    ClearLog();
    ClearCmds();
    rp::TryDispatchRu(true, 0, "Console", "ru mode idle");
    Check(FramesUntil([] { return Logged("deathmatch: off (match plugin mode idle)"); }, 2000),
          ".ru mode idle: deathmatch notices and turns itself off");
    Check(Sent("game_type 0") && Sent("game_mode 1") && Sent("mp_damage_headshot_only 0") && Sent("changelevel de_dm"),
          "... competitive again, the map loads again");
    Check(Has(Summary(), "\"ru_mode\":\"idle\""), "... match flow idle");
    ClearLog();
    ClearCmds();
    rp::TryDispatchRu(true, 0, "Console", "ru dm ffa aim_x");
    rp::Frame(true);
    map.map = "aim_x";
    rp::PostEvent(map);
    rp::Frame(true);
    Check(Sent("changelevel aim_x") && Sent("mp_teammates_are_enemies 1"), "ru dm ffa aim_x: that map, free for all");
    ClearCmds();
    rp::TryDispatchRu(true, 0, "Console", "ru dm off");
    rp::Frame(true);
    Check(SentAt("game_type 0") >= 0 && SentAt("game_type 0") < SentAt("changelevel aim_x") && Sent("mp_teammates_are_enemies 0"),
          "ru dm off: competitive, then the map again");
    Check(!Has(Summary(), "\"ru_mode\":\"external\""), "ru dm off: the match flow has the server back");
    rp::TryDispatchRu(true, 0, "Console", "ru mode scrim");
    map.map = "de_test";
    rp::PostEvent(map);
    rp::Frame(true);
  }

  std::puts("-- ru match load, then reload with the match loaded");
  g_players.push_back({3, 3, 76561198000000002ull, 2, false, "bob"});
  const std::string body =
      "{\"id\":4242,\"slug\":\"hosttest\",\"config\":{\"matchid\":4242,\"num_maps\":1,\"maplist\":[\"de_test\"],"
      "\"map_sides\":[\"team1_ct\"],\"maxRounds\":24,\"overtimeMode\":\"disabled\","
      "\"max_tech_pauses_per_team\":2,\"tech_pause_max_seconds\":45,\"forfeit_after_seconds\":0,"
      "\"cvars\":{\"mp_freezetime\":7,\"sv_talk_enemy_dead\":1,\"mp_nope_cvar\":3},"
      "\"team1\":{\"name\":\"Alpha\",\"players\":{\"76561198000000001\":\"alice\"}},"
      "\"team2\":{\"name\":\"Bravo\",\"players\":{\"76561198000000002\":\"bob\"}}}}";
  std::thread http;
  const int port = ServeOnce(body, &http);
  Check(port > 0, "test http server up");
  // The last map's score is still up when the match loads (onto the map the server is on).
  g_team.score = 13;
  ClearLog();
  ClearCmds();
  Check(rp::TryDispatchRu(true, 0, "Console", "ru match load http://127.0.0.1:" + std::to_string(port) + "/m.json"),
        "`ru match load` dispatched");
  rp::Frame(true);
  http.join();
  Check(Logged("match context set: matchid=4242 slug=hosttest"), "match loaded");
  // reset_cvars_on_series_end: the pre-match values of cvars{} are read at load (ru_api cvar_query:
  // the bare name goes to the console, the core answers from the `name = value` line).
  Check(FramesUntil([] { return Sent("mp_freezetime") && Sent("sv_talk_enemy_dead") && Sent("mp_nope_cvar"); }, 2000),
        "match load reads every cvars{} name before go-live");
  Check(!Sent("mp_freezetime 7"), "the match value is not set at load");
  rp::PostLogLine("mp_freezetime = 15\n");
  rp::PostLogLine("sv_talk_enemy_dead = 0\n");
  rp::PostLogLine("Unknown command 'mp_nope_cvar'!\n");
  rp::Frame(true);
  if (withPractice) {
    rp::TryDispatchRu(true, 0, "Console", "ru practice on");
    rp::Frame(true);
    Check(Logged("practice mode refused: a match is loaded"), "practice refused while a match is loaded");
  }
  Check(Sent("changelevel de_test"), "match load changes map also onto the map the server is on");
  if (withDeathmatch && withEssentials) {
    Check(SentAt("game_type 0") >= 0 && SentAt("game_type 0") < SentAt("changelevel de_test"),
          "match load after deathmatch: game_type 0 / game_mode 1 before its map change");
  }
  Check(FramesUntil([] { return Has(Summary(), "\"ru_mode\":\"match_warmup\""); }, 2000), "mode match_warmup");
  Check(FramesUntil([] { return g_suppressed.load() == 1; }, 2000), "warmup suppresses round termination");
  Check(FramesUntil([] { return Sent("mp_autokick 0"); }, 2000) && !Sent("exec ReadyUp/warmup.cfg"), "match warmup without cfg exec: team damage kick off");
  // CS2 credits a round before it calls TerminateRound, which match warmup drops: a win condition
  // that holds (the round timer at 0:00 once the warmup cvars are gone) scores on every tick.
  Check(FramesUntil([] { return Sent("mp_roundtime 7"); }, 2000), "match warmup rules applied");
  Check(!Sent("sv_cheats 1") && !Sent("sv_infinite_ammo 2"), "warmup infinite ammo without sv_cheats");
  ClearCmds();
  g_teamsUp = true;
  Check(!FramesUntil([] { return Sent("mp_restartgame 1"); }, 300),
        "match warmup before its map loads: the last map's score is not reset");
  // The changelevel onto the same map: CS2's gamemode cfg runs again over the warmup cvars.
  ClearCmds();
  rp::PostLogLine("L 01/01/2026 - 00:00:02: Loading map \"de_test\"\n");
  Check(!FramesUntil([] { return Sent("mp_warmup_pausetimer 0"); }, 300), "same-map `Loading map`: nothing yet");
  rp::PostLogLine("L 01/01/2026 - 00:00:02: Started map \"de_test\" (CRC \"1\")\n");
  Check(FramesUntil([] { return Sent("mp_roundtime 7"); }, 3000), "same-map reload in match_warmup: warmup rules applied again");
  FramesUntil([] { return false; }, 300);
  Check(CountPrefix("mp_roundtime ") == 1, "same-map reload: the warmup rules go out once");
  Check(FramesUntil([] { return Sent("mp_restartgame 1"); }, 2000),
        "same-map match load: a score once the map has loaded is reset");
  ClearCmds();
  FramesUntil([] { return Sent("mp_roundtime 7"); }, 2000);  // the warmup rules that go with it
  g_team.score = 0;
  g_teamsUp = false;
  ClearCmds();
  g_teamsUp = true;
  Check(!FramesUntil([] { return Sent("mp_restartgame 1") || Sent("mp_roundtime 7"); }, 300),
        "match warmup, no score: no restart, warmup rules not sent again");
  g_team.score = 37;
  Check(FramesUntil([] { return Sent("mp_restartgame 1"); }, 4000),
        "match warmup, a team has a score: mp_restartgame 1 (score back to 0, everyone respawns)");
  // The score means the warmup rules were lost (gamemode_competitive.cfg ran again): they go out again.
  Check(FramesUntil([] { return Sent("mp_ignore_round_win_conditions 1") && Sent("mp_roundtime 7"); }, 2000),
        "match warmup, a team has a score: warmup rules sent again");
  ClearCmds();
  Check(!FramesUntil([] { return Sent("mp_restartgame 1"); }, 500), "one restart at a time");
  g_team.score = 0;
  g_teamsUp = false;
  rp::TryDispatchChat(76561198000000002ull, "bob", ".ready", 3);
  rp::Frame(true);
  Check(Has(Summary(), "\"ready\":{\"ready\":1,\"total\":2}"), "bob ready (1/2)");
  ClearLog();
  ClearCmds();
  rp::HandlePluginCommand({"reload", "match"}, false);
  rp::Frame(true);
  Check(Logged("restored the previous image's state (mode=match_warmup match=hosttest"), "match restored after reload");
  Check(!Sent("mp_freezetime") && !Sent("mp_nope_cvar"), "reload: the cvar snapshot came along, nothing read again");
  const std::string s = Summary();
  Check(Has(s, "\"match_id\":\"4242\"") && Has(s, "\"slug\":\"hosttest\"") && Has(s, "\"ready\":{\"ready\":1,\"total\":2}"),
        "loaded match + ready states survive the reload");
  Check(Has(s, "\"Alpha\"") && Has(s, "\"76561198000000002\""), "MatchState roster survives the reload");
  Check(rp::ChatCommandOwned(".r", &flags), "commands registered again by the new image");
  ClearLog();
  Check(rp::TryDispatchRu(true, 0, "Console", "ru match state"), "`ru match state` after the reload");
  rp::Frame(true);
  Check(Logged("rules: tech_pauses=2 tech_max_s=45 unpause=both force_ready=1 min_ready=0 forfeit_s=0"),
        "match rules survive the reload");

  std::puts("-- team lock: a refused move is retried quietly, a landed one is logged once");
  ClearLog();
  g_players.back().team = 0;
  g_refuseTeamChange = true;
  Check(FramesUntil([] { return Logged("ready: could not move bob to T"); }, 3000), "a refused move is reported");
  (void)FramesUntil([] { return false; }, 3000);  // a few more retries
  Check(CountLogged("ready: could not move bob to T") == 1, "the refusal is reported once, not every retry");
  Check(!Logged("ready: moved bob"), "a refused move is not logged as moved");
  g_refuseTeamChange = false;
  Check(FramesUntil([] { return Logged("ready: moved bob to T"); }, 3000), "the retry lands");
  (void)FramesUntil([] { return false; }, 2000);
  Check(CountLogged("ready: moved bob to T") == 1, "a landed move is logged once");

  std::puts("-- team lock in match warmup: a forced move respawns the player");
  auto respawned = [](int slot) { return std::find(g_respawned.begin(), g_respawned.end(), slot) != g_respawned.end(); };
  ClearLog();
  ClearCmds();
  g_respawned.clear();
  g_players.back().team = 0;
  g_refuseTeamChange = true;
  Check(FramesUntil([] { return Logged("ready: could not move bob to T"); }, 3000), "a refused move is still attempted");
  Check(!FramesUntil([&] { return Sent("mp_restartgame 1") || respawned(3); }, 3000),
        "a refused move neither respawns nor restarts the warmup round");
  g_refuseTeamChange = false;
  ClearLog();
  Check(FramesUntil([] { return Logged("ready: moved bob to T"); }, 6000), "bob on no team is moved to his side");
  Check(FramesUntil([&] { return respawned(3); }, 3000), "the forced move respawns bob");
  Check(!Sent("mp_restartgame 1"), "no warmup round restart when respawn is available");

  std::puts("-- team lock: a joiner who is gone before the respawn is left alone");
  ClearLog();
  g_respawned.clear();
  g_players.back().team = 0;
  Check(FramesUntil([] { return Logged("ready: moved bob to T"); }, 6000), "bob is moved again");
  g_players.back().team = 1;  // back in the spectators before the deferred respawn
  g_refuseTeamChange = true;
  Check(!FramesUntil([&] { return respawned(3); }, 2000), "a joiner no longer on T / CT is not respawned");
  g_refuseTeamChange = false;

  std::puts("-- team lock + the engine's player_team for the same move: one respawn");
  ClearLog();
  g_respawned.clear();
  g_players.back().team = 0;
  Check(FramesUntil([] { return Logged("ready: moved bob to T"); }, 6000), "bob is moved");
  {
    rp::LifecycleEvent ev;
    ev.type = RU_EVENT_PLAYER_TEAM;
    ev.source = RU_SOURCE_ENGINE;
    ev.slot = 3;
    ev.steamid64 = 76561198000000002ull;
    ev.name = "bob";
    ev.team = 2;
    rp::PostEvent(ev);
  }
  (void)FramesUntil([] { return false; }, 1500);
  Check(std::count(g_respawned.begin(), g_respawned.end(), 3) == 1, "the move and its player_team event respawn bob once");

  std::puts("-- another player in the joiner's slot before the respawn is left alone");
  ClearLog();
  g_respawned.clear();
  g_players.back().team = 0;
  Check(FramesUntil([] { return Logged("ready: moved bob to T"); }, 6000), "bob is moved again");
  g_players.back().steamid64 = 76561198000000009ull;  // bob left, someone else got slot 3 (on T)
  g_refuseTeamChange = true;
  Check(!FramesUntil([&] { return respawned(3); }, 2000), "a different player in the slot is not respawned");
  g_players.back().steamid64 = 76561198000000002ull;
  g_refuseTeamChange = false;

  std::puts("-- team lock: an already alive player is left alone, a refused respawn restarts the round");
  ClearLog();
  ClearCmds();
  g_respawned.clear();
  g_respawnRc = 2;
  g_players.back().team = 0;
  Check(FramesUntil([&] { return respawned(3); }, 6000), "player_respawn is tried for the moved player");
  Check(!FramesUntil([] { return Sent("mp_restartgame 1"); }, 2500), "an already alive player does not restart the round");
  ClearLog();
  ClearCmds();
  g_respawned.clear();
  g_respawnRc = 0;
  g_players.back().team = 0;
  Check(FramesUntil([&] { return respawned(3); }, 6000), "player_respawn is tried again");
  Check(FramesUntil([] { return Sent("mp_restartgame 1"); }, 3000), "a refused respawn falls back to the warmup round restart");
  g_respawnRc = 1;
  (void)FramesUntil([] { return false; }, 4500);  // past the restart debounce

  std::puts("-- team lock without respawn: a forced move restarts the warmup round");
  ClearLog();
  ClearCmds();
  g_respawned.clear();
  g_respawnAvailable = false;
  g_players.back().team = 0;
  Check(FramesUntil([] { return Logged("ready: moved bob to T"); }, 6000), "bob is moved to his side");
  Check(FramesUntil([] { return Sent("mp_restartgame 1"); }, 3000), "the forced move restarts the warmup round");
  Check(g_respawned.empty(), "player_respawn is not called without the feature");
  g_respawnAvailable = true;

  std::puts("-- round restore: backups list, refusals, ru_pause_after_restore");
  ClearLog();
  rp::TryDispatchRu(true, 0, "Console", "ru match backups");
  rp::Frame(true);
  Check(Logged("no round backups for match 4242 on this server."), "ru match backups: none yet");
  std::system(("mkdir -p '" + csgo + "/readyup' && touch '" + csgo + "/readyup/readyup_backup_4242_map1_round00.txt' '" +
               csgo + "/readyup/readyup_backup_4242_map1_round01.txt' '" + csgo +
               "/readyup/readyup_backup_99_map1_round05.txt'").c_str());
  ClearLog();
  rp::TryDispatchRu(true, 0, "Console", "ru match backups");
  rp::TryDispatchConsole("ru_listbackups 99");
  rp::Frame(true);
  Check(Logged("map 1 round 1 (0-0) readyup_backup_4242_map1_round00.txt") &&
            Logged("map 1 round 2 readyup_backup_4242_map1_round01.txt"),
        "ru match backups: this match's files, round = the round they restore");
  Check(Logged("map 1 round 6 readyup_backup_99_map1_round05.txt"), "ru_listbackups <matchid>: another match's files");
  ClearLog();
  ClearCmds();
  rp::TryDispatchRu(true, 0, "Console", "ru match restore 1");
  rp::TryDispatchRu(true, 0, "Console", "ru match restore x");
  rp::TryDispatchConsole("ru_loadbackup readyup_backup_99_map1_round05.txt");
  rp::TryDispatchChat(76561198000000002ull, "bob", ".restore 1", 3);
  rp::Frame(true);
  Check(Logged("restore refused: a restore needs a live map."), "ru match restore in warmup: refused");
  Check(Logged("usage: .restore <round>"), "ru match restore x: usage");
  Check(Logged("is not a round backup of the loaded match"), "ru_loadbackup: another match's file refused");
  Check(Chatted("not authorized"), ".restore from a non-admin refused");
  Check(!Sent("mp_backup_restore_load_file"), "nothing restored");
  ClearLog();
  rp::TryDispatchConsole("ru_pause_after_restore");
  rp::TryDispatchConsole("ru_pause_after_restore 0");
  rp::Frame(true);
  Check(Logged("pause after restore: 1") && Logged("pause after restore: 0"), "ru_pause_after_restore: default 1, set 0");

  std::puts("-- console settings go to state.json (a server restart restores them)");
  rp::TryDispatchConsole("ru_demo_path persist/");
  rp::TryDispatchConsole("ru_demo_path /rejected/");
  rp::TryDispatchConsole("ru_warmup_startmoney 20000");
  rp::TryDispatchConsole("ru_demo_upload_header \"X-Token\" \"abc\"");
  // The platform's token header (AT at_demo_upload_header_key / _value; get5_* aliases).
  rp::TryDispatchConsole("ru_demo_upload_header_key \"X-Auto-Tournament-Token\"");
  rp::TryDispatchConsole("get5_demo_upload_header_value \"tok123\"");
  rp::TryDispatchConsole("ru_demo_status");
  rp::TryDispatchConsole("ru_series_end_kick_delay_demo_upload 77");
  rp::TryDispatchConsole("ru_series_end_kick_delay_demo_upload default");
  rp::Frame(true);
  Check(Logged("ru_series_end_kick_delay_demo_upload: back to the default"), "`<setting> default` answered");
  Check(Logged("ru_demo_upload_header_value: set") && !Logged("tok123"), "header value set, never printed");
  Check(Logged("headers=[X-Token,X-Auto-Tournament-Token]"), "ru_demo_status lists the token header");

  std::puts("-- match server settings (server_settings.h): console, `ru settings`, hostname_format");
  ClearLog();
  rp::TryDispatchConsole("ru_playout_enabled_default 1");
  rp::TryDispatchConsole("ru_minimum_ready_required 99");
  rp::TryDispatchConsole("ru_hostname_format \"{TEAM1} vs {TEAM2}\"");
  rp::Frame(true);
  Check(Logged("playout_enabled_default = on (runtime)"), "ru_playout_enabled_default 1: set");
  Check(Logged("minimum_ready_required takes a number from 0 to 32"), "ru_minimum_ready_required 99: refused");
  Check(FramesUntil([] { return Sent("hostname \"Alpha vs Bravo\""); }, 3000), "hostname_format applied to the loaded match");
  ClearLog();
  Check(rp::TryDispatchRu(true, 0, "Console", "ru settings show"), "`ru settings show` dispatched");
  rp::Frame(true);
  Check(Logged("hostname_format = \"{TEAM1} vs {TEAM2}\" (runtime)") && Logged("series_end_kick_delay: no_demo=") &&
              Logged("pause_after_restore = "),
        "ru settings show lists the settings");
  ClearLog();
  rp::TryDispatchRu(true, 0, "Console", "ru settings set whitelist_enabled_default off");
  rp::TryDispatchRu(true, 0, "Console", "ru settings default playout_enabled_default");
  rp::TryDispatchRu(true, 0, "Console", "ru settings set series_end_kick_delay_no_demo 12");
  rp::Frame(true);
  Check(Logged("whitelist_enabled_default = off (runtime)"), "ru settings set");
  Check(Logged("playout_enabled_default = off"), "ru settings default");
  Check(Logged("series_end_kick_delay_no_demo = 12"), "ru settings set takes the kick delays too");

  std::puts("-- go-live pending (still match warmup): a join neither respawns nor restarts");
  ClearLog();
  ClearCmds();
  rp::TryDispatchChat(76561198000000001ull, "alice", ".ready", 2);
  rp::Frame(true);
  Check(FramesUntil([] { return Sent("mp_restartgame 1"); }, 3000), "all ready: going live");
  Check(Has(Summary(), "\"ru_mode\":\"match_warmup\""), "match warmup until the live round starts");
  ClearLog();
  ClearCmds();
  g_respawned.clear();
  g_players.back().team = 0;
  Check(FramesUntil([] { return Logged("ready: moved bob to T"); }, 6000), "the team lock still moves bob");
  Check(!FramesUntil([&] { return respawned(3) || Sent("mp_restartgame 1"); }, 2500),
        "no respawn and no extra restart while going live");

  std::puts("-- unload");
  ClearLog();
  rp::HandlePluginCommand({"unload", "match"}, false);
  rp::Frame(true);
  Check(Logged("plugin: unloaded match"), "unloaded");
  Check(!Logged("still mapped"), "image unmapped");
  Check(g_suppressed.load() == 0, "unload lifted round-termination suppression");
  Check(!rp::ChatCommandOwned(".r", &flags), ".r no longer routed");
  Check(rp::CoreGetInterface(RU_MATCH_IFACE_NAME, RU_MATCH_IFACE_VERSION) == nullptr, "readyup.match.v1 removed");
  Check(dlopen((pluginsDir + "/match.so").c_str(), RTLD_NOW | RTLD_NOLOAD) == nullptr, "match.so no longer mapped");
  {
    std::string st;
    if (FILE* f = std::fopen((pluginsDir + "/match/state.json").c_str(), "r")) {
      char buf[4096];
      size_t n = 0;
      while ((n = std::fread(buf, 1, sizeof buf, f)) > 0) st.append(buf, n);
      std::fclose(f);
    }
    Check(Has(st, "\"ru_demo_path\"") && Has(st, "\"persist/\"") && !Has(st, "rejected"),
          "state.json: ru_demo_path saved, rejected value not");
    Check(Has(st, "\"ru_warmup_startmoney\"") && Has(st, "\"ru_warmup_maxmoney\""),
          "state.json: startmoney + the maxmoney it raised");
    Check(Has(st, "X-Token: abc"), "state.json: upload headers");
    Check(Has(st, "\"ru_demo_upload_header_key\"") && Has(st, "\"X-Auto-Tournament-Token\"") &&
              Has(st, "\"ru_demo_upload_header_value\"") && Has(st, "\"tok123\""),
          "state.json: the token header key / value (get5_ alias saved under the ru_ name)");
    Check(Has(st, "\"ru_series_end_kick_delay_no_demo\"") && !Has(st, "ru_series_end_kick_delay_demo_upload"),
          "state.json: kick delay saved; `default` removed the other");
    Check(!Has(st, "ru_warmup_respawn"), "state.json: settings left at their default are not stored");
    Check(Has(st, "\"ru_pause_after_restore\"") && Has(st, "\"ru_active_match_json\""),
          "state.json: ru_pause_after_restore and the loaded match (crash recovery)");
    // Values only (the unknown mp_nope_cvar has none), as a JSON string inside state.json.
    Check(Has(st, R"("ru_active_cvar_snapshot": "{\"values\":{\"mp_freezetime\":\"15\",\"sv_talk_enemy_dead\":\"0\"}}")"),
          "state.json: the pre-match cvar values (a restart mid-series still restores them)");
    Check(Has(st, "\"ru_whitelist_enabled_default\"") && Has(st, "\"ru_hostname_format\"") &&
              !Has(st, "ru_playout_enabled_default") && !Has(st, "ru_minimum_ready_required"),
          "state.json: server settings saved; `default` and refused values not");
    if (g_failed) std::printf("  state.json: %s\n", st.c_str());
  }

  std::puts("-- load again: the stash still has the match");
  ClearLog();
  rp::HandlePluginCommand({"load", "match"}, false);
  rp::Frame(true);
  Check(Logged("plugin: loaded match") && Logged("match=hosttest"), "load after unload restores the match");
  Check(Logged("restored 2 server setting(s) from state.json"), "server settings restored from state.json");

  std::puts("-- match end puts the match config's cvars back (reset_cvars_on_series_end 1)");
  ClearLog();
  ClearCmds();
  rp::TryDispatchRu(true, 0, "Console", "ru match end");
  Check(FramesUntil([] { return Sent("mp_restartgame 1"); }, 2000), "ru match end resets the server");
  Check(Sent("mp_freezetime \"15\"") && Sent("sv_talk_enemy_dead \"0\"") && !Sent("mp_nope_cvar \"\""),
        "the values read before the match are restored (the unknown cvar is not)");
  Check(SentAt("mp_freezetime \"15\"") > SentAt("mp_respawn_on_death_ct 0") &&
            SentAt("mp_freezetime \"15\"") < SentAt("mp_restartgame 1"),
        "after the fixed warmup resets, before the restart");
  ClearCmds();
  rp::TryDispatchRu(true, 0, "Console", "ru match end");
  rp::Frame(true);
  Check(!Sent("mp_freezetime \"15\""), "restored once: the snapshot is cleared");

  std::puts("-- demo: every way out of a match stops the recording, the next match records its own file");
  {
    const auto loadMatch = [&](int id) {
      const std::string b =
          "{\"id\":" + std::to_string(id) + ",\"slug\":\"demotest\",\"config\":{\"matchid\":" + std::to_string(id) +
          ",\"num_maps\":1,\"maplist\":[\"de_test\"],\"map_sides\":[\"team1_ct\"],\"maxRounds\":24,"
          "\"overtimeMode\":\"disabled\",\"cvars\":{},"
          "\"team1\":{\"name\":\"Alpha\",\"players\":{\"76561198000000001\":\"alice\"}},"
          "\"team2\":{\"name\":\"Bravo\",\"players\":{\"76561198000000002\":\"bob\"}}}}";
      std::thread h;
      const int pt = ServeOnce(b, &h);
      rp::TryDispatchRu(true, 0, "Console", "ru match load http://127.0.0.1:" + std::to_string(pt) + "/m.json");
      rp::Frame(true);
      h.join();
      return FramesUntil([] { return Has(Summary(), "\"ru_mode\":\"match_warmup\""); }, 2000);
    };
    // Each way out: the command that drops the match, and the mode it leaves.
    const char* exits[] = {"ru mode idle", "ru match end"};
    int id = 5000;
    for (const char* exitCmd : exits) {
      ClearCmds();
      Check(loadMatch(++id), (std::string(exitCmd) + ": match loaded").c_str());
      rp::Frame(true);
      ClearCmds();
      rp::TryDispatchRu(true, 0, "Console", "ru match start force");
      rp::Frame(true);
      Check(CountPrefix("tv_record \"") == 1, (std::string(exitCmd) + ": the map's demo started").c_str());
      Check(Sent("mp_autokick 0") && !Sent("exec ReadyUp/live.cfg"),
            (std::string(exitCmd) + ": go-live without cfg exec: team damage kick off").c_str());
      rp::TryDispatchRu(true, 0, "Console", "ru match start force");
      rp::Frame(true);
      Check(CountPrefix("tv_record \"") == 1, (std::string(exitCmd) + ": starting again does not start a second file").c_str());
      Check(CountPrefix("tv_stoprecord") == 0, (std::string(exitCmd) + ": still recording before the exit").c_str());
      rp::TryDispatchRu(true, 0, "Console", exitCmd);
      rp::Frame(true);
      Check(CountPrefix("tv_stoprecord") == 1, (std::string(exitCmd) + ": the recording was stopped").c_str());
      // The next match starts its own demo, not a leftover one.
      ClearCmds();
      rp::TryDispatchRu(true, 0, "Console", "ru mode idle");
      rp::Frame(true);
      rp::TryDispatchRu(true, 0, "Console", "ru match end");
      rp::Frame(true);
      ClearCmds();
      Check(loadMatch(++id), (std::string(exitCmd) + ": next match loaded").c_str());
      rp::TryDispatchRu(true, 0, "Console", "ru match start force");
      rp::Frame(true);
      Check(CountPrefix("tv_record \"") == 1, (std::string(exitCmd) + ": the next match records its own demo").c_str());
      rp::TryDispatchRu(true, 0, "Console", "ru match end");
      rp::Frame(true);
      Check(CountPrefix("tv_stoprecord") == 1, (std::string(exitCmd) + ": and that one is stopped too").c_str());
      rp::TryDispatchRu(true, 0, "Console", "ru mode idle");
      rp::Frame(true);
    }

    std::puts("-- round backups are written only while the match is live");
    for (const char* exitCmd : exits) {
      ClearCmds();
      Check(loadMatch(++id), (std::string(exitCmd) + ": match loaded").c_str());
      Check(Sent("mp_backup_round_auto 0") && !Sent("mp_backup_round_auto 1") &&
                Sent("mp_backup_round_file readyup_backup_" + std::to_string(id) + "_map1_"),
            (std::string(exitCmd) + ": match load sets the backup file, backups stay off in warmup").c_str());
      ClearCmds();
      rp::TryDispatchRu(true, 0, "Console", "ru match start force");
      rp::Frame(true);
      Check(Sent("mp_backup_round_auto 1"), (std::string(exitCmd) + ": the go-live turns backups on").c_str());
      rp::Frame(true);
      Check(!Sent("mp_backup_round_auto 0"), (std::string(exitCmd) + ": they stay on while live").c_str());
      rp::TryDispatchRu(true, 0, "Console", exitCmd);
      Check(FramesUntil([] { return Sent("mp_backup_round_auto 0"); }, 2000),
            (std::string(exitCmd) + ": leaving live turns them off").c_str());
      rp::TryDispatchRu(true, 0, "Console", "ru mode idle");
      rp::Frame(true);
      rp::TryDispatchRu(true, 0, "Console", "ru match end");
      rp::Frame(true);
    }

    // A pause belongs to its match (an admin pause, a failover resume's pause): ending the match
    // while paused must not leave the next match on this server starting as paused.
    std::puts("-- a pause does not outlive its match");
    ClearLog();
    Check(loadMatch(++id), "pause: match loaded");
    rp::TryDispatchRu(true, 0, "Console", "ru match start force");
    rp::Frame(true);
    rp::TryDispatchRu(true, 0, "Console", "ru match pause");
    rp::Frame(true);
    rp::TryDispatchRu(true, 0, "Console", "ru match pause");
    rp::Frame(true);
    Check(Logged("match is already paused"), "pause: the match is paused");
    rp::TryDispatchRu(true, 0, "Console", "ru match end");
    rp::Frame(true);
    ClearLog();
    ClearCmds();
    Check(loadMatch(++id), "pause: next match loaded");
    rp::TryDispatchRu(true, 0, "Console", "ru match start force");
    rp::Frame(true);
    rp::TryDispatchRu(true, 0, "Console", "ru match pause");
    rp::Frame(true);
    Check(!Logged("match is already paused") && Sent("mp_pause_match"), "pause: the next match starts unpaused");
    rp::TryDispatchRu(true, 0, "Console", "ru match end");
    rp::Frame(true);
    rp::TryDispatchRu(true, 0, "Console", "ru mode idle");
    rp::Frame(true);
  }

  std::puts("-- mixed bot fill: real roster, arriving humans, map change and SourceTV");
  {
    const std::string body = R"({"id":6000,"slug":"mixed","config":{"matchid":6000,"num_maps":3,"maplist":["de_mixed","de_mixed2","de_mixed3"],"map_sides":["team1_ct","team1_ct","team1_ct"],"bot_fill":true,"players_per_team":2,"maxRounds":24,"cvars":{},"team1":{"name":"Alpha","players":{"76561198000000001":"alice","76561198000000003":"charlie"}},"team2":{"name":"Bravo","players":{"76561198000000002":"bob"}}}})";
    g_players = {{2, 2, 76561198000000001ull, 3, false, "alice"},
                 {3, 3, 76561198000000002ull, 2, false, "bob"},
                 {9, 99, 0, 1, true, "SourceTV"}};
    std::thread http;
    const int port = ServeOnce(body, &http);
    rp::TryDispatchRu(true, 0, "Console", "ru match load http://127.0.0.1:" + std::to_string(port) + "/mixed.json");
    rp::Frame(true);
    http.join();
    Check(FramesUntil([] { return Has(Summary(), "\"ru_mode\":\"match_warmup\""); }, 2000), "mixed: match loaded");
    ClearCmds();
    rp::LifecycleEvent mapStart;
    mapStart.map = "de_mixed";
    mapStart.type = RU_EVENT_MAP_START;
    rp::PostEvent(mapStart);
    Check(FramesUntil([] { return Sent("bot_join_team CT") && Sent("bot_quota 1"); }, 7000), "mixed: first empty slot gets a CT bot");
    g_players.push_back({10, 110, 0, 3, true, "CT bot"});
    Check(FramesUntil([] { return Sent("bot_join_team T") && Sent("bot_quota 2"); }, 3000), "mixed: the other empty slot gets a T bot");
    g_players.push_back({11, 111, 0, 2, true, "T bot"});
    Check(!Sent("bot_kick") && !Sent("kickid 99"), "mixed: SourceTV is preserved");
    Check(Has(Summary(), "\"ru_mode\":\"match_warmup\""), "mixed: bots do not ready up for humans");
    ClearCmds();
    // Joining from spectator must free the roster side before the human tries to take it.
    g_players.push_back({4, 4, 76561198000000003ull, 1, false, "charlie"});
    Check(FramesUntil([] { return Sent("kickid 110"); }, 3000), "mixed: arriving human displaces only the excess CT bot");
    Check(Sent("bot_quota 1") && !Sent("kickid 111") && !Sent("kickid 4") && !Sent("kickid 99"),
          "mixed: the other side, humans and SourceTV remain");
    g_players.pop_back();
    g_players.erase(g_players.begin() + 3);  // engine applied kickid 110
    ClearCmds();
    Check(FramesUntil([] { return Sent("bot_join_team CT") && Sent("bot_quota 2"); }, 3000), "mixed: human disconnect refills the empty slot");
    // A map start clears the old quota, then fills the new map after its cfg has settled.
    g_players.erase(g_players.begin() + 3);  // old map's T bot
    ClearCmds();
    mapStart.map = "de_mixed2";
    rp::PostEvent(mapStart);
    Check(FramesUntil([] { return Sent("bot_quota 0") && Sent("bot_join_team CT"); }, 7000), "mixed: map change refills after setup");
    Check(!Sent("bot_kick"), "mixed: map setup keeps SourceTV");
    ClearCmds();
    rp::TryDispatchRu(true, 0, "Console", "ru match end");
    Check(FramesUntil([] { return Sent("bot_join_after_player 1"); }, 2000), "mixed: end restores bot defaults");
    Check(Sent("bot_quota 0") && !Sent("bot_kick"), "mixed: end removes gameplay bots and preserves SourceTV");
  }

  // Return with match.so still loaded and its workers running, like a server `quit` (the core never
  // unloads plugins): the plugin's exit handler must join them before its statics are destroyed,
  // or exit() aborts (joinable std::thread) or hangs (a worker waiting on a destroyed condvar).
  std::system(("rm -rf '" + std::string(root) + "'").c_str());
  std::printf("%s\n", g_failed ? "match_host_test: FAIL" : "match_host_test: PASS");
  return g_failed ? 1 : 0;
}

namespace readyup::plugins::hooks {
void DropPluginHooks(int, const std::string&) {}  // plugin_hooks.cpp
}  // namespace readyup::plugins::hooks

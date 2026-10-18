// Offline test for the plugin host (src/readyup/plugin_loader.cpp) - no CS2 server needed.
//
// Links the real loader against stubs of the few core functions it calls, then drives it
// the way the GameFrame hook does: load from a plugins dir, dispatch a chat command, a
// console command and lifecycle events, tick, then hot-swap hello.so for a rebuilt copy
// and `reload` it, checking that the new code runs and the old image was really unmapped.
//
//   build/readyup_plugin_host_test <hello.so v1> <hello.so v2>
//
// Run by `ctest` (see CMakeLists.txt).
#include "readyup/plugin_loader.h"
#include "readyup/plugin_needs.h"
#include "readyup/plugin_needs_iface.h"

#include "readyup/plugin_api.h"

#include <dlfcn.h>
#include <sys/stat.h>
#include <unistd.h>

#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <optional>
#include <string>
#include <vector>

static std::vector<std::string> g_log;   // everything the core would print to the console
static std::vector<std::string> g_chat;  // chat_all / chat_to_slot output
static std::vector<std::string> g_serverCmds;  // server_command / cvar_query output (the engine command buffer)

static void Capture(const char* fmt, va_list ap) {
  char buf[2048];
  std::vsnprintf(buf, sizeof(buf), fmt, ap);
  std::string s(buf);
  while (!s.empty() && s.back() == '\n') s.pop_back();
  std::printf("  | %s\n", s.c_str());
  g_log.push_back(s);
}

// ---- stubs for the core functions plugin_loader.cpp uses ---------------------------
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
void SendToChat(const char* msg) { g_chat.push_back(std::string("[all] ") + msg); }
void SendRawToChat(const char* msg) { g_chat.push_back(std::string("[raw] ") + msg); }
void SendImportantToChat(const char* msg) { g_chat.push_back(std::string("[important] ") + msg); }
bool ClientPrintChat(int slot, const char* msg) {
  g_chat.push_back("[slot " + std::to_string(slot) + "] " + msg);
  return true;
}
bool SendToSlotChat(int slot, const char* msg) { return ClientPrintChat(slot, msg); }
bool EnqueueServerCommand(const char* c) {
  g_serverCmds.push_back(c);
  return true;
}
std::optional<int> GameEventsSlotForSteam(unsigned long long steamid64) {
  if (steamid64 == 76561198000000001ull) return 4;
  return std::nullopt;
}
std::string GetCsgoDirFromModuleDir() { return {}; }
static std::string g_moduleDir;  // holds the test's readyup.cfg
std::string GetThisModuleDir() { return g_moduleDir; }
bool IsCoreChatCommand(const std::string& t) { return t == ".ru" || t == ".r" || t == ".ready"; }
}  // namespace readyup

namespace readyup::plugins::hooks {
void DropPluginHooks(int, const std::string&) {}  // plugin_hooks.cpp
}  // namespace readyup::plugins::hooks

// ---- stub for the engine-facing API members (plugin_engine_api.cpp) -----------------
// A fake game event: accessors read from this struct instead of an engine IGameEvent.
struct FakeEvent {
  int attacker, userid, headshot;
  const char* weapon;
};
static const FakeEvent* Fake(const ru_game_event* ev) { return reinterpret_cast<const FakeEvent*>(ev); }
namespace readyup::plugins::detail {
void FillEngineApi(ru_api* a) {
  a->ev_get_player_slot = [](ru_plugin*, const ru_game_event* ev, const char* key) {
    return std::string(key) == "attacker" ? Fake(ev)->attacker : Fake(ev)->userid;
  };
  a->ev_get_string = [](ru_plugin*, const ru_game_event* ev, const char*, const char*) { return Fake(ev)->weapon; };
  a->ev_get_int = [](ru_plugin*, const ru_game_event* ev, const char*, int) { return Fake(ev)->headshot; };
  a->feature_state = [](ru_plugin*, const char* name) { return std::string(name) == "events_live" ? 1 : -1; };
  // v1.12: no engine here, so no grenade type is available (hello_nade must say so, not crash).
  a->grenade_spawn_available = [](ru_plugin*, uint32_t) { return 0; };
}
}  // namespace readyup::plugins::detail

// ---- helpers -----------------------------------------------------------------------
static int g_failed = 0;

static bool Logged(const std::string& needle) {
  for (const auto& l : g_log) {
    if (l.find(needle) != std::string::npos) return true;
  }
  return false;
}
static bool Chatted(const std::string& needle) {
  for (const auto& l : g_chat) {
    if (l.find(needle) != std::string::npos) return true;
  }
  return false;
}
static void Check(bool ok, const char* what) {
  std::printf("%s %s\n", ok ? "PASS" : "FAIL", what);
  if (!ok) ++g_failed;
}
static bool CopyFile(const std::string& from, const std::string& to) {
  // Write to a temp name and rename into place, like scripts/dev-deploy.sh does.
  FILE* in = std::fopen(from.c_str(), "rb");
  if (!in) return false;
  const std::string tmp = to + ".tmp";
  FILE* out = std::fopen(tmp.c_str(), "wb");
  if (!out) {
    std::fclose(in);
    return false;
  }
  char buf[65536];
  size_t n;
  while ((n = std::fread(buf, 1, sizeof(buf), in)) > 0) std::fwrite(buf, 1, n, out);
  std::fclose(in);
  std::fclose(out);
  return std::rename(tmp.c_str(), to.c_str()) == 0;
}

namespace rp = readyup::plugins;

int main(int argc, char** argv) {
  if (argc < 3) {
    std::fprintf(stderr, "usage: %s <hello.so v1> <hello.so v2>\n", argv[0]);
    return 2;
  }
  char tmpl[] = "/tmp/ru-plugin-test.XXXXXX";
  const char* dir = mkdtemp(tmpl);
  if (!dir) return 2;
  setenv("READYUP_PLUGINS_DIR", dir, 1);
  const std::string so = std::string(dir) + "/hello.so";
  if (!CopyFile(argv[1], so)) return 2;

  std::puts("-- first frame loads everything in the plugins dir");
  rp::Frame(false);
  Check(Logged("plugin: loaded hello 1.2.0"), "hello 1.2.0 loaded on first frame");
  Check(Logged("load #1, greeting \"hello\""), "first load: no stash, default greeting");

  std::puts("-- chat command");
  Check(!rp::TryDispatchChat(76561198000000001ull, "alice", ".r"), "core command .r is not taken by plugins");
  Check(rp::TryDispatchChat(76561198000000001ull, "alice", ".hello there"), ".hello is owned by the plugin");
  Check(!rp::TryDispatchChat(0, "Console", ".hello"), "console sender never triggers chat commands");
  Check(g_chat.empty(), "callback deferred to the next frame");
  rp::Frame(true);
  Check(Chatted("[slot 4] hello, alice!"), ".hello replied to the sender's slot");

  std::puts("-- lifecycle events + ticks");
  rp::LifecycleEvent map;
  map.type = RU_EVENT_MAP_START;
  map.source = RU_SOURCE_LOG;
  map.map = "de_dust2";
  rp::PostEvent(map);
  rp::PostEvent(map);  // "Loading map" + "Started map": delivered once
  rp::LifecycleEvent rs;
  rs.type = RU_EVENT_ROUND_START;
  rs.round = 1;
  rp::PostEvent(rs);
  rp::PostLogLine("L 01/01/2026 - 00:00:00: World triggered \"Round_Start\"");
  rp::PostLogLine("L 01/01/2026 - 00:00:01: server cvars start");
  for (int i = 0; i < 5; ++i) rp::Frame(true);
  int mapEvents = 0;
  for (const auto& l : g_log) mapEvents += l.find("event map_start (log) map=de_dust2") != std::string::npos;
  Check(mapEvents == 1, "map_start delivered exactly once");
  Check(Logged("event round_start (engine) map=de_dust2"), "round_start delivered with current map");

  std::puts("-- raw engine event (synchronous)");
  const auto wanted = rp::WantedGameEvents();
  Check(wanted.size() == 1 && wanted[0] == "player_death", "player_death is in the wanted set for the listener");
  FakeEvent death{3, 7, 1, "ak47"};
  rp::DispatchGameEvent("player_other", &death);
  rp::DispatchGameEvent("player_death", &death);
  Check(Logged("player_death attacker=3 victim=7 weapon=ak47 headshot=1"), "player_death delivered with accessors");
  Check(!Logged("player_other"), "events nobody subscribed to are not delivered");

  std::puts("-- console command");
  Check(rp::TryDispatchConsole("hello_status"), "hello_status is owned by the plugin");
  rp::Frame(false);
  Check(Logged("status: version 1.2.0, load #1, 6 ticks, 7 frames, 1 greetings, 2 log lines, map de_dust2"),
        "console command ran; 6 simulating ticks, 7 frames (on_frame also on non-simulating ones), 2 log lines, current map");

  std::puts("-- v1.2: ru subcommand, hidden chat command, console observer");
  Check(rp::TryDispatchRu(true, 0, "Console", "ru hello world"), "`ru hello` is owned by the plugin");
  Check(!rp::TryDispatchRu(true, 0, "Console", "ru nope"), "unknown ru subcommand is not taken");
  Check(rp::TryDispatchRu(false, 76561198000000001ull, "alice", ".ru hello there", 6), "`.ru hello` from chat too");
  rp::Frame(false);
  Check(Logged("hello-ru: world via console (argc=3, slot=-1)"), "ru subcommand ran from the console, logged untagged");
  Check(Logged("hello-ru: there via chat (argc=3, slot=6)"), "ru subcommand ran from chat with the sender slot");
  Check(!Logged("plugin[hello]: hello-ru"), "log_untagged has no plugin tag");
  uint32_t flags = 0;
  Check(rp::ChatCommandOwned(".hellohide", &flags) && (flags & RU_CMD_HIDE), ".hellohide is owned and hidden");
  Check(rp::ChatCommandOwned(".hello", &flags) && flags == 0, ".hello is owned and visible");
  Check(!rp::ChatCommandOwned(".nope", &flags), "unknown chat command is not owned");
  g_chat.clear();
  Check(rp::TryDispatchChat(76561198000000001ull, "alice", ".hellohide", 9), ".hellohide dispatched with slot 9");
  rp::Frame(false);
  Check(Chatted("[slot 9] psst"), "sender slot from the router reached the plugin");
  Check(!rp::TryDispatchConsole("sv_cheats 1"), "an observed console command is not consumed");
  rp::ObserveConsole("sv_cheats 1");
  rp::ObserveConsole("sv_gravity 800");
  rp::Frame(false);
  Check(Logged("observed console: sv_cheats 1"), "console observer saw its command");
  Check(!Logged("observed console: sv_gravity"), "console observer only sees its own command");
  {
    const auto subs = rp::PluginRuSubcommands();
    Check(subs.size() == 1 && subs[0] == "hello (hello)", "ru subcommand listed for ru help");
  }

  std::puts("-- v1.11: cvar_query line parsing");
  {
    std::string v;
    Check(rp::ParseCvarAnswer("mp_maxrounds = 24\n", "mp_maxrounds", &v) == 1 && v == "24", "`name = value` answers");
    Check(rp::ParseCvarAnswer("hostname = My Server #1", "hostname", &v) == 1 && v == "My Server #1",
          "value keeps its spaces");
    Check(rp::ParseCvarAnswer("mp_t_default_primary = ", "mp_t_default_primary", &v) == 1 && v.empty(),
          "empty string value");
    Check(rp::ParseCvarAnswer("mp_t_default_primary =\r\n", "mp_t_default_primary", &v) == 1 && v.empty(),
          "empty value with the line end trimmed");
    Check(rp::ParseCvarAnswer("MP_MaxRounds = 30", "mp_maxrounds", &v) == 1 && v == "30", "name is case-insensitive");
    Check(rp::ParseCvarAnswer("mp_maxrounds = 24", "mp_maxround", &v) == -1, "a longer name is another cvar");
    Check(rp::ParseCvarAnswer("mp_maxrounds", "mp_maxrounds", &v) == -1, "the echoed command is no answer");
    Check(rp::ParseCvarAnswer("Unknown command 'mp_nope'!", "mp_nope", &v) == 0, "`Unknown command` = unknown");
    Check(rp::ParseCvarAnswer("Unknown command 'mp_nope2'!", "mp_nope", &v) == -1, "another unknown name is unrelated");
    Check(rp::ParseCvarAnswer("L 01/01/2026 - 00:00:00: server_cvar: \"mp_maxrounds\" \"24\"", "mp_maxrounds", &v) == -1,
          "server_cvar log lines are not answers");
    Check(rp::ValidCvarName("sv_cheats") && rp::ValidCvarName("tv.delay") && !rp::ValidCvarName("") &&
              !rp::ValidCvarName("sv_cheats;quit") && !rp::ValidCvarName("a b") && !rp::ValidCvarName(std::string(64, 'a')),
          "cvar names: [A-Za-z0-9_.], 1..63");
  }

  std::puts("-- v1.11: cvar_query through the plugin (hello_cvar)");
  g_serverCmds.clear();
  Check(rp::TryDispatchConsole("hello_cvar mp_maxrounds"), "hello_cvar is owned by the plugin");
  rp::Frame(false);
  Check(g_serverCmds.size() == 1 && g_serverCmds[0] == "mp_maxrounds", "the query went out as the bare cvar name");
  Check(!Logged("cvar mp_maxrounds"), "no answer before the engine printed one");
  rp::PostLogLine("mp_maxrounds = 24\n");
  rp::PostLogLine("mp_maxrounds = 99\n");  // only the first answer counts
  rp::Frame(false);
  Check(Logged("cvar mp_maxrounds = \"24\""), "the plugin got the value from the console line");
  Check(!Logged("cvar mp_maxrounds = \"99\""), "answered once");
  rp::TryDispatchConsole("hello_cvar mp_t_default_primary");
  rp::TryDispatchConsole("hello_cvar mp_nope");
  rp::Frame(false);
  rp::PostLogLine("mp_t_default_primary = ");
  rp::PostLogLine("Unknown command 'mp_nope'!");
  rp::Frame(false);
  Check(Logged("cvar mp_t_default_primary = \"\""), "empty string value delivered as \"\"");
  Check(Logged("cvar mp_nope: unknown"), "unknown cvar delivered as NULL");
  g_serverCmds.clear();
  rp::TryDispatchConsole("hello_cvar sv_cheats;quit");
  rp::Frame(false);
  Check(Logged("cvar sv_cheats;quit: query refused") && g_serverCmds.empty(), "a name with ';' is refused, nothing queued");
  rp::TryDispatchConsole("hello_cvar sv_silent");
  rp::Frame(false);
  usleep(3200 * 1000);
  rp::Frame(false);
  Check(Logged("cvar sv_silent: unknown"), "no answer within 3 s: delivered as NULL");

  std::puts("-- v1.11: selftest_summary (hello_selftest)");
  rp::TryDispatchConsole("hello_selftest");
  rp::Frame(false);
  Check(Logged("selftest: none yet"), "no provider / no run yet: -1");
  rp::SetSelftestJsonProvider([] {
    return std::string("{\"pass\":false,\"passed\":40,\"total\":41,\"failures\":[\"fn:Host_Say\"],\"ran_at\":1}");
  });
  rp::TryDispatchConsole("hello_selftest");
  rp::Frame(false);
  Check(Logged("selftest: {\"pass\":false,\"passed\":40,\"total\":41,\"failures\":[\"fn:Host_Say\"],\"ran_at\":1}"),
        "selftest_summary returns the core's JSON");
  rp::SetSelftestJsonProvider(nullptr);

  std::puts("-- v1.12: grenade_spawn_available (hello_nade)");
  rp::TryDispatchConsole("hello_nade 3 0");
  rp::Frame(false);
  Check(Logged("nade: type 3 not available on this CS2 build"), "an unavailable grenade type is reported, nothing spawned");

  std::puts("-- list");
  rp::HandlePluginCommand({"list"}, false);
  Check(Logged("hello 1.2.0 (api 1.0) cmds=8 ticks=2 subs=3"), "list shows the plugin and its registrations");

  std::puts("-- hot reload with a rebuilt hello.so");
  if (!CopyFile(argv[2], so)) return 2;
  readyup::g_moduleDir = dir;  // config_get: readyup.cfg [hello] section
  {
    FILE* cfg = std::fopen((std::string(dir) + "/readyup.cfg").c_str(), "w");
    if (!cfg) return 2;
    std::fputs("debug=0\ngreeting=not-for-plugins\n[other]\ngreeting=wrong\n[hello]\ngreeting = \"howdy\"\n", cfg);
    std::fclose(cfg);
  }
  g_chat.clear();
  rp::TryDispatchConsole("hello_cvar sv_gravity");  // pending across the reload: dropped, never delivered
  rp::Frame(false);
  rp::HandlePluginCommand({"reload", "hello"}, false);
  Check(!Logged("reloaded hello"), "reload is deferred to the next frame");
  rp::TryDispatchChat(76561198000000001ull, "alice", ".hello");  // queued before the reload runs
  rp::Frame(true);
  Check(Logged("unloading after 6 ticks, 1 greetings"), "old image got its unload call");
  Check(!Logged("image is still mapped"), "old image really unmapped by dlclose");
  Check(Logged("plugin: loaded hello 1.0.1-reloaded"), "new code loaded");
  Check(Logged("reloaded hello"), "reload reported");
  Check(g_chat.empty(), "command queued for the old image was dropped, not run against the new one");
  rp::PostLogLine("sv_gravity = 800");
  rp::Frame(false);
  Check(!Logged("cvar sv_gravity"), "a cvar_query pending at unload is dropped, not delivered to the new image");
  rp::TryDispatchChat(76561198000000001ull, "alice", ".hello");
  rp::Frame(true);
  Check(Chatted("readyup-hello 1.0.1-reloaded, greeting #1"), "new code answers .hello with fresh state");
  Check(Logged("load #2, greeting \"howdy\""), "stash survived the reload; config_get read the [hello] section");
  Check(Chatted("howdy, alice!"), "configured greeting used");
  Check(rp::WantedGameEvents().size() == 1, "old image's game event subscription was dropped (not doubled)");

  std::puts("-- unload removes every registration");
  rp::HandlePluginCommand({"unload", "hello"}, false);
  rp::Frame(true);
  Check(Logged("unloaded hello"), "unloaded");
  Check(!rp::TryDispatchChat(76561198000000001ull, "alice", ".hello"), ".hello no longer routed");
  Check(!rp::TryDispatchConsole("hello_status"), "hello_status no longer routed");
  Check(dlopen(so.c_str(), RTLD_NOW | RTLD_NOLOAD) == nullptr, "hello.so no longer mapped");
  Check(rp::WantedGameEvents().empty(), "game event subscription removed on unload");
  Check(!rp::TryDispatchRu(true, 0, "Console", "ru hello"), "ru subcommand removed on unload");
  g_log.clear();
  rp::DispatchGameEvent("player_death", &death);
  Check(!Logged("player_death"), "no delivery into the unloaded image");

  std::puts("-- enable / disable are remembered (plugins.json) and the boot scan skips disabled");
  g_log.clear();
  rp::HandlePluginCommand({"disable", "hello"}, false);
  rp::Frame(false);
  Check(Logged("disabled hello (stays off after a restart"), "disable reported");
  Check(Logged("unloaded hello") || !rp::TryDispatchChat(76561198000000001ull, "alice", ".hello"), "disable unloads it");
  {
    std::string st;
    if (FILE* f = std::fopen((std::string(dir) + "/plugins.json").c_str(), "r")) {
      char buf[512];
      size_t n = std::fread(buf, 1, sizeof buf, f);
      st.assign(buf, n);
      std::fclose(f);
    }
    Check(st.find("\"hello\"") != std::string::npos, "plugins.json lists hello");
  }
  rp::HandlePluginCommand({"list"}, false);
  Check(Logged("disabled (plugins.json): hello"), "list shows the disabled plugin");
  g_log.clear();
  rp::LoadAllFromDirForTest();
  Check(Logged("hello is disabled (plugins.json)") && !Logged("plugin: loaded hello"), "boot scan skips a disabled plugin");
  rp::HandlePluginCommand({"enable", "hello"}, false);
  rp::Frame(false);
  Check(Logged("enabled hello") && Logged("plugin: loaded hello"), "enable loads it again");
  rp::HandlePluginCommand({"unload", "hello"}, false);
  rp::Frame(false);
  unlink((std::string(dir) + "/plugins.json").c_str());

  std::puts("-- load errors");
  rp::HandlePluginCommand({"load", "nope"}, false);
  rp::Frame(false);
  Check(Logged("load nope failed: no such file"), "missing plugin reported");
  rp::HandlePluginCommand({"load", "hello"}, false);
  rp::Frame(false);
  Check(Logged("loaded hello"), "load after unload works");

  std::puts("-- needs.json: unmet needs keep a plugin out, with the reason everywhere");
  rp::HandlePluginCommand({"unload", "hello"}, false);
  rp::Frame(false);
  const std::string needsPath = std::string(dir) + "/hello.needs.json";
  if (FILE* f = std::fopen(needsPath.c_str(), "w")) {
    std::fputs("{\"plugin\":\"hello\",\"surface\":[\"Host_Say\",\"Gone_Fn\"],"
               "\"schema_optional\":[\"CFoo.m_bar\"],\"events\":[\"player_death\"]}", f);
    std::fclose(f);
  }
  static std::vector<std::string> s_notices;
  rp::SetNeedsProbeProvider([] {
    rp::NeedsProbe p;
    p.surface = [](const std::string& n) { return n == "Gone_Fn" ? 0 : 1; };
    p.schema = [](const std::string&, const std::string&) { return -1; };
    p.event = [](const std::string&) { return 1; };
    p.cs2Build = "12345";
    return p;
  });
  rp::SetNeedsAdminNotifier([](const std::vector<std::string>& lines, uint64_t) {
    s_notices.insert(s_notices.end(), lines.begin(), lines.end());
  });
  g_log.clear();
  rp::LoadAllFromDirForTest();
  Check(Logged("WARN plugin[hello] disabled: missing Gone_Fn after CS2 build 12345") && !Logged("plugin: loaded hello"),
        "boot scan refuses a plugin whose needed surface entry is missing");
  Check(Logged("optional schema field CFoo.m_bar not found"), "missing optional schema field only warns");
  Check(rp::GetPluginHostStatus().failures.empty(), "an unmet-needs plugin is not a load failure");
  g_log.clear();
  rp::HandlePluginCommand({"list"}, false);
  Check(Logged("disabled (needs.json not met): hello - missing Gone_Fn after CS2 build 12345"), "ru plugin list shows why");
  {
    const auto* ni = static_cast<const ru_plugin_needs_iface_v1*>(
        rp::CoreGetInterface(RU_PLUGIN_NEEDS_IFACE_NAME, RU_PLUGIN_NEEDS_IFACE_VERSION));
    char buf[256] = {0};
    Check(ni && ni->disabled_json(buf, sizeof(buf)) > 0 &&
              std::string(buf) == "[{\"name\":\"hello\",\"reason\":\"missing Gone_Fn after CS2 build 12345\"}]",
          "core interface lists the disabled plugin as JSON");
  }
  rp::PostEvent(rp::LifecycleEvent{RU_EVENT_MAP_START, 0, -1, 0, "", "de_needs"});
  rp::Frame(false);
  Check(s_notices.size() == 1 && s_notices[0] == "plugin[hello] disabled: missing Gone_Fn after CS2 build 12345",
        "admins are told after a map starts");
  g_log.clear();
  rp::HandlePluginCommand({"load", "hello"}, false);
  rp::Frame(false);
  Check(Logged("load hello failed: disabled: missing Gone_Fn"), "a manual load is refused too");
  unlink(needsPath.c_str());
  rp::HandlePluginCommand({"load", "hello"}, false);
  rp::Frame(false);
  Check(Logged("loaded hello") && rp::NeedsDisabledPlugins().empty(), "no needs.json: loads as before");
  rp::SetNeedsProbeProvider(nullptr);
  rp::SetNeedsAdminNotifier(nullptr);

  unlink(so.c_str());
  unlink((std::string(dir) + "/readyup.cfg").c_str());
  rmdir(dir);
  std::printf("%s (%d failed)\n", g_failed ? "FAILED" : "ALL OK", g_failed);
  return g_failed ? 1 : 0;
}

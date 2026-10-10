// fleet.so inside the real plugin host (core/src/readyup/plugin_loader.cpp with stubbed core
// functions, like tools/plugin_host_test.cpp), against the mock platform:
//   1. no [fleet] url: loads, stays idle, `fleet status` says standalone, selftest INFO line
//   2. [fleet] url + enroll_code: `ru plugin reload fleet` -> enrolls, connects, selftest OK,
//      `fleet status` shows it online, the network thread stops on unload
//   build/plugins/fleet/fleet_host_test <fleet.so>
#include "readyup/plugin_loader.h"

#include "readyup/fleet_iface.h"
#include "readyup/plugin_api.h"

#include "mock_platform.h"

#include <sys/stat.h>
#include <unistd.h>

#include <chrono>
#include <cstddef>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <functional>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

static std::mutex g_logMu;
static std::vector<std::string> g_log;

static void Capture(const char* fmt, va_list ap) {
  char buf[4096];
  std::vsnprintf(buf, sizeof(buf), fmt, ap);
  std::string s(buf);
  while (!s.empty() && s.back() == '\n') s.pop_back();
  std::printf("  | %s\n", s.c_str());
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
bool DebugEnabled() { return true; }
void SendToChat(const char*) {}
void SendRawToChat(const char*) {}
bool ClientPrintChat(int, const char*) { return true; }
bool SendToSlotChat(int, const char*) { return true; }
bool EnqueueServerCommand(const char*) { return true; }
std::optional<int> GameEventsSlotForSteam(unsigned long long) { return std::nullopt; }
std::string GetCsgoDirFromModuleDir() { return {}; }
static std::string g_moduleDir;
std::string GetThisModuleDir() { return g_moduleDir; }
bool IsCoreChatCommand(const std::string& t) { return t == ".ru"; }
}  // namespace readyup

// ru_api set_core_setting (v1.10): what fleet.so pushed last (server.config status_http.token).
static std::mutex g_coreMu;
static std::string g_coreKey, g_coreValue;
static int g_coreCalls = 0;

namespace readyup::plugins::detail {
void FillEngineApi(ru_api* a) {
  a->for_each_player = [](ru_plugin*, ru_player_fn, void*) { return 0; };
  a->set_core_setting = [](ru_plugin*, const char* key, const char* value) {
    std::lock_guard<std::mutex> lk(g_coreMu);
    g_coreKey = key ? key : "";
    g_coreValue = value ? value : "";
    ++g_coreCalls;
    return 1;
  };
}
}  // namespace readyup::plugins::detail

namespace rp = readyup::plugins;

// Fake core selftest record for selftest_summary (status_feed.cpp keeps the real one).
static std::mutex g_selftestMu;
static std::string g_selftestJson =
    R"({"pass":true,"passed":58,"total":58,"failures":[],"summary":"58 OK","ran_at":1})";
static std::string CoreSelftestJson() {
  std::lock_guard<std::mutex> lk(g_selftestMu);
  return g_selftestJson;
}

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
static bool AnyLogContains(const std::string& needle) { return Logged(needle); }
static void ClearLog() {
  std::lock_guard<std::mutex> lk(g_logMu);
  g_log.clear();
}
static bool FramesUntil(const std::function<bool()>& pred, int ms) {
  const auto end = std::chrono::steady_clock::now() + std::chrono::milliseconds(ms);
  while (std::chrono::steady_clock::now() < end) {
    rp::Frame(true);
    if (pred()) return true;
    std::this_thread::sleep_for(std::chrono::milliseconds(15));
  }
  return pred();
}
static const rp::PluginSelftestCheck* FindCheck(const std::vector<rp::PluginSelftestCheck>& v, const std::string& name) {
  for (const auto& c : v) {
    if (c.plugin == "fleet" && c.name == name) return &c;
  }
  return nullptr;
}

int main(int argc, char** argv) {
  if (argc < 2) {
    std::fprintf(stderr, "usage: %s <fleet.so>\n", argv[0]);
    return 2;
  }
  char tmpl[] = "/tmp/ru-fleet-host.XXXXXX";
  const char* dir = mkdtemp(tmpl);
  if (!dir) return 2;
  setenv("READYUP_PLUGINS_DIR", dir, 1);
  readyup::g_moduleDir = dir;
  const std::string so = std::string(dir) + "/fleet.so";
  if (std::system(("cp '" + std::string(argv[1]) + "' '" + so + "'").c_str()) != 0) return 2;

  std::puts("-- standalone: no [fleet] url");
  rp::Frame(false);
  Check(Logged("plugin: loaded fleet"), "fleet.so loaded");
  Check(Logged("standalone (no [fleet] url); idle"), "standalone, idle");
  Check(!Logged("error"), "no errors in standalone mode");
  Check(rp::TryDispatchConsole("fleet status"), "`fleet` console command is owned by the plugin");
  rp::Frame(false);
  Check(Logged("fleet: standalone (no [fleet] url)"), "`fleet status` reports standalone");
  auto checks = rp::RunPluginSelftests();
  const auto* link = FindCheck(checks, "link");
  Check(link && link->status == "INFO", "selftest: fleet link INFO (standalone)");
  struct stat st {};
  Check(stat((std::string(dir) + "/fleet/credentials.json").c_str(), &st) != 0, "standalone writes no credentials");
  {
    // What the core's /status does (status_feed.cpp): get_status 0 -> platform.mode "standalone".
    const auto* f = static_cast<const ru_fleet_v1*>(rp::CoreGetInterface(RU_FLEET_IFACE_NAME, RU_FLEET_IFACE_VERSION));
    ru_fleet_status fs{};
    fs.struct_size = sizeof(fs);
    Check(f && f->get_status && f->get_status(&fs) == 0, "standalone: readyup.fleet.v1 get_status returns 0");
    // No platform: demos are not streamed (the match plugin records / keeps / uploads as before).
    const std::string spec = std::string(R"({"match_id":"m1","epoch":1,"map_number":1,"path":")") + dir +
                             R"(/standalone.dem","started_at":1})";
    Check(f && f->struct_size >= offsetof(ru_fleet_v1, demo_stream_end) + sizeof(f->demo_stream_end) &&
              f->demo_stream_begin(spec.c_str()) == 0 && f->demo_stream_end((std::string(dir) + "/standalone.dem").c_str()) == 0,
          "standalone: demo_stream_begin / _end return 0 (demos handled as without a platform)");
    Check(stat((std::string(dir) + "/fleet/demos.json").c_str(), &st) != 0, "standalone writes no demos.json");
  }

  std::puts("-- fleet mode: enroll + connect to the mock platform");
  mock::Platform platform;
  if (!platform.Start()) return 2;
  platform.heartbeatIntervalMs = 500;
  {
    FILE* cfg = std::fopen((std::string(dir) + "/readyup.cfg").c_str(), "w");
    std::fprintf(cfg, "debug=1\n[fleet]\nurl=%s\ninsecure_dev=1\nenroll_code=RUE-HOST-TEST-CODE\noffline_pause_minutes=3\n",
                 platform.BaseUrl().c_str());
    std::fclose(cfg);
  }
  // The core's latest selftest (ru_api 1.11 selftest_summary), as status_feed.cpp records it.
  rp::SetSelftestJsonProvider(&CoreSelftestJson);
  ClearLog();
  rp::HandlePluginCommand({"reload", "fleet"}, false);
  rp::Frame(false);
  Check(Logged("plugin: reloaded fleet"), "reloaded with the [fleet] section");
  Check(FramesUntil([] { return AnyLogContains("fleet: online"); }, 8000), "enrolled and online");
  Check(platform.enrollments() == 1, "one enrollment request");
  Check(stat((std::string(dir) + "/fleet/credentials.json").c_str(), &st) == 0 && (st.st_mode & 0777) == 0600,
        "credentials.json written with mode 0600");
  Check(!AnyLogContains(platform.token) && !AnyLogContains("HOST-TEST-CODE"), "no secrets in the log");
  Check(platform.WaitFor([&] { return platform.MessagesOfType("ping").size() >= 1; }, 3000), "pings flowing");
  const auto hellos = platform.MessagesOfType("hello");
  Check(!hellos.empty() && hellos[0].payload()->Get("versions")->Get("plugins")->Get("fleet") != nullptr,
        "hello carries the plugin versions");
  Check(!hellos.empty() && hellos[0].payload()->Get("availability")->AsStr() == "busy" &&
            hellos[0].payload()->Get("reason") && hellos[0].payload()->Get("reason")->AsStr() == "match_disabled",
        "no match plugin: hello says busy, match_disabled");
  Check(platform.SendToServer("server.drain", R"({"reason":"cs2 update"})", true), "server.drain sent");
  Check(FramesUntil([&] {
          for (const auto& m : platform.MessagesOfType("server.availability"))
            if (m.payload()->Get("reason")->AsStr() == "draining" && m.payload()->Get("detail") &&
                m.payload()->Get("detail")->AsStr() == "cs2 update" && m.payload()->Get("availability")->AsStr() == "draining")
              return true;
          return false;
        }, 3000),
        "drain without the match plugin: draining, detail = drain reason");
  Check(platform.SendToServer("server.undrain", "{}", true), "server.undrain sent");
  Check(FramesUntil([&] {
          const auto a = platform.MessagesOfType("server.availability");
          return !a.empty() && a.back().payload()->Get("availability")->AsStr() == "busy" &&
                 a.back().payload()->Get("reason")->AsStr() == "match_disabled";
        }, 3000),
        "undrain without the match plugin: back to busy, match_disabled");
  {
    const auto* st = hellos.empty() ? nullptr : hellos[0].payload()->Get("selftest");
    Check(st && st->Get("pass") && st->Get("pass")->AsBool() && st->Get("passed")->AsInt() == 58 &&
              st->Get("total")->AsInt() == 58 && st->Get("failures")->IsArr() && !st->Get("summary") &&
              !st->Get("ran_at"),
          "hello.selftest: the core's latest selftest (pass, passed, total, failures only)");
    Check(FramesUntil([&] { return platform.MessagesOfType("server.selftest").size() == 1; }, 8000),
          "server.selftest sent once for the first result");
    std::lock_guard<std::mutex> lk(g_selftestMu);
    g_selftestJson = R"({"pass":false,"passed":57,"total":58,"failures":["fn:Host_Say"],"summary":"1 FAIL","ran_at":2})";
  }
  Check(FramesUntil([&] { return platform.MessagesOfType("server.selftest").size() >= 2; }, 8000),
        "server.selftest sent again when the outcome changed");
  {
    const auto msgs = platform.MessagesOfType("server.selftest");
    const auto* p = msgs.size() >= 2 ? msgs[1].payload() : nullptr;
    Check(p && !p->Get("pass")->AsBool() && p->Get("passed")->AsInt() == 57 && p->Get("failures")->a.size() == 1 &&
              p->Get("failures")->a[0].AsStr() == "fn:Host_Say" && msgs[1].env.Get("seq") != nullptr,
          "server.selftest: failing outcome, reliable (has a seq)");
    std::lock_guard<std::mutex> lk(g_selftestMu);
    g_selftestJson = R"({"pass":false,"passed":57,"total":58,"failures":["fn:Host_Say"],"summary":"1 FAIL","ran_at":3})";
  }
  FramesUntil([] { return false; }, 6000);
  Check(platform.MessagesOfType("server.selftest").size() == 2, "a re-run with the same outcome sends nothing");
  checks = rp::RunPluginSelftests();
  const auto* enrolled = FindCheck(checks, "enrolled");
  const auto* connected = FindCheck(checks, "connected");
  Check(enrolled && enrolled->status == "OK", "selftest: enrolled OK");
  Check(connected && connected->status == "OK", "selftest: connected OK");
  {
    const auto* f = static_cast<const ru_fleet_v1*>(rp::CoreGetInterface(RU_FLEET_IFACE_NAME, RU_FLEET_IFACE_VERSION));
    Check(f && f->struct_size >= offsetof(ru_fleet_v1, add_capability) + sizeof(f->add_capability),
          "readyup.fleet.v1 has the plugin members after get_status");
    ru_fleet_status fs{};
    fs.struct_size = sizeof(fs);
    Check(f && f->get_status(&fs) == 1 && fs.state == RU_FLEET_STATE_ONLINE && fs.link_state == RU_FLEET_LINK_ONLINE &&
              std::string(fs.server_id) == "srv_test_1" && fs.auto_pause_in_s == -1 && fs.sessions == 1,
          "get_status: online, server id, no auto-pause countdown");
    // A caller built against the first (shorter) ru_fleet_status gets only what fits.
    struct Old {
      uint32_t struct_size, state;
      uint64_t since_ms;
      uint32_t reconnects, spool_msgs;
      int32_t auto_pause_in_s, update_blocked;
      char server_id[64];
      uint32_t canary;
    } old{};
    old.struct_size = offsetof(Old, canary);
    old.canary = 0xDEADBEEFu;
    Check(f && f->get_status(reinterpret_cast<ru_fleet_status*>(&old)) == 1 && old.canary == 0xDEADBEEFu &&
              old.state == RU_FLEET_STATE_ONLINE && old.struct_size == offsetof(Old, canary),
          "get_status fills only what an older caller's struct has room for");
    Check(f && f->connection_state() == RU_FLEET_LINK_ONLINE, "connection_state() online");
    Check(f && f->send_event("event.test", "{\"n\":1}", 1, RU_FLEET_RELIABLE) == 1, "send_event queued");
    Check(platform.WaitFor([&] { return !platform.MessagesOfType("event.test").empty(); }, 2000),
          "send_event reached the platform with a seq");
    Check(f && f->publish_state("{\"match_id\":\"m9\"}", "busy") == 1 && f->publish_state("[1]", nullptr) == 0,
          "publish_state validates its JSON");
    Check(f && f->struct_size >= offsetof(ru_fleet_v1, publish_availability) + sizeof(f->publish_availability) &&
              f->publish_availability("busy", "local_match", "cup-final") == 1 &&
              f->publish_availability("maybe", nullptr, nullptr) == 0 &&
              f->publish_availability("busy", std::string(65, 'r').c_str(), nullptr) == 0,
          "publish_availability validates availability and lengths");
    {
      // An older match plugin registers match.assign and never publishes a reason: fleet.so
      // itself says the server is available again, a frame later.
      const size_t before = platform.MessagesOfType("server.availability").size();
      const uint64_t hid = f->register_handler("match.assign", +[](void*, const ru_fleet_msg*) {}, nullptr);
      Check(FramesUntil([&] {
              const auto a = platform.MessagesOfType("server.availability");
              return a.size() > before && a.back().payload()->Get("availability")->AsStr() == "available";
            }, 3000),
            "match.assign handler without a published reason: fleet.so clears match_disabled");
      f->unregister_handler(hid);
      Check(FramesUntil([&] {
              const auto a = platform.MessagesOfType("server.availability");
              return !a.empty() && a.back().payload()->Get("reason")->AsStr() == "match_disabled";
            }, 3000),
            "the last match.assign handler gone: match_disabled again");
    }
    const std::string demoPath = std::string(dir) + "/host-demo.dem";
    const std::string spec = R"({"match_id":"m9","epoch":2,"map_number":1,"path":")" + demoPath + R"(","started_at":1})";
    Check(f->demo_stream_begin(spec.c_str()) == 1, "fleet mode: demo_stream_begin streams a platform demo");
    Check(f->demo_stream_begin(R"({"match_id":"m9","path":"relative.dem"})") == 0, "demo_stream_begin refuses a bad spec");
    Check(f->demo_stream_end(demoPath.c_str()) == 1, "demo_stream_end accepted");
  }
  ClearLog();
  Check(rp::TryDispatchConsole("fleet status"), "fleet status dispatched");
  rp::Frame(false);
  Check(Logged("fleet: online for"), "`fleet status` shows online");
  Check(Logged("server srv_test_1"), "`fleet status` shows the server id");

  std::puts("-- server.config: offline_pause_minutes and status_http.token (fleet.so's part)");
  {
    ClearLog();
    const std::string tok = "platform_status_token_0123456789";
    const int64_t ackBefore = platform.lastAckFromServer();
    Check(platform.SendToServer("server.config",
                                R"({"rev":2,"settings":{"offline_pause_minutes":5,"chat_prefix":"x",)"
                                R"("status_http":{"token":")" + tok + R"("}}})",
                                true),
          "server.config sent");
    Check(FramesUntil([&] {
      std::lock_guard<std::mutex> lk(g_coreMu);
      return g_coreKey == "status_http_token" && g_coreValue == tok;
    }, 3000), "status_http.token pushed to the core (set_core_setting)");
    Check(FramesUntil([] { return AnyLogContains("offline_pause_minutes=5 (platform)"); }, 2000),
          "offline_pause_minutes from the platform");
    Check(!AnyLogContains(tok), "the status token is never logged");
    Check(FramesUntil([&] { return platform.lastAckFromServer() > ackBefore; }, 8000), "server.config acked");
    Check(platform.MessagesOfType("error").empty(), "server.config is a handled type (no unknown_type)");
    Check(stat((std::string(dir) + "/fleet/server-config.json").c_str(), &st) == 0 && (st.st_mode & 0777) == 0600,
          "server-config.json written with mode 0600");
    // Reload: the saved values apply again before the platform sends anything.
    ClearLog();
    {
      std::lock_guard<std::mutex> lk(g_coreMu);
      g_coreValue.clear();
    }
    rp::HandlePluginCommand({"reload", "fleet"}, false);
    rp::Frame(false);
    Check(AnyLogContains("server.config (saved): offline_pause_minutes=5 (platform)"), "saved server.config restored on load");
    {
      std::lock_guard<std::mutex> lk(g_coreMu);
      Check(g_coreValue == tok, "saved status token pushed again on load");
    }
    Check(FramesUntil([] { return AnyLogContains("fleet: online"); }, 8000), "online again after the reload");
  }

  std::puts("-- reconnect command + platform restart");
  ClearLog();
  rp::TryDispatchConsole("fleet reconnect");
  Check(FramesUntil([&] { return platform.connections() >= 2 && AnyLogContains("fleet: online"); }, 5000),
        "`fleet reconnect` reconnects");

  std::puts("-- unload joins the network thread");
  rp::HandlePluginCommand({"unload", "fleet"}, false);
  rp::Frame(false);
  Check(Logged("plugin: unloaded fleet"), "unloaded");
  checks = rp::RunPluginSelftests();
  Check(FindCheck(checks, "link") == nullptr && FindCheck(checks, "connected") == nullptr,
        "selftest interface removed on unload");
  Check(platform.WaitFor([&] {
    const auto m = platform.Messages();
    return !m.empty();
  }, 100), "platform saw traffic");
  const int before = platform.connections();
  std::this_thread::sleep_for(std::chrono::milliseconds(1500));
  Check(platform.connections() == before, "no reconnect after unload (thread joined)");

  platform.Stop();
  std::system(("rm -rf '" + std::string(dir) + "'").c_str());
  std::printf("%s\n", g_failed ? "fleet_host_test: FAIL" : "fleet_host_test: PASS");
  return g_failed ? 1 : 0;
}

namespace readyup::plugins::hooks {
void DropPluginHooks(int, const std::string&) {}  // plugin_hooks.cpp
}  // namespace readyup::plugins::hooks

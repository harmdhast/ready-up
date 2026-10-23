#include "readyup/plugin_loader.h"
#include "readyup/plugin_hooks.h"

#include "readyup/chat.h"
#include "readyup/client_print.h"
#include "readyup/command_buffer_hook.h"
#include "readyup/config.h"
#include "readyup/game_events.h"
#include "readyup/logging.h"
#include "readyup/perf_stats.h"
#include "readyup/path.h"
#include "readyup/plugin_needs.h"
#include "readyup/plugin_state.h"
#include "readyup/plugin_needs_iface.h"
#include "readyup/ru_router.h"
#include "readyup/version.h"

#include "readyup/plugin_api.h"
#include "readyup/selftest_iface.h"

#include <dirent.h>
#include <dlfcn.h>
#include <sys/stat.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cctype>
#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <deque>
#include <exception>
#include <fstream>
#include <map>
#include <mutex>
#include <set>
#include <shared_mutex>
#include <string>
#include <thread>
#include <cstdio>
#include <vector>

// The opaque handle from core/include/readyup/plugin_api.h. Never freed: a plugin thread that outlives
// its unload (a plugin bug) then finds alive == false instead of freed memory.
struct ru_plugin {
  std::atomic<bool> alive{false};
  std::atomic<bool> unloading{false};
  int id = 0;
  char name[48] = {};
};

namespace readyup::plugins {
namespace {

// One loaded (or previously loaded) plugin image. Leaked on unload together with its
// ru_api, for the same reason as ru_plugin (a few hundred bytes per reload).
struct Instance {
  ru_plugin handle;
  std::string name;
  std::string path;
  std::string version;
  std::string author;
  std::string description;
  std::string dataDir;
  uint32_t apiVersion = 0;
  void* dl = nullptr;
  readyup_plugin_unload_fn unload = nullptr;
  ru_api api{};
  std::string mapCopy;  // current_map's return value (game thread)
};

enum class RegKind { Chat, Console, Tick, Event, GameEvent, LogLine, RuSub, Frame, ClientFilter };

struct Reg {
  ru_handle id = 0;
  int owner = 0;
  RegKind kind = RegKind::Tick;
  uint32_t flags = 0;  // RU_CMD_* (chat / console)
  std::string name;  // chat / console command, or engine game event name
  uint32_t eventType = 0;
  void* fn = nullptr;
  void* user = nullptr;
};

struct QueuedCmd {
  ru_handle reg = 0;
  uint64_t steamid64 = 0;
  int slot = -1;  // sender slot when the router knew it
  bool console = false;
  std::string playerName;
  std::string text;
};

struct QueuedTask {
  int owner = 0;
  ru_task_fn fn = nullptr;
  void* user = nullptr;
};

struct PendingOp {
  std::string verb;
  std::string name;
  bool replyToChat = false;
  int slot = -1;  // chat sender: the reply goes to that player only
};

constexpr size_t kMaxQueued = 1024;

std::mutex g_mu;  // guards everything below except the game-thread-only fields
std::map<std::string, Instance*> g_loaded;
std::vector<Reg> g_regs;
std::deque<QueuedCmd> g_cmds;
std::deque<QueuedTask> g_tasks;
std::deque<LifecycleEvent> g_events;
std::vector<PendingOp> g_ops;
std::string g_currentMap;
std::vector<std::string> g_loadFailures;  // "<name>: <error>" from the initial directory load
// Plugins the core refused to load because their needs.json is not met on this CS2 build
// (plugin_needs.h): name -> reason. Also guarded by g_mu.
std::map<std::string, std::string> g_needsDisabled;
NeedsProbeProvider g_needsProvider = nullptr;   // set by the server build (plugin_needs_engine.cpp)
NeedsAdminNotifier g_needsNotifier = nullptr;
uint64_t g_mapGen = 0;                          // bumped on every map start (admin notices)
double g_nextNeedsNotice = 0;                   // game thread
int g_nextId = 0;
ru_handle g_nextHandle = 0;

// v1.1 state (guarded by g_mu).
std::deque<std::string> g_logLines;
struct Iface {
  int owner = 0;
  uint32_t version = 0;
  void* ptr = nullptr;
};
std::map<std::string, Iface> g_ifaces;
// Keyed by "<plugin>\n<key>". Survives unload/reload (that is the point); never freed.
std::map<std::string, std::string> g_stash;
constexpr uint32_t kMaxStashBlob = 1u << 20;
struct ChatPrefix {
  int owner = 0;
  std::string prefix;
};
std::map<uint64_t, ChatPrefix> g_chatPrefixes;
std::atomic<uint64_t> g_wantedGen{0};
std::atomic<int> g_gameEventRegs{0};
std::atomic<int> g_logLineRegs{0};

// v1.11 cvar_query (guarded by g_mu). A query is answered by the first matching console line
// after it was queued (PostLogLine), then delivered on the next Frame; or it times out there.
struct CvarQuery {
  int owner = 0;
  std::string name;
  ru_cvar_fn fn = nullptr;
  void* user = nullptr;
  double deadline = 0;  // NowSeconds()
  int answer = -1;      // -1 pending, 0 unknown, 1 value
  std::string value;
};
std::vector<CvarQuery> g_cvarQueries;
std::atomic<int> g_cvarPending{0};  // queries still waiting for their line (fast path in PostLogLine)
constexpr size_t kMaxCvarQueries = 256;
constexpr double kCvarQueryTimeoutS = 3.0;

// v1.11 selftest_summary.
std::atomic<SelftestJsonProvider> g_selftestProvider{nullptr};

// Plugin selftest interfaces run on arbitrary threads under a shared lock; CloseImage takes it
// exclusively, so an image is never unmapped while its `run` is on some stack.
std::shared_mutex g_selftestMu;

// Admin provider. Its own lock: providers run on arbitrary threads and may block, so they
// are called under a shared lock that unload takes exclusively (unload waits for them).
std::shared_mutex g_adminMu;
struct AdminProvider {
  int owner = 0;
  ru_admin_provider_fn fn = nullptr;
  void* user = nullptr;
};
// One per plugin (essentials: admins.json; match: the match config's admins and the platform's
// list). A player is an admin when any of them says so.
std::vector<AdminProvider> g_admins;
thread_local bool t_inAdminProvider = false;

// Game thread only.
std::atomic<bool> g_haveGameThread{false};
std::thread::id g_gameThread;
bool g_initialized = false;
int g_depth = 0;  // >0 while plugin code is on the stack
uint64_t g_frame = 0;
char g_crashName[48] = {};

bool OnGameThread() {
  return g_haveGameThread.load(std::memory_order_acquire) && std::this_thread::get_id() == g_gameThread;
}

// Plugin time per game frame (`ru perf`, perf_stats.h). Game thread only.
PerfStats g_perf;
bool g_perfStarted = false;
double g_perfWarnMs = 8, g_perfGapWarnMs = 250;  // SetPerfThresholds (readyup.cfg)

double PerfNowS() {
  return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
}

std::string Lower(std::string s) {
  for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  return s;
}

std::vector<std::string> SplitWS(const std::string& s) {
  std::vector<std::string> out;
  std::string cur;
  for (char ch : s) {
    if (std::isspace(static_cast<unsigned char>(ch)) != 0) {
      if (!cur.empty()) out.push_back(std::move(cur));
      cur.clear();
    } else {
      cur.push_back(ch);
    }
  }
  if (!cur.empty()) out.push_back(std::move(cur));
  return out;
}

bool ValidPluginName(const std::string& n) {
  if (n.empty() || n.size() > 32) return false;
  for (unsigned char c : n) {
    if (!(std::islower(c) != 0 || std::isdigit(c) != 0 || c == '_' || c == '-')) return false;
  }
  return true;
}

double NowSeconds() {
  timespec ts{};
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return static_cast<double>(ts.tv_sec) + static_cast<double>(ts.tv_nsec) / 1e9;
}

std::string PluginsDir() {
  if (const char* env = std::getenv("READYUP_PLUGINS_DIR"); env && *env) return env;
  const std::string csgo = GetCsgoDirFromModuleDir();
  if (csgo.empty()) return {};
  return csgo + "/readyup/plugins";
}

// Must hold g_mu.
Instance* FindLiveByIdLocked(int id) {
  for (auto& kv : g_loaded) {
    if (kv.second->handle.id == id) return kv.second;
  }
  return nullptr;
}

// Validates `self` for a call that must run on the game thread.
Instance* GameThreadCaller(ru_plugin* self, const char* fn) {
  if (!self || !self->alive.load(std::memory_order_acquire)) return nullptr;
  if (!OnGameThread()) {
    Print("plugin[%s]: %s called off the game thread; ignored (use post_to_game_thread)\n", self->name, fn);
    return nullptr;
  }
  std::lock_guard<std::mutex> lk(g_mu);
  return FindLiveByIdLocked(self->id);
}

// Runs plugin code with the bookkeeping the crash handler / unload safety rely on.
template <typename F>
void InvokePlugin(Instance* inst, const char* what, F&& f) {
  char saved[sizeof(g_crashName)];
  std::memcpy(saved, g_crashName, sizeof(saved));
  std::strncpy(g_crashName, inst->name.c_str(), sizeof(g_crashName) - 1);
  // Only the outermost call is timed (a plugin calling into the core that calls another plugin
  // is part of the first plugin's time), and only on the game thread (the server frame).
  const bool timed = g_depth == 0 && OnGameThread();
  const auto t0 = timed ? std::chrono::steady_clock::now() : std::chrono::steady_clock::time_point{};
  ++g_depth;
  struct Timer {
    bool on;
    std::chrono::steady_clock::time_point t0;
    const std::string& plugin;
    const char* what;
    ~Timer() {
      if (on) g_perf.Add(plugin, what, std::chrono::duration<double, std::micro>(std::chrono::steady_clock::now() - t0).count());
    }
  } timer{timed, t0, inst->name, what};
  try {
    f();
  } catch (const std::exception& e) {
    Print("plugin[%s]: %s threw: %s (plugins must not throw across the C ABI)\n", inst->name.c_str(), what, e.what());
  } catch (...) {
    Print("plugin[%s]: %s threw (plugins must not throw across the C ABI)\n", inst->name.c_str(), what);
  }
  --g_depth;
  std::memcpy(g_crashName, saved, sizeof(saved));
}

void Reply(const std::string& line, bool toChat, int slot = -1) {
  Print("plugin: %s\n", line.c_str());
  if (toChat && !SendToSlotChat(slot, ("plugin: " + line).c_str())) SendToChat(("plugin: " + line).c_str());
}

// ---- API implementation ----------------------------------------------------------

// One console line (Print keeps long lines whole, e.g. a JSON dump).
void EmitLine(const std::string& head, const std::string& m) { Print("%s%s\n", head.c_str(), m.c_str()); }

void ApiLog(ru_plugin* self, int level, const char* msg) {
  if (!msg) return;
  if (level == RU_LOG_DEBUG && !DebugEnabled()) return;
  std::string m(msg);
  while (!m.empty() && (m.back() == '\n' || m.back() == '\r')) m.pop_back();
  const char* tag = level == RU_LOG_WARN ? "warn: " : level == RU_LOG_ERROR ? "error: " : level == RU_LOG_DEBUG ? "[dbg] " : "";
  EmitLine(std::string("plugin[") + (self ? self->name : "?") + "]: " + tag, m);
}

// v1.2: `[ReadyUp] <msg>` without the plugin tag (formats operators already parse).
void ApiLogUntagged(ru_plugin* self, int level, const char* msg) {
  if (!self || !msg) return;
  if (level == RU_LOG_DEBUG && !DebugEnabled()) return;
  std::string m(msg);
  while (!m.empty() && (m.back() == '\n' || m.back() == '\r')) m.pop_back();
  const char* tag = level == RU_LOG_WARN ? "warn: " : level == RU_LOG_ERROR ? "error: " : level == RU_LOG_DEBUG ? "[dbg] " : "";
  EmitLine(tag, m);
}

int ApiServerCommand(ru_plugin* self, const char* cmd) {
  if (!GameThreadCaller(self, "server_command") || !cmd || !*cmd) return 0;
  return EnqueueServerCommand(cmd) ? 1 : 0;
}

int ApiChatAll(ru_plugin* self, const char* msg, uint32_t flags) {
  if (!GameThreadCaller(self, "chat_all") || !msg || !*msg) return 0;
  if (flags & RU_CHAT_IMPORTANT) {
    SendImportantToChat(msg);
    return 1;
  }
  if (flags & RU_CHAT_RAW) SendRawToChat(msg);
  else SendToChat(msg);
  return 1;
}

int ApiChatToSlot(ru_plugin* self, int slot, const char* msg) {
  if (!GameThreadCaller(self, "chat_to_slot") || !msg || !*msg) return 0;
  return ClientPrintChat(slot, msg) ? 1 : 0;
}

ru_handle AddReg(Instance* inst, RegKind kind, std::string name, uint32_t type, void* fn, void* user,
                 uint32_t flags = 0) {
  std::lock_guard<std::mutex> lk(g_mu);
  // Observers (RU_CMD_OBSERVE) never own a name, so they never conflict.
  if (!name.empty() && !(flags & RU_CMD_OBSERVE)) {
    for (const auto& r : g_regs) {
      if (r.kind == kind && r.name == name && !(r.flags & RU_CMD_OBSERVE)) {
        const Instance* other = FindLiveByIdLocked(r.owner);
        Print("plugin[%s]: command \"%s\" is already registered by plugin \"%s\"\n", inst->name.c_str(), name.c_str(),
              other ? other->name.c_str() : "?");
        return 0;
      }
    }
  }
  Reg r;
  r.id = ++g_nextHandle;
  r.owner = inst->handle.id;
  r.kind = kind;
  r.name = std::move(name);
  r.eventType = type;
  r.flags = flags;
  r.fn = fn;
  r.user = user;
  g_regs.push_back(std::move(r));
  return g_regs.back().id;
}

ru_handle RegisterChat(ru_plugin* self, const char* name, uint32_t flags, ru_command_fn fn, void* user) {
  Instance* inst = GameThreadCaller(self, "register_chat_command");
  if (!inst || !name || !fn) return 0;
  const std::string n = Lower(name);
  bool ok = n.size() >= 2 && n.size() <= 32 && (n[0] == '.' || n[0] == '!');
  for (size_t i = 1; ok && i < n.size(); ++i) {
    const unsigned char c = static_cast<unsigned char>(n[i]);
    ok = std::isalnum(c) != 0 || c == '_' || c == '-';
  }
  if (!ok) {
    Print("plugin[%s]: invalid chat command name \"%s\" (want .name or !name, [a-z0-9_-])\n", self->name, name);
    return 0;
  }
  if (IsCoreChatCommand(n)) {
    Print("plugin[%s]: chat command \"%s\" is owned by the core\n", self->name, n.c_str());
    return 0;
  }
  return AddReg(inst, RegKind::Chat, n, 0, reinterpret_cast<void*>(fn), user, flags & RU_CMD_HIDE);
}

ru_handle ApiRegisterChat(ru_plugin* self, const char* name, ru_command_fn fn, void* user) {
  return RegisterChat(self, name, 0, fn, user);
}

ru_handle ApiRegisterChatEx(ru_plugin* self, const char* name, uint32_t flags, ru_command_fn fn, void* user) {
  return RegisterChat(self, name, flags, fn, user);
}

ru_handle ApiFilterClientCommand(ru_plugin* self, const char* name, ru_client_command_filter_fn fn, void* user) {
  Instance* inst = GameThreadCaller(self, "filter_client_command");
  if (!inst || !name || !fn) return 0;
  const std::string n = Lower(name);
  bool ok = !n.empty() && n.size() <= 48;
  for (size_t i = 0; ok && i < n.size(); ++i) {
    const unsigned char c = static_cast<unsigned char>(n[i]);
    ok = std::islower(c) != 0 || std::isdigit(c) != 0 || c == '_';
  }
  if (!ok || n == "say" || n == "say_team" || n == "ru") {
    Print("plugin[%s]: client command filter \"%s\" refused (invalid or reserved)\n", self->name, name);
    return 0;
  }
  return AddReg(inst, RegKind::ClientFilter, n, 0, reinterpret_cast<void*>(fn), user, 0);
}

ru_handle RegisterConsole(ru_plugin* self, const char* name, uint32_t flags, ru_command_fn fn, void* user) {
  Instance* inst = GameThreadCaller(self, "register_console_command");
  if (!inst || !name || !fn) return 0;
  const std::string n = Lower(name);
  bool ok = n.size() >= 2 && n.size() <= 48 && std::islower(static_cast<unsigned char>(n[0])) != 0;
  for (size_t i = 1; ok && i < n.size(); ++i) {
    const unsigned char c = static_cast<unsigned char>(n[i]);
    ok = std::islower(c) != 0 || std::isdigit(c) != 0 || c == '_';
  }
  if (!ok || n == "ru") {
    Print("plugin[%s]: invalid or reserved console command name \"%s\"\n", self->name, name);
    return 0;
  }
  return AddReg(inst, RegKind::Console, n, 0, reinterpret_cast<void*>(fn), user, flags & RU_CMD_OBSERVE);
}

ru_handle ApiRegisterConsole(ru_plugin* self, const char* name, ru_command_fn fn, void* user) {
  return RegisterConsole(self, name, 0, fn, user);
}

ru_handle ApiRegisterConsoleEx(ru_plugin* self, const char* name, uint32_t flags, ru_command_fn fn, void* user) {
  return RegisterConsole(self, name, flags, fn, user);
}

ru_handle ApiRegisterRuSub(ru_plugin* self, const char* name, ru_command_fn fn, void* user) {
  Instance* inst = GameThreadCaller(self, "register_ru_subcommand");
  if (!inst || !name || !fn) return 0;
  const std::string n = Lower(name);
  bool ok = !n.empty() && n.size() <= 32;
  for (size_t i = 0; ok && i < n.size(); ++i) {
    const unsigned char c = static_cast<unsigned char>(n[i]);
    ok = std::islower(c) != 0 || std::isdigit(c) != 0 || c == '_' || c == '-';
  }
  if (!ok) {
    Print("plugin[%s]: invalid ru subcommand name \"%s\" (want [a-z0-9_-])\n", self->name, name);
    return 0;
  }
  if (IsCoreRuSubcommand(n)) {
    Print("plugin[%s]: ru subcommand \"%s\" is owned by the core\n", self->name, n.c_str());
    return 0;
  }
  return AddReg(inst, RegKind::RuSub, n, 0, reinterpret_cast<void*>(fn), user);
}

ru_handle ApiOnFrame(ru_plugin* self, ru_tick_fn fn, void* user) {
  Instance* inst = GameThreadCaller(self, "on_frame");
  if (!inst || !fn) return 0;
  return AddReg(inst, RegKind::Frame, {}, 0, reinterpret_cast<void*>(fn), user);
}

const char* ApiCurrentMap(ru_plugin* self) {
  Instance* inst = GameThreadCaller(self, "current_map");
  if (!inst) return "";
  std::lock_guard<std::mutex> lk(g_mu);
  inst->mapCopy = g_currentMap;
  return inst->mapCopy.c_str();
}

ru_handle ApiOnTick(ru_plugin* self, ru_tick_fn fn, void* user) {
  Instance* inst = GameThreadCaller(self, "on_tick");
  if (!inst || !fn) return 0;
  return AddReg(inst, RegKind::Tick, {}, 0, reinterpret_cast<void*>(fn), user);
}

ru_handle ApiSubscribe(ru_plugin* self, uint32_t type, ru_event_fn fn, void* user) {
  Instance* inst = GameThreadCaller(self, "subscribe");
  if (!inst || !fn || type >= RU_EVENT_TYPE_COUNT_) return 0;
  return AddReg(inst, RegKind::Event, {}, type, reinterpret_cast<void*>(fn), user);
}

int ApiUnregister(ru_plugin* self, ru_handle handle) {
  if (!GameThreadCaller(self, "unregister") || handle == 0) return 0;
  std::lock_guard<std::mutex> lk(g_mu);
  for (auto it = g_regs.begin(); it != g_regs.end(); ++it) {
    if (it->id == handle && it->owner == self->id) {
      g_regs.erase(it);
      return 1;
    }
  }
  return 0;
}

int ApiPostToGameThread(ru_plugin* self, ru_task_fn fn, void* user) {
  if (!self || !fn || !self->alive.load(std::memory_order_acquire) || self->unloading.load()) return 0;
  std::lock_guard<std::mutex> lk(g_mu);
  if (g_tasks.size() >= kMaxQueued * 4) return 0;
  g_tasks.push_back(QueuedTask{self->id, fn, user});
  return 1;
}

int ApiSlotForSteam(ru_plugin* self, uint64_t steamid64) {
  if (!GameThreadCaller(self, "slot_for_steamid")) return -1;
  return GameEventsSlotForSteam(steamid64).value_or(-1);
}

const char* ApiDataDir(ru_plugin* self) {
  if (!self) return "";
  std::lock_guard<std::mutex> lk(g_mu);
  Instance* inst = FindLiveByIdLocked(self->id);
  return inst ? inst->dataDir.c_str() : "";
}

// ---- API v1.1 (bookkeeping members; engine-facing ones are in plugin_engine_api.cpp) ----

ru_handle ApiSubscribeGameEvent(ru_plugin* self, const char* name, ru_game_event_fn fn, void* user) {
  Instance* inst = GameThreadCaller(self, "subscribe_game_event");
  if (!inst || !name || !fn) return 0;
  const std::string n = Lower(name);
  bool ok = !n.empty() && n.size() <= 64;
  for (size_t i = 0; ok && i < n.size(); ++i) {
    const unsigned char c = static_cast<unsigned char>(n[i]);
    ok = std::islower(c) != 0 || std::isdigit(c) != 0 || c == '_';
  }
  if (!ok) {
    Print("plugin[%s]: invalid game event name \"%s\"\n", self->name, name);
    return 0;
  }
  // AddReg rejects duplicate names per kind (commands); several plugins may want one event.
  ru_handle h = 0;
  {
    std::lock_guard<std::mutex> lk(g_mu);
    Reg r;
    r.id = ++g_nextHandle;
    r.owner = inst->handle.id;
    r.kind = RegKind::GameEvent;
    r.name = n;
    r.fn = reinterpret_cast<void*>(fn);
    r.user = user;
    g_regs.push_back(std::move(r));
    h = g_regs.back().id;
  }
  g_gameEventRegs.fetch_add(1);
  g_wantedGen.fetch_add(1);
  return h;
}

ru_handle ApiSubscribeLogLine(ru_plugin* self, ru_log_line_fn fn, void* user) {
  Instance* inst = GameThreadCaller(self, "subscribe_log_line");
  if (!inst || !fn) return 0;
  const ru_handle h = AddReg(inst, RegKind::LogLine, {}, 0, reinterpret_cast<void*>(fn), user);
  if (h) g_logLineRegs.fetch_add(1);
  return h;
}

// ---- API v1.11 ----

int ApiCvarQuery(ru_plugin* self, const char* name, ru_cvar_fn fn, void* user) {
  Instance* inst = GameThreadCaller(self, "cvar_query");
  if (!inst || !name || !fn) return 0;
  const std::string n(name);
  if (!ValidCvarName(n)) {
    Print("plugin[%s]: cvar_query: invalid cvar name \"%s\"\n", self->name, name);
    return 0;
  }
  {
    std::lock_guard<std::mutex> lk(g_mu);
    if (g_cvarQueries.size() >= kMaxCvarQueries) return 0;
    CvarQuery q;
    q.owner = inst->handle.id;
    q.name = n;
    q.fn = fn;
    q.user = user;
    q.deadline = NowSeconds() + kCvarQueryTimeoutS;
    g_cvarQueries.push_back(std::move(q));
    g_cvarPending.fetch_add(1);
  }
  // Registered before the command goes out: the answer can be printed while AddText runs.
  if (EnqueueServerCommand(n.c_str())) return 1;
  std::lock_guard<std::mutex> lk(g_mu);
  for (auto it = g_cvarQueries.end(); it != g_cvarQueries.begin();) {
    --it;
    if (it->owner == inst->handle.id && it->fn == fn && it->user == user && it->name == n && it->answer < 0) {
      g_cvarQueries.erase(it);
      g_cvarPending.fetch_sub(1);
      break;
    }
  }
  return 0;
}

int ApiSelftestSummary(ru_plugin* self, char* buf, uint32_t len) {
  if (!self) return -1;
  const SelftestJsonProvider p = g_selftestProvider.load(std::memory_order_acquire);
  if (!p) return -1;
  const std::string j = p();
  if (j.empty()) return -1;
  if (buf && len > 0) std::snprintf(buf, len, "%s", j.c_str());
  return static_cast<int>(j.size());
}

// Must hold g_mu. A console line answers every pending query of the name it is about (several
// plugins asking for the same cvar asked the same question).
void MatchCvarLineLocked(const std::string& line) {
  for (auto& q : g_cvarQueries) {
    if (q.answer >= 0) continue;
    std::string value;
    const int a = ParseCvarAnswer(line, q.name, &value);
    if (a < 0) continue;
    q.answer = a;
    q.value = std::move(value);
    g_cvarPending.fetch_sub(1);
  }
}

// Game thread (Frame): answered and timed-out queries go to their plugins.
void DeliverCvarQueries() {
  std::vector<CvarQuery> ready;
  const double now = NowSeconds();
  {
    std::lock_guard<std::mutex> lk(g_mu);
    if (g_cvarQueries.empty()) return;
    for (auto it = g_cvarQueries.begin(); it != g_cvarQueries.end();) {
      if (it->answer < 0 && now < it->deadline) {
        ++it;
        continue;
      }
      if (it->answer < 0) g_cvarPending.fetch_sub(1);  // timed out
      ready.push_back(std::move(*it));
      it = g_cvarQueries.erase(it);
    }
  }
  for (const auto& q : ready) {
    Instance* inst = nullptr;
    {
      std::lock_guard<std::mutex> lk(g_mu);
      inst = FindLiveByIdLocked(q.owner);
    }
    if (!inst || inst->handle.unloading.load()) continue;
    const char* v = q.answer == 1 ? q.value.c_str() : nullptr;
    InvokePlugin(inst, "cvar_query", [&] { q.fn(q.user, q.name.c_str(), v); });
  }
}

std::string Trim(std::string s) {
  while (!s.empty() && std::isspace(static_cast<unsigned char>(s.back())) != 0) s.pop_back();
  size_t i = 0;
  while (i < s.size() && std::isspace(static_cast<unsigned char>(s[i])) != 0) ++i;
  return s.substr(i);
}

// Looks `key` up in a `key = value` file. With `section`, only inside `[section]`.
bool CfgLookup(const std::string& path, const std::string& section, const std::string& key, std::string* out) {
  std::ifstream f(path);
  if (!f.good()) return false;
  std::string line;
  std::string cur;  // current [section], "" = top level
  while (std::getline(f, line)) {
    line = Trim(line);
    if (line.empty() || line[0] == '#' || line.rfind("//", 0) == 0) continue;
    if (line.front() == '[' && line.back() == ']') {
      cur = Lower(Trim(line.substr(1, line.size() - 2)));
      continue;
    }
    if (cur != section) continue;
    const size_t eq = line.find('=');
    if (eq == std::string::npos) continue;
    if (Lower(Trim(line.substr(0, eq))) != key) continue;
    std::string v = Trim(line.substr(eq + 1));
    if (v.size() >= 2 && (v.front() == '"' || v.front() == '\'') && v.back() == v.front()) v = v.substr(1, v.size() - 2);
    *out = v;
    return true;
  }
  return false;
}

int ApiConfigGet(ru_plugin* self, const char* key, char* buf, uint32_t len) {
  if (!GameThreadCaller(self, "config_get") || !key || !*key) return -1;
  const std::string k = Lower(key);
  const std::string name = self->name;
  std::string v;
  bool found = false;
  const std::string csgo = GetCsgoDirFromModuleDir();
  if (!csgo.empty()) {
    // Top-level keys, or the same keys under a [<plugin>] header (as in readyup.cfg).
    const std::string file = csgo + "/cfg/ReadyUp/" + name + ".cfg";
    found = CfgLookup(file, "", k, &v) || CfgLookup(file, Lower(name), k, &v);
  }
  if (!found) {
    const std::string dir = GetThisModuleDir();
    if (!dir.empty()) found = CfgLookup(dir + "/readyup.cfg", name, k, &v);
    // Core keys every plugin may read (above the first section): the ruleset (docs/ESPORTS-MODE.md).
    if (!found && !dir.empty() && k == "ruleset") found = CfgLookup(dir + "/readyup.cfg", "", k, &v);
  }
  if (!found) return -1;
  if (buf && len > 0) {
    const size_t n = std::min<size_t>(v.size(), len - 1);
    std::memcpy(buf, v.data(), n);
    buf[n] = '\0';
  }
  return static_cast<int>(v.size());
}

int ApiDebugEnabled(ru_plugin*) { return DebugEnabled() ? 1 : 0; }

const char* ApiConfigDir(ru_plugin*) {
  static const std::string dir = GetThisModuleDir();
  return dir.c_str();
}

// ---- plugin needs (plugin_needs.h) ------------------------------------------------------------

std::string JsonEscape(const std::string& s) {
  std::string o;
  for (unsigned char c : s) {
    if (c == '"' || c == '\\') {
      o += '\\';
      o += static_cast<char>(c);
    } else if (c < 0x20) {
      char b[8];
      std::snprintf(b, sizeof(b), "\\u%04x", c);
      o += b;
    } else {
      o += static_cast<char>(c);
    }
  }
  return o;
}

uint32_t NeedsDisabledJson(char* buf, uint32_t cap) {
  std::string j = "[";
  for (const auto& d : NeedsDisabledPlugins()) {
    j += (j.size() > 1 ? "," : "") + std::string("{\"name\":\"") + JsonEscape(d.name) + "\",\"reason\":\"" +
         JsonEscape(d.reason) + "\"}";
  }
  j += "]";
  if (buf && cap > 0) {
    const size_t n = j.size() < cap - 1 ? j.size() : cap - 1;
    std::memcpy(buf, j.data(), n);
    buf[n] = '\0';
  }
  return static_cast<uint32_t>(j.size());
}

const ru_plugin_needs_iface_v1 g_needsIface = {sizeof(ru_plugin_needs_iface_v1), &NeedsDisabledJson};

// Interfaces the core itself provides (never owned by a plugin).
void* CoreOwnedInterface(const char* name, uint32_t minVersion) {
  if (std::strcmp(name, RU_PLUGIN_NEEDS_IFACE_NAME) == 0 && minVersion <= RU_PLUGIN_NEEDS_IFACE_VERSION) {
    return const_cast<ru_plugin_needs_iface_v1*>(&g_needsIface);
  }
  return nullptr;
}

// Game thread, every frame: once per map (after it starts), tell in-game admins which plugins
// the core did not load and why. The notifier (server build) remembers who it told.
void NoticeNeedsDisabled() {
  if (!g_needsNotifier) return;
  const double now = NowSeconds();
  if (now < g_nextNeedsNotice) return;
  g_nextNeedsNotice = now + 5.0;
  std::vector<std::string> lines;
  uint64_t gen = 0;
  {
    std::lock_guard<std::mutex> lk(g_mu);
    gen = g_mapGen;
    for (const auto& kv : g_needsDisabled) lines.push_back(NeedsDisabledLine(kv.first, kv.second));
  }
  if (gen == 0 || lines.empty()) return;
  g_needsNotifier(lines, gen);
}

int ApiProvideInterface(ru_plugin* self, const char* name, uint32_t version, void* iface) {
  Instance* inst = GameThreadCaller(self, "provide_interface");
  if (!inst || !name || !*name || !iface) return 0;
  std::lock_guard<std::mutex> lk(g_mu);
  auto it = g_ifaces.find(name);
  if (it != g_ifaces.end() && it->second.owner != inst->handle.id) {
    const Instance* other = FindLiveByIdLocked(it->second.owner);
    Print("plugin[%s]: interface \"%s\" is already provided by plugin \"%s\"\n", self->name, name,
          other ? other->name.c_str() : "?");
    return 0;
  }
  g_ifaces[name] = Iface{inst->handle.id, version, iface};
  return 1;
}

void* ApiGetInterface(ru_plugin* self, const char* name, uint32_t minVersion) {
  if (!GameThreadCaller(self, "get_interface") || !name) return nullptr;
  if (void* core = CoreOwnedInterface(name, minVersion)) return core;
  std::lock_guard<std::mutex> lk(g_mu);
  auto it = g_ifaces.find(name);
  if (it == g_ifaces.end() || it->second.version < minVersion) return nullptr;
  const Instance* owner = FindLiveByIdLocked(it->second.owner);
  if (!owner || owner->handle.unloading.load()) return nullptr;
  return it->second.ptr;
}

int ApiStashPut(ru_plugin* self, const char* key, const void* data, uint32_t len) {
  if (!GameThreadCaller(self, "stash_put") || !key || !*key) return 0;
  if (len > kMaxStashBlob || (len > 0 && !data)) return 0;
  const std::string k = std::string(self->name) + "\n" + key;
  std::lock_guard<std::mutex> lk(g_mu);
  if (len == 0) {
    g_stash.erase(k);
    return 1;
  }
  g_stash[k].assign(static_cast<const char*>(data), len);
  return 1;
}

int ApiStashGet(ru_plugin* self, const char* key, void* buf, uint32_t cap) {
  if (!GameThreadCaller(self, "stash_get") || !key || !*key) return -1;
  const std::string k = std::string(self->name) + "\n" + key;
  std::lock_guard<std::mutex> lk(g_mu);
  auto it = g_stash.find(k);
  if (it == g_stash.end()) return -1;
  if (buf && cap > 0) std::memcpy(buf, it->second.data(), std::min<size_t>(cap, it->second.size()));
  return static_cast<int>(it->second.size());
}

int ApiSetChatNamePrefix(ru_plugin* self, uint64_t steamid64, const char* prefix) {
  Instance* inst = GameThreadCaller(self, "set_chat_name_prefix");
  if (!inst || steamid64 == 0) return 0;
  std::lock_guard<std::mutex> lk(g_mu);
  if (!prefix || !*prefix) {
    auto it = g_chatPrefixes.find(steamid64);
    if (it != g_chatPrefixes.end() && it->second.owner == inst->handle.id) g_chatPrefixes.erase(it);
    return 1;
  }
  g_chatPrefixes[steamid64] = ChatPrefix{inst->handle.id, prefix};
  return 1;
}

int ApiSetAdminProvider(ru_plugin* self, ru_admin_provider_fn fn, void* user) {
  Instance* inst = GameThreadCaller(self, "set_admin_provider");
  if (!inst) return 0;
  std::unique_lock<std::shared_mutex> lk(g_adminMu);
  const int id = inst->handle.id;
  g_admins.erase(std::remove_if(g_admins.begin(), g_admins.end(), [id](const AdminProvider& p) { return p.owner == id; }),
                 g_admins.end());
  if (fn) g_admins.push_back(AdminProvider{id, fn, user});
  return 1;
}

// ---- load / unload ---------------------------------------------------------------

// Removes every registration and queued task owned by `id`.
void DropOwnedLocked(int id) {
  int gameEvents = 0, logLines = 0;
  for (const auto& r : g_regs) {
    if (r.owner != id) continue;
    gameEvents += r.kind == RegKind::GameEvent;
    logLines += r.kind == RegKind::LogLine;
  }
  if (gameEvents) {
    g_gameEventRegs.fetch_sub(gameEvents);
    g_wantedGen.fetch_add(1);
  }
  if (logLines) g_logLineRegs.fetch_sub(logLines);
  for (auto it = g_ifaces.begin(); it != g_ifaces.end();) {
    if (it->second.owner == id) it = g_ifaces.erase(it);
    else ++it;
  }
  for (auto it = g_chatPrefixes.begin(); it != g_chatPrefixes.end();) {
    if (it->second.owner == id) it = g_chatPrefixes.erase(it);
    else ++it;
  }
  g_regs.erase(std::remove_if(g_regs.begin(), g_regs.end(), [id](const Reg& r) { return r.owner == id; }),
               g_regs.end());
  g_tasks.erase(std::remove_if(g_tasks.begin(), g_tasks.end(), [id](const QueuedTask& t) { return t.owner == id; }),
                g_tasks.end());
  for (auto it = g_cvarQueries.begin(); it != g_cvarQueries.end();) {
    if (it->owner != id) {
      ++it;
      continue;
    }
    if (it->answer < 0) g_cvarPending.fetch_sub(1);
    it = g_cvarQueries.erase(it);
  }
}

void DropAdminProvider(int id) {
  std::unique_lock<std::shared_mutex> lk(g_adminMu);
  g_admins.erase(std::remove_if(g_admins.begin(), g_admins.end(), [id](const AdminProvider& p) { return p.owner == id; }),
                 g_admins.end());
}

// dlclose + verify the image really went away (STB_GNU_UNIQUE symbols, a leaked thread's
// TLS or RTLD_NODELETE pin it, and then a reload would silently keep the old code).
void CloseImage(Instance* inst) {
  if (!inst->dl) return;
  { std::unique_lock<std::shared_mutex> barrier(g_selftestMu); }  // no selftest `run` in flight
  dlclose(inst->dl);
  inst->dl = nullptr;
  if (void* still = dlopen(inst->path.c_str(), RTLD_NOW | RTLD_NOLOAD)) {
    dlclose(still);
    Print("plugin[%s]: WARNING: image is still mapped after dlclose (unique symbols? build with "
          "-fno-gnu-unique); a reload would reuse the old code\n",
          inst->name.c_str());
  }
}

void UnloadPluginsAtQuit();

// Game thread, g_depth == 0.
bool UnloadNow(const std::string& name, std::string* err) {
  Instance* inst = nullptr;
  {
    std::lock_guard<std::mutex> lk(g_mu);
    auto it = g_loaded.find(name);
    if (it == g_loaded.end()) {
      if (err) *err = "not loaded";
      return false;
    }
    inst = it->second;
  }
  inst->handle.unloading.store(true);  // stops dispatch + post_to_game_thread for it
  if (inst->unload) InvokePlugin(inst, "readyup_plugin_unload", [&] { inst->unload(); });
  DropAdminProvider(inst->handle.id);  // waits for provider calls in flight on other threads
  hooks::DropPluginHooks(inst->handle.id, inst->name);  // before the image is closed
  {
    std::lock_guard<std::mutex> lk(g_mu);
    DropOwnedLocked(inst->handle.id);
    g_loaded.erase(name);
  }
  inst->handle.alive.store(false, std::memory_order_release);
  CloseImage(inst);
  return true;
}

// Game thread, g_depth == 0.
// needs.json gate (plugin_needs.h). False (reason in *why) when the plugin must not load on this
// build. Logs the decision and warnings; records / clears the plugin in g_needsDisabled.
bool NeedsAllow(const std::string& dir, const std::string& name, std::string* why) {
  if (!g_needsProvider) return true;  // offline host: no engine to check against
  std::string perr;
  const auto needs = LoadNeedsFile(dir + "/" + name + ".needs.json", &perr);
  if (!needs) {
    if (!perr.empty()) Print("plugin[%s]: WARNING: %s.needs.json ignored: %s\n", name.c_str(), name.c_str(), perr.c_str());
    std::lock_guard<std::mutex> lk(g_mu);
    g_needsDisabled.erase(name);
    return true;
  }
  const NeedsVerdict v = EvaluateNeeds(*needs, g_needsProvider());
  for (const auto& w : v.warnings) Print("plugin[%s]: WARNING: %s\n", name.c_str(), w.c_str());
  std::lock_guard<std::mutex> lk(g_mu);
  if (v.load) {
    g_needsDisabled.erase(name);
    return true;
  }
  Print("WARN %s\n", NeedsDisabledLine(name, v.reason).c_str());
  g_needsDisabled[name] = v.reason;
  if (why) *why = "disabled: " + v.reason;
  return false;
}

bool LoadNow(const std::string& name, std::string* err, bool checkNeeds = true) {
  auto fail = [&](const std::string& why) {
    if (err) *err = why;
    return false;
  };
  if (!ValidPluginName(name)) return fail("invalid plugin name (want [a-z0-9_-], max 32)");
  {
    std::lock_guard<std::mutex> lk(g_mu);
    if (g_loaded.count(name)) return fail("already loaded");
  }
  const std::string dir = PluginsDir();
  if (dir.empty()) return fail("cannot locate plugins dir");
  const std::string path = dir + "/" + name + ".so";
  struct stat st {};
  if (stat(path.c_str(), &st) != 0 || !S_ISREG(st.st_mode)) return fail("no such file: " + path);
  if (checkNeeds) {
    std::string why;
    if (!NeedsAllow(dir, name, &why)) return fail(why);
  }

  if (void* old = dlopen(path.c_str(), RTLD_NOW | RTLD_NOLOAD)) {
    dlclose(old);
    Print("plugin[%s]: WARNING: a previous image of %s is still mapped; dlopen will return the old code\n",
          name.c_str(), path.c_str());
  }

  dlerror();
  void* dl = dlopen(path.c_str(), RTLD_NOW | RTLD_LOCAL);
  if (!dl) {
    const char* e = dlerror();
    return fail(std::string("dlopen failed: ") + (e ? e : "?"));
  }
  auto infoFn = reinterpret_cast<readyup_plugin_info_fn>(dlsym(dl, READYUP_PLUGIN_INFO_SYMBOL));
  auto loadFn = reinterpret_cast<readyup_plugin_load_fn>(dlsym(dl, READYUP_PLUGIN_LOAD_SYMBOL));
  auto unloadFn = reinterpret_cast<readyup_plugin_unload_fn>(dlsym(dl, READYUP_PLUGIN_UNLOAD_SYMBOL));
  if (!infoFn || !loadFn || !unloadFn) {
    dlclose(dl);
    return fail("missing export (need readyup_plugin_info, readyup_plugin_load, readyup_plugin_unload)");
  }
  // Info is read before any bookkeeping exists; it must not call into the core.
  const ru_plugin_info* info = infoFn();
  const size_t minInfo = offsetof(ru_plugin_info, description) + sizeof(info->description);
  if (!info || info->struct_size < minInfo || !info->name) {
    dlclose(dl);
    return fail("readyup_plugin_info returned an invalid struct");
  }
  const uint32_t want = info->api_version;
  if (RU_API_VERSION_MAJOR(want) != READYUP_PLUGIN_API_VERSION_MAJOR ||
      RU_API_VERSION_MINOR(want) > READYUP_PLUGIN_API_VERSION_MINOR) {
    dlclose(dl);
    char buf[160];
    std::snprintf(buf, sizeof(buf), "API version mismatch: plugin wants %u.%u, core provides %u.%u",
                  RU_API_VERSION_MAJOR(want), RU_API_VERSION_MINOR(want), READYUP_PLUGIN_API_VERSION_MAJOR,
                  READYUP_PLUGIN_API_VERSION_MINOR);
    return fail(buf);
  }
  if (name != info->name) {
    dlclose(dl);
    return fail(std::string("plugin reports name \"") + info->name + "\" but the file is " + name + ".so");
  }

  auto* inst = new Instance();
  inst->name = name;
  inst->path = path;
  inst->version = info->version ? info->version : "";
  inst->author = info->author ? info->author : "";
  inst->description = info->description ? info->description : "";
  inst->dataDir = dir + "/" + name;
  // The directory data_dir() names exists before the plugin loads: a new plugin's first write
  // (essentials copying an older admins.json, `ru admins add`) must not fail on a missing folder.
  if (mkdir(inst->dataDir.c_str(), 0755) != 0 && errno != EEXIST) {
    PrintLine(("plugin: could not create " + inst->dataDir + ": " + std::strerror(errno)).c_str());
  }
  inst->apiVersion = want;
  inst->dl = dl;
  inst->unload = unloadFn;
  std::strncpy(inst->handle.name, name.c_str(), sizeof(inst->handle.name) - 1);

  ru_api& a = inst->api;
  a.struct_size = sizeof(ru_api);
  a.api_version = READYUP_PLUGIN_API_VERSION;
  a.core_version = BuildVersion();
  a.self = &inst->handle;
  a.log = &ApiLog;
  a.server_command = &ApiServerCommand;
  a.chat_all = &ApiChatAll;
  a.chat_to_slot = &ApiChatToSlot;
  a.register_chat_command = &ApiRegisterChat;
  a.register_console_command = &ApiRegisterConsole;
  a.on_tick = &ApiOnTick;
  a.subscribe = &ApiSubscribe;
  a.unregister = &ApiUnregister;
  a.post_to_game_thread = &ApiPostToGameThread;
  a.slot_for_steamid = &ApiSlotForSteam;
  a.data_dir = &ApiDataDir;
  // v1.1
  a.subscribe_game_event = &ApiSubscribeGameEvent;
  a.subscribe_log_line = &ApiSubscribeLogLine;
  a.set_chat_name_prefix = &ApiSetChatNamePrefix;
  a.set_admin_provider = &ApiSetAdminProvider;
  a.config_get = &ApiConfigGet;
  a.debug_enabled = &ApiDebugEnabled;
  a.config_dir = &ApiConfigDir;
  a.provide_interface = &ApiProvideInterface;
  a.get_interface = &ApiGetInterface;
  a.stash_put = &ApiStashPut;
  a.stash_get = &ApiStashGet;
  // v1.2 (feature_state is engine-facing: plugin_engine_api.cpp)
  a.log_untagged = &ApiLogUntagged;
  a.register_chat_command_ex = &ApiRegisterChatEx;
  a.filter_client_command = &ApiFilterClientCommand;
  a.register_console_command_ex = &ApiRegisterConsoleEx;
  a.register_ru_subcommand = &ApiRegisterRuSub;
  a.on_frame = &ApiOnFrame;
  a.current_map = &ApiCurrentMap;
  // v1.11 (engine-free: the command buffer and the log listener, like server_command)
  a.cvar_query = &ApiCvarQuery;
  a.selftest_summary = &ApiSelftestSummary;
  // Engine-facing members (output, players, event accessors, schema/entities, round
  // suppression, is_admin). Anything left NULL there is a core bug; fail closed.
  detail::FillEngineApi(&a);

  {
    std::lock_guard<std::mutex> lk(g_mu);
    inst->handle.id = ++g_nextId;
    g_loaded[name] = inst;
  }
  inst->handle.alive.store(true, std::memory_order_release);

  int rc = -1;
  InvokePlugin(inst, "readyup_plugin_load", [&] { rc = loadFn(&inst->api, READYUP_PLUGIN_API_VERSION); });
  if (rc != 0) {
    inst->handle.unloading.store(true);
    DropAdminProvider(inst->handle.id);
    hooks::DropPluginHooks(inst->handle.id, inst->name);
    {
      std::lock_guard<std::mutex> lk(g_mu);
      DropOwnedLocked(inst->handle.id);
      g_loaded.erase(name);
    }
    inst->handle.alive.store(false, std::memory_order_release);
    CloseImage(inst);
    return fail("readyup_plugin_load returned " + std::to_string(rc));
  }
  std::atexit(&UnloadPluginsAtQuit);  // after this plugin's own exit handlers: runs before them
  Print("plugin: loaded %s %s (api %u.%u) from %s\n", name.c_str(), inst->version.c_str(), RU_API_VERSION_MAJOR(want),
        RU_API_VERSION_MINOR(want), path.c_str());
  return true;
}

void LoadAllFromDir() {
  if (const char* env = std::getenv("READYUP_PLUGINS"); env && std::strcmp(env, "0") == 0) {
    PrintLine("plugin: disabled by READYUP_PLUGINS=0");
    return;
  }
  const std::string dir = PluginsDir();
  DIR* d = dir.empty() ? nullptr : opendir(dir.c_str());
  if (!d) {
    Debug("plugin: no plugins dir (%s)\n", dir.c_str());
    return;
  }
  std::vector<std::string> names;
  while (dirent* e = readdir(d)) {
    const std::string f = e->d_name;
    if (f.size() <= 3 || f.compare(f.size() - 3, 3, ".so") != 0) continue;
    names.push_back(f.substr(0, f.size() - 3));
  }
  closedir(d);
  std::sort(names.begin(), names.end());
  Print("plugin: %zu plugin file(s) in %s\n", names.size(), dir.c_str());
  const std::set<std::string> disabled = LoadDisabled(PluginStatePath(dir));
  // needs.json first, for every plugin, so the list of plugins this build cannot run is complete
  // before any plugin loads (fleet reports it in hello).
  std::set<std::string> unmet;
  for (const auto& n : names) {
    if (!disabled.count(n) && !NeedsAllow(dir, n, nullptr)) unmet.insert(n);
  }
  for (const auto& n : names) {
    if (disabled.count(n)) {
      Print("plugin: %s is disabled (plugins.json); `ru plugin enable %s` turns it on\n", n.c_str(), n.c_str());
      continue;
    }
    if (unmet.count(n)) continue;  // logged by NeedsAllow; a WARN in ru selftest, not a load failure
    std::string err;
    if (!LoadNow(n, &err, /*checkNeeds=*/false)) {
      Print("plugin: failed to load %s: %s\n", n.c_str(), err.c_str());
      std::lock_guard<std::mutex> lk(g_mu);
      g_loadFailures.push_back(n + ": " + err);
    }
  }
}

void RunOp(const PendingOp& op) {
  std::string err;
  if (op.verb == "enable" || op.verb == "disable") {
    // Saved first, so the choice holds even if the load / unload below fails.
    const std::string path = PluginStatePath(PluginsDir());
    std::set<std::string> disabled = LoadDisabled(path);
    const bool disable = op.verb == "disable";
    if (disable) disabled.insert(op.name);
    else disabled.erase(op.name);
    if (!SaveDisabled(path, disabled)) {
      Reply(op.verb + " " + op.name + " failed: cannot write " + path, op.replyToChat, op.slot);
      return;
    }
    bool loaded = false;
    {
      std::lock_guard<std::mutex> lk(g_mu);
      loaded = g_loaded.count(op.name) != 0;
    }
    if (disable) {
      if (loaded && !UnloadNow(op.name, &err)) Reply("disabled " + op.name + ", but unload failed: " + err, op.replyToChat, op.slot);
      else Reply("disabled " + op.name + " (stays off after a restart; ru plugin enable " + op.name + ")", op.replyToChat, op.slot);
    } else {
      if (!loaded && !LoadNow(op.name, &err)) Reply("enabled " + op.name + ", but load failed: " + err, op.replyToChat, op.slot);
      else Reply("enabled " + op.name, op.replyToChat, op.slot);
    }
    return;
  }
  if (op.verb == "load") {
    if (LoadNow(op.name, &err)) Reply("loaded " + op.name, op.replyToChat, op.slot);
    else Reply("load " + op.name + " failed: " + err, op.replyToChat, op.slot);
  } else if (op.verb == "unload") {
    if (UnloadNow(op.name, &err)) Reply("unloaded " + op.name, op.replyToChat, op.slot);
    else Reply("unload " + op.name + " failed: " + err, op.replyToChat, op.slot);
  } else if (op.verb == "reload") {
    bool wasLoaded = false;
    {
      std::lock_guard<std::mutex> lk(g_mu);
      wasLoaded = g_loaded.count(op.name) != 0;
    }
    if (wasLoaded && !UnloadNow(op.name, &err)) {
      Reply("reload " + op.name + " failed during unload: " + err, op.replyToChat, op.slot);
      return;
    }
    if (LoadNow(op.name, &err)) Reply("reloaded " + op.name, op.replyToChat, op.slot);
    else Reply("reload " + op.name + " failed: " + err + " (plugin is now unloaded)", op.replyToChat, op.slot);
  }
}

// Looks up a registration by handle, returning a copy plus its (live) owner.
bool LookupReg(ru_handle id, Reg* out, Instance** owner) {
  std::lock_guard<std::mutex> lk(g_mu);
  for (const auto& r : g_regs) {
    if (r.id != id) continue;
    Instance* inst = FindLiveByIdLocked(r.owner);
    if (!inst || inst->handle.unloading.load()) return false;
    *out = r;
    *owner = inst;
    return true;
  }
  return false;
}

void DeliverCommand(const QueuedCmd& c) {
  Reg reg;
  Instance* inst = nullptr;
  if (!LookupReg(c.reg, &reg, &inst)) return;  // unregistered / unloaded meanwhile
  const std::vector<std::string> parts = SplitWS(c.text);
  std::vector<const char*> argv;
  argv.reserve(parts.size() + 1);
  for (const auto& p : parts) argv.push_back(p.c_str());
  argv.push_back(nullptr);
  ru_command_ctx ctx{};
  ctx.struct_size = sizeof(ctx);
  ctx.steamid64 = c.steamid64;
  ctx.slot = c.console ? -1 : c.slot >= 0 ? c.slot : GameEventsSlotForSteam(c.steamid64).value_or(-1);
  ctx.is_console = c.console ? 1 : 0;
  ctx.name = c.playerName.c_str();
  ctx.text = c.text.c_str();
  ctx.argc = static_cast<int>(parts.size());
  ctx.argv = argv.data();
  auto fn = reinterpret_cast<ru_command_fn>(reg.fn);
  const std::string what = "cmd:" + reg.name;
  InvokePlugin(inst, what.c_str(), [&] { fn(reg.user, &ctx); });
}

void DeliverEvent(const LifecycleEvent& e) {
  std::vector<ru_handle> ids;
  {
    std::lock_guard<std::mutex> lk(g_mu);
    for (const auto& r : g_regs) {
      if (r.kind == RegKind::Event && (r.eventType == RU_EVENT_ANY || r.eventType == e.type)) ids.push_back(r.id);
    }
  }
  ru_event ev{};
  ev.struct_size = sizeof(ev);
  ev.type = e.type;
  ev.source = e.source;
  ev.slot = e.slot;
  ev.steamid64 = e.steamid64;
  ev.name = e.name.c_str();
  ev.map = e.map.c_str();
  ev.team = e.team;
  ev.old_team = e.old_team;
  ev.winner = e.winner;
  ev.reason = e.reason;
  ev.round = e.round;
  ev.team_ct_score = e.team_ct_score;
  ev.team_t_score = e.team_t_score;
  for (ru_handle id : ids) {
    Reg reg;
    Instance* inst = nullptr;
    if (!LookupReg(id, &reg, &inst)) continue;
    auto fn = reinterpret_cast<ru_event_fn>(reg.fn);
    InvokePlugin(inst, "event", [&] { fn(reg.user, &ev); });
  }
}

std::vector<ru_handle> RegIds(RegKind kind, const char* name = nullptr) {
  std::vector<ru_handle> ids;
  std::lock_guard<std::mutex> lk(g_mu);
  for (const auto& r : g_regs) {
    if (r.kind == kind && (!name || r.name == name)) ids.push_back(r.id);
  }
  return ids;
}

void DeliverLogLines(const std::deque<std::string>& lines) {
  const std::vector<ru_handle> ids = RegIds(RegKind::LogLine);
  for (const auto& line : lines) {
    for (ru_handle id : ids) {
      Reg reg;
      Instance* inst = nullptr;
      if (!LookupReg(id, &reg, &inst)) continue;
      auto fn = reinterpret_cast<ru_log_line_fn>(reg.fn);
      InvokePlugin(inst, "log line", [&] { fn(reg.user, line.c_str()); });
    }
  }
}

// on_tick (kind Tick, simulating frames only) and on_frame (kind Frame, every frame).
void RunTicks(RegKind kind, bool simulating) {
  const std::vector<ru_handle> ids = RegIds(kind);
  if (ids.empty()) return;
  ru_tick_info t{};
  t.struct_size = sizeof(t);
  t.frame = g_frame;
  t.now = NowSeconds();
  t.simulating = simulating ? 1 : 0;
  for (ru_handle id : ids) {
    Reg reg;
    Instance* inst = nullptr;
    if (!LookupReg(id, &reg, &inst)) continue;
    auto fn = reinterpret_cast<ru_tick_fn>(reg.fn);
    InvokePlugin(inst, kind == RegKind::Tick ? "tick" : "frame", [&] { fn(reg.user, &t); });
  }
}

bool AnyRegLocked(RegKind kind, uint32_t eventType) {
  for (const auto& r : g_regs) {
    if (r.kind != kind) continue;
    if (kind != RegKind::Event || r.eventType == RU_EVENT_ANY || r.eventType == eventType) return true;
  }
  return false;
}

bool QueueCommandLocked(RegKind kind, const std::string& token, QueuedCmd c) {
  for (const auto& r : g_regs) {
    if (r.kind != kind || r.name != token || (r.flags & RU_CMD_OBSERVE)) continue;
    Instance* inst = FindLiveByIdLocked(r.owner);
    if (!inst || inst->handle.unloading.load()) return false;
    if (g_cmds.size() >= kMaxQueued) return true;  // owned, but dropped under flood
    c.reg = r.id;
    g_cmds.push_back(std::move(c));
    return true;
  }
  return false;
}

// At quit, exit() runs the plugins' static destructors while their threads (fleet, workers, the
// license check-in) still run: a joinable std::thread aborted the process, or a thread used freed
// state and it crashed with SIGSEGV, on every stop. Exit handlers run in reverse order of
// registration, so this one, registered after each plugin loads (LoadNow), runs before that
// plugin's destructors and unloads every plugin first, which joins those threads.
void UnloadPluginsAtQuit() {
  std::vector<std::string> names;
  {
    std::lock_guard<std::mutex> lk(g_mu);
    for (const auto& kv : g_loaded) names.push_back(kv.first);
  }
  if (names.empty()) return;
  std::fprintf(stderr, "[ReadyUp] quit: unloading %zu plugin(s)\n", names.size());
  for (auto it = names.rbegin(); it != names.rend(); ++it) (void)UnloadNow(*it, nullptr);
}

}  // namespace

bool IsCoreRuSubcommand(const std::string& sub) {
  static const char* const kCore[] = {
      // The core's own (engine / plugin host); the match flow's are plugins/match's.
      "help", "list", "plugin", "plugins", "version", "selftest", "sigtest", "reload", "status_http", "license"};
  for (const char* c : kCore) {
    if (sub == c) return true;
  }
  return false;
}

void PostEvent(LifecycleEvent ev) {
  std::lock_guard<std::mutex> lk(g_mu);
  if (ev.type == RU_EVENT_MAP_START) {
    if (ev.map.empty() || ev.map == g_currentMap) return;  // "Loading map" + "Started map"
    g_currentMap = ev.map;
    ++g_mapGen;
  } else {
    ev.map = g_currentMap;
  }
  if (!AnyRegLocked(RegKind::Event, ev.type)) return;
  if (g_events.size() >= kMaxQueued) g_events.pop_front();
  g_events.push_back(std::move(ev));
}

bool TryDispatchChat(uint64_t steamid64, const std::string& playerName, const std::string& text, int slot) {
  if (steamid64 == 0) return false;
  const auto parts = SplitWS(text);
  if (parts.empty()) return false;
  QueuedCmd c;
  c.steamid64 = steamid64;
  c.slot = slot;
  c.playerName = playerName;
  c.text = text;
  std::lock_guard<std::mutex> lk(g_mu);
  return QueueCommandLocked(RegKind::Chat, Lower(parts[0]), std::move(c));
}

bool FilterClientCommand(int slot, uint64_t steamid64, const std::vector<std::string>& argv) {
  if (argv.empty()) return false;
  const std::string cmd = Lower(argv[0]);
  struct Hit {
    ru_client_command_filter_fn fn;
    void* user;
  };
  std::vector<Hit> hits;
  {
    std::lock_guard<std::mutex> lk(g_mu);
    for (const auto& r : g_regs) {
      if (r.kind != RegKind::ClientFilter || r.name != cmd) continue;
      const Instance* inst = FindLiveByIdLocked(r.owner);
      if (!inst || inst->handle.unloading.load()) continue;
      hits.push_back({reinterpret_cast<ru_client_command_filter_fn>(r.fn), r.user});
    }
  }
  if (hits.empty()) return false;
  std::vector<const char*> args;
  args.reserve(argv.size());
  for (const auto& a : argv) args.push_back(a.c_str());
  for (const auto& h : hits) {
    if (h.fn(h.user, slot, steamid64, cmd.c_str(), static_cast<int>(args.size()), args.data()) == 1) return true;
  }
  return false;
}

bool ChatCommandOwned(const std::string& token, uint32_t* flags) {
  std::lock_guard<std::mutex> lk(g_mu);
  for (const auto& r : g_regs) {
    if (r.kind != RegKind::Chat || r.name != token) continue;
    const Instance* inst = FindLiveByIdLocked(r.owner);
    if (!inst || inst->handle.unloading.load()) return false;
    if (flags) *flags = r.flags;
    return true;
  }
  return false;
}

bool TryDispatchRu(bool console, uint64_t steamid64, const std::string& playerName, const std::string& text, int slot) {
  if (!console && steamid64 == 0) return false;
  const auto parts = SplitWS(text);
  if (parts.size() < 2) return false;
  QueuedCmd c;
  c.console = console;
  c.steamid64 = console ? 0 : steamid64;
  c.slot = console ? -1 : slot;
  c.playerName = console ? std::string("Console") : playerName;
  c.text = text;
  std::lock_guard<std::mutex> lk(g_mu);
  return QueueCommandLocked(RegKind::RuSub, Lower(parts[1]), std::move(c));
}

void ObserveConsole(const std::string& line) {
  const auto parts = SplitWS(line);
  if (parts.empty()) return;
  const std::string token = Lower(parts[0]);
  std::lock_guard<std::mutex> lk(g_mu);
  for (const auto& r : g_regs) {
    if (r.kind != RegKind::Console || !(r.flags & RU_CMD_OBSERVE) || r.name != token) continue;
    const Instance* inst = FindLiveByIdLocked(r.owner);
    if (!inst || inst->handle.unloading.load() || g_cmds.size() >= kMaxQueued) continue;
    QueuedCmd c;
    c.reg = r.id;
    c.console = true;
    c.playerName = "Console";
    c.text = line;
    g_cmds.push_back(std::move(c));
  }
}

std::vector<std::string> PluginCommandSummary() {
  std::map<std::string, std::string> lines;  // "<plugin> <kind>" -> names
  std::lock_guard<std::mutex> lk(g_mu);
  for (const auto& r : g_regs) {
    const char* kind = r.kind == RegKind::Chat ? "chat" : r.kind == RegKind::Console && !(r.flags & RU_CMD_OBSERVE) ? "console"
                       : r.kind == RegKind::RuSub                                    ? "ru"
                                                                                     : nullptr;
    if (!kind) continue;
    const Instance* inst = FindLiveByIdLocked(r.owner);
    std::string& l = lines[(inst ? inst->name : std::string("?")) + " " + kind];
    l += (l.empty() ? "" : " ") + r.name;
  }
  std::vector<std::string> out;
  for (const auto& kv : lines) out.push_back(kv.first + ": " + kv.second);
  return out;
}

std::vector<std::string> PluginRuSubcommands() {
  std::vector<std::string> out;
  std::lock_guard<std::mutex> lk(g_mu);
  for (const auto& r : g_regs) {
    if (r.kind != RegKind::RuSub) continue;
    const Instance* inst = FindLiveByIdLocked(r.owner);
    out.push_back(r.name + " (" + (inst ? inst->name : std::string("?")) + ")");
  }
  std::sort(out.begin(), out.end());
  return out;
}

bool TryDispatchConsole(const std::string& line) {
  const auto parts = SplitWS(line);
  if (parts.empty()) return false;
  QueuedCmd c;
  c.console = true;
  c.playerName = "Console";
  c.text = line;
  std::lock_guard<std::mutex> lk(g_mu);
  return QueueCommandLocked(RegKind::Console, Lower(parts[0]), std::move(c));
}

void LoadAllFromDirForTest() { LoadAllFromDir(); }

void Frame(bool simulating) {
  if (!g_haveGameThread.load(std::memory_order_relaxed)) {
    g_gameThread = std::this_thread::get_id();
    g_haveGameThread.store(true, std::memory_order_release);
  }
  if (g_depth != 0) return;  // never re-entered from plugin code, but stay safe
  if (!g_initialized) {
    g_initialized = true;
    LoadAllFromDir();
  }
  NoticeNeedsDisabled();

  // 1. Load / unload / reload: nothing of any plugin is on the stack here.
  std::vector<PendingOp> ops;
  {
    std::lock_guard<std::mutex> lk(g_mu);
    ops.swap(g_ops);
  }
  for (const auto& op : ops) RunOp(op);

  // 2. Deferred work, in arrival order per queue.
  std::deque<QueuedTask> tasks;
  std::deque<QueuedCmd> cmds;
  std::deque<LifecycleEvent> events;
  {
    std::lock_guard<std::mutex> lk(g_mu);
    tasks.swap(g_tasks);
    cmds.swap(g_cmds);
    events.swap(g_events);
  }
  for (const auto& t : tasks) {
    Instance* inst = nullptr;
    {
      std::lock_guard<std::mutex> lk(g_mu);
      inst = FindLiveByIdLocked(t.owner);
    }
    if (!inst || inst->handle.unloading.load()) continue;
    InvokePlugin(inst, "task", [&] { t.fn(t.user); });
  }
  for (const auto& c : cmds) DeliverCommand(c);
  for (const auto& e : events) DeliverEvent(e);
  if (g_logLineRegs.load(std::memory_order_relaxed) > 0) {
    std::deque<std::string> lines;
    {
      std::lock_guard<std::mutex> lk(g_mu);
      lines.swap(g_logLines);
    }
    if (!lines.empty()) DeliverLogLines(lines);
  }
  DeliverCvarQueries();

  // 3. Per-tick callbacks (simulating frames), then per-frame callbacks (every frame).
  if (simulating) {
    ++g_frame;
    RunTicks(RegKind::Tick, true);
  }
  RunTicks(RegKind::Frame, simulating);

  // 4. Frame time: log a frame whose plugin time or gap is over the thresholds (SetPerfThresholds).
  if (simulating) {
    const double now = PerfNowS();
    if (!g_perfStarted) {
      g_perfStarted = true;
      g_perf.Reset(now);
    }
    const std::string line = g_perf.EndFrame(now, g_perfWarnMs, g_perfGapWarnMs);
    if (!line.empty()) Print("%s\n", line.c_str());
  }
}

void SetPerfThresholds(int warnMs, int gapWarnMs) {
  g_perfWarnMs = warnMs > 0 ? warnMs : 8;
  g_perfGapWarnMs = gapWarnMs;
}

void HandlePluginCommand(const std::vector<std::string>& args, bool replyToChat, int slot) {
  const std::string verb = args.empty() ? "list" : Lower(args[0]);
  if (verb == "perf") {
    // `ru plugin perf [reset]` (`ru perf`): where the server frame goes, since the last reset.
    if (args.size() > 1 && Lower(args[1]) == "reset") {
      g_perf.Reset(PerfNowS());
      Reply("perf: counters reset", replyToChat, slot);
      return;
    }
    for (const auto& l : g_perf.Report(PerfNowS(), replyToChat ? 6 : 16)) Reply(l, replyToChat, slot);
    return;
  }
  if (verb == "list") {
    std::vector<std::string> lines;
    {
      std::lock_guard<std::mutex> lk(g_mu);
      for (const auto& kv : g_loaded) {
        const Instance* inst = kv.second;
        int cmds = 0, ticks = 0, subs = 0;
        for (const auto& r : g_regs) {
          if (r.owner != inst->handle.id) continue;
          if (r.kind == RegKind::Chat || r.kind == RegKind::Console || r.kind == RegKind::RuSub) ++cmds;
          else if (r.kind == RegKind::Tick || r.kind == RegKind::Frame) ++ticks;
          else ++subs;
        }
        char buf[512];
        std::snprintf(buf, sizeof(buf), "%s %s (api %u.%u) cmds=%d ticks=%d subs=%d - %s", inst->name.c_str(),
                      inst->version.c_str(), RU_API_VERSION_MAJOR(inst->apiVersion),
                      RU_API_VERSION_MINOR(inst->apiVersion), cmds, ticks, subs, inst->description.c_str());
        lines.emplace_back(buf);
      }
    }
    char head[160];
    std::snprintf(head, sizeof(head), "%zu loaded, core api %u.%u, dir %s%s", lines.size(),
                  READYUP_PLUGIN_API_VERSION_MAJOR, READYUP_PLUGIN_API_VERSION_MINOR, PluginsDir().c_str(),
                  g_haveGameThread.load() ? "" : " (plugins load on the first server frame)");
    Reply(head, replyToChat, slot);
    for (const auto& l : lines) Reply("  " + l, replyToChat, slot);
    const std::set<std::string> disabled = LoadDisabled(PluginStatePath(PluginsDir()));
    if (!disabled.empty()) {
      std::string d;
      for (const auto& n : disabled) d += (d.empty() ? "" : " ") + n;
      Reply("  disabled (plugins.json): " + d, replyToChat, slot);
    }
    for (const auto& d : NeedsDisabledPlugins()) {
      Reply("  disabled (needs.json not met): " + d.name + " - " + d.reason, replyToChat, slot);
    }
    return;
  }
  if (verb != "load" && verb != "unload" && verb != "reload" && verb != "enable" && verb != "disable") {
    Reply("usage: ru plugin list | load|unload|reload <name> | enable|disable <name>", replyToChat, slot);
    return;
  }
  if (args.size() < 2 || !ValidPluginName(Lower(args[1]))) {
    Reply("usage: ru plugin " + verb + " <name>   (file csgo/readyup/plugins/<name>.so)", replyToChat, slot);
    return;
  }
  // Always deferred to the top of the next GameFrame: this may be running inside the
  // engine's AddText (console) or inside a plugin callback (server_command), neither of
  // which is a safe point to run plugin load/unload code.
  {
    std::lock_guard<std::mutex> lk(g_mu);
    g_ops.push_back(PendingOp{verb, Lower(args[1]), replyToChat, slot});
  }
  Debug("plugin: %s %s queued for the next server frame\n", verb.c_str(), args[1].c_str());
}

const char* CrashContextPlugin() {
  return g_crashName;
}

int detail::PluginId(ru_plugin* self) { return self ? self->id : 0; }
const char* detail::PluginNameOf(ru_plugin* self) { return self && self->name[0] ? self->name : "?"; }

bool detail::CheckGameThread(ru_plugin* self, const char* fn) { return GameThreadCaller(self, fn) != nullptr; }

void DispatchGameEvent(const char* name, void* ev) {
  if (!name || !ev || g_gameEventRegs.load(std::memory_order_relaxed) <= 0) return;
  if (!OnGameThread()) return;  // the engine dispatches on the game thread; anything else is dropped
  const std::string n = Lower(name);
  const std::vector<ru_handle> ids = RegIds(RegKind::GameEvent, n.c_str());
  for (ru_handle id : ids) {
    Reg reg;
    Instance* inst = nullptr;
    if (!LookupReg(id, &reg, &inst)) continue;
    auto fn = reinterpret_cast<ru_game_event_fn>(reg.fn);
    const std::string what = "event:" + n;
    InvokePlugin(inst, what.c_str(), [&] { fn(reg.user, n.c_str(), static_cast<const ru_game_event*>(ev)); });
  }
}

std::vector<std::string> WantedGameEvents() {
  std::vector<std::string> out;
  std::lock_guard<std::mutex> lk(g_mu);
  for (const auto& r : g_regs) {
    if (r.kind == RegKind::GameEvent && std::find(out.begin(), out.end(), r.name) == out.end()) out.push_back(r.name);
  }
  return out;
}

uint64_t WantedGameEventsGeneration() { return g_wantedGen.load(std::memory_order_acquire); }

void PostLogLine(const std::string& line) {
  if (line.empty()) return;
  if (g_cvarPending.load(std::memory_order_acquire) > 0) {
    std::lock_guard<std::mutex> lk(g_mu);
    MatchCvarLineLocked(line);
  }
  if (g_logLineRegs.load(std::memory_order_relaxed) <= 0) return;
  std::lock_guard<std::mutex> lk(g_mu);
  if (g_logLines.size() >= kMaxQueued) g_logLines.pop_front();
  g_logLines.push_back(line);
}

std::string CurrentMap() {
  std::lock_guard<std::mutex> lk(g_mu);
  return g_currentMap;
}

bool PluginChatPrefixFor(uint64_t steamid64, std::string* prefix) {
  std::lock_guard<std::mutex> lk(g_mu);
  auto it = g_chatPrefixes.find(steamid64);
  if (it == g_chatPrefixes.end()) return false;
  if (prefix) *prefix = it->second.prefix;
  return true;
}

int PluginAdminVerdict(uint64_t steamid64) {
  if (t_inAdminProvider) return -1;  // a provider asking is_admin must not recurse into itself
  std::shared_lock<std::shared_mutex> lk(g_adminMu);
  if (g_admins.empty()) return -1;
  // 1 if any provider says admin; 0 if at least one answered and none said admin; -1 otherwise.
  int verdict = -1;
  t_inAdminProvider = true;
  for (const auto& p : g_admins) {
    int v = -1;
    try {
      v = p.fn(p.user, steamid64);
    } catch (...) {
      v = -1;
    }
    if (v > 0) {
      verdict = 1;
      break;
    }
    if (v == 0) verdict = 0;
  }
  t_inAdminProvider = false;
  return verdict;
}

void* CoreGetInterface(const char* name, uint32_t minVersion) {
  if (!name) return nullptr;
  if (void* core = CoreOwnedInterface(name, minVersion)) return core;
  std::lock_guard<std::mutex> lk(g_mu);
  auto it = g_ifaces.find(name);
  if (it == g_ifaces.end() || it->second.version < minVersion) return nullptr;
  const Instance* owner = FindLiveByIdLocked(it->second.owner);
  if (!owner || owner->handle.unloading.load()) return nullptr;
  return it->second.ptr;
}

std::vector<PluginSelftestCheck> RunPluginSelftests() {
  std::vector<PluginSelftestCheck> out;
  std::shared_lock<std::shared_mutex> running(g_selftestMu);
  struct Item {
    std::string plugin;
    const ru_selftest_iface_v1* iface;
  };
  std::vector<Item> items;
  {
    std::lock_guard<std::mutex> lk(g_mu);
    const std::string prefix = RU_SELFTEST_IFACE_PREFIX;
    for (const auto& kv : g_ifaces) {
      if (kv.first.compare(0, prefix.size(), prefix) != 0 || kv.second.version < RU_SELFTEST_IFACE_VERSION) continue;
      const Instance* owner = FindLiveByIdLocked(kv.second.owner);
      if (!owner || owner->handle.unloading.load()) continue;
      const auto* iface = static_cast<const ru_selftest_iface_v1*>(kv.second.ptr);
      if (!iface || iface->struct_size < sizeof(ru_selftest_iface_v1) || !iface->run) continue;
      items.push_back(Item{owner->name, iface});
    }
  }
  struct Ctx {
    std::vector<PluginSelftestCheck>* out;
    const std::string* plugin;
  };
  for (const auto& it : items) {
    Ctx ctx{&out, &it.plugin};
    try {
      it.iface->run(
          [](void* c, const char* status, const char* name, const char* detail) {
            auto* x = static_cast<Ctx*>(c);
            x->out->push_back(PluginSelftestCheck{*x->plugin, status ? status : "INFO", name ? name : "?",
                                                  detail ? detail : ""});
          },
          &ctx);
    } catch (...) {
      out.push_back(PluginSelftestCheck{it.plugin, "FAIL", "selftest", "threw an exception"});
    }
  }
  return out;
}

void SetSelftestJsonProvider(SelftestJsonProvider provider) {
  g_selftestProvider.store(provider, std::memory_order_release);
}

bool ValidCvarName(const std::string& name) {
  if (name.empty() || name.size() > 63) return false;
  for (unsigned char c : name) {
    if (!(std::isalnum(c) != 0 || c == '_' || c == '.')) return false;
  }
  return true;
}

int ParseCvarAnswer(const std::string& line, const std::string& name, std::string* value) {
  if (name.empty()) return -1;
  std::string l = line;
  while (!l.empty() && (l.back() == '\n' || l.back() == '\r')) l.pop_back();
  size_t i = 0;
  while (i < l.size() && (l[i] == ' ' || l[i] == '\t')) ++i;
  auto nameAt = [&](size_t pos) {
    if (pos + name.size() > l.size()) return false;
    for (size_t k = 0; k < name.size(); ++k) {
      if (std::tolower(static_cast<unsigned char>(l[pos + k])) != std::tolower(static_cast<unsigned char>(name[k]))) {
        return false;
      }
    }
    return true;
  };
  // `mp_maxrounds = 24` (a string cvar prints its text as is, "" as nothing after "= ").
  if (nameAt(i) && l.compare(i + name.size(), 3, " = ") == 0) {
    if (value) *value = l.substr(i + name.size() + 3);
    return 1;
  }
  if (nameAt(i) && l.size() == i + name.size() + 2 && l.compare(i + name.size(), 2, " =") == 0) {
    if (value) value->clear();  // the line end was trimmed right after "="
    return 1;
  }
  // `Unknown command 'mp_nope'!`
  static const std::string kUnknown = "Unknown command '";
  if (l.compare(i, kUnknown.size(), kUnknown) == 0) {
    const size_t p = i + kUnknown.size();
    if (nameAt(p) && l.compare(p + name.size(), 1, "'") == 0) return 0;
  }
  return -1;
}

void SetNeedsProbeProvider(NeedsProbeProvider provider) { g_needsProvider = provider; }
void SetNeedsAdminNotifier(NeedsAdminNotifier notifier) { g_needsNotifier = notifier; }

std::vector<DisabledPlugin> NeedsDisabledPlugins() {
  std::lock_guard<std::mutex> lk(g_mu);
  std::vector<DisabledPlugin> out;
  for (const auto& kv : g_needsDisabled) out.push_back({kv.first, kv.second});
  return out;
}

std::vector<PluginNeedsReport> EvaluateAllNeedsNow() {
  std::vector<PluginNeedsReport> out;
  if (!g_needsProvider) return out;
  const std::string dir = PluginsDir();
  DIR* d = dir.empty() ? nullptr : opendir(dir.c_str());
  if (!d) return out;
  std::vector<std::string> names;
  const std::string suffix = ".needs.json";
  while (dirent* e = readdir(d)) {
    const std::string f = e->d_name;
    if (f.size() > suffix.size() && f.compare(f.size() - suffix.size(), suffix.size(), suffix) == 0) {
      names.push_back(f.substr(0, f.size() - suffix.size()));
    }
  }
  closedir(d);
  std::sort(names.begin(), names.end());
  const NeedsProbe probe = g_needsProvider();
  for (const auto& n : names) {
    std::string err;
    if (const auto needs = LoadNeedsFile(dir + "/" + n + suffix, &err)) out.push_back({n, EvaluateNeeds(*needs, probe)});
  }
  return out;
}

PluginHostStatus GetPluginHostStatus() {
  PluginHostStatus s;
  s.dir = PluginsDir();
  s.apiMajor = READYUP_PLUGIN_API_VERSION_MAJOR;
  s.apiMinor = READYUP_PLUGIN_API_VERSION_MINOR;
  s.started = g_haveGameThread.load(std::memory_order_acquire);
  const char* env = std::getenv("READYUP_PLUGINS");
  s.disabledByEnv = env && std::strcmp(env, "0") == 0;
  std::lock_guard<std::mutex> lk(g_mu);
  for (const auto& kv : g_loaded) {
    const Instance* inst = kv.second;
    s.loaded.push_back(inst->name + " " + inst->version);
  }
  s.failures = g_loadFailures;
  return s;
}

}  // namespace readyup::plugins

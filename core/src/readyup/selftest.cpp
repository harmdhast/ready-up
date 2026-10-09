#include "readyup/selftest.h"

#include "readyup/status_feed.h"

#include "readyup/client_command_hook.h"
#include "readyup/command_buffer_hook.h"
#include "readyup/config.h"
#include "readyup/cs2_version.h"
#include "readyup/disabled.h"
#include "readyup/engine_surface.h"
#include "readyup/features.h"
#include "readyup/game_events.h"
#include "readyup/game_frame_hook.h"
#include "readyup/host_say_hook.h"
#include "readyup/logging.h"
#include "readyup/path.h"
#include "readyup/plugin_loader.h"
#include "readyup/plugin_needs.h"
#include "readyup/round_termination_hook.h"
#include "readyup/schema.h"
#include "readyup/steam_ugc.h"
#include "readyup/entity.h"
#include "readyup/version.h"

#include <unistd.h>

#include <atomic>
#include <chrono>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <mutex>
#include <sstream>
#include <thread>

#include "third_party/funchook/include/funchook.h"

namespace readyup {
namespace {

using Clock = std::chrono::steady_clock;

std::string Fmt(const char* fmt, ...) __attribute__((format(printf, 1, 2)));
std::string Fmt(const char* fmt, ...) {
  char buf[1024];
  va_list ap;
  va_start(ap, fmt);
  std::vsnprintf(buf, sizeof(buf), fmt, ap);
  va_end(ap);
  return buf;
}

std::string Hex(uintptr_t v) { return Fmt("0x%lx", static_cast<unsigned long>(v)); }

// Never called: only a detour target for funchook_prepare in AddFunchookSites.
void SelftestDummyDetour() {}

// Every schema field Ready Up reads, and whether its absence breaks a feature (required) or only
// degrades it (optional: e.g. scoreboard stats, which fall back to event accumulation).
struct FieldUse {
  const char* cls;
  const char* field;
  bool required;
  const char* user;
};
constexpr FieldUse kSchemaFields[] = {
    {"CEntityIdentity", "m_designerName", true, "entity system"},
    {"CEntityInstance", "m_pEntity", true, "entity system"},
    {"CCSPlayerController", "m_iTeamNum", true, "events (team of slot)"},
    {"CBasePlayerController", "m_steamID", true, "skins"},
    {"CCSPlayerController", "m_hPlayerPawn", true, "skins"},
    {"CBaseEntity", "m_iTeamNum", true, "skins"},
    {"CBaseEntity", "m_lifeState", true, "skins"},
    {"CBasePlayerPawn", "m_pWeaponServices", true, "skins"},
    {"CPlayer_WeaponServices", "m_hMyWeapons", true, "skins"},
    {"CEconEntity", "m_AttributeManager", true, "skins"},
    {"CEconEntity", "m_nFallbackPaintKit", true, "skins"},
    {"CEconEntity", "m_nFallbackSeed", true, "skins"},
    {"CEconEntity", "m_flFallbackWear", true, "skins"},
    {"CEconEntity", "m_nFallbackStatTrak", false, "skins (stattrak)"},
    {"CEconEntity", "m_OriginalOwnerXuidLow", false, "skins"},
    {"CEconEntity", "m_OriginalOwnerXuidHigh", false, "skins"},
    {"CAttributeContainer", "m_Item", true, "skins"},
    {"CEconItemView", "m_iItemDefinitionIndex", true, "skins"},
    {"CEconItemView", "m_iEntityQuality", false, "skins"},
    {"CEconItemView", "m_iItemID", false, "skins"},
    {"CEconItemView", "m_iItemIDHigh", true, "skins"},
    {"CEconItemView", "m_iItemIDLow", true, "skins"},
    {"CEconItemView", "m_iAccountID", false, "skins"},
    {"CEconItemView", "m_bInitialized", false, "skins"},
    {"CEconItemView", "m_AttributeList", true, "skins"},
    {"CEconItemView", "m_NetworkedDynamicAttributes", true, "skins"},
    {"CEconItemView", "m_szCustomName", false, "skins (name tags)"},
    {"CCSPlayerPawn", "m_EconGloves", true, "skins (gloves)"},
    {"CCSPlayerPawn", "m_nEconGlovesChanged", false, "skins (gloves)"},
    // plugins/midas (gold tint on weapons).
    {"CBaseModelEntity", "m_clrRender", false, "midas (gold tint)"},
    // plugins/match (match_events.cpp).
    {"CCSPlayerController", "m_iszPlayerName", false, "stats (bot names)"},
    // Scoreboard K / D / A / HS: controller -> action tracking services -> m_matchStats.
    {"CCSPlayerController", "m_pActionTrackingServices", false, "stats (else from events)"},
    {"CCSPlayerController_ActionTrackingServices", "m_matchStats", false, "stats (else from events)"},
    {"CSMatchStats_t", "m_iKills", false, "stats (else from events)"},
    {"CSMatchStats_t", "m_iDeaths", false, "stats (else from events)"},
    {"CSMatchStats_t", "m_iAssists", false, "stats (else from events)"},
    {"CSMatchStats_t", "m_iHeadShotKills", false, "stats (else from events)"},
    {"CCSPlayerController", "m_iMVPs", false, "stats"},
    {"CCSPlayerController", "m_iScore", false, "stats"},
};

// ---- Report builder ---------------------------------------------------------------------------

struct Report {
  std::vector<std::string> lines;
  std::vector<std::string> failures;
  int passed = 0, total = 0, pending = 0;

  void Section(const std::string& s) { lines.push_back("[" + s + "]"); }
  void Info(const std::string& s) { lines.push_back("  " + s); }
  // status: "OK" | "FAIL" | "PEND" | "SKIP" | "INFO" | "WARN"
  void Check(const char* status, const std::string& name, const std::string& detail) {
    lines.push_back(Fmt("  %-4s %-46s %s", status, name.c_str(), detail.c_str()));
    if (std::strcmp(status, "OK") == 0) {
      ++passed;
      ++total;
    } else if (std::strcmp(status, "FAIL") == 0) {
      ++total;
      failures.push_back(name);
    } else if (std::strcmp(status, "PEND") == 0) {
      ++pending;
    }
  }
  void Dep(const std::string& name, const DepStatus& s) {
    const char* st = s.state == DepStatus::State::Ok ? "OK" : s.state == DepStatus::State::Fail ? "FAIL" : "PEND";
    Check(st, name, s.detail);
  }
};

// ---- Sections -----------------------------------------------------------------------------------

void AddFunctions(Report& r, const es::EngineSurface& s) {
  r.Section("engine-surface functions");
  const es::Image* img = RealServerImage();
  const uintptr_t base = img && !img->regions.empty() ? img->regions.front().addr : 0;
  for (const auto& f : s.functions) {
    const es::Resolution res = EngineFunctionResolution(f.name.c_str());
    const std::string where =
        res.addr ? Fmt("%p (rva %s)", reinterpret_cast<void*>(res.addr), Hex(res.addr - base).c_str()) : std::string("-");
    r.Check(res.ok ? "OK" : "FAIL", "fn " + f.name,
            where + (f.required ? " required" : "") + (res.ok ? " verified; " : " UNVERIFIED; ") + res.detail);
  }
}

void AddRttiVtablesLayouts(Report& r, const es::EngineSurface& s) {
  r.Section("rtti");
  const es::Image* img = RealServerImage();
  for (const auto& rt : s.rtti) {
    if (rt.module != "server") {
      r.Check("INFO", "rtti " + rt.cls, rt.typeinfo_name + " (module " + rt.module + ": checked on the object at use)");
      continue;
    }
    std::string detail;
    const auto vt = img ? es::FindVtableByRtti(*img, rt.typeinfo_name, &detail) : std::nullopt;
    if (!img) detail = "real libserver.so not loaded";
    r.Check(vt ? "OK" : "FAIL", "rtti " + rt.cls, rt.typeinfo_name + "  " + detail);
  }

  r.Section("vtable slots");
  for (const auto& v : EngineVtableReport()) {
    const char* st = !v.checked ? "PEND" : v.ok ? "OK" : "FAIL";
    std::string detail = v.detail;
    if (v.ok) detail += v.patched ? "; PATCHED (hook live)" : "; not patched (called through / unused)";
    r.Check(st, Fmt("vtable %s [%d]", v.name.c_str(), v.index), detail);
  }

  r.Section("struct layouts");
  for (const auto& l : EngineLayoutReport()) {
    std::string fields;
    for (const auto& f : l.fields) fields += Fmt(" %s=0x%x", f.first.c_str(), f.second);
    r.Check(l.ok ? "OK" : "FAIL", "layout " + l.name, fields.substr(fields.empty() ? 0 : 1) + " (verified by " + l.verified_by + ")");
  }
}

void AddSchema(Report& r) {
  r.Section("schema");
  std::string detail;
  const int st = SchemaStatus(&detail);
  r.Check(st == 1 ? "OK" : st == 2 ? "FAIL" : "PEND", "schema system", detail);
  if (st != 1) {
    r.Info(Fmt("(%zu schema fields not checked: schema %s)", sizeof(kSchemaFields) / sizeof(kSchemaFields[0]),
               st == 2 ? "failed verification" : "not available yet"));
    return;
  }
  // Resolve every declared field first (fills the lookup registry), then report the registry,
  // which also shows any other field looked up at runtime.
  for (const auto& f : kSchemaFields) (void)SchemaFindOffset("server", f.cls, f.field);
  (void)SchemaClassSize("CEntityIdentity");
  for (const auto& l : SchemaLookupReport()) {
    const FieldUse* use = nullptr;
    for (const auto& f : kSchemaFields) {
      if (l.cls == f.cls && l.field == f.field) use = &f;
    }
    const bool required = !use || use->required;  // undeclared lookups count as required
    const std::string name = "schema " + l.cls + "::" + l.field;
    const std::string who = use ? std::string(" [") + use->user + "]" : std::string(" [runtime lookup]");
    if (l.offset) {
      r.Check("OK", name, Fmt("0x%x", *l.offset) + who);
    } else if (l.cls == "CCSPlayerController" && l.field.find("Head") != std::string::npos) {
      continue;  // headshot-count candidates: several spellings are tried, one may exist
    } else {
      r.Check(required ? "FAIL" : "WARN", name, "not found" + who);
    }
  }
}

void AddRuntime(Report& r) {
  r.Section("runtime");
  r.Dep("hook GameFrame (tick)", DependencyStatus("hook:GameFrame"));
  r.Check(GameFrameSimulatingTicks() > 0 ? "OK" : "PEND", "GameFrame ticking",
          Fmt("%llu simulating ticks", GameFrameSimulatingTicks()));
  r.Dep("hook ClientCommand", DependencyStatus("hook:ClientCommand"));
  {
    DepStatus cb = DependencyStatus("cmdbuf");
    cb.detail += CommandBufferSeen() ? "; engine used it (commands can be queued)" : "; not used by the engine yet";
    r.Dep("command buffer (server commands)", cb);
  }
  r.Dep("log listener (chat + lifecycle)", DependencyStatus("loglistener"));
  r.Dep("event manager", DependencyStatus("eventmgr"));
  {
    const GameEventsStatus ev = GetGameEventsStatus();
    const std::string d = Fmt("manager=%s listener=%s delivered=%s (%llu events%s%s)", ev.manager ? "yes" : "no",
                              ev.listenerRegistered ? "yes" : "no", ev.delivered ? "yes" : "no", ev.count,
                              ev.lastEvent.empty() ? "" : ", last ", ev.lastEvent.c_str());
    // Informational: on 1.41.8.3 AddListener succeeds but the engine never delivers; Ready Up then
    // drives the match from log lines (not a surface failure).
    r.Check(ev.delivered ? "OK" : "WARN", "engine event delivery", d);
  }
  r.Dep("entity system", DependencyStatus("entsys"));

  // Chat output paths (the functions themselves are counted above).
  const bool all = EngineFunctionResolution("UTIL_ClientPrintAll").ok;
  const bool one = EngineFunctionResolution("ClientPrint").ok;
  r.Check("INFO", "clientprint",
          Fmt("broadcast=%s (UTIL_ClientPrintAll%s) per-player=%s (ClientPrint)", all ? "yes" : "no",
              all ? "" : "; falls back to `say`", one ? "yes" : "no"));
  // Workshop download progress (plugin API 1.4): Steam's flat C API, not the engine surface.
  r.Check("INFO", "steam ugc", steam_ugc::Status());
}

// Runtime equivalent of readyup_hookcheck: every "hook": "funchook" function must be detoured
// already, or funchook must be able to relocate its live prologue (prepare only, never installed).
void AddFunchookSites(Report& r, const es::EngineSurface& s) {
  r.Section("funchook detour sites (hookcheck)");
  for (const auto& f : s.functions) {
    if (f.hook != "funchook") continue;
    const es::Resolution res = EngineFunctionResolution(f.name.c_str());
    if (!res.ok) {
      r.Check("FAIL", "hook " + f.name, "unresolved; cannot be detoured");
      continue;
    }
    bool live = false;
    std::string role;
    if (f.name == "Host_Say") {
      live = HostSayHookInstalled();
      role = "chat";
    } else if (f.name == "CCSGameRules_TerminateRound") {
      live = RoundTerminationHookInstalled();
      role = "installed on first warmup/practice suppression";
    } else if (f.name == "CGameEventManager_Init") {
      live = GameEventsInitHookInstalled();
      role = "fallback manager capture; only if the Init call site was unreadable";
    }
    if (live) {
      r.Check("OK", "hook " + f.name, "detour live (" + role + ")");
      continue;
    }
    // Not installed (yet): prove the prologue still relocates. prepare allocates a trampoline
    // and copies instructions; the target itself is not modified.
    funchook_t* fh = funchook_create();
    void* target = reinterpret_cast<void*>(res.addr);
    const int rv = fh ? funchook_prepare(fh, &target, reinterpret_cast<void*>(&SelftestDummyDetour)) : -1;
    const std::string err = fh && rv != 0 ? funchook_error_message(fh) : std::string();
    if (fh) funchook_destroy(fh);
    r.Check(rv == 0 ? "OK" : "FAIL", "hook " + f.name,
            rv == 0 ? "not installed (" + role + "); prologue relocates" : "funchook_prepare failed: " + err);
  }
}

// Every plugin manifest (csgo/readyup/plugins/<name>.needs.json) evaluated against this build now:
// "need <plugin> <surface|schema|schema_optional|event> <entry>". A miss is a WARN (the plugin is
// disabled or degraded, Ready Up itself is fine); scripts/ci/compat-report.py turns these into
// the per-plugin schema / event verdicts of docs/CS2-COMPAT.md.
void AddPluginNeeds(Report& r) {
  const auto all = plugins::EvaluateAllNeedsNow();
  if (all.empty()) return;
  r.Section("plugin needs");
  for (const auto& p : all) {
    for (const auto& c : p.verdict.checks) {
      r.Check(c.state == 1 ? "OK" : c.state == 0 ? "WARN" : "PEND", "need " + p.plugin + " " + c.kind + " " + c.entry,
              c.detail);
    }
  }
}

void AddPluginsAndHud(Report& r) {
  r.Section("plugins");
  const plugins::PluginHostStatus p = plugins::GetPluginHostStatus();
  if (p.disabledByEnv) {
    r.Check("SKIP", "plugin host", "disabled by READYUP_PLUGINS=0");
  } else {
    std::string loaded;
    for (const auto& l : p.loaded) loaded += (loaded.empty() ? "" : ", ") + l;
    const std::string d = Fmt("api %u.%u, dir %s, %zu loaded%s%s", p.apiMajor, p.apiMinor, p.dir.c_str(), p.loaded.size(),
                              loaded.empty() ? "" : ": ", loaded.c_str());
    r.Check(p.started ? "OK" : "PEND", "plugin host", p.started ? d : d + " (plugins load on the first server frame)");
    for (const auto& f : p.failures) r.Check("FAIL", "plugin load", f);
    // Lines plugins add through "readyup.selftest.<name>" (core/include/readyup/selftest_iface.h).
    for (const auto& c : plugins::RunPluginSelftests()) {
      static const char* const kKnown[] = {"OK", "FAIL", "PEND", "SKIP", "INFO", "WARN"};
      const char* st = "INFO";
      for (const char* k : kKnown) {
        if (c.status == k) st = k;
      }
      r.Check(st, c.plugin + ": " + c.name, c.detail);
    }
    // Plugins not loaded because their needs.json is not met on this build (plugin_needs.h).
    for (const auto& d : plugins::NeedsDisabledPlugins()) r.Check("WARN", d.name + ": disabled", d.reason);
  }

  // (The database, ready HUD and hud brand lines come from plugins/match:
  // "readyup.selftest.match", in the [plugins] section above.)
}

void AddFeatures(Report& r) {
  r.Section("features");
  for (const auto& f : FeatureReports()) {
    std::string needs;
    for (const auto& g : f.needs) {
      if (!needs.empty()) needs += ", ";
      for (size_t i = 0; i < g.size(); ++i) needs += (i ? "|" : "") + g[i];
    }
    std::string line = Fmt("  %-7s %-24s needs: %s", f.state == "on" ? "on" : f.state == "off" ? "OFF" : "pending",
                           f.name.c_str(), needs.c_str());
    if (f.state != "on" && !f.missing.empty()) line += "  -- missing: " + f.missing;
    r.lines.push_back(line);
  }
}

SelftestResult Build(const std::string& extraFailure) {
  Report r;
  const Cs2VersionSnapshot v = GetCs2VersionSnapshot();
  const es::EngineSurface* s = GetEngineSurface();
  r.lines.push_back(Fmt("selftest: Ready Up %s on CS2 %s (build %lld); engine-surface for %s", BuildVersion(),
                        v.version_string ? v.version_string->c_str() : "?", v.build_id ? *v.build_id : -1LL,
                        s ? s->game_version.c_str() : "?"));
  if (IsDisabled()) r.Check("FAIL", "readyup enabled", "Ready Up disabled: " + DisabledReason());
  if (!extraFailure.empty()) r.Check("FAIL", "selftest-and-quit", extraFailure);
  if (!s) {
    r.Check("FAIL", "engine-surface.json", "unavailable (parse error)");
  } else {
    AddFunctions(r, *s);
    AddRttiVtablesLayouts(r, *s);
    AddFunchookSites(r, *s);
  }
  AddSchema(r);
  AddRuntime(r);
  AddPluginsAndHud(r);
  AddPluginNeeds(r);
  AddFeatures(r);

  SelftestResult out;
  out.passed = r.passed;
  out.total = r.total;
  out.pending = r.pending;
  out.failures = r.failures;
  out.pass = r.failures.empty();
  std::string sum = Fmt("selftest: %s %d/%d", out.pass ? "PASS" : "FAIL", r.passed, r.total);
  if (!out.pass) {
    sum += " (";
    for (size_t i = 0; i < r.failures.size() && i < 4; ++i) sum += (i ? "; " : "") + r.failures[i];
    if (r.failures.size() > 4) sum += Fmt("; +%zu more", r.failures.size() - 4);
    sum += ")";
  }
  if (r.pending) sum += Fmt(" [%d pending]", r.pending);
  r.lines.push_back(sum);
  out.summary = sum;
  out.lines = std::move(r.lines);
  return out;
}

// ---- Selftest-and-quit --------------------------------------------------------------------------

std::atomic<bool> g_quitDone{false};
Clock::time_point g_firstTick{};
std::atomic<bool> g_sawTick{false};

bool EnvFlag(const char* name) {
  const char* v = std::getenv(name);
  return v && *v && !(v[0] == '0' || v[0] == 'n' || v[0] == 'N' || v[0] == 'f' || v[0] == 'F');
}

int EnvInt(const char* name, int def) {
  const char* v = std::getenv(name);
  if (!v || !*v) return def;
  const int n = std::atoi(v);
  return n > 0 ? n : def;
}

bool CmdlineHas(const char* flag) {
  std::ifstream f("/proc/self/cmdline", std::ios::binary);
  std::string arg;
  while (std::getline(f, arg, '\0')) {
    if (arg == flag) return true;
  }
  return false;
}

std::string StatusFilePath() {
  if (const char* p = std::getenv("READYUP_SELFTEST_FILE"); p && *p) return p;
  const std::string dir = GetThisModuleDir();
  return (dir.empty() ? std::string(".") : dir) + "/readyup_selftest.txt";
}

[[noreturn]] void QuitWith(int code) {
  // Ask the engine to shut down cleanly; if it does not within 20 s (or cannot queue commands),
  // leave hard so a CI job never hangs.
  const bool queued = EnqueueServerCommand("quit");
  if (queued) {
    for (int i = 0; i < 200; ++i) std::this_thread::sleep_for(std::chrono::milliseconds(100));
  }
  std::fflush(nullptr);
  _exit(code);
}

void FinishAndQuit(const std::string& extraFailure, bool fromGameThread) {
  if (g_quitDone.exchange(true)) return;
  const SelftestResult r = Build(extraFailure);
  for (const auto& l : r.lines) Print("%s\n", l.c_str());
  const std::string path = StatusFilePath();
  {
    std::ofstream f(path, std::ios::trunc);
    for (const auto& l : r.lines) f << l << "\n";
    f << "exit_code: " << (r.pass ? 0 : 1) << "\n";
  }
  Print("selftest: report written to %s; quitting (READYUP_SELFTEST_AND_QUIT)\n", path.c_str());
  const int code = r.pass ? 0 : 1;
  if (fromGameThread) {
    // Don't block the game thread: queue `quit` now, hard-exit from a helper thread as backstop.
    const bool queued = EnqueueServerCommand("quit");
    std::thread([code, queued] {
      std::this_thread::sleep_for(std::chrono::seconds(queued ? 20 : 0));
      std::fflush(nullptr);
      _exit(code);
    }).detach();
    return;
  }
  QuitWith(code);
}

}  // namespace

SelftestResult RunSelftest(bool printToConsole) {
  SelftestResult r = Build({});
  status_feed::NoteSelftest(r);  // last result for /health, /status and /selftest
  if (printToConsole) {
    for (const auto& l : r.lines) Print("%s\n", l.c_str());
  }
  return r;
}

bool SelftestAndQuitRequested() {
  static const bool on = EnvFlag("READYUP_SELFTEST_AND_QUIT") || CmdlineHas("-readyup_selftest_and_quit");
  return on;
}

void StartSelftestWatchdogIfRequested() {
  if (!SelftestAndQuitRequested()) return;
  static std::atomic<bool> started{false};
  if (started.exchange(true)) return;
  const int timeoutS = EnvInt("READYUP_SELFTEST_TIMEOUT", 300);
  Print("selftest: READYUP_SELFTEST_AND_QUIT set; will run after the first map (timeout %d s), report -> %s\n", timeoutS,
        StatusFilePath().c_str());
  std::thread([timeoutS] {
    const auto deadline = Clock::now() + std::chrono::seconds(timeoutS);
    while (Clock::now() < deadline) {
      if (g_quitDone.load()) return;
      // Ready Up disabled at load (required signature failed): nothing will ever tick. Report now.
      if (IsDisabled()) {
        FinishAndQuit("Ready Up disabled at load", /*fromGameThread=*/false);
        return;
      }
      std::this_thread::sleep_for(std::chrono::seconds(1));
    }
    if (g_quitDone.load()) return;
    FinishAndQuit(Fmt("no simulating GameFrame within %d s (map never loaded or GameFrame not hooked)", timeoutS),
                  /*fromGameThread=*/false);
  }).detach();
}

void SelftestFrameTick() {
  if (!SelftestAndQuitRequested() || g_quitDone.load(std::memory_order_relaxed)) return;
  if (!g_sawTick.exchange(true)) {
    g_firstTick = Clock::now();
    return;
  }
  const int delayS = EnvInt("READYUP_SELFTEST_DELAY", 5);
  if (Clock::now() - g_firstTick < std::chrono::seconds(delayS)) return;
  FinishAndQuit({}, /*fromGameThread=*/true);
}

}  // namespace readyup

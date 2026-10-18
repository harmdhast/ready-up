#include "readyup/game_frame_hook.h"

#include "readyup/entity.h"

#include "readyup/command_buffer_hook.h"
#include "readyup/config.h"
#include "readyup/engine_surface.h"
#include "readyup/features.h"
#include "readyup/game_events.h"
#include "readyup/license_status.h"
#include "readyup/logging.h"
#include "readyup/plugin_loader.h"
#include "readyup/practice_engine.h"
#include "readyup/real_server.h"
#include "readyup/selftest.h"
#include "readyup/slot_registry.h"
#include "readyup/status_feed.h"

#include <sys/mman.h>
#include <unistd.h>

#include <atomic>
#include <cstring>
#include <mutex>

namespace readyup {
namespace {

constexpr const char* kSlotName = "ISource2Server::GameFrame";

// Matches CS2 HL2SDK: ISource2Server::GameFrame(bool simulating, bool bFirstTick, bool bLastTick)
using GameFrameFn = void (*)(void* thisptr, bool simulating, bool bFirstTick, bool bLastTick);
GameFrameFn g_orig = nullptr;

std::mutex g_installMu;
std::atomic<bool> g_ok{false};
std::atomic<bool> g_failed{false};
std::string g_failDetail;  // guarded by g_installMu
std::atomic<unsigned long long> g_simTicks{0};

static bool PatchVtable(void** vtable, int index, void* replacement, void** outOld) {
  if (!vtable || index < 0) return false;
  void** slot = &vtable[index];
  const uintptr_t addr = reinterpret_cast<uintptr_t>(slot);
  const uintptr_t page = addr & ~(static_cast<uintptr_t>(getpagesize() - 1));
  if (mprotect(reinterpret_cast<void*>(page), static_cast<size_t>(getpagesize()), PROT_READ | PROT_WRITE) != 0) {
    return false;
  }
  if (outOld) *outOld = *slot;
  *slot = replacement;
  mprotect(reinterpret_cast<void*>(page), static_cast<size_t>(getpagesize()), PROT_READ);
  return true;
}

static void Hook_GameFrame(void* thisptr, bool simulating, bool bFirstTick, bool bLastTick) {
  if (g_orig) g_orig(thisptr, simulating, bFirstTick, bLastTick);

  // Our logic is best-effort. Every feature below gates itself on its engine-surface
  // dependencies (features.cpp; one log line when one is disabled).
  const bool plugins = FeatureEnabled(Feature::Plugins);
  // Ready Up reads chat, rounds and the knife winner from the game log: turn it on once (it
  // survives map changes), retried until the command buffer accepts the command.
  static std::atomic<bool> s_logOn{false};
  if (!s_logOn.load(std::memory_order_relaxed) && CommandBufferSeen() && EnqueueServerCommand("log on")) {
    s_logOn.store(true, std::memory_order_relaxed);
    PrintLine("logging: log on (Ready Up reads chat, rounds and the knife winner from the game log)");
  }
  if (!simulating) {
    // Plugin load/unload + queued commands/events still run while not simulating.
    if (plugins) readyup::plugins::Frame(/*simulating=*/false);
    readyup::status_feed::FrameTick(/*simulating=*/false);
    return;
  }
  if (g_simTicks.fetch_add(1, std::memory_order_relaxed) == 0 && DebugEnabled()) {
    DebugLine("gameframe: first simulating tick observed");
  }
  // Engine event listener: register / verify (throttled; gated by the "events" feature).
  readyup::GameEventsFrameTick();
  // Entity primitives status (once the entity system verified on the first map). Skins itself
  // is a plugin now (plugins/skins); its per-tick work runs in plugins::Frame below.
  if (readyup::entity::EntitySystemReady()) readyup::entity::LogEngineStatusOnce();
  // Human teams from the engine, about once a second (a byte per connected human): a player who
  // stays on their side over a map load sends no player_team event, and the registry kept the 0
  // the map load gave them (2026-10-17: the team lock moved BBro to CT 260 times).
  {
    static int s_teamFrames = 0;
    if (s_teamFrames-- <= 0 && readyup::entity::EntitySystemReady()) {
      s_teamFrames = 64;
      const int n = readyup::ReconcileHumanTeams(&readyup::practice_engine::TeamOf);
      if (n > 0 && DebugEnabled()) Debug("teams: %d registry team(s) taken from the engine\n", n);
    }
  }
  // Plugins last: pending load/unload/reload (a safe point: no plugin code is on the
  // stack), then queued commands/events, then per-tick callbacks. The match flow (ready-up,
  // knife, HUD, timers) is plugins/match and runs here too.
  if (plugins) {
    static int s_cfgFrames = 0;
    if (s_cfgFrames-- <= 0) {  // about every 5 s at 64 tick (Cfg() copies the whole config)
      s_cfgFrames = 320;
      const readyup::ReadyUpCfg c = readyup::Cfg();
      readyup::plugins::SetPerfThresholds(c.perf_warn_ms, c.perf_gap_warn_ms);
    }
    readyup::plugins::Frame(/*simulating=*/true);
  }
  // No license key anywhere ~10 s into the first map: one free-use console line (never blocks).
  readyup::license::LicenseFrame();
  // READYUP_SELFTEST_AND_QUIT: runs the selftest once the first map is up, then quits.
  readyup::SelftestFrameTick();
  // Local status endpoint: rebuild the snapshot (at most every 250 ms) and hand it to the
  // HTTP thread with a pointer swap (status_feed.h). Last, so it sees this frame's state.
  readyup::status_feed::FrameTick(/*simulating=*/true);
}

}  // namespace

void InstallGameFrameHook(void* source2ServerIface) {
  if (!source2ServerIface || g_ok.load(std::memory_order_acquire)) return;
  std::lock_guard<std::mutex> lk(g_installMu);
  if (g_ok.load(std::memory_order_acquire)) return;

  int index = -1;
  std::string why;
  if (!VerifyEngineVtableSlot(kSlotName, source2ServerIface, &index, &why)) {
    // Unverified slot: final, logged once. Another interface whose factory name merely contains
    // "Source2Server" (e.g. Source2ServerConfig001) is skipped quietly.
    const VtableVerdict v = EngineVtableVerdict(kSlotName);
    if (!v.ok && !g_failed.exchange(true)) {
      g_failDetail = why;
      Print("gameframe: GameFrame hook NOT installed (%s)\n", why.c_str());
    } else if (v.ok) {
      Debug("gameframe: skipping interface %p: %s\n", source2ServerIface, why.c_str());
    }
    return;
  }

  void** vt = *reinterpret_cast<void***>(source2ServerIface);
  void* old = nullptr;
  if (!PatchVtable(vt, index, reinterpret_cast<void*>(&Hook_GameFrame), &old) || !old) {
    g_failDetail = "mprotect/patch of the vtable slot failed";
    g_failed.store(true);
    PrintLine("gameframe: failed to patch vtable for GameFrame.");
    return;
  }

  g_orig = reinterpret_cast<GameFrameFn>(old);
  g_ok.store(true, std::memory_order_release);
  MarkEngineVtablePatched(kSlotName, old);
  Print("gameframe: hooked GameFrame vtbl[%d] (verified) old=%p new=%p\n", index, old,
        reinterpret_cast<void*>(&Hook_GameFrame));
}

void InstallGameFrameHookAuto() {
  int rc = 0;
  void* iface = RealCreateInterface("Source2Server001", &rc);
  if (!iface) {
    if (DebugEnabled()) Print("gameframe: RealCreateInterface(Source2Server001) failed (rc=%d)\n", rc);
    return;
  }
  InstallGameFrameHook(iface);
}

bool GameFrameHookInstalled() {
  return g_ok.load(std::memory_order_acquire);
}

bool GameFrameHookFailed(std::string* detail) {
  if (!g_failed.load(std::memory_order_acquire)) return false;
  if (detail) {
    std::lock_guard<std::mutex> lk(g_installMu);
    *detail = g_failDetail;
  }
  return true;
}

unsigned long long GameFrameSimulatingTicks() {
  return g_simTicks.load(std::memory_order_relaxed);
}

}  // namespace readyup

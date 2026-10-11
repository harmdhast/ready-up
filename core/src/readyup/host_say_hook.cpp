#include "readyup/host_say_hook.h"

#include "readyup/ccommand.h"
#include "readyup/chat_intercept.h"
#include "readyup/engine_surface.h"
#include "readyup/game_events.h"
#include "readyup/logging.h"

#include <atomic>
#include <cstdint>
#include <string>

#include "third_party/funchook/include/funchook.h"

namespace readyup {
namespace {

using HostSayFn = void (*)(void* controller, void* cmdRef, bool teamonly, int unk1, const char* unk2);
HostSayFn g_hostSay = nullptr;  // will be rewritten to trampoline after prepare
HostSayFn g_hostSay_orig = nullptr;

std::atomic<bool> g_installed{false};
std::atomic<bool> g_hook_ok{false};
funchook_t* g_hook = nullptr;

// Sender slot = the slot whose controller (as seen in engine events) is this pointer.
// No vtable guessing: the old code searched the controller's vtable for a dladdr() symbol name
// containing "GetEntityIndex", which never resolves on stripped Valve builds.
int SlotForController(void* controller) {
  if (!controller) return -1;
  for (int slot = 0; slot < 64; ++slot) {
    if (GameEventsControllerForSlot(slot) == controller) return slot;
  }
  return -1;
}

static void DetourHostSay(void* controller, void* cmdRef, bool teamonly, int unk1, const char* unk2) {
  // Arguments via the verified CCommand layout only (no pointer-scanning for strings).
  const auto argv = ReadCCommandArgs(cmdRef);
  if (!argv || argv->size() < 2) return g_hostSay(controller, cmdRef, teamonly, unk1, unk2);

  // A console `say` has no controller: not a player line.
  const int slot = SlotForController(controller);
  if (slot < 0) return g_hostSay(controller, cmdRef, teamonly, unk1, unk2);

  if (HandlePlayerChat(slot, CCommandArgString(*argv), teamonly)) return;
  g_hostSay(controller, cmdRef, teamonly, unk1, unk2);
}

}  // namespace

void InstallHostSayHook() {
  if (g_installed.exchange(true)) return;

  // Unresolved => nothing to hook; chat still works through the log listener / ClientCommand.
  void* p = EngineFunction("Host_Say");
  if (!p) return;

  g_hostSay_orig = reinterpret_cast<HostSayFn>(p);
  g_hostSay = g_hostSay_orig;

  g_hook = funchook_create();
  if (!g_hook) {
    PrintLine("hostsay: funchook_create failed");
    return;
  }

  int rv = funchook_prepare(g_hook, (void**)&g_hostSay, (void*)&DetourHostSay);
  if (rv != 0) {
    Print("hostsay: funchook_prepare failed (%d): %s\n", rv, funchook_error_message(g_hook));
    funchook_destroy(g_hook);
    g_hook = nullptr;
    return;
  }

  rv = funchook_install(g_hook, 0);
  if (rv != 0) {
    Print("hostsay: funchook_install failed (%d): %s\n", rv, funchook_error_message(g_hook));
    funchook_destroy(g_hook);
    g_hook = nullptr;
    return;
  }

  g_hook_ok.store(true, std::memory_order_release);
  Print("hostsay: hooked Host_Say at %p (trampoline=%p)\n", reinterpret_cast<void*>(g_hostSay_orig), reinterpret_cast<void*>(g_hostSay));
}

bool HostSayHookInstalled() {
  return g_hook_ok.load(std::memory_order_acquire);
}

}  // namespace readyup

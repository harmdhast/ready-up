#include "readyup/server_game_clients_hook.h"

#include "readyup/ccommand.h"
#include "readyup/chat_intercept.h"
#include "readyup/engine_surface.h"
#include "readyup/logging.h"
#include "readyup/ru_router.h"
#include "readyup/slot_registry.h"

#include <sys/mman.h>
#include <unistd.h>

#include <atomic>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>

namespace readyup {
namespace {

constexpr const char* kSlotName = "ISource2GameClients::ClientCommand";

// Minimal ABI declarations matching Source2 SDK usage (see Metamod Source2 provider).
// On CS2 this is a tiny wrapper around an int.
struct CPlayerSlot {
  int value;
};

using ClientCommandFn = void (*)(void* thisptr, CPlayerSlot slot, const void* args);
ClientCommandFn g_origClientCommand = nullptr;

std::mutex g_installMu;
std::atomic<bool> g_hook_ok{false};
std::atomic<bool> g_failed{false};
std::string g_failDetail;  // guarded by g_installMu

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

static void Hook_ClientCommand(void* thisptr, CPlayerSlot slot, const void* args) {
  if (!g_origClientCommand) return;

  // Arguments come from the verified CCommand layout; anything unexpected => pure pass-through.
  const auto argv = ReadCCommandArgs(args);
  if (!argv || argv->empty()) {
    static std::atomic<bool> s_warned{false};
    if (!argv && !s_warned.exchange(true)) {
      PrintLine("client-command: could not read CCommand arguments (layout sanity check failed); passing commands through.");
    }
    return g_origClientCommand(thisptr, slot, args);
  }
  const std::string& cmd = (*argv)[0];

  const int slotNum = slot.value;
  const auto ident = GetSlotIdentity(slotNum);
  Debug("client-command: slot=%d cmd=\"%s\" argc=%zu ident=%s steamid64=%llu\n", slotNum, cmd.c_str(), argv->size(),
        ident ? "yes" : "no", static_cast<unsigned long long>(ident ? ident->steamid64 : 0ull));

  // `ru ...` typed in a player's own console (like Metamod's `meta` / CounterStrikeSharp's
  // `css_...`): the same as `.ru ...` in chat (same admin checks, replies to that player), and
  // nothing shows in chat.
  if (cmd == "ru" && ident) {
    std::string line = ".ru";
    for (size_t i = 1; i < argv->size(); ++i) line += " " + (*argv)[i];
    Debug("client-command: console `ru` from slot %d: \"%s\"\n", slotNum, line.c_str());
    RouteChatCommand(ident->steamid64, ident->name, line, slotNum);
    return;
  }

  if (cmd != "say" && cmd != "say_team") return g_origClientCommand(thisptr, slot, args);
  if (!ident) return g_origClientCommand(thisptr, slot, args);

  const bool isTeamOnly = (cmd == "say_team");
  const std::string msg = CCommandArgString(*argv);

  if (HandlePlayerChat(slotNum, msg, isTeamOnly)) return;
  return g_origClientCommand(thisptr, slot, args);
}

}  // namespace

void InstallServerGameClientsHook(void* serverGameClientsIface) {
  if (!serverGameClientsIface || g_hook_ok.load(std::memory_order_acquire)) return;
  std::lock_guard<std::mutex> lk(g_installMu);
  if (g_hook_ok.load(std::memory_order_acquire)) return;

  int index = -1;
  std::string why;
  if (!VerifyEngineVtableSlot(kSlotName, serverGameClientsIface, &index, &why)) {
    // An unverified slot is final and logged once; a different interface object (another
    // *GameClients* factory name) is simply skipped.
    const VtableVerdict v = EngineVtableVerdict(kSlotName);
    if (!v.ok && !g_failed.exchange(true)) {
      g_failDetail = why;
      Print("client-command: ClientCommand hook NOT installed (%s)\n", why.c_str());
    } else if (v.ok) {
      Debug("client-command: skipping interface %p: %s\n", serverGameClientsIface, why.c_str());
    }
    return;
  }

  void** vt = *reinterpret_cast<void***>(serverGameClientsIface);
  void* old = nullptr;
  if (!PatchVtable(vt, index, reinterpret_cast<void*>(&Hook_ClientCommand), &old) || !old) {
    g_failDetail = "mprotect/patch of the vtable slot failed";
    g_failed.store(true);
    PrintLine("client-command: failed to patch vtable for ClientCommand.");
    return;
  }

  g_origClientCommand = reinterpret_cast<ClientCommandFn>(old);
  g_hook_ok.store(true, std::memory_order_release);
  MarkEngineVtablePatched(kSlotName, old);
  Print("client-command: hooked ISource2GameClients::ClientCommand vtbl[%d] (verified)\n", index);
}

bool ServerGameClientsHookInstalled() {
  return g_hook_ok.load(std::memory_order_acquire);
}

bool ServerGameClientsHookFailed(std::string* detail) {
  if (!g_failed.load(std::memory_order_acquire)) return false;
  if (detail) {
    std::lock_guard<std::mutex> lk(g_installMu);
    *detail = g_failDetail;
  }
  return true;
}

bool ForceJoinTeamForSlot(int /*slot*/, int /*joinTeam*/) {
  return false;
}

}  // namespace readyup

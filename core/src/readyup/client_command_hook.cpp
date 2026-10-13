#include "readyup/game_events.h"
#include "readyup/client_command_hook.h"

#include "readyup/config.h"
#include "readyup/host_say_hook.h"
#include "readyup/ru_router.h"
#include "readyup/log_header.h"
#include "readyup/log_receiver.h"
#include "readyup/logging.h"
#include "readyup/plugin_api.h"
#include "readyup/plugin_loader.h"
#include "readyup/slot_registry.h"
#include "readyup/steamid.h"

#include <dlfcn.h>

#include <algorithm>
#include <atomic>
#include <cstdlib>
#include <cstring>
#include <string>

namespace readyup {
namespace {

using RegisterLoggingListenerFn = void (*)(void* listener);

struct Listener {
  void** vptr;
};

std::atomic<bool> g_installed{false};
std::atomic<bool> g_active{false};
RegisterLoggingListenerFn g_register = nullptr;

static bool ExtractQuoted(const std::string& s, size_t& i, std::string& out) {
  const size_t q1 = s.find('"', i);
  if (q1 == std::string::npos) return false;
  const size_t q2 = s.find('"', q1 + 1);
  if (q2 == std::string::npos) return false;
  out = s.substr(q1 + 1, q2 - (q1 + 1));
  i = q2 + 1;
  return true;
}

static void ParseHeader(const std::string& header, std::string& nameOut, int& slotOut, uint64_t& steamid64Out) {
  // Format usually: name<userid><steamid><team>
  nameOut.clear();
  slotOut = -1;
  steamid64Out = 0;

  const size_t a = header.find('<');
  if (a == std::string::npos) {
    nameOut = header;
    return;
  }
  nameOut = header.substr(0, a);

  const size_t b = header.find('>', a + 1);
  if (b != std::string::npos) {
    const std::string slotStr = header.substr(a + 1, b - (a + 1));
    slotOut = std::atoi(slotStr.c_str());
  }

  const size_t c = header.find('<', b == std::string::npos ? 0 : b + 1);
  const size_t d = header.find('>', c == std::string::npos ? 0 : c + 1);
  if (c == std::string::npos || d == std::string::npos) return;

  // SteamID is the *second* <> group: name<userid><steamid><team>
  const std::string steam = header.substr(c + 1, d - (c + 1));
  steamid64Out = ParseSteamId64Loose(steam);
}

static void HandleLogLine(const char* lineCStr) {
  if (!lineCStr) return;
  std::string line(lineCStr);
  if (line.empty()) return;

  // Avoid feedback loops: our own Msg()/Print() output can be observed by this listener.
  if (line.find(kLogPrefix) != std::string::npos) return;

  const bool isChat =
      line.find("\" say \"") != std::string::npos || line.find("\" say_team \"") != std::string::npos;

  // Bots are re-added (with new userids) on map load; drop stale ones.
  // Humans stay connected across a map change but must pick a team again.
  if (!isChat && (line.find("Loading map \"") != std::string::npos || line.find("Started map \"") != std::string::npos)) {
    ClearBots();
    ResetHumanTeams();
  }

  // Plugin lifecycle events from log lines (map, Match_Start, rounds) and the line itself for
  // subscribe_log_line (the match plugin drives its round lifecycle from them when engine
  // events are not delivered).
  ObserveLifecycleLogLine(line);

  // Typical: L 02/07/2026 - 12:00:00: "Name<...><STEAM_...><...>" say "message"
  size_t i = 0;
  std::string header;
  if (!ExtractQuoted(line, i, header)) return;

  std::string name;
  int slot = -1;
  uint64_t steamid64 = 0;
  ParseHeader(header, name, slot, steamid64);

  // Bots: `Name<userid><BOT><TEAM>`. No SteamID, so track them separately
  // (only consumed when dev_bots_ready is on). Never routed as chat senders.
  if (steamid64 == 0 && slot >= 0 && IdGroupFromHeader(header) == "BOT") {
    if (line.find("\" disconnected") != std::string::npos) {
      ForgetBot(slot);
    } else {
      int team = TeamFromHeader(header);
      const int switched = isChat ? -1 : TeamFromSwitchLine(line);
      if (switched >= 0) team = switched;
      ObserveBot(slot, name, team);
    }
    Debug("chat-hook: bot header=\"%s\" -> name=\"%s\" userid=%d\n", header.c_str(), name.c_str(), slot);
    return;
  }

  // Only treat as a player header if it looks like one.
  if (header.find('<') != std::string::npos && steamid64 != 0) {
    int team = TeamFromHeader(header);
    const int switched = isChat ? -1 : TeamFromSwitchLine(line);
    if (switched >= 0) team = switched;
    const bool disconnect = !isChat && line.find("\" disconnected (reason") != std::string::npos;
    if (!isChat && (line.find(" connected, address") != std::string::npos ||
                    line.find("\" entered the game") != std::string::npos ||
                    line.find("STEAM USERID validated") != std::string::npos ||
                    line.find("switched from team <Unassigned>") != std::string::npos)) {
      ObserveHumanArrival(steamid64);
    }
    // CS2 keeps logging lines for a player right after their disconnect line.
    const bool departed = !disconnect && IsHumanDeparted(steamid64);
    if (!departed) {
      ObserveSlotIdentity(slot, steamid64, name);
      if (team >= 0) ObserveSlotTeamFromLog(slot, team);
    }
    // Plugin lifecycle events from log lines, only when engine events are not driving them.
    if (!isChat && !GameEventsListenerInstalled()) {
      plugins::LifecycleEvent e;
      e.source = RU_SOURCE_LOG;
      e.slot = slot;
      e.steamid64 = steamid64;
      e.name = name;
      if (line.find("\" disconnected (reason") != std::string::npos) {
        e.type = RU_EVENT_PLAYER_DISCONNECT;
      } else if (switched >= 0) {
        e.type = RU_EVENT_PLAYER_TEAM;
        e.team = switched;
        e.old_team = std::max(0, TeamFromHeader(header));
      } else if (line.find("\" entered the game") != std::string::npos) {
        e.type = RU_EVENT_PLAYER_CONNECT;
      }
      if (e.type != 0) plugins::PostEvent(std::move(e));
    }
    // `"Name<2><[U:1:x]><CT>" disconnected (reason ...)`: the player leaves the registry
    // (plugins get RU_EVENT_PLAYER_DISCONNECT and drop their own state, e.g. ready).
    if (disconnect) {
      ForgetHuman(steamid64);
      ForgetSlotIdentitiesForSteam(steamid64);
      ObserveSlotTeamFromLog(slot, 0);
      Debug("chat-hook: cleared presence for disconnected steamid64=%llu\n",
            static_cast<unsigned long long>(steamid64));
    } else {
      ObserveHuman(slot, steamid64, name, team);
    }
    Debug("chat-hook: header=\"%s\" -> name=\"%s\" slot=%d steamid64=%llu\n",
          header.c_str(),
          name.c_str(),
          slot,
          static_cast<unsigned long long>(steamid64));
  } else if (DebugEnabled()) {
    Debug("chat-hook: header ignored=\"%s\" (name=\"%s\" slot=%d steamid64=%llu)\n",
          header.c_str(),
          name.c_str(),
          slot,
          static_cast<unsigned long long>(steamid64));
  }

  // Find say/say_team and message.
  const size_t sayPos = line.find(" say \"");
  const size_t sayTeamPos = line.find(" say_team \"");
  size_t msgStart = std::string::npos;
  if (sayTeamPos != std::string::npos) msgStart = sayTeamPos + std::strlen(" say_team \"");
  else if (sayPos != std::string::npos) msgStart = sayPos + std::strlen(" say \"");
  if (msgStart == std::string::npos) return;

  const size_t msgEnd = line.find('"', msgStart);
  if (msgEnd == std::string::npos) return;
  const std::string msg = line.substr(msgStart, msgEnd - msgStart);
  Debug("chat-hook: say msg=\"%s\" (steamid64=%llu name=\"%s\")\n",
        msg.c_str(),
        static_cast<unsigned long long>(steamid64),
        name.c_str());

  // Route every dot-message; RouteChatCommand ignores the ones it doesn't know.
  // Keeping a second allowlist here let practice commands (.bot, .cbot,
  // .nobots, .tactics) silently fall through.
  auto isCmd = [&](const std::string& s) -> bool { return s.size() > 1 && s[0] == '.'; };

  if (isCmd(msg) && steamid64 == 0 && header != "Console<0><Console><Console>") {
    // No SteamID parsed from a player's header: never route it as the console (steamid64 0 may
    // run every `.ru` command); the ClientCommand hook routes that player's line with its SteamID.
    Debug("chat-hook: chat cmd without a SteamID ignored (header=\"%s\")\n", header.c_str());
  } else if (isCmd(msg)) {
    DebugLine("chat-hook: routing chat cmd via RouteChatCommand()");
    RouteChatCommand(steamid64, name, msg);
  } else if (DebugEnabled()) {
    DebugLine("chat-hook: chat ignored");
  }
}

// Vtable entry #0 for the engine logging listener.
// Called as: listener->OnLog(ctx*, msg*)
static void VOnLog(void* /*self*/, const void* /*ctx*/, const char* msg) {
  // The message pointer is a whole formatted log line.
  HandleLogLine(msg);
}

// Dummy extra vtable slots (some builds may probe additional methods).
static void VNoop(void* /*self*/) {}

static void* g_vtable[] = {
    reinterpret_cast<void*>(&VOnLog),
    reinterpret_cast<void*>(&VNoop),
    reinterpret_cast<void*>(&VNoop),
};

static Listener g_listener = {g_vtable};

static void ResolveTier0Once() {
  if (g_register) return;
  g_register = reinterpret_cast<RegisterLoggingListenerFn>(dlsym(RTLD_DEFAULT, "LoggingSystem_RegisterLoggingListener"));
  if (!g_register) {
    // Try explicit tier0 handle.
    void* tier0 = dlopen("libtier0.so", RTLD_NOW | RTLD_NOLOAD);
    if (tier0) g_register = reinterpret_cast<RegisterLoggingListenerFn>(dlsym(tier0, "LoggingSystem_RegisterLoggingListener"));
  }
}

}  // namespace

void InstallClientCommandHook() {
  if (g_installed.exchange(true)) return;

  ResolveTier0Once();
  if (!g_register) {
    PrintLine("chat-hook: LoggingSystem_RegisterLoggingListener not found; in-game `.ru` disabled.");
    return;
  }

  g_register(&g_listener);
  g_active.store(true);
  PrintLine("chat-hook: installed (in-process logging listener).");
}

bool InProcessLogListenerActive() {
  return g_active.load();
}

}  // namespace readyup


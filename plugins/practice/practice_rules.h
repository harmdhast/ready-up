#pragma once

// readyup-practice pure logic (ctest `practice_rules`): which chat commands are practice tools,
// when they may run, and when a dedicated practice server (always=1) switches practice on.

#include <string>

namespace practice {

// .rethrow .rt .savepos .loadpos .back .clear .noflash .god .spawn .ctspawn .tspawn and the ME extras
// (.last .throwidx .delay .rethrowsmoke ... .solid .impacts .traj .break .timer .bestspawn ...)
bool IsToolCommand(const std::string& cmd);
// .bot .cbot .crouchbot .boost .crouchboost .nobots
bool IsBotCommand(const std::string& cmd);

// Tools and bots run only in practice mode, and never under the valve ruleset (not in Valve's
// rulebook; docs/ESPORTS-MODE.md).
bool ToolsAllowed(bool practiceActive, const std::string& ruleset);

// A match plugin mode in which practice may not start (a match or scrim is under way).
bool MatchBlocksPractice(const std::string& ruMode);

// always=1 (dedicated practice server): switch practice on when it is off and nothing blocks it.
bool ShouldAutoEnter(bool always, bool practiceActive, const std::string& ruMode);

// "1"/"true"/"yes"/"on" -> true, "0"/"false"/"no"/"off" -> false, else def.
bool ParseBool(const std::string& text, bool def);

// What the engine says about player slot N (its controller, entity N + 1), for slot lookups
// (`ru practice as N`, the tools). The core's player registry can keep a player the engine no longer
// has (M1 play-test: a kicked bot answered `ru practice as N` although `status` no longer listed it),
// so a registry entry only counts when the engine agrees the slot holds a connected player.
enum class SlotEngineState {
  kUnknown,    // entity system / schema not ready: trust the registry (older behaviour)
  kEmpty,      // no player controller at that index
  kConnected,  // a controller with m_iConnected == PlayerConnected (0)
  kGone,       // a controller that is connecting, disconnecting or disconnected
};
// m_iConnected (PlayerConnectedState) of the slot's controller; hasController false: no controller.
// connectedKnown false: the field could not be read (schema), the controller alone counts.
SlotEngineState SlotStateFrom(bool entitySystemReady, bool hasController, bool connectedKnown, int connectedState);
// A registry entry for the slot may be used.
bool SlotEntryUsable(SlotEngineState state);

// The `.help` line while practice is on.
const char* HelpLine();

// A home map setting (practice.set {home_cvars}): "sv_<name> <number>" or "mp_<name> <number>",
// nothing else (they go straight to the console). At most kMaxHomeCvars of them.
bool ValidHomeCvar(const std::string& line);
constexpr int kMaxHomeCvars = 24;

}  // namespace practice

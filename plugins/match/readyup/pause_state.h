#pragma once

#include "readyup/webhook.h"

#include <cstdint>
#include <optional>
#include <string>

namespace readyup {

struct PauseSnapshot {
  bool paused = false;
  bool team1_ready_to_unpause = false;
  bool team2_ready_to_unpause = false;
  // Who paused and why (docs/FLEET.md §9.1 pause.type / pause.by): "tactical" | "technical" |
  // "admin" | "offline" | "halftime" | "auto_5v5" | "engine"; `by` = SteamID64, "Console", "platform:<user>"
  // or "server"; team = the pausing team (auto_5v5: the team that is short).
  std::string type;
  std::string by;
  WebhookTeam team = WebhookTeam::Unknown;
};

// Marks paused (and resets unpause readiness). type/by/team: see PauseSnapshot.
void PauseStateOnPaused(const char* type = "tactical", const std::string& by = {},
                        WebhookTeam team = WebhookTeam::Unknown);

// Marks unpaused (clears state).
void PauseStateOnUnpaused();

// Marks a team as having requested unpause. Returns current snapshot.
PauseSnapshot PauseStateRequestUnpause(WebhookTeam team);

// Returns current snapshot.
PauseSnapshot PauseStateGet();

// Returns seconds since pause started (0 if not paused).
int PauseStatePauseDurationSeconds();

// Pauses each team used on the current map (docs/FLEET.md §9.1 pause.used): type "tactical" |
// "technical" (others are not counted). Reset when a map starts (match_features.cpp).
void PauseStateCountUse(WebhookTeam team, const std::string& type);
int PauseStateUsed(WebhookTeam team, const std::string& type);
void PauseStateResetUsage();
// [team1 tactical, team1 technical, team2 tactical, team2 technical] (plugin reload).
void PauseStateUsageSave(int out[4]);
void PauseStateUsageRestore(const int in[4]);

// Plugin reload (reload_state.cpp). startTicks: the pause start as steady_clock ticks.
void PauseStateSave(PauseSnapshot* snap, long long* startTicks);
void PauseStateRestore(const PauseSnapshot& snap, long long startTicks);

}  // namespace readyup


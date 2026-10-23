#pragma once

// Roster display names, engine side (rules in roster_names_rules.h).
//
// While a match is loaded and roster names are on, each connected roster player's controller name
// (CBasePlayerController::m_iszPlayerName, char[128]) is set to their roster name and the
// controller is marked changed (ru_api entity_mark_changed) so clients get it: scoreboard, kill
// feed, chat (the engine's say line takes the controller name) and server log lines.
//
// The engine writes the player's own name back on connect, on every map change (a new controller)
// and when the client sends a new `name` (player_changename). A check about once a second (only
// the roster's players, a string compare each) puts the roster name back; player_changename, player
// connect / team and map start make the next check run at once. When the match unloads or the
// feature is turned off, a name that is still ours is set back to the player's own name (also when
// this plugin unloads).
//
// Everything Ready Up keys on players uses the SteamID64, not the name, so stats and logs keep
// working; names in log lines and reports become the roster names.

#include "readyup/plugin_api.h"

#include <string>

namespace readyup {

void RosterNamesInstall(const ru_api* api);
// Plugin unload: our names back to the players' own names.
void RosterNamesUninstall();
// Simulation: the bot named `botName` plays a roster player; it takes that player's `name`, so the
// scoreboard, GOTV and the demo show who it stands in for (the demo analysis maps it back by name).
// Game thread.
void RosterNamesNameBot(const std::string& botName, const std::string& name);

}  // namespace readyup

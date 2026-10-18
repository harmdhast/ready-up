#pragma once

// readyup-match console / RCON commands (see match_console.cpp).

#include "readyup/webhook.h"

#include <string>
#include <vector>

namespace readyup {

// The command names the plugin registers (ru_api register_console_command).
const std::vector<std::string>& MatchConsoleCommands();

// One console line whose first token is one of MatchConsoleCommands() (or `tv_delay`, observed).
// Returns true if it was handled.
bool MatchConsoleCommand(const std::string& line);

// `ru match load <url>`: fetch the match config (blocking, short timeouts) and load it.
bool LoadMatchFromUrl(const std::string& url);

// Loads a parsed match: closes a previous match's webhooks, sets the context, enters warmup,
// persists `configJson` for recovery, kicks bots, sets the CS2 round backup file
// (readyup_backup_<matchid>_map<N>_*, written once the match is live) and changes to map N =
// `firstMapNumber` (1, or the map a fleet failover resumes), also when the server is already on
// it (LoadMapEntry). Game thread.
// Used by `ru match load` and by the fleet link's match.assign (fleet_bridge.cpp).
// Loads the match. False (nothing changes, the reason printed) when the server's paid license has
// stopped (core ru_api 1.15 license_stopped): unpaid, its key replaced, or in use elsewhere.
bool ApplyLoadedMatch(const WebhookMatchContext& ctx, const std::string& configJson, int firstMapNumber = 1);

// Why a new match can't be loaded on this server (a stopped paid license), or "".
std::string LicenseStoppedReason();

// Changes to a map list entry (map_names.h): `host_workshop_map <id>` for a workshop map (and
// remembers the id so the map that loads is bound to it), else `changelevel <name>`. False (and
// logged) for an invalid entry. Game thread.
bool LoadMapEntry(const std::string& entry);

}  // namespace readyup

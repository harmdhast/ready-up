#pragma once

#include <cstdint>
#include <string>

namespace readyup {

// Name shown for a player in the ready panel: the connected name, else the name from the match
// config roster, else the name the server last saw, else "#" + the last 4 digits of the Steam ID.
std::string ReadyHudDisplayName(uint64_t steamid64, const std::string& connected, const std::string& roster,
                                const std::string& observed);

}  // namespace readyup

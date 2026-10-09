#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace readyup {

// Palette (matches the welcome screen).
constexpr const char* kCt = "#5EA8FF";
constexpr const char* kT = "#FF9D3B";
constexpr const char* kGold = "#FFD23F";
constexpr const char* kGrey = "#8A8F98";
constexpr const char* kDim = "#5B6068";
constexpr const char* kOk = "#7CD35A";
constexpr const char* kNo = "#F87171";

std::string HudFont(const char* color, const std::string& inner);

// Name shown for a player in the ready panel: the connected name, else the name from the match
// config roster, else the name the server last saw, else "#" + the last 4 digits of the Steam ID.
std::string ReadyHudDisplayName(uint64_t steamid64, const std::string& connected, const std::string& roster,
                                const std::string& observed);

// One team line of the ready panel: "CT 1/5 · waiting: a, b +2 ✔4 bots", or "CT 5/5 · ready" when the
// team is complete. `waiting` holds already escaped HTML names; `hidden` is the overflow count.
// total 0 (nobody on the side) draws "CT -".
std::string ReadyHudTeamLine(bool ct, int ready, int total, const std::vector<std::string>& waiting, int hidden,
                             int bots);

}  // namespace readyup

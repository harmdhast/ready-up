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

// HTML-escaped text, cut at maxBytes on a UTF-8 boundary (".." marks the cut); control chars dropped.
std::string HudEscape(const std::string& in, size_t maxBytes);

// Panel header "Brand · STATUS": brand gold, status bold in statusColor. `brand` is plain text (cut at
// 40 bytes); empty brand draws the status alone. Text only: panels resent every frame carry no image.
std::string HudHeader(const std::string& brand, const char* statusColor, const std::string& status);

// "3/10 READY" in the large font: gold while waiting, green when everyone is ready.
std::string ReadyHudCountLine(int ready, int total);

// Name shown for a player in the ready panel: the connected name, else the name from the match
// config roster, else the name the server last saw, else "#" + the last 4 digits of the Steam ID.
std::string ReadyHudDisplayName(uint64_t steamid64, const std::string& connected, const std::string& roster,
                                const std::string& observed);

// One team line of the ready panel: "Navi 1/5 · waiting: a, b +2 ✔4 bots", or "Navi 5/5 · ready" when
// the team is complete. `label` (team name or CT / T) and `waiting` are already escaped HTML; the label
// takes the side color, the count is gold until the team is full. `hidden` is the overflow count.
// total 0 (nobody on the side) draws "CT -".
std::string ReadyHudTeamLine(bool ct, const std::string& label, int ready, int total, const std::vector<std::string>& waiting, int hidden,
                             int bots);

// The viewer's own line: Absent (not on the roster or a side), NotReady, Ready.
enum class ReadyViewer { Absent, NotReady, Ready };
std::string ReadyHudViewerLine(ReadyViewer v, bool scrim);

// A team line label: the escaped team name (16 bytes), or CT / T when the name is empty.
std::string ReadyHudTeamLabel(bool ct, const std::string& teamName);

}  // namespace readyup

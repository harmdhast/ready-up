#pragma once

// Roster display names, pure logic (roster_names.h is the engine side; ctest `match_roster_names`).
//
// While a match is loaded, a roster player's in-game name (CBasePlayerController::m_iszPlayerName:
// scoreboard, kill feed, chat) is the name the platform gave them in the match config
// (team1/team2.players {steamid64: name}, fleet match.assign players[].name), not their Steam name.
// On by default when the match has names; the match rule `roster_names` (fleet rules.roster_names)
// and the server setting `roster_names_enabled` turn it off.

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <unordered_map>

namespace readyup::roster_names {

// m_iszPlayerName is char[128]: at most 127 bytes and the NUL.
constexpr size_t kMaxNameBytes = 127;

// A name safe to write into the controller: valid UTF-8 only (bad bytes dropped), no control bytes
// (CS2 chat color bytes 0x01..0x10 included) or DEL, none of `"` `<` `>` (server log lines quote
// players as "Name<slot><[U:1:x]><TEAM>"; the log parsers must keep working), runs of blanks
// collapsed to one space, trimmed, at most `maxBytes` bytes cut on a character boundary.
// "" when nothing is left.
std::string Sanitize(const std::string& in, size_t maxBytes = kMaxNameBytes);

// Whether roster names apply to the loaded match: the match rule when set (-1 = unset, 0 / 1),
// else the server setting, and only when the match names someone.
bool Enabled(int matchRule, bool serverSetting, bool matchHasNames);

// The name `steamid64` should have in game: its roster name, sanitized. nullopt when the player has
// none (or it sanitizes to nothing): their own name stays.
std::optional<std::string> DesiredName(uint64_t steamid64, const std::unordered_map<uint64_t, std::string>& names);

// What to do with one player's controller name on a check.
enum class Action { None, Write, Restore };

// Per-player memory between checks.
struct PlayerState {
  std::string written;   // the name this plugin last wrote ("" = none)
  std::string original;  // their own (Steam) name, seen before the first write
};

// One check of a rostered or previously renamed player. `current` is the controller's name now,
// `desired` the roster name (nullopt: the feature is off, or no match / no name). Updates `st`:
//   - desired and current differ -> Write; a current name that is not ours is their own name
//     (connect, map change, a Steam name change or the engine resetting it) and is remembered.
//   - no desired name, our name still on the controller -> Restore (st.original); a name that is no
//     longer ours is left alone.
// `*out` is the name to write for Write / Restore.
Action Decide(const std::string& current, const std::optional<std::string>& desired, PlayerState* st,
              std::string* out);

}  // namespace readyup::roster_names

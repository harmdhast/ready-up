#pragma once
// Team lock (modes.cpp MaybeForceRosterTeamsLocked): whether a roster player has to be moved to
// their team's side. Engine-free, ctest `match_team_lock`.
//
// The core's cached team (ListHumans().team) comes from log lines and player_team events. A map
// load resets it to 0 while the players stay on their side, and moving a player to the team they
// are already on is a no-op in CS2 (CCSPlayerController::ChangeTeam returns early, no event). So
// the cache can say "no team" for a whole map while the player plays CT, and the lock moved them
// again every 900 ms (2026-10-17, a live map: 260 "moved BBro to CT" lines in 2 min). The
// engine's team (m_iTeamNum, read this frame) wins whenever it is known.
#include <cstdint>
#include <optional>
#include <unordered_map>

namespace readyup {

// cachedTeam / target / *engineTeam: 0 none, 1 spectators, 2 T, 3 CT.
inline bool TeamMoveNeeded(int cachedTeam, std::optional<int> engineTeam, int target) {
  const int current = engineTeam ? *engineTeam : cachedTeam;
  return current != target;
}

// Backstop for when the engine cannot be read (no controller, schema not ready): at most
// `maxTries` moves of one player to one target until their cached team changes (a team event or
// log line arrived) or the target does. Then a move that does not take can never loop.
class MoveRetryCap {
 public:
  explicit MoveRetryCap(int maxTries = 3) : max_(maxTries) {}
  // True when one more move of `steamid64` to `target` may be tried; it is counted.
  bool Allow(uint64_t steamid64, int target, int cachedTeam) {
    Entry& e = tries_[steamid64];
    if (e.target != target || e.cachedTeam != cachedTeam) e = Entry{target, cachedTeam, 0};
    if (e.tries >= max_) return false;
    ++e.tries;
    return true;
  }
  void Forget(uint64_t steamid64) { tries_.erase(steamid64); }
  void Clear() { tries_.clear(); }

 private:
  struct Entry {
    int target = -1, cachedTeam = -1, tries = 0;
  };
  int max_;
  std::unordered_map<uint64_t, Entry> tries_;
};

}  // namespace readyup

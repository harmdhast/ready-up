#pragma once

// Warmup ammo: `ru_warmup_infinite_ammo 1` (default) in scrim and match warmup (not the recovery
// gate of a restored match, which pauses a live round). sv_infinite_ammo is a cheat cvar, and CS2
// resets every cheat cvar when sv_cheats goes back to 0, so the plugin keeps the reserve itself:
// each tick, every weapon a player carries (pawn m_pWeaponServices -> m_hMyWeapons) gets its
// reserve (CBasePlayerWeapon m_pReserveAmmo) back to the most it had since it was picked up.
// Magazines still need a reload, as with `sv_infinite_ammo 2`. Log lines: `warmup-ammo: ...`.

#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace readyup {

// Pure (ctest `match_rules`): keep reserves in this ru mode string with this setting?
inline bool WarmupAmmoActive(const char* ruMode, bool enabled, bool recoveryGate) {
  if (!enabled || !ruMode) return false;
  const std::string m = ruMode;
  return m == "scrim_warmup" || (m == "match_warmup" && !recoveryGate);
}

// Pure (ctest `match_rules`): the most reserve each carried weapon (by entity handle) has had.
// One pass: Keep() for every carried weapon, then EndPass() forgets the weapons not seen.
class ReserveLedger {
 public:
  // The reserve `handle` should have now (>= `reserve`).
  int32_t Keep(uint32_t handle, int32_t reserve) {
    int32_t most = reserve;
    for (const auto& e : prev_) {
      if (e.first == handle) {
        if (e.second > most) most = e.second;
        break;
      }
    }
    cur_.emplace_back(handle, most);
    return most;
  }
  void EndPass() {
    prev_.swap(cur_);
    cur_.clear();
  }
  void Clear() {
    prev_.clear();
    cur_.clear();
  }

 private:
  std::vector<std::pair<uint32_t, int32_t>> prev_, cur_;
};

// on_tick.
void WarmupAmmoTick();

}  // namespace readyup

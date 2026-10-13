#pragma once

// Warmup score: no round counts in Ready Up's (emulated) warmup. CS2 credits a round win before it
// calls CCSGameRules::TerminateRound, and in match warmup the core's detour drops that call
// (set_round_termination_suppressed), so a win condition that holds (the round timer at 0:00, a
// team eliminated) credits a round on every tick and the round never ends. The warmup rules keep
// that from happening (mp_ignore_round_win_conditions 1, 60-minute rounds) until they are lost,
// e.g. when a map reload runs gamemode_competitive.cfg again. In scrim and match warmup (not the
// recovery gate of a restored match, no go-live pending), a team score above 0 (CTeam m_iScore)
// sends the warmup rules again and `mp_restartgame 1` (scrim warmup rules restart the game
// themselves): scores and round count back to 0, everyone respawns, warmup money stays
// (warmup_money.h). Log line: `warmup-score: ...`.
// A failover resume warmup (fleet_bridge.h ResumeWarmupScore) shows the resumed round's score
// instead: each tick the team entities' m_iScore are written to it (the backup load at go-live
// restores the real one) and nothing is reset.

#include <string>

struct ru_api;  // readyup/plugin_api.h

namespace readyup {

// Pure (ctest `match_rules`): reset the score in this ru mode string? Not before the warmup's map
// has loaded (mapReady): a match load or the next map of a series switches to match warmup before
// its map change, while the last map's score is still up. Not in a resume warmup (resumePreset).
inline bool WarmupScoreResetActive(const char* ruMode, bool recoveryGate, bool goLivePending, bool mapReady,
                                   bool resumePreset = false) {
  if (!ruMode || goLivePending || !mapReady || resumePreset) return false;
  const std::string m = ruMode;
  return m == "scrim_warmup" || (m == "match_warmup" && !recoveryGate);
}

// Pure (ctest `match_rules`): the CT and T team scores for team1 / team2's resumed map score.
inline void WarmupPresetScores(int team1, int team2, bool team1Ct, int* ct, int* t) {
  *ct = team1Ct ? team1 : team2;
  *t = team1Ct ? team2 : team1;
}

// readyup_plugin_load.
void WarmupScoreInstall(const ru_api* api);
// on_tick.
void WarmupScoreTick(double now);

}  // namespace readyup

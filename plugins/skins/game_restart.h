#pragma once
// Whether CS2 has a round restart pending (apply.cpp GameRestartPending), engine-free: ctest
// `skins_game_restart`.
//
// Read from the round times, not CCSGameRules::m_bGameRestart: the match plugin's hud_steady sets
// that flag every tick so center panels do not blink (true once the restart is behind us), which
// read as "restart pending" here held every knife swap and glove pass until the knife had been
// networked, and the client kept the stock knife (2026-10-23).
namespace skins {

// restartRoundTime: CCSGameRules::m_flRestartRoundTime; roundStartTime: m_fRoundStartTime.
inline bool RestartPending(float restartRoundTime, float roundStartTime) {
  return restartRoundTime > roundStartTime;
}

}  // namespace skins

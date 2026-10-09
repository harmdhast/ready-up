#pragma once

namespace readyup {

// CS2 replays the center panel's fade-in on every show_survival_respawn_status unless the game
// rules say a restart happened (workaround by Poggu, girlglock/CS2FlashingHtmlHudFix:
// m_bGameRestart = m_flRestartRoundTime < curtime). Plugins see no curtime, so "the restart is
// behind us" is read as: no restart scheduled, or it came before the current round started.
inline bool HudSteadyGameRestart(float restartRoundTime, float roundStartTime) {
  return restartRoundTime <= roundStartTime;
}

}  // namespace readyup

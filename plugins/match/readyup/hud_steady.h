#pragma once

// Keeps the center panel from blinking: every tick, CCSGameRules::m_bGameRestart is set from
// HudSteadyGameRestart (hud_steady_logic.h). readyup.cfg `hud_steady=0` turns it off.

namespace readyup {

void HudSteadyTick();
void HudSteadyOnMapStart();

}  // namespace readyup

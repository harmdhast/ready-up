#pragma once

// Round economy: each player's money when a round starts, and at the end of the freeze time what
// they bought (money left, armour, helmet, defuse kit, the weapons and grenades they carry). It
// goes into the round's player rows (match_stats.h PlayerRound), so the platform's admin match
// page can show what everyone had in a round before restoring it. Read from the entities through
// schema offsets (never hard-coded); a field the schema does not know is left unread (-1).

namespace readyup {

// Game thread: round_start (money at the start) and round_freeze_end (the buys).
void RoundEconomyOnRoundStart();
void RoundEconomyOnFreezeEnd();

}  // namespace readyup

// Offline tests for plugins/skins/knife_swap.h: only a knife the plugin swapped in goes back to
// stock; the player's own Steam inventory knife is never touched. ctest `skins_knife_swap`.
#include "knife_swap.h"

#include <cstdio>

using namespace skins;

static int g_failures = 0;

#define CHECK(cond)                                                                 \
  do {                                                                              \
    if (!(cond)) {                                                                  \
      std::fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #cond); \
      ++g_failures;                                                                 \
    }                                                                               \
  } while (0)

int main() {
  constexpr int karambit = 507, butterfly = 515;

  // A platform knife equipped: swap to it (whatever the game gave), keep it once it is in hand.
  CHECK(DecideKnife(karambit, kStockKnifeCtDef, false) == KnifeAction::Swap);
  CHECK(DecideKnife(karambit, butterfly, false) == KnifeAction::Swap);  // platform wins over Steam
  CHECK(DecideKnife(karambit, karambit, true) == KnifeAction::Keep);

  // No platform knife (none equipped, or no platform skins at all): the game's knife stays,
  // including the player's own Steam inventory knife (2026-10-17).
  CHECK(DecideKnife(std::nullopt, butterfly, false) == KnifeAction::Keep);
  CHECK(DecideKnife(std::nullopt, kStockKnifeTDef, false) == KnifeAction::Keep);
  // ... but a knife we swapped in goes back to stock when it is unequipped (#241).
  CHECK(DecideKnife(std::nullopt, karambit, true) == KnifeAction::RevertStock);
  CHECK(DecideKnife(std::nullopt, kStockKnifeCtDef, true) == KnifeAction::Keep);

  // The ledger: per entity handle and player; a reused handle is not ours.
  KnifeSwapLedger l;
  CHECK(!l.Swapped(0x1234, 1));
  l.MarkSwapped(0x1234, 1);
  CHECK(l.Swapped(0x1234, 1));
  CHECK(!l.Swapped(0x1234, 2));
  CHECK(!l.Swapped(0x9999, 1));
  l.Forget(0x1234);
  CHECK(!l.Swapped(0x1234, 1));
  l.MarkSwapped(1, 1);
  l.MarkSwapped(2, 1);
  CHECK(l.Size() == 2);
  l.Clear();
  CHECK(l.Size() == 0);

  // Paint stamps: the same look on the same item is not written again; a new item ID (the game
  // reset it: new spawn, new weapon) or another look is.
  PaintStamp none;
  CHECK(!StampMatches(none, 0, 0, 0, 0, 0.0f));
  const PaintStamp st{0x10001, 5030, 10018, 7, 0.15f};
  CHECK(StampMatches(st, 0x10001, 5030, 10018, 7, 0.15f));
  CHECK(!StampMatches(st, 0x99999, 5030, 10018, 7, 0.15f));
  CHECK(!StampMatches(st, 0x10001, 5031, 10018, 7, 0.15f));
  CHECK(!StampMatches(st, 0x10001, 5030, 10019, 7, 0.15f));
  CHECK(!StampMatches(st, 0x10001, 5030, 10018, 8, 0.15f));
  CHECK(!StampMatches(st, 0x10001, 5030, 10018, 7, 0.2f));

  if (g_failures == 0) std::printf("skins_knife_swap: ok\n");
  return g_failures == 0 ? 0 : 1;
}

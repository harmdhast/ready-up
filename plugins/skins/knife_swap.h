#pragma once
// Knife swaps (apply.cpp ProcessWeapon), engine-free: ctest `skins_knife_swap`.
//
// A knife the plugin swapped in (ChangeSubclass to the platform knife) goes back to the stock knife
// once the player has no platform knife equipped any more (#241: unequipping left the old knife in
// hand). A knife the game handed out (the stock one, or the player's own Steam inventory knife) is
// never touched: reverting every non-stock knife took players their Steam knives (2026-10-17).
#include <cstdint>
#include <optional>
#include <unordered_map>

namespace skins {

constexpr int kStockKnifeCtDef = 42;
constexpr int kStockKnifeTDef = 59;

enum class KnifeAction {
  Keep,         // leave the knife as it is
  Swap,         // ChangeSubclass to wantDef
  RevertStock,  // ChangeSubclass back to the side's stock knife (only a knife we swapped)
};

// wantDef: the platform knife equipped for this side (nullopt = none). heldDef: the knife entity's
// defindex now. pluginSwapped: this entity got its knife from us (KnifeSwapLedger).
inline KnifeAction DecideKnife(std::optional<int> wantDef, int heldDef, bool pluginSwapped) {
  if (wantDef) return *wantDef != heldDef ? KnifeAction::Swap : KnifeAction::Keep;
  if (!pluginSwapped) return KnifeAction::Keep;
  if (heldDef == kStockKnifeCtDef || heldDef == kStockKnifeTDef) return KnifeAction::Keep;
  return KnifeAction::RevertStock;
}

// Knife entities (by entity handle) whose knife the plugin swapped in, and for whom. Cleared on a
// map change (handles start over), an entry dropped when the knife reverts or the entity is gone.
class KnifeSwapLedger {
 public:
  void MarkSwapped(uint32_t handle, uint64_t steamid64) { owner_[handle] = steamid64; }
  // Swapped by us for this player (a handle reused by someone else's knife does not count).
  bool Swapped(uint32_t handle, uint64_t steamid64) const {
    const auto it = owner_.find(handle);
    return it != owner_.end() && it->second == steamid64;
  }
  void Forget(uint32_t handle) { owner_.erase(handle); }
  void Clear() { owner_.clear(); }
  size_t Size() const { return owner_.size(); }

 private:
  std::unordered_map<uint32_t, uint64_t> owner_;
};

// What the plugin last wrote onto one econ item (a weapon by entity handle, a pawn's gloves by
// player): the fake item ID and the look. The same look again on the same item is skipped: each
// write hands the client a new item ID, and for gloves a bumped m_nEconGlovesChanged, so it
// rebuilds the model. Gloves were rebuilt on 4 passes of every spawn, and knives repainted on every
// loadout refresh (a crash lead, 2026-10-17). The game resetting the item (a new spawn, a new
// weapon) changes its item ID, and then it is written again.
struct PaintStamp {
  uint64_t itemId = 0;  // 0 = nothing written
  int defindex = 0, paint = 0, seed = 0;
  float wear = 0.0f;
};
inline bool StampMatches(const PaintStamp& last, uint64_t currentItemId, int defindex, int paint, int seed, float wear) {
  return last.itemId != 0 && last.itemId == currentItemId && last.defindex == defindex && last.paint == paint &&
         last.seed == seed && last.wear == wear;
}

}  // namespace skins

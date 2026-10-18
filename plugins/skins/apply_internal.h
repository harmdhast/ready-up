#pragma once

#include "skins.h"

#include <cstdint>

// Shared between apply.cpp and cosmetics.cpp. Game thread only.
namespace skins {

// Thin wrappers over ru_api (entity + econ members). Each returns 0/NULL when the core could
// not resolve the engine function (e.g. the skins gamedata fragment is missing).
inline void* EntityByIndex(int i) { return g_api->entity_by_index(g_api->self, i); }
inline void* EntityFromHandle(uint32_t h) { return g_api->entity_from_handle(g_api->self, h); }
inline const char* EntityDesignerName(void* e) { return g_api->entity_classname(g_api->self, e); }
inline bool MarkEntityFullyChanged(void* e) { return g_api->entity_mark_changed(g_api->self, e) == 1; }
inline bool AttrSetOrAddByName(void* list, const char* name, float v) {
  return g_api->econ_attr_set_by_name(g_api->self, list, name, v) == 1;
}
inline bool ChangeSubclass(void* e, const char* sub) { return g_api->entity_change_subclass(g_api->self, e, sub) == 1; }
inline bool SetModel(void* e, const char* model) { return g_api->entity_set_model(g_api->self, e, model) == 1; }
inline int SetBodygroupByName(void* e, const char* group, int v) {
  return g_api->entity_set_bodygroup_by_name(g_api->self, e, group, v);
}
inline const char* BodygroupResultName(int r) {
  switch (r) {
    case RU_BODYGROUP_OK: return "ok";
    case RU_BODYGROUP_UNAVAILABLE: return "functions unresolved";
    case RU_BODYGROUP_NO_MODEL: return "entity has no loaded model";
    case RU_BODYGROUP_NO_GROUP: return "model has no such bodygroup";
    default: return "?";
  }
}

namespace detail {

// Resolves the schema offsets the skins feature needs (idempotent). False if any are missing.
bool EconOffsetsReady();
int ItemDefIndexOffset();     // CEconItemView::m_iItemDefinitionIndex
int ItemInitializedOffset();  // CEconItemView::m_bInitialized (may be -1)

// Writes paint/seed/wear/StatTrak/nametag + a fake item ID onto a CEconItemView (and the fallback
// fields of `ownerEconEntity` if non-null).
// Returns the item ID written.
uint64_t WritePaint(void* ownerEconEntity, void* itemView, uint64_t steamid64, const WeaponSkinEntry& skin);
// CEconItemView::m_iItemID now.
uint64_t ItemIdOf(void* itemView);

void* EconItemViewOfWeapon(void* weapon);

// Clears resolved offsets (a reload re-resolves; offsets never change within a process).
void ResetOffsets();

}  // namespace detail
}  // namespace skins

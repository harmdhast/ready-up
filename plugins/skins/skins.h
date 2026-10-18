#pragma once

// readyup-skins internals. The plugin talks to the core only through ru_api
// (core/include/readyup/plugin_api.h); nothing here includes a core header.

#include "readyup/plugin_api.h"

#include <cstdint>
#include <optional>
#include <string>
#include <unordered_map>

namespace skins {

// ---- host (skins_plugin.cpp) --------------------------------------------------------------
extern const ru_api* g_api;
void Log(int level, const char* fmt, ...) RU_PRINTF(2, 3);
bool DebugOn();  // readyup.cfg debug=1 (any thread)
// Whether nothing is being played (Ready Up's idle, practice or a match / scrim warmup, or no
// match plugin): a new loadout may swap the held weapons and gloves at once. Game thread.
bool WarmupLike();
// Ruleset "valve" (docs/ESPORTS-MODE.md, Valve rulebook: inventories "should not be modified or
// augmented"): the plugin applies and restores nothing. Asked from the match plugin
// (readyup.match.v1 inventory_locked), else readyup.cfg `ruleset=`; refreshed once a second on the
// game thread. Inert() any thread; InertReason() "" when active.
bool Inert();
std::string InertReason();
#define SKINS_DEBUG(...)                                             \
  do {                                                               \
    if (::skins::DebugOn()) ::skins::Log(RU_LOG_DEBUG, __VA_ARGS__); \
  } while (0)
// schema_offset("server" scope), -1 if unknown.
int SchemaOffset(const char* cls, const char* field);

// ---- loadout rows (loadouts.json tables: docs/json-contract.md; fleet: skins.loadout) -----
struct WeaponSkinEntry {
  int weapon_team = 0;  // 2=T, 3=CT, 0=both
  int weapon_defindex = 0;
  int paint_id = 0;
  float wear = 0.0f;
  int seed = 0;
  std::string nametag;
  bool stattrak_enabled = false;
  int stattrak_count = 0;
};
struct WeaponKnifeEntry {
  int weapon_team = 0;
  std::string knife_classname;
};
struct WeaponGloveEntry {
  int weapon_team = 0;
  int glove_defindex = 0;
};
struct WeaponAgentEntry {
  std::string agent_ct;
  std::string agent_t;
};

// ---- loadout cache (loadout.cpp) ---------------------------------------------------------
// File I/O runs on one worker thread owned by the plugin (started in load, joined in unload);
// lookups never block the game thread.
struct TeamLoadout {
  std::unordered_map<int, WeaponSkinEntry> skins_by_defindex;
  std::optional<std::string> knife_classname;
  std::optional<int> glove_defindex;
};
struct Loadout {
  std::unordered_map<int, TeamLoadout> by_team;  // 2=T, 3=CT, 0=both/default
  std::optional<WeaponAgentEntry> agents;
};

// Starts the worker (loadouts.json / stattrak.json in the plugin data dir). false: no data dir.
bool LoadoutStart();
void LoadoutStop();
// on_tick: fleet mode on/off, skins.loadout / skins.invalidate handlers, skins.stattrak flush.
void FleetTick(double now);
std::string LoadoutStatus();

void MaybeRefreshAsync(uint64_t steamid64);
void Invalidate(uint64_t steamid64);
bool IsLoaded(uint64_t steamid64);
std::optional<WeaponSkinEntry> FindWeaponSkin(uint64_t steamid64, int weapon_team, int weapon_defindex);
std::optional<std::string> FindKnifeClassname(uint64_t steamid64, int weapon_team);
std::optional<int> FindGloveDefindex(uint64_t steamid64, int weapon_team);
std::optional<WeaponAgentEntry> FindAgents(uint64_t steamid64);
std::optional<int> KnifeClassnameToDefindex(const std::string& classname);
// StatTrak +1 for the row that matched (team 0 rows included): stattrak.json standalone,
// skins.stattrak to the platform in fleet mode.
void IncrementStatTrakAsync(uint64_t steamid64, int weapon_team, int weapon_defindex);

// ---- apply (apply.cpp, cosmetics.cpp) ----------------------------------------------------
// Per simulating tick: walks connected players, paints newly seen weapons / knives and applies
// gloves + agents on spawn. Everything the engine does for skins happens here.
void GameFrameTick();
std::string ApplyStatus();
// Dev only: treat bot `slot` as `steamid64` (0 clears). Needs readyup.cfg debug=1.
bool SetDebugAs(int slot, uint64_t steamid64);
bool ApplySpawnCosmetics(void* pawn, uint64_t steamid64, int team, bool late);
// player_spawn (game thread): gloves + agent again for that slot on its next alive tick, also
// when the pawn stayed alive (mp_restartgame, round start).
void RequestSpawnCosmetics(int slot);
// readyup.skins.v1 (skins_iface.h), game thread. ExternalPaintReady: not inert, entity system and
// schema fields ready. PaintWeaponExternal: paint_weapon (paintKit 0 hands the weapon back).
bool ExternalPaintReady();
bool PaintWeaponExternal(uint32_t handle, uint64_t steamid64, int paintKit, float wear, int seed);
// set_player_paint: new weapons of `steamid64` get this paint at creation (paintKit 0 clears).
bool SetPlayerPaintExternal(uint64_t steamid64, int paintKit, float wear, int seed);
// refresh_weapons: swap the held weapons of `slot` for new ones (player_give_item).
bool RefreshWeaponsExternal(int slot);
// set_player_name_tag: the name tag that goes with set_player_paint (empty clears).
bool SetPlayerNameTagExternal(uint64_t steamid64, const char* nameTag);
// `.skins reload`: about a second from now, gloves + agent again and the held weapons repainted
// with the (re-read) loadout. Game thread.
void RequestReapply(int slot);
// A round restart happened (round_prestart / round_start): re-gives wait a moment.
void NoteRoundChange();
// A new loadout came from the platform for this player (any thread). In warmup their held
// weapons are swapped for ones with the new skins right away (like `.skins reload`); in a live
// round nothing changes until the next spawn, so a fight never loses its weapon mid-round.
void QueueWarmupRegive(uint64_t steamid64);
// A player's first loadout arrived: repaint what they hold (they may have spawned before it).
void QueueFirstLoadoutRepaint(uint64_t steamid64);

// ---- StatTrak (stattrak.cpp) -------------------------------------------------------------
void OnPlayerDeath(const ru_game_event* ev);

}  // namespace skins

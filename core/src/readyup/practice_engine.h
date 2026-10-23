#pragma once

// ru_api 1.13: player_change_team, player_teleport, entity_create, entity_spawn and
// engine_feature_available; 1.17: player_respawn.
//
// Like grenade_spawn (grenades.h), the engine functions come from the practice gamedata fragment
// (gamedata/engine-surface.practice.json), which ships only with the practice plugin:
//   change_team    CCSPlayerController::ChangeTeam   (vtable slot checked on every controller)
//   teleport       CCSPlayerPawn::Teleport           (vtable slot checked on every pawn)
//   entity_create  UTIL_CreateEntityByName + CBaseEntity::DispatchSpawn
// respawn comes from the core gamedata (gamedata/engine-surface.json), so it needs no fragment:
//   respawn        CBasePlayerController::SetPawn + CCSPlayerController::Respawn (vtable slot,
//                  anchors only)
// Each function must match exactly once and pass its anchors (a vtable entry without a function,
// its own anchors); the virtuals must also sit in their vtable slot and the object's vptr must be
// that vtable. Otherwise the feature is unavailable and nothing is called. Game thread only. The pure request checks are in practice_engine_spec.h.

#include <string>

namespace readyup::practice_engine {

// True if `feature` ("change_team", "teleport", "entity_create", "respawn") verified on this build.
bool Available(const char* feature);

// The player in `slot` joins `team` (1 = spectators, 2 = T, 3 = CT), as jointeam does.
bool ChangeTeam(int slot, int team, std::string* why);
// The controller's team now (m_iTeamNum: 0 none, 1 spectators, 2 T, 3 CT), or -1 when `slot` is
// not a player or the schema is not ready.
int TeamOf(int slot);

// The live pawn of `slot`: new origin / view angles (pitch, yaw, roll) / velocity; a null part is
// left alone.
bool Teleport(int slot, const float* origin, const float* angles, const float* velocity, std::string* why);

// The dead player in `slot` (on T or CT) respawns on their player pawn: 1 respawned, 2 already
// alive (nothing called), 0 refused (*why).
int Respawn(int slot, std::string* why);

// A new entity of `classname`, not spawned yet (write its fields, then Spawn it).
void* Create(const char* classname, std::string* why);
// DispatchSpawn for an entity made by Create (its handle must round-trip).
bool Spawn(void* entity, std::string* why);

}  // namespace readyup::practice_engine

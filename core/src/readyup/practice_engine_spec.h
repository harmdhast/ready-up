#pragma once

// Pure rules of ru_api 1.13 (player_change_team, player_teleport, entity_create, entity_spawn,
// engine_feature_available) and 1.17 (player_respawn), kept free of engine code so they are unit tested
// (tests/practice_engine_spec_test.cpp): which engine-surface entries each feature needs and which
// requests are refused before any engine call.

#include <cmath>
#include <cstring>

namespace readyup::practice_engine {

struct FeatureInfo {
  const char* name;      // engine_feature_available(name)
  const char* function;  // "functions" key of the verified function behind it, or nullptr (vtable slot only)
  const char* function2; // a second function it needs, or nullptr
  const char* vtable;    // "vtable_indices" key checked before each call, or nullptr
};

// Nullptr for an unknown feature name.
inline const FeatureInfo* Feature(const char* name) {
  static const FeatureInfo k[] = {
      {"change_team", "CCSPlayerController_ChangeTeam", nullptr, "CCSPlayerController::ChangeTeam"},
      {"teleport", "CCSPlayerPawn_Teleport", nullptr, "CCSPlayerPawn::Teleport"},
      {"entity_create", "UTIL_CreateEntityByName", "CBaseEntity_DispatchSpawn", nullptr},
      {"respawn", nullptr, "CBasePlayerController_SetPawn", "CCSPlayerController::Respawn"},
  };
  if (!name) return nullptr;
  for (const auto& f : k) {
    if (std::strcmp(f.name, name) == 0) return &f;
  }
  return nullptr;
}

// player_change_team: 1 = spectators, 2 = T, 3 = CT.
inline bool ValidTeam(int team) { return team >= 1 && team <= 3; }

// player_respawn: a dead player on T (2) or CT (3). Why it is refused, or nullptr.
inline const char* ValidateRespawn(int team, bool alive) {
  if (team != 2 && team != 3) return "the player is not on T or CT";
  if (alive) return "the player is alive";
  return nullptr;
}

// player_teleport. Each part may be null (left alone), not all three. Why it is refused, or nullptr.
constexpr float kMaxCoord = 32768.f;  // off any map beyond this
constexpr float kMaxSpeed = 10000.f;  // sv_maxvelocity is 3500

inline bool Finite3(const float v[3], float limit) {
  for (int i = 0; i < 3; ++i) {
    if (!std::isfinite(v[i]) || v[i] <= -limit || v[i] >= limit) return false;
  }
  return true;
}

inline const char* ValidateTeleport(const float origin[3], const float angles[3], const float velocity[3]) {
  if (!origin && !angles && !velocity) return "nothing to change (origin, angles and velocity are all null)";
  if (origin && !Finite3(origin, kMaxCoord)) return "origin is not a finite map position";
  if (angles) {
    if (!Finite3(angles, 360.f * 16)) return "angles are not finite";
    if (angles[0] < -90.f || angles[0] > 90.f) return "pitch is outside -90..90";
  }
  if (velocity && !Finite3(velocity, kMaxSpeed)) return "velocity is not finite or too large";
  return nullptr;
}

// entity_create: a designer name, [a-z0-9_] (1..63). Player entities are refused (a controller or
// pawn made outside the engine's own connect path would not belong to anyone).
inline const char* ValidateClassname(const char* cls) {
  if (!cls || !*cls) return "empty classname";
  const size_t n = std::strlen(cls);
  if (n > 63) return "classname longer than 63 characters";
  for (size_t i = 0; i < n; ++i) {
    const char c = cls[i];
    if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_')) return "classname is not [a-z0-9_]";
  }
  if (std::strcmp(cls, "player") == 0 || std::strcmp(cls, "cs_player_controller") == 0 ||
      std::strcmp(cls, "worldent") == 0 || std::strncmp(cls, "cs_gamerules", 12) == 0 ||
      std::strcmp(cls, "cs_team_manager") == 0) {
    return "that classname is not allowed";
  }
  return nullptr;
}

}  // namespace readyup::practice_engine

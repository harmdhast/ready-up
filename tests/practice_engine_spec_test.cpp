// ru_api 1.13 / 1.14 rules (core/src/readyup/practice_engine_spec.h): the feature table and the checks
// that refuse a request before any engine call. The engine functions themselves are checked
// offline by readyup_sigcheck and on a server by `ru selftest` / compat-report.
#include "readyup/practice_engine_spec.h"
#include "readyup/plugin_api.h"

#include <cmath>
#include <cstddef>
#include <cstdio>
#include <cstring>
#include <limits>
#include <string>

namespace pe = readyup::practice_engine;

static int g_failed = 0;
static void Check(bool ok, const char* what) {
  std::printf("%s %s\n", ok ? "PASS" : "FAIL", what);
  if (!ok) ++g_failed;
}

int main() {
  // Features and the engine-surface entries (gamedata/engine-surface.practice.json) they need.
  const pe::FeatureInfo* team = pe::Feature("change_team");
  Check(team && std::strcmp(team->function, "CCSPlayerController_ChangeTeam") == 0 &&
            std::strcmp(team->vtable, "CCSPlayerController::ChangeTeam") == 0,
        "change_team = CCSPlayerController::ChangeTeam, slot checked");
  const pe::FeatureInfo* tp = pe::Feature("teleport");
  Check(tp && std::strcmp(tp->function, "CCSPlayerPawn_Teleport") == 0 &&
            std::strcmp(tp->vtable, "CCSPlayerPawn::Teleport") == 0,
        "teleport = CCSPlayerPawn::Teleport, slot checked");
  const pe::FeatureInfo* ec = pe::Feature("entity_create");
  Check(ec && std::strcmp(ec->function, "UTIL_CreateEntityByName") == 0 && ec->function2 &&
            std::strcmp(ec->function2, "CBaseEntity_DispatchSpawn") == 0 && !ec->vtable,
        "entity_create = UTIL_CreateEntityByName + DispatchSpawn");
  const pe::FeatureInfo* rs = pe::Feature("respawn");
  Check(rs && !rs->function && rs->function2 && std::strcmp(rs->function2, "CBasePlayerController_SetPawn") == 0 &&
            std::strcmp(rs->vtable, "CCSPlayerController::Respawn") == 0,
        "respawn = SetPawn + the CCSPlayerController::Respawn slot (anchors only)");
  Check(!pe::Feature("") && !pe::Feature(nullptr) && !pe::Feature("grenade"), "unknown features have no entry");

  // Teams: the public RU_TEAM_* values.
  Check(pe::ValidTeam(RU_TEAM_SPECTATOR) && pe::ValidTeam(RU_TEAM_T) && pe::ValidTeam(RU_TEAM_CT), "teams 1..3");
  Check(!pe::ValidTeam(RU_TEAM_UNASSIGNED) && !pe::ValidTeam(4) && !pe::ValidTeam(-1), "other teams refused");

  // Respawn requests: a dead player on T or CT.
  Check(!pe::ValidateRespawn(RU_TEAM_T, false) && !pe::ValidateRespawn(RU_TEAM_CT, false), "a dead T / CT respawns");
  Check(pe::ValidateRespawn(RU_TEAM_T, true) != nullptr, "an alive player is refused");
  Check(pe::ValidateRespawn(RU_TEAM_SPECTATOR, false) && pe::ValidateRespawn(RU_TEAM_UNASSIGNED, false),
        "spectators / unassigned refused");

  // Teleport requests.
  const float pos[3] = {100.f, -2000.f, 64.f}, ang[3] = {-12.5f, 179.f, 0.f}, vel[3] = {0.f, 0.f, 0.f};
  Check(!pe::ValidateTeleport(pos, ang, vel), "origin + angles + velocity");
  Check(!pe::ValidateTeleport(nullptr, ang, nullptr), "angles only (the view)");
  Check(!pe::ValidateTeleport(pos, nullptr, nullptr), "origin only");
  Check(pe::ValidateTeleport(nullptr, nullptr, nullptr) != nullptr, "nothing to do is refused");
  const float nan = std::numeric_limits<float>::quiet_NaN();
  const float far[3] = {40000.f, 0.f, 0.f}, bad[3] = {nan, 0.f, 0.f}, steep[3] = {91.f, 0.f, 0.f},
              fast[3] = {20000.f, 0.f, 0.f};
  Check(pe::ValidateTeleport(far, nullptr, nullptr) != nullptr, "off-map origin refused");
  Check(pe::ValidateTeleport(bad, nullptr, nullptr) != nullptr, "NaN origin refused");
  Check(pe::ValidateTeleport(nullptr, bad, nullptr) != nullptr, "NaN angles refused");
  Check(pe::ValidateTeleport(nullptr, steep, nullptr) != nullptr, "pitch > 90 refused");
  Check(pe::ValidateTeleport(nullptr, nullptr, fast) != nullptr, "absurd velocity refused");

  // Classnames.
  Check(!pe::ValidateClassname("beam") && !pe::ValidateClassname("prop_dynamic") && !pe::ValidateClassname("env_sprite"),
        "ordinary classnames");
  Check(pe::ValidateClassname("") && pe::ValidateClassname(nullptr) && pe::ValidateClassname("Beam") &&
            pe::ValidateClassname("beam;quit") && pe::ValidateClassname(std::string(64, 'a').c_str()),
        "empty, upper case, punctuation, too long refused");
  Check(pe::ValidateClassname("player") && pe::ValidateClassname("cs_player_controller") &&
            pe::ValidateClassname("cs_gamerules") && pe::ValidateClassname("worldent"),
        "players / game rules / world refused");

  // The ru_api members are appended at the end (1.13).
  Check(offsetof(ru_api, engine_feature_available) > offsetof(ru_api, grenade_spawn_available) &&
            offsetof(ru_api, entity_spawn) > offsetof(ru_api, player_teleport),
        "1.13 members follow 1.12");
  Check(RU_API_VERSION_MINOR(READYUP_PLUGIN_API_VERSION) >= 13, "API minor 13");
  Check(offsetof(ru_api, license_key) > offsetof(ru_api, license_stopped), "1.16 member follows 1.15");
  Check(offsetof(ru_api, player_respawn) > offsetof(ru_api, license_key), "1.17 member follows 1.16");
  Check(RU_API_VERSION_MINOR(READYUP_PLUGIN_API_VERSION) >= 17, "API minor 17");

  std::printf("practice_engine_spec_test: %s\n", g_failed ? "FAIL" : "PASS");
  return g_failed ? 1 : 0;
}

#include "readyup/practice_engine.h"

#include "readyup/engine_surface.h"
#include "readyup/entity.h"
#include "readyup/logging.h"
#include "readyup/practice_engine_spec.h"
#include "readyup/schema.h"

#include <cstdint>
#include <cstring>
#include <optional>

namespace readyup::practice_engine {
namespace {

void* Controller(int slot) {
  if (slot < 0 || slot >= 64 || !entity::EntitySystemReady()) return nullptr;
  return entity::EntityByIndex(slot + 1);
}

// The object must be what the vtable entry is about (its vptr == the RTTI-located vtable) and the
// slot must hold the verified function. Returns that function, or nullptr with *why.
// A vtable entry without a function is trusted on its anchors alone: the call goes to the slot's
// verified target.
void* VerifiedVirtual(const FeatureInfo& f, const void* obj, std::string* why) {
  void* fn = f.function ? EngineFunction(f.function) : nullptr;
  if (f.function && !fn) {
    if (why) *why = "not available on this CS2 build (its gamedata did not verify, or the practice gamedata is not installed)";
    return nullptr;
  }
  int index = -1;
  std::string detail;
  if (!VerifyEngineVtableSlot(f.vtable, obj, &index, &detail)) {
    if (why) *why = std::string(f.vtable) + " did not verify: " + detail;
    return nullptr;
  }
  return fn ? fn : reinterpret_cast<void*>(EngineVtableVerdict(f.vtable).target);
}

}  // namespace

bool Available(const char* feature) {
  const FeatureInfo* f = Feature(feature);
  if (!f || (f->function && !EngineFunction(f->function))) return false;
  if (f->function2 && !EngineFunction(f->function2)) return false;
  int index = -1;
  std::string detail;
  return !f->vtable || VerifyEngineVtableSlot(f->vtable, nullptr, &index, &detail);
}

bool ChangeTeam(int slot, int team, std::string* why) {
  auto fail = [&](const std::string& w) {
    if (why) *why = w;
    return false;
  };
  if (!ValidTeam(team)) return fail("team must be 1 (spectators), 2 (T) or 3 (CT)");
  void* ctrl = Controller(slot);
  if (!ctrl) return fail("slot " + std::to_string(slot) + " is not a player");
  void* fn = VerifiedVirtual(*Feature("change_team"), ctrl, why);
  if (!fn) return false;
  // CCSPlayerController::ChangeTeam(this, int team); the signature covers `movzx r14d, byte [rdi+m_iTeamNum];
  // cmp esi, r14d`, so the ABI (this in rdi, team in esi) is what the check matched.
  using Fn = void (*)(void*, int);
  reinterpret_cast<Fn>(fn)(ctrl, team);
  return true;
}

int TeamOf(int slot) {
  void* ctrl = Controller(slot);
  if (!ctrl) return -1;
  // Resolved once (and again only while the schema does not have it yet).
  static std::optional<int> off;
  if (!off) off = SchemaFindOffset("server", "CCSPlayerController", "m_iTeamNum");
  if (!off) return -1;
  // A byte (ChangeTeam itself reads it with `movzx r14d, byte [rdi+m_iTeamNum]`).
  const int team = static_cast<const unsigned char*>(ctrl)[*off];
  return team <= 3 ? team : -1;
}

bool Teleport(int slot, const float* origin, const float* angles, const float* velocity, std::string* why) {
  auto fail = [&](const std::string& w) {
    if (why) *why = w;
    return false;
  };
  if (const char* bad = ValidateTeleport(origin, angles, velocity)) return fail(bad);
  void* ctrl = Controller(slot);
  if (!ctrl) return fail("slot " + std::to_string(slot) + " is not a player");
  const auto pawnOff = SchemaFindOffset("server", "CCSPlayerController", "m_hPlayerPawn");
  const auto lifeOff = SchemaFindOffset("server", "CBaseEntity", "m_lifeState");
  if (!pawnOff || !lifeOff) return fail("schema not ready");
  uint32_t pawnHandle = 0;
  std::memcpy(&pawnHandle, static_cast<const char*>(ctrl) + *pawnOff, sizeof(pawnHandle));
  void* pawn = entity::EntityFromHandle(pawnHandle);
  if (!pawn || static_cast<const unsigned char*>(pawn)[*lifeOff] != 0) return fail("the player is not alive");
  void* fn = VerifiedVirtual(*Feature("teleport"), pawn, why);
  if (!fn) return false;
  // CCSPlayerPawn::Teleport(this, const Vector* origin, const QAngle* angles, const Vector* velocity):
  // three packed floats each (padded and aligned like entity::SetAbsOrigin's), only read.
  alignas(16) float o[4] = {0, 0, 0, 0}, a[4] = {0, 0, 0, 0}, v[4] = {0, 0, 0, 0};
  if (origin) std::memcpy(o, origin, 3 * sizeof(float));
  if (angles) std::memcpy(a, angles, 3 * sizeof(float));
  if (velocity) std::memcpy(v, velocity, 3 * sizeof(float));
  using Fn = void (*)(void*, const float*, const float*, const float*);
  reinterpret_cast<Fn>(fn)(pawn, origin ? o : nullptr, angles ? a : nullptr, velocity ? v : nullptr);
  return true;
}

int Respawn(int slot, std::string* why) {
  auto fail = [&](const std::string& w) {
    if (why) *why = w;
    return 0;
  };
  void* ctrl = Controller(slot);
  if (!ctrl) return fail("slot " + std::to_string(slot) + " is not a player");
  const auto teamOff = SchemaFindOffset("server", "CBaseEntity", "m_iTeamNum");
  const auto pawnOff = SchemaFindOffset("server", "CCSPlayerController", "m_hPlayerPawn");
  const auto lifeOff = SchemaFindOffset("server", "CBaseEntity", "m_lifeState");
  if (!teamOff || !pawnOff || !lifeOff) return fail("schema not ready");
  uint32_t pawnHandle = 0;
  std::memcpy(&pawnHandle, static_cast<const char*>(ctrl) + *pawnOff, sizeof(pawnHandle));
  void* pawn = entity::EntityFromHandle(pawnHandle);
  if (!pawn) return fail("the player has no pawn");
  const int team = static_cast<const unsigned char*>(ctrl)[*teamOff];
  const bool alive = static_cast<const unsigned char*>(pawn)[*lifeOff] == 0;
  if (const char* bad = ValidateRespawn(team, false)) return fail(bad);
  if (alive) return 2;
  const FeatureInfo& f = *Feature("respawn");
  void* setPawn = EngineFunction(f.function2);
  if (!setPawn) return fail("not available on this CS2 build (its gamedata did not verify)");
  void* fn = VerifiedVirtual(f, ctrl, why);
  if (!fn) return 0;
  // CBasePlayerController::SetPawn(this, pawn, bool, bool, bool, bool) with CounterStrikeSharp's
  // (true, false, false, false): a player who died and watched from the observer pawn owns the
  // player pawn again, then CCSPlayerController::Respawn(this) respawns that current pawn.
  using SetPawnFn = void (*)(void*, void*, bool, bool, bool, bool);
  reinterpret_cast<SetPawnFn>(setPawn)(ctrl, pawn, true, false, false, false);
  using Fn = void (*)(void*);
  reinterpret_cast<Fn>(fn)(ctrl);
  return 1;
}

void* Create(const char* classname, std::string* why) {
  auto fail = [&](const std::string& w) -> void* {
    if (why) *why = w;
    return nullptr;
  };
  if (const char* bad = ValidateClassname(classname)) return fail(bad);
  if (!Available("entity_create")) {
    return fail("not available on this CS2 build (its gamedata did not verify, or the practice gamedata is not installed)");
  }
  if (!entity::EntitySystemReady()) return fail("no map loaded yet");
  // UTIL_CreateEntityByName(const char* classname, int forceEntityIndex): -1 = any free index.
  using Fn = void* (*)(const char*, int);
  void* ent = reinterpret_cast<Fn>(EngineFunction("UTIL_CreateEntityByName"))(classname, -1);
  if (!ent) return fail(std::string("the engine did not create a '") + classname + "'");
  return ent;
}

bool Spawn(void* ent, std::string* why) {
  auto fail = [&](const std::string& w) {
    if (why) *why = w;
    return false;
  };
  if (!ent) return fail("no entity");
  if (!Available("entity_create")) {
    return fail("not available on this CS2 build (its gamedata did not verify, or the practice gamedata is not installed)");
  }
  const uint32_t h = entity::EntityHandleOf(ent);
  if (h == 0xFFFFFFFFu || entity::EntityFromHandle(h) != ent) return fail("not a live entity");
  if (static_cast<int>(h & 0x7FFF) <= 64) return fail("the world and player controllers cannot be spawned");
  // CBaseEntity::DispatchSpawn(CBaseEntity* entity, CEntityKeyValues* keyValues): null = none.
  using Fn = void (*)(void*, void*);
  reinterpret_cast<Fn>(EngineFunction("CBaseEntity_DispatchSpawn"))(ent, nullptr);
  return true;
}

}  // namespace readyup::practice_engine

// Warmup ammo (see warmup_ammo.h).
#include "readyup/warmup_ammo.h"

#include "readyup/host.h"
#include "readyup/logging.h"
#include "readyup/modes.h"
#include "readyup/plugin_api.h"

#include <cstring>

namespace readyup {
namespace {

struct Offsets {
  int pawn = -1;            // CCSPlayerController::m_hPlayerPawn (CHandle)
  int weaponServices = -1;  // CBasePlayerPawn::m_pWeaponServices (ptr)
  int myWeapons = -1;       // CPlayer_WeaponServices::m_hMyWeapons ({int size; CHandle* elems @+8})
  int reserve = -1;         // CBasePlayerWeapon::m_pReserveAmmo (int32[2], primary first)
  bool resolved = false;
  bool Ok() const { return pawn >= 0 && weaponServices >= 0 && myWeapons >= 0 && reserve >= 0; }
} g_off;

ReserveLedger g_ledger;

template <typename T>
T Rd(const void* base, int off) {
  T v{};
  std::memcpy(&v, static_cast<const unsigned char*>(base) + off, sizeof(T));
  return v;
}

bool Resolve(const ru_api* a) {
  if (!g_off.resolved) {
    if (a->entity_system_status(a->self) != RU_ENTSYS_OK) return false;
    g_off.pawn = a->schema_offset(a->self, "CCSPlayerController", "m_hPlayerPawn");
    g_off.weaponServices = a->schema_offset(a->self, "CBasePlayerPawn", "m_pWeaponServices");
    g_off.myWeapons = a->schema_offset(a->self, "CPlayer_WeaponServices", "m_hMyWeapons");
    g_off.reserve = a->schema_offset(a->self, "CBasePlayerWeapon", "m_pReserveAmmo");
    g_off.resolved = true;
    if (!g_off.Ok()) {
      Print("warmup-ammo: schema fields missing (pawn=%d services=%d weapons=%d reserve=%d); warmup ammo is not "
            "infinite\n",
            g_off.pawn, g_off.weaponServices, g_off.myWeapons, g_off.reserve);
    }
  }
  return g_off.Ok();
}

int KeepPlayerReserves(void* user, const ru_player* p) {
  const auto* a = static_cast<const ru_api*>(user);
  if (p->slot < 0 || !p->connected) return 1;
  void* ctrl = a->entity_by_index(a->self, p->slot + 1);
  if (!ctrl) return 1;
  void* pawn = a->entity_from_handle(a->self, Rd<uint32_t>(ctrl, g_off.pawn));
  if (!pawn) return 1;
  void* ws = Rd<void*>(pawn, g_off.weaponServices);
  if (!ws) return 1;
  const int count = Rd<int32_t>(ws, g_off.myWeapons);
  const uint32_t* handles = Rd<const uint32_t*>(ws, g_off.myWeapons + 8);
  if (!handles || count <= 0 || count > 64) return 1;
  for (int i = 0; i < count; ++i) {
    void* weapon = a->entity_from_handle(a->self, handles[i]);
    if (!weapon) continue;
    const int32_t reserve = Rd<int32_t>(weapon, g_off.reserve);
    const int32_t keep = g_ledger.Keep(handles[i], reserve);
    if (keep == reserve) continue;
    std::memcpy(static_cast<unsigned char*>(weapon) + g_off.reserve, &keep, sizeof(keep));
    a->entity_mark_changed(a->self, weapon);
  }
  return 1;
}

}  // namespace

void WarmupAmmoTick() {
  if (!WarmupAmmoActive(GetModeString(), WarmupInfiniteAmmoEnabled(), RecoveryGateEnabled())) {
    g_ledger.Clear();
    return;
  }
  const ru_api* a = host::Api();
  if (!a || !Resolve(a)) return;
  a->for_each_player(a->self, &KeepPlayerReserves, const_cast<ru_api*>(a));
  g_ledger.EndPass();
}

}  // namespace readyup

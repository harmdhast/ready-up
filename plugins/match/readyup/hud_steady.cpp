#include "readyup/hud_steady.h"
#include "readyup/hud_steady_logic.h"

#include "readyup/config.h"
#include "readyup/host.h"
#include "readyup/logging.h"
#include "readyup/plugin_api.h"

#include <cstdint>
#include <cstring>

namespace readyup {
namespace {

constexpr int kMaxIndex = 2048;

int g_offRules = -2, g_offRestart = -2, g_offRestartTime = -2, g_offRoundStart = -2;  // -2 = not looked up
uint32_t g_proxy = 0xFFFFFFFFu;  // cs_gamerules handle
bool g_logged = false;
bool g_scanned = false;  // one full scan per map when cs_gamerules is missing

bool Resolve(const ru_api* a) {
  if (g_offRules == -2) {
    g_offRules = a->schema_offset(a->self, "CCSGameRulesProxy", "m_pGameRules");
    g_offRestart = a->schema_offset(a->self, "CCSGameRules", "m_bGameRestart");
    g_offRestartTime = a->schema_offset(a->self, "CCSGameRules", "m_flRestartRoundTime");
    g_offRoundStart = a->schema_offset(a->self, "CCSGameRules", "m_fRoundStartTime");
    if (g_offRules < 0 || g_offRestart < 0 || g_offRestartTime < 0 || g_offRoundStart < 0) {
      Print("hud-steady: game rules fields not found; center panels may blink\n");
    }
  }
  return g_offRules >= 0 && g_offRestart >= 0 && g_offRestartTime >= 0 && g_offRoundStart >= 0;
}

void* Proxy(const ru_api* a) {
  if (g_proxy != 0xFFFFFFFFu) {
    if (void* e = a->entity_from_handle(a->self, g_proxy)) return e;
    g_proxy = 0xFFFFFFFFu;
  }
  if (g_scanned) return nullptr;
  g_scanned = true;
  for (int i = 0; i < kMaxIndex; ++i) {
    void* e = a->entity_by_index(a->self, i);
    if (!e) continue;
    const char* cls = a->entity_classname(a->self, e);
    if (cls && std::strcmp(cls, "cs_gamerules") == 0) {
      g_proxy = a->entity_handle_of(a->self, e);
      g_scanned = false;
      return e;
    }
  }
  return nullptr;
}

}  // namespace

void HudSteadyOnMapStart() {
  g_proxy = 0xFFFFFFFFu;
  g_scanned = false;
  g_logged = false;
}

void HudSteadyTick() {
  if (!Cfg().hud_steady) return;
  const ru_api* a = host::Api();
  if (!a || a->entity_system_status(a->self) != RU_ENTSYS_OK || !Resolve(a)) return;
  void* proxy = Proxy(a);
  if (!proxy) return;
  auto* rules = *reinterpret_cast<uint8_t**>(static_cast<uint8_t*>(proxy) + g_offRules);
  if (!rules) return;
  float restartTime = 0, roundStart = 0;
  std::memcpy(&restartTime, rules + g_offRestartTime, sizeof restartTime);
  std::memcpy(&roundStart, rules + g_offRoundStart, sizeof roundStart);
  bool& restart = *reinterpret_cast<bool*>(rules + g_offRestart);
  const bool want = HudSteadyGameRestart(restartTime, roundStart);
  if (!g_logged) {
    g_logged = true;
    Debug("hud-steady: restart_time=%.2f round_start=%.2f game_restart=%d -> %d\n", restartTime, roundStart,
          restart ? 1 : 0, want ? 1 : 0);
  }
  if (restart == want) return;
  restart = want;
  a->entity_mark_changed(a->self, proxy);
}

}  // namespace readyup

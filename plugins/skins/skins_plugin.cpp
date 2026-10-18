// readyup-skins: weapon paints, knives, gloves and agent models. Standalone they come from
// loadouts.json in the plugin data dir (docs/json-contract.md), in fleet mode from the platform
// (skins.loadout, docs/FLEET.md D6). No database (D13).
//
// Ships separately from the core (csgo/readyup/plugins/skins.so plus the gamedata fragment
// engine-surface.skins.json next to the core) because servers running skin changers risk GSLT
// bans. Everything engine-facing goes through ru_api; the plugin has no signatures, offsets or
// hooks of its own.
//
//   skins_status               (console)  loadout source / cache / apply counters
//   skins_refresh [steamid64]  (console)  refetch one player's (or everyone's) loadout
//   skins_debug_as <slot> <steamid64>  (console, debug=1 only) decorate a bot with a player's
//                              loadout, to test the apply path without a human client
#include "skins.h"
#include "skins_reload.h"

#include "apply_internal.h"

#include "readyup/match_iface.h"
#include "readyup/selftest_iface.h"
#include "readyup/skins_iface.h"

#include <atomic>
#include <cctype>
#include <chrono>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <map>
#include <mutex>
#include <string>
#include <vector>

#ifndef SKINS_VERSION
#define SKINS_VERSION "0.0.0-dev"
#endif

namespace skins {

const ru_api* g_api = nullptr;

void Log(int level, const char* fmt, ...) {
  if (!g_api) return;
  char buf[1024];
  va_list ap;
  va_start(ap, fmt);
  std::vsnprintf(buf, sizeof(buf), fmt, ap);
  va_end(ap);
  g_api->log(g_api->self, level, buf);
}

bool DebugOn() { return g_api && g_api->debug_enabled(g_api->self) != 0; }

int SchemaOffset(const char* cls, const char* field) { return g_api->schema_offset(g_api->self, cls, field); }

namespace {
std::atomic<bool> g_inert{false};
std::mutex g_inertMu;
std::string g_inertReason;
double g_lastInertCheck = -1e9;

// +sv_setsteamaccount <token> on the command line: the server logs in with a Valve game server
// token (GSLT). Valve bans the token, and the account behind it, of a server that lets players use
// items they do not own, so the plugin stays inert on such a server. Read once (the command line
// does not change).
bool LaunchedWithSteamToken() {
  FILE* f = std::fopen("/proc/self/cmdline", "rb");
  if (!f) return false;
  std::string cmd;
  char buf[4096];
  size_t n = 0;
  while ((n = std::fread(buf, 1, sizeof(buf), f)) > 0 && cmd.size() < (1u << 16)) cmd.append(buf, n);
  std::fclose(f);
  std::vector<std::string> args;
  size_t s = 0;
  for (size_t i = 0; i <= cmd.size(); ++i) {
    if (i == cmd.size() || cmd[i] == '\0') {
      if (i > s) args.push_back(cmd.substr(s, i - s));
      s = i + 1;
    }
  }
  for (size_t i = 0; i + 1 < args.size(); ++i) {
    if (args[i] == "+sv_setsteamaccount" && !args[i + 1].empty() && args[i + 1][0] != '+' && args[i + 1][0] != '-') {
      return true;
    }
  }
  return false;
}
const bool g_steamToken = LaunchedWithSteamToken();

// Game thread, once a second: the match plugin knows the loaded match's ruleset (and overrides);
// without it, readyup.cfg `ruleset=` (config_get falls back to the core key).
void RefreshInert(double now) {
  if (now - g_lastInertCheck < 1.0) return;
  g_lastInertCheck = now;
  bool locked = false;
  std::string ruleset = "default";
  const auto* m = static_cast<const ru_match_v1*>(g_api->get_interface(g_api->self, RU_MATCH_IFACE_NAME, 1));
  if (m && RU_API_HAS(m, inventory_locked) && m->inventory_locked) {
    locked = m->inventory_locked() != 0;
    if (RU_API_HAS(m, ruleset) && m->ruleset && m->ruleset()) ruleset = m->ruleset();
  } else {
    char buf[32] = {};
    if (g_api->config_get(g_api->self, "ruleset", buf, sizeof(buf)) > 0) {
      ruleset = buf;
      for (auto& c : ruleset) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
      locked = ruleset == "valve";
    }
  }
  std::string reason =
      !locked ? std::string() : ruleset == "valve" ? "inert (valve ruleset)" : "inert (cosmetics: inventory)";
  if (g_steamToken) {
    locked = true;
    reason = "inert (Steam server token: remove +sv_setsteamaccount to use skins)";
  }
  std::string prev;
  {
    std::lock_guard<std::mutex> lk(g_inertMu);
    prev = g_inertReason;
    g_inertReason = reason;
  }
  g_inert.store(locked);
  if (reason != prev) {
    if (locked && g_steamToken) {
      Log(RU_LOG_WARN, "%s: Valve bans the token of a server that hands out items; nothing is applied",
          reason.c_str());
    } else if (locked) {
      Log(RU_LOG_WARN, "%s: players' inventories are not modified (Valve rulebook); nothing is applied or restored",
          reason.c_str());
    } else {
      Log(RU_LOG_INFO, "active again (ruleset %s)", ruleset.c_str());
    }
  }
}

void RunSelftest(ru_selftest_add_fn add, void* ctx) {
  std::lock_guard<std::mutex> lk(g_inertMu);
  add(ctx, "INFO", "skins", g_inertReason.empty() ? "active (ruleset default)" : g_inertReason.c_str());
}
const ru_selftest_iface_v1 g_selftestIface = {sizeof(ru_selftest_iface_v1), &RunSelftest};

// readyup.skins.v1 (the midas plugin paints its gold weapons through this).
int SkinsActiveIface() {
  try {
    return ExternalPaintReady() ? 1 : 0;
  } catch (...) {
    return 0;
  }
}
int PaintWeaponIface(uint32_t handle, uint64_t steamid64, int32_t paintKit, float wear, int32_t seed) {
  try {
    return PaintWeaponExternal(handle, steamid64, paintKit, wear, seed) ? 1 : 0;
  } catch (...) {
    return 0;
  }
}
int SetPlayerPaintIface(uint64_t steamid64, int32_t paintKit, float wear, int32_t seed) {
  try {
    return SetPlayerPaintExternal(steamid64, paintKit, wear, seed) ? 1 : 0;
  } catch (...) {
    return 0;
  }
}
int RefreshWeaponsIface(int slot) {
  try {
    return RefreshWeaponsExternal(slot) ? 1 : 0;
  } catch (...) {
    return 0;
  }
}
int SetPlayerNameTagIface(uint64_t steamid64, const char* nameTag) {
  try {
    return SetPlayerNameTagExternal(steamid64, nameTag) ? 1 : 0;
  } catch (...) {
    return 0;
  }
}
const ru_skins_v1 g_skinsIface = {sizeof(ru_skins_v1), &SkinsActiveIface, &PaintWeaponIface, &SetPlayerPaintIface,
                                  &RefreshWeaponsIface, &SetPlayerNameTagIface};
}  // namespace

bool Inert() { return g_inert.load(); }
std::string InertReason() {
  std::lock_guard<std::mutex> lk(g_inertMu);
  return g_inertReason;
}

namespace {

bool g_haveStore = false;

void OnTick(void*, const ru_tick_info* t) {
  try {
    RefreshInert(t->now);
    FleetTick(t->now);
    GameFrameTick();
  } catch (const std::exception& e) {
    Log(RU_LOG_ERROR, "tick threw: %s", e.what());
  }
}

uint64_t SteamOfEventPlayer(const ru_game_event* ev, const char* key) {
  const int slot = g_api->ev_get_player_slot(g_api->self, ev, key);
  if (slot < 0) return 0;
  ru_player p{};
  p.struct_size = sizeof(p);
  return g_api->get_player(g_api->self, slot, &p) == 1 ? p.steamid64 : 0;
}

// Spawn / pickup / equip: prefetch the loadout so it is cached by the time GameFrameTick sees the
// new pawn or weapon (item_equip's "item" is a classname string, not an entity).
void OnPrefetchEvent(void*, const char*, const ru_game_event* ev) {
  if (Inert()) return;
  if (const uint64_t sid = SteamOfEventPlayer(ev, "userid")) MaybeRefreshAsync(sid);
}

void OnSpawnEvent(void* user, const char* name, const ru_game_event* ev) {
  OnPrefetchEvent(user, name, ev);
  if (Inert()) return;
  RequestSpawnCosmetics(g_api->ev_get_player_slot(g_api->self, ev, "userid"));
}

std::map<uint64_t, double> g_lastReload;  // steamid64 -> when `.skins reload` last ran

// The match plugin's ru_mode ("" without it).
std::string RuMode() {
  const auto* m = static_cast<const ru_match_v1*>(g_api->get_interface(g_api->self, RU_MATCH_IFACE_NAME, 1));
  if (!m || !m->get_status) return {};
  ru_match_status st{};
  st.struct_size = sizeof(st);
  if (m->get_status(&st) != 1 || !st.ru_mode) return "unknown";  // can't tell: treat as live
  return st.ru_mode;
}

// `.skins reload` (also `.ru skins reload`): re-read the caller's loadout and re-apply it.
void OnSkinsChat(void*, const ru_command_ctx* ctx) {
  auto reply = [&](const char* msg) {
    if (ctx->slot >= 0) g_api->chat_to_slot(g_api->self, ctx->slot, msg);
  };
  const std::string sub = ctx->argc >= 2 && ctx->argv[1] ? ctx->argv[1] : "";
  if (sub != "reload") {
    reply("Ready Up skins: .skins reload  (re-read your skins; not while a match is live)");
    return;
  }
  if (ctx->steamid64 == 0 || ctx->slot < 0) return;
  if (Inert()) {
    reply("Ready Up skins: skins are off on this server right now.");
    return;
  }
  const std::string mode = RuMode();
  if (!SkinsReloadAllowed(mode)) {
    reply("Ready Up skins: .skins reload is not available while a match is live.");
    Log(RU_LOG_INFO, "skins reload by %llu refused (mode %s)", static_cast<unsigned long long>(ctx->steamid64),
        mode.c_str());
    return;
  }
  const double now = std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
  const auto it = g_lastReload.find(ctx->steamid64);
  if (!SkinsReloadCooldownOk(now, it == g_lastReload.end() ? -1 : it->second)) {
    reply("Ready Up skins: wait a few seconds before reloading again.");
    return;
  }
  g_lastReload[ctx->steamid64] = now;
  Invalidate(ctx->steamid64);
  MaybeRefreshAsync(ctx->steamid64);
  RequestReapply(ctx->slot);
  Log(RU_LOG_INFO, "skins reload by %llu (slot %d, mode %s)", static_cast<unsigned long long>(ctx->steamid64),
      ctx->slot, mode.empty() ? "no match plugin" : mode.c_str());
  reply("Ready Up skins: reloading your skins. Gloves, agent and the weapons you hold update in a second.");
}

// A round restart (mp_restartgame, a match going live, a new round): re-gives wait a moment.
void OnRoundChangeEvent(void*, const char*, const ru_game_event*) { NoteRoundChange(); }

void OnDeathEvent(void*, const char*, const ru_game_event* ev) {
  if (Inert()) return;  // no StatTrak counting either
  try {
    OnPlayerDeath(ev);
  } catch (...) {
  }
}

void OnStatus(void*, const ru_command_ctx*) {
  Log(RU_LOG_INFO, "status: version " SKINS_VERSION "; %sloadouts: %s; apply: %s; entity system: %s",
      Inert() ? ("skins " + InertReason() + "; ").c_str() : "", LoadoutStatus().c_str(),
      ApplyStatus().c_str(),
      g_api->entity_system_status(g_api->self) == RU_ENTSYS_OK       ? "ok"
      : g_api->entity_system_status(g_api->self) == RU_ENTSYS_FAILED ? "FAILED"
                                                                      : "pending (no map yet)");
}

void OnRefresh(void*, const ru_command_ctx* ctx) {
  if (ctx->argc >= 2) {
    const uint64_t sid = std::strtoull(ctx->argv[1], nullptr, 10);
    Invalidate(sid);
    MaybeRefreshAsync(sid);
    Log(RU_LOG_INFO, "refreshing loadout for %llu", static_cast<unsigned long long>(sid));
    return;
  }
  int n = 0;
  g_api->for_each_player(
      g_api->self,
      [](void* user, const ru_player* p) {
        if (p->steamid64 != 0) {
          Invalidate(p->steamid64);
          MaybeRefreshAsync(p->steamid64);
          ++*static_cast<int*>(user);
        }
        return 1;
      },
      &n);
  Log(RU_LOG_INFO, "refreshing loadouts for %d connected player(s)", n);
}

void OnDebugAs(void*, const ru_command_ctx* ctx) {
  if (!DebugOn()) {
    Log(RU_LOG_WARN, "skins_debug_as needs readyup.cfg debug=1");
    return;
  }
  if (ctx->argc < 3) {
    Log(RU_LOG_INFO, "usage: skins_debug_as <bot slot> <steamid64|0>");
    return;
  }
  const int slot = std::atoi(ctx->argv[1]);
  const uint64_t sid = std::strtoull(ctx->argv[2], nullptr, 10);
  if (!SetDebugAs(slot, sid)) {
    Log(RU_LOG_WARN, "bad slot %d", slot);
    return;
  }
  if (sid) MaybeRefreshAsync(sid);
  Log(RU_LOG_INFO, "debug: slot %d uses the loadout of %llu (bots only; takes effect on respawn / new weapons)",
      slot, static_cast<unsigned long long>(sid));
}

}  // namespace

bool WarmupLike() { return SkinsReloadAllowed(RuMode()); }

}  // namespace skins

using namespace skins;

extern "C" {

READYUP_PLUGIN_EXPORT const ru_plugin_info* readyup_plugin_info(void) {
  static const ru_plugin_info info = {
      sizeof(ru_plugin_info),
      (1u << 16) | 1u,  // needs API 1.1 (entities, schema, raw events, config_dir)
      "skins",
      SKINS_VERSION,
      "Ready Up",
      "weapon paints, knives, gloves and agents (loadouts.json or the platform)",
  };
  return &info;
}

READYUP_PLUGIN_EXPORT int readyup_plugin_load(const ru_api* api, uint32_t core_api_version) {
  if (RU_API_VERSION_MAJOR(core_api_version) != 1 || !RU_API_HAS(api, stash_get)) return 1;
  try {
    g_api = api;
    detail::ResetOffsets();
    g_haveStore = LoadoutStart();
    if (!g_haveStore) Log(RU_LOG_WARN, "loadouts unavailable (%s); skins stay idle", LoadoutStatus().c_str());
    api->on_tick(api->self, OnTick, nullptr);
    api->subscribe_game_event(api->self, "player_spawn", OnSpawnEvent, nullptr);
    api->subscribe_game_event(api->self, "item_equip", OnPrefetchEvent, nullptr);
    api->subscribe_game_event(api->self, "item_pickup", OnPrefetchEvent, nullptr);
    api->subscribe_game_event(api->self, "player_death", OnDeathEvent, nullptr);
    api->subscribe_game_event(api->self, "round_prestart", OnRoundChangeEvent, nullptr);
    api->subscribe_game_event(api->self, "round_start", OnRoundChangeEvent, nullptr);
    api->register_console_command(api->self, "skins_status", OnStatus, nullptr);
    api->register_console_command(api->self, "skins_refresh", OnRefresh, nullptr);
    api->register_chat_command(api->self, ".skins", OnSkinsChat, nullptr);
    api->register_console_command(api->self, "skins_debug_as", OnDebugAs, nullptr);
    if (RU_API_HAS(api, provide_interface)) {
      api->provide_interface(api->self, RU_SELFTEST_IFACE_PREFIX "skins", RU_SELFTEST_IFACE_VERSION,
                             const_cast<ru_selftest_iface_v1*>(&g_selftestIface));
      api->provide_interface(api->self, RU_SKINS_IFACE_NAME, RU_SKINS_IFACE_VERSION, const_cast<ru_skins_v1*>(&g_skinsIface));
    }
    g_lastInertCheck = -1e9;
    Log(RU_LOG_INFO, "loaded " SKINS_VERSION " (%s)", LoadoutStatus().c_str());
    return 0;
  } catch (const std::exception& e) {
    Log(RU_LOG_ERROR, "load failed: %s", e.what());
    LoadoutStop();
    return 2;
  }
}

READYUP_PLUGIN_EXPORT void readyup_plugin_unload(void) {
  // Join the loadout worker before the image is unmapped. Paints already applied stay on live
  // entities (ordinary entity state; they reset on respawn / map change).
  try {
    LoadoutStop();
  } catch (...) {
  }
  Log(RU_LOG_INFO, "unloaded");
  g_api = nullptr;
}

}  // extern "C"

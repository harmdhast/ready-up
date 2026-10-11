// readyup-match (csgo/readyup/plugins/match.so): the Ready Up match flow as a plugin.
//
// Everything here used to be compiled into the core (docs/ARCHITECTURE.md, migration step 4):
// ready-up, scrims, the knife round, pauses, practice, match configs (`ru match load`),
// webhooks + heartbeat, JSON-persisted settings / match state and boot recovery (local_store.h),
// MAT / local / platform admins, per-map stats, the map-end / series-end flow, GOTV demos, the welcome card
// and the ready HUD, and the match part of the local status endpoint (readyup.match.v1).
// It talks to the engine only through ru_api (host.cpp); `ru plugin reload match` keeps the
// match going (reload_state.h lists what survives).
#include "readyup/plugin_api.h"
#include "readyup/match_iface.h"
#include "readyup/selftest_iface.h"

#include "readyup/admin_check.h"
#include "readyup/coach.h"
#include "readyup/config.h"
#include "readyup/damage_report.h"
#include "readyup/demo_recorder.h"
#include "readyup/engine.h"
#include "readyup/esports.h"
#include "readyup/fleet_bridge.h"
#include "readyup/game_timers.h"
#include "readyup/host.h"
#include "readyup/idle_refresh.h"
#include "readyup/warmup_money.h"
#include "readyup/warmup_score.h"
#include "readyup/weapon_cleanup.h"
#include "readyup/hud_steady.h"
#include "readyup/local_store.h"
#include "readyup/simulation.h"
#include "readyup/logging.h"
#include "readyup/match_console.h"
#include "readyup/match_events.h"
#include "readyup/match_features.h"
#include "readyup/match_log.h"
#include "readyup/match_stats.h"
#include "readyup/match_recovery.h"
#include "readyup/match_router.h"
#include "readyup/match_settings.h"
#include "readyup/match_status.h"
#include "readyup/modes.h"
#include "readyup/persisted_settings.h"
#include "readyup/player_registry.h"
#include "readyup/players.h"
#include "readyup/ready_hud.h"
#include "readyup/reload_state.h"
#include "readyup/retention.h"
#include "readyup/scrim_flow.h"
#include "readyup/votes.h"
#include "readyup/webhook.h"
#include "readyup/admin_call.h"
#include "readyup/golive_card.h"
#include "readyup/welcome.h"
#include "readyup/workers.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

#ifndef MATCH_VERSION
#define MATCH_VERSION READYUP_SEMVER
#endif

namespace readyup {
namespace {

const ru_api* g_api = nullptr;
std::vector<ru_handle> g_readyCmds;  // `.r` & co: re-registered when consume_ready_chat changes
bool g_readyHidden = false;

// Callbacks must never throw into the core.
template <typename F>
void Guard(const char* what, F&& f) {
  try {
    f();
  } catch (const std::exception& e) {
    Print("match: %s threw: %s\n", what, e.what());
  } catch (...) {
    Print("match: %s threw\n", what);
  }
}

// ---- commands ------------------------------------------------------------------------------

void OnPlayerChat(void*, const ru_command_ctx* c) {
  Guard("chat command", [&] { MatchChatCommand(c->steamid64, c->name, c->text, c->slot); });
}

// `.coach` / `.uncoach` (coach.h): with the sender's slot for the private replies.
void OnCoachChat(void*, const ru_command_ctx* c) {
  Guard("coach command", [&] { CoachChatCommand(c->steamid64, c->name, c->text, c->slot); });
}

void OnRuSub(void*, const ru_command_ctx* c) {
  Guard("ru command", [&] {
    if (c->is_console) MatchRuConsole(c->text);
    else MatchRuCommand(c->steamid64, c->name, c->text, c->slot);
  });
}

void OnConsole(void*, const ru_command_ctx* c) {
  Guard("console command", [&] { (void)MatchConsoleCommand(c->text); });
}

// `hostname "<x>"` typed on the console (server.cfg): the hostname hostname_format gives back.
void OnHostname(void*, const ru_command_ctx* c) {
  Guard("hostname", [&] {
    if (c->argc >= 2 && c->argv[1]) match_settings::ObserveHostname(c->argv[1]);
  });
}

// `tv_delay N` typed on the console: observed for the GOTV flush timing, the engine runs it.
void OnTvDelay(void*, const ru_command_ctx* c) {
  if (c->argc >= 2) demo::ObserveTvDelay(std::atoi(c->argv[1]));
}

void RegisterReadyCommands() {
  const bool hide = ConsumeReadyChat();
  if (!g_readyCmds.empty() && hide == g_readyHidden) return;
  for (ru_handle h : g_readyCmds) g_api->unregister(g_api->self, h);
  g_readyCmds.clear();
  g_readyHidden = hide;
  for (const auto& name : MatchPlayerChatCommands()) {
    const bool isReady = name == ".r" || name == ".ready" || name == ".unready" || name == ".ur" ||
                         name == ".notready" || name == ".nr";
    const uint32_t flags = (isReady && hide) ? static_cast<uint32_t>(RU_CMD_HIDE) : 0u;
    const ru_handle h = g_api->register_chat_command_ex(g_api->self, name.c_str(), flags, &OnPlayerChat, nullptr);
    if (h) g_readyCmds.push_back(h);
  }
}

// ---- admin / captain chat prefixes (set_chat_name_prefix; the core relays the line) --------

std::unordered_map<uint64_t, std::string> g_prefixes;
double g_lastPrefixSync = 0;

void PrefixTick(double now) {
  if (now - g_lastPrefixSync < 2.0) return;
  g_lastPrefixSync = now;
  const auto ctx = WebhookGetMatchContext();
  std::unordered_map<uint64_t, std::string> want;
  for (const auto& h : ListHumans()) {
    if (h.steamid64 == 0) continue;
    std::string p;
    if (IsReadyUpAdmin(h.steamid64)) p = AdminPrefix();
    else if (ctx && h.steamid64 == ctx->team1_captain_steamid64) p = CaptainPrefixTeam1();
    else if (ctx && h.steamid64 == ctx->team2_captain_steamid64) p = CaptainPrefixTeam2();
    if (!p.empty()) want[h.steamid64] = p;
  }
  for (const auto& kv : g_prefixes) {
    if (!want.count(kv.first)) g_api->set_chat_name_prefix(g_api->self, kv.first, nullptr);
  }
  for (const auto& kv : want) {
    auto it = g_prefixes.find(kv.first);
    if (it == g_prefixes.end() || it->second != kv.second) {
      g_api->set_chat_name_prefix(g_api->self, kv.first, kv.second.c_str());
    }
  }
  g_prefixes.swap(want);
}

// ---- frames, ticks, events, log lines ------------------------------------------------------

void OnFrame(void*, const ru_tick_info*) {
  Guard("frame", [] {
    host::FrameBegin();
    // Map-end / series-end timers and the demo stop also run while the server does not simulate.
    GameTimersFrameTick();
    IdleRefreshFrame(host::NowSeconds());
    if (MaybeReloadCfg()) RegisterReadyCommands();
  });
}

void OnTick(void*, const ru_tick_info* t) {
  Guard("tick", [&] {
    // Round events of this frame, after the core delivered this frame's log lines.
    MatchEventsTick();
    if (FeatureEnabled(Feature::MatchFlow)) {
      Tick();
      SimulationTick(t->now);  // simulation mode: bots in/out, simulated ready, timescale (simulation.h)
      ScrimTick();  // scrim flow + `state:` log; outside Tick() (which holds the modes mutex)
      MatchFeaturesTick();  // tactical timeout end, technical auto-unpause, forfeit timer
      DamageReportTick();       // damage reports built at round_end (damage_report.h)
      VotesTick(t->now);        // .gg / .stop vote timeouts (votes.h)
      WarmupMoneyTick(t->now);  // warmup money top-up (warmup_money.h)
      WarmupScoreTick(t->now);  // no round counts in warmup (warmup_score.h)
      match_settings::Tick(t->now);  // autoready, kick_when_no_match_loaded, hostname_format
      WeaponCleanupTick(t->now);  // dropped weapons in warmup (weapon_cleanup.h)
      CoachTick(t->now);          // coaches: m_iCoachingTeam, sv_coaching_enabled (coach.h)
    }
    // Fleet link (no-op without fleet.so): platform handlers, MatchState patches, events.
    fleet_bridge::Tick(t->now);
    // One-off cards before the HUD: the HUD leaves a player's panel alone while one is up.
    if (FeatureEnabled(Feature::MatchFlow)) GoLiveCardTick();  // "LIVE · GO GO GO" (golive_card.h)
    HudSteadyTick();  // m_bGameRestart so center panels do not blink (hud_steady.h)
    AdminCallTick();  // `.admin` cards for in-game admins (admin_call.h)
    if (FeatureEnabled(Feature::WelcomeHtml)) WelcomeTick();
    if (FeatureEnabled(Feature::ReadyHud)) ReadyHudTick();  // skips players whose welcome card is up
    if (FeatureEnabled(Feature::ReadyHud)) ReadyHudAnimTick();  // `.ru hud anim`, every frame
    PrefixTick(t->now);
  });
}

void OnEvent(void*, const ru_event* e) {
  Guard("event", [&] {
    if (e->type == RU_EVENT_MAP_START) {
      IdleRefreshOnMapStart(host::NowSeconds());
      retention::SweepAsync();  // old round backups and demos (retention.h)
      EsportsOnMapStart();  // the GOTV client is looked for again
      demo::OnMapStart();  // GOTV's recording ended with the level change
      fleet_bridge::OnMapStart();
      HudSteadyOnMapStart();  // cs_gamerules is a new entity
      match_recovery::OnMapStart();  // a recovery changing to the match map continues
      // Every map of a real match starts without bots: the gamemode cfg brings bot_quota back on
      // each level change, and a bot alive on a team kept a joining player spectating it (live
      // test, map 2). ApplyLoadedMatch did this only once, before the first map change.
      // Simulation brings its own bots in just below. Only bot_quota 0: the engine removes the
      // quota's bots by itself, while `bot_kick` also kicks the GOTV client (SourceTV) and
      // ends the map's demo recording.
      if (!SimulationActive()) {
        if (auto ctx = WebhookGetMatchContext(); ctx && ctx->slug != "scrim") {
          (void)EnqueueServerCommand("bot_quota 0");
        }
      }
      SimulationOnMapStart(host::NowSeconds());  // the bots start over on the new map
    } else if (e->type == RU_EVENT_ROUND_START) {
      SimulationOnRoundStart();  // host_timescale again while a simulated map is live
    } else if (e->type == RU_EVENT_PLAYER_DISCONNECT) {
      // Ready state must not survive a reconnect.
      if (e->steamid64) ClearReady(e->steamid64);
    } else if (e->type == RU_EVENT_PLAYER_TEAM && e->source == RU_SOURCE_ENGINE) {
      // Welcome card trigger (the log-line source comes through MatchObserveLogLine).
      if (e->team == 2 || e->team == 3) {
        WelcomeObserveTeamJoin(e->slot, e->team, e->steamid64, e->name ? e->name : "", WelcomeSource::GameEvent);
        if (e->steamid64) ModesOnHumanJoinedTeam(e->team);  // spawn the joiner in match warmup
      }
    }
    CoachOnEvent(e);
    if (e->steamid64 && e->name && *e->name) ObservePlayer(e->steamid64, e->name);
    fleet_bridge::OnCoreEvent(e);
  });
}

void OnLogLine(void*, const char* line) {
  Guard("log line", [&] {
    MatchObserveLogLine(line ? line : "");
    fleet_bridge::OnLogLine(line);
  });
}

int AdminProvider(void*, uint64_t steamid64) {
  try {
    return MatchOwnAdmin(steamid64) ? 1 : 0;  // essentials answers for admins.json itself
  } catch (...) {
    return -1;
  }
}

// ---- readyup.match.v1 + selftest lines -------------------------------------------------------

int GetStatus(ru_match_status* out) {
  int rc = 0;
  Guard("status", [&] { rc = MatchStatusGet(out); });
  return rc;
}
int InventoryLockedIface() {
  int rc = 0;
  Guard("inventory_locked", [&] { rc = InventoryLockedNow() ? 1 : 0; });
  return rc;
}
const char* RulesetIface() {
  const char* r = "default";
  Guard("ruleset", [&] { r = RulesetName(CurrentEffectiveRules().ruleset); });
  return r;
}
int SetPracticeIface(int on) {
  int rc = 0;
  Guard("set_practice", [&] { rc = MatchSetPractice(on != 0) ? 1 : 0; });
  return rc;
}
const char* ModeIface() { return GetModeString(); }
// v1.4: per-player totals of the map being recorded (the midas plugin's best-player rule).
int MapStatsIface(ru_match_map_info* info, ru_match_player_stats_fn fn, void* user) {
  if (!info || info->struct_size < sizeof(uint32_t)) return 0;
  int rc = 0;
  Guard("map_stats", [&] {
    const auto ctx = WebhookGetMatchContext();
    ru_match_map_info out{};
    out.struct_size = info->struct_size;
    out.scrim = ctx && ctx->slug == "scrim" ? 1 : 0;
    out.half = MatchEventsSnapshot().swapCount + 1;
    stats::MapStats snap;
    {
      std::lock_guard<std::recursive_mutex> lk(stats::Mutex());
      out.live = stats::Current().Live() ? 1 : 0;
      out.rounds = stats::Current().RoundsRecorded();
      if (out.live && fn) snap = stats::Current().Snapshot();
    }
    std::memcpy(info, &out, std::min<size_t>(info->struct_size, sizeof(out)));
    for (const auto& p : snap.players) {
      if (p.bot || p.id == 0) continue;
      ru_match_player_stats ps{};
      ps.struct_size = sizeof(ps);
      ps.steamid64 = p.id;
      ps.side = p.last_side;
      ps.kills = p.stats.kills;
      ps.deaths = p.stats.deaths;
      ps.assists = p.stats.assists;
      ps.damage = p.stats.damage;
      ps.rounds_played = p.stats.rounds_played;
      fn(user, &ps);
    }
    rc = 1;
  });
  return rc;
}
// v1.5: another plugin's mode (deathmatch) takes the server; NULL / "" hands it back (idle).
int SetExternalModeIface(const char* name) {
  int rc = 0;
  Guard("set_external_mode", [&] { rc = MatchSetExternal(name ? name : "") ? 1 : 0; });
  return rc;
}
const ru_match_v1 g_matchIface = {sizeof(ru_match_v1), &GetStatus, &InventoryLockedIface, &RulesetIface,
                                  &SetPracticeIface, &ModeIface, &MapStatsIface, &SetExternalModeIface};

std::atomic<int> g_hudShowing{0}, g_hudFeature{0};
std::mutex g_brandMu;
std::string g_brandLine;

void RunSelftest(ru_selftest_add_fn add, void* ctx) {
  // Any thread, must not block: report what the game thread / workers cached.
  add(ctx, "INFO", "match", ("readyup-match " MATCH_VERSION ", mode=" + std::string(GetModeString())).c_str());
  add(ctx, "OK", "store", local_store::Summary().c_str());
  add(ctx, "INFO", "ruleset", EsportsSelftestLine().c_str());
  add(ctx, "INFO", "weapon cleanup",
      (Cfg().warmup_weapon_cleanup ? WeaponCleanupStatus() : std::string("off (warmup_weapon_cleanup=0)")).c_str());
  add(ctx, "INFO", "ready HUD",
      (std::string("showing=") + (g_hudShowing.load() ? "yes" : "no") + " (feature " +
       (g_hudFeature.load() ? "on" : "off") + ")")
          .c_str());
  std::lock_guard<std::mutex> lk(g_brandMu);
  add(ctx, "INFO", "hud brand", g_brandLine.c_str());
}
const ru_selftest_iface_v1 g_selftestIface = {sizeof(ru_selftest_iface_v1), &RunSelftest};

// Every ~60 s (and right after load): refresh what the selftest reports.
double g_lastSelftestRefresh = -1e9;
void SelftestRefresh(double now) {
  g_hudShowing.store(ReadyHudShowing() ? 1 : 0);
  g_hudFeature.store(FeatureEnabled(Feature::ReadyHud) ? 1 : 0);
  if (now - g_lastSelftestRefresh < 60.0) return;
  g_lastSelftestRefresh = now;
  const auto c = Cfg();
  const std::string header = HudBrandHtml(24, "fontSize-l");
  char buf[512];
  std::snprintf(buf, sizeof(buf), "hud_brand=\"%s\" hud_logo_url=%s -> header %zu bytes%s", c.hud_brand.c_str(),
                c.hud_logo_url.empty() ? "(none)" : c.hud_logo_url.c_str(), header.size(),
                !c.hud_logo_url.empty() && header.find("<img") == std::string::npos ? " (logo url rejected)" : "");
  {
    std::lock_guard<std::mutex> lk(g_brandMu);
    g_brandLine = buf;
  }
}

void OnSlowTick(void*, const ru_tick_info* t) {
  Guard("selftest refresh", [&] { SelftestRefresh(t->now); });
}

}  // namespace
}  // namespace readyup

using namespace readyup;

extern "C" {

READYUP_PLUGIN_EXPORT const ru_plugin_info* readyup_plugin_info(void) {
  static const ru_plugin_info info = {
      sizeof(ru_plugin_info),
      (1u << 16) | 2u,  // requires 1.2 (log_untagged, ru subcommands, on_frame, ...); 1.3 members via RU_API_HAS
      "match",
      MATCH_VERSION,
      "Ready Up",
      "match flow: ready-up, scrims, knife, pauses, votes, match configs, webhooks, demos, stats",
  };
  return &info;
}

READYUP_PLUGIN_EXPORT int readyup_plugin_load(const ru_api* api, uint32_t core_api_version) {
  if (RU_API_VERSION_MAJOR(core_api_version) != READYUP_PLUGIN_API_VERSION_MAJOR ||
      !RU_API_HAS(api, current_map)) {
    return 1;
  }
  int rc = 0;
  try {
    g_api = api;
    host::Attach(api);
    workers::Start();
    {
      const char* dir = RU_API_HAS(api, data_dir) ? api->data_dir(api->self) : nullptr;
      local_store::Init(dir && *dir ? std::string(dir) : GetThisModuleDir() + "/../../plugins/match");
    }
    (void)ReloadCfg(nullptr);
    match_settings::Install();  // server settings saved in state.json (server_settings.h)
    retention::SweepAsync();    // old round backups and demos (retention.h); again at every map start

    // Commands: player chat, `ru <sub>` / `.ru <sub>`, console settings, `tv_delay` observer.
    RegisterReadyCommands();
    for (const auto& sub : MatchRuSubcommands()) {
      if (!api->register_ru_subcommand(api->self, sub.c_str(), &OnRuSub, nullptr)) {
        Print("match: could not register `ru %s`\n", sub.c_str());
      }
    }
    for (const auto& cmd : MatchConsoleCommands()) {
      if (!api->register_console_command(api->self, cmd.c_str(), &OnConsole, nullptr)) {
        Print("match: could not register console command %s\n", cmd.c_str());
      }
    }
    api->register_console_command_ex(api->self, "tv_delay", RU_CMD_OBSERVE, &OnTvDelay, nullptr);
    api->register_console_command_ex(api->self, "hostname", RU_CMD_OBSERVE, &OnHostname, nullptr);
    for (const char* name : {".coach", ".uncoach"}) {
      if (!api->register_chat_command_ex(api->self, name, 0, &OnCoachChat, nullptr)) {
        Print("match: could not register %s\n", name);
      }
    }

    api->on_frame(api->self, &OnFrame, nullptr);
    api->on_tick(api->self, &OnTick, nullptr);
    api->on_tick(api->self, &OnSlowTick, nullptr);
    api->subscribe(api->self, RU_EVENT_ANY, &OnEvent, nullptr);
    api->subscribe_log_line(api->self, &OnLogLine, nullptr);
    MatchEventsInstall(api);
    DamageReportInstall(api);   // end-of-round damage report
    VotesInstall(api);          // .gg / .stop
    WarmupMoneyInstall(api);    // warmup money top-up
    WarmupScoreInstall(api);    // warmup score reset
    WeaponCleanupInstall(api);  // dropped weapons in warmup
    EsportsInstall(api);  // default_models (player_spawn), halftime pause
    api->set_admin_provider(api->self, &AdminProvider, nullptr);
    api->provide_interface(api->self, RU_MATCH_IFACE_NAME, RU_MATCH_IFACE_VERSION, const_cast<ru_match_v1*>(&g_matchIface));
    api->provide_interface(api->self, RU_SELFTEST_IFACE_PREFIX "match", RU_SELFTEST_IFACE_VERSION,
                           const_cast<ru_selftest_iface_v1*>(&g_selftestIface));
    MatchStatusInstall();
    demo::AddListener(&WebhookEmitDemoEvent);  // demo_recording_* / demo_upload_* webhooks
    fleet_bridge::Install(api);

    // Players already connected (plugin loaded mid-map / reloaded).
    for (const auto& h : ListHumans()) ObservePlayer(h.steamid64, h.name);

    persisted_settings::CaptureDefaults();  // built-in defaults, before anything changes them
    if (!ReloadStateRestore()) {
      // Fresh start (server boot, or no previous image): current map, then the settings and the
      // match persisted in state.json (a rebooted server resumes without the platform re-sending them).
      MatchLogSeedMap(api->current_map(api->self));
      persisted_settings::Restore();
      match_recovery::TryRecoverAsync();
    }
    (void)DevBotsReadyEnabled();  // loud warning if a debug-only flag is on
    (void)DevBotsScrimEnabled();
    ru_logf(api, RU_LOG_INFO, "loaded " MATCH_VERSION " (core %s): mode=%s", api->core_version, GetModeString());
  } catch (const std::exception& e) {
    ru_logf(api, RU_LOG_ERROR, "load failed: %s", e.what());
    rc = 2;
  } catch (...) {
    rc = 2;
  }
  if (rc != 0) {
    workers::Shutdown();
    host::Detach();
  }
  return rc;
}

READYUP_PLUGIN_EXPORT void readyup_plugin_unload(void) {
  try {
    // Without the match flow nothing may keep rounds from ending.
    if (g_api) g_api->set_round_termination_suppressed(g_api->self, 0);
    workers::Shutdown();  // webhook sender, store writer (saves what is pending), admin refresh, demo uploads, ...
    ReloadStateSave();
    fleet_bridge::Uninstall();  // fleet.so cannot see this plugin unload (fleet_iface.h)
    ru_logf(g_api, RU_LOG_INFO, "unloading (mode=%s)", GetModeString());
  } catch (...) {
  }
  host::Detach();
  g_api = nullptr;
}

}  // extern "C"

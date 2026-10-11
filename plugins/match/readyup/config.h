#pragma once

// readyup-match settings.
//
// Read from, in order (later wins):
//   1. readyup.cfg next to the core, top-level keys (where these keys lived while the match flow
//      was part of the core, so existing configs keep working),
//   2. the `[match]` section of readyup.cfg,
//   3. csgo/cfg/ReadyUp/match.cfg (top-level keys or a `[match]` section).
// Re-read automatically when any of those files changes (checked once a second), and on
// `ru reload`. Environment overrides as before: READYUP_DEV_BOTS_READY, READYUP_DEV_BOTS_SCRIM,
// READYUP_CHAT_DEBUG. `debug` is the core's (ru_api debug_enabled).

#include "readyup/match_rules.h"

#include <map>
#include <string>

namespace readyup {

struct ReadyUpCfg {
  // Per-player center-HTML welcome screen on first T/CT join each map.
  bool welcome = true;
  // The welcome card waits this long after a round (re)start: CS2's own "Match started"
  // announcement covers the center panel for a few seconds after the warmup restart.
  int welcome_round_delay_ms = 5000;
  // How long the welcome card stays up once shown, then the ready HUD takes over.
  int welcome_show_seconds = 8;
  // Go-live card (golive_card.h): seconds everyone sees "LIVE · GO GO GO" + the commands once a
  // map goes live (starts welcome_round_delay_ms after the go-live round start). 0 = off.
  int golive_card_seconds = 10;
  // `.admin [message]` (admin_call.h): seconds before the same player can call again.
  int admin_call_cooldown_s = 60;
  // Per-player center-HTML ready list during scrim/match warmup + knife side pick.
  bool ready_hud = true;
  bool hud_steady = true;  // hud_steady.h
  // Center-panel refresh tuning (ms / s).
  int hud_tick_ms = 0;
  int hud_resend_ms = 0;
  int hud_duration_s = 1;
  // Seconds the KNIFE ROUND panel stays up after the knife round begins.
  int hud_knife_hold_s = 10;
  // Header of the welcome card + ready HUD: optional logo image URL + brand text.
  std::string hud_brand = "Auto Tournament";
  std::string hud_logo_url;
  bool chat_debug = false;
  // Admin / captain chat prefixes (token-based; expanded to CS2 chat control bytes).
  std::string admin_prefix;
  std::string captain_prefix_team1;
  std::string captain_prefix_team2;
  // Hide `.r` / `.ready` / `.ur` ... from chat (RU_CMD_HIDE).
  bool consume_ready_chat = false;
  // DEBUG ONLY: bots on CT/T count toward the scrim roster and are always READY.
  bool dev_bots_ready = false;
  // DEBUG ONLY: a scrim can start and run with only bots on CT and T.
  bool dev_bots_scrim = false;
  // Scrims: knife round after everyone readied up is the server setting knife_enabled_default
  // (server_settings.h; readyup.cfg `scrim_knife` is read as its file value).
  // Scrim / match warmup: money topped up to mp_maxmoney after every purchase (warmup_money.h),
  // like CS2's own warmup. 0 = off (players keep what they did not spend).
  bool warmup_money = true;
  // Idle / scrim / match warmup: weapons on the ground with no owner for 2s are removed
  // (weapon_cleanup.h). 0 = off (the warmup cvars still stop death drops).
  bool warmup_weapon_cleanup = true;
  // Scrim knife side-pick window in seconds (match configs use knifeDecisionSeconds).
  int knife_pick_seconds = 60;
  // Damage report in chat to each player after every live round (damage_report.h).
  bool damage_report = true;
  // Idle map refresh (idle_refresh.h): reload the map after this many hours on it with no match
  // loaded and nobody connected. 0 = off.
  int idle_map_refresh_hours = 12;
  // File retention (retention.h): hours an old round backup / resume file
  // (readyup_backup_* / readyup_resume_*) and a GOTV demo are kept after they were last written,
  // except the loaded match's. 0 = keep forever.
  int backup_keep_hours = 72;
  int demo_keep_hours = 24;
  // Pause / ready / forfeit rules (match_rules.h): the values for scrims, and the fallback for
  // match configs that leave a rule out. Keys: max_tech_pauses_per_team, tech_pause_max_seconds,
  // both_teams_unpause_required, allow_force_ready, min_players_to_ready, forfeit_after_seconds.
  MatchRules rules;
  // Ruleset for loaded matches (ruleset.h, docs/ESPORTS-MODE.md): "default" | "valve". A match
  // config's "ruleset" wins. Anything else is logged and read as "default".
  std::string ruleset = "default";
  // Models the `default_models` rule resets players to (per team, the map-independent defaults).
  std::string default_model_ct = "agents/models/ctm_sas/ctm_sas.vmdl";
  std::string default_model_t = "agents/models/tm_phoenix/tm_phoenix.vmdl";
  // Server settings (server_settings.h) named in the file: `playout_enabled_default=1`, ... and the
  // legacy keys `min_players_to_ready`, `scrim_knife`. Handed to settings::Global() on every
  // (re)load; a runtime value (console / chat / fleet) wins over them.
  std::map<std::string, std::string> settings;
};

ReadyUpCfg Cfg();
// ru_api debug_enabled (readyup.cfg debug=1 or READYUP_DEBUG=1). Any thread.
bool DebugEnabled();
bool ChatDebugEnabled();
std::string AdminPrefix();
std::string CaptainPrefixTeam1();
std::string CaptainPrefixTeam2();
bool ConsumeReadyChat();
// Log a loud line whenever the effective value flips.
bool DevBotsReadyEnabled();
bool DevBotsScrimEnabled();
// `ru_dev_bots_scrim 0|1|cfg`: -1 = no override (cfg/env), 0 = off, 1 = on.
void SetDevBotsScrimOverride(int v);
int DevBotsScrimOverride();

// Re-reads every source now. False (and *err) if readyup.cfg could not be read.
bool ReloadCfg(std::string* err = nullptr);
// Frame: re-reads when a source file changed (stat at most once a second). True if it did.
bool MaybeReloadCfg();

}  // namespace readyup

// readyup-match console / RCON commands (moved from the core's command_buffer_hook.cpp): match
// token, webhook / heartbeat / admins URLs, warmup rules, `ru match load <url>`, plus the demo and
// series-end settings of match_end.h / demo_recorder.h. The core consumes the line in its AddText
// hook and delivers it here on the next frame (ru_api register_console_command).
#include "readyup/match_console.h"

#include "readyup/config.h"
#include "readyup/demo_recorder.h"
#include "readyup/engine.h"
#include "readyup/esports.h"
#include "readyup/host.h"
#include "readyup/http_client.h"
#include "readyup/logging.h"
#include "readyup/map_names.h"
#include "readyup/mat_admins.h"
#include "readyup/match_config_parser.h"
#include "readyup/match_end.h"
#include "readyup/match_log.h"
#include "readyup/match_settings.h"
#include "readyup/match_state.h"
#include "readyup/match_token.h"
#include "readyup/modes.h"
#include "readyup/persisted_match_state.h"
#include "readyup/persisted_settings.h"
#include "readyup/round_restore.h"
#include "readyup/webhook.h"
#include "readyup/wingman.h"

#include <atomic>
#include <cctype>
#include <cstdint>
#include <cstring>
#include <optional>
#include <string>
#include <vector>

namespace readyup {
namespace {

static std::string Trim(std::string s) {
  auto is_ws = [](unsigned char ch) { return std::isspace(ch) != 0; };
  while (!s.empty() && is_ws(static_cast<unsigned char>(s.back()))) s.pop_back();
  size_t i = 0;
  while (i < s.size() && is_ws(static_cast<unsigned char>(s[i]))) ++i;
  if (i) s.erase(0, i);
  return s;
}

static std::vector<std::string> SplitWS(const std::string& s) {
  std::vector<std::string> out;
  std::string cur;
  for (size_t i = 0; i < s.size(); ++i) {
    unsigned char c = static_cast<unsigned char>(s[i]);
    if (std::isspace(c) != 0) {
      if (!cur.empty()) {
        out.push_back(cur);
        cur.clear();
      }
      continue;
    }
    cur.push_back(static_cast<char>(c));
  }
  if (!cur.empty()) out.push_back(cur);
  return out;
}

static bool StartsWithToken(const std::string& s, const char* tok) {
  const size_t n = std::strlen(tok);
  if (s.size() < n) return false;
  if (std::memcmp(s.data(), tok, n) != 0) return false;
  if (s.size() == n) return true;
  const unsigned char next = static_cast<unsigned char>(s[n]);
  return std::isspace(next) != 0 || next == ';' || next == '\n' || next == '\r';
}

static std::string StripCrLf(std::string s) {
  std::string out;
  out.reserve(s.size());
  for (char ch : s) {
    if (ch == '\r') continue;
    out.push_back(ch);
  }
  return out;
}

static std::string TruncateForPrint(const std::string& s, size_t maxBytes, bool& truncated) {
  truncated = false;
  if (s.size() <= maxBytes) return s;
  truncated = true;
  return s.substr(0, maxBytes);
}

// Match config auth token (set via ru_match_token).
std::atomic<unsigned long long> g_matchReqId{0};

static std::optional<WebhookMatchContext> ParseMatchContextFromJson(const std::string& json, std::string* errOut) {
  return readyup::ParseWebhookMatchContextFromJson(json, errOut);
}

static bool HandleMatchTokenCommand(const std::string& line) {
  std::string t = Trim(line);
  if (t.empty()) return false;
  if (!StartsWithToken(t, "ru_match_token")) return false;

  // `ru_match_token` -> show status only
  // `ru_match_token clear` -> clear
  // `ru_match_token <token...>` -> set
  auto parts = SplitWS(t);
  if (parts.size() == 1) {
    const bool set = readyup::GetMatchTokenCopy().has_value();
    PrintLine(set ? "match token: set" : "match token: not set");
    return true;
  }

  // Everything after the command name is treated as the token (so we don't lose characters).
  const size_t firstWs = t.find_first_of(" \t");
  std::string token = (firstWs == std::string::npos) ? "" : Trim(t.substr(firstWs));

  if (token == "clear") {
    readyup::SetMatchToken("");
    readyup::persisted_settings::PersistMatchToken(std::nullopt);
    PrintLine("match token: cleared");
    return true;
  }

  if (token.empty()) {
    PrintLine("Usage: ru_match_token <token>  (or: ru_match_token clear)");
    return true;
  }

  readyup::SetMatchToken(token);
  readyup::persisted_settings::PersistMatchToken(token);
  PrintLine("match token: set");
  return true;
}

static bool HandleWebhookUrlCommand(const std::string& line) {
  std::string t = Trim(line);
  if (t.empty()) return false;
  if (!StartsWithToken(t, "ru_webhook_url")) return false;

  auto parts = SplitWS(t);
  if (parts.size() == 1) {
    const std::string cur = WebhookBaseUrl();
    Print("webhook url: %s\n", cur.empty() ? "(not set)" : cur.c_str());
    return true;
  }

  const size_t firstWs = t.find_first_of(" \t");
  std::string url = (firstWs == std::string::npos) ? "" : Trim(t.substr(firstWs));
  if (url == "clear") url.clear();

  WebhookConfigure(url);
  if (url.empty()) readyup::persisted_settings::PersistWebhookUrl(std::nullopt);
  else readyup::persisted_settings::PersistWebhookUrl(url);
  WebhookStartSenderThread();
  PrintLine(url.empty() ? "webhook url: cleared" : "webhook url: set");
  return true;
}

static bool HandleHeartbeatUrlCommand(const std::string& line) {
  std::string t = Trim(line);
  if (t.empty()) return false;
  if (!StartsWithToken(t, "ru_heartbeat_url")) return false;

  auto parts = SplitWS(t);
  if (parts.size() == 1) {
    const std::string cur = WebhookHeartbeatUrl();
    Print("heartbeat url: %s\n", cur.empty() ? "(not set)" : cur.c_str());
    return true;
  }

  const size_t firstWs = t.find_first_of(" \t");
  std::string url = (firstWs == std::string::npos) ? "" : Trim(t.substr(firstWs));
  if (url == "clear") url.clear();

  WebhookConfigureHeartbeatUrl(url);
  if (url.empty()) readyup::persisted_settings::PersistHeartbeatUrl(std::nullopt);
  else readyup::persisted_settings::PersistHeartbeatUrl(url);
  WebhookStartSenderThread();
  PrintLine(url.empty() ? "heartbeat url: cleared" : "heartbeat url: set");
  return true;
}

static bool HandleAdminsUrlCommand(const std::string& line) {
  std::string t = Trim(line);
  if (t.empty()) return false;
  if (!StartsWithToken(t, "ru_admins_url")) return false;

  auto parts = SplitWS(t);
  if (parts.size() == 1) {
    const std::string cur = readyup::mat_admins::AdminsUrl();
    Print("admins url: %s\n", cur.empty() ? "(not set)" : cur.c_str());
    return true;
  }

  const size_t firstWs = t.find_first_of(" \t");
  std::string url = (firstWs == std::string::npos) ? "" : Trim(t.substr(firstWs));
  if (url == "clear") url.clear();

  readyup::mat_admins::ConfigureAdminsUrl(std::move(url));
  readyup::mat_admins::RefreshNow();  // fetch admins JSON immediately
  {
    const std::string cur = readyup::mat_admins::AdminsUrl();
    if (cur.empty()) readyup::persisted_settings::PersistAdminsUrl(std::nullopt);
    else readyup::persisted_settings::PersistAdminsUrl(cur);
  }
  PrintLine(readyup::mat_admins::AdminsUrl().empty() ? "admins url: cleared" : "admins url: set");
  return true;
}

static bool HandleAdminsRefreshSecondsCommand(const std::string& line) {
  std::string t = Trim(line);
  if (t.empty()) return false;
  if (!StartsWithToken(t, "ru_admins_refresh_seconds")) return false;

  auto parts = SplitWS(t);
  if (parts.size() == 1) {
    Print("admins refresh seconds: %d\n", readyup::mat_admins::RefreshSeconds());
    return true;
  }

  if (parts.size() >= 2) {
    const int s = std::atoi(parts[1].c_str());
    if (s >= 0) {
      readyup::mat_admins::ConfigureRefreshSeconds(s);
      readyup::persisted_settings::PersistAdminsRefreshSeconds(readyup::mat_admins::RefreshSeconds());
      PrintLine("admins refresh seconds: set");
      return true;
    }
  }

  PrintLine("Usage: ru_admins_refresh_seconds <0|10..3600>");
  return true;
}

static bool HandleWarmupEnableCommand(const std::string& line) {
  std::string t = Trim(line);
  if (t.empty()) return false;
  if (!StartsWithToken(t, "ru_warmup_enable")) return false;

  auto parts = SplitWS(t);
  if (parts.size() == 1) {
    Print("warmup enabled: %s (%s)\n", WarmupEnabled() ? "1" : "0",
          WarmupEnabled() ? "matches wait for ready-up" : "matches skip ready-up and knife; live on next round start");
    return true;
  }
  if (parts.size() >= 2) {
    const std::string v = parts[1];
    if (v == "1" || v == "true" || v == "on") {
      SetWarmupEnabled(true);
      PrintLine("warmup enabled: 1 (matches wait for ready-up)");
      return true;
    }
    if (v == "0" || v == "false" || v == "off") {
      SetWarmupEnabled(false);
      PrintLine("warmup enabled: 0 (matches skip ready-up and knife; live on next round start)");
      return true;
    }
  }
  PrintLine("Usage: ru_warmup_enable 0|1");
  return true;
}

// DEBUG ONLY: runtime override of readyup.cfg `dev_bots_scrim` (the live test turns it on
// for one run and back to `cfg` afterwards).
static bool HandleDevBotsScrimCommand(const std::string& line) {
  std::string t = Trim(line);
  if (t.empty()) return false;
  if (!StartsWithToken(t, "ru_dev_bots_scrim")) return false;
  auto parts = SplitWS(t);
  if (parts.size() >= 2) {
    const std::string v = parts[1];
    if (v == "1" || v == "on" || v == "true") SetDevBotsScrimOverride(1);
    else if (v == "0" || v == "off" || v == "false") SetDevBotsScrimOverride(0);
    else if (v == "cfg" || v == "default") SetDevBotsScrimOverride(-1);
    else {
      PrintLine("Usage: ru_dev_bots_scrim 0|1|cfg");
      return true;
    }
  }
  const int ov = DevBotsScrimOverride();
  Print("dev_bots_scrim: %d (%s)\n", DevBotsScrimEnabled() ? 1 : 0,
        ov < 0 ? "from readyup.cfg / READYUP_DEV_BOTS_SCRIM" : "console override; `ru_dev_bots_scrim cfg` clears it");
  return true;
}

static bool HandleCfgExecEnableCommand(const std::string& line) {
  std::string t = Trim(line);
  if (t.empty()) return false;
  if (!StartsWithToken(t, "ru_cfg_exec_enable")) return false;

  auto parts = SplitWS(t);
  if (parts.size() == 1) {
    Print("cfg exec enabled: %s\n", CfgExecEnabled() ? "1" : "0");
    return true;
  }
  if (parts.size() >= 2) {
    const std::string v = parts[1];
    if (v == "1" || v == "true" || v == "on") {
      SetCfgExecEnabled(true);
      PrintLine("cfg exec enabled: 1");
      return true;
    }
    if (v == "0" || v == "false" || v == "off") {
      SetCfgExecEnabled(false);
      PrintLine("cfg exec enabled: 0");
      return true;
    }
  }
  PrintLine("Usage: ru_cfg_exec_enable 0|1");
  return true;
}

static bool HandleWarmupMessageHtmlCommand(const std::string& line) {
  std::string t = Trim(line);
  if (t.empty()) return false;
  if (!StartsWithToken(t, "ru_warmup_message_html")) return false;

  auto parts = SplitWS(t);
  if (parts.size() == 1) {
    const std::string cur = WarmupHtmlMessage();
    Print("warmup html: %s\n", cur.empty() ? "(empty)" : cur.c_str());
    return true;
  }

  const size_t firstWs = t.find_first_of(" \t");
  std::string html = (firstWs == std::string::npos) ? "" : Trim(t.substr(firstWs));
  if (html == "clear" || html == "default") html.clear();
  SetWarmupHtmlMessage(std::move(html));
  PrintLine("warmup html: set");
  return true;
}

static bool HandleWarmupRespawnCommand(const std::string& line) {
  std::string t = Trim(line);
  if (t.empty()) return false;
  if (!StartsWithToken(t, "ru_warmup_respawn")) return false;

  auto parts = SplitWS(t);
  if (parts.size() == 1) {
    Print("warmup respawn: %s\n", WarmupRespawnEnabled() ? "1" : "0");
    return true;
  }
  const std::string v = parts.size() >= 2 ? parts[1] : "";
  if (v == "1" || v == "true" || v == "on") {
    SetWarmupRespawnEnabled(true);
    PrintLine("warmup respawn: 1");
    return true;
  }
  if (v == "0" || v == "false" || v == "off") {
    SetWarmupRespawnEnabled(false);
    PrintLine("warmup respawn: 0");
    return true;
  }
  PrintLine("Usage: ru_warmup_respawn 0|1");
  return true;
}

static bool HandleWarmupIgnoreWinCommand(const std::string& line) {
  std::string t = Trim(line);
  if (t.empty()) return false;
  if (!StartsWithToken(t, "ru_warmup_ignore_win_conditions")) return false;

  auto parts = SplitWS(t);
  if (parts.size() == 1) {
    Print("warmup ignore win conditions: %s\n", WarmupIgnoreWinConditions() ? "1" : "0");
    return true;
  }
  const std::string v = parts.size() >= 2 ? parts[1] : "";
  if (v == "1" || v == "true" || v == "on") {
    SetWarmupIgnoreWinConditions(true);
    PrintLine("warmup ignore win conditions: 1");
    return true;
  }
  if (v == "0" || v == "false" || v == "off") {
    SetWarmupIgnoreWinConditions(false);
    PrintLine("warmup ignore win conditions: 0");
    return true;
  }
  PrintLine("Usage: ru_warmup_ignore_win_conditions 0|1");
  return true;
}

static bool HandleWarmupRoundTimeMinutesCommand(const std::string& line) {
  std::string t = Trim(line);
  if (t.empty()) return false;
  if (!StartsWithToken(t, "ru_warmup_roundtime_minutes")) return false;

  auto parts = SplitWS(t);
  if (parts.size() == 1) {
    Print("warmup roundtime minutes: %d\n", WarmupRoundTimeMinutes());
    return true;
  }
  if (parts.size() >= 2) {
    const int m = std::atoi(parts[1].c_str());
    if (m >= 1 && m <= 120) {
      SetWarmupRoundTimeMinutes(m);
      PrintLine("warmup roundtime minutes: set");
      return true;
    }
  }
  PrintLine("Usage: ru_warmup_roundtime_minutes <1..120>");
  return true;
}

static bool HandleWarmupStartMoneyCommand(const std::string& line) {
  std::string t = Trim(line);
  if (t.empty()) return false;
  if (!StartsWithToken(t, "ru_warmup_startmoney")) return false;

  auto parts = SplitWS(t);
  if (parts.size() == 1) {
    Print("warmup startmoney: %d\n", WarmupStartMoney());
    return true;
  }
  if (parts.size() >= 2) {
    const int m = std::atoi(parts[1].c_str());
    if (m >= 0 && m <= 60000) {
      SetWarmupStartMoney(m);
      PrintLine("warmup startmoney: set");
      return true;
    }
  }
  PrintLine("Usage: ru_warmup_startmoney <0..60000>");
  return true;
}

static bool HandleWarmupMaxMoneyCommand(const std::string& line) {
  std::string t = Trim(line);
  if (t.empty()) return false;
  if (!StartsWithToken(t, "ru_warmup_maxmoney")) return false;

  auto parts = SplitWS(t);
  if (parts.size() == 1) {
    Print("warmup maxmoney: %d\n", WarmupMaxMoney());
    return true;
  }
  if (parts.size() >= 2) {
    const int m = std::atoi(parts[1].c_str());
    if (m >= 0 && m <= 60000) {
      SetWarmupMaxMoney(m);
      PrintLine("warmup maxmoney: set");
      return true;
    }
  }
  PrintLine("Usage: ru_warmup_maxmoney <0..60000>");
  return true;
}

static bool HandleWarmupBuyAnywhereCommand(const std::string& line) {
  std::string t = Trim(line);
  if (t.empty()) return false;
  if (!StartsWithToken(t, "ru_warmup_buy_anywhere")) return false;

  auto parts = SplitWS(t);
  if (parts.size() == 1) {
    Print("warmup buy anywhere: %s\n", WarmupBuyAnywhereEnabled() ? "1" : "0");
    return true;
  }
  const std::string v = parts.size() >= 2 ? parts[1] : "";
  if (v == "1" || v == "true" || v == "on") {
    SetWarmupBuyAnywhereEnabled(true);
    PrintLine("warmup buy anywhere: 1");
    return true;
  }
  if (v == "0" || v == "false" || v == "off") {
    SetWarmupBuyAnywhereEnabled(false);
    PrintLine("warmup buy anywhere: 0");
    return true;
  }
  PrintLine("Usage: ru_warmup_buy_anywhere 0|1");
  return true;
}

static bool HandleWarmupInfiniteAmmoCommand(const std::string& line) {
  std::string t = Trim(line);
  if (t.empty()) return false;
  if (!StartsWithToken(t, "ru_warmup_infinite_ammo")) return false;

  auto parts = SplitWS(t);
  if (parts.size() == 1) {
    Print("warmup infinite ammo: %s\n", WarmupInfiniteAmmoEnabled() ? "1" : "0");
    return true;
  }
  const std::string v = parts.size() >= 2 ? parts[1] : "";
  if (v == "1" || v == "true" || v == "on") {
    SetWarmupInfiniteAmmoEnabled(true);
    PrintLine("warmup infinite ammo: 1");
    return true;
  }
  if (v == "0" || v == "false" || v == "off") {
    SetWarmupInfiniteAmmoEnabled(false);
    PrintLine("warmup infinite ammo: 0");
    return true;
  }
  PrintLine("Usage: ru_warmup_infinite_ammo 0|1");
  return true;
}

}  // namespace

bool LoadMapEntry(const std::string& entry) {
  const std::string cmd = mapnames::LoadCommand(entry);
  if (cmd.empty()) {
    Print("match: map \"%s\" is not a valid map name or workshop id; not loading it\n", entry.c_str());
    return false;
  }
  mapnames::MapRef ref;
  if (mapnames::ParseEntry(entry, &ref) && !ref.workshop_id.empty()) mapnames::NoteWorkshopLoad(ref.workshop_id);
  return EnqueueServerCommand(cmd.c_str());
}

std::string LicenseStoppedReason() {
  const ru_api* api = host::Api();
  char buf[512];
  if (RU_API_HAS(api, license_stopped) && api->license_stopped && api->license_stopped(api->self, buf, sizeof(buf)) > 0) {
    return buf;
  }
  return {};
}

bool ApplyLoadedMatch(const WebhookMatchContext& loaded, const std::string& configJson, int firstMapNumber) {
  if (const std::string why = LicenseStoppedReason(); !why.empty()) {
    PrintLine(why.c_str());
    return false;
  }
  if (firstMapNumber < 1) firstMapNumber = 1;
  // Maps the config gives no side: knife or team1_ct by knife_enabled_default (match_settings.h),
  // fixed here so a knife pick (WebhookUpdateMapSide) has an entry to update.
  WebhookMatchContext ctx = loaded;
  match_settings::FillMissingSides(&ctx);
  WebhookStartSenderThread();
  if (auto prev = WebhookGetMatchContext()) {
    if (prev->matchid != 0 && prev->matchid != ctx.matchid) {
      // Best-effort close out previous series when a new match is loaded.
      WebhookEmitSeriesEnd(/*team1_series_score=*/0, /*team2_series_score=*/0, /*winner=*/"none", /*time_until_restore=*/0);
    }
  }
  WebhookSetMatchContext(ctx);
  OnMatchLoaded();
  EsportsOnMatchLoaded(ctx);
  WebhookEmitSeriesStart();

  // Persist match config so a rebooted server can restore without MAT re-init.
  readyup::persisted_match_state::PersistActiveMatchJson(configJson);

  // Human matches generally should not start with CS2 auto-spawned bots.
  // Some server configs/gamemode cfgs will create fill bots when empty.
  // Kick any existing bots and set bot_quota=0 as a best-effort mitigation.
  // (These commands are safe no-ops if bots are disabled.)
  (void)EnqueueServerCommand("bot_kick");
  (void)EnqueueServerCommand("bot_quota 0");

  // CS2 round backups for recovery: the file prefix now, written from the go-live on
  // (round_restore::EnableRoundBackups), never in warmup or the knife round.
  // Prefix includes matchid and map number to avoid collisions.
  {
    const int mapNumber = firstMapNumber;
    const std::string prefix =
        "readyup_backup_" + std::to_string(static_cast<unsigned long long>(ctx.matchid)) +
        "_map" + std::to_string(mapNumber) + "_";
    (void)EnqueueServerCommand("mp_backup_round_auto 0");
    (void)EnqueueServerCommand("mp_backup_restore_load_autopause 1");
    const std::string cmd = "mp_backup_round_file " + prefix;
    (void)EnqueueServerCommand(cmd.c_str());
    readyup::persisted_match_state::PersistBackupPrefix(prefix);
  }

  // Change to the first map (map 1, or the map a failover resumes), also when the server is
  // already on it: a fresh map load resets the engine clock (after a day or more on one map
  // player animations run in slow motion, idle_refresh.h).
  const size_t idx = static_cast<size_t>(firstMapNumber - 1);
  // The new match is on map N from here: the map number of a previous match must not carry over.
  {
    MatchLogState ls = MatchLogSnapshot();
    ls.mapNumber = firstMapNumber;
    MatchLogRestore(ls);
    MatchStateSetMap(firstMapNumber, MatchStateGet().current_map);
  }
  // GOTV only records when tv_enable was 1 when the map loaded (the recorder's own tv_enable 1 at
  // go-live comes too late: "Only TV Master can record demos instantly", no file). Turn it on
  // before the match's first map load when this match records (rules.demo.record, else
  // ru_demo_recording_enabled). Later maps load with it still on.
  if (ctx.demo_record == 1 || (ctx.demo_record != 0 && demo::Get().recordingEnabled)) {
    (void)EnqueueServerCommand("tv_enable 1");
  }
  if (idx < ctx.maplist.size()) {
    // CS2 reads game_type / game_mode at the map load: competitive, or wingman (wingman.h).
    for (const auto& c : wingman::GameModeCommands(ctx.wingman)) (void)EnqueueServerCommand(c.c_str());
    if (ctx.wingman) PrintLine("match: wingman - game_type 0 / game_mode 2 for the map load");
    ModesAwaitMapLoad();  // the server's map and score until then
    (void)LoadMapEntry(ctx.maplist[idx]);
  }
  return true;
}

namespace {

bool LoadMatchFromUrlImpl(const std::string& url) {
  if (url.empty()) return false;
  const auto token = readyup::GetMatchTokenCopy();
  const bool auth = token.has_value();

  const unsigned long long reqId = g_matchReqId.fetch_add(1, std::memory_order_relaxed) + 1ull;
  Print("match-load[%llu]: GET %s (auth=%s)\n",
        reqId,
        url.c_str(),
        auth ? "on" : "off");

  // NOTE: This is currently synchronous on the server thread. We keep timeouts short in HttpGet().
  const HttpResponse r = HttpGet(url, token);
  if (!r.error.empty()) {
    Print("match-load[%llu]: error: %s (status=%ld)\n", reqId, r.error.c_str(), r.status);
    return true;
  }

  Print("match-load[%llu]: status=%ld bytes=%zu\n", reqId, r.status, r.body.size());

  bool truncated = false;
  const std::string body = TruncateForPrint(r.body, /*maxBytes=*/16384, truncated);
  const std::string safeBody = StripCrLf(body);

  if (truncated) {
    Print("match-load[%llu]: body (first %zu bytes, truncated):\n%s\n", reqId, safeBody.size(), safeBody.c_str());
  } else {
    Print("match-load[%llu]: body:\n%s\n", reqId, safeBody.c_str());
  }

  // Try parse match config and seed webhook context for MAT.
  {
    std::string parseErr;
    auto ctx = ParseMatchContextFromJson(r.body, &parseErr);
    if (!ctx) {
      // Not a match config, or refused (a bad ruleset / override, a knife side under valve).
      Print("match-load[%llu]: error: match config not loaded: %s\n", reqId,
            parseErr.empty() ? "json parse failed" : parseErr.c_str());
    } else {
      ApplyLoadedMatch(*ctx, r.body);

      // Print a concise parsed summary (the raw JSON is already printed above).
      Print("match-load[%llu]: parsed: teams=\"%s\" vs \"%s\" roster=%zu map1=%s maxRounds=%d ot=%s seg=%d maxOT=%d dmgTiebreak=%s suddenDeath=%s\n",
            reqId,
            ctx->team1_name.c_str(),
            ctx->team2_name.c_str(),
            ctx->roster_team.size(),
            ctx->maplist.empty() ? "(none)" : ctx->maplist[0].c_str(),
            ctx->maxRounds,
            ctx->overtime_enabled ? "on" : "off",
            ctx->overtimeSegments,
            ctx->maxOvertimes,
            ctx->damageTiebreakEnabled ? "on" : "off",
            ctx->suddenDeathOnDamageTie ? "on" : "off");

      Print("match-load[%llu]: match context set: matchid=%llu slug=%s\n",
            reqId,
            static_cast<unsigned long long>(ctx->matchid),
            ctx->slug.empty() ? "(none)" : ctx->slug.c_str());
    }
  }
  return true;
}

}  // namespace

const std::vector<std::string>& MatchConsoleCommands() {
  static const std::vector<std::string> k = {
      "ru_match_token", "ru_webhook_url", "ru_heartbeat_url", "ru_admins_url", "ru_admins_refresh_seconds",
      "ru_cfg_exec_enable", "ru_warmup_enable", "ru_dev_bots_scrim", "ru_warmup_message_html", "ru_warmup_respawn",
      "ru_warmup_ignore_win_conditions", "ru_warmup_roundtime_minutes", "ru_warmup_startmoney", "ru_warmup_maxmoney",
      "ru_warmup_buy_anywhere", "ru_warmup_infinite_ammo",
      // match_end.h / demo_recorder.h
      "ru_demo_recording_enabled", "ru_demo_path", "ru_demo_name_format", "ru_demo_upload_url",
      "ru_demo_upload_method", "ru_demo_upload_header", "ru_demo_upload_headers_clear", "ru_demo_upload_attempts",
      "ru_demo_upload_header_key", "ru_demo_upload_header_value", "get5_demo_upload_header_key",
      "get5_demo_upload_header_value",
      "ru_demo_status", "ru_series_end_kick_delay_no_demo", "ru_series_end_kick_delay_demo_no_upload",
      "ru_series_end_kick_delay_demo_upload", "ru_match_stats",
      // round_restore.h
      "ru_pause_after_restore", "ru_listbackups", "ru_loadbackup"};
  static const std::vector<std::string> all = [] {
    std::vector<std::string> v = k;
    for (const auto& c : match_settings::ConsoleCommands()) v.push_back(c);  // ru_<server setting>
    return v;
  }();
  return all;
}

namespace {
bool RunConsoleCommand(const std::string& line) {
  if (HandleMatchTokenCommand(line)) return true;
  if (HandleWebhookUrlCommand(line)) return true;
  if (HandleHeartbeatUrlCommand(line)) return true;
  if (HandleAdminsUrlCommand(line)) return true;
  if (HandleAdminsRefreshSecondsCommand(line)) return true;
  if (HandleCfgExecEnableCommand(line)) return true;
  if (HandleWarmupEnableCommand(line)) return true;
  if (HandleDevBotsScrimCommand(line)) return true;
  if (HandleWarmupMessageHtmlCommand(line)) return true;
  if (HandleWarmupRespawnCommand(line)) return true;
  if (HandleWarmupIgnoreWinCommand(line)) return true;
  if (HandleWarmupRoundTimeMinutesCommand(line)) return true;
  if (HandleWarmupStartMoneyCommand(line)) return true;
  if (HandleWarmupMaxMoneyCommand(line)) return true;
  if (HandleWarmupBuyAnywhereCommand(line)) return true;
  if (HandleWarmupInfiniteAmmoCommand(line)) return true;
  if (round_restore::HandleConsoleLine(Trim(line))) return true;
  // Demo recording/upload, series-end kick delays, ru_match_stats, tv_delay (match_end.h).
  return MatchFlowHandleConsoleLine(Trim(line));
}
}  // namespace

bool MatchConsoleCommand(const std::string& line) {
  // Server settings (server_settings.h): ru_minimum_ready_required, ru_playout_enabled_default, ...
  if (match_settings::ConsoleCommand(line)) return true;
  // ru_warmup_* / ru_cfg_exec_enable / ru_demo_* / kick delays: saved in state.json (and
  // `<setting> default`), see persisted_settings.h.
  bool consumed = false;
  if (persisted_settings::ConsoleSetting(Trim(line), &RunConsoleCommand, &consumed)) return consumed;
  return RunConsoleCommand(line);
}

bool LoadMatchFromUrl(const std::string& url) { return LoadMatchFromUrlImpl(url); }

}  // namespace readyup

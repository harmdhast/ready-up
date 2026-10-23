#include "readyup/modes.h"

#include "readyup/coach.h"
#include "readyup/scrim_flow.h"

#include "readyup/map_names.h"
#include "readyup/demo_recorder.h"
#include "readyup/match_end.h"
#include "readyup/match_stats.h"
#include "readyup/ready_hud.h"

#include "readyup/engine.h"
#include "readyup/esports.h"
#include "readyup/golive_card.h"
#include "readyup/welcome.h"
#include "readyup/config.h"
#include "readyup/cvar_snapshot.h"
#include "readyup/match_events.h"
#include "readyup/match_features.h"
#include "readyup/match_recovery.h"
#include "readyup/match_signals.h"
#include "readyup/game_timers.h"
#include "readyup/knife_tracker.h"
#include "readyup/logging.h"
#include "readyup/match_rules.h"
#include "readyup/match_settings.h"
#include "readyup/match_state.h"
#include "readyup/pause_state.h"
#include "readyup/admin_check.h"
#include "readyup/mat_admins.h"
#include "readyup/persisted_match_state.h"
#include "readyup/players.h"
#include "readyup/round_restore.h"
#include "readyup/simulation.h"
#include "readyup/status_snapshot.h"
#include "readyup/weapon_cleanup.h"
#include "readyup/webhook.h"
#include "readyup/wingman.h"
#include "readyup/host.h"
#include "readyup/practice_iface.h"

#include <atomic>
#include <chrono>
#include <cctype>
#include <cstring>
#include <random>
#include <unordered_set>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>


namespace {
// readyup.practice.v1 dry_run: a .dryrun round is under way (the practice plugin).
bool PracticeDryRun() {
  const ru_api* a = readyup::host::Api();
  if (!a) return false;
  const auto* p = static_cast<const ru_practice_v1*>(a->get_interface(a->self, RU_PRACTICE_IFACE_NAME, 1));
  return p && RU_API_HAS(p, dry_run) && p->dry_run && p->dry_run() == 1;
}
}  // namespace

namespace readyup {
namespace {

struct State {
  std::mutex mu;
  // Boot in idle. With no match loaded, the scrim flow (scrim_flow.cpp) moves
  // idle -> scrim_warmup as soon as a human joins CT/T; a match load moves
  // straight to match_warmup.
  ReadyUpMode mode = ReadyUpMode::Idle;

  bool warmupEnabled = true;
  std::string warmupHtml;
  // Optional: exec mode cfg files (MatchZy-style).
  // Default OFF so Ready Up uses its built-in (custom) warmup that doesn't rely
  // on CS2's engine warmup and can suppress round termination.
  bool cfgExecEnabled = false;
  // Run once per idle entry (avoid spamming `exec` every tick).
  bool idleCfgExecuted = false;
  // Idle as a waiting room (scrim_auto on): rounds cannot end, so the first player taking a bot's
  // place does not hand the other side a round before scrim warmup starts. -1 = not sent yet.
  int idleIgnoreWinSent = -1;
  bool warmupRespawn = true;
  bool warmupIgnoreWin = true;
  int warmupRoundTimeMinutes = 60;
  int warmupStartMoney = 16000;
  int warmupMaxMoney = 16000;
  bool warmupBuyAnywhere = true;
  bool warmupInfiniteAmmo = true;

  // steamid64 -> ready
  std::unordered_map<uint64_t, bool> ready;

  // rate limit: steamid64 -> last send time
  std::unordered_map<uint64_t, std::chrono::steady_clock::time_point> lastUi;

  // rate limit: steamid64 -> last kick time (whitelist enforcement)
  std::unordered_map<uint64_t, std::chrono::steady_clock::time_point> lastKick;

  // rate limit: steamid64 -> last forced jointeam time (roster enforcement)
  std::unordered_map<uint64_t, std::chrono::steady_clock::time_point> lastForceJoin;

  // One-time / rate-limited user-facing chat prompts.
  bool matchLoadedChatSent = false;
  std::chrono::steady_clock::time_point lastNotReadyChat{};

  // Track current map so cfg baselines are re-executed on map changes (MatchZy behavior).
  std::string lastSeenMap;
  // A map load was logged (ModesOnMapLoaded): also a reload of lastSeenMap.
  bool mapLoaded = false;

  // The map of this warmup is loaded: false from a match load or next-map warmup until its map
  // change or its first round start shows up (warmup_score.h).
  bool warmupMapReady = true;
  // The mode was match_live at the last Tick (round backups are turned off when it leaves).
  bool wasLive = false;

  // Best-effort warmup rules + transition into live.
  bool warmupRulesApplied = false;
  bool startTriggered = false;
  // A force start is live at once, so the go-live round start below never ends its win-condition
  // hold: the first live round start after its restart does.
  bool forceStartWinHold = false;
  std::chrono::steady_clock::time_point lastGateCmd{};

  // Boot-time recovery gate (pause + wait for ready) for mid-match restart.
  bool recoveryGate = false;
  bool fullRosterGate = false;  // SetFullRosterGate

  // Lifecycle webhook idempotency.
  int lifecycleMapNumber = 0;
  bool warmupEndedSent = false;
  bool goingLiveSent = false;

  // External mode (SetModeExternal): the owning plugin mode's name.
  std::string externalName;
  // An external mode was on since the last match load: it may have changed game_type /
  // game_mode (the deathmatch plugin does), so the next match load puts competitive back.
  bool externalTouchedGameMode = false;
  // A wingman match load switched CS2 to game_mode 2: back to competitive once the server is idle
  // (it takes effect on the next map load).
  bool wingmanGameMode = false;

  // Practice mode rules.
  bool practiceRulesApplied = false;
  bool practiceResetPending = false;

  // Demo recording (per map).
  bool demoRecording = false;
  int demoMapNumber = 0;
  std::string demoName;

  // Map result idempotency.
  int mapResultEmittedForMapNumber = 0;

  // Series score (maps won), maintained by RU while match is loaded.
  int seriesWinsTeam1 = 0;
  int seriesWinsTeam2 = 0;

  // Knife decider state (per map).
  int knifeMapNumber = 0;
  bool knifeRulesApplied = false;
  bool knifeStartedSent = false;
  bool knifeEndedSent = false;
  WebhookTeam knifeWinner = WebhookTeam::Unknown;
  bool knifeAwaitingPick = false;
  std::chrono::steady_clock::time_point knifePickDeadline{};
  KnifePhase knifePhase = KnifePhase::None;
  std::chrono::steady_clock::time_point knifeTriggeredAt{};
  int knifeWinnerCs = 0;
  std::string knifeReasonShort;
  bool knifeReminderSent = false;
  // mp_logdetail 3 was set for the knife round (reset to 0 afterwards).
  bool knifeLogdetailSet = false;

  // Admin-set `ru_warmup_message_html` (shown as a footer in the ready HUD).
  bool warmupHtmlCustom = false;

  // Last time CS2's native warmup was ended in reaction to Warmup_Start.
  std::chrono::steady_clock::time_point lastNativeWarmupEnd{};
};

State& St() {
  static State st;
  if (st.warmupHtml.empty()) {
    st.warmupHtml = "<b><font color='yellow'>Ready Up</font></b><br>"
                    "You are not ready yet.<br>"
                    "Type <b>.r</b> to ready up.";
  }
  return st;
}

static const char* ModeToString(ReadyUpMode m) {
  switch (m) {
    case ReadyUpMode::Idle: return "idle";
    case ReadyUpMode::Practice: return "practice";
    case ReadyUpMode::MatchWarmup: return "match_warmup";
    case ReadyUpMode::MatchKnife: return "match_knife";
    case ReadyUpMode::MatchLive: return "match_live";
    case ReadyUpMode::Postgame: return "postgame";
    case ReadyUpMode::ScrimWarmup: return "scrim_warmup";
    case ReadyUpMode::External: return "external";
    default: return "unknown";
  }
}

static bool IsKnifeForMap(const WebhookMatchContext& ctx, int mapNumber) {
  if (mapNumber <= 0) return false;
  const size_t idx = static_cast<size_t>(mapNumber - 1);
  if (idx >= ctx.map_sides.size()) return false;
  if (ctx.map_sides[idx] != "knife") return false;
  // Knife feature off (log listener / command buffer missing): the map goes live with default
  // sides instead of waiting in warmup for a knife round that cannot be run.
  return FeatureEnabled(Feature::Knife);
}

static void ResetKnifeStateForMapLocked(State& st, int mapNumber) {
  st.knifeMapNumber = mapNumber;
  st.knifeRulesApplied = false;
  st.knifeStartedSent = false;
  st.knifeEndedSent = false;
  st.knifeWinner = WebhookTeam::Unknown;
  st.knifeAwaitingPick = false;
  st.knifePickDeadline = {};
  st.knifePhase = KnifePhase::None;
  st.knifeTriggeredAt = {};
  st.knifeWinnerCs = 0;
  st.knifeReasonShort.clear();
  st.knifeReminderSent = false;
  KnifeTrackerReset(/*active=*/false);
  if (st.knifeLogdetailSet) {
    (void)EnqueueServerCommand("mp_logdetail 0");
    st.knifeLogdetailSet = false;
  }
}

static const char* CsSideName(int cs) {
  return cs == 3 ? "CT" : cs == 2 ? "T" : "?";
}

// Knife winner display name: team name for real matches, the side for scrims.
static std::string KnifeWinnerNameLocked(const State& st, const WebhookMatchContext& ctx) {
  if (ctx.slug == "scrim") return CsSideName(st.knifeWinnerCs);
  const std::string& n = (st.knifeWinner == WebhookTeam::Team2) ? ctx.team2_name : ctx.team1_name;
  return n.empty() ? std::string(CsSideName(st.knifeWinnerCs)) : n;
}

// Forward declarations (defined later in this file).
static void ApplyWarmupRulesLocked(State& st);
static bool AllRosterReadyAndConnectedLocked(State& st, const WebhookMatchContext& ctx);
static void ApplyMatchCvarsLocked(const WebhookMatchContext& ctx, bool followSides = false);
static void FinishMapLocked(State& st, const WebhookMatchContext& ctx, int map_number, const std::string& map,
                            int team1_score, int team2_score, const char* winner, bool forfeit);

// Knife-round rules. knife.cfg is the baseline; the overrides undo what
// Ready Up's (emulated) warmup set and knife.cfg does not touch, so the round
// can actually end and nobody keeps a gun. mp_logdetail 3 adds `attacked`
// log lines (with the victim's remaining health) for the time-out tiebreak.
// CS2 runs an `exec`ed cfg's lines after the commands already queued behind the exec,
// so a cvar enqueued right after `exec ReadyUp/live.cfg` is overwritten by the cfg.
// Commands that must win over a cfg go out this much later (well before the
// mp_restartgame 1 that follows every cfg exec here).
constexpr double kAfterCfgSeconds = 0.25;

static void EnqueueAfterCfg(std::vector<std::string> cmds) {
  if (cmds.empty()) return;
  ScheduleOnGameThread(kAfterCfgSeconds, [cmds = std::move(cmds)]() {
    for (const auto& c : cmds) (void)EnqueueServerCommand(c.c_str());
  });
}

// The go-live window (mode still match warmup, mp_restartgame pending) drops CCSGameRules::TerminateRound:
// a win condition that holds (a team with nobody on it) would credit a round on every tick and the
// restart's halftime check would swap the sides. Win conditions are ignored until the live round starts.
static void HoldWinConditionsForGoLive() { EnqueueAfterCfg({"mp_ignore_round_win_conditions 1"}); }

static bool AnyHumanOnCtOrT() {
  for (const auto& h : ListHumans()) {
    if (h.team == 2 || h.team == 3) return true;
  }
  return false;
}

static void ApplyKnifeRulesLocked(State& st) {
  bool any = false;
  if (EnqueueServerCommand("exec ReadyUp/knife.cfg")) any = true;
  // Dev flags (dev_bots_ready / dev_bots_scrim) or a simulated match with nobody human on CT/T:
  // bots do not knife each other, so the round would run knife.cfg's full 1:55 to a time-out.
  // Cut it to 30s. live.cfg (and match cvars) set the real round time before going live.
  const bool simulated = SimulationActive();
  if ((DevBotsReadyEnabled() || DevBotsScrimEnabled() || simulated) && !AnyHumanOnCtOrT()) {
    EnqueueAfterCfg({"mp_roundtime 0.5", "mp_roundtime_defuse 0.5", "mp_roundtime_hostage 0.5"});
    PrintLine(simulated ? "knife: simulation and no humans on CT/T - knife round time 0.5 min (bots do not knife)."
                        : "knife: dev flag on and no humans on CT/T - knife round time 0.5 min (bots do not knife).");
  }
  const char* cmds[] = {
      "mp_warmup_pausetimer 0",
      "mp_ignore_round_win_conditions 0",
      "mp_respawn_on_death_ct 0",
      "mp_respawn_on_death_t 0",
      "mp_buy_anywhere 0",
      "mp_buytime 0",
      "mp_startmoney 0",
      "mp_maxmoney 0",
      "mp_give_player_c4 0",
      "mp_ct_default_primary \"\"",
      "mp_t_default_primary \"\"",
      "mp_ct_default_secondary \"\"",
      "mp_t_default_secondary \"\"",
      "mp_ct_default_grenades \"\"",
      "mp_t_default_grenades \"\"",
      "mp_ct_default_melee weapon_knife",
      "mp_t_default_melee weapon_knife",
      "mp_team_intro_time 0",
      "mp_logdetail 3",
      "mp_warmup_end",
      "mp_restartgame 1",
  };
  for (const char* c : cmds) {
    if (EnqueueServerCommand(c)) any = true;
  }
  if (any) {
    st.knifeRulesApplied = true;
    st.knifeLogdetailSet = true;
  }
}

static bool StartKnifeLocked(State& st, const WebhookMatchContext& ctx) {
  if (st.mode != ReadyUpMode::MatchWarmup) return false;
  if (!FeatureEnabled(Feature::Knife)) return false;  // callers go live without a knife round
  const auto ms = MatchStateGet();
  const int mapNumber = ms.map_number <= 0 ? 1 : ms.map_number;
  ResetKnifeStateForMapLocked(st, mapNumber);

  const auto now = std::chrono::steady_clock::now();
  st.mode = ReadyUpMode::MatchKnife;
  st.startTriggered = false;
  st.lastUi.clear();
  st.lastGateCmd = now;
  ApplyKnifeRulesLocked(st);
  if (!st.knifeRulesApplied) {
    // Command buffer not ready; stay in warmup and retry on a later tick.
    st.mode = ReadyUpMode::MatchWarmup;
    return false;
  }
  st.knifePhase = KnifePhase::Starting;
  st.knifeTriggeredAt = now;
  WebhookSetHeartbeatStatus("warmup");
  Print("knife: starting knife round (map %d, match %s) - exec knife.cfg + mp_restartgame 1\n", mapNumber,
        ctx.slug.empty() ? "?" : ctx.slug.c_str());
  if (!HudReplacesChat()) SendToChat("Ready Up: KNIFE ROUND after the restart - knives only, the winning team picks its side.");
  return true;
}

static void MaybeEnterKnifeModeLocked(State& st) {
  if (st.mode != ReadyUpMode::MatchWarmup) return;
  if (!st.warmupEnabled || st.recoveryGate || st.startTriggered) return;
  // Warmup rules first (applied by MaybeGateMatchLocked on an earlier tick).
  if (!st.warmupRulesApplied) return;
  auto ctxOpt = WebhookGetMatchContext();
  if (!ctxOpt) return;
  const auto& ctx = *ctxOpt;
  // (Scrims normally start it right away via StartKnifeRound(); this path
  // covers real matches and a knife round interrupted by a map change.)
  const auto ms = MatchStateGet();
  const int mapNumber = ms.map_number <= 0 ? 1 : ms.map_number;
  if (!IsKnifeForMap(ctx, mapNumber)) return;

  const auto now = std::chrono::steady_clock::now();
  if (st.lastGateCmd.time_since_epoch().count() != 0 && (now - st.lastGateCmd) < std::chrono::milliseconds(800)) return;
  if (!AllRosterReadyAndConnectedLocked(st, ctx)) return;
  // valve: GOTV must be up before the match starts (esports.h). Asked again on the next gate.
  if (!EsportsGoLiveAllowed(/*forced=*/false)) {
    st.lastGateCmd = now;
    return;
  }
  (void)StartKnifeLocked(st, ctx);
}

static bool ApplyKnifeSideChoiceLocked(State& st,
                                      const WebhookMatchContext& ctx,
                                      int mapNumber,
                                      const std::string& choice,
                                      uint64_t pickerSteamid64,
                                      const std::string& pickerName);

static void MaybeAutoPickKnifeSideLocked(State& st) {
  if (st.mode != ReadyUpMode::MatchKnife) return;
  if (!st.knifeAwaitingPick) return;
  if (st.knifeWinner == WebhookTeam::Unknown) return;
  if (st.knifePickDeadline.time_since_epoch().count() == 0) return;
  const auto now = std::chrono::steady_clock::now();

  auto ctxOpt = WebhookGetMatchContext();
  if (!ctxOpt) return;
  const auto& ctx = *ctxOpt;

  if (now < st.knifePickDeadline) {
    const auto left = std::chrono::duration_cast<std::chrono::seconds>(st.knifePickDeadline - now).count();
    if (!st.knifeReminderSent && left <= 15 && left >= 5) {
      st.knifeReminderSent = true;
      if (!HudReplacesChat()) SendToChat(("Ready Up: " + KnifeWinnerNameLocked(st, ctx) + ": .stay or .switch - " + std::to_string(left + 1) +
                  "s left, then sides stay.")
                     .c_str());
    }
    return;
  }

  const auto ms = MatchStateGet();
  const int mapNumber = ms.map_number <= 0 ? 1 : ms.map_number;
  // No decision in time: keep the current sides.
  Print("knife: pick window expired -> stay\n");
  (void)ApplyKnifeSideChoiceLocked(st, ctx, mapNumber, "stay", /*pickerSteamid64=*/0, /*pickerName=*/"timeout");
}

static void MaybeForceRosterTeamsLocked(State& st) {
  if (st.mode != ReadyUpMode::MatchWarmup && st.mode != ReadyUpMode::MatchKnife) return;
  auto ctxOpt = WebhookGetMatchContext();
  if (!ctxOpt) return;
  const auto& ctx = *ctxOpt;
  if (ctx.roster_team.empty() && ctx.spectators.empty()) return;
  // Scrims are built from the current teams; never force-swap anyone.
  if (ctx.slug == "scrim") return;

  const auto ms = MatchStateGet();
  const int mapNum = ms.map_number <= 0 ? 1 : ms.map_number;

  // The map's start side (knife maps: team1 = CT until decided), flipped by the swap count: a
  // resume warmup is on the resumed round's sides. The count is read lock-free (match_events.h).
  const bool team1IsCt = Team1IsCtWithSwapCount(mapNum, MatchEventsSwapCountFor(mapNum));
  const auto now = std::chrono::steady_clock::now();
  const auto minInterval = std::chrono::milliseconds(900);

  // Roster players go to their team's side for this map (team lock). Only players on the wrong
  // side (or none yet) are moved: a change of team kills a live player, so someone already in
  // place is never touched. Others are left to the whitelist.
  for (const auto& h : ListHumans()) {
    if (h.steamid64 == 0 || h.slot < 0) continue;

    int join = 1;  // spectators
    if (ctx.spectators.find(h.steamid64) == ctx.spectators.end()) {
      auto it = ctx.roster_team.find(h.steamid64);
      if (it == ctx.roster_team.end()) continue;  // not on the roster: the whitelist handles them
      if (it->second == WebhookTeam::Team1) join = team1IsCt ? 3 : 2;       // CT : T
      else if (it->second == WebhookTeam::Team2) join = team1IsCt ? 2 : 3;  // T : CT
      else continue;
    }
    if (h.team == join) continue;

    auto lastIt = st.lastForceJoin.find(h.steamid64);
    if (lastIt != st.lastForceJoin.end() && (now - lastIt->second) < minInterval) continue;

    // Not here: ChangeTeam fires the engine's team/death callbacks synchronously, and those
    // take Ready Up's locks, which this tick holds, so a move from inside it froze the game
    // thread (a live test hung on a map change). The move runs on the next frame, outside
    // every lock; the throttle keeps it to one per player per interval.
    st.lastForceJoin[h.steamid64] = now;
    ScheduleOnGameThread(0.0, [slot = h.slot, steamid = h.steamid64, join, name = h.name]() {
      // The slot may have changed hands in between: only move the same player.
      bool same = false;
      for (const auto& x : ListHumans()) {
        if (x.slot == slot) {
          same = x.steamid64 == steamid && x.team != join;
          break;
        }
      }
      if (!same || !ForceJoinTeamForSlot(slot, join)) return;
      // ChangeTeam fires player_team synchronously, so the registry already shows a move that
      // landed. One the engine refused (a full team) is retried every interval: report it once.
      static std::unordered_set<uint64_t> s_refusalReported;
      const char* side = join == 3 ? "CT" : join == 2 ? "T" : "spectators";
      for (const auto& x : ListHumans()) {
        if (x.slot != slot || x.steamid64 != steamid) continue;
        if (x.team == join) {
          s_refusalReported.erase(steamid);
          Print("ready: moved %s to %s (their team's side this map)\n", name.c_str(), side);
          // Only a move that landed: a refused one is retried every interval and must not restart
          // the warmup round each time.
          ModesOnHumanJoinedTeam(slot, steamid, join);
        } else if (s_refusalReported.insert(steamid).second) {
          Print("ready: could not move %s to %s (still trying)\n", name.c_str(), side);
        }
        return;
      }
    });
  }
}

static bool ApplyKnifeSideChoiceLocked(State& st,
                                      const WebhookMatchContext& ctx,
                                      int mapNumber,
                                      const std::string& choice,
                                      uint64_t pickerSteamid64,
                                      const std::string& pickerName) {
  if (st.knifeWinner == WebhookTeam::Unknown) return false;
  if (!st.knifeAwaitingPick) return false;

  auto lower = [](std::string s) {
    for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
  };

  const std::string c = lower(choice);
  const bool winnerIsTeam1 = (st.knifeWinner == WebhookTeam::Team1);

  // Interpret choice as the side the knife winner wants to START on.
  std::optional<bool> winnerWantsCt;
  if (c == "ct") winnerWantsCt = true;
  else if (c == "t") winnerWantsCt = false;
  else if (c == "stay") winnerWantsCt = winnerIsTeam1;          // team1 starts CT in knife
  else if (c == "switch") winnerWantsCt = !winnerIsTeam1;
  else return false;

  // Determine whether team1 should start CT.
  const bool team1StartsCt = (*winnerWantsCt) ? winnerIsTeam1 : (!winnerIsTeam1);
  const char* mapSide = team1StartsCt ? "team1_ct" : "team2_ct";

  // Persist: update stored match context so jointeam enforcement + WinnerToTeamString() work.
  (void)WebhookUpdateMapSide(mapNumber, mapSide);

  // If team1 is not starting CT, swap teams once to reflect the choice immediately.
  if (!team1StartsCt) {
    (void)EnqueueServerCommand("mp_swapteams");
  }

  // Emit MatchZy-style side pick event.
  const MatchStateSnapshot ms = MatchStateGet();
  const std::string mapName = ms.current_map;
  const char* teamStr = winnerIsTeam1 ? "team1" : "team2";
  const char* sideStr = (*winnerWantsCt) ? "ct" : "t";
  const std::string by = !pickerName.empty() ? pickerName : (pickerSteamid64 ? std::to_string(pickerSteamid64) : "server");
  WebhookEmitSidePicked(mapNumber, mapName.c_str(), sideStr, by.c_str(), teamStr);
  if (signals::Enabled()) {
    status::Json d = status::Json::Object();
    d["team"] = teamStr;
    d["side"] = sideStr;
    d["picked_by"] = pickerSteamid64 ? std::to_string(pickerSteamid64)
                                     : (pickerName == "timeout" ? std::string("timeout") : std::string("console"));
    signals::Emit("side_picked", std::move(d), -1, mapNumber);
  }

  // Straight to live (everyone readied up before the knife round): live.cfg is
  // the baseline (it undoes knife.cfg), then match cvars, then a clean restart.
  // Mode goes back to match_warmup with go-live pending; the next Round_Start
  // flips it to match_live (OnMatchRoundStarted).
  const std::string winnerName = KnifeWinnerNameLocked(st, ctx);
  const bool stayed = (team1StartsCt == true);  // knife round had team1 on CT
  ResetKnifeStateForMapLocked(st, mapNumber);    // also resets mp_logdetail
  st.mode = ReadyUpMode::MatchWarmup;
  st.lastUi.clear();
  st.warmupRulesApplied = true;   // don't re-apply warmup rules
  st.matchLoadedChatSent = true;  // no "type .r" prompt
  bool any = false;
  if (EnqueueServerCommand(LiveCfgExecCommand().c_str())) any = true;
  ApplyMatchCvarsLocked(ctx);
  if (EnqueueServerCommand("mp_warmup_pausetimer 0")) any = true;
  if (EnqueueServerCommand("mp_warmup_end")) any = true;
  round_restore::EnableRoundBackups(ctx.matchid, std::max(1, mapNumber));
  HoldWinConditionsForGoLive();
  if (EnqueueServerCommand("mp_restartgame 1")) any = true;
  st.startTriggered = any;
  st.lastGateCmd = std::chrono::steady_clock::now();
  Print("knife: side picked by %s: %s (%s) -> %s; exec live.cfg + mp_restartgame 1\n", by.c_str(), c.c_str(),
        winnerName.c_str(), stayed ? "stay" : "mp_swapteams");

  // Let players know.
  std::string msg = "Ready Up: " + winnerName + " " + (pickerName == "timeout" ? "(no pick in time) " : "") +
                    "start on " + ((*winnerWantsCt) ? "CT" : "T") +
                    (stayed ? " (sides stay)" : " (teams swapped)") + " - LIVE after the restart.";
  SendToChat(msg.c_str());
  return true;
}

static void MaybeShowWarmupUiLocked(State& st) {
  if (!st.warmupEnabled) return;
  if (st.mode != ReadyUpMode::MatchWarmup) return;

  auto ctx = WebhookGetMatchContext();
  if (!ctx) return;

  const auto now = std::chrono::steady_clock::now();
  const auto minInterval = std::chrono::milliseconds(1200);

  // One-time user-facing instruction.
  if (!st.matchLoadedChatSent) {
    SendToChat("Ready Up: match loaded. Type .r or .ready to ready up.");
    st.matchLoadedChatSent = true;
  }

  // Counts for banner tokens: per team, ready players up to a full team over the full team, so
  // substitutes never count toward the players the match waits for (match_rules.h TeamReadyTally).
  int rosterTotal = 0;
  int rosterReady = 0;
  int rosterConnected = 0;
  {
    std::unordered_set<uint64_t> connected;
    for (const auto& s : ListSlotIdentities()) {
      if (s.steamid64 != 0) connected.insert(s.steamid64);
    }
    int teamRoster[2] = {0, 0}, teamReady[2] = {0, 0};
    for (const auto& kv : ctx->roster_team) {
      const uint64_t sid = kv.first;
      if (sid == 0) continue;
      const int t = kv.second == WebhookTeam::Team2 ? 1 : 0;
      ++teamRoster[t];
      if (connected.find(sid) != connected.end()) rosterConnected++;
      auto it = st.ready.find(sid);
      if (it != st.ready.end() && it->second) ++teamReady[t];
    }
    for (int t = 0; t < 2; ++t) {
      const ReadyTally tally = TeamReadyTally(teamRoster[t], teamReady[t], ctx->players_per_team);
      rosterTotal += tally.total;
      rosterReady += tally.ready;
    }
  }

  // Periodic chat reminder listing who is not ready yet (cap for spam). Only
  // when the ready HUD is not on screen: the HUD is the ready-up UI.
  const auto chatInterval = std::chrono::seconds(12);
  if (!ReadyHudShowing() &&
      (st.lastNotReadyChat.time_since_epoch().count() == 0 || (now - st.lastNotReadyChat) >= chatInterval)) {
    std::string list;
    int count = 0;
    const int maxNames = 6;
    for (const auto& kv : ctx->roster_team) {
      const uint64_t sid = kv.first;
      if (sid == 0) continue;
      if (ctx->substitutes.count(sid)) continue;  // nobody waits for a substitute
      const bool ready = (st.ready.find(sid) != st.ready.end() && st.ready[sid]);
      if (ready) continue;
      std::string name;
      for (const auto& s : ListSlotIdentities()) {
        if (s.steamid64 == sid && !s.name.empty()) {
          name = s.name;
          break;
        }
      }
      if (name.empty()) name = std::to_string(static_cast<unsigned long long>(sid));
      if (!list.empty()) list += ", ";
      list += name;
      count += 1;
      if (count >= maxNames) break;
    }

    if (!list.empty() && rosterTotal > 0 && rosterReady < rosterTotal) {
      const std::string msg =
          "Ready Up: waiting for (" + std::to_string(rosterReady) + "/" + std::to_string(rosterTotal) +
          " ready): " + list;
      SendToChat(msg.c_str());
      st.lastNotReadyChat = now;
    }
  }

  // The center panel belongs to the ready HUD (ready_hud.cpp): per-player ready
  // list in scrim and match warmup. Only the chat reminder lives here.
  (void)rosterConnected;
  (void)now;
  (void)minInterval;
}

static void EnforceWhitelistLocked(State& st) {
  auto ctxOpt = WebhookGetMatchContext();
  if (!ctxOpt) return;
  const auto& ctx = *ctxOpt;
  // Pickup scrims are open: late joiners are not kicked.
  if (ctx.slug == "scrim") return;
  // whitelist_enabled_default / the match's `whitelist` (match_settings.h).
  if (!match_settings::WhitelistOn(ctx)) return;

  // Allow roster players and configured spectators only.
  std::unordered_set<uint64_t> allowed;
  allowed.reserve(ctx.roster_team.size() + ctx.spectators.size() + 8);
  for (const auto& kv : ctx.roster_team) allowed.insert(kv.first);
  for (const uint64_t sid : ctx.spectators) allowed.insert(sid);

  const auto now = std::chrono::steady_clock::now();
  const auto minKickInterval = std::chrono::seconds(3);

  // Match admins, MAT admins and the (cached) database admins; never waits on the database.
  auto isAdmin = [&](uint64_t steamid64) -> bool { return IsReadyUpAdmin(steamid64); };

  auto slots = ListSlotIdentities();
  std::unordered_set<uint64_t> seenSteam;
  for (const auto& s : slots) {
    if (s.steamid64 == 0) continue;
    if (s.slot < 0) continue;
    if (!seenSteam.insert(s.steamid64).second) continue;
    if (allowed.find(s.steamid64) != allowed.end()) continue;
    if (isAdmin(s.steamid64)) continue;
    if (CoachAllowedOnServer(s.steamid64)) continue;  // an admin made them a coach (coach.h)

    auto it = st.lastKick.find(s.steamid64);
    if (it != st.lastKick.end() && (now - it->second) < minKickInterval) continue;

    const std::string cmd =
        "kickid " + std::to_string(s.slot) + " \"Not whitelisted for this match\"";
    if (EnqueueServerCommand(cmd.c_str())) {
      st.lastKick[s.steamid64] = now;
    }
  }
}

static bool AllRosterReadyAndConnectedLocked(State& st, const WebhookMatchContext& ctx) {
  // min_spectators_to_ready: that many of the match's spectators (casters) typed .ready.
  if (const int needSpec = EffectiveRules().min_spectators_to_ready; needSpec > 0) {
    int readySpec = 0;
    for (const uint64_t sid : ctx.spectators) {
      auto it = st.ready.find(sid);
      if (it != st.ready.end() && it->second) ++readySpec;
    }
    if (!SpectatorsReadyToGoLive(needSpec, readySpec)) return false;
  }
  if (ctx.roster_team.empty()) return true;

  // Build connected set from observed slots.
  std::unordered_set<uint64_t> connected;
  for (const auto& s : ListSlotIdentities()) {
    if (s.steamid64 != 0) connected.insert(s.steamid64);
  }
  // Simulation: the roster players bots play as are there.
  SimulationAddConnected(&connected);

  // Each team has min_players_to_ready ready (0 = a full team: players_per_team, else 5; always a
  // full team after a failover or a crash recovery), and every
  // connected player a full team needs is ready; a connected substitute beyond it does not hold
  // the match up (match_rules.h TeamReadyToGoLive).
  const int minPlayers = ReadyMinPlayers(EffectiveRules().min_players_to_ready, st.fullRosterGate || st.recoveryGate);
  int roster[2] = {0, 0}, present[2] = {0, 0}, ready[2] = {0, 0};
  for (const auto& kv : ctx.roster_team) {
    const uint64_t sid = kv.first;
    if (sid == 0) continue;
    const int t = kv.second == WebhookTeam::Team2 ? 1 : 0;
    ++roster[t];
    if (connected.find(sid) == connected.end()) continue;
    ++present[t];
    auto it = st.ready.find(sid);
    if (it != st.ready.end() && it->second) ++ready[t];
  }
  for (int t = 0; t < 2; ++t) {
    if (!TeamReadyToGoLive(roster[t], present[t], ready[t], minPlayers, ctx.players_per_team)) return false;
  }
  return true;
}

static void ApplyWarmupRulesLocked(State& st) {
  // In MatchZy-compat mode, cfg files are authoritative.
  if (st.cfgExecEnabled) {
    bool any = false;
    if (EnqueueServerCommand("exec ReadyUp/warmup.cfg")) any = true;
    // Emulated warmup: CS2's own warmup text hides Ready Up's center HTML, so
    // it is ended even if an older warmup.cfg still starts it.
    const char* off[] = {"mp_warmup_pausetimer 0", "mp_warmuptime 0", "mp_team_intro_time 0", "mp_warmup_end"};
    for (const char* c : off) {
      if (EnqueueServerCommand(c)) any = true;
    }
    // No death drops even with an older warmup.cfg that lacks them (weapon_cleanup.h).
    EnqueueAfterCfg(std::vector<std::string>(std::begin(kWarmupNoDropCmds), std::end(kWarmupNoDropCmds)));
    if (any) {
      st.warmupRulesApplied = true;
      DebugLine("modes: warmup cfg executed (CS2 warmup off)");
    }
    return;
  }

  // Keep warmup playable: respawns on, ignore win conditions, long round time.
  // Do NOT enable CS2 built-in warmup. Best-effort force it off so Ready Up warmup
  // is the only warmup behavior (some builds may ignore/unknown these cvars).
  const std::string respawnCt = std::string("mp_respawn_on_death_ct ") + (st.warmupRespawn ? "1" : "0");
  const std::string respawnT = std::string("mp_respawn_on_death_t ") + (st.warmupRespawn ? "1" : "0");
  const std::string ignoreWin = std::string("mp_ignore_round_win_conditions ") + (st.warmupIgnoreWin ? "1" : "0");
  const std::string buyAny = std::string("mp_buy_anywhere ") + (st.warmupBuyAnywhere ? "1" : "0");
  const int rt = std::max(1, std::min(120, st.warmupRoundTimeMinutes));
  const std::string roundTime = "mp_roundtime " + std::to_string(rt);
  const std::string roundTimeDefuse = "mp_roundtime_defuse " + std::to_string(rt);
  const std::string roundTimeHostage = "mp_roundtime_hostage " + std::to_string(rt);
  const int startMoney = std::max(0, std::min(60000, st.warmupStartMoney));
  const int maxMoney = std::max(startMoney, std::min(60000, st.warmupMaxMoney));
  const std::string startMoneyCmd = "mp_startmoney " + std::to_string(startMoney);
  const std::string maxMoneyCmd = "mp_maxmoney " + std::to_string(maxMoney);

  bool any = false;
  // Disable built-in warmup (best-effort). Note: `mp_do_warmup_period` is an
  // unknown command on current CS2 builds (1.41.8.3); `mp_warmup_end` does the job.
  if (EnqueueServerCommand("mp_warmup_pausetimer 0")) any = true;
  if (EnqueueServerCommand("mp_warmuptime 0")) any = true;
  if (EnqueueServerCommand("mp_team_intro_time 0")) any = true;
  if (EnqueueServerCommand("mp_warmup_end")) any = true;
  if (EnqueueServerCommand(respawnCt.c_str())) any = true;
  if (EnqueueServerCommand(respawnT.c_str())) any = true;
  if (EnqueueServerCommand(ignoreWin.c_str())) any = true;
  if (EnqueueServerCommand("mp_freezetime 0")) any = true;
  // Team damage / team kills must not kick anyone (warmup.cfg sets it in cfg mode).
  if (EnqueueServerCommand("mp_autokick 0")) any = true;
  // Extend buy period during freeplay warmup.
  if (EnqueueServerCommand("mp_buytime 9999")) any = true;
  if (EnqueueServerCommand(roundTime.c_str())) any = true;
  if (EnqueueServerCommand(roundTimeDefuse.c_str())) any = true;
  if (EnqueueServerCommand(roundTimeHostage.c_str())) any = true;
  if (EnqueueServerCommand(startMoneyCmd.c_str())) any = true;
  if (EnqueueServerCommand(maxMoneyCmd.c_str())) any = true;
  if (EnqueueServerCommand(buyAny.c_str())) any = true;
  // Warmup runs for hours: nothing drops on death (weapon_cleanup.h).
  for (const char* c : kWarmupNoDropCmds) {
    if (EnqueueServerCommand(c)) any = true;
  }
  if (any) {
    st.warmupRulesApplied = true;
    DebugLine("modes: warmup rules applied (respawn/ignorewin/long time)");
  }
}

// Scrim (pickup) warmup, emulated instead of CS2's own warmup. CS2's warmup
// text shares the center panel with Ready Up's HTML (welcome/ready screen) and
// wins, so warmup is kept off and its effects are reproduced: rounds can't
// end, players respawn, buy anywhere with full money. live.cfg resets these
// (mp_ignore_round_win_conditions 0, normal round/freeze times) on go-live.
static void ApplyScrimWarmupRulesLocked(State& st) {
  bool any = false;
  // cfg-exec mode: warmup.cfg is the baseline; the lines below override it,
  // including ending the real warmup that warmup.cfg starts.
  if (st.cfgExecEnabled) {
    if (EnqueueServerCommand("exec ReadyUp/warmup.cfg")) any = true;
  }
  const char* cmds[] = {
      "mp_autoteambalance 0",
      "mp_limitteams 0",
      "mp_autokick 0",
      // Ending CS2 warmup restarts the round, which plays the team intro and
      // covers Ready Up HTML; skip it.
      "mp_team_intro_time 0",
      "mp_ignore_round_win_conditions 1",
      "mp_freezetime 0",
      "mp_roundtime 60",
      "mp_roundtime_defuse 60",
      "mp_roundtime_hostage 60",
      "mp_respawn_on_death_ct 1",
      "mp_respawn_on_death_t 1",
      "mp_buy_anywhere 1",
      "mp_buytime 9999",
      "mp_startmoney 16000",
      "mp_maxmoney 16000",
      "mp_warmup_pausetimer 0",
      "mp_warmuptime 0",
      "mp_warmup_end",
      // Idle is plain CS2: a round can end before warmup starts (the first player takes a bot's
      // place and a side is briefly empty), leaving a 1:0 that the warmup never clears.
      "mp_restartgame 1",
  };
  for (const char* c : cmds) {
    if (EnqueueServerCommand(c)) any = true;
  }
  // Warmup runs for hours: nothing drops on death (weapon_cleanup.h). After warmup.cfg, if it ran.
  if (st.cfgExecEnabled) {
    EnqueueAfterCfg(std::vector<std::string>(std::begin(kWarmupNoDropCmds), std::end(kWarmupNoDropCmds)));
  } else {
    for (const char* c : kWarmupNoDropCmds) {
      if (EnqueueServerCommand(c)) any = true;
    }
  }
  if (any) {
    st.warmupRulesApplied = true;
    DebugLine("modes: scrim warmup rules applied (emulated warmup, CS2 warmup off)");
  }
}

static bool IsSafeConvarKey(const std::string& s) {
  if (s.empty()) return false;
  for (unsigned char c : s) {
    if (!(std::isalnum(c) != 0 || c == '_')) return false;
  }
  return true;
}

static bool IsSafeConvarValue(const std::string& s) {
  if (s.empty()) return false;
  // Keep strict to prevent command injection (`;`, newlines, quotes, spaces).
  for (unsigned char c : s) {
    if (c == ';' || c == '\n' || c == '\r' || c == '"' || c == '\\' || std::isspace(c) != 0) return false;
    // Allow a common subset of value chars.
    if (!(std::isalnum(c) != 0 || c == '_' || c == '.' || c == '-' || c == '/' || c == ':')) return false;
  }
  return true;
}

// Match config cvars win over live.cfg / knife.cfg: they go out after the cfg ran
// (EnqueueAfterCfg), whichever order the callers queue them in.
// mp_teamname_1 is the team that starts the map on CT (the engine keeps the names with the teams
// at halftime); after a knife `.switch` (mp_swapteams) the map side, and so the names, change.
// Scrims keep CS2's default names. Queued with the match cvars (after the cfg).
// followSides: a resume warmup, where the CT / T names follow the current sides (swap count).
static void AppendTeamNameCmds(std::vector<std::string>* cmds, bool followSides = false) {
  const auto ctx = WebhookGetMatchContext();  // fresh: the knife pick just updated map_sides
  std::string n1, n2, f1, f2;
  if (ctx && ctx->slug != "scrim") {
    const int mapNum = std::max(1, MatchStateGet().map_number);
    bool team1Ct = !(static_cast<size_t>(mapNum) <= ctx->map_sides.size() &&
                     ctx->map_sides[static_cast<size_t>(mapNum - 1)] == "team2_ct");
    if (followSides && MatchEventsSwapCountFor(mapNum) % 2 != 0) team1Ct = !team1Ct;
    n1 = SanitizeTeamName(team1Ct ? ctx->team1_name : ctx->team2_name);
    n2 = SanitizeTeamName(team1Ct ? ctx->team2_name : ctx->team1_name);
    f1 = SanitizeTeamName(team1Ct ? ctx->team1_flag : ctx->team2_flag);
    f2 = SanitizeTeamName(team1Ct ? ctx->team2_flag : ctx->team1_flag);
    if (f1.size() > 3) f1.clear();
    if (f2.size() > 3) f2.clear();
  }
  cmds->push_back("mp_teamname_1 \"" + n1 + "\"");
  cmds->push_back("mp_teamname_2 \"" + n2 + "\"");
  cmds->push_back("mp_teamflag_1 \"" + f1 + "\"");
  cmds->push_back("mp_teamflag_2 \"" + f2 + "\"");
  Debug("modes: team names ct-start=\"%s\" t-start=\"%s\"\n", n1.c_str(), n2.c_str());
}

static void ApplyMatchCvarsLocked(const WebhookMatchContext& ctx, bool followSides) {
  // Normally read at match load already; this catches names added later (match.update) or a read
  // that could not go out then. The reads are queued now, the cvars below only after the cfg.
  cvar_snapshot::CaptureMatchCvars(ctx.cvars);
  std::vector<std::string> cmds;
  AppendTeamNameCmds(&cmds, followSides);
  // Playout (match_settings.h): the engine must not end the map at the clinch either. A match
  // cvar mp_match_can_clinch wins (it follows below).
  if (!ctx.cvars.count("mp_match_can_clinch")) {
    cmds.push_back(match_settings::PlayoutOn(&ctx) ? "mp_match_can_clinch 0" : "mp_match_can_clinch 1");
  }
  if (ctx.cvars.empty()) {
    AppendRuleCommands(&cmds);  // ruleset overrides win over the cfg (esports.h)
    EnqueueAfterCfg(std::move(cmds));
    return;
  }
  if (DebugEnabled()) {
    Debug("modes: applying %zu match cvars\n", ctx.cvars.size());
  }
  for (const auto& kv : ctx.cvars) {
    const std::string& key = kv.first;
    const std::string& val = kv.second;
    if (!IsSafeConvarKey(key) || !IsSafeConvarValue(val)) {
      Debug("modes: skipped unsafe cvar key=\"%s\" val=\"%s\"\n", key.c_str(), val.c_str());
      continue;
    }
    const std::string cmd = key + " " + val;
    if (DebugEnabled()) {
      Debug("modes: cvar: %s\n", cmd.c_str());
    }
    cmds.push_back(cmd);
  }
  AppendRuleCommands(&cmds);
  EnqueueAfterCfg(std::move(cmds));
}

// The live round's win-condition setting: a match config cvar wins over the default 0.
static std::string WinConditionsAtLiveCommand(const WebhookMatchContext& ctx) {
  const auto it = ctx.cvars.find("mp_ignore_round_win_conditions");
  if (it != ctx.cvars.end() && IsSafeConvarValue(it->second)) return "mp_ignore_round_win_conditions " + it->second;
  return "mp_ignore_round_win_conditions 0";
}

static void StopDemoLocked(State& st) {
  // Admin restart / end: stop now, no upload (map ends go through match_end.cpp).
  demo::StopNowWithoutUpload();
  st.demoRecording = false;
  st.demoMapNumber = 0;
  st.demoName.clear();
}

static void StartDemoForMapLocked(State& st, int mapNumber, const std::string& mapName) {
  auto ctxOpt = WebhookGetMatchContext();
  if (!ctxOpt) return;
  const auto& ctx = *ctxOpt;
  if (mapNumber <= 0) mapNumber = 1;
  // Idempotent per map.
  if (st.demoRecording && st.demoMapNumber == mapNumber && demo::IsRecording()) return;

  demo::RecordingInfo info;
  info.matchid = static_cast<long long>(ctx.matchid);
  info.slug = ctx.slug;
  info.mapNumber = mapNumber;
  info.mapName = mapName;
  if (info.mapName.empty() && static_cast<size_t>(mapNumber) <= ctx.maplist.size()) {
    info.mapName = mapnames::DisplayName(ctx.maplist[static_cast<size_t>(mapNumber - 1)]);
  }
  info.team1 = ctx.team1_name;
  info.team2 = ctx.team2_name;
  info.record = ctx.demo_record;
  info.upload = ctx.demo_upload;
  if (demo::StartRecording(info)) {
    st.demoRecording = true;
    st.demoMapNumber = mapNumber;
    st.demoName = info.mapName;
  }
}

// A map went live: per-map stats start from zero (warmup and knife rounds never count).
static void BeginMapStats(const WebhookMatchContext& ctx, int mapNumber) {
  bool team1IsCt = true;
  if (mapNumber >= 1 && static_cast<size_t>(mapNumber) <= ctx.map_sides.size()) {
    team1IsCt = ctx.map_sides[static_cast<size_t>(mapNumber - 1)] != "team2_ct";
  }
  std::lock_guard<std::recursive_mutex> lk(stats::Mutex());
  stats::Current().BeginMap(team1IsCt);
}

static void ClearMapStats() {
  std::lock_guard<std::recursive_mutex> lk(stats::Mutex());
  stats::Current().Clear();
}

static void ApplyLiveRulesAndRestartLocked(State& st, const WebhookMatchContext& ctx) {
  bool any = false;
  ApplyMatchCvarsLocked(ctx);
  // The valve ruleset is its cfg: it runs even with ru_cfg_exec_enable 0 (esports.h).
  if (st.cfgExecEnabled || LiveCfgRequired()) {
    if (EnqueueServerCommand(LiveCfgExecCommand().c_str())) any = true;
  } else {
    const char* cmds[] = {
        // End CS2 built-in warmup if it came back (e.g. after a map change).
        "mp_warmup_end",
        "mp_respawn_on_death_ct 0",
        "mp_respawn_on_death_t 0",
        "mp_buy_anywhere 0",
        "mp_buytime 20",
        "sv_infinite_ammo 0",
        "sv_cheats 0",
        // Undo the rest of the emulated warmup (ApplyScrimWarmupRulesLocked): without these a
        // match went live with 60-minute rounds, no freeze time and 16000 start money. Values as
        // in ReadyUp/live.cfg; match config cvars still win (they go out after, EnqueueAfterCfg).
        "mp_roundtime 1.92",
        "mp_roundtime_defuse 1.92",
        "mp_roundtime_hostage 1.92",
        "mp_freezetime 18",
        "mp_startmoney 800",
        "mp_maxmoney 16000",
        "mp_autoteambalance 0",
        "mp_limitteams 0",
        "mp_autokick 0",
    };
    for (const char* c : cmds) {
      if (EnqueueServerCommand(c)) any = true;
    }
    // Weapons drop on death again (warmup turned it off; live.cfg does this in cfg mode).
    for (const char* c : kLiveDropCmds) {
      if (EnqueueServerCommand(c)) any = true;
    }
  }
  round_restore::EnableRoundBackups(ctx.matchid, std::max(1, MatchStateGet().map_number));
  HoldWinConditionsForGoLive();
  // Start clean (also effectively clears warmup scoreboard noise).
  if (EnqueueServerCommand("mp_restartgame 1")) any = true;
  if (any) {
    st.startTriggered = true;
    DebugLine("modes: all ready -> applied live rules + mp_restartgame 1");
  }
}

static bool ResetServerRulesAndRestartLocked(State& st) {
  // Revert any warmup/practice-esque cvars and restart.
  std::vector<std::string> cmds = {
      "mp_respawn_on_death_ct 0",
      "mp_respawn_on_death_t 0",
      "mp_ignore_round_win_conditions 0",
      "mp_buy_anywhere 0",
      "mp_buytime 20",
      "sv_infinite_ammo 0",
      "sv_cheats 0",
      "mp_teamname_1 \"\"",
      "mp_teamname_2 \"\"",
      "mp_teamflag_1 \"\"",
      "mp_teamflag_2 \"\"",
  };
  // The match config's own cvars{} go back to their pre-match values (cvar_snapshot.h), after the
  // fixed resets so a config that set one of those gets its original back too.
  // reset_cvars_on_series_end 0: the match values stay and the snapshot is dropped.
  if (match_settings::ResetCvarsOnSeriesEnd()) {
    for (auto& c : cvar_snapshot::TakeRestoreCommands()) cmds.push_back(std::move(c));
  } else {
    cvar_snapshot::Discard();
  }
  cmds.emplace_back("mp_restartgame 1");

  bool any = false;
  for (const auto& c : cmds) {
    if (EnqueueServerCommand(c.c_str())) any = true;
  }
  (void)st;
  return any;
}

static void MaybeGateMatchLocked(State& st) {
  if (st.mode != ReadyUpMode::MatchWarmup) return;
  if (!st.warmupEnabled) return;

  const auto now = std::chrono::steady_clock::now();
  const auto minCmdInterval = std::chrono::milliseconds(800);
  if (st.lastGateCmd.time_since_epoch().count() != 0 && (now - st.lastGateCmd) < minCmdInterval) return;

  // Recovery gate: keep match state/cvars and just wait for players to connect + ready.
  if (st.recoveryGate) {
    auto ctxOpt = WebhookGetMatchContext();
    if (!ctxOpt) return;
    const bool allReady = AllRosterReadyAndConnectedLocked(st, *ctxOpt);
    if (!allReady) return;

    (void)EnqueueServerCommand("mp_unpause_match");
    if (PauseStateGet().paused) PauseStateOnUnpaused();  // the restore's autopause
    WebhookSetHeartbeatStatus("live");
    st.mode = ReadyUpMode::MatchLive;
    st.recoveryGate = false;
    st.fullRosterGate = false;
    st.lastUi.clear();

    const auto ms = MatchStateGet();
    const int mapNumber = ms.map_number <= 0 ? 1 : ms.map_number;
    // match_recovery.h put the map's stats back (live): they continue; else the map starts over.
    bool statsLive = false;
    {
      std::lock_guard<std::recursive_mutex> slk(stats::Mutex());
      statsLive = stats::Current().Live();
    }
    if (!statsLive) BeginMapStats(*ctxOpt, mapNumber);
    match_recovery::NoteProgress();
    StartDemoForMapLocked(st, mapNumber, ms.current_map);

    DebugLine("modes: recovery gate cleared -> mp_unpause_match");
    st.lastGateCmd = now;
    return;
  }

  // Apply warmup rules once (best-effort; can retry if command buffer unavailable early).
  if (!st.warmupRulesApplied) {
    ApplyWarmupRulesLocked(st);
    st.lastGateCmd = now;
    return;
  }

  // No match loaded yet: keep warmup rules active but do not attempt to gate start.
  auto ctxOpt = WebhookGetMatchContext();
  if (!ctxOpt) {
    // No active match context => server is allocatable (MAT should see "idle").
    // We still keep the warmup/freeplay rules applied so players can run around.
    WebhookSetHeartbeatStatus("idle");
    return;
  }

  // When all roster players are connected + ready, flip to live rules and restart.
  if (!st.startTriggered) {
    // Knife maps go live through the knife round (MaybeEnterKnifeModeLocked).
    {
      const auto ms = MatchStateGet();
      if (IsKnifeForMap(*ctxOpt, ms.map_number <= 0 ? 1 : ms.map_number)) return;
    }
    const bool allReady = AllRosterReadyAndConnectedLocked(st, *ctxOpt);
    if (allReady) {
      st.lastGateCmd = now;
      // valve: GOTV must be up (esports.h); refused until it is or an admin forces the start.
      if (!EsportsGoLiveAllowed(/*forced=*/false)) return;
      ApplyLiveRulesAndRestartLocked(st, *ctxOpt);
    }
  }
}

static void ApplyPracticeRulesLocked(State& st) {
  bool any = false;
  if (st.cfgExecEnabled) {
    // In MatchZy-compat mode, cfg files are authoritative.
    if (EnqueueServerCommand("exec ReadyUp/prac.cfg")) any = true;
  } else {
    const char* cmds[] = {
        "sv_cheats 1",
        "mp_respawn_on_death_ct 1",
        "mp_respawn_on_death_t 1",
        "mp_ignore_round_win_conditions 1",
        "mp_freezetime 0",
        "mp_buy_anywhere 1",
        "mp_buytime 9999",
        // infinite ammo with reload
        "sv_infinite_ammo 2",
        // long-ish round time
        "mp_roundtime 60",
        "mp_roundtime_defuse 60",
        "mp_roundtime_hostage 60",
    };
    for (const char* c : cmds) {
      if (EnqueueServerCommand(c)) any = true;
    }
  }
  if (any) {
    st.practiceRulesApplied = true;
    st.practiceResetPending = true;
    DebugLine("modes: practice rules applied");
  }
}

static void ResetPracticeRulesLocked(State& st) {
  if (!st.practiceResetPending) return;
  const char* cmds[] = {
      "mp_respawn_on_death_ct 0",
      "mp_respawn_on_death_t 0",
      "mp_ignore_round_win_conditions 0",
      "mp_buy_anywhere 0",
      "mp_buytime 20",
      "sv_infinite_ammo 0",
      "sv_cheats 0",
  };
  bool any = false;
  for (const char* c : cmds) {
    if (EnqueueServerCommand(c)) any = true;
  }
  if (any) {
    st.practiceResetPending = false;
    DebugLine("modes: practice rules reset");
  }
}

}  // namespace

// A joiner is spawned only in the match warmup itself: not once going live was triggered (the
// go-live restart spawns everyone) nor in a recovered live map's gate (its restored round stands).
static bool JoinSpawnApplies(const State& st) {
  return st.mode == ReadyUpMode::MatchWarmup && !st.startTriggered && !st.recoveryGate;
}

// Restarts the match warmup round so everyone spawns. Debounced: a few joins (and a team-lock
// move) land in the same restart.
static void RestartWarmupRoundForJoiner() {
  static bool s_pending = false;
  static auto s_last = std::chrono::steady_clock::time_point{};
  const auto now = std::chrono::steady_clock::now();
  if (s_pending || (now - s_last) < std::chrono::seconds(4)) return;
  s_pending = true;
  ScheduleOnGameThread(1.5, []() {
    s_pending = false;
    s_last = std::chrono::steady_clock::now();
    {
      auto& st = St();
      std::lock_guard<std::mutex> lk(st.mu);
      if (!JoinSpawnApplies(st)) return;
    }
    (void)EnqueueServerCommand("mp_restartgame 1");
    DebugLine("modes: warmup round restarted so a joining player spawns");
  });
}

void ModesOnHumanJoinedTeam(int slot, uint64_t steamid, int team) {
  // One respawn per join: a team-lock move also comes back as the engine's player_team.
  static std::unordered_set<int> s_pendingSlots;
  if (team != 2 && team != 3) return;
  {
    auto& st = St();
    std::lock_guard<std::mutex> lk(st.mu);
    if (!JoinSpawnApplies(st)) return;
  }
  if (slot < 0 || !RespawnAvailable()) {
    RestartWarmupRoundForJoiner();
    return;
  }
  if (!s_pendingSlots.insert(slot).second) return;
  // 0.5 s: the team change has landed; only the joiner respawns (if still dead and on a team).
  ScheduleOnGameThread(0.5, [slot, steamid]() {
    s_pendingSlots.erase(slot);
    {
      auto& st = St();
      std::lock_guard<std::mutex> lk(st.mu);
      if (!JoinSpawnApplies(st)) return;
    }
    for (const auto& x : ListHumans()) {
      if (x.slot != slot || x.steamid64 != steamid || (x.team != 2 && x.team != 3)) continue;
      const int rc = RespawnSlot(slot);
      if (rc == 1) Debug("modes: respawned slot %d so the joiner spawns\n", slot);
      else if (rc == 0) RestartWarmupRoundForJoiner();
      return;
    }
  });
}

void ModesOnMapLoaded() {
  auto& st = St();
  std::lock_guard<std::mutex> lk(st.mu);
  st.mapLoaded = true;
}

void ModesOnEngineGameOver(int ctScore, int tScore) {
  // Later, not now: the final round_end normally decides the map itself just after this line.
  ScheduleOnGameThread(2.0, [ctScore, tScore]() {
    auto ctxOpt = WebhookGetMatchContext();
    if (!ctxOpt || ctxOpt->slug == "scrim") return;
    auto& st = St();
    std::lock_guard<std::mutex> lk(st.mu);
    if (st.mode != ReadyUpMode::MatchLive) return;
    const auto ms = MatchStateGet();
    const int map = ms.map_number <= 0 ? 1 : ms.map_number;
    if (st.mapResultEmittedForMapNumber == map) return;
    bool team1Ct = true;
    {
      std::lock_guard<std::recursive_mutex> slk(stats::Mutex());
      team1Ct = stats::Current().Snapshot().team1_is_ct;
    }
    const int t1 = std::max(0, team1Ct ? ctScore : tScore);
    const int t2 = std::max(0, team1Ct ? tScore : ctScore);
    const char* winner = t1 > t2 ? "team1" : t2 > t1 ? "team2" : "none";
    std::string mapName = ms.current_map;
    if (mapName.empty() && static_cast<size_t>(map) <= ctxOpt->maplist.size()) {
      mapName = ctxOpt->maplist[static_cast<size_t>(map - 1)];
    }
    Print("map-end: the engine ended map %d (%d-%d) and Ready Up had not; finishing it from the engine's score\n",
          map, t1, t2);
    FinishMapLocked(st, *ctxOpt, map, mapName, t1, t2, winner, /*forfeit=*/false);
  });
}

ReadyUpMode GetMode() {
  auto& st = St();
  std::lock_guard<std::mutex> lk(st.mu);
  return st.mode;
}

const char* GetModeString() {
  return ModeToString(GetMode());
}

void SetModeIdle() {
  auto& st = St();
  std::lock_guard<std::mutex> lk(st.mu);
  StopDemoLocked(st);  // a dropped match must not leave its demo recording
  if (st.mode == ReadyUpMode::Practice) {
    // reset will happen from Tick (server thread), but we can mark it here.
    st.practiceResetPending = true;
  }
  st.mode = ReadyUpMode::Idle;
  st.externalName.clear();
  st.idleCfgExecuted = false;
  st.lastUi.clear();
  st.warmupRulesApplied = false;
  st.startTriggered = false;
  st.lifecycleMapNumber = 0;
  st.warmupEndedSent = false;
  st.goingLiveSent = false;
  st.mapResultEmittedForMapNumber = 0;
  st.seriesWinsTeam1 = 0;
  st.seriesWinsTeam2 = 0;
  ResetKnifeStateForMapLocked(st, /*mapNumber=*/0);
}

bool SetModeExternal(const std::string& name) {
  auto& st = St();
  std::lock_guard<std::mutex> lk(st.mu);
  if (st.mode != ReadyUpMode::Idle && st.mode != ReadyUpMode::ScrimWarmup && st.mode != ReadyUpMode::External) {
    return false;
  }
  StopDemoLocked(st);
  st.mode = ReadyUpMode::External;
  st.externalName = name;
  st.externalTouchedGameMode = true;
  st.idleCfgExecuted = false;
  st.ready.clear();
  st.lastUi.clear();
  st.warmupRulesApplied = false;
  st.startTriggered = false;
  st.lifecycleMapNumber = 0;
  st.warmupEndedSent = false;
  st.goingLiveSent = false;
  st.mapResultEmittedForMapNumber = 0;
  st.seriesWinsTeam1 = 0;
  st.seriesWinsTeam2 = 0;
  ResetKnifeStateForMapLocked(st, /*mapNumber=*/0);
  return true;
}

std::string ExternalModeName() {
  auto& st = St();
  std::lock_guard<std::mutex> lk(st.mu);
  return st.mode == ReadyUpMode::External ? st.externalName : std::string();
}

void SetModePractice() {
  auto& st = St();
  std::lock_guard<std::mutex> lk(st.mu);
  StopDemoLocked(st);
  st.mode = ReadyUpMode::Practice;
  st.externalName.clear();
  st.idleCfgExecuted = false;
  st.lastUi.clear();
  st.warmupRulesApplied = false;
  st.startTriggered = false;
  st.lifecycleMapNumber = 0;
  st.warmupEndedSent = false;
  st.goingLiveSent = false;
  st.practiceRulesApplied = false;
  st.practiceResetPending = false;
  st.mapResultEmittedForMapNumber = 0;
  st.seriesWinsTeam1 = 0;
  st.seriesWinsTeam2 = 0;
  ResetKnifeStateForMapLocked(st, /*mapNumber=*/0);
}

bool SetModeScrimWarmup() {
  auto& st = St();
  std::lock_guard<std::mutex> lk(st.mu);
  if (st.mode != ReadyUpMode::Idle) return false;
  st.mode = ReadyUpMode::ScrimWarmup;
  st.idleCfgExecuted = false;
  st.lastUi.clear();
  st.warmupRulesApplied = false;  // Tick() applies scrim warmup rules
  st.startTriggered = false;
  st.lastNotReadyChat = {};
  return true;
}

void SetModeMatchWarmupForRecovery() {
  auto& st = St();
  std::lock_guard<std::mutex> lk(st.mu);
  st.mode = ReadyUpMode::MatchWarmup;
  st.idleCfgExecuted = false;
  st.warmupRulesApplied = false;
  st.startTriggered = false;
}

bool ScrimGoLive(int restartSeconds) {
  auto ctxOpt = WebhookGetMatchContext();
  if (!ctxOpt) return false;
  auto& st = St();
  std::lock_guard<std::mutex> lk(st.mu);
  if (st.mode != ReadyUpMode::MatchWarmup) return false;
  restartSeconds = std::max(1, std::min(10, restartSeconds));

  // Don't let MaybeGateMatchLocked re-apply (code-side) warmup rules or send the
  // "match loaded, type .r" prompt: everyone is already ready.
  st.warmupRulesApplied = true;
  st.matchLoadedChatSent = true;
  st.lastUi.clear();

  ApplyMatchCvarsLocked(*ctxOpt);
  bool any = false;
  // Always use the live baseline cfg for scrims (it ends with mp_warmup_end).
  if (EnqueueServerCommand(LiveCfgExecCommand().c_str())) any = true;
  if (EnqueueServerCommand("mp_warmup_pausetimer 0")) any = true;
  if (EnqueueServerCommand("mp_warmup_end")) any = true;
  round_restore::EnableRoundBackups(ctxOpt->matchid, std::max(1, MatchStateGet().map_number));
  HoldWinConditionsForGoLive();
  const std::string restart = "mp_restartgame " + std::to_string(restartSeconds);
  if (EnqueueServerCommand(restart.c_str())) any = true;
  if (any) {
    st.startTriggered = true;
    st.lastGateCmd = std::chrono::steady_clock::now();
    DebugLine("modes: scrim go-live -> exec live.cfg + mp_warmup_end + mp_restartgame");
  }
  return any;
}

bool GoLiveTriggered() {
  auto& st = St();
  std::lock_guard<std::mutex> lk(st.mu);
  return st.startTriggered;
}

void ReapplyWarmupRules() {
  auto& st = St();
  std::lock_guard<std::mutex> lk(st.mu);
  if (st.mode != ReadyUpMode::ScrimWarmup && st.mode != ReadyUpMode::MatchWarmup) return;
  if (st.recoveryGate || st.startTriggered) return;
  st.warmupRulesApplied = false;
  st.lastGateCmd = {};
}

bool WarmupMapReady() {
  auto& st = St();
  std::lock_guard<std::mutex> lk(st.mu);
  return st.warmupMapReady;
}

void ModesAwaitMapLoad() {
  auto& st = St();
  std::lock_guard<std::mutex> lk(st.mu);
  st.warmupMapReady = false;
}

void OnMatchLoaded() {
  MatchEndCancelPending();
  ClearMapStats();
  PauseStateOnUnpaused();  // a new match never starts as paused (the last one's pause, if any)
  PauseStateResetUsage();
  auto& st = St();
  std::lock_guard<std::mutex> lk(st.mu);
  if (st.externalTouchedGameMode) {
    // An external mode (deathmatch) may have switched CS2's game mode. game_type / game_mode
    // take effect on the next map load, which the match load issues right after this: a match is
    // classic competitive.
    (void)EnqueueServerCommand("game_type 0");
    (void)EnqueueServerCommand("game_mode 1");
    st.externalTouchedGameMode = false;
    Print("modes: an external mode was on; game_type 0 / game_mode 1 (competitive) for the match\n");
  }
  if (const auto ctx = WebhookGetMatchContext(); ctx && ctx->wingman) st.wingmanGameMode = true;
  st.mode = ReadyUpMode::MatchWarmup;
  st.externalName.clear();
  st.idleCfgExecuted = false;
  st.ready.clear();
  st.lastUi.clear();
  st.warmupRulesApplied = false;
  st.startTriggered = false;
  st.lifecycleMapNumber = 0;
  st.warmupEndedSent = false;
  st.goingLiveSent = false;
  st.mapResultEmittedForMapNumber = 0;
  st.seriesWinsTeam1 = 0;
  st.seriesWinsTeam2 = 0;
  ResetKnifeStateForMapLocked(st, /*mapNumber=*/0);
  readyup::persisted_match_state::PersistLiveFlag(false);
  st.recoveryGate = false;
  st.fullRosterGate = false;
  st.lastForceJoin.clear();
  st.forceStartWinHold = false;
  st.matchLoadedChatSent = false;
  st.lastNotReadyChat = {};

  // Refresh MAT admins once per match load (best-effort) so admin status/prefixes
  // reflect recent changes without periodic polling.
  readyup::mat_admins::RefreshNow();
}

void OnMatchRoundStarted() {
  auto& st = St();
  std::lock_guard<std::mutex> lk(st.mu);
  if (st.forceStartWinHold && st.mode == ReadyUpMode::MatchLive) {
    if (st.lastGateCmd.time_since_epoch().count() != 0 &&
        (std::chrono::steady_clock::now() - st.lastGateCmd) < std::chrono::milliseconds(900)) {
      DebugLine("modes: ignoring Round_Start right after the force-start restart");
      return;
    }
    st.forceStartWinHold = false;
    if (const auto ctxWin = WebhookGetMatchContext()) (void)EnqueueServerCommand(WinConditionsAtLiveCommand(*ctxWin).c_str());
    return;
  }
  if (st.mode != ReadyUpMode::MatchWarmup) return;
  // A round started after the match load / next map's changelevel: the warmup's map is up, also
  // when it is the map the server was already on.
  st.warmupMapReady = true;

  // When warmup gating is enabled, a map's initial Round_Start (or other spurious
  // round starts) should NOT automatically mark the match as live. Only transition
  // once Ready Up actually triggered going-live (startTriggered), or when warmup
  // gating is disabled.
  if (st.warmupEnabled && !st.startTriggered) return;
  // mp_restartgame 1 needs a second: a Round_Start inside that window belongs
  // to the round being replaced (e.g. the knife side-pick round), not to live.
  if (st.startTriggered && st.lastGateCmd.time_since_epoch().count() != 0 &&
      (std::chrono::steady_clock::now() - st.lastGateCmd) < std::chrono::milliseconds(900)) {
    DebugLine("modes: ignoring Round_Start right after the go-live restart");
    return;
  }

  st.mode = ReadyUpMode::MatchLive;
  st.warmupRulesApplied = false;
  st.startTriggered = false;
  if (const auto ctxWin = WebhookGetMatchContext()) (void)EnqueueServerCommand(WinConditionsAtLiveCommand(*ctxWin).c_str());
  readyup::persisted_match_state::PersistLiveFlag(true);
  match_recovery::NoteProgress();
  WebhookSetHeartbeatStatus("live");
  SendToChat("Ready Up: LIVE! Good luck, have fun.");
  GoLiveCardArm("go-live round start", /*restartPending=*/false);  // "LIVE · GO GO GO" + commands

  // Emit warmup/live lifecycle events once per map transition.
  // MatchZy-style semantics: warmup_ended then going_live.
  const auto ms = MatchStateGet();
  const int mapNumber = ms.map_number <= 0 ? 1 : ms.map_number;
  if (st.lifecycleMapNumber != mapNumber) {
    st.lifecycleMapNumber = mapNumber;
    st.warmupEndedSent = false;
    st.goingLiveSent = false;
  }
  if (!st.warmupEndedSent) {
    WebhookEmitWarmupEnded(mapNumber);
    st.warmupEndedSent = true;
  }
  if (!st.goingLiveSent) {
    WebhookEmitGoingLive(mapNumber);
    st.goingLiveSent = true;
    if (auto ctxLive = WebhookGetMatchContext()) BeginMapStats(*ctxLive, mapNumber);
  }

  // Start per-map demo recording when map goes live.
  StartDemoForMapLocked(st, mapNumber, ms.current_map);
}

bool ForceStartMatch(bool force) {
  auto ctxOpt = WebhookGetMatchContext();
  if (!ctxOpt) return false;
  if (!EsportsGoLiveAllowed(force)) return false;  // valve without GOTV needs `force`
  auto& st = St();
  std::lock_guard<std::mutex> lk(st.mu);

  // Apply live rules + restart immediately.
  ApplyLiveRulesAndRestartLocked(st, *ctxOpt);
  st.mode = ReadyUpMode::MatchLive;
  st.lastUi.clear();
  st.warmupRulesApplied = false;
  st.startTriggered = false;
  st.forceStartWinHold = true;
  st.lastGateCmd = std::chrono::steady_clock::now();
  readyup::persisted_match_state::PersistLiveFlag(true);
  match_recovery::NoteProgress();
  WebhookSetHeartbeatStatus("live");

  // Emit warmup/live lifecycle events for force-start as well.
  const auto ms = MatchStateGet();
  const int mapNumber = ms.map_number <= 0 ? 1 : ms.map_number;
  if (st.lifecycleMapNumber != mapNumber) {
    st.lifecycleMapNumber = mapNumber;
    st.warmupEndedSent = false;
    st.goingLiveSent = false;
  }
  if (!st.warmupEndedSent) {
    WebhookEmitWarmupEnded(mapNumber);
    st.warmupEndedSent = true;
  }
  if (!st.goingLiveSent) {
    WebhookEmitGoingLive(mapNumber);
    st.goingLiveSent = true;
    if (auto ctxLive = WebhookGetMatchContext()) BeginMapStats(*ctxLive, mapNumber);
  }

  GoLiveCardArm("ru match start", /*restartPending=*/true);  // after the queued restart

  // Start per-map demo recording when map goes live (force-start path).
  StartDemoForMapLocked(st, mapNumber, ms.current_map);
  // Knife flow is irrelevant once we're forcing live.
  ResetKnifeStateForMapLocked(st, /*mapNumber=*/0);
  return true;
}

bool RestartMatch() {
  auto ctxOpt = WebhookGetMatchContext();
  if (!ctxOpt) return false;
  auto& st = St();
  std::lock_guard<std::mutex> lk(st.mu);

  // Stop any running demo when restarting back to warmup.
  StopDemoLocked(st);
  MatchEndCancelPending();
  ClearMapStats();

  // Return to warmup gating and restart.
  st.mode = ReadyUpMode::MatchWarmup;
  st.forceStartWinHold = false;
  st.idleCfgExecuted = false;
  st.ready.clear();
  st.lastUi.clear();
  st.warmupRulesApplied = false;
  st.startTriggered = false;
  st.lifecycleMapNumber = 0;
  st.warmupEndedSent = false;
  st.goingLiveSent = false;
  st.mapResultEmittedForMapNumber = 0;
  st.seriesWinsTeam1 = 0;
  st.seriesWinsTeam2 = 0;
  ResetKnifeStateForMapLocked(st, /*mapNumber=*/0);
  readyup::persisted_match_state::PersistLiveFlag(false);
  st.recoveryGate = false;
  st.fullRosterGate = false;
  WebhookSetHeartbeatStatus("warmup");

  // Best-effort reset scoreboard state.
  (void)EnqueueServerCommand("mp_restartgame 1");
  return true;
}

bool EndMatchResetServer() {
  auto& st = St();
  std::lock_guard<std::mutex> lk(st.mu);

  // Stop any running demo when ending match/resetting server.
  StopDemoLocked(st);
  MatchEndCancelPending();
  ClearMapStats();
  // A pause belongs to its match (an admin pause, a failover resume's pause): the next match on
  // this server must not start as paused.
  PauseStateOnUnpaused();
  PauseStateResetUsage();

  // Put server back into a neutral state regardless of match context.
  const bool ok = ResetServerRulesAndRestartLocked(st);
  st.mode = ReadyUpMode::Idle;
  st.externalName.clear();  // externalTouchedGameMode stays for the next match load
  st.idleCfgExecuted = false;
  st.ready.clear();
  st.lastUi.clear();
  st.warmupRulesApplied = false;
  st.startTriggered = false;
  st.practiceRulesApplied = false;
  st.practiceResetPending = false;
  st.lifecycleMapNumber = 0;
  st.warmupEndedSent = false;
  st.goingLiveSent = false;
  st.mapResultEmittedForMapNumber = 0;
  st.seriesWinsTeam1 = 0;
  st.seriesWinsTeam2 = 0;
  ResetKnifeStateForMapLocked(st, /*mapNumber=*/0);
  WebhookSetHeartbeatStatus("idle");
  readyup::persisted_match_state::ClearActiveMatch();
  st.recoveryGate = false;
  st.fullRosterGate = false;
  return ok;
}

void ModesSetSeriesWins(int team1, int team2) {
  auto& st = St();
  std::lock_guard<std::mutex> lk(st.mu);
  st.seriesWinsTeam1 = std::max(0, team1);
  st.seriesWinsTeam2 = std::max(0, team2);
}

std::pair<int, int> ModesGetSeriesWins() {
  auto& st = St();
  std::lock_guard<std::mutex> lk(st.mu);
  return {st.seriesWinsTeam1, st.seriesWinsTeam2};
}

void SetRecoveryGate(bool enabled) {
  auto& st = St();
  std::lock_guard<std::mutex> lk(st.mu);
  st.recoveryGate = enabled;
  if (enabled) {
    st.warmupHtml = "<b><font color='yellow'>Ready Up</font></b><br>"
                    "Recovered match.<br>"
                    "Waiting for all players to connect + ready.<br>"
                    "Type <b>.r</b> to ready up.";
    // Prevent warmup rule application from overriding recovered match state.
    st.warmupRulesApplied = true;
    // Prevent warmup->live transition logic from firing implicitly.
    st.startTriggered = true;
  } else if (st.warmupHtml.find("Recovered match") != std::string::npos) {
    // Restore default message if we previously set a recovery banner.
    st.warmupHtml = "<b><font color='yellow'>Ready Up</font></b><br>"
                    "You are not ready yet.<br>"
                    "Type <b>.r</b> to ready up.";
  }
}

bool RecoveryGateEnabled() {
  auto& st = St();
  std::lock_guard<std::mutex> lk(st.mu);
  return st.recoveryGate;
}

void SetFullRosterGate(bool enabled) {
  auto& st = St();
  std::lock_guard<std::mutex> lk(st.mu);
  st.fullRosterGate = enabled;
}

bool FullRosterGateEnabled() {
  auto& st = St();
  std::lock_guard<std::mutex> lk(st.mu);
  return st.fullRosterGate || st.recoveryGate;
}

void ApplyTeamNamesNow() {
  std::vector<std::string> cmds;
  AppendTeamNameCmds(&cmds);
  for (const auto& c : cmds) (void)EnqueueServerCommand(c.c_str());
}

void ApplyResumeWarmupTeamNames() {
  std::vector<std::string> cmds;
  AppendTeamNameCmds(&cmds, /*followSides=*/true);
  EnqueueAfterCfg(std::move(cmds));
}

void ApplyMatchCvarsNow() {
  auto ctxOpt = WebhookGetMatchContext();
  if (!ctxOpt) return;
  auto& st = St();
  std::lock_guard<std::mutex> lk(st.mu);
  ApplyMatchCvarsLocked(*ctxOpt, st.mode == ReadyUpMode::MatchWarmup && st.fullRosterGate);
}

static std::optional<const char*> DetermineMapWinnerIfComplete(const WebhookMatchContext& ctx,
                                                              int team1Score,
                                                              int team2Score) {
  team1Score = std::max(0, team1Score);
  team2Score = std::max(0, team2Score);
  const int maxRounds = std::max(1, ctx.maxRounds);
  const int winTargetReg = (maxRounds / 2) + 1;

  const int sum = team1Score + team2Score;

  // Playout: every regulation round (and every round of an overtime block) is played.
  if (match_settings::PlayoutOn(&ctx) &&
      PlayoutRoundsLeft(maxRounds, ctx.overtime_enabled, ctx.overtimeSegments, ctx.maxOvertimes, team1Score, team2Score)) {
    return std::nullopt;
  }

  auto winnerByDamage = [&]() -> std::optional<const char*> {
    if (!ctx.damageTiebreakEnabled) return std::nullopt;
    const auto dmg = GetRosterTeamDamageTotals();
    if (dmg.first > dmg.second) return "team1";
    if (dmg.second > dmg.first) return "team2";
    return std::nullopt;
  };

  // Regulation win (including clinch before maxRounds and the final maxRounds round).
  if (sum <= maxRounds) {
    if (team1Score >= winTargetReg && team1Score != team2Score) return "team1";
    if (team2Score >= winTargetReg && team1Score != team2Score) return "team2";
    // Not complete yet.
    if (sum < maxRounds) return std::nullopt;
  }

  // No overtime: allow draw at maxRounds tie.
  if (!ctx.overtime_enabled) {
    if (team1Score != team2Score) {
      return team1Score > team2Score ? "team1" : "team2";
    }
    // Regulation tie: resolve via damage when enabled.
    if (auto w = winnerByDamage()) return *w;
    // If damage is tied and sudden death is enabled, keep playing (requires server OT enabled).
    if (ctx.damageTiebreakEnabled && ctx.suddenDeathOnDamageTie) return std::nullopt;
    return "none";
  }

  const int seg = std::max(1, ctx.overtimeSegments);
  const int roundsPastReg = std::max(0, sum - maxRounds);
  const int blockSize = 2 * seg;
  if (blockSize <= 0) return std::nullopt;

  // Overtime cap reached: resolve ties by damage, or sudden-death if damage tied.
  if (ctx.maxOvertimes >= 0) {
    const int capRounds = ctx.maxOvertimes * blockSize;
    if (roundsPastReg >= capRounds) {
      if (team1Score != team2Score) {
        // Sudden death beyond the cap: first team to lead wins.
        return team1Score > team2Score ? "team1" : "team2";
      }
      if (auto w = winnerByDamage()) return *w;
      if (ctx.damageTiebreakEnabled && ctx.suddenDeathOnDamageTie) return std::nullopt;
      return "none";
    }
  }

  // Overtime enabled: first team to reach tieStart + seg + 1 wins the OT block.
  if (team1Score != team2Score) {
    // 0-based OT index of the round that just ended: the last round of a block (roundsPastReg ==
    // blockSize, e.g. 16-14 in MR12 + MR3 OT) still belongs to that block, not the next one.
    const int blockIndex = roundsPastReg > 0 ? (roundsPastReg - 1) / blockSize : 0;
    const int tieStart = (maxRounds / 2) + (blockIndex * seg);
    const int winTargetOt = tieStart + seg + 1;
    if (team1Score >= winTargetOt) return "team1";
    if (team2Score >= winTargetOt) return "team2";
  }

  return std::nullopt;
}

void OnMatchRoundEnded(int map_number, int team1_score, int team2_score, const std::string& map_name) {
  auto ctxOpt = WebhookGetMatchContext();
  if (!ctxOpt) return;

  auto& st = St();
  std::lock_guard<std::mutex> lk(st.mu);

  if (map_number <= 0) map_number = 1;
  if (st.mapResultEmittedForMapNumber == map_number) {
    return;
  }

  const auto winnerOpt = DetermineMapWinnerIfComplete(*ctxOpt, team1_score, team2_score);
  if (!winnerOpt) return;
  const char* winner = *winnerOpt;

  // Prefer config maplist name for this map number.
  std::string map = map_name;
  if (map.empty()) {
    if (static_cast<size_t>(map_number) >= 1 && static_cast<size_t>(map_number) <= ctxOpt->maplist.size()) {
      map = ctxOpt->maplist[static_cast<size_t>(map_number - 1)];
    }
  }

  FinishMapLocked(st, *ctxOpt, map_number, map, team1_score, team2_score, winner, /*forfeit=*/false);
}

namespace {
// The map is decided: map_result, series score, postgame, MatchEndOnMapComplete. forfeit: the
// series ends here with `winner` as the series winner.
static void FinishMapLocked(State& st, const WebhookMatchContext& ctx, int map_number, const std::string& map,
                            int team1_score, int team2_score, const char* winner, bool forfeit) {
  const WebhookMatchContext* ctxOpt = &ctx;
  // Update series score (maps won). A drawn map counts as played.
  if (std::strcmp(winner, "team1") == 0) st.seriesWinsTeam1 += 1;
  else if (std::strcmp(winner, "team2") == 0) st.seriesWinsTeam2 += 1;

  // map_result with the map's final player stats (the round that decided it is already closed in
  // the stats model) and the series score after this map.
  stats::MapStats mapStats;
  {
    std::lock_guard<std::recursive_mutex> slk(stats::Mutex());
    mapStats = stats::Current().Snapshot();
  }
  WebhookEmitMapResult(map_number, map.empty() ? "" : map.c_str(), std::max(0, team1_score), std::max(0, team2_score),
                       winner, mapStats, st.seriesWinsTeam1, st.seriesWinsTeam2);
  st.mapResultEmittedForMapNumber = map_number;

  const int totalMaps =
      ctxOpt->num_maps > 0 ? ctxOpt->num_maps : static_cast<int>(ctxOpt->maplist.size());
  const int remaining = RemainingMaps(totalMaps, static_cast<int>(ctxOpt->maplist.size()), map_number);
  bool seriesOver =
      forfeit || IsSeriesOver(totalMaps, remaining, st.seriesWinsTeam1, st.seriesWinsTeam2, ctxOpt->clinch_series);

  // map_number is 1-based; the next map is maplist[map_number].
  std::string nextMap;
  if (!seriesOver) {
    const size_t nextIndex = static_cast<size_t>(map_number);
    if (nextIndex < ctxOpt->maplist.size()) nextMap = ctxOpt->maplist[nextIndex];
    // No next map in the list: end the series instead of getting stuck.
    if (nextMap.empty()) seriesOver = true;
  }

  // Postgame until match_end.cpp starts the next map (ModesBeginNextMapWarmup) or unloads
  // the match (ModesFinishSeriesResetToIdle) once the demo is flushed.
  st.mode = ReadyUpMode::Postgame;
  st.startTriggered = false;
  st.warmupRulesApplied = false;
  st.demoRecording = false;  // demo_recorder stops it after the GOTV flush
  WebhookSetHeartbeatStatus("postgame");

  MapEndInput in;
  in.mapNumber = map_number;
  in.mapName = map;
  in.winner = winner;
  in.team1Score = std::max(0, team1_score);
  in.team2Score = std::max(0, team2_score);
  in.team1SeriesScore = st.seriesWinsTeam1;
  in.team2SeriesScore = st.seriesWinsTeam2;
  in.seriesOver = seriesOver;
  in.nextMap = nextMap;
  if (forfeit) in.seriesWinner = winner;
  (void)MatchEndOnMapComplete(in);
  match_recovery::NoteProgress();  // map_over + the series score (crash recovery)
}
}  // namespace

bool ForfeitCurrentMap(WebhookTeam loser, const char* reason) {
  auto ctxOpt = WebhookGetMatchContext();
  if (!ctxOpt || (loser != WebhookTeam::Team1 && loser != WebhookTeam::Team2)) return false;
  auto& st = St();
  std::lock_guard<std::mutex> lk(st.mu);
  if (st.mode != ReadyUpMode::MatchLive) return false;
  const auto ms = MatchStateGet();
  const int mapNumber = ms.map_number <= 0 ? 1 : ms.map_number;
  if (st.mapResultEmittedForMapNumber == mapNumber) return false;
  const char* loserStr = loser == WebhookTeam::Team1 ? "team1" : "team2";
  const char* winner = loser == WebhookTeam::Team1 ? "team2" : "team1";
  const std::string why = reason ? reason : "";
  if (PauseStateGet().paused) {
    (void)EnqueueServerCommand("mp_unpause_match");
    PauseStateOnUnpaused();
  }
  std::string map = ms.current_map;
  if (map.empty() && static_cast<size_t>(mapNumber) <= ctxOpt->maplist.size()) {
    map = ctxOpt->maplist[static_cast<size_t>(mapNumber - 1)];
  }
  Print("forfeit: %s (%s) forfeits map %d (%s) at %d-%d -> %s wins\n", loserStr, why.c_str(), mapNumber,
        map.c_str(), ms.team1_score, ms.team2_score, winner);
  WebhookEnqueueEvent(std::string("{\"event\":\"match_forfeit\",\"matchid\":") + std::to_string(ctxOpt->matchid) +
                      ",\"map_number\":" + std::to_string(mapNumber) + ",\"team\":\"" + loserStr +
                      "\",\"reason\":\"" + why + "\"}");
  if (signals::Enabled()) {
    status::Json d = status::Json::Object();
    d["team"] = loserStr;
    d["reason"] = why;
    signals::Emit("forfeit", std::move(d));
  }
  FinishMapLocked(st, *ctxOpt, mapNumber, map, ms.team1_score, ms.team2_score, winner, /*forfeit=*/true);
  return true;
}

// Unloads the match and returns to idle (series over; match_end.cpp calls this after the
// kick delay).
static void ResetToIdleAfterSeriesLocked(State& st) {
  // reset_cvars_on_series_end 0: the cvars stay as the match left them (match_settings.h); the
  // team names go (they are the match's) and the game restarts as usual.
  if (match_settings::ResetCvarsOnSeriesEnd()) {
    (void)ResetServerRulesAndRestartLocked(st);
  } else {
    Print("match: series over; cvars kept (ru_reset_cvars_on_series_end 0)\n");
    cvar_snapshot::Discard();  // the match values are the server's values from now on
    for (const char* c : {"mp_teamname_1 \"\"", "mp_teamname_2 \"\"", "mp_teamflag_1 \"\"", "mp_teamflag_2 \"\"",
                          "mp_restartgame 1"}) {
      (void)EnqueueServerCommand(c);
    }
  }
  st.mode = ReadyUpMode::Idle;
  st.idleCfgExecuted = false;
  st.ready.clear();
  st.lastUi.clear();
  st.warmupRulesApplied = false;
  st.startTriggered = false;
  st.practiceRulesApplied = false;
  st.practiceResetPending = false;
  st.lifecycleMapNumber = 0;
  st.warmupEndedSent = false;
  st.goingLiveSent = false;
  st.mapResultEmittedForMapNumber = 0;
  st.seriesWinsTeam1 = 0;
  st.seriesWinsTeam2 = 0;
  st.demoRecording = false;
  ResetKnifeStateForMapLocked(st, /*mapNumber=*/0);
  st.recoveryGate = false;
  st.fullRosterGate = false;
  WebhookSetHeartbeatStatus("idle");
}

void ModesBeginNextMapWarmup() {
  auto& st = St();
  std::lock_guard<std::mutex> lk(st.mu);
  if (st.mode != ReadyUpMode::Postgame) return;
  PauseStateResetUsage();
  st.mode = ReadyUpMode::MatchWarmup;
  st.idleCfgExecuted = false;
  st.ready.clear();
  st.lastUi.clear();
  st.warmupRulesApplied = false;
  st.warmupMapReady = false;
  st.startTriggered = false;
  st.lifecycleMapNumber = 0;
  st.warmupEndedSent = false;
  st.goingLiveSent = false;
  st.fullRosterGate = false;
  WebhookSetHeartbeatStatus("warmup");
}

void ModesFinishSeriesResetToIdle() {
  WebhookClearMatchContext();
  readyup::persisted_match_state::ClearActiveMatch();
  ClearMapStats();
  auto& st = St();
  std::lock_guard<std::mutex> lk(st.mu);
  ResetToIdleAfterSeriesLocked(st);
}

bool StartKnifeRound() {
  auto ctxOpt = WebhookGetMatchContext();
  if (!ctxOpt) return false;
  auto& st = St();
  std::lock_guard<std::mutex> lk(st.mu);
  return StartKnifeLocked(st, *ctxOpt);
}

KnifePhase GetKnifePhase() {
  auto& st = St();
  std::lock_guard<std::mutex> lk(st.mu);
  return st.mode == ReadyUpMode::MatchKnife ? st.knifePhase : KnifePhase::None;
}

const char* KnifePhaseString() {
  switch (GetKnifePhase()) {
    case KnifePhase::Starting: return "starting";
    case KnifePhase::Running: return "running";
    case KnifePhase::Picking: return "pick";
    default: return nullptr;
  }
}

void KnifeOnRoundStart(int map_number, const char* source) {
  auto ctxOpt = WebhookGetMatchContext();
  if (!ctxOpt) return;
  auto& st = St();
  std::lock_guard<std::mutex> lk(st.mu);
  if (st.mode != ReadyUpMode::MatchKnife) return;
  if (st.knifePhase != KnifePhase::Starting) return;  // running (dup source) or the pick round

  // mp_restartgame 1 takes a second; a Round_Start before that is stale.
  const auto now = std::chrono::steady_clock::now();
  if ((now - st.knifeTriggeredAt) < std::chrono::milliseconds(800)) {
    Debug("knife: ignoring Round_Start (%s) right after the knife restart\n", source ? source : "?");
    return;
  }
  if (map_number <= 0) map_number = 1;
  st.knifeMapNumber = map_number;
  st.knifePhase = KnifePhase::Running;
  KnifeTrackerReset(/*active=*/true);

  if (!st.knifeStartedSent) {
    WebhookEmitKnifeRoundStarted(map_number);
    st.knifeStartedSent = true;
  }
  Print("knife: round started (via %s)\n", source ? source : "?");
  if (!HudReplacesChat()) SendToChat("Ready Up: KNIFE! Winning team picks the side.");
}

void KnifeOnRoundEnd(int map_number, int csWinnerTeamNum, bool elimination, const char* source,
                     const std::string& notice) {
  auto ctxOpt = WebhookGetMatchContext();
  if (!ctxOpt) return;
  const auto& ctx = *ctxOpt;

  auto& st = St();
  std::lock_guard<std::mutex> lk(st.mu);
  if (st.mode != ReadyUpMode::MatchKnife) return;
  if (st.knifePhase != KnifePhase::Running) {
    Debug("knife: round end (%s %s) ignored in phase %d\n", source ? source : "?", notice.c_str(),
          static_cast<int>(st.knifePhase));
    return;
  }
  if (map_number <= 0) map_number = 1;

  int winnerCs = 0;
  std::string why;
  if (elimination && (csWinnerTeamNum == 2 || csWinnerTeamNum == 3)) {
    winnerCs = csWinnerTeamNum;
    st.knifeReasonShort = "elimination";
    why = std::string(CsSideName(winnerCs)) + " eliminated the other team";
  } else {
    // Time ran out / draw: more players alive, then more HP left, then random.
    const KnifeSideStats ct = KnifeTrackerStats(3);
    const KnifeSideStats t = KnifeTrackerStats(2);
    const std::string counts = "CT " + std::to_string(ct.alive) + " alive/" + std::to_string(ct.hp) + " HP vs T " +
                               std::to_string(t.alive) + " alive/" + std::to_string(t.hp) + " HP";
    if (ct.alive != t.alive) {
      winnerCs = ct.alive > t.alive ? 3 : 2;
      st.knifeReasonShort = "more alive";
      why = "time ran out, more players alive (" + counts + ")";
    } else if (ct.hp != t.hp) {
      winnerCs = ct.hp > t.hp ? 3 : 2;
      st.knifeReasonShort = "more HP";
      why = "time ran out, more HP left (" + counts + ")";
    } else {
      std::random_device rd;
      std::mt19937 gen(rd());
      winnerCs = std::uniform_int_distribution<int>(0, 1)(gen) == 0 ? 3 : 2;
      st.knifeReasonShort = "coin flip";
      why = "time ran out, dead even (" + counts + "), random pick";
    }
  }

  st.knifeWinnerCs = winnerCs;
  // Knife round is played with team1 on CT.
  st.knifeWinner = (winnerCs == 3) ? WebhookTeam::Team1 : WebhookTeam::Team2;
  st.knifeEndedSent = true;
  st.knifePhase = KnifePhase::Picking;
  st.knifeAwaitingPick = true;
  st.knifeReminderSent = false;
  KnifeTrackerReset(/*active=*/false);
  WebhookEmitKnifeRoundEnded(map_number, st.knifeWinner == WebhookTeam::Team2 ? "team2" : "team1");

  // Keep everyone knifing while the winner decides: no more round ends, respawn on.
  (void)EnqueueServerCommand("mp_ignore_round_win_conditions 1");
  (void)EnqueueServerCommand("mp_respawn_on_death_ct 1");
  (void)EnqueueServerCommand("mp_respawn_on_death_t 1");
  if (st.knifeLogdetailSet) {
    (void)EnqueueServerCommand("mp_logdetail 0");
    st.knifeLogdetailSet = false;
  }

  int sec = (ctx.slug == "scrim") ? Cfg().knife_pick_seconds : ctx.knifeDecisionSeconds;
  sec = std::max(5, std::min(300, sec));
  int humansOnWinner = 0;
  for (const auto& h : ListHumans()) {
    if (h.team == winnerCs) humansOnWinner++;
  }
  const bool botsOnly = (humansOnWinner == 0);
  if (botsOnly) sec = 3;  // nobody to ask (bots won): keep sides
  st.knifePickDeadline = std::chrono::steady_clock::now() + std::chrono::seconds(sec);

  const std::string name = KnifeWinnerNameLocked(st, ctx);
  Print("knife: winner=%s (%s) via %s notice=%s: %s; pick window %ds%s\n", CsSideName(winnerCs),
        st.knifeWinner == WebhookTeam::Team2 ? "team2" : "team1", source ? source : "?", notice.c_str(), why.c_str(),
        sec, botsOnly ? " (no humans on the winning side)" : "");
  if (!HudReplacesChat()) SendToChat(("Ready Up: " + name + " won the knife round (" + why + ").").c_str());
  if (botsOnly) {
    SendToChat("Ready Up: only bots on the winning side - keeping sides.");
  } else {
    if (!HudReplacesChat()) SendToChat(("Ready Up: " + name + " players: type .stay or .switch (.ct / .t) - " + std::to_string(sec) +
                "s, then sides stay.")
                   .c_str());
  }
}

bool KnifeApplySideChoice(const std::string& choice,
                          uint64_t pickerSteamid64,
                          const std::string& pickerName,
                          bool isAdminOverride) {
  (void)isAdminOverride;
  auto ctxOpt = WebhookGetMatchContext();
  if (!ctxOpt) return false;
  const auto& ctx = *ctxOpt;

  auto& st = St();
  std::lock_guard<std::mutex> lk(st.mu);
  if (st.mode != ReadyUpMode::MatchKnife) return false;

  const int mapNumber = st.knifeMapNumber > 0 ? st.knifeMapNumber : 1;
  return ApplyKnifeSideChoiceLocked(st, ctx, mapNumber, choice, pickerSteamid64, pickerName);
}

bool KnifeIsAwaitingPick() {
  auto& st = St();
  std::lock_guard<std::mutex> lk(st.mu);
  return st.mode == ReadyUpMode::MatchKnife && st.knifeAwaitingPick;
}

KnifeHudInfo KnifeHudSnapshot() {
  KnifeHudInfo out;
  auto ctxOpt = WebhookGetMatchContext();
  auto& st = St();
  std::lock_guard<std::mutex> lk(st.mu);
  if (st.mode != ReadyUpMode::MatchKnife) return out;
  out.phase = st.knifePhase;
  out.winnerCs = st.knifeWinnerCs;
  out.reasonShort = st.knifeReasonShort;
  if (ctxOpt && st.knifePhase == KnifePhase::Picking) out.winnerName = KnifeWinnerNameLocked(st, *ctxOpt);
  if (st.knifePhase == KnifePhase::Picking && st.knifePickDeadline.time_since_epoch().count() != 0) {
    const auto left =
        std::chrono::duration_cast<std::chrono::milliseconds>(st.knifePickDeadline - std::chrono::steady_clock::now())
            .count();
    out.secondsLeft = static_cast<int>(std::max<long long>(0, (left + 999) / 1000));
  }
  return out;
}

const char* KnifeWinnerTeamString() {
  auto& st = St();
  std::lock_guard<std::mutex> lk(st.mu);
  if (st.knifeWinner == WebhookTeam::Team1) return "team1";
  if (st.knifeWinner == WebhookTeam::Team2) return "team2";
  return "unknown";
}

bool ToggleReady(uint64_t steamid64) {
  if (steamid64 == 0) return false;
  auto& st = St();
  std::lock_guard<std::mutex> lk(st.mu);
  const bool cur = (st.ready.find(steamid64) != st.ready.end() && st.ready[steamid64]);
  st.ready[steamid64] = !cur;
  return !cur;
}

bool IsReady(uint64_t steamid64) {
  auto& st = St();
  std::lock_guard<std::mutex> lk(st.mu);
  auto it = st.ready.find(steamid64);
  return it != st.ready.end() && it->second;
}

bool SetReady(uint64_t steamid64, bool ready) {
  if (steamid64 == 0) return false;
  auto& st = St();
  std::lock_guard<std::mutex> lk(st.mu);
  const bool prev = (st.ready.find(steamid64) != st.ready.end() && st.ready[steamid64]);
  st.ready[steamid64] = ready;
  return prev;
}

void ClearReady(uint64_t steamid64) {
  if (steamid64 == 0) return;
  auto& st = St();
  std::lock_guard<std::mutex> lk(st.mu);
  st.ready.erase(steamid64);
  st.lastUi.erase(steamid64);
}

void ClearReadyStates() {
  auto& st = St();
  std::lock_guard<std::mutex> lk(st.mu);
  st.ready.clear();
  st.lastUi.clear();
}

void SetWarmupEnabled(bool enabled) {
  auto& st = St();
  std::lock_guard<std::mutex> lk(st.mu);
  st.warmupEnabled = enabled;
}

bool WarmupEnabled() {
  auto& st = St();
  std::lock_guard<std::mutex> lk(st.mu);
  return st.warmupEnabled;
}

void SetWarmupHtmlMessage(std::string html) {
  auto& st = St();
  std::lock_guard<std::mutex> lk(st.mu);
  if (html.empty()) {
    st.warmupHtml = "<b><font color='yellow'>Ready Up</font></b><br>"
                    "You are not ready yet.<br>"
                    "Type <b>.r</b> to ready up.";
    st.warmupHtmlCustom = false;
    return;
  }
  st.warmupHtml = std::move(html);
  st.warmupHtmlCustom = true;
}

std::string WarmupHtmlCustom() {
  auto& st = St();
  std::lock_guard<std::mutex> lk(st.mu);
  return st.warmupHtmlCustom ? st.warmupHtml : std::string();
}

std::string WarmupHtmlMessage() {
  auto& st = St();
  std::lock_guard<std::mutex> lk(st.mu);
  return st.warmupHtml;
}

void SetCfgExecEnabled(bool enabled) {
  auto& st = St();
  std::lock_guard<std::mutex> lk(st.mu);
  st.cfgExecEnabled = enabled;
  // If enabling while already idle, allow idle cfg to run once.
  if (enabled && st.mode == ReadyUpMode::Idle) {
    st.idleCfgExecuted = false;
  }
}

bool CfgExecEnabled() {
  auto& st = St();
  std::lock_guard<std::mutex> lk(st.mu);
  return st.cfgExecEnabled;
}

void SetWarmupRespawnEnabled(bool enabled) {
  auto& st = St();
  std::lock_guard<std::mutex> lk(st.mu);
  st.warmupRespawn = enabled;
  st.warmupRulesApplied = false;  // re-apply
}

bool WarmupRespawnEnabled() {
  auto& st = St();
  std::lock_guard<std::mutex> lk(st.mu);
  return st.warmupRespawn;
}

void SetWarmupIgnoreWinConditions(bool enabled) {
  auto& st = St();
  std::lock_guard<std::mutex> lk(st.mu);
  st.warmupIgnoreWin = enabled;
  st.warmupRulesApplied = false;
}

bool WarmupIgnoreWinConditions() {
  auto& st = St();
  std::lock_guard<std::mutex> lk(st.mu);
  return st.warmupIgnoreWin;
}

void SetWarmupRoundTimeMinutes(int minutes) {
  auto& st = St();
  std::lock_guard<std::mutex> lk(st.mu);
  st.warmupRoundTimeMinutes = std::max(1, std::min(120, minutes));
  st.warmupRulesApplied = false;
}

int WarmupRoundTimeMinutes() {
  auto& st = St();
  std::lock_guard<std::mutex> lk(st.mu);
  return st.warmupRoundTimeMinutes;
}

void SetWarmupStartMoney(int amount) {
  auto& st = St();
  std::lock_guard<std::mutex> lk(st.mu);
  st.warmupStartMoney = std::max(0, std::min(60000, amount));
  // Keep invariant: max >= start
  st.warmupMaxMoney = std::max(st.warmupMaxMoney, st.warmupStartMoney);
  st.warmupRulesApplied = false;
}

int WarmupStartMoney() {
  auto& st = St();
  std::lock_guard<std::mutex> lk(st.mu);
  return st.warmupStartMoney;
}

void SetWarmupMaxMoney(int amount) {
  auto& st = St();
  std::lock_guard<std::mutex> lk(st.mu);
  st.warmupMaxMoney = std::max(0, std::min(60000, amount));
  // Keep invariant: max >= start
  st.warmupStartMoney = std::min(st.warmupStartMoney, st.warmupMaxMoney);
  st.warmupRulesApplied = false;
}

int WarmupMaxMoney() {
  auto& st = St();
  std::lock_guard<std::mutex> lk(st.mu);
  return st.warmupMaxMoney;
}

void SetWarmupBuyAnywhereEnabled(bool enabled) {
  auto& st = St();
  std::lock_guard<std::mutex> lk(st.mu);
  st.warmupBuyAnywhere = enabled;
  st.warmupRulesApplied = false;
}

bool WarmupBuyAnywhereEnabled() {
  auto& st = St();
  std::lock_guard<std::mutex> lk(st.mu);
  return st.warmupBuyAnywhere;
}

void SetWarmupInfiniteAmmoEnabled(bool enabled) {
  auto& st = St();
  std::lock_guard<std::mutex> lk(st.mu);
  st.warmupInfiniteAmmo = enabled;
}

bool WarmupInfiniteAmmoEnabled() {
  auto& st = St();
  std::lock_guard<std::mutex> lk(st.mu);
  return st.warmupInfiniteAmmo;
}

void OnNativeWarmupStarted(const char* source) {
  if (IsDisabled()) return;
  auto& st = St();
  std::lock_guard<std::mutex> lk(st.mu);
  const ReadyUpMode m = st.mode;
  const bool emulated = (m == ReadyUpMode::Idle || m == ReadyUpMode::ScrimWarmup || m == ReadyUpMode::MatchWarmup ||
                         m == ReadyUpMode::MatchKnife);
  // Practice has no warmup either. After a map change or a restart, CS2 starts its warmup after
  // prac.cfg (and its mp_warmup_end) already ran, so practice sat in CS2's warmup until `.prac`.
  if (!emulated && m != ReadyUpMode::Practice) {
    Debug("warmup: CS2 warmup started (%s) in mode=%s; left alone\n", source ? source : "?", ModeToString(m));
    return;
  }
  // The log line and the event both fire for the same start: act once.
  const auto now = std::chrono::steady_clock::now();
  if (st.lastNativeWarmupEnd.time_since_epoch().count() != 0 &&
      (now - st.lastNativeWarmupEnd) < std::chrono::seconds(2)) {
    return;
  }
  bool any = false;
  const char* cmds[] = {"mp_warmup_pausetimer 0", "mp_warmuptime 0", "mp_team_intro_time 0", "mp_warmup_end"};
  for (const char* c : cmds) {
    if (EnqueueServerCommand(c)) any = true;
  }
  if (any) st.lastNativeWarmupEnd = now;
  Print("warmup: CS2 warmup started (%s) in mode=%s; ending it (%s)%s\n", source ? source : "?", ModeToString(m),
        m == ReadyUpMode::Practice ? "practice has no warmup" : "Ready Up emulates warmup",
        any ? "" : " - command buffer not ready");
}

void Tick() {
  // (The core registers the engine event listener; this plugin only consumes events.)
  static const auto s_boot = std::chrono::steady_clock::now();
  static std::atomic<bool> s_warned{false};
  if (!GameEventsListenerInstalled()) {
    const auto elapsed = std::chrono::steady_clock::now() - s_boot;
    if (elapsed > std::chrono::seconds(20) && !s_warned.exchange(true)) {
      // Don't fail-close: keep Ready Up warmup + chat commands alive even if engine events
      // cannot be resolved on this build. Some environments don't expose a compatible
      // GAMEEVENTS interface, but Ready Up can still function in a degraded mode.
      PrintLine("WARNING: engine game events unavailable; running in degraded mode (no engine events).");
    }
  }
  if (IsDisabled()) {
    // Ensure we don't keep suppressing termination when disabled.
    SetRoundTerminationSuppressed(false);
    return;
  }

  auto& st = St();
  static std::atomic<int> s_lastSuppress{-1};
  const bool scrimAuto = ScrimAutoEnabled();  // before st.mu (scrim_flow has its own lock)
  std::unique_lock<std::mutex> lk(st.mu);

  // MatchZy behavior: treat every map load as a fresh baseline and re-exec cfgs once. A changelevel
  // to the same map counts too: CS2's gamemode cfg overwrote the warmup cvars again.
  {
    const auto ms = MatchStateGet();
    if (!ms.current_map.empty() && (ms.current_map != st.lastSeenMap || st.mapLoaded)) {
      st.lastSeenMap = ms.current_map;
      st.mapLoaded = false;
      st.warmupMapReady = true;
      // Force re-application of cfg-driven baselines on the new map.
      st.idleCfgExecuted = false;
      st.idleIgnoreWinSent = -1;
      st.warmupRulesApplied = false;
      st.practiceRulesApplied = false;
      st.practiceResetPending = false;
      ResetKnifeStateForMapLocked(st, /*mapNumber=*/ms.map_number <= 0 ? 1 : ms.map_number);
      // Baseline: CS2's own warmup stays off (Ready Up emulates it). If CS2
      // starts it anyway, OnNativeWarmupStarted ends it.
      if (st.mode != ReadyUpMode::External) {
        (void)EnqueueServerCommand("mp_warmup_pausetimer 0");
        (void)EnqueueServerCommand("mp_warmuptime 0");
      }
      if (st.mode == ReadyUpMode::MatchKnife) {
        // Map changed mid-knife: back to warmup; the knife round restarts once
        // everyone is ready again.
        st.mode = ReadyUpMode::MatchWarmup;
        st.startTriggered = false;
        PrintLine("knife: map changed during the knife round; back to match_warmup.");
      }
      if (DebugEnabled()) {
        Debug("modes: map loaded -> will reapply cfg baselines (map=%s)\n", st.lastSeenMap.c_str());
      }
    }
  }

  // CS2 writes round backups only while the match is live (the go-live turns them on): a warmup,
  // a knife round or an idle server leaves no backup to restore.
  const bool live = st.mode == ReadyUpMode::MatchLive;
  if (st.wasLive && !live) (void)EnqueueServerCommand("mp_backup_round_auto 0");
  st.wasLive = live;

  if (st.mode == ReadyUpMode::Idle) {
    if (st.wingmanGameMode && !WebhookGetMatchContext()) {
      for (const auto& c : wingman::GameModeCommands(false)) (void)EnqueueServerCommand(c.c_str());
      st.wingmanGameMode = false;
      PrintLine("modes: wingman match over; game_type 0 / game_mode 1 (competitive) from the next map load");
    }
    if (st.cfgExecEnabled && !st.idleCfgExecuted) {
      if (EnqueueServerCommand("exec ReadyUp/idle.cfg")) {
        st.idleCfgExecuted = true;
        st.idleIgnoreWinSent = -1;  // idle.cfg may set it: send ours after
      }
    }
    // Waiting for players (scrim_auto): no round can end. `ru mode idle` (scrim_auto off) is plain
    // CS2 again: rounds end normally.
    const int want = scrimAuto ? 1 : 0;
    if (st.idleIgnoreWinSent != want &&
        EnqueueServerCommand(want ? "mp_ignore_round_win_conditions 1" : "mp_ignore_round_win_conditions 0")) {
      st.idleIgnoreWinSent = want;
    }
  } else {
    st.idleIgnoreWinSent = -1;
  }
  if (st.mode == ReadyUpMode::ScrimWarmup && !st.warmupRulesApplied) {
    const auto now = std::chrono::steady_clock::now();
    if (st.lastGateCmd.time_since_epoch().count() == 0 || (now - st.lastGateCmd) >= std::chrono::milliseconds(800)) {
      ApplyScrimWarmupRulesLocked(st);
      st.lastGateCmd = now;
    }
  }
  if (st.mode == ReadyUpMode::Practice) {
    if (!st.practiceRulesApplied) ApplyPracticeRulesLocked(st);
  } else {
    ResetPracticeRulesLocked(st);
  }
  MaybeEnterKnifeModeLocked(st);
  MaybeAutoPickKnifeSideLocked(st);
  MaybeForceRosterTeamsLocked(st);
  MaybeGateMatchLocked(st);
  MaybeShowWarmupUiLocked(st);
  EnforceWhitelistLocked(st);

  bool suppressRoundEnd =
      (!st.cfgExecEnabled) && ((st.mode == ReadyUpMode::Practice) || (st.mode == ReadyUpMode::MatchWarmup));
  const bool practiceMode = st.mode == ReadyUpMode::Practice;
  if (DebugEnabled()) {
    const int cur = suppressRoundEnd ? 1 : 0;
    const int prev = s_lastSuppress.exchange(cur);
    if (prev != cur) {
      Debug("modes: roundterm suppress=%d mode=%s\n", cur, ModeToString(st.mode));
    }
  }
  lk.unlock();
  // A practice dry run (practice plugin .dryrun) plays one normal round: its end must go through.
  if (suppressRoundEnd && practiceMode && PracticeDryRun()) suppressRoundEnd = false;
  SetRoundTerminationSuppressed(suppressRoundEnd);
}

// ---------------------------------------------------------------------------- reload state
// steady_clock time points are CLOCK_MONOTONIC: the same process, so their raw counts stay
// valid from one plugin image to the next.

namespace {
long long Tp(std::chrono::steady_clock::time_point t) { return static_cast<long long>(t.time_since_epoch().count()); }
std::chrono::steady_clock::time_point FromTp(long long v) {
  return std::chrono::steady_clock::time_point(std::chrono::steady_clock::duration(v));
}
}  // namespace

status::Json ModesSnapshotJson() {
  auto& st = St();
  std::lock_guard<std::mutex> lk(st.mu);
  status::Json j = status::Json::Object();
  j["mode"] = static_cast<int>(st.mode);
  j["external_name"] = st.externalName;
  j["external_touched_game_mode"] = st.externalTouchedGameMode;
  j["wingman_game_mode"] = st.wingmanGameMode;
  j["warmup_enabled"] = st.warmupEnabled;
  j["warmup_html"] = st.warmupHtml;
  j["warmup_html_custom"] = st.warmupHtmlCustom;
  j["cfg_exec_enabled"] = st.cfgExecEnabled;
  j["idle_cfg_executed"] = st.idleCfgExecuted;
  j["warmup_respawn"] = st.warmupRespawn;
  j["warmup_ignore_win"] = st.warmupIgnoreWin;
  j["warmup_round_time_minutes"] = st.warmupRoundTimeMinutes;
  j["warmup_start_money"] = st.warmupStartMoney;
  j["warmup_max_money"] = st.warmupMaxMoney;
  j["warmup_buy_anywhere"] = st.warmupBuyAnywhere;
  j["warmup_infinite_ammo"] = st.warmupInfiniteAmmo;
  status::Json ready = status::Json::Array();
  for (const auto& kv : st.ready) {
    if (kv.second) ready.Push(std::to_string(kv.first));
  }
  j["ready"] = std::move(ready);
  j["match_loaded_chat_sent"] = st.matchLoadedChatSent;
  j["last_not_ready_chat"] = Tp(st.lastNotReadyChat);
  j["last_seen_map"] = st.lastSeenMap;
  j["warmup_rules_applied"] = st.warmupRulesApplied;
  j["start_triggered"] = st.startTriggered;
  j["last_gate_cmd"] = Tp(st.lastGateCmd);
  j["recovery_gate"] = st.recoveryGate;
  j["full_roster_gate"] = st.fullRosterGate;
  j["lifecycle_map_number"] = st.lifecycleMapNumber;
  j["warmup_ended_sent"] = st.warmupEndedSent;
  j["going_live_sent"] = st.goingLiveSent;
  j["practice_rules_applied"] = st.practiceRulesApplied;
  j["practice_reset_pending"] = st.practiceResetPending;
  j["demo_recording"] = st.demoRecording;
  j["demo_map_number"] = st.demoMapNumber;
  j["demo_name"] = st.demoName;
  j["map_result_emitted_for"] = st.mapResultEmittedForMapNumber;
  j["series_wins_team1"] = st.seriesWinsTeam1;
  j["series_wins_team2"] = st.seriesWinsTeam2;
  status::Json k = status::Json::Object();
  k["map_number"] = st.knifeMapNumber;
  k["rules_applied"] = st.knifeRulesApplied;
  k["started_sent"] = st.knifeStartedSent;
  k["ended_sent"] = st.knifeEndedSent;
  k["winner"] = static_cast<int>(st.knifeWinner);
  k["awaiting_pick"] = st.knifeAwaitingPick;
  k["pick_deadline"] = Tp(st.knifePickDeadline);
  k["phase"] = static_cast<int>(st.knifePhase);
  k["triggered_at"] = Tp(st.knifeTriggeredAt);
  k["winner_cs"] = st.knifeWinnerCs;
  k["reason_short"] = st.knifeReasonShort;
  k["reminder_sent"] = st.knifeReminderSent;
  k["logdetail_set"] = st.knifeLogdetailSet;
  j["knife"] = std::move(k);
  j["last_native_warmup_end"] = Tp(st.lastNativeWarmupEnd);
  return j;
}

void ModesRestoreJson(const status::Json& j) {
  auto& st = St();
  std::lock_guard<std::mutex> lk(st.mu);
  auto b = [&](const status::Json* o, const char* key, bool& out) {
    if (const auto* v = o ? o->Find(key) : nullptr) out = v->AsBool();
  };
  auto i = [&](const status::Json* o, const char* key, int& out) {
    if (const auto* v = o ? o->Find(key) : nullptr) out = static_cast<int>(v->AsInt());
  };
  auto s = [&](const status::Json* o, const char* key, std::string& out) {
    if (const auto* v = o ? o->Find(key) : nullptr) out = v->AsString();
  };
  auto t = [&](const status::Json* o, const char* key, std::chrono::steady_clock::time_point& out) {
    if (const auto* v = o ? o->Find(key) : nullptr) out = FromTp(v->AsInt());
  };
  int mode = static_cast<int>(st.mode);
  i(&j, "mode", mode);
  st.mode = static_cast<ReadyUpMode>(mode);
  s(&j, "external_name", st.externalName);
  b(&j, "external_touched_game_mode", st.externalTouchedGameMode);
  b(&j, "wingman_game_mode", st.wingmanGameMode);
  b(&j, "warmup_enabled", st.warmupEnabled);
  s(&j, "warmup_html", st.warmupHtml);
  b(&j, "warmup_html_custom", st.warmupHtmlCustom);
  b(&j, "cfg_exec_enabled", st.cfgExecEnabled);
  b(&j, "idle_cfg_executed", st.idleCfgExecuted);
  b(&j, "warmup_respawn", st.warmupRespawn);
  b(&j, "warmup_ignore_win", st.warmupIgnoreWin);
  i(&j, "warmup_round_time_minutes", st.warmupRoundTimeMinutes);
  i(&j, "warmup_start_money", st.warmupStartMoney);
  i(&j, "warmup_max_money", st.warmupMaxMoney);
  b(&j, "warmup_buy_anywhere", st.warmupBuyAnywhere);
  b(&j, "warmup_infinite_ammo", st.warmupInfiniteAmmo);
  st.ready.clear();
  if (const auto* r = j.Find("ready")) {
    for (const auto& v : r->Items()) {
      const uint64_t sid = std::strtoull(v.AsString().c_str(), nullptr, 10);
      if (sid) st.ready[sid] = true;
    }
  }
  b(&j, "match_loaded_chat_sent", st.matchLoadedChatSent);
  t(&j, "last_not_ready_chat", st.lastNotReadyChat);
  s(&j, "last_seen_map", st.lastSeenMap);
  b(&j, "warmup_rules_applied", st.warmupRulesApplied);
  b(&j, "start_triggered", st.startTriggered);
  t(&j, "last_gate_cmd", st.lastGateCmd);
  b(&j, "recovery_gate", st.recoveryGate);
  b(&j, "full_roster_gate", st.fullRosterGate);
  i(&j, "lifecycle_map_number", st.lifecycleMapNumber);
  b(&j, "warmup_ended_sent", st.warmupEndedSent);
  b(&j, "going_live_sent", st.goingLiveSent);
  b(&j, "practice_rules_applied", st.practiceRulesApplied);
  b(&j, "practice_reset_pending", st.practiceResetPending);
  b(&j, "demo_recording", st.demoRecording);
  i(&j, "demo_map_number", st.demoMapNumber);
  s(&j, "demo_name", st.demoName);
  i(&j, "map_result_emitted_for", st.mapResultEmittedForMapNumber);
  i(&j, "series_wins_team1", st.seriesWinsTeam1);
  i(&j, "series_wins_team2", st.seriesWinsTeam2);
  const status::Json* k = j.Find("knife");
  i(k, "map_number", st.knifeMapNumber);
  b(k, "rules_applied", st.knifeRulesApplied);
  b(k, "started_sent", st.knifeStartedSent);
  b(k, "ended_sent", st.knifeEndedSent);
  int winner = static_cast<int>(st.knifeWinner);
  i(k, "winner", winner);
  st.knifeWinner = static_cast<WebhookTeam>(winner);
  b(k, "awaiting_pick", st.knifeAwaitingPick);
  t(k, "pick_deadline", st.knifePickDeadline);
  int phase = static_cast<int>(st.knifePhase);
  i(k, "phase", phase);
  st.knifePhase = static_cast<KnifePhase>(phase);
  t(k, "triggered_at", st.knifeTriggeredAt);
  i(k, "winner_cs", st.knifeWinnerCs);
  s(k, "reason_short", st.knifeReasonShort);
  b(k, "reminder_sent", st.knifeReminderSent);
  b(k, "logdetail_set", st.knifeLogdetailSet);
  t(&j, "last_native_warmup_end", st.lastNativeWarmupEnd);
}

}  // namespace readyup


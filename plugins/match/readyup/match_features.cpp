// Tactical / technical pauses, .forceready and the team-left forfeit (see match_features.h).
#include "readyup/match_features.h"
#include "readyup/match_status.h"

#include "readyup/config.h"
#include "readyup/engine.h"
#include "readyup/esports.h"
#include "readyup/fleet_bridge.h"
#include "readyup/host.h"
#include "readyup/logging.h"
#include "readyup/match_events.h"
#include "readyup/match_state.h"
#include "readyup/modes.h"
#include "readyup/pause_state.h"
#include "readyup/players.h"
#include "readyup/scrim_flow.h"
#include "readyup/simulation.h"

#include <algorithm>
#include <atomic>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <set>
#include <string>
#include <unordered_set>
#include <vector>

namespace readyup {
namespace {

// Engine events (game thread) -> tick.
std::atomic<int> g_roundStartSeq{0};
std::atomic<int> g_freezeEndSeq{0};
std::atomic<bool> g_inFreeze{false};
std::atomic<bool> g_roundOver{false};  // round_end .. round_start

// Game thread only.
int g_rsSeqAuto5v5 = 0;  // the round start the auto_5v5 check last ran for
bool g_auto5v5Pending = false;  // this round's auto_5v5 check still runs (until freeze time ends)
double g_auto5v5NextAt = 0;
double g_auto5v5CountAt = 0;  // the paused branch's side count, every 250 ms
int g_auto5v5Ct = 0, g_auto5v5T = 0;
double g_auto5v5FullSince = 0;  // when both sides were last seen full during an auto_5v5 pause (0: not)
bool g_wasPaused = false;
// Engine pauses Ready Up did not start (EnginePauseTick).
ReadyUpMode g_epMode = ReadyUpMode::Idle;  // the mode at the last check
double g_epWindowFrom = 0, g_epWindowUntil = 0;  // the go-live window (0: none)
bool g_epCleared = false;                         // this window already sent mp_unpause_match
double g_epNextAt = 0;
double g_epUnmarkedSince = 0;  // engine paused, no Ready Up pause, since (0: not)
double g_epEngineFreeSince = 0;  // an "engine" pause recorded, engine not paused, since (0: not)
double g_effectiveAt = 0;  // technical: when the pause took effect (0 = waiting for freeze time)
int g_rsSeqAtPause = 0;
int g_feSeqAtPause = 0;
bool g_warnedAutoUnpause = false;
ForfeitTimer g_ff;
uint64_t g_ffMatch = 0;
int g_ffAnnounced = -1;
double g_ffLastEval = 0;

// HUD view (any thread).
std::mutex g_hudMu;
LiveHudInfo g_hud;

const char* TeamKey(WebhookTeam t) {
  return t == WebhookTeam::Team1 ? "team1" : t == WebhookTeam::Team2 ? "team2" : "unknown";
}
int TeamNum(WebhookTeam t) { return t == WebhookTeam::Team1 ? 1 : t == WebhookTeam::Team2 ? 2 : 0; }

std::string TeamName(const WebhookMatchContext& ctx, WebhookTeam t) {
  const std::string& n = t == WebhookTeam::Team1 ? ctx.team1_name : ctx.team2_name;
  if (!n.empty()) return n;
  return t == WebhookTeam::Team1 ? "Team1" : "Team2";
}

void Reply(uint64_t steamid64, const std::string& msg) {
  if (steamid64 != 0) SendToChat(msg.c_str());
  else PrintLine(msg.c_str());
}

bool LiveMap() { return GetMode() == ReadyUpMode::MatchLive; }

// Current CS side of a team (3 CT / 2 T) from the map side and the swaps so far.
int CsSideOf(const WebhookMatchContext& ctx, WebhookTeam team) {
  const int mapNum = std::max(1, MatchStateGet().map_number);
  bool team1Ct = !(static_cast<size_t>(mapNum) <= ctx.map_sides.size() &&
                   ctx.map_sides[static_cast<size_t>(mapNum - 1)] == "team2_ct");
  if (MatchEventsSnapshot().swapCount % 2 != 0) team1Ct = !team1Ct;
  const bool ct = (team == WebhookTeam::Team1) == team1Ct;
  return ct ? 3 : 2;
}

void Unpause(const char* why) {
  const int dur = PauseStatePauseDurationSeconds();
  PauseStateOnUnpaused();
  g_wasPaused = false;
  WebhookEmitMatchUnpaused(MatchStateGet().map_number, dur);
  Print("pause: ended (%s) after %ds\n", why, dur);
}

// A team is there if one of its roster players is connected. Teams without a roster (bot tests:
// livetest's empty roster, dev_bots_scrim), the dev bot flags and simulated matches count bots on
// the team's side.
bool TeamPresent(const WebhookMatchContext& ctx, WebhookTeam team, const std::unordered_set<uint64_t>& connected,
                 const std::vector<BotIdentity>& bots) {
  bool hasRoster = false;
  for (const auto& kv : ctx.roster_team) {
    if (kv.second != team) continue;
    hasRoster = true;
    if (connected.count(kv.first)) return true;
  }
  if (hasRoster && !DevBotsReadyEnabled() && !DevBotsScrimEnabled() && !SimulationActive()) return false;
  const int side = CsSideOf(ctx, team);
  for (const auto& b : bots) {
    if (b.team == side) return true;
  }
  return false;
}

void ForfeitTick(double now) {
  if (now - g_ffLastEval < 0.25) return;
  g_ffLastEval = now;
  const auto ctx = WebhookGetMatchContext();
  const uint64_t mid = ctx ? ctx->matchid : 0;
  if (mid != g_ffMatch) {
    g_ff.Reset();
    g_ffMatch = mid;
    g_ffAnnounced = -1;
  }
  const int limit = EffectiveRules().forfeit_after_seconds;
  // A scrim has no roster and nothing to forfeit: a human taking a bot's place
  // must not start a countdown for the team the bot left.
  const bool active = ctx && limit > 0 && LiveMap() && ctx->slug != "scrim";
  bool p1 = true, p2 = true;
  if (active) {
    std::unordered_set<uint64_t> connected;
    for (const auto& h : ListHumans()) connected.insert(h.steamid64);
    const auto bots = ListBots();
    p1 = TeamPresent(*ctx, WebhookTeam::Team1, connected, bots);
    p2 = TeamPresent(*ctx, WebhookTeam::Team2, connected, bots);
  }
  const auto r = g_ff.Update(now, active, limit, p1, p2);
  const WebhookTeam absent = r.absentTeam == 1 ? WebhookTeam::Team1 : r.absentTeam == 2 ? WebhookTeam::Team2
                                                                                        : WebhookTeam::Unknown;
  const std::string name = ctx && absent != WebhookTeam::Unknown ? TeamName(*ctx, absent) : std::string();
  switch (r.action) {
    case ForfeitTimer::Action::Started:
      Print("forfeit: %s has nobody connected; forfeit in %ds\n", TeamKey(absent), r.secondsLeft);
      SendToChat(("Ready Up: " + name + " has no players on the server. They forfeit in " +
                  std::to_string(r.secondsLeft) + "s unless someone reconnects.")
                     .c_str());
      g_ffAnnounced = r.secondsLeft;
      break;
    case ForfeitTimer::Action::Cancelled:
      Print("forfeit: %s is back; countdown cancelled\n", TeamKey(absent));
      SendToChat(("Ready Up: " + name + " is back. Forfeit cancelled.").c_str());
      g_ffAnnounced = -1;
      break;
    case ForfeitTimer::Action::Expired:
      Print("forfeit: %s did not come back in %ds\n", TeamKey(absent), limit);
      SendToChat(("Ready Up: " + name + " did not come back and forfeits the match.").c_str());
      if (!ForfeitCurrentMap(absent, "team_absent")) Print("forfeit: no live map to forfeit\n");
      g_ffAnnounced = -1;
      break;
    case ForfeitTimer::Action::None:
      if (g_ff.Running()) {
        for (int mark : {120, 60, 30, 10}) {
          if (r.secondsLeft <= mark && g_ffAnnounced > mark) {
            SendToChat(("Ready Up: " + name + " forfeits in " + std::to_string(r.secondsLeft) + "s.").c_str());
            g_ffAnnounced = mark;
            break;
          }
        }
      }
      break;
  }
}

// sv_matchpause_auto_5v5 (on under the valve ruleset, esports.h): at a round start of a live map
// with a side short of 5 players the engine pauses the match in freeze time. Ready Up marks that
// pause "auto_5v5" (it belongs to nobody: both teams .unpause, or an admin; it counts against
// neither team). If the engine did not pause after all, the freeze time ends and the tick clears it.
// People on each side. CS2 counts only people for this pause: a side of one player and four bots
// is paused (2026-10-14), so bots counted here left that pause unmarked and unexplained in chat.
void CountSides(int* ct, int* t) {
  *ct = 0;
  *t = 0;
  // The engine's team (a byte per player) over the cached one, which a map load leaves at 0 for a
  // player who stays on their side (team_lock.h). Called at most every 250 ms.
  for (const auto& h : ListHumans()) {
    const int team = EngineTeamForSlot(h.slot).value_or(h.team);
    if (team == 3) ++*ct;
    else if (team == 2) ++*t;
  }
}

std::mutex g_leftMu;
std::set<uint64_t> g_left;  // rostered players who left a live map, for the "back" line

// True when the check is over for this round (marked, or nothing to mark); false = look again.
bool MaybeMarkAuto5v5Pause() {
  const auto ctx = WebhookGetMatchContext();
  if (!ctx) return true;
  int ct = 0, t = 0;
  CountSides(&ct, &t);
  const Auto5v5Check c = Auto5v5Decide(AutoPause5v5Enabled(), LiveMap(), PauseStateGet().paused, EngineMatchPaused(), ct, t);
  if (c == Auto5v5Check::Wait) return false;
  if (c == Auto5v5Check::Done) return true;
  const int side = Auto5v5ShortSide(ct, t);
  const WebhookTeam shortTeam = CsSideOf(*ctx, WebhookTeam::Team1) == side ? WebhookTeam::Team1 : WebhookTeam::Team2;
  PauseStateOnPaused("auto_5v5", "server", shortTeam);
  WebhookEmitMatchPaused(MatchStateGet().map_number, WebhookPlayer{0, "server", shortTeam}, /*is_tactical=*/false,
                         /*is_admin=*/false, /*pause_time=*/0);
  Print("pause: auto_5v5 (sv_matchpause_auto_5v5 1): CT %d, T %d players; %s is short\n", ct, t, TeamKey(shortTeam));
  const int maxSeconds = EffectiveRules().disconnect_pause_max_seconds;
  SendToChat(("Ready Up: match paused automatically - " + TeamName(*ctx, shortTeam) +
              " is missing a player. It goes on when they are back" +
              (maxSeconds > 0 ? " (or in " + std::to_string(maxSeconds / 60) + " min)" : std::string()) +
              ". To go on now, both teams type .up.")
                 .c_str());
  return true;
}


// The go-live window: from 1 s after the knife round starts / the map goes live / a resume goes
// live (the go-live's own commands land first: the recovery gate sends mp_unpause_match itself)
// until 10 s after.
constexpr double kGoLiveWindowDelay = 1.0, kGoLiveWindowSeconds = 10.0;

bool Playing(ReadyUpMode m) { return m == ReadyUpMode::MatchKnife || m == ReadyUpMode::MatchLive; }

// An engine pause Ready Up did not start (match_rules.h DecideEnginePause): the map-load
// auto_5v5 pause cleared at go-live, else recorded so the platform shows it and `.up` works.
// Every 250 ms; game thread.
void EnginePauseTick(double now) {
  const ReadyUpMode mode = GetMode();
  // From match warmup only: a plugin reload mid-map (Idle -> live) is no go-live.
  if (Playing(mode) && g_epMode == ReadyUpMode::MatchWarmup) {
    g_epWindowFrom = now + kGoLiveWindowDelay;
    g_epWindowUntil = now + kGoLiveWindowDelay + kGoLiveWindowSeconds;
    g_epCleared = false;
  }
  if (!Playing(mode)) g_epWindowUntil = 0;
  g_epMode = mode;
  if (now < g_epNextAt) return;
  g_epNextAt = now + 0.25;
  const auto ctx = WebhookGetMatchContext();
  if (!ctx) {
    g_epUnmarkedSince = g_epEngineFreeSince = 0;
    return;
  }
  const int engine = EngineMatchPaused();
  const PauseSnapshot ps = PauseStateGet();
  // A recorded engine pause the engine no longer has (an admin's console unpause): over.
  if (ps.paused && ps.type == "engine" && engine == 0) {
    if (g_epEngineFreeSince == 0) g_epEngineFreeSince = now;
    if (now - g_epEngineFreeSince >= 1.0) {
      g_epEngineFreeSince = 0;
      Unpause("the engine is not paused any more");
    }
    return;
  }
  g_epEngineFreeSince = 0;
  if (engine == 1 && !ps.paused && Playing(mode)) {
    if (g_epUnmarkedSince == 0) g_epUnmarkedSince = now;
  } else {
    g_epUnmarkedSince = 0;
  }
  if (now < g_epWindowFrom && g_epWindowUntil > 0) return;  // the go-live's own commands first
  const bool window = !g_epCleared && g_epWindowUntil > 0 && now <= g_epWindowUntil;
  int ct = 0, t = 0;
  if (engine == 1 && !ps.paused) CountSides(&ct, &t);
  const int full = ctx->players_per_team > 0 ? ctx->players_per_team : kFullTeamPlayers;
  const EnginePauseAction a = DecideEnginePause(engine, ps.paused, Playing(mode), window, AutoPause5v5Enabled(), ct, t,
                                                full, g_epUnmarkedSince > 0 ? now - g_epUnmarkedSince : 0);
  if (a == EnginePauseAction::None) return;
  if (a == EnginePauseAction::Clear) {
    if (!EnqueueServerCommand("mp_unpause_match")) return;  // tried again next check
    g_epCleared = true;
    g_epUnmarkedSince = 0;
    Print("pause: cleared a stale engine pause at go-live (auto_5v5 from map load): CT %d, T %d players, mode %s\n",
          ct, t, mode == ReadyUpMode::MatchKnife ? "knife" : "live");
    return;
  }
  const bool autoType = a == EnginePauseAction::MarkAuto5v5;
  WebhookTeam shortTeam = WebhookTeam::Unknown;
  if (ct < full || t < full) {
    const int side = ct <= t ? 3 : 2;
    shortTeam = CsSideOf(*ctx, WebhookTeam::Team1) == side ? WebhookTeam::Team1 : WebhookTeam::Team2;
  }
  PauseStateOnPaused(autoType ? "auto_5v5" : "engine", "server", shortTeam);
  WebhookEmitMatchPaused(MatchStateGet().map_number, WebhookPlayer{0, "server", shortTeam}, /*is_tactical=*/false,
                         /*is_admin=*/false, /*pause_time=*/0);
  g_epUnmarkedSince = 0;
  Print("pause: %s (engine paused, not by Ready Up%s): CT %d, T %d players%s%s\n", autoType ? "auto_5v5" : "engine",
        window ? ", at go-live" : "", ct, t, shortTeam != WebhookTeam::Unknown ? "; short: " : "",
        shortTeam != WebhookTeam::Unknown ? TeamKey(shortTeam) : "");
  if (shortTeam != WebhookTeam::Unknown) {
    SendToChat(("Ready Up: match paused - " + TeamName(*ctx, shortTeam) +
                " is missing a player. To go on now, both teams type .up.")
                   .c_str());
  } else {
    SendToChat("Ready Up: the game is paused. To go on, both teams type .up.");
  }
}

}  // namespace

// The ruleset (esports.h): preset, the match config and readyup.cfg keys, overrides.
MatchRules EffectiveRules() { return CurrentEffectiveRules().match_rules; }

void MatchFeaturesOnGameEvent(const char* name) {
  if (!name) return;
  if (std::strcmp(name, "round_start") == 0) {
    g_inFreeze.store(true);
    g_roundOver.store(false);
    g_roundStartSeq.fetch_add(1);
  } else if (std::strcmp(name, "round_end") == 0) {
    g_roundOver.store(true);
  } else if (std::strcmp(name, "round_announce_warmup") == 0) {
    g_roundOver.store(false);
  } else if (std::strcmp(name, "round_freeze_end") == 0) {
    g_inFreeze.store(false);
    g_freezeEndSeq.fetch_add(1);
  }
}

bool MatchFeaturesInFreeze() { return g_inFreeze.load(); }

bool MatchFeaturesRoundOver() { return g_roundOver.load(); }

void MatchFeaturesTechPause(WebhookTeam team, uint64_t steamid64, const std::string& name) {
  const auto ctx = WebhookGetMatchContext();
  if (!ctx) return Reply(steamid64, "Ready Up: no match loaded.");
  if (!LiveMap()) return Reply(steamid64, "Ready Up: pauses only work while the map is live.");
  if (PauseStateGet().paused) return Reply(steamid64, "Ready Up: match is already paused.");
  const MatchRules rules = EffectiveRules();
  const int used = PauseStateUsed(team, "technical");
  const std::string tn = TeamName(*ctx, team);
  if (!TechPauseAllowed(used, rules.tech_pauses_per_team)) {
    Print("pause: technical refused for %s (%d/%d used)\n", TeamKey(team), used, rules.tech_pauses_per_team);
    // "(3/3)" read as three left; say it in words (2026-10-15).
    return Reply(steamid64, "Ready Up: " + tn + " has used all " + std::to_string(rules.tech_pauses_per_team) +
                                " technical pauses; none left.");
  }
  if (!EnqueueServerCommand("mp_pause_match")) return Reply(steamid64, "Ready Up: pause unavailable yet.");
  PauseStateOnPaused("technical", steamid64 ? std::to_string(steamid64) : std::string("Console"), team);
  PauseStateCountUse(team, "technical");
  WebhookEmitMatchPaused(MatchStateGet().map_number, WebhookPlayer{steamid64, name, team}, /*is_tactical=*/false,
                         /*is_admin=*/steamid64 == 0, /*pause_time=*/rules.tech_pause_max_seconds);
  const int left = TechPausesLeft(used + 1, rules.tech_pauses_per_team);
  Print("pause: technical by %s (%s) used=%d/%d max_seconds=%d\n", TeamKey(team), name.c_str(), used + 1,
        rules.tech_pauses_per_team, rules.tech_pause_max_seconds);
  std::string msg = "Ready Up: technical pause by " + tn + (g_inFreeze.load() ? "" : " (at freeze time)");
  if (left >= 0) msg += " - " + std::to_string(left) + " left";
  SendToChat((msg + ".").c_str());
}

void MatchFeaturesTacticalTimeout(WebhookTeam team, uint64_t steamid64, const std::string& name) {
  const auto ctx = WebhookGetMatchContext();
  if (!ctx) return Reply(steamid64, "Ready Up: no match loaded.");
  if (!LiveMap()) return Reply(steamid64, "Ready Up: timeouts only work while the map is live.");
  if (PauseStateGet().paused) return Reply(steamid64, "Ready Up: match is already paused.");
  // The caller's side (a player), else the team's side from the map sides and swaps.
  int side = 0;
  if (steamid64 != 0) {
    for (const auto& h : ListHumans()) {
      if (h.steamid64 == steamid64) side = h.team;
    }
  }
  if (side != 2 && side != 3) side = CsSideOf(*ctx, team);
  // The engine enforces mp_team_timeout_max; refuse early when the match config sets it.
  const int used = PauseStateUsed(team, "tactical");
  if (auto it = ctx->cvars.find("mp_team_timeout_max"); it != ctx->cvars.end()) {
    const int max = std::atoi(it->second.c_str());
    if (used >= max) {
      return Reply(steamid64, "Ready Up: " + TeamName(*ctx, team) + " has no tactical timeouts left.");
    }
  }
  int seconds = 30;
  if (auto it = ctx->cvars.find("mp_team_timeout_time"); it != ctx->cvars.end()) seconds = std::atoi(it->second.c_str());
  if (!EnqueueServerCommand(side == 3 ? "timeout_ct_start" : "timeout_terrorist_start")) {
    return Reply(steamid64, "Ready Up: timeout unavailable yet.");
  }
  PauseStateOnPaused("tactical", steamid64 ? std::to_string(steamid64) : std::string("Console"), team);
  PauseStateCountUse(team, "tactical");
  WebhookEmitMatchPaused(MatchStateGet().map_number, WebhookPlayer{steamid64, name, team}, /*is_tactical=*/true,
                         /*is_admin=*/steamid64 == 0, /*pause_time=*/seconds);
  Print("pause: tactical timeout by %s (%s) side=%s used=%d\n", TeamKey(team), name.c_str(), side == 3 ? "CT" : "T",
        used + 1);
  SendToChat(("Ready Up: tactical timeout for " + TeamName(*ctx, team) +
              (g_inFreeze.load() ? "." : " (at freeze time)."))
                 .c_str());
}

void MatchFeaturesUnpause(WebhookTeam team, uint64_t steamid64, const std::string& name) {
  (void)name;
  const auto ctx = WebhookGetMatchContext();
  if (!ctx) return Reply(steamid64, "Ready Up: no match loaded.");
  const PauseSnapshot cur = PauseStateGet();
  if (!cur.paused) return Reply(steamid64, "Ready Up: match is not paused.");
  if (cur.type == "tactical") return Reply(steamid64, "Ready Up: the tactical timeout ends by itself.");
  if (cur.type == "admin") return Reply(steamid64, "Ready Up: an admin paused the match; only an admin can unpause (.fup).");
  const MatchRules rules = EffectiveRules();
  const int mapNumber = MatchStateGet().map_number;
  auto snap = PauseStateRequestUnpause(team);
  // dev_bots_ready / simulation: a team with no connected human on the roster is all bots (or
  // empty); nobody can confirm for it, so confirm on its behalf.
  if (DevBotsReadyEnabled() || SimulationActive()) {
    std::unordered_set<uint64_t> connected;
    for (const auto& h : ListHumans()) connected.insert(h.steamid64);
    auto botOnly = [&](WebhookTeam t) {
      for (const auto& kv : ctx->roster_team) {
        if (kv.second == t && connected.count(kv.first)) return false;
      }
      return true;
    };
    const std::pair<WebhookTeam, bool> teams[] = {{WebhookTeam::Team1, snap.team1_ready_to_unpause},
                                                  {WebhookTeam::Team2, snap.team2_ready_to_unpause}};
    for (const auto& t : teams) {
      if (t.second || !botOnly(t.first)) continue;
      snap = PauseStateRequestUnpause(t.first);
      Print("dev_bots_ready: auto-confirmed unpause for bot-only %s\n", TeamKey(t.first));
      SendToChat(("Ready Up: dev_bots_ready - unpause confirmed for bot-only team " + TeamName(*ctx, t.first) + ".")
                     .c_str());
    }
  }
  // The halftime pause (mp_halftime_pausematch, esports.h) and the engine's auto 5v5 pause
  // (sv_matchpause_auto_5v5) belong to nobody: both teams resume them.
  // A restore pause (fleet_bridge DoRestoreNow) is nobody's either: both teams confirm, and so is
  // an engine pause Ready Up did not start (EnginePauseTick).
  const bool both = rules.both_teams_unpause != 0 || cur.type == "halftime" || cur.type == "auto_5v5" ||
                    cur.type == "restore" || cur.type == "engine";
  const int teamsReady = (snap.team1_ready_to_unpause ? 1 : 0) + (snap.team2_ready_to_unpause ? 1 : 0);
  WebhookEmitUnpauseRequested(mapNumber, team, teamsReady, both ? 2 : 1);
  if (UnpauseSatisfied(snap.team1_ready_to_unpause, snap.team2_ready_to_unpause, both, TeamNum(cur.team))) {
    if (!EnqueueServerCommand("mp_unpause_match")) return Reply(steamid64, "Ready Up: unpause unavailable yet.");
    Unpause("players");
    SendToChat("Ready Up: unpause accepted.");
  } else if (both) {
    SendToChat(("Ready Up: " + TeamName(*ctx, team) + " wants to unpause (waiting for the other team).").c_str());
  } else {
    SendToChat(("Ready Up: waiting for " + TeamName(*ctx, cur.team) + " (they paused) to unpause.").c_str());
  }
}

void MatchFeaturesForceReady(uint64_t steamid64, const std::string& name) {
  const MatchRules rules = EffectiveRules();
  if (!rules.allow_force_ready) return Reply(steamid64, "Ready Up: .forceready is disabled on this server.");
  const auto ctx = WebhookGetMatchContext();
  const ReadyUpMode mode = GetMode();
  const auto humans = ListHumans();
  int readied = 0;
  std::string teamName;
  if (!ctx) {
    // Scrim warmup: everyone on the caller's side.
    if (mode != ReadyUpMode::ScrimWarmup) return Reply(steamid64, "Ready Up: .forceready only works in warmup.");
    const ScrimRoster roster = BuildScrimRoster();
    const auto me = roster.teamNum.find(steamid64);
    if (me == roster.teamNum.end()) return Reply(steamid64, "Ready Up: join CT or T first.");
    for (const auto& kv : roster.teamNum) {
      if (kv.second == me->second && !SetReady(kv.first, true)) ++readied;
    }
    teamName = me->second == 3 ? "CT" : "T";
  } else {
    if (mode != ReadyUpMode::MatchWarmup || GoLiveTriggered() || KnifeIsAwaitingPick()) {
      return Reply(steamid64, "Ready Up: .forceready only works in warmup.");
    }
    const auto it = ctx->roster_team.find(steamid64);
    if (it == ctx->roster_team.end()) return Reply(steamid64, "Ready Up: you are not on this match's roster.");
    const WebhookTeam team = it->second;
    std::unordered_set<uint64_t> connected;
    for (const auto& h : humans) connected.insert(h.steamid64);
    int rosterSize = 0, present = 0;
    for (const auto& kv : ctx->roster_team) {
      if (kv.second != team) continue;
      ++rosterSize;
      if (connected.count(kv.first)) ++present;
    }
    teamName = TeamName(*ctx, team);
    const int minPlayers = ReadyMinPlayers(rules.min_players_to_ready, FullRosterGateEnabled());
    if (!ForceReadyAllowed(present, rosterSize, minPlayers, ctx->players_per_team)) {
      return Reply(steamid64, "Ready Up: " + teamName + " needs " +
                                  std::to_string(ForceReadyRequired(rosterSize, minPlayers, ctx->players_per_team)) +
                                  " players connected to force ready (" + std::to_string(present) + " now).");
    }
    for (const auto& kv : ctx->roster_team) {
      if (kv.second == team && connected.count(kv.first) && !SetReady(kv.first, true)) ++readied;
    }
  }
  ScrimNoteReadyChanged();
  Print("ready: .forceready by %s -> %s, %d player(s) readied\n", name.c_str(), teamName.c_str(), readied);
  SendToChat(("Ready Up: " + name + " readied " + teamName + " (" + std::to_string(readied) + " player" +
              (readied == 1 ? "" : "s") + ").")
                 .c_str());
}

bool MatchFeaturesTeamReady(uint64_t steamid64, const std::string& name, bool ready, std::vector<uint64_t>* changed) {
  const MatchRules rules = EffectiveRules();
  if (!rules.team_ready) return false;
  const auto ctx = WebhookGetMatchContext();
  if (!ctx || ctx->slug == "scrim") return false;
  if (GetMode() != ReadyUpMode::MatchWarmup || GoLiveTriggered() || KnifeIsAwaitingPick()) return false;
  const auto it = ctx->roster_team.find(steamid64);
  if (it == ctx->roster_team.end()) return false;
  const WebhookTeam team = it->second;
  // The same count as .forceready's.
  std::unordered_set<uint64_t> connected;
  for (const auto& h : ListHumans()) connected.insert(h.steamid64);
  int rosterSize = 0, present = 0;
  for (const auto& kv : ctx->roster_team) {
    if (kv.second != team) continue;
    ++rosterSize;
    if (connected.count(kv.first)) ++present;
  }
  const std::string teamName = TeamName(*ctx, team);
  switch (DecideTeamReady(true, ready, present, rosterSize, rules.min_players_to_ready, ctx->players_per_team)) {
    case TeamReadyAction::Single: return false;
    case TeamReadyAction::NotEnough:
      Reply(steamid64, "Ready Up: " + teamName + " needs " +
                           std::to_string(ForceReadyRequired(rosterSize, rules.min_players_to_ready, ctx->players_per_team)) +
                           " players connected to ready the whole team (" + std::to_string(present) + " now).");
      return false;
    case TeamReadyAction::Team: break;
  }
  int n = 0;
  for (const auto& kv : ctx->roster_team) {
    if (kv.second != team || !connected.count(kv.first)) continue;
    if (SetReady(kv.first, ready) != ready) {
      ++n;
      if (changed) changed->push_back(kv.first);
    }
  }
  ScrimNoteReadyChanged();
  Print("ready: team_ready %s by %s -> %s, %d player(s)\n", ready ? ".ready" : ".unready", name.c_str(),
        teamName.c_str(), n);
  SendToChat(("Ready Up: " + name + (ready ? " readied " : " unreadied ") + teamName + " (" + std::to_string(n) +
              " player" + (n == 1 ? "" : "s") + ").")
                 .c_str());
  return true;
}

bool MatchFeaturesSwapTeams(std::string* why) {
  const auto ctx = WebhookGetMatchContext();
  if (!ctx || ctx->slug == "scrim") {
    if (why) *why = "no match loaded";
    return false;
  }
  if (GetMode() != ReadyUpMode::MatchWarmup || GoLiveTriggered() || KnifeIsAwaitingPick()) {
    if (why) *why = "teams can only be swapped in warmup";
    return false;
  }
  const auto ms = MatchStateGet();
  const int mapNumber = std::max(1, ms.map_number);
  const size_t idx = static_cast<size_t>(mapNumber - 1);
  if (idx < ctx->map_sides.size()) {
    const std::string& side = ctx->map_sides[idx];
    if (side == "team1_ct") WebhookUpdateMapSide(mapNumber, "team2_ct");
    else if (side == "team2_ct") WebhookUpdateMapSide(mapNumber, "team1_ct");
  }
  if (!EnqueueServerCommand("mp_swapteams")) {
    if (why) *why = "the command could not be queued";
    return false;
  }
  ApplyTeamNamesNow();  // team names follow the sides
  Print("match: teams swapped (map %d)\n", mapNumber);
  return true;
}

bool MatchFeaturesRenameTeam(int team, const std::string& name, std::string* why) {
  auto ctx = WebhookGetMatchContext();
  if (!ctx || ctx->slug == "scrim") {
    if (why) *why = "no match loaded";
    return false;
  }
  if (fleet_bridge::Assigned()) {
    if (why) *why = "this match comes from the platform: rename the team there";
    return false;
  }
  const std::string clean = SanitizeTeamName(name);
  if (clean.empty() || (team != 1 && team != 2)) {
    if (why) *why = "usage: .team1 <name> / .team2 <name>";
    return false;
  }
  (team == 1 ? ctx->team1_name : ctx->team2_name) = clean;
  WebhookSetMatchContext(*ctx);
  ApplyTeamNamesNow();  // mp_teamname_1 / _2
  Print("match: team%d renamed to \"%s\"\n", team, clean.c_str());
  return true;
}

void MatchFeaturesTick() {
  const double now = host::NowSeconds();
  // auto_5v5: from the round start until the freeze time ends, every 250 ms, until CS2 paused
  // (marked) or the check is over. Two byte reads of the game rules and one per player.
  if (const int rs = g_roundStartSeq.load(); rs != g_rsSeqAuto5v5) {
    g_rsSeqAuto5v5 = rs;
    g_auto5v5Pending = true;
    g_auto5v5NextAt = 0;
  }
  if (g_auto5v5Pending && !g_inFreeze.load()) g_auto5v5Pending = false;
  if (g_auto5v5Pending && now >= g_auto5v5NextAt) {
    g_auto5v5NextAt = now + 0.25;
    if (MaybeMarkAuto5v5Pause()) g_auto5v5Pending = false;
  }
  EnginePauseTick(now);
  const PauseSnapshot ps = PauseStateGet();
  const auto ctx = WebhookGetMatchContext();
  const MatchRules rules = EffectiveRules();

  if (ps.paused && !g_wasPaused) {
    g_wasPaused = true;
    g_effectiveAt = g_inFreeze.load() ? now : 0;
    g_rsSeqAtPause = g_roundStartSeq.load();
    g_feSeqAtPause = g_freezeEndSeq.load();
    g_warnedAutoUnpause = false;
  } else if (!ps.paused) {
    g_wasPaused = false;
    g_auto5v5FullSince = 0;
  }
  int secondsLeft = -1;
  if (ps.paused) {
    if (g_effectiveAt == 0 && g_roundStartSeq.load() != g_rsSeqAtPause) g_effectiveAt = now;
    if (ps.type == "tactical" && g_freezeEndSeq.load() != g_feSeqAtPause) {
      Unpause("tactical timeout over");
    } else if (ps.type == "auto_5v5" && g_freezeEndSeq.load() != g_feSeqAtPause) {
      Unpause("freeze time ended: the engine is not paused (auto_5v5)");
    } else if (ps.type == "auto_5v5" && LiveMap()) {
      // The player is back (or the wait is over): on with the round, nobody has to .unpause.
      if (now >= g_auto5v5CountAt) {
        g_auto5v5CountAt = now + 0.25;
        CountSides(&g_auto5v5Ct, &g_auto5v5T);
      }
      const int ct = g_auto5v5Ct, t = g_auto5v5T;
      if (Auto5v5ShortSide(ct, t) == 0) {
        if (g_auto5v5FullSince == 0) {
          g_auto5v5FullSince = now;
          SendToChat("Ready Up: both teams are full again - live in 5 seconds.");
        }
      } else {
        g_auto5v5FullSince = 0;
      }
      const double paused = g_effectiveAt > 0 ? now - g_effectiveAt : 0;
      const std::string why = Auto5v5UnpauseReason(ct, t, g_auto5v5FullSince > 0 ? now - g_auto5v5FullSince : 0,
                                                   paused, rules.disconnect_pause_max_seconds);
      if (!why.empty() && EnqueueServerCommand("mp_unpause_match")) {
        g_auto5v5FullSince = 0;
        Unpause(why == "full" ? "both teams are full again (auto_5v5)" : "the disconnect pause time is up");
        if (why == "timeout") SendToChat("Ready Up: the wait for the missing player is over - unpaused.");
      }
    } else if (ps.type == "technical" && rules.tech_pause_max_seconds > 0 && g_effectiveAt > 0 && LiveMap()) {
      secondsLeft = TechPauseSecondsLeft(static_cast<int>(now - g_effectiveAt), rules.tech_pause_max_seconds);
      if (secondsLeft <= 10 && secondsLeft > 0 && !g_warnedAutoUnpause) {
        g_warnedAutoUnpause = true;
        SendToChat(("Ready Up: technical pause ends in " + std::to_string(secondsLeft) + "s.").c_str());
      }
      if (secondsLeft == 0 && EnqueueServerCommand("mp_unpause_match")) {
        Unpause("technical pause time is up");
        SendToChat("Ready Up: technical pause time is up - unpaused.");
      }
    }
  }

  ForfeitTick(now);

  LiveHudInfo h;
  const PauseSnapshot cur = PauseStateGet();
  if (ctx && LiveMap()) {
    h.team1 = TeamName(*ctx, WebhookTeam::Team1);
    h.team2 = TeamName(*ctx, WebhookTeam::Team2);
    h.bothRequired = rules.both_teams_unpause != 0;
    if (cur.paused) {
      h.paused = true;
      h.type = cur.type.empty() ? "admin" : cur.type;
      if (cur.team != WebhookTeam::Unknown) h.byTeam = TeamName(*ctx, cur.team);
      h.pendingFreeze = g_effectiveAt == 0;
      h.secondsLeft = secondsLeft;
      h.team1Confirmed = cur.team1_ready_to_unpause;
      h.team2Confirmed = cur.team2_ready_to_unpause;
    }
    if (g_ff.Running()) {
      h.forfeit = true;
      h.forfeitTeam = g_ff.AbsentTeam() == 1 ? h.team1 : h.team2;
      h.forfeitSecondsLeft = g_ff.SecondsLeft(now);
    }
  }
  std::lock_guard<std::mutex> lk(g_hudMu);
  g_hud = std::move(h);
}

LiveHudInfo MatchFeaturesHud() {
  std::lock_guard<std::mutex> lk(g_hudMu);
  return g_hud;
}

void MatchFeaturesAdminUnpausing() {
  if (PauseStateGet().type != "auto_5v5") return;
  // An unpause while a side is still short was undone by CS2 at once (2026-10-14: a bot-filled
  // test match stayed paused after the admin unpause).
  if (EnqueueServerCommand("sv_matchpause_auto_5v5 0")) {
    SendToChat("Ready Up: an admin unpaused - no more automatic pauses this map.");
    Print("pause: admin unpause of auto_5v5; sv_matchpause_auto_5v5 0 for the rest of the map\n");
  }
}

void MatchFeaturesOnPlayerLeft(uint64_t steamid64, const std::string& name) {
  const auto ctx = WebhookGetMatchContext();
  if (!ctx || !steamid64 || !LiveMap() || ctx->slug == "scrim") return;
  const auto it = ctx->roster_team.find(steamid64);
  if (it == ctx->roster_team.end()) return;
  {
    std::lock_guard<std::mutex> lk(g_leftMu);
    if (!g_left.insert(steamid64).second) return;
  }
  const std::string who = name.empty() ? "A player" : name;
  SendToChat(PlayerLeftChatLine(who, TeamName(*ctx, it->second), AutoPause5v5Enabled(),
                                EffectiveRules().disconnect_pause_max_seconds)
                 .c_str());
}

void MatchFeaturesOnPlayerBack(uint64_t steamid64, const std::string& name) {
  {
    std::lock_guard<std::mutex> lk(g_leftMu);
    if (!g_left.erase(steamid64)) return;
  }
  if (!LiveMap()) return;
  SendToChat(("Ready Up: " + (name.empty() ? std::string("A player") : name) + " is back.").c_str());
}

}  // namespace readyup

// Simulation mode, engine side (simulation.h).
#include "readyup/simulation.h"

#include "readyup/engine.h"
#include "readyup/logging.h"
#include "readyup/match_events.h"
#include "readyup/match_features.h"
#include "readyup/match_state.h"
#include "readyup/modes.h"
#include "readyup/players.h"
#include "readyup/roster_names.h"
#include "readyup/simulation_rules.h"
#include "readyup/webhook.h"

#include <algorithm>
#include <map>
#include <mutex>
#include <set>
#include <string>
#include <vector>

namespace readyup {
namespace {

// A new map is given this long to settle (gamemode cfgs, the engine re-adding the old quota's
// bots) before the bots start over; a match load that never brings a map start still goes on.
constexpr double kMapSettleSeconds = 3.0;
constexpr double kMapStartWaitSeconds = 20.0;
constexpr double kAfterSetupSeconds = 1.5;
constexpr int kFullTeam = 5;

struct FillData {
  uint64_t matchid = 0;
  bool setup = false;
  double lastMap = -1;
  double noticedAt = 0;
  double resumeAt = 0;
  sim::BotFeeder feeder;
  // Roster humans seen on the server while the map was live, and when each went missing
  // (0 = here). A missing one keeps their seat: no bot takes it, so the side is short and
  // CS2's auto 5v5 pause holds the match for them, until the disconnect pause time is up.
  std::map<uint64_t, double> liveHumans;
};
FillData g_fill;  // game-thread owned
bool Team1IsCtNow(const WebhookMatchContext& ctx);

void FillTick(double now, const std::optional<WebhookMatchContext>& ctx) {
  auto& s = g_fill;
  const bool active = ctx && ctx->bot_fill && ctx->slug != "scrim";
  if (!active) {
    if (s.matchid) {
      for (const auto& c : sim::FillTeardownCommands()) (void)EnqueueServerCommand(c.c_str());
      const double lastMap = s.lastMap;
      s = FillData();
      s.lastMap = lastMap;
    }
    return;
  }
  const auto mode = GetMode();
  if (s.matchid != ctx->matchid) {
    const double lastMap = s.lastMap;
    s = FillData();
    s.lastMap = lastMap;
    s.matchid = ctx->matchid;
    s.noticedAt = now;
    // Recovery of a loaded live match must keep its current bots.
    s.setup = mode == ReadyUpMode::MatchLive || mode == ReadyUpMode::MatchKnife;
    // The map-start event may arrive before this frame notices the new context.
    const bool freshMap = lastMap >= 0 && now - lastMap < kMapSettleSeconds;
    s.resumeAt = s.setup ? now : freshMap ? lastMap + kMapSettleSeconds : now + kMapStartWaitSeconds;
  }
  if (mode != ReadyUpMode::MatchWarmup && mode != ReadyUpMode::MatchKnife && mode != ReadyUpMode::MatchLive) return;
  if (!s.setup && s.lastMap > s.noticedAt) s.resumeAt = s.lastMap + kMapSettleSeconds;
  if (now < s.resumeAt) return;
  if (!s.setup) {
    for (const auto& c : sim::RealTimeCommands()) (void)EnqueueServerCommand(c.c_str());
    for (const auto& c : sim::FillSetupCommands()) (void)EnqueueServerCommand(c.c_str());
    s.setup = true;
    s.resumeAt = now + kAfterSetupSeconds;
    s.feeder.Reset();
    return;
  }
  int humansCt = 0, humansT = 0;
  const bool team1Ct = Team1IsCtNow(*ctx);
  std::set<uint64_t> here;
  for (const auto& h : ListHumans()) {
    if (!ctx->roster_team.count(h.steamid64) || ctx->coaches.count(h.steamid64)) continue;
    here.insert(h.steamid64);
    // Reserve the roster side as soon as the human connects, even while spectating:
    // a full bot side must not prevent the arriving human from joining it.
    const bool isTeam1 = ctx->roster_team.at(h.steamid64) == WebhookTeam::Team1;
    if (isTeam1 == team1Ct) ++humansCt;
    else ++humansT;
    if (mode == ReadyUpMode::MatchLive) s.liveHumans[h.steamid64] = 0;
  }
  // A live human who left keeps their seat while the disconnect pause allows.
  const std::vector<uint64_t> hereList(here.begin(), here.end());
  for (uint64_t id : sim::HeldSeats(&s.liveHumans, hereList, now, std::max(0, EffectiveRules().disconnect_pause_max_seconds))) {
    const auto team = ctx->roster_team.find(id);
    if (team == ctx->roster_team.end()) continue;
    if ((team->second == WebhookTeam::Team1) == team1Ct) ++humansCt;
    else ++humansT;
  }
  std::vector<sim::Bot> bots;
  int ct = 0, t = 0;
  for (const auto& b : ListBots()) {
    if (b.team == 1 || b.name == "SourceTV") continue;
    bots.push_back({b.userid, b.team});
    if (b.team == 3) ++ct;
    else if (b.team == 2) ++t;
  }
  // While the match is live, bots change only in freeze time: a bot kicked or
  // added mid-round respawned in front of the players (2026-10-12).
  if (mode == ReadyUpMode::MatchLive && !MatchFeaturesInFreeze()) return;
  const auto plan = sim::PlanFill(ctx->players_per_team > 0 ? ctx->players_per_team : kFullTeam, humansCt, humansT, bots);
  if (!plan.removeUserids.empty()) {
    // Remove only an excess bot, never a human or the GOTV spectator.
    (void)EnqueueServerCommand(("kickid " + std::to_string(plan.removeUserids.front())).c_str());
    (void)EnqueueServerCommand(("bot_quota " + std::to_string(std::max(0, static_cast<int>(bots.size()) - 1))).c_str());
    s.feeder.Reset();
    s.resumeAt = now + 1.0;
    return;
  }
  int quota = 0;
  const int side = s.feeder.Next(now, static_cast<int>(bots.size()), ct, t, plan.ctWanted, plan.tWanted,
                               plan.ctWanted + plan.tWanted, &quota);
  if (side) for (const auto& c : sim::AddBotCommands(side, quota)) (void)EnqueueServerCommand(c.c_str());
}

struct Data {
  uint64_t matchid = 0;  // the simulated match (0 = none)
  std::vector<sim::Identity> ids;
  double timescale = 1.0;
  bool timescaleOn = false;
  // Waiting for the map the match load changes to (or a new map of the series).
  bool waitMapStart = false;
  double noticedAt = 0;
  double lastMapStartAt = -1;
  double resumeAt = 0;       // nothing before this (map settle, after the setup commands)
  bool setupDone = false;    // SetupCommands sent on this map
  sim::BotFeeder feeder;
  std::map<int, int> assigned;          // bot userid -> ids index
  std::map<int, double> assignedAt;     // bot userid -> when it took its identity
  std::set<uint64_t> announced;         // roster ids sent as player_connect (this match)
};

std::mutex g_mu;
Data g_d;

Data& S() { return g_d; }

bool ActiveCtx(const std::optional<WebhookMatchContext>& ctx) {
  return ctx && ctx->simulation && ctx->slug != "scrim";
}

void Send(const std::vector<std::string>& cmds) {
  for (const auto& c : cmds) (void)EnqueueServerCommand(c.c_str());
}

// team1 plays CT right now: the map's starting side, flipped by every side swap so far.
bool Team1IsCtNow(const WebhookMatchContext& ctx) {
  const int mapNumber = std::max(1, MatchStateGet().map_number);
  bool team1Ct = true;
  if (static_cast<size_t>(mapNumber) <= ctx.map_sides.size()) {
    team1Ct = ctx.map_sides[static_cast<size_t>(mapNumber - 1)] != "team2_ct";
  }
  if (MatchEventsSnapshot().swapCount % 2 != 0) team1Ct = !team1Ct;
  return team1Ct;
}

std::vector<sim::Identity> IdentitiesFor(const WebhookMatchContext& ctx) {
  std::vector<sim::Identity> roster;
  for (const auto& kv : ctx.roster_team) {
    const int team = kv.second == WebhookTeam::Team1 ? 1 : kv.second == WebhookTeam::Team2 ? 2 : 0;
    auto n = ctx.roster_names.find(kv.first);
    roster.push_back(sim::Identity{kv.first, n != ctx.roster_names.end() ? n->second : std::string(), team});
  }
  // A team without a roster gets a full team of anonymous bots (players_per_team; wingman: 2).
  return sim::PlanIdentities(roster, ctx.players_per_team > 0 ? ctx.players_per_team : kFullTeam);
}

WebhookTeam TeamTag(int team) {
  return team == 1 ? WebhookTeam::Team1 : team == 2 ? WebhookTeam::Team2 : WebhookTeam::Unknown;
}

// A player_ready webhook with the roster counts, as `.ready` sends it (match_router.cpp).
void EmitReady(const WebhookMatchContext& ctx, const sim::Identity& id) {
  int r1 = 0, r2 = 0;
  for (const auto& kv : ctx.roster_team) {
    if (kv.first == 0 || !IsReady(kv.first)) continue;
    if (kv.second == WebhookTeam::Team1) ++r1;
    else if (kv.second == WebhookTeam::Team2) ++r2;
  }
  WebhookEmitPlayerReady(WebhookPlayer{id.steamid64, id.name, TeamTag(id.team)}, true, r1, r2, r1 + r2,
                         static_cast<int>(ctx.roster_team.size()));
}

}  // namespace

bool SimulationActive() { return ActiveCtx(WebhookGetMatchContext()); }

void SimulationOnMapStart(double now) {
  auto& s = S();
  std::lock_guard<std::mutex> lk(g_mu);
  g_fill.lastMap = now;
  g_fill.setup = false;
  g_fill.resumeAt = now + kMapSettleSeconds;
  g_fill.feeder.Reset();
  s.lastMapStartAt = now;
  if (s.matchid == 0) return;
  // New userids on the new map: identities are handed out again (no disconnect / connect webhooks:
  // the roster stays "connected" across the map change, as humans do).
  s.waitMapStart = false;
  s.setupDone = false;
  s.resumeAt = now + kMapSettleSeconds;
  s.assigned.clear();
  s.assignedAt.clear();
  s.feeder.Reset();
  s.timescaleOn = false;  // the map starts in warmup (real time); the engine keeps host_timescale
}

void SimulationOnRoundStart() {
  auto& s = S();
  std::vector<std::string> cmds;
  {
    std::lock_guard<std::mutex> lk(g_mu);
    if (s.matchid == 0 || !s.timescaleOn) return;
    cmds = sim::TimescaleCommands(s.timescale);
  }
  Send(cmds);
}

void SimulationAddConnected(std::unordered_set<uint64_t>* connected) {
  if (!connected) return;
  auto& s = S();
  std::lock_guard<std::mutex> lk(g_mu);
  if (s.matchid == 0) return;
  for (const auto& kv : s.assigned) {
    const uint64_t sid = s.ids[static_cast<size_t>(kv.second)].steamid64;
    if (sid != 0) connected->insert(sid);
  }
}

std::optional<SimulatedPlayer> SimulationPlayerForSlot(int slot) {
  auto& s = S();
  std::lock_guard<std::mutex> lk(g_mu);
  if (s.matchid == 0) return std::nullopt;
  auto it = s.assigned.find(slot);
  if (it == s.assigned.end()) return std::nullopt;
  const auto& id = s.ids[static_cast<size_t>(it->second)];
  if (id.steamid64 == 0) return std::nullopt;
  return SimulatedPlayer{id.steamid64, id.name};
}

void SimulationTick(double now) {
  auto& s = S();
  const auto ctx = WebhookGetMatchContext();
  const bool active = ActiveCtx(ctx);
  const bool fill = ctx && ctx->bot_fill && ctx->slug != "scrim";
  // End mixed mode before bot-only simulation applies its own setup.
  if (!fill) FillTick(now, ctx);
  const ReadyUpMode mode = GetMode();
  // Read before taking g_mu: match_events / the player registry call back into this file
  // (SimulationPlayerForSlot) while holding their own locks.
  const bool team1Ct = active ? Team1IsCtNow(*ctx) : true;
  // The GOTV bot (SourceTV, on no side) is not one of ours: counted, it looked like a bot still
  // joining, so the feeder kept giving up on it and asking for another (6 v 6 for 5 v 5 rosters,
  // "Unknown" players in the stats; NTLAN trial run) and the surplus was never trimmed.
  std::vector<BotIdentity> botList = active ? ListBots() : std::vector<BotIdentity>();
  botList.erase(std::remove_if(botList.begin(), botList.end(),
                               [](const BotIdentity& b) { return b.team == 1 || b.name == "SourceTV"; }),
                botList.end());

  std::vector<std::string> cmds;
  std::vector<WebhookPlayer> connects, disconnects;
  std::vector<sim::Identity> toReady;
  std::vector<uint64_t> unready;  // identities whose bot is gone: their .ready goes with it
  {
    std::lock_guard<std::mutex> lk(g_mu);
    if (!active && s.matchid != 0) {
      // The simulated match is gone (series over, unloaded, replaced): bots out, defaults back.
      if (s.timescaleOn) cmds = sim::RealTimeCommands();
      for (const auto& c : fill ? sim::FillTeardownCommands() : sim::TeardownCommands()) cmds.push_back(c);
      Print("simulation: match %llu is over - bots removed, bot and timescale defaults restored\n",
            static_cast<unsigned long long>(s.matchid));
      const double lastMap = s.lastMapStartAt;
      s = Data();
      s.lastMapStartAt = lastMap;
    } else if (active) {
      if (s.matchid != ctx->matchid) {
        // A new simulated match. Loaded now: its map load comes next, the bots start on that map.
        // Found already going (plugin reload, boot recovery of a live map): keep the bots there.
        const double lastMap = s.lastMapStartAt;
        s = Data();
        s.lastMapStartAt = lastMap;
        s.matchid = ctx->matchid;
        s.ids = IdentitiesFor(*ctx);
        s.timescale = sim::ClampTimescale(ctx->simulation_timescale);
        s.noticedAt = now;
        const bool adopt = mode == ReadyUpMode::MatchLive || mode == ReadyUpMode::MatchKnife ||
                           mode == ReadyUpMode::Postgame;
        s.waitMapStart = !adopt;
        s.setupDone = adopt;
        int named = 0;
        for (const auto& id : s.ids) named += id.steamid64 != 0 ? 1 : 0;
        Print("simulation: match %llu - %zu bot(s) (%d roster player(s), %zu anonymous), timescale %s%s%s\n",
              static_cast<unsigned long long>(s.matchid), s.ids.size(), named, s.ids.size() - named,
              sim::TimescaleText(s.timescale).c_str(), ctx->wingman ? ", wingman" : "",
              adopt ? " (already running: keeping the bots)" : "");
      }
      if (s.waitMapStart) {
        if (s.lastMapStartAt > s.noticedAt) {
          s.waitMapStart = false;
          s.resumeAt = s.lastMapStartAt + kMapSettleSeconds;
        } else if (now - s.noticedAt > kMapStartWaitSeconds) {
          Print("simulation: no map start %.0fs after the match load; adding bots on this map\n", kMapStartWaitSeconds);
          s.waitMapStart = false;
          s.resumeAt = now;
        }
      }
      const bool playing = mode == ReadyUpMode::MatchWarmup || mode == ReadyUpMode::MatchKnife ||
                           mode == ReadyUpMode::MatchLive;
      // Real time outside a live map (warmup, knife, between maps); simulation_timescale while live.
      if (mode == ReadyUpMode::MatchLive && !s.timescaleOn) {
        cmds = sim::TimescaleCommands(s.timescale);
        if (!cmds.empty()) {
          s.timescaleOn = true;
          Print("simulation: map live - host_timescale %s\n", sim::TimescaleText(s.timescale).c_str());
        }
      } else if (mode != ReadyUpMode::MatchLive && s.timescaleOn) {
        cmds = sim::RealTimeCommands();
        s.timescaleOn = false;
      }
      if (playing && !s.waitMapStart && now >= s.resumeAt && s.setupDone) {
        int ct = 0, t = 0, ctWanted = 0, tWanted = 0;
        for (const auto& b : botList) {
          if (b.team == 3) ++ct;
          else if (b.team == 2) ++t;
        }
        sim::WantedPerSide(s.ids, team1Ct, &ctWanted, &tWanted);
        if (sim::WrongSides(ct, t, ctWanted, tWanted)) {
          // Start the fill over: the setup's bot_quota 0 / bot_kick just below, then bot by bot.
          Print("simulation: bots on the wrong side (CT %d/%d, T %d/%d); filling again\n", ct, ctWanted, t, tWanted);
          for (const auto& kv : s.assigned) {
            const auto& id = s.ids[static_cast<size_t>(kv.second)];
            if (id.steamid64 != 0) unready.push_back(id.steamid64);
          }
          s.setupDone = false;
        }
      }
      if (playing && !s.waitMapStart && now >= s.resumeAt) {
        if (!s.setupDone) {
          for (const auto& c : sim::SetupCommands()) cmds.push_back(c);
          s.setupDone = true;
          s.resumeAt = now + kAfterSetupSeconds;
          s.feeder.Reset();
          s.assigned.clear();
          s.assignedAt.clear();
        } else {
          std::vector<sim::Bot> bots;
          int ct = 0, t = 0;
          for (const auto& b : botList) {
            bots.push_back(sim::Bot{b.userid, b.team});
            if (b.team == 3) ++ct;
            else if (b.team == 2) ++t;
          }
          int ctWanted = 0, tWanted = 0;
          sim::WantedPerSide(s.ids, team1Ct, &ctWanted, &tWanted);
          int quota = 0;
          const int side = s.feeder.Next(now, static_cast<int>(bots.size()), ct, t, ctWanted, tWanted,
                                         static_cast<int>(s.ids.size()) + 2, &quota);
          int trimTo = 0;
          if (side == 0 && s.feeder.Trim(now, static_cast<int>(bots.size()), ct, t, ctWanted, tWanted, &trimTo)) {
            cmds.push_back("bot_quota " + std::to_string(trimTo));
            Print("simulation: %zu bots for %d roster players; bot_quota %d\n", bots.size(), ctWanted + tWanted, trimTo);
          }
          if (side != 0) {
            for (const auto& c : sim::AddBotCommands(side, quota)) cmds.push_back(c);
            Debug("simulation: adding a bot on %s (bot_quota %d; CT %d/%d, T %d/%d)\n", side == 3 ? "CT" : "T", quota,
                  ct, ctWanted, t, tWanted);
          }

          const auto next = sim::Assign(s.assigned, bots, s.ids, team1Ct);
          for (const auto& kv : s.assigned) {
            if (next.count(kv.first)) continue;
            const auto& id = s.ids[static_cast<size_t>(kv.second)];
            s.assignedAt.erase(kv.first);
            if (id.steamid64 != 0) unready.push_back(id.steamid64);
            if (id.steamid64 != 0 && s.announced.erase(id.steamid64)) {
              disconnects.push_back(WebhookPlayer{id.steamid64, id.name, TeamTag(id.team)});
            }
          }
          for (const auto& kv : next) {
            if (s.assigned.count(kv.first)) continue;
            const auto& id = s.ids[static_cast<size_t>(kv.second)];
            s.assignedAt[kv.first] = now;
            std::string botName;
            for (const auto& b : botList) {
              if (b.userid == kv.first) botName = b.name;
            }
            Print("simulation: bot %s<%d> plays %s (%llu, team%d)\n", botName.c_str(), kv.first,
                  id.steamid64 ? id.name.c_str() : "an anonymous player", static_cast<unsigned long long>(id.steamid64),
                  id.team);
            if (id.steamid64 != 0) RosterNamesNameBot(botName, id.name);
            if (id.steamid64 != 0 && s.announced.insert(id.steamid64).second) {
              connects.push_back(WebhookPlayer{id.steamid64, id.name, TeamTag(id.team)});
            }
          }
          s.assigned = next;

          // Simulated `.ready` in warmup, a moment after each bot took its identity.
          if (mode == ReadyUpMode::MatchWarmup) {
            for (const auto& kv : s.assigned) {
              const auto& id = s.ids[static_cast<size_t>(kv.second)];
              if (id.steamid64 == 0) continue;
              if (now - s.assignedAt[kv.first] >= sim::ReadyDelaySeconds(kv.second)) toReady.push_back(id);
            }
          }
        }
      }
    }
  }

  // Outside the lock: modes / webhooks take their own.
  Send(cmds);
  for (const auto sid : unready) ClearReady(sid);
  for (const auto& p : disconnects) WebhookEmitPlayerDisconnect(p);
  for (const auto& p : connects) WebhookEmitPlayerConnect(p);
  if (!toReady.empty() && ctx && !GoLiveTriggered()) {
    for (const auto& id : toReady) {
      if (IsReady(id.steamid64)) continue;
      (void)SetReady(id.steamid64, true);
      EmitReady(*ctx, id);
      Debug("simulation: %s (%llu) is ready\n", id.name.c_str(), static_cast<unsigned long long>(id.steamid64));
    }
  }
  // Previous simulation cleanup runs first so it cannot undo the mixed match setup.
  if (fill) FillTick(now, ctx);
}

}  // namespace readyup

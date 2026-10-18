// .gg surrender vote and .stop round-restore vote (see votes.h).
#include "readyup/votes.h"

#include "readyup/config.h"
#include "readyup/damage_report.h"
#include "readyup/engine.h"
#include "readyup/esports.h"
#include "readyup/fleet_bridge.h"
#include "readyup/host.h"
#include "readyup/logging.h"
#include "readyup/match_events.h"
#include "readyup/match_features.h"
#include "readyup/match_state.h"
#include "readyup/match_stats.h"
#include "readyup/modes.h"
#include "readyup/pause_state.h"
#include "readyup/players.h"
#include "readyup/round_restore_rules.h"
#include "readyup/vote_logic.h"

#include <algorithm>
#include <cstring>
#include <string>
#include <unordered_set>

namespace readyup {
namespace {

constexpr double kGgWindowSeconds = 60.0;

// Game thread only.
GgVote g_gg[3];  // per team (1 / 2)
StopVote g_stop;
uint64_t g_voteMatch = 0;
int g_voteMap = 0;

const char* TeamKey(WebhookTeam t) {
  return t == WebhookTeam::Team1 ? "team1" : t == WebhookTeam::Team2 ? "team2" : "unknown";
}
int TeamIdx(WebhookTeam t) { return t == WebhookTeam::Team1 ? 1 : t == WebhookTeam::Team2 ? 2 : 0; }

std::string TeamName(const WebhookMatchContext& ctx, WebhookTeam t) {
  const std::string& n = t == WebhookTeam::Team1 ? ctx.team1_name : ctx.team2_name;
  if (!n.empty()) return n;
  return t == WebhookTeam::Team1 ? "Team1" : "Team2";
}

// CS side (3 CT / 2 T) of a team on the current map (map side + swaps so far).
int CsSideOf(const WebhookMatchContext& ctx, WebhookTeam team) {
  const int mapNum = std::max(1, MatchStateGet().map_number);
  bool team1Ct = !(static_cast<size_t>(mapNum) <= ctx.map_sides.size() &&
                   ctx.map_sides[static_cast<size_t>(mapNum - 1)] == "team2_ct");
  if (MatchEventsSnapshot().swapCount % 2 != 0) team1Ct = !team1Ct;
  return ((team == WebhookTeam::Team1) == team1Ct) ? 3 : 2;
}

// Connected players that may vote for `team`: its connected roster players; bots on its side
// when the team has no roster (bot tests) or a dev bot flag is on.
int EligibleVoters(const WebhookMatchContext& ctx, WebhookTeam team) {
  std::unordered_set<uint64_t> connected;
  for (const auto& h : ListHumans()) connected.insert(h.steamid64);
  int n = 0;
  bool hasRoster = false;
  for (const auto& kv : ctx.roster_team) {
    if (kv.second != team) continue;
    hasRoster = true;
    if (connected.count(kv.first)) ++n;
  }
  if (!hasRoster || DevBotsReadyEnabled() || DevBotsScrimEnabled()) {
    const int side = CsSideOf(ctx, team);
    for (const auto& b : ListBots()) {
      if (b.team == side) ++n;
    }
  }
  return n;
}

// team1 / team2 map score: the stats model (engine round winners) while live, else MatchState.
void Scores(int* t1, int* t2) {
  {
    std::lock_guard<std::recursive_mutex> lk(stats::Mutex());
    if (stats::Current().Live()) {
      *t1 = stats::Current().Team1Score();
      *t2 = stats::Current().Team2Score();
      return;
    }
  }
  const auto ms = MatchStateGet();
  *t1 = ms.team1_score;
  *t2 = ms.team2_score;
}

// A new match or map drops every vote.
void SyncMatch() {
  const auto ctx = WebhookGetMatchContext();
  const uint64_t mid = ctx ? ctx->matchid : 0;
  const int map = MatchStateGet().map_number;
  if (mid == g_voteMatch && map == g_voteMap) return;
  g_voteMatch = mid;
  g_voteMap = map;
  g_gg[1].Reset();
  g_gg[2].Reset();
  g_stop.Reset();
}

void OnGameEvent(void*, const char* name, const ru_game_event*) {
  if (name && std::strcmp(name, "round_start") == 0) {
    int team = g_stop.Pending();
    if (team != 0) Print("vote: stop: request of team%d dropped (new round)\n", team);
    g_stop.Reset();
  }
}

}  // namespace

WebhookTeam VotesTeamForSide(int csTeam) {
  const auto ctx = WebhookGetMatchContext();
  if (!ctx || (csTeam != 2 && csTeam != 3)) return WebhookTeam::Unknown;
  return CsSideOf(*ctx, WebhookTeam::Team1) == csTeam ? WebhookTeam::Team1 : WebhookTeam::Team2;
}

// `ru as <slot> <.gg|.stop>`: the server console votes as that player (bot-only live tests).
static void OnRuAs(void*, const ru_command_ctx* c) {
  if (!c->is_console) return;
  if (c->argc < 4) {
    PrintLine("usage: ru as <slot> <.gg|.stop> (vote as that player; practice tools: ru practice as)");
    return;
  }
  const int slot = std::atoi(c->argv[2]);
  const std::string cmd = c->argv[3] ? c->argv[3] : "";
  struct Found {
    int slot;
    ru_player p;
    bool found;
  } f{slot, {}, false};
  const ru_api* a = host::Api();
  a->for_each_player(
      a->self,
      [](void* u, const ru_player* pl) -> int {
        auto* f = static_cast<Found*>(u);
        if ((pl->slot >= 0 ? pl->slot : pl->userid) != f->slot) return 1;
        std::memcpy(&f->p, pl, std::min<size_t>(sizeof(ru_player), pl->struct_size));
        f->found = true;
        return 0;
      },
      &f);
  if (!f.found) {
    Print("ru as: no player in slot %d\n", slot);
    return;
  }
  const uint64_t id = f.p.is_bot ? DevBotIdForUserid(slot) : f.p.steamid64;
  WebhookTeam team = WebhookTeam::Unknown;
  if (auto ctx = WebhookGetMatchContext()) {
    if (auto it = ctx->roster_team.find(id); it != ctx->roster_team.end()) team = it->second;
  }
  if (team == WebhookTeam::Unknown) {
    const auto cs = GetCsTeamNumForSlot(slot);
    team = VotesTeamForSide(cs ? *cs : f.p.team);
  }
  Print("ru as: slot %d (%s) runs %s\n", slot, f.p.name, cmd.c_str());
  if (team == WebhookTeam::Unknown) {
    Print("ru as: slot %d has no match team\n", slot);
  } else if (cmd == ".gg") {
    if (!VotesGg(id, team, f.p.name)) Print("ru as: .gg vote is off (gg_enabled=0)\n");
  } else if (cmd == ".stop") {
    VotesStop(id, team, f.p.name);
  } else {
    Print("ru as: %s is not supported (.gg, .stop; practice tools: ru practice as)\n", cmd.c_str());
  }
}

void VotesInstall(const ru_api* api) {
  if (!api->register_ru_subcommand(api->self, "as", &OnRuAs, nullptr)) Print("vote: could not register `ru as`\n");
  if (!api->subscribe_game_event(api->self, "round_start", &OnGameEvent, nullptr)) {
    Print("vote: could not subscribe to round_start\n");
  }
}

void VotesTick(double now) {
  SyncMatch();
  const auto ctx = WebhookGetMatchContext();
  for (int t = 1; t <= 2; ++t) {
    if (g_gg[t].Expire(now)) {
      const WebhookTeam team = t == 1 ? WebhookTeam::Team1 : WebhookTeam::Team2;
      Print("vote: gg: %s vote expired\n", TeamKey(team));
      if (ctx) SendToChat(("Ready Up: " + TeamName(*ctx, team) + "'s surrender vote failed.").c_str());
    }
  }
  int team = 0;
  if (g_stop.Expire(now, &team)) {
    Print("vote: stop: team%d request expired\n", team);
    SendToChat("Ready Up: .stop expired (the other team did not confirm).");
  }
}

bool VotesGg(uint64_t voter, WebhookTeam team, const std::string& name) {
  SyncMatch();
  // Valve ruleset: no surrender vote (`.gg` only emits player_gg, as with gg off).
  if (!PlayerExtrasAllowed(CurrentEffectiveRules())) return false;
  const auto ctx = WebhookGetMatchContext();
  const MatchRules rules = EffectiveRules();
  if (!ctx || !rules.gg_enabled) return false;
  const int ti = TeamIdx(team);
  if (ti == 0) return true;
  if (GetMode() != ReadyUpMode::MatchLive) {
    SendToChat("Ready Up: .gg only works while the map is live.");
    return true;
  }
  int s1 = 0, s2 = 0;
  Scores(&s1, &s2);
  const int mine = ti == 1 ? s1 : s2, theirs = ti == 1 ? s2 : s1;
  if (!GgScoreAllowed(mine, theirs, rules.gg_min_score_diff)) {
    SendToChat(("Ready Up: .gg needs your team to trail by " + std::to_string(rules.gg_min_score_diff) +
                " rounds (score " + std::to_string(mine) + "-" + std::to_string(theirs) + ").")
                   .c_str());
    Print("vote: gg: %s refused (score %d-%d, min diff %d)\n", TeamKey(team), mine, theirs, rules.gg_min_score_diff);
    return true;
  }
  const int eligible = EligibleVoters(*ctx, team);
  const int need = GgVotesNeeded(eligible, rules.gg_threshold_pct);
  const auto r = g_gg[ti].Add(voter, host::NowSeconds(), eligible, rules.gg_threshold_pct, kGgWindowSeconds);
  const std::string tn = TeamName(*ctx, team);
  if (r == GgVote::Result::AlreadyVoted) {
    SendToChat(("Ready Up: " + name + " already voted to surrender (" + std::to_string(g_gg[ti].Count()) + "/" +
                std::to_string(need) + ").")
                   .c_str());
    return true;
  }
  if (r == GgVote::Result::Counted) {
    Print("vote: gg: %s %d/%d (by %s)\n", TeamKey(team), g_gg[ti].Count(), need, name.c_str());
    SendToChat(("Ready Up: " + name + " votes to surrender for " + tn + " (" + std::to_string(g_gg[ti].Count()) +
                "/" + std::to_string(need) + ", " + std::to_string(static_cast<int>(kGgWindowSeconds)) +
                "s). Teammates: type .gg to agree.")
                   .c_str());
    return true;
  }
  Print("vote: gg: %s passed (%d eligible, %d%%) -> forfeit\n", TeamKey(team), eligible, rules.gg_threshold_pct);
  SendToChat(("Ready Up: " + tn + " surrendered (.gg vote passed).").c_str());
  if (!ForfeitCurrentMap(team, "gg")) Print("vote: gg: no live map to forfeit\n");
  return true;
}

void VotesStop(uint64_t /*voter*/, WebhookTeam team, const std::string& name) {
  SyncMatch();
  if (!PlayerExtrasAllowed(CurrentEffectiveRules())) {
    Print("vote: stop: ignored (valve ruleset)\n");
    return;
  }
  const auto ctx = WebhookGetMatchContext();
  const MatchRules rules = EffectiveRules();
  const int ti = TeamIdx(team);
  if (!ctx || ti == 0) return;
  if (!rules.stop_command_available) {
    SendToChat("Ready Up: .stop is not enabled for this match.");
    return;
  }
  if (GetMode() != ReadyUpMode::MatchLive) {
    SendToChat("Ready Up: .stop only works while the map is live.");
    return;
  }
  if (rules.stop_command_no_damage && DamageReportEnemyDamageThisRound()) {
    Print("vote: stop: %s refused (damage dealt this round)\n", TeamKey(team));
    SendToChat("Ready Up: .stop is unavailable: damage was already dealt this round.");
    return;
  }
  const auto r = g_stop.Request(ti, host::NowSeconds(), rules.stop_vote_seconds);
  if (r == StopVote::Result::AlreadyRequested) {
    SendToChat(("Ready Up: " + TeamName(*ctx, team) + " already asked for .stop; waiting for the other team.").c_str());
    return;
  }
  if (r == StopVote::Result::Waiting) {
    Print("vote: stop: requested by %s (%s)\n", TeamKey(team), name.c_str());
    const WebhookTeam other = ti == 1 ? WebhookTeam::Team2 : WebhookTeam::Team1;
    SendToChat(("Ready Up: " + name + " (" + TeamName(*ctx, team) + ") wants to restore this round. " +
                TeamName(*ctx, other) + ": type .stop within " + std::to_string(rules.stop_vote_seconds) + "s to agree.")
                   .c_str());
    return;
  }
  int s1 = 0, s2 = 0;
  Scores(&s1, &s2);
  const int round = s1 + s2 + 1;
  std::string err;
  bool autoUnpause = false;
  if (!fleet_bridge::RestoreRoundFromLocalBackup(round, "vote", "stop", &err, &autoUnpause)) {
    err = restore::ExplainRestoreError(err);
    Print("vote: stop: passed, restore of round %d failed: %s\n", round, err.c_str());
    SendToChat(("Ready Up: .stop passed but round " + std::to_string(round) + " cannot be restored (" + err + ").").c_str());
    return;
  }
  if (autoUnpause) {
    // pause_after_restore 0 (round_restore.h): live again in 3 s.
    Print("vote: stop: passed -> round %d restored (live in 3 s)\n", round);
    SendToChat(("Ready Up: both teams agreed: round " + std::to_string(round) + " restored. Live in 3 seconds.").c_str());
    return;
  }
  // The restore autopauses (marked as an admin pause); the players agreed to it, so they unpause
  // it like a technical pause (.unpause, both_teams_unpause_required).
  PauseStateOnPaused("technical", "vote:stop");
  Print("vote: stop: passed -> round %d restored\n", round);
  SendToChat(("Ready Up: both teams agreed: round " + std::to_string(round) +
              " restored. The match is paused; .unpause when ready.")
                 .c_str());
}

}  // namespace readyup

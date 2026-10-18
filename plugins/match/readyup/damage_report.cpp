// End-of-round damage report (see damage_report.h).
#include "readyup/damage_report.h"

#include "readyup/config.h"
#include "readyup/damage_ledger.h"
#include "readyup/engine.h"
#include "readyup/esports.h"
#include "readyup/host.h"
#include "readyup/logging.h"
#include "readyup/match_events.h"
#include "readyup/match_state.h"
#include "readyup/modes.h"
#include "readyup/players.h"

#include <cstring>
#include <string>
#include <unordered_set>
#include <vector>

namespace readyup {
namespace {

// Game thread only (engine events and ticks).
DamageLedger g_ledger;
bool g_enemyDamage = false;

struct Outgoing {
  int slot = -1;
  bool human = false;
  std::string name;
  std::vector<std::string> lines;
};
std::vector<Outgoing> g_out;
int g_outRound = 0;
// Who was on a team when the round started: someone who joined mid-round gets no report
// for it (it would be all zeros: a scrim player taking a bot's place saw one on joining).
std::unordered_set<int> g_roundStartSlots;
// Everyone on a team at the round start: a player who leaves mid-round still shows in the
// others' reports with the damage done to and by them (2026-10-15, a live test).
std::vector<DamageParticipant> g_roundStartPlayers;
bool g_haveRoundStart = false;

int TeamOf(int slot) {
  if (slot < 0) return 0;
  const auto t = GetCsTeamNumForSlot(slot);
  return t ? *t : 0;
}

struct Collect {
  std::vector<DamageParticipant> players;
  std::vector<bool> human;
};

int CollectPlayer(void* user, const ru_player* p) {
  auto* c = static_cast<Collect*>(user);
  if (!p->connected) return 1;
  // Bots have no engine slot in the registry; their log `<N>` is the slot in CS2.
  const int key = p->slot >= 0 ? p->slot : p->userid;
  if (key < 0) return 1;
  int team = TeamOf(key);
  if (team != 2 && team != 3) team = p->team;
  if (team != 2 && team != 3) return 1;
  c->players.push_back(DamageParticipant{key, p->name, team});
  c->human.push_back(!p->is_bot && p->steamid64 != 0);
  return 1;
}

void BuildReports() {
  const ru_api* a = host::Api();
  if (!a) return;
  Collect c;
  a->for_each_player(a->self, &CollectPlayer, &c);
  // Who left since the round started: no report for them (gone), but they are in everyone else's.
  std::unordered_set<int> here;
  for (const auto& p : c.players) here.insert(p.key);
  std::vector<DamageParticipant> all = c.players;
  for (const auto& p : g_roundStartPlayers) {
    if (!here.count(p.key)) all.push_back(p);
  }
  g_out.clear();
  g_outRound = MatchStateGet().round_number;
  for (size_t i = 0; i < c.players.size(); ++i) {
    Outgoing o;
    o.slot = c.players[i].key;
    o.human = c.human[i] && (!g_haveRoundStart || g_roundStartSlots.count(o.slot) > 0);
    o.name = c.players[i].name;
    o.lines = FormatDamageReport(c.players[i], all, g_ledger);
    if (!o.lines.empty()) g_out.push_back(std::move(o));
  }
}

void OnGameEvent(void*, const char* name, const ru_game_event* ev) {
  const ru_api* a = host::Api();
  if (!a || !name || !ev) return;
  if (std::strcmp(name, "round_start") == 0) {
    g_ledger.Reset();
    g_enemyDamage = false;
    Collect c;
    a->for_each_player(a->self, &CollectPlayer, &c);
    g_roundStartSlots.clear();
    for (const auto& p : c.players) g_roundStartSlots.insert(p.key);
    g_roundStartPlayers = c.players;
    g_haveRoundStart = true;
  } else if (std::strcmp(name, "player_hurt") == 0) {
    const int victim = a->ev_get_player_slot(a->self, ev, "userid");
    const int attacker = a->ev_get_player_slot(a->self, ev, "attacker");
    const int dmg = a->ev_get_int(a->self, ev, "dmg_health", 0);
    g_ledger.OnHurt(attacker, victim, dmg, a->ev_get_int(a->self, ev, "health", 0));
    if (attacker >= 0 && victim >= 0 && attacker != victim && dmg > 0) {
      const int ta = TeamOf(attacker), tv = TeamOf(victim);
      if ((ta == 2 || ta == 3) && (tv == 2 || tv == 3) && ta != tv) g_enemyDamage = true;
    }
  } else if (std::strcmp(name, "player_death") == 0) {
    g_ledger.OnDeath(a->ev_get_player_slot(a->self, ev, "userid"));
  } else if (std::strcmp(name, "round_end") == 0) {
    // Warmup rounds never end (mp_ignore_round_win_conditions 1): one that does means something
    // reset the warmup rules (a game mode cfg, another plugin). Said without debug=1, not every time.
    if (GetMode() == ReadyUpMode::MatchWarmup) {
      static int warmupRoundEnds = 0;
      ++warmupRoundEnds;
      if (warmupRoundEnds <= 3 || warmupRoundEnds % 50 == 0) {
        Print("warmup: a round ended during match warmup (%d so far, reason %d): the warmup rules were "
              "reset by something else (game mode cfg, another plugin)\n",
              warmupRoundEnds, a->ev_get_int(a->self, ev, "reason", -1));
      }
    }
    if (!Cfg().damage_report || GetMode() != ReadyUpMode::MatchLive) return;
    if (!PlayerExtrasAllowed(CurrentEffectiveRules())) return;  // valve ruleset: no report
    BuildReports();
  }
}

}  // namespace

void DamageReportInstall(const ru_api* api) {
  for (const char* e : {"round_start", "round_end", "player_hurt", "player_death"}) {
    if (!api->subscribe_game_event(api->self, e, &OnGameEvent, nullptr)) {
      Print("damage-report: could not subscribe to %s\n", e);
    }
  }
}

void DamageReportTick() {
  if (g_out.empty()) return;
  int humans = 0, lines = 0;
  for (const auto& o : g_out) {
    lines += static_cast<int>(o.lines.size());
    if (o.human) {
      ++humans;
      (void)ClientPrintChat(o.slot, " \x04[ReadyUp]\x01 Damage report:");
      for (const auto& l : o.lines) (void)ClientPrintChat(o.slot, (" " + l).c_str());
    }
    for (const auto& l : o.lines) Debug("damage-report: %s: %s\n", o.name.c_str(), l.c_str());
  }
  Print("damage-report: round %d: %zu players, %d lines, sent to %d humans\n", g_outRound, g_out.size(), lines, humans);
  g_out.clear();
}

bool DamageReportEnemyDamageThisRound() { return g_enemyDamage; }

}  // namespace readyup

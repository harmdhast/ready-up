// Match half of the old core game_events.cpp (see match_events.h). Engine access goes through
// ru_api only: event fields via ev_get_*, controllers via entity_by_index / schema_offset.
#include "readyup/match_events.h"
#include "readyup/match_recovery.h"

#include "readyup/backup_files.h"
#include "readyup/engine.h"
#include "readyup/fleet_state.h"
#include "readyup/esports.h"
#include "readyup/host.h"
#include "readyup/logging.h"
#include "readyup/match_signals.h"
#include "readyup/match_features.h"
#include "readyup/match_state.h"
#include "readyup/match_stats.h"
#include "readyup/modes.h"
#include "readyup/persisted_match_state.h"
#include "readyup/players.h"
#include "readyup/simulation.h"
#include "readyup/webhook.h"
#include "readyup/golive_card.h"
#include "readyup/welcome.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <climits>
#include <cstring>
#include <deque>
#include <initializer_list>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace readyup {
namespace {

const ru_api* A() { return host::Api(); }

struct PlayerStats {
  std::string name;
  WebhookTeam team = WebhookTeam::Unknown;
  int kills = 0;
  int deaths = 0;
  int assists = 0;
  int headshot_kills = 0;
  int damage = 0;
};

// Recursive: handlers re-enter this module on the same thread (round_end -> OnMatchRoundEnded
// -> GetRosterTeamDamageTotals).
std::recursive_mutex g_mu;
int g_roundNumber = 0;
int g_lastMapNumber = 0;
std::unordered_map<uint64_t, PlayerStats> g_stats;
std::unordered_set<int> g_spawned;  // slots seen in player_spawn this map (stats participants)

int g_swapCount = 0;           // side swaps applied so far on this map
// Lock-free copies for modes.cpp, which holds its own mutex when it reads them (this module's
// handlers take that one while holding g_mu).
std::atomic<int> g_swapCountPub{0};
std::atomic<int> g_lastMapNumberPub{0};
struct ResumeSide {
  int mapNumber = 0;
  bool team1Ct = true;
};
std::optional<ResumeSide> g_resumeSide;  // the resumed round's side of team1, until the restore
int g_lastHalfStartTotal = -1; // (team1+team2) total at which halftime + swap was emitted
int g_lastOvertimeNumber = 0;  // last overtime_number overtime_started was emitted for

// round_start / round_end copied at dispatch, handled on the tick (see match_events.h).
struct PendingRound {
  bool start = false;
  int ct = INT_MIN, t = INT_MIN;  // round_start score keys, INT_MIN = absent
  int winner = 0, reason = 0;     // round_end
  int roundTimeMs = 0;            // round_end: since round_freeze_end (0 = not seen)
};
std::mutex g_pendingMu;
std::deque<PendingRound> g_pending;
double g_freezeEndAt = 0.0;  // host::NowSeconds() of this round's round_freeze_end (g_pendingMu)

std::atomic<int> g_eventsLive{0};

// ---- helpers -----------------------------------------------------------------------------

// A simulated match's bot counts as the roster player it plays as (simulation.h).
std::optional<uint64_t> SteamForSlot(int slot) {
  auto ident = GetSlotIdentity(slot);
  if (!ident || ident->steamid64 == 0) {
    if (auto sim = SimulationPlayerForSlot(slot)) return sim->steamid64;
    return std::nullopt;
  }
  return ident->steamid64;
}

std::string NameForSlot(int slot) {
  auto ident = GetSlotIdentity(slot);
  if (!ident) {
    if (auto sim = SimulationPlayerForSlot(slot)) return sim->name;
  }
  return ident ? ident->name : std::string();
}

WebhookTeam TeamForSteam(uint64_t steamid64) {
  auto ctx = WebhookGetMatchContext();
  if (!ctx) return WebhookTeam::Unknown;
  auto it = ctx->roster_team.find(steamid64);
  return it == ctx->roster_team.end() ? WebhookTeam::Unknown : it->second;
}

// After a swap count reset: the pending resume side of this map decides it.
void ApplyResumeSideLocked(int mapNumber) {
  if (!g_resumeSide || g_resumeSide->mapNumber != mapNumber) return;
  g_swapCount = fleetstate::SwapParityFor(Team1IsCtWithSwapCount(mapNumber, 0), g_resumeSide->team1Ct);
}

void MaybeResetForMapLocked(int mapNumber) {
  if (mapNumber <= 0) mapNumber = 1;
  if (g_lastMapNumber != mapNumber) {
    g_lastMapNumber = mapNumber;
    g_roundNumber = 0;
    g_stats.clear();
    g_spawned.clear();
    g_swapCount = 0;
    g_lastHalfStartTotal = -1;
    g_lastOvertimeNumber = 0;
    ApplyResumeSideLocked(mapNumber);
  }
  g_swapCountPub = g_swapCount;
  g_lastMapNumberPub = g_lastMapNumber;
}

template <typename T>
std::optional<T> ReadAt(void* base, int offset) {
  if (!base || offset < 0 || offset > 0x20000) return std::nullopt;
  T out{};
  std::memcpy(&out, reinterpret_cast<const unsigned char*>(base) + offset, sizeof(T));
  return out;
}

// Schema offset on CCSPlayerController (or a base class), cached per plugin load.
std::optional<int> ControllerOffset(const char* field, std::initializer_list<const char*> classes) {
  const ru_api* a = A();
  if (!a) return std::nullopt;
  for (const char* c : classes) {
    const int off = a->schema_offset(a->self, c, field);
    if (off >= 0) return off;
  }
  return std::nullopt;
}

struct Offsets {
  bool looked = false;
  std::optional<int> steamId, teamNum, playerName, score, mvps;
  // Scoreboard kills / deaths / assists / headshot kills. CS2 keeps them in the controller's
  // action tracking services (CCSPlayerController::m_pActionTrackingServices ->
  // CCSPlayerController_ActionTrackingServices::m_matchStats, a CSMatchStats_t whose base
  // CSPerRoundStats_t has m_iKills / m_iDeaths / m_iAssists); the controller's own m_iKills /
  // m_iDeaths / m_iAssists are gone. Missing -> the event-accumulated stats are used.
  std::optional<int> actionTracking, matchStats, kills, deaths, assists, headshots;
};
Offsets& Off() {
  static Offsets o;
  if (!o.looked && A() && A()->schema_offset(A()->self, "CCSPlayerController", "m_iTeamNum") >= 0) {
    o.looked = true;
    o.steamId = ControllerOffset("m_steamID", {"CCSPlayerController", "CBasePlayerController"});
    if (!o.steamId) PrintLine("match-events: m_steamID offset not found; event players resolve via the log slot map.");
    o.teamNum = ControllerOffset("m_iTeamNum", {"CCSPlayerController", "CBaseEntity"});
    o.playerName = ControllerOffset("m_iszPlayerName", {"CCSPlayerController", "CBasePlayerController"});
    o.score = ControllerOffset("m_iScore", {"CCSPlayerController"});
    o.mvps = ControllerOffset("m_iMVPs", {"CCSPlayerController"});
    o.actionTracking = ControllerOffset("m_pActionTrackingServices", {"CCSPlayerController"});
    o.matchStats = ControllerOffset("m_matchStats", {"CCSPlayerController_ActionTrackingServices"});
    o.kills = ControllerOffset("m_iKills", {"CSMatchStats_t"});
    o.deaths = ControllerOffset("m_iDeaths", {"CSMatchStats_t"});
    o.assists = ControllerOffset("m_iAssists", {"CSMatchStats_t"});
    o.headshots = ControllerOffset("m_iHeadShotKills", {"CSMatchStats_t"});
  }
  return o;
}

// The controller's CSMatchStats_t (scoreboard totals for this match), or nullptr. This frame only.
void* MatchStatsOf(void* controller) {
  const Offsets& o = Off();
  if (!controller || !o.actionTracking || !o.matchStats) return nullptr;
  const auto svc = ReadAt<void*>(controller, *o.actionTracking);
  if (!svc || !*svc) return nullptr;
  return static_cast<unsigned char*>(*svc) + *o.matchStats;
}

// One scoreboard counter from CSMatchStats_t; nullopt when unknown or out of range.
std::optional<int> MatchStat(void* matchStats, const std::optional<int>& offset) {
  if (!matchStats || !offset) return std::nullopt;
  const auto v = ReadAt<int>(matchStats, *offset);
  if (!v || *v < 0 || *v > 10000) return std::nullopt;
  return v;
}

// The player controller of a slot (entity index slot + 1), or nullptr. This frame only.
void* ControllerForSlot(int slot) {
  const ru_api* a = A();
  if (!a || slot < 0 || slot >= 64) return nullptr;
  void* ent = a->entity_by_index(a->self, slot + 1);
  if (!ent) return nullptr;
  const char* cls = a->entity_classname(a->self, ent);
  return cls && std::strcmp(cls, "cs_player_controller") == 0 ? ent : nullptr;
}

// SteamID64 straight from a controller (m_steamID). 0 for bots / unknown.
uint64_t SteamFromController(void* controller) {
  if (!controller || !Off().steamId) return 0;
  const auto v = ReadAt<uint64_t>(controller, *Off().steamId);
  if (!v || *v == 0 || (*v >> 52) != 0x011) return 0;  // individual accounts only
  return *v;
}

const char* WinnerToTeamString(int csWinnerTeamNum) {
  auto ctx = WebhookGetMatchContext();
  const auto ms = MatchStateGet();
  bool team1IsCt = true;
  if (ctx && ms.map_number >= 1 && static_cast<size_t>(ms.map_number) <= ctx->map_sides.size()) {
    const std::string& side = ctx->map_sides[static_cast<size_t>(ms.map_number - 1)];
    if (side == "team2_ct") team1IsCt = false;
  }
  if (csWinnerTeamNum == 3) return team1IsCt ? "team1" : "team2";
  if (csWinnerTeamNum == 2) return team1IsCt ? "team2" : "team1";
  return "team1";
}

}  // namespace

bool InitialTeam1IsCtForMap(int mapNumber) {
  auto ctx = WebhookGetMatchContext();
  if (ctx && mapNumber >= 1 && static_cast<size_t>(mapNumber) <= ctx->map_sides.size()) {
    return ctx->map_sides[static_cast<size_t>(mapNumber - 1)] != "team2_ct";
  }
  return true;
}

bool Team1IsCtWithSwapCount(int mapNumber, int swapCount) {
  bool team1IsCt = InitialTeam1IsCtForMap(mapNumber);
  return fleetstate::Team1IsCtAfterSwaps(team1IsCt, swapCount);
}

namespace {

// ---- match stats (match_stats.h) ---------------------------------------------------------------
// Humans are keyed by SteamID64; bots get an id in the dev-bot range from their slot.

double StatsNowSeconds() {
  return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
}

int ControllerSide(void* controller, int slot) {
  if (controller && Off().teamNum) {
    if (auto v = ReadAt<uint8_t>(controller, *Off().teamNum)) {
      if (*v == 2 || *v == 3) return *v;
    }
  }
  if (auto t = GetCsTeamNumForSlot(slot)) return *t;
  return 0;
}

std::string ControllerName(void* controller) {
  if (!controller || !Off().playerName) return {};
  char buf[128];
  std::memcpy(buf, reinterpret_cast<const unsigned char*>(controller) + *Off().playerName, sizeof(buf));
  buf[sizeof(buf) - 1] = 0;
  return buf;
}

uint64_t StatsIdForController(void* controller, int slot) {
  if (slot < 0 || slot >= 64) return 0;
  uint64_t id = SteamFromController(controller);
  if (id == 0) id = SteamForSlot(slot).value_or(0);
  bool bot = false;
  if (id == 0) {
    if (!controller) return 0;
    id = kDevBotIdBase | 0x100000000ull | static_cast<uint64_t>(slot);
    bot = true;
  }
  const int side = ControllerSide(controller, slot);
  std::string name = bot ? ControllerName(controller) : NameForSlot(slot);
  if (name.empty()) name = ControllerName(controller);
  int teamSlot = 0;
  if (!bot) {
    const WebhookTeam t = TeamForSteam(id);
    teamSlot = t == WebhookTeam::Team1 ? 1 : t == WebhookTeam::Team2 ? 2 : 0;
  }
  stats::Current().ObservePlayer(id, name, side, teamSlot, bot);
  return id;
}

uint64_t StatsIdForEvent(const ru_game_event* ev, const char* key) {
  const ru_api* a = A();
  if (!a || !ev) return 0;
  const int slot = a->ev_get_player_slot(a->self, ev, key);
  void* ctrl = a->ev_get_player_controller(a->self, ev, key);
  return StatsIdForController(ctrl, slot);
}

void StatsSyncSidesLocked() {
  const auto ms = MatchStateGet();
  stats::Current().SetTeam1IsCt(Team1IsCtWithSwapCount(ms.map_number <= 0 ? 1 : ms.map_number, g_swapCount));
}

// Every player that spawned takes part in the round that just started.
void StatsRegisterRoundPlayersLocked() {
  std::lock_guard<std::recursive_mutex> lk(stats::Mutex());
  StatsSyncSidesLocked();
  stats::Current().OnRoundStart();
  if (!stats::Current().Live()) return;
  for (int slot : g_spawned) {
    if (void* c = ControllerForSlot(slot)) (void)StatsIdForController(c, slot);
  }
}

void StatsOnGameEventLocked(const char* name, const ru_game_event* ev) {
  const ru_api* a = A();
  std::lock_guard<std::recursive_mutex> lk(stats::Mutex());
  auto& st = stats::Current();
  if (!st.Live()) return;
  StatsSyncSidesLocked();
  if (std::strcmp(name, "player_death") == 0) {
    const uint64_t victim = StatsIdForEvent(ev, "userid");
    const uint64_t attacker = StatsIdForEvent(ev, "attacker");
    const uint64_t assister = StatsIdForEvent(ev, "assister");
    st.OnPlayerDeath(victim, attacker, assister, a->ev_get_int(a->self, ev, "assistedflash", 0) != 0,
                     a->ev_get_int(a->self, ev, "headshot", 0) != 0, a->ev_get_string(a->self, ev, "weapon", ""),
                     StatsNowSeconds());
  } else if (std::strcmp(name, "player_hurt") == 0) {
    st.OnPlayerHurt(StatsIdForEvent(ev, "userid"), StatsIdForEvent(ev, "attacker"),
                    a->ev_get_int(a->self, ev, "dmg_health", 0), a->ev_get_int(a->self, ev, "health", 0),
                    a->ev_get_string(a->self, ev, "weapon", ""));
  } else if (std::strcmp(name, "player_blind") == 0) {
    st.OnPlayerBlind(StatsIdForEvent(ev, "userid"), StatsIdForEvent(ev, "attacker"),
                     a->ev_get_float(a->self, ev, "blind_duration", 0.0));
  } else if (std::strcmp(name, "bomb_planted") == 0) {
    st.OnBombPlanted(StatsIdForEvent(ev, "userid"));
  } else if (std::strcmp(name, "bomb_defused") == 0) {
    st.OnBombDefused(StatsIdForEvent(ev, "userid"));
  } else if (std::strcmp(name, "round_mvp") == 0) {
    st.OnRoundMvp(StatsIdForEvent(ev, "userid"));
  }
}

// Closes the round in the stats model. Returns true (and the team1/team2 map score) when stats
// are live: that score comes from round winners mapped through the current sides, so it stays
// right after halftime (the log-derived MatchState score is CT/T based).
bool StatsOnRoundEndLocked(int csWinnerTeamNum, int reason, int* team1, int* team2) {
  std::lock_guard<std::recursive_mutex> lk(stats::Mutex());
  auto& st = stats::Current();
  if (!st.Live()) return false;
  StatsSyncSidesLocked();
  for (int slot : g_spawned) {
    void* c = ControllerForSlot(slot);
    if (!c) continue;
    const uint64_t id = StatsIdForController(c, slot);
    if (id && Off().score) {
      if (auto v = ReadAt<int>(c, *Off().score)) {
        if (*v >= 0 && *v < 1000000) st.SetScore(id, *v);
      }
    }
  }
  st.OnRoundEnd(csWinnerTeamNum, reason);
  *team1 = st.Team1Score();
  *team2 = st.Team2Score();
  return true;
}

std::optional<int> FindRosterSlot(uint64_t steamid64) {
  if (!steamid64) return std::nullopt;
  for (const auto& s : ListSlotIdentities()) {
    if (s.steamid64 == steamid64 && s.slot >= 0) return s.slot;
  }
  return std::nullopt;
}

// round_end in the MatchZy shape with the scoreboard subset (kills, deaths, assists, HS kills,
// damage, MVPs, score) of every roster player: only when the stats model is not live.
void EmitLegacyRoundEndLocked(const WebhookMatchContext& ctx, const MatchStateSnapshot& ms, int csWinnerTeamNum,
                              int reason) {
  auto bestNameForSteam = [&](uint64_t steamid64) -> std::string {
    if (steamid64 == 0) return {};
    auto it = g_stats.find(steamid64);
    if (it != g_stats.end() && !it->second.name.empty()) return it->second.name;
    for (const auto& s : ListSlotIdentities()) {
      if (s.steamid64 == steamid64 && !s.name.empty()) return s.name;
    }
    return {};
  };

  std::vector<WebhookPlayerStats> team1;
  std::vector<WebhookPlayerStats> team2;
  team1.reserve(ctx.roster_team.size());
  team2.reserve(ctx.roster_team.size());
  const Offsets& o = Off();

  for (const auto& kv : ctx.roster_team) {
    const uint64_t sid = kv.first;
    const WebhookTeam team = kv.second;
    PlayerStats st{};
    if (auto it = g_stats.find(sid); it != g_stats.end()) st = it->second;
    if (st.team == WebhookTeam::Unknown) st.team = team;
    if (st.name.empty()) st.name = bestNameForSteam(sid);

    WebhookPlayerStats out;
    void* ctrl = nullptr;
    if (auto slotOpt = FindRosterSlot(sid)) ctrl = ControllerForSlot(*slotOpt);
    if (ctrl) {
      // Scoreboard netvars (best-effort); event-accumulated stats otherwise.
      // Both reset when the match goes live (mp_restartgame / MatchEventsOnMatchStart). A scoreboard
      // that reads all zeros while the events counted something is not trusted (events win).
      void* ms = MatchStatsOf(ctrl);
      const auto sk = MatchStat(ms, o.kills), sd = MatchStat(ms, o.deaths), sa = MatchStat(ms, o.assists);
      const bool blank = sk.value_or(0) == 0 && sd.value_or(0) == 0 && sa.value_or(0) == 0;
      if (!blank || (st.kills == 0 && st.deaths == 0 && st.assists == 0)) {
        if (sk) st.kills = *sk;
        if (sd) st.deaths = *sd;
        if (sa) st.assists = *sa;
        if (auto v = MatchStat(ms, o.headshots)) {
          if (*v <= st.kills) st.headshot_kills = std::max(st.headshot_kills, *v);
        }
      }
      if (o.mvps) {
        if (auto v = ReadAt<int>(ctrl, *o.mvps)) {
          if (*v >= 0 && *v < 1000) out.mvps = *v;
        }
      }
      if (o.score) {
        if (auto v = ReadAt<int>(ctrl, *o.score)) {
          if (*v >= 0 && *v < 1000000) out.score = *v;
        }
      }
    }
    out.steamid64 = sid;
    out.name = st.name.empty() ? std::to_string(sid) : st.name;
    out.kills = st.kills;
    out.deaths = st.deaths;
    out.assists = st.assists;
    out.headshot_kills = st.headshot_kills;
    out.damage = st.damage;
    if (team == WebhookTeam::Team1) team1.push_back(out);
    else if (team == WebhookTeam::Team2) team2.push_back(out);
  }

  WebhookEmitRoundEndMatchzy(ms.map_number, ms.round_number > 0 ? ms.round_number : g_roundNumber,
                             /*round_time=*/0, reason, WinnerToTeamString(csWinnerTeamNum), ms.team1_score,
                             ms.team2_score, team1, team2);
}

void EmitRoundEndLocked(int csWinnerTeamNum, int reason, int roundTimeMs) {
  auto ctxOpt = WebhookGetMatchContext();
  if (!ctxOpt) return;
  const auto& ctx = *ctxOpt;
  const auto ms = MatchStateGet();

  // The stats model closes the round first: round_end carries the totals including it. Its
  // team1/team2 score comes from round winners through the current sides (right after halftime;
  // the log-derived MatchState score is CT/T based), so the map end prefers it.
  int mapScore1 = ms.team1_score;
  int mapScore2 = ms.team2_score;
  const bool statsLive = StatsOnRoundEndLocked(csWinnerTeamNum, reason, &mapScore1, &mapScore2);
  stats::MapStats snap;
  if (statsLive) {
    {
      std::lock_guard<std::recursive_mutex> slk(stats::Mutex());
      snap = stats::Current().Snapshot();
    }
    // round_end in the AT shape (at_payloads.h): winner {side, team}, team1/team2 with the full
    // per-player stat set (docs/PARITY.md §6, §8).
    const auto series = ModesGetSeriesWins();
    WebhookEmitRoundEndStats(ms.map_number, ms.round_number > 0 ? ms.round_number : g_roundNumber, roundTimeMs,
                             reason, csWinnerTeamNum, snap, series.first, series.second);
  } else {
    EmitLegacyRoundEndLocked(ctx, ms, csWinnerTeamNum, reason);
  }

  // Minimal snapshot for crash/restart recovery.
  persisted_match_state::PersistSnapshot(ms.map_number <= 0 ? 1 : ms.map_number,
                                         ms.round_number > 0 ? ms.round_number : g_roundNumber, ms.team1_score,
                                         ms.team2_score);

  // Backup prefix for the current map + discover the latest backup file.
  {
    const int mapNumber = ms.map_number <= 0 ? 1 : ms.map_number;
    const std::string prefix = "readyup_backup_" + std::to_string(static_cast<unsigned long long>(ctx.matchid)) +
                               "_map" + std::to_string(mapNumber) + "_";
    persisted_match_state::PersistBackupPrefix(prefix);
    (void)EnqueueServerCommand(("mp_backup_round_file " + prefix).c_str());
    backup_files::DiscoverAndPersistNewestBackupFileAsync(prefix);
  }

  if (statsLive && signals::Enabled()) {
    // event.round_end (docs/FLEET.md §8.1, §13): the round's own RoundSummary, queued before the
    // map result OnMatchRoundEnded may trigger.
    std::string rj;
    int roundNo = -1;
    if (!snap.rounds.empty()) {
      rj = stats::ToJson(snap.rounds.back());
      roundNo = snap.rounds.back().round_number;
    }
    status::Json round;
    if (!rj.empty() && status::Json::Parse(rj, &round)) {
      status::Json d = status::Json::Object();
      d["round"] = std::move(round);
      signals::Emit("round_end", std::move(d), roundNo, ms.map_number <= 0 ? 1 : ms.map_number);
    }
  }
  OnMatchRoundEnded(ms.map_number, mapScore1, mapScore2, ms.current_map);
  match_recovery::NoteProgress();  // stats + series score for crash recovery (match_recovery.h)
}

void OnRoundStartLocked(const PendingRound& ev) {
  const auto ms = MatchStateGet();
  MaybeResetForMapLocked(ms.map_number);
  const int mapNumber = ms.map_number <= 0 ? 1 : ms.map_number;

  // Knife decider: no match rounds, no counters.
  if (GetMode() == ReadyUpMode::MatchKnife) {
    KnifeOnRoundStart(mapNumber, "event");
    return;
  }

  int team1Score = ms.team1_score;
  int team2Score = ms.team2_score;

  auto ctxOpt = WebhookGetMatchContext();
  if (ctxOpt) {
    const auto& ctx = *ctxOpt;
    const int maxRounds = std::max(1, ctx.maxRounds);
    const bool otEnabled = ctx.overtime_enabled;
    const int seg = std::max(1, ctx.overtimeSegments);
    const int sum = team1Score + team2Score;

    bool emitHalfStart = false;
    bool emitOvertimeStart = false;
    int overtimeNumber = 0;

    // Regulation halftime boundary.
    const int regHalf = maxRounds / 2;
    if (sum == regHalf && g_lastHalfStartTotal != sum) emitHalfStart = true;

    // Overtime boundaries. CS2 does not swap sides when an overtime starts (each team keeps the
    // side it ended the previous half on); it swaps at each overtime's own halftime, whatever the
    // score is then. Seen on a live server (M1 play-test, MR8 + MR6 OT): swapping at the OT start
    // and skipping the OT halftime (score not tied) credited rounds to the wrong team, so the map
    // never ended for Ready Up while CS2 had ended it.
    if (otEnabled && sum >= maxRounds) {
      const int roundsPastReg = sum - maxRounds;
      const int blockSize = 2 * seg;
      if (blockSize > 0) {
        const int block = roundsPastReg / blockSize;  // 0-based OT index
        const int offset = roundsPastReg % blockSize;
        overtimeNumber = block + 1;
        if (offset == 0 && team1Score == team2Score) {
          if (overtimeNumber > g_lastOvertimeNumber) {
            emitOvertimeStart = true;
            g_lastOvertimeNumber = overtimeNumber;
          }
        } else if (offset == seg) {
          if (g_lastHalfStartTotal != sum) emitHalfStart = true;
        }
      }
    }

    if (emitOvertimeStart && overtimeNumber > 0) WebhookEmitOvertimeStarted(mapNumber, overtimeNumber);

    // Every half start implies a side swap (except the very first half).
    if (emitHalfStart) {
      g_lastHalfStartTotal = sum;
      const int swapCountForThisRound = g_swapCount + 1;
      const bool team1IsCtNow = Team1IsCtWithSwapCount(mapNumber, swapCountForThisRound);
      WebhookEmitHalftimeStarted(mapNumber, team1Score, team2Score);
      if (sum == regHalf) EsportsOnHalftime();  // mp_halftime_pausematch (ruleset)
      WebhookEmitSideSwap(mapNumber, team1IsCtNow ? "CT" : "T", team1IsCtNow ? "T" : "CT");
      g_swapCount = swapCountForThisRound;
      g_swapCountPub = g_swapCount;
    }

    // Event-provided ct/t scores, mapped with the current swap count.
    if (ev.ct != INT_MIN && ev.t != INT_MIN) {
      const bool team1IsCtNow = Team1IsCtWithSwapCount(mapNumber, g_swapCount);
      const int ctScore = std::max(0, ev.ct);
      const int tScore = std::max(0, ev.t);
      team1Score = team1IsCtNow ? ctScore : tScore;
      team2Score = team1IsCtNow ? tScore : ctScore;
    }
  }

  g_roundNumber += 1;
  MatchStateSetRound(g_roundNumber);

  // Warmup -> live (first real round start).
  if (GetMode() == ReadyUpMode::MatchWarmup) OnMatchRoundStarted();
  WebhookEmitRoundStarted(mapNumber, g_roundNumber, team1Score, team2Score);
  StatsRegisterRoundPlayersLocked();
}

double g_ignoreRoundEndsUntil = 0.0;

void OnRoundEndLocked(const PendingRound& ev) {
  if (host::NowSeconds() < g_ignoreRoundEndsUntil) {
    Print("match-events: round end (winner=%d reason=%d) ignored: backup restore\n", ev.winner, ev.reason);
    return;
  }
  const auto ms = MatchStateGet();
  MaybeResetForMapLocked(ms.map_number);
  if (GetMode() == ReadyUpMode::MatchKnife) {
    // Only eliminations are decided from the event (CS2 reasons 8 = CTs win, 9 = Terrorists
    // win). Time-outs / draws need the alive/HP tiebreak the log path (SFUI notice) runs; the
    // first source wins, the other is ignored by phase.
    if (ev.reason == 8 || ev.reason == 9) {
      KnifeOnRoundEnd(ms.map_number <= 0 ? 1 : ms.map_number, ev.winner, /*elimination=*/true, "event",
                      ev.reason == 8 ? "reason=8" : "reason=9");
    }
    return;
  }
  EmitRoundEndLocked(ev.winner, ev.reason, ev.roundTimeMs);
}

void OnPlayerDeathLocked(const ru_game_event* ev) {
  const ru_api* a = A();
  const int attackerSlot = a->ev_get_player_slot(a->self, ev, "attacker");
  const int victimSlot = a->ev_get_player_slot(a->self, ev, "userid");
  const int assisterSlot = a->ev_get_player_slot(a->self, ev, "assister");
  const bool headshot = a->ev_get_int(a->self, ev, "headshot", 0) != 0;
  const auto attackerSid = SteamForSlot(attackerSlot);
  const auto victimSid = SteamForSlot(victimSlot);
  const auto assisterSid = SteamForSlot(assisterSlot);

  if (attackerSid) {
    auto& s = g_stats[*attackerSid];
    const std::string nm = NameForSlot(attackerSlot);
    if (!nm.empty()) s.name = nm;
    s.team = TeamForSteam(*attackerSid);
    s.kills += 1;
    if (headshot) s.headshot_kills += 1;
  }
  if (victimSid) {
    auto& s = g_stats[*victimSid];
    const std::string nm = NameForSlot(victimSlot);
    if (!nm.empty()) s.name = nm;
    s.team = TeamForSteam(*victimSid);
    s.deaths += 1;
  }
  if (assisterSid && attackerSid && victimSid && *assisterSid != *attackerSid && *assisterSid != *victimSid) {
    auto& s = g_stats[*assisterSid];
    const std::string nm = NameForSlot(assisterSlot);
    if (!nm.empty()) s.name = nm;
    s.team = TeamForSteam(*assisterSid);
    s.assists += 1;
  }
}

void OnPlayerHurtLocked(const ru_game_event* ev) {
  const ru_api* a = A();
  const int attackerSlot = a->ev_get_player_slot(a->self, ev, "attacker");
  const int dmg = a->ev_get_int(a->self, ev, "dmg_health", 0);
  const auto attackerSid = SteamForSlot(attackerSlot);
  if (!attackerSid) return;
  auto& s = g_stats[*attackerSid];
  const std::string nm = NameForSlot(attackerSlot);
  if (!nm.empty()) s.name = nm;
  s.team = TeamForSteam(*attackerSid);
  s.damage += std::max(0, dmg);
}

int ScoreKey(const ru_game_event* ev, std::initializer_list<const char*> keys) {
  const ru_api* a = A();
  for (const char* k : keys) {
    const int v = a->ev_get_int(a->self, ev, k, INT_MIN);
    if (v != INT_MIN) return v;
  }
  return INT_MIN;
}

// Raw engine events: synchronous, inside the engine's dispatch (never keep `ev`).
void OnGameEvent(void* /*user*/, const char* name, const ru_game_event* ev) {
  if (!name || !ev) return;
  if (std::strcmp(name, "round_announce_warmup") == 0) {
    MatchFeaturesOnGameEvent(name);  // a new warmup is not a round-over (match_features.h)
    OnNativeWarmupStarted("round_announce_warmup event");
    return;
  }
  if (std::strcmp(name, "round_end") == 0) MatchFeaturesOnGameEvent(name);  // restores wait (match_features.h)
  if (std::strcmp(name, "round_start") == 0) {
    WelcomeObserveRoundStart();     // the cards wait it out
    GoLiveCardObserveRoundStart();  // (golive_card.h)
  }
  if (std::strcmp(name, "round_start") == 0 || std::strcmp(name, "round_freeze_end") == 0) {
    MatchFeaturesOnGameEvent(name);  // freeze time tracking for pauses (match_features.h)
    if (name[6] == 'f') {
      GoLiveCardObserveFreezeEnd();  // the round is being played: the go-live card ends
      std::lock_guard<std::mutex> lk(g_pendingMu);
      g_freezeEndAt = host::NowSeconds();  // round_time of round_end
      return;
    }
  }
  if (std::strcmp(name, "round_start") == 0 || std::strcmp(name, "round_end") == 0) {
    PendingRound p;
    const ru_api* a = A();
    p.start = name[6] == 's';
    if (p.start) {
      p.ct = ScoreKey(ev, {"ct_score", "score_ct", "ct_score_total"});
      p.t = ScoreKey(ev, {"t_score", "score_t", "t_score_total"});
    } else {
      p.winner = a->ev_get_int(a->self, ev, "winner", 0);
      p.reason = a->ev_get_int(a->self, ev, "reason", 0);
    }
    std::lock_guard<std::mutex> lk(g_pendingMu);
    if (p.start) {
      g_freezeEndAt = 0.0;
    } else if (g_freezeEndAt > 0.0) {
      const double ms = (host::NowSeconds() - g_freezeEndAt) * 1000.0;
      p.roundTimeMs = ms > 0.0 && ms < 3600000.0 ? static_cast<int>(ms) : 0;
    }
    if (g_pending.size() < 64) g_pending.push_back(p);
    return;
  }

  const auto ms = MatchStateGet();
  std::lock_guard<std::recursive_mutex> lk(g_mu);
  MaybeResetForMapLocked(ms.map_number);
  if (std::strcmp(name, "player_spawn") == 0) {
    const int slot = A()->ev_get_player_slot(A()->self, ev, "userid");
    if (slot >= 0) g_spawned.insert(slot);
    if (slot >= 0) WelcomeObservePlayerSpawn(slot);  // the welcome card waits for this
    return;
  }
  if (std::strcmp(name, "player_disconnect") == 0) {
    const int slot = A()->ev_get_player_slot(A()->self, ev, "userid");
    if (slot >= 0) g_spawned.erase(slot);
    return;
  }
  if (std::strcmp(name, "player_death") == 0) {
    OnPlayerDeathLocked(ev);
  } else if (std::strcmp(name, "player_hurt") == 0) {
    OnPlayerHurtLocked(ev);
  }
  StatsOnGameEventLocked(name, ev);
}

}  // namespace

void MatchEventsSetResumeSide(int mapNumber, bool team1Ct) {
  std::lock_guard<std::recursive_mutex> lk(g_mu);
  if (mapNumber <= 0) mapNumber = 1;
  g_resumeSide = ResumeSide{mapNumber, team1Ct};
  MaybeResetForMapLocked(mapNumber);
  ApplyResumeSideLocked(mapNumber);
  g_swapCountPub = g_swapCount;
  StatsSyncSidesLocked();
}

void MatchEventsClearResumeSide() {
  std::lock_guard<std::recursive_mutex> lk(g_mu);
  g_resumeSide.reset();
}

int MatchEventsSwapCountFor(int mapNumber) {
  return g_lastMapNumberPub.load() == (mapNumber <= 0 ? 1 : mapNumber) ? g_swapCountPub.load() : 0;
}

void MatchEventsOnMatchStart() {
  std::lock_guard<std::recursive_mutex> lk(g_mu);
  g_roundNumber = 0;
  g_swapCount = 0;
  g_lastHalfStartTotal = -1;
  g_lastOvertimeNumber = 0;
  g_stats.clear();
  ApplyResumeSideLocked(std::max(1, MatchStateGet().map_number));
  g_swapCountPub = g_swapCount;
  StatsSyncSidesLocked();
  MatchStateSetRound(0);
  MatchStateSetScore(0, 0);
}

void MatchEventsIgnoreRoundEndsFor(double seconds) {
  std::lock_guard<std::recursive_mutex> lk(g_mu);
  g_ignoreRoundEndsUntil = host::NowSeconds() + seconds;
}

void MatchEventsInstall(const ru_api* api) {
  static const char* const kEvents[] = {
      "round_start", "round_end", "round_freeze_end", "round_announce_warmup", "player_spawn", "player_disconnect", "player_death",
      "player_hurt", "player_blind", "bomb_planted", "bomb_defused", "round_mvp",
  };
  for (const char* e : kEvents) {
    if (!api->subscribe_game_event(api->self, e, &OnGameEvent, nullptr)) {
      Print("match-events: could not subscribe to %s\n", e);
    }
  }
}

void MatchEventsTick() {
  const ru_api* a = A();
  if (a) g_eventsLive.store(a->feature_state(a->self, "events_live") == 1 ? 1 : 0, std::memory_order_relaxed);
  std::deque<PendingRound> q;
  {
    std::lock_guard<std::mutex> lk(g_pendingMu);
    q.swap(g_pending);
  }
  for (const auto& p : q) {
    std::lock_guard<std::recursive_mutex> lk(g_mu);
    if (p.start) OnRoundStartLocked(p);
    else OnRoundEndLocked(p);
  }
}

bool GameEventsListenerInstalled() {
  const ru_api* a = A();
  if (a && host::OnGameThread()) {
    const int v = a->feature_state(a->self, "events_live") == 1 ? 1 : 0;
    g_eventsLive.store(v, std::memory_order_relaxed);
    return v == 1;
  }
  return g_eventsLive.load(std::memory_order_relaxed) == 1;
}

std::optional<int> GetCsTeamNumForSlot(int slot) {
  if (slot < 0) return std::nullopt;
  if (GameEventsListenerInstalled() && host::OnGameThread() && Off().teamNum) {
    if (void* c = ControllerForSlot(slot)) {
      if (auto v = ReadAt<int>(c, *Off().teamNum)) {
        if (*v == 2 || *v == 3) return *v;
      }
      return std::nullopt;
    }
  }
  // Log-derived team (the core's registry) of the player whose log <N> is this slot.
  for (const auto& h : ListHumans()) {
    if (h.userid == slot) return (h.team == 2 || h.team == 3) ? std::optional<int>(h.team) : std::nullopt;
  }
  for (const auto& b : ListBots()) {
    if (b.userid == slot) return (b.team == 2 || b.team == 3) ? std::optional<int>(b.team) : std::nullopt;
  }
  return std::nullopt;
}

std::pair<int, int> GetRosterTeamDamageTotals() {
  std::lock_guard<std::recursive_mutex> lk(g_mu);
  auto ctxOpt = WebhookGetMatchContext();
  if (!ctxOpt) return {0, 0};
  int team1 = 0, team2 = 0;
  for (const auto& kv : ctxOpt->roster_team) {
    auto it = g_stats.find(kv.first);
    if (it == g_stats.end()) continue;
    const int dmg = std::max(0, it->second.damage);
    if (kv.second == WebhookTeam::Team1) team1 += dmg;
    else if (kv.second == WebhookTeam::Team2) team2 += dmg;
  }
  return {team1, team2};
}

std::optional<int> GameEventsSlotForSteam(uint64_t steamid64) {
  const ru_api* a = A();
  if (!a || steamid64 == 0 || !host::OnGameThread()) return std::nullopt;
  const int s = a->slot_for_steamid(a->self, steamid64);
  return s >= 0 ? std::optional<int>(s) : std::nullopt;
}

MatchEventsState MatchEventsSnapshot() {
  std::lock_guard<std::recursive_mutex> lk(g_mu);
  MatchEventsState s;
  s.roundNumber = g_roundNumber;
  s.lastMapNumber = g_lastMapNumber;
  s.swapCount = g_swapCount;
  s.lastHalfStartTotal = g_lastHalfStartTotal;
  s.lastOvertimeNumber = g_lastOvertimeNumber;
  for (const auto& kv : g_stats) {
    MatchEventsState::Totals t;
    t.steamid64 = kv.first;
    t.name = kv.second.name;
    t.team = static_cast<int>(kv.second.team);
    t.kills = kv.second.kills;
    t.deaths = kv.second.deaths;
    t.assists = kv.second.assists;
    t.headshot_kills = kv.second.headshot_kills;
    t.damage = kv.second.damage;
    s.players.push_back(std::move(t));
  }
  return s;
}

void MatchEventsRestore(const MatchEventsState& s) {
  std::lock_guard<std::recursive_mutex> lk(g_mu);
  g_roundNumber = s.roundNumber;
  g_lastMapNumber = s.lastMapNumber;
  g_swapCount = s.swapCount;
  g_swapCountPub = g_swapCount;
  g_lastMapNumberPub = g_lastMapNumber;
  g_lastHalfStartTotal = s.lastHalfStartTotal;
  g_lastOvertimeNumber = s.lastOvertimeNumber;
  g_stats.clear();
  for (const auto& t : s.players) {
    PlayerStats p;
    p.name = t.name;
    p.team = static_cast<WebhookTeam>(t.team);
    p.kills = t.kills;
    p.deaths = t.deaths;
    p.assists = t.assists;
    p.headshot_kills = t.headshot_kills;
    p.damage = t.damage;
    g_stats[t.steamid64] = p;
  }
  // Players who spawned before the reload take part in rounds from now on.
  for (const auto& h : ListHumans()) {
    if (h.userid >= 0 && (h.team == 2 || h.team == 3)) g_spawned.insert(h.userid);
  }
  for (const auto& b : ListBots()) {
    if (b.userid >= 0 && (b.team == 2 || b.team == 3)) g_spawned.insert(b.userid);
  }
}

}  // namespace readyup

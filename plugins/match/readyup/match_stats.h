#pragma once

// Internal per-player match statistics, per round and per map.
//
// Engine-free: game_events.cpp resolves engine events to player ids and sides and
// feeds a StatsAccumulator; tests/match_stats_test.cpp drives one directly. The
// transport (what is sent where, and in which shape) is designed separately; this
// file only owns the model and a JSON serializer for it (ToJson).
//
// Ids: SteamID64 for humans. game_events.cpp gives bots ids in the reserved
// dev-bot range (slot_registry.h IsDevBotId) so they can be tracked; consumers
// decide whether to show them.
// Sides: CS team numbers, 2 = T, 3 = CT. Team slots: 1 = team1, 2 = team2, 0 = unknown.
//
// Definitions:
//  - kills exclude team kills and suicides (those count in team_kills / suicides);
//    deaths always count. Suicide = attacker is the victim or the world.
//  - assists: an assist on an enemy; a flash assist counts in flash_assists instead.
//  - damage: health actually removed from enemies (capped at the victim's remaining
//    health); utility_damage: the part dealt by HE / molotov / incendiary.
//  - enemies_flashed / friendlies_flashed: player_blind with a duration > 0.
//  - entry_kills_* / entry_deaths_*: the first kill of a round (not a team kill),
//    split by the side the player was on.
//  - trade_kills: killing an enemy within 5 s of that enemy killing a teammate; the
//    teammate's death counts as traded (traded_deaths) and for KAST.
//  - kast_rounds: rounds with a kill, assist (incl. flash assist), survival or trade.
//  - clutches_won[n-1]: rounds won as the last player alive against n enemies (1..5).
//  - multi_kills[n-1]: rounds with exactly n kills (5 = five or more).
//  - rounds_played: rounds the player was on CT/T for.
//  - mvp: round_mvp awards; score: the engine scoreboard score (set by the feeder).

#include <array>
#include <cstdint>
#include <mutex>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace readyup::stats {

struct PlayerStats {
  int kills = 0;
  int deaths = 0;
  int assists = 0;
  int flash_assists = 0;
  int team_kills = 0;
  int suicides = 0;
  int headshot_kills = 0;
  int knife_kills = 0;
  int damage = 0;
  int utility_damage = 0;
  int enemies_flashed = 0;
  int friendlies_flashed = 0;
  int bomb_plants = 0;
  int bomb_defuses = 0;
  int entry_kills_t = 0;
  int entry_kills_ct = 0;
  int entry_deaths_t = 0;
  int entry_deaths_ct = 0;
  int trade_kills = 0;
  int traded_deaths = 0;
  int kast_rounds = 0;
  int rounds_played = 0;
  int mvp = 0;
  int score = 0;
  std::array<int, 5> multi_kills{};    // [0] = 1k ... [4] = 5k
  std::array<int, 5> clutches_won{};   // [0] = 1v1 ... [4] = 1v5
};

// One player's part in one round.
struct PlayerRound {
  uint64_t id = 0;
  int team = 0;  // slot
  int side = 0;  // 2 / 3
  int kills = 0;
  int assists = 0;
  int flash_assists = 0;
  int damage = 0;
  int utility_damage = 0;
  int headshot_kills = 0;
  bool died = false;
  bool survived = false;
  bool traded = false;
  bool kast = false;
  bool entry_kill = false;
  bool entry_death = false;
  bool mvp = false;
  int clutch_vs = 0;       // >0: was the last alive against this many enemies
  bool clutch_won = false;
  // Economy (round_economy.h): money when the round started and after the freeze-time buys,
  // armour, helmet, defuse kit and what they carried into the round. -1 = not read.
  int money_start = -1;
  int money_after_buy = -1;
  int armor = -1;
  bool helmet = false;
  bool defuser = false;
  std::vector<std::string> loadout;  // "ak47", "smokegrenade", ... (weapon_ dropped)
};

struct RoundSummary {
  int round_number = 0;   // 1-based, counts rounds recorded on this map
  int winner_side = 0;    // 2 / 3 / 0
  int winner_team = 0;    // slot 1 / 2 / 0
  int reason = 0;         // engine round_end reason
  int team1_score = 0;    // map score after the round
  int team2_score = 0;
  bool team1_was_ct = true;
  std::vector<PlayerRound> players;
};

struct PlayerLine {
  uint64_t id = 0;
  std::string name;
  int team = 0;       // slot
  int last_side = 0;  // 2 / 3
  bool bot = false;
  PlayerStats stats;
};

struct TeamLine {
  int score = 0;
  int score_ct = 0;  // rounds won on CT
  int score_t = 0;   // rounds won on T
};

// Snapshot of one map.
struct MapStats {
  bool live = false;
  bool team1_is_ct = true;  // current side of team1
  TeamLine team1;
  TeamLine team2;
  std::vector<PlayerLine> players;   // sorted by team, then id
  std::vector<RoundSummary> rounds;  // in play order
};

class StatsAccumulator {
 public:
  // Clears everything and starts a map (going live). team1IsCt: team1's starting side.
  void BeginMap(bool team1IsCt);
  // Stops recording (map over / match unloaded); the data stays readable.
  void EndMap() { live_ = false; }
  void Clear();
  bool Live() const { return live_; }
  // Rounds recorded on this map (closed by OnRoundEnd).
  int RoundsRecorded() const { return static_cast<int>(rounds_.size()); }

  void SetTeam1IsCt(bool team1IsCt) { team1IsCt_ = team1IsCt; }
  bool Team1IsCt() const { return team1IsCt_; }

  // Player is on `side` (2/3) now. teamSlot 0 = derive from the side (first time only).
  // While live, a player seen on CT/T takes part in the current round.
  void ObservePlayer(uint64_t id, const std::string& name, int side, int teamSlot, bool bot = false);

  void OnRoundStart();
  // time: seconds on any monotonic clock (trades).
  void OnPlayerDeath(uint64_t victim, uint64_t attacker, uint64_t assister, bool assistedFlash, bool headshot,
                     const std::string& weapon, double time);
  // healthAfter: the victim's health after the hit (player_hurt `health`).
  void OnPlayerHurt(uint64_t victim, uint64_t attacker, int dmgHealth, int healthAfter, const std::string& weapon);
  void OnPlayerBlind(uint64_t victim, uint64_t attacker, double duration);
  void OnBombPlanted(uint64_t id);
  void OnBombDefused(uint64_t id);
  void OnRoundMvp(uint64_t id);
  // winnerSide 2/3 (else nobody). Closes the round: scores, rounds_played, KAST,
  // multi-kills, clutches and the round summary.
  void OnRoundEnd(int winnerSide, int reason);
  // Engine scoreboard score (CCSPlayerController::m_iScore).
  void SetScore(uint64_t id, int score);
  // The player's money at the round start (round_economy.h).
  void SetMoneyStart(uint64_t id, int money);
  // What the player has when the freeze time ends: money left, armour, helmet, kit, loadout.
  void SetLoadout(uint64_t id, int moneyAfterBuy, int armor, bool helmet, bool defuser,
                  const std::vector<std::string>& loadout);

  int Team1Score() const { return t1_.score; }
  int Team2Score() const { return t2_.score; }
  TeamLine Team1Line() const { return t1_; }
  TeamLine Team2Line() const { return t2_; }
  MapStats Snapshot() const;
  // Plugin reload: continue a map from a Snapshot() (the round in progress starts over empty).
  void Restore(const MapStats& m);

 private:
  struct Player {
    std::string name;
    int team = 0;
    int lastSide = 0;
    bool bot = false;
    PlayerStats s;
  };
  struct Round {
    std::unordered_map<uint64_t, int> side;  // participants
    std::unordered_set<uint64_t> dead;
    std::unordered_map<uint64_t, PlayerRound> pr;
    std::unordered_map<uint64_t, int> health;
    std::unordered_map<uint64_t, std::pair<uint64_t, double>> deathBy;  // victim -> (killer, time)
    bool entryDone = false;
    std::unordered_map<int, std::pair<uint64_t, int>> clutch;  // side -> (id, enemies)
  };

  int SlotForSide(int side) const;
  int SideOf(uint64_t id) const;
  Player& P(uint64_t id) { return players_[id]; }
  PlayerRound& R(uint64_t id);

  bool live_ = false;
  bool team1IsCt_ = true;
  std::unordered_map<uint64_t, Player> players_;
  Round round_;
  TeamLine t1_, t2_;
  std::vector<RoundSummary> rounds_;
};

// JSON for the internal model (field names as in the structs above).
std::string ToJson(const PlayerStats& s);
std::string ToJson(const RoundSummary& r);
std::string ToJson(const MapStats& m);
std::string JsonEscape(const std::string& s);
// Inverse of ToJson(const MapStats&). False if `json` is not such a document.
bool FromJson(const std::string& json, MapStats* out);

// Round restore (docs/FLEET.md §11.3, `rounds_voided`): drops rounds with round_number >=
// fromRound from a map snapshot, with their contribution to the team scores and to every player
// total a PlayerRound carries (kills, assists, flash assists, damage, utility damage, HS kills,
// deaths, KAST, rounds played, MVPs, entry kills/deaths, traded deaths, multi-kills, clutches).
// Totals PlayerRound does not carry (trade kills, team kills, suicides, knife kills, flashes, bomb
// plants/defuses, scoreboard score) are kept. team1_is_ct becomes team1's side in the first
// dropped round. Returns the number of rounds dropped.
int RewindTo(MapStats* m, int fromRound);

// Process-wide accumulator for the current map. Lock Mutex() around every access.
StatsAccumulator& Current();
std::recursive_mutex& Mutex();

}  // namespace readyup::stats

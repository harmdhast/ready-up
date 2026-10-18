#pragma once

// Simulation mode (match config `simulation: true`, `simulation_timescale`): bots play a whole
// match or series for platform end-to-end tests. The pure logic (no engine calls; ctest
// `match_simulation`); the engine side is simulation.h.
//
// One bot per roster player (a team without a roster gets `fill` anonymous bots). A bot that joins
// takes a free roster identity of the team on its side and keeps it for the map: its stats and
// the player events use that SteamID64 and name, so the platform sees the configured roster play.
// Bots are added one at a time (`bot_join_team CT|T` + `bot_quota n`, the quota only ever goes
// up while the map runs) until every side has its bots; a missing bot is added again.

#include <cmath>
#include <cstdint>
#include <map>
#include <string>
#include <vector>

namespace readyup {
namespace sim {

// host_timescale while a simulated map is live: 0.1 .. 4 (1 = real time; anything invalid is 1).
constexpr double kMinTimescale = 0.1;
constexpr double kMaxTimescale = 4.0;
// Inline: the match config parser uses it (and is linked into many tests).
inline double ClampTimescale(double ts) {
  if (!std::isfinite(ts) || ts <= 0) return 1.0;
  return ts < kMinTimescale ? kMinTimescale : ts > kMaxTimescale ? kMaxTimescale : ts;
}
// "2", "1.5", "0.25": the value for `host_timescale <text>`.
std::string TimescaleText(double ts);

// A roster player a bot plays as. steamid64 0 = an anonymous filler (a team without a roster).
struct Identity {
  uint64_t steamid64 = 0;
  std::string name;
  int team = 0;  // 1 / 2
};

// Identities for the match: team1 then team2, each by SteamID64 (stable order), `fillPerTeam`
// fillers for a team without roster players. Entries with another team are dropped.
std::vector<Identity> PlanIdentities(const std::vector<Identity>& roster, int fillPerTeam);

// CS side (3 CT / 2 T) of team 1 / 2.
int SideOfTeam(int team, bool team1IsCt);

struct Bot {
  int userid = -1;
  int side = 0;  // 0 unassigned, 1 spectator, 2 T, 3 CT
};

// userid -> index into `ids`. A bot keeps the identity it has while it is on the server (sides swap
// at halftime; the identity stays). A bot without one takes the first free identity of the team
// playing its side (team1IsCt: now); bots on no side, or with no free identity, get none.
std::map<int, int> Assign(const std::map<int, int>& prev, const std::vector<Bot>& bots,
                          const std::vector<Identity>& ids, bool team1IsCt);

// Bots each side should have: the identities of the team playing it.
void WantedPerSide(const std::vector<Identity>& ids, bool team1IsCt, int* ctWanted, int* tWanted);

// Mixed matches: fillers remain anonymous and never borrow a human's identity.
struct FillPlan {
  int ctWanted = 0;
  int tWanted = 0;
  std::vector<int> removeUserids;
};
FillPlan PlanFill(int playersPerTeam, int humansCt, int humansT, const std::vector<Bot>& bots);

// Bot fill, live: roster humans who left keep their seat (no bot takes it) for `holdSeconds`.
// `missingSince` maps each human seen live to when they went missing (0 = on the server);
// `here` is who is on now. Updates it (drops those past the hold) and returns the seats still
// held, so the side stays short and CS2's auto 5v5 pause waits for them.
std::vector<uint64_t> HeldSeats(std::map<uint64_t, double>* missingSince, const std::vector<uint64_t>& here,
                                double now, int holdSeconds);
std::vector<std::string> FillSetupCommands();
std::vector<std::string> FillTeardownCommands();

// When to add the next bot. Fed once per tick with the bots on the server.
class BotFeeder {
 public:
  // now: seconds (monotonic). total: every bot on the server; ct / t: those on a side.
  // Returns the side (3 / 2) to add a bot on now, with the bot_quota to set in *quota, or 0.
  // One bot per `kAddInterval`; while a requested bot has not appeared yet, or a bot is on no
  // side, it waits (at most `kSettleSeconds`, then it counts what is there). Never above maxBots.
  int Next(double now, int total, int ct, int t, int ctWanted, int tWanted, int maxBots, int* quota);
  // More bots than the rosters need (a bot that was slow to join arrived after a replacement was
  // asked for): once every bot is on a side, at most every `kSettleSeconds`, returns true with the
  // bot_quota for the rosters (CS2's normal quota mode kicks from the fuller side). The extra bots
  // played as no one ("Unknown" in the stats).
  bool Trim(double now, int total, int ct, int t, int ctWanted, int tWanted, int* quota);
  // A new map: the engine adds the quota's bots again; start over from the bots that are there.
  void Reset() { *this = BotFeeder(); }
  int QuotaSent() const { return sent_; }

  static constexpr double kAddInterval = 1.0;
  static constexpr double kSettleSeconds = 8.0;

 private:
  int sent_ = 0;
  double lastAdd_ = -1e9;
  double unsettledSince_ = -1;
  double lastTrim_ = -1e9;
};

// Seconds after a bot took its identity before it "types .ready": 1.5 .. 3.5 s, spread by index.
double ReadyDelaySeconds(int index);

// Console commands. Setup: before the first bot of a match (bots join an empty server, play, stay
// where they are put). Add: one more bot on `side`. Timescale: while a simulated map is live
// (none at 1x). Teardown: the match is gone (bots out, CS2's bot and speed defaults back).
std::vector<std::string> SetupCommands();
std::vector<std::string> AddBotCommands(int side, int quota);

// Bots on the wrong side: one side has more than it needs while the other is short. Adding cannot
// fix that (bot_join_team only places new bots), so the fill starts over. Seen after the engine
// refilled a kept bot_quota with the last bot_join_team (all ten on T, cs.sivert.io 2026-10-07).
bool WrongSides(int ct, int t, int ctWanted, int tWanted);
std::vector<std::string> TimescaleCommands(double ts);
std::vector<std::string> RealTimeCommands();
std::vector<std::string> TeardownCommands();

}  // namespace sim
}  // namespace readyup

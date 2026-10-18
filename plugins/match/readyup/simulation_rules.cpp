#include "readyup/simulation_rules.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <set>

namespace readyup {
namespace sim {

FillPlan PlanFill(int playersPerTeam, int humansCt, int humansT, const std::vector<Bot>& bots) {
  FillPlan p;
  const int size = std::max(1, std::min(32, playersPerTeam));
  p.ctWanted = std::max(0, size - std::max(0, humansCt));
  p.tWanted = std::max(0, size - std::max(0, humansT));
  int ct = 0, t = 0;
  for (const auto& b : bots) {
    if (b.side == 3 && ++ct > p.ctWanted) p.removeUserids.push_back(b.userid);
    if (b.side == 2 && ++t > p.tWanted) p.removeUserids.push_back(b.userid);
  }
  return p;
}

std::vector<uint64_t> HeldSeats(std::map<uint64_t, double>* missingSince, const std::vector<uint64_t>& here,
                                double now, int holdSeconds) {
  std::vector<uint64_t> held;
  for (auto it = missingSince->begin(); it != missingSince->end();) {
    if (std::find(here.begin(), here.end(), it->first) != here.end()) {
      it->second = 0;
      ++it;
      continue;
    }
    if (it->second == 0) it->second = now;
    if (holdSeconds <= 0 || now - it->second > holdSeconds) {
      it = missingSince->erase(it);
      continue;
    }
    held.push_back(it->first);
    ++it;
  }
  return held;
}

std::vector<std::string> FillSetupCommands() {
  auto commands = SetupCommands();
  // bot_kick also kicks GOTV. Quota zero removes gameplay bots while keeping the demo alive.
  commands.erase(std::remove(commands.begin(), commands.end(), "bot_kick"), commands.end());
  return commands;
}
std::vector<std::string> FillTeardownCommands() {
  auto commands = TeardownCommands();
  commands.erase(std::remove(commands.begin(), commands.end(), "bot_kick"), commands.end());
  return commands;
}

std::string TimescaleText(double ts) {
  char buf[32];
  std::snprintf(buf, sizeof(buf), "%.2f", ClampTimescale(ts));
  std::string s = buf;
  while (!s.empty() && s.back() == '0') s.pop_back();
  if (!s.empty() && s.back() == '.') s.pop_back();
  return s;
}

std::vector<Identity> PlanIdentities(const std::vector<Identity>& roster, int fillPerTeam) {
  std::vector<Identity> out;
  for (int team = 1; team <= 2; ++team) {
    std::vector<Identity> mine;
    for (const auto& r : roster) {
      if (r.team == team && r.steamid64 != 0) mine.push_back(r);
    }
    std::sort(mine.begin(), mine.end(), [](const Identity& a, const Identity& b) { return a.steamid64 < b.steamid64; });
    if (mine.empty()) {
      for (int i = 0; i < fillPerTeam; ++i) mine.push_back(Identity{0, std::string(), team});
    }
    out.insert(out.end(), mine.begin(), mine.end());
  }
  return out;
}

int SideOfTeam(int team, bool team1IsCt) {
  if (team == 1) return team1IsCt ? 3 : 2;
  if (team == 2) return team1IsCt ? 2 : 3;
  return 0;
}

std::map<int, int> Assign(const std::map<int, int>& prev, const std::vector<Bot>& bots,
                          const std::vector<Identity>& ids, bool team1IsCt) {
  std::map<int, int> out;
  std::vector<bool> used(ids.size(), false);
  std::set<int> present;
  for (const auto& b : bots) present.insert(b.userid);
  for (const auto& kv : prev) {
    if (!present.count(kv.first) || kv.second < 0 || static_cast<size_t>(kv.second) >= ids.size()) continue;
    if (used[static_cast<size_t>(kv.second)]) continue;
    used[static_cast<size_t>(kv.second)] = true;
    out[kv.first] = kv.second;
  }
  std::vector<Bot> sorted = bots;
  std::sort(sorted.begin(), sorted.end(), [](const Bot& a, const Bot& b) { return a.userid < b.userid; });
  for (const auto& b : sorted) {
    if (b.userid < 0 || out.count(b.userid) || (b.side != 2 && b.side != 3)) continue;
    for (size_t i = 0; i < ids.size(); ++i) {
      if (used[i] || SideOfTeam(ids[i].team, team1IsCt) != b.side) continue;
      used[i] = true;
      out[b.userid] = static_cast<int>(i);
      break;
    }
  }
  return out;
}

void WantedPerSide(const std::vector<Identity>& ids, bool team1IsCt, int* ctWanted, int* tWanted) {
  int ct = 0, t = 0;
  for (const auto& id : ids) {
    const int side = SideOfTeam(id.team, team1IsCt);
    if (side == 3) ++ct;
    else if (side == 2) ++t;
  }
  if (ctWanted) *ctWanted = ct;
  if (tWanted) *tWanted = t;
}

int BotFeeder::Next(double now, int total, int ct, int t, int ctWanted, int tWanted, int maxBots, int* quota) {
  if (quota) *quota = 0;
  // A bot we asked for is not there yet, or one is on no side yet (joining): wait for it, but not
  // forever (kicked by a cfg, the quota lowered by someone else, a bot that never picks a side).
  const bool unsettled = total < sent_ || ct + t < total;
  if (unsettled) {
    if (unsettledSince_ < 0) unsettledSince_ = now;
    if (now - unsettledSince_ < kSettleSeconds) return 0;
    unsettledSince_ = now;  // after this add, wait again
    sent_ = total;
  } else {
    unsettledSince_ = -1;
  }
  if (now - lastAdd_ < kAddInterval) return 0;
  if (total >= maxBots) return 0;
  int side = 0;
  const int ctShort = ctWanted - ct, tShort = tWanted - t;
  if (ctShort > 0 && ctShort >= tShort) side = 3;  // CT first on a tie
  else if (tShort > 0) side = 2;
  if (side == 0) return 0;
  const int q = std::max(sent_, total) + 1;
  sent_ = q;
  lastAdd_ = now;
  if (quota) *quota = q;
  return side;
}

bool BotFeeder::Trim(double now, int total, int ct, int t, int ctWanted, int tWanted, int* quota) {
  if (quota) *quota = 0;
  const int wanted = ctWanted + tWanted;
  if (wanted <= 0 || total <= wanted) return false;
  if (total < sent_ || ct + t < total) return false;  // still joining: count once they are in
  if (now - lastTrim_ < kSettleSeconds) return false;
  lastTrim_ = now;
  sent_ = wanted;
  if (quota) *quota = wanted;
  return true;
}

double ReadyDelaySeconds(int index) {
  if (index < 0) index = 0;
  return 1.5 + std::fmod(index * 0.7, 2.0);
}

std::vector<std::string> SetupCommands() {
  return {
      // A clean slate: no bots from the gamemode cfgs, and only the bots added one by one below.
      "bot_quota 0",
      "bot_kick",
      "bot_quota_mode normal",
      // CS2 kicks bots from a server without humans otherwise (a simulation has none).
      "bot_join_after_player 0",
      // Bots stay on the side they are put on.
      "mp_autoteambalance 0",
      "mp_limitteams 0",
      "mp_autokick 0",
      // Bots play: none of the debug switches that freeze them or keep them from shooting.
      "bot_stop 0",
      "bot_freeze 0",
      "bot_dont_shoot 0",
      "bot_ignore_enemies 0",
      "bot_defer_to_human_goals 0",
      "bot_defer_to_human_items 0",
  };
}

std::vector<std::string> AddBotCommands(int side, int quota) {
  return {side == 3 ? "bot_join_team CT" : "bot_join_team T", "bot_quota " + std::to_string(quota)};
}

bool WrongSides(int ct, int t, int ctWanted, int tWanted) {
  return (ct > ctWanted && t < tWanted) || (t > tWanted && ct < ctWanted);
}

std::vector<std::string> TimescaleCommands(double ts) {
  const double v = ClampTimescale(ts);
  if (v == 1.0) return {};
  return {"sv_cheats 1", "host_timescale " + TimescaleText(v)};
}

std::vector<std::string> RealTimeCommands() { return {"host_timescale 1", "sv_cheats 0"}; }

std::vector<std::string> TeardownCommands() {
  // CS2's defaults for what SetupCommands changed about bots. mp_autoteambalance / mp_limitteams /
  // mp_autokick stay as a match leaves them (live.cfg sets all three to 0 as well).
  return {"bot_quota 0", "bot_kick", "bot_join_team any", "bot_join_after_player 1"};
}

}  // namespace sim
}  // namespace readyup

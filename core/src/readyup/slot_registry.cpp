#include "readyup/slot_registry.h"

#include <chrono>
#include <mutex>
#include <unordered_set>
#include <unordered_map>
 #include <vector>

namespace readyup {
namespace {

std::mutex g_mu;
std::unordered_map<int, SlotIdentity> g_slotToId;

std::mutex g_botMu;
std::unordered_map<int, BotIdentity> g_bots;

std::mutex g_humanMu;
std::unordered_map<uint64_t, HumanIdentity> g_humans;
std::unordered_set<uint64_t> g_departed;
constexpr size_t kMaxDeparted = 256;

}  // namespace

void ObserveSlotIdentity(int slot, uint64_t steamid64, const std::string& name) {
  if (slot < 0) return;
  if (steamid64 == 0) return;
  if (name.empty()) return;

  std::lock_guard<std::mutex> lk(g_mu);
  // One key per player. CS2 logs the player slot as `<N>` (bots Rex<0>/Skullhead<1>
  // and the human Simpert<2> arrive as engine-event slots 0/1/2), so it is NOT
  // 1-based. The old extra `slot - 1` alias made every human also claim the slot
  // below theirs: with a CT bot on slot 1, Simpert<2> (T) resolved to the bot's
  // controller and was counted as CT. Never alias; a new occupant of the key
  // replaces the old one.
  auto& v = g_slotToId[slot];
  v.slot = slot;
  v.steamid64 = steamid64;
  v.name = name;
}

std::optional<SlotIdentity> GetSlotIdentity(int slot) {
  std::lock_guard<std::mutex> lk(g_mu);
  auto it = g_slotToId.find(slot);
  if (it == g_slotToId.end()) return std::nullopt;
  return it->second;
}

std::vector<SlotIdentity> ListSlotIdentities() {
  std::vector<SlotIdentity> out;
  std::lock_guard<std::mutex> lk(g_mu);
  out.reserve(g_slotToId.size());
  for (const auto& kv : g_slotToId) {
    out.push_back(kv.second);
  }
  return out;
}

void ObserveBot(int userid, const std::string& name, int team) {
  if (userid < 0) return;
  std::lock_guard<std::mutex> lk(g_botMu);
  auto& b = g_bots[userid];
  b.userid = userid;
  b.pseudo_id = DevBotIdForUserid(userid);
  if (!name.empty()) b.name = name;
  if (team >= 0) b.team = team;
}

void ForgetBot(int userid) {
  std::lock_guard<std::mutex> lk(g_botMu);
  g_bots.erase(userid);
}

void ClearBots() {
  std::lock_guard<std::mutex> lk(g_botMu);
  g_bots.clear();
}

std::vector<BotIdentity> ListBots() {
  std::vector<BotIdentity> out;
  std::lock_guard<std::mutex> lk(g_botMu);
  out.reserve(g_bots.size());
  for (const auto& kv : g_bots) out.push_back(kv.second);
  return out;
}

void ForgetSlotIdentitiesForSteam(uint64_t steamid64) {
  if (steamid64 == 0) return;
  std::lock_guard<std::mutex> lk(g_mu);
  for (auto it = g_slotToId.begin(); it != g_slotToId.end();) {
    if (it->second.steamid64 == steamid64) it = g_slotToId.erase(it);
    else ++it;
  }
}

static void ApplyTeamLocked(HumanIdentity& h, int team, TeamSource src, std::chrono::steady_clock::time_point at) {
  if (team < 0) return;
  // Latest observation wins; an older one never overwrites a newer one.
  if (h.teamSource != TeamSource::None && at < h.teamAt) return;
  h.team = team;
  h.teamSource = src;
  h.teamAt = at;
}

void ObserveHuman(int userid, uint64_t steamid64, const std::string& name, int team) {
  if (steamid64 == 0) return;
  const auto now = std::chrono::steady_clock::now();
  std::lock_guard<std::mutex> lk(g_humanMu);
  if (g_departed.count(steamid64)) return;
  auto& h = g_humans[steamid64];
  h.steamid64 = steamid64;
  if (userid >= 0) h.userid = userid;
  if (!name.empty()) h.name = name;
  ApplyTeamLocked(h, team, TeamSource::Log, now);
}

bool ObserveHumanTeamFromEvent(uint64_t steamid64, int slot, int team) {
  if (steamid64 == 0) return false;
  const auto now = std::chrono::steady_clock::now();
  std::lock_guard<std::mutex> lk(g_humanMu);
  auto it = g_humans.find(steamid64);
  // Presence comes from log lines (connect/disconnect); an event alone never
  // creates a human (it may race a disconnect).
  if (it == g_humans.end()) return false;
  if (slot >= 0) it->second.slot = slot;
  ApplyTeamLocked(it->second, team, TeamSource::Event, now);
  return true;
}

void ObserveHumanSlot(uint64_t steamid64, int slot) {
  if (steamid64 == 0 || slot < 0) return;
  std::lock_guard<std::mutex> lk(g_humanMu);
  auto it = g_humans.find(steamid64);
  if (it != g_humans.end()) it->second.slot = slot;
}

const char* TeamSourceName(TeamSource s) {
  switch (s) {
    case TeamSource::None: return "none";
    case TeamSource::Log: return "log";
    case TeamSource::Event: return "event";
  }
  return "?";
}

void ForgetHuman(uint64_t steamid64) {
  std::lock_guard<std::mutex> lk(g_humanMu);
  g_humans.erase(steamid64);
  if (steamid64 == 0) return;
  if (g_departed.size() >= kMaxDeparted) g_departed.clear();
  g_departed.insert(steamid64);
}

void ObserveHumanArrival(uint64_t steamid64) {
  std::lock_guard<std::mutex> lk(g_humanMu);
  g_departed.erase(steamid64);
}

bool IsHumanDeparted(uint64_t steamid64) {
  std::lock_guard<std::mutex> lk(g_humanMu);
  return g_departed.count(steamid64) != 0;
}

void ResetHumanTeams() {
  std::lock_guard<std::mutex> lk(g_humanMu);
  g_departed.clear();
  const auto now = std::chrono::steady_clock::now();
  for (auto& kv : g_humans) {
    kv.second.team = 0;
    kv.second.teamSource = TeamSource::None;
    kv.second.teamAt = now;
  }
}

std::vector<HumanIdentity> ListHumans() {
  std::vector<HumanIdentity> out;
  std::lock_guard<std::mutex> lk(g_humanMu);
  out.reserve(g_humans.size());
  for (const auto& kv : g_humans) out.push_back(kv.second);
  return out;
}

}  // namespace readyup

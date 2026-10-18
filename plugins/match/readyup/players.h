#pragma once

// Connected players, from the core's identity / team registry (ru_api for_each_player).
//
// Same shapes the match flow used while it read the core's slot_registry directly. The core
// keeps the registry (log lines + engine events); this is a view of it:
//   - humans: one entry per connected human (SteamID64, log userid, engine slot, name, team)
//   - bots:   one entry per bot seen in log lines (userid, name, team) with a pseudo id from the
//             dev-bot range (never a real SteamID; must never reach webhooks / the DB)
//   - "slot identities": the humans keyed by their log `<N>` (the player slot in CS2)
//
// On the game thread every call reads the core fresh; other threads get the view of the last
// frame.

#include <chrono>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace readyup {

struct SlotIdentity {
  int slot = -1;  // the log header's `<N>` (the player slot in CS2)
  uint64_t steamid64 = 0;
  std::string name;
};

std::optional<SlotIdentity> GetSlotIdentity(int slot);
std::vector<SlotIdentity> ListSlotIdentities();

struct BotIdentity {
  int userid = -1;
  uint64_t pseudo_id = 0;
  std::string name;
  int team = 0;  // 0 unassigned, 1 spec, 2 T, 3 CT
};

constexpr uint64_t kDevBotIdBase = 0xB0B0000000000000ull;
inline uint64_t DevBotIdForUserid(int userid) {
  return kDevBotIdBase | static_cast<uint64_t>(static_cast<uint32_t>(userid));
}
inline bool IsDevBotId(uint64_t id) { return (id & 0xFFFF000000000000ull) == kDevBotIdBase; }

std::vector<BotIdentity> ListBots();

enum class TeamSource { None = 0, Log, Event };
const char* TeamSourceName(TeamSource s);

struct HumanIdentity {
  int userid = -1;  // log `<N>`
  int slot = -1;    // engine player slot (from events); -1 until seen
  uint64_t steamid64 = 0;
  std::string name;
  int team = 0;  // 0 unassigned, 1 spec, 2 T, 3 CT
  TeamSource teamSource = TeamSource::None;  // not exposed by ru_api: always None
  std::chrono::steady_clock::time_point teamAt{};
};

std::vector<HumanIdentity> ListHumans();

// Refreshes the view (host.cpp calls it at the start of every frame).
void PlayersRefreshCache();
// A player connected, left or changed team: the next read on the game thread reads the engine again.
void PlayersInvalidate();

}  // namespace readyup

#pragma once

#include <chrono>
#include <cstdint>
#include <optional>
#include <string>
 #include <vector>

namespace readyup {

struct SlotIdentity {
  int slot = -1;  // the log header's `<N>` (the player slot in CS2); exact key, never aliased
  uint64_t steamid64 = 0;
  std::string name;
};

// Observes a mapping between a server-side slot/userid and Steam identity.
void ObserveSlotIdentity(int slot, uint64_t steamid64, const std::string& name);

std::optional<SlotIdentity> GetSlotIdentity(int slot);

// Lists all currently observed slot identities (best-effort, one entry per key).
std::vector<SlotIdentity> ListSlotIdentities();

// ---------------------------------------------------------------------------
// Bots (tracked for the debug-only `dev_bots_ready` setting).
//
// Bots show up in server log lines as `Name<userid><BOT><TEAM>`; they have no
// SteamID, so they never enter the slot identity map above. We track them here
// keyed by log userid and hand out synthetic ids from a reserved range that can
// never collide with a real SteamID64 (real ids have universe byte 0x01; ours
// use 0xB0B0 in the top 16 bits). These ids must never reach webhooks/DB.

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
inline bool IsDevBotId(uint64_t id) {
  return (id & 0xFFFF000000000000ull) == kDevBotIdBase;
}

// Records/updates a bot seen in a log header or a player_team event (game_events.cpp).
// team < 0 keeps the previous team, an empty name the previous name.
void ObserveBot(int userid, const std::string& name, int team);
void ForgetBot(int userid);
void ClearBots();
std::vector<BotIdentity> ListBots();

// Drops every slot identity entry that maps to this SteamID (on disconnect).
void ForgetSlotIdentitiesForSteam(uint64_t steamid64);

// ---------------------------------------------------------------------------
// Connected humans, keyed by SteamID64.
//
// Exactly one entry per connected human. Presence comes from log lines (added
// by any player header, removed on `disconnected (reason ...)`). This table is
// the single source of truth for a human's team: it is fed by log lines
// (header team / "switched from team") and by `player_team` engine events
// (resolved to a SteamID through the event's controller), and the latest
// observation wins. Engine netvars (m_iTeamNum) are only a cross-check that
// logs a debug line when it disagrees (scrim_flow.cpp), never the answer.
//
// `userid` is the log's `<N>`; `slot` is the engine player slot seen in engine
// events. They are stored separately and never used as each other's key.

enum class TeamSource { None = 0, Log, Event };
const char* TeamSourceName(TeamSource s);

struct HumanIdentity {
  int userid = -1;  // log `<N>`
  int slot = -1;    // engine player slot (from events); -1 until seen
  uint64_t steamid64 = 0;
  std::string name;
  int team = 0;  // 0 unassigned, 1 spec, 2 T, 3 CT
  TeamSource teamSource = TeamSource::None;
  std::chrono::steady_clock::time_point teamAt{};
};

// Log-derived observation. team < 0 keeps the previous team. Ignored for a departed SteamID
// (CS2 keeps logging lines for a player right after their `disconnected` line).
void ObserveHuman(int userid, uint64_t steamid64, const std::string& name, int team);
// Engine `player_team` event, already resolved to a SteamID. Returns false if
// the SteamID is not a connected human.
bool ObserveHumanTeamFromEvent(uint64_t steamid64, int slot, int team);
// Engine slot seen for this SteamID in an engine event (player_spawn etc).
void ObserveHumanSlot(uint64_t steamid64, int slot);
// Removes the human and marks the SteamID departed until ObserveHumanArrival.
void ForgetHuman(uint64_t steamid64);
// An arrival line (connected, entered the game, STEAM USERID validated, switch from
// <Unassigned>): the SteamID is no longer departed.
void ObserveHumanArrival(uint64_t steamid64);
bool IsHumanDeparted(uint64_t steamid64);
// Map load: players stay connected but have to pick a team again. Also forgets the departed set.
void ResetHumanTeams();
std::vector<HumanIdentity> ListHumans();

}  // namespace readyup

#pragma once
// Player chat to the platform: every human chat line that is not a command goes out as the fleet
// event `event.chat` (plugins/fleet/protocol/v1/messages/event.chat.json) while the server has a
// platform assignment (fleet_bridge.cpp OnLogLine). The engine-free half: the log line parser,
// the per-player rate limit and the event's `data`. ctest `match_chat_forward`.
#include "readyup/status_snapshot.h"

#include <cstddef>
#include <cstdint>
#include <deque>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace readyup::chat_forward {

constexpr size_t kMessageMaxChars = 300;

struct ChatLine {
  uint64_t steamid64 = 0;
  std::string name;
  std::string side;  // "ct" | "t" | "spec", from the header's team at the time of the line
  bool team_only = false;
  std::string message;  // raw, as logged
};

// A connected human (ListHumans()): log userid `<N>` (-1 = unknown) and SteamID64.
struct KnownHuman {
  int userid = -1;
  uint64_t steamid64 = 0;
};

// `L 10/17/2026 - 12:00:00: "Name<2><[U:1:123]><CT>" say "gg"` (or say_team). nullopt for
// anything else, bots (`<BOT>`), the console and players without a SteamID.
//
// The name and the message are player text and may hold `<..><..><..>" say "` themselves, so
// the header is not "the first ` say `": every `<userid><steamid><team>" say[_team] "` block in
// the line is a candidate, and a candidate counts only when its userid + SteamID is a connected
// human in `humans`. Exactly one must count; more than one (a forged block naming another
// connected player, in the name or the message) or none drops the line. The sender, the side and
// say vs say_team all come from that one block.
std::optional<ChatLine> ParseChatLogLine(const std::string& line, const std::vector<KnownHuman>& humans);

// A chat command (`.ready`, `!admin`): never forwarded. Leading spaces are ignored.
bool IsCommand(const std::string& message);

// At most `maxLines` per player per `windowSeconds` (sliding window).
class RateLimiter {
 public:
  RateLimiter(int maxLines = 5, double windowSeconds = 5.0) : max_(maxLines), window_(windowSeconds) {}
  // True when `steamid64` may send a line at `now` (seconds, any monotonic clock); it is counted.
  bool Allow(uint64_t steamid64, double now);
  void Clear() { seen_.clear(); }

 private:
  int max_;
  double window_;
  std::unordered_map<uint64_t, std::deque<double>> seen_;
};

// {player: {steamid64, name, team, side}, message, team_only, at}. `team`: "team1" | "team2"
// (the roster), "" = null. The message is cleaned (control bytes dropped, trimmed) and cut to
// kMessageMaxChars characters; the name to 64.
status::Json EventData(const ChatLine& c, const std::string& team, long long atUnixMs);

}  // namespace readyup::chat_forward

#include "readyup/chat_forward.h"

#include "readyup/admin_call_logic.h"
#include "readyup/steamid.h"

namespace readyup::chat_forward {

namespace {

struct Candidate {
  size_t uidLt;     // the `<` of `<userid>`: the name ends here
  size_t msgStart;  // first byte of the message
  int userid;
  uint64_t steamid64;
  std::string team;
  bool teamOnly;
};

// The `<userid><steamid><team>` block whose last `>` is at `gt` (groups hold no `<` / `>`).
bool BlockEndingAt(const std::string& line, size_t q, size_t gt, Candidate* c) {
  const size_t teamLt = line.rfind('<', gt);
  if (teamLt == std::string::npos || teamLt <= q || teamLt == 0 || line[teamLt - 1] != '>') return false;
  const size_t idLt = line.rfind('<', teamLt - 2);
  if (idLt == std::string::npos || idLt <= q || idLt == 0 || line[idLt - 1] != '>') return false;
  const size_t uidLt = line.rfind('<', idLt - 2);
  if (uidLt == std::string::npos || uidLt <= q) return false;
  const std::string team = line.substr(teamLt + 1, gt - teamLt - 1);
  const std::string id = line.substr(idLt + 1, teamLt - 1 - idLt - 1);
  const std::string uid = line.substr(uidLt + 1, idLt - 1 - uidLt - 1);
  if (team.find('>') != std::string::npos || id.find('>') != std::string::npos) return false;
  if (uid.empty() || uid.size() > 9 || uid.find_first_not_of("0123456789") != std::string::npos) return false;
  if (id == "BOT" || id == "Console") return false;
  const uint64_t sid = ParseSteamId64Loose(id);
  if (sid == 0) return false;
  c->uidLt = uidLt;
  c->userid = std::stoi(uid);
  c->steamid64 = sid;
  c->team = team;
  return true;
}

bool Connected(const Candidate& c, const std::vector<KnownHuman>& humans) {
  for (const auto& h : humans) {
    if (h.steamid64 == c.steamid64 && (h.userid < 0 || h.userid == c.userid)) return true;
  }
  return false;
}

}  // namespace

std::optional<ChatLine> ParseChatLogLine(const std::string& line, const std::vector<KnownHuman>& humans) {
  // The header opens at the line's first quote (the `L date - time: ` prefix holds none).
  const size_t q = line.find('"');
  if (q == std::string::npos) return std::nullopt;
  // The message runs to the line's last quote (it may hold quotes itself).
  const size_t msgEnd = line.rfind('"');
  static const std::string kSay = "\" say \"", kSayTeam = "\" say_team \"";
  std::vector<Candidate> hits;
  for (const std::string* kw : {&kSay, &kSayTeam}) {
    for (size_t at = line.find(*kw, q + 1); at != std::string::npos; at = line.find(*kw, at + 1)) {
      Candidate c{};
      if (at == 0 || line[at - 1] != '>' || !BlockEndingAt(line, q, at - 1, &c)) continue;
      c.msgStart = at + kw->size();
      c.teamOnly = kw == &kSayTeam;
      if (msgEnd == std::string::npos || msgEnd < c.msgStart) continue;
      if (Connected(c, humans)) hits.push_back(std::move(c));
    }
  }
  if (hits.size() != 1) return std::nullopt;
  const Candidate& h = hits.front();
  ChatLine c;
  c.steamid64 = h.steamid64;
  c.name = line.substr(q + 1, h.uidLt - (q + 1));
  c.side = h.team == "CT" ? "ct" : h.team == "TERRORIST" ? "t" : "spec";
  c.team_only = h.teamOnly;
  c.message = line.substr(h.msgStart, msgEnd - h.msgStart);
  return c;
}

bool IsCommand(const std::string& message) {
  const size_t i = message.find_first_not_of(' ');
  if (i == std::string::npos || i + 1 >= message.size()) return false;
  return message[i] == '.' || message[i] == '!';
}

bool RateLimiter::Allow(uint64_t steamid64, double now) {
  auto& q = seen_[steamid64];
  while (!q.empty() && now - q.front() >= window_) q.pop_front();
  if (static_cast<int>(q.size()) >= max_) return false;
  q.push_back(now);
  // Players who left: drop their empty windows now and then.
  if (seen_.size() > 256) {
    for (auto it = seen_.begin(); it != seen_.end();) {
      if (it->second.empty() || now - it->second.back() >= window_) it = seen_.erase(it);
      else ++it;
    }
  }
  return true;
}

status::Json EventData(const ChatLine& c, const std::string& team, long long atUnixMs) {
  status::Json player = status::Json::Object();
  player["steamid64"] = std::to_string(c.steamid64);
  player["name"] = CleanAdminCallMessage(c.name, 64);
  player["team"] = team == "team1" || team == "team2" ? status::Json(team) : status::Json();
  player["side"] = c.side;
  status::Json d = status::Json::Object();
  d["player"] = std::move(player);
  d["message"] = CleanAdminCallMessage(c.message, kMessageMaxChars);
  d["team_only"] = c.team_only;
  d["at"] = atUnixMs;
  return d;
}

}  // namespace readyup::chat_forward

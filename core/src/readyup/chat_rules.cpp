#include "readyup/chat_rules.h"

#include <cctype>

namespace readyup {

namespace {

std::string StripChatColorBytes(const std::string& s) {
  std::string out;
  out.reserve(s.size());
  for (unsigned char c : s) {
    // Skip common CS chat color control bytes (1..16).
    if (c >= 1 && c <= 16) continue;
    out.push_back(static_cast<char>(c));
  }
  return out;
}

}  // namespace

bool HasPrefixAlready(const std::string& name, const std::string& prefix) {
  const std::string want = StripChatColorBytes(prefix);
  if (want.empty()) return false;
  const std::string stripped = StripChatColorBytes(name);
  return stripped.rfind(want + " ", 0) == 0;
}

std::string ChatFirstToken(const std::string& msg) {
  std::string first = msg.substr(0, msg.find_first_of(" \t"));
  for (char& c : first) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  return first;
}

ChatDecision DecideChat(const ChatInput& in) {
  ChatDecision d;

  // `.ru` visibility is configurable (consume_ru_chat).
  if (in.msg.rfind(".ru", 0) == 0) {
    d.route = true;
    if (in.consumeRuChat) {
      d.swallow = true;
      return d;
    }
  }
  // Plugin chat commands; RU_CMD_HIDE swallows the line before the engine prints it.
  if (in.owned) {
    d.route = true;
    if (in.ownedHide) {
      d.swallow = true;
      return d;
    }
  }

  // Name prefix relay. Team chat stays with the engine: the relay reaches every player.
  if (in.prefix.empty() || in.teamOnly || in.msg.empty() || HasPrefixAlready(in.name, in.prefix)) return d;
  d.relayLine = in.prefix + " " + in.name + ": " + in.msg;
  d.swallow = true;
  return d;
}

}  // namespace readyup

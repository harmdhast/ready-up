#pragma once

#include <string>

namespace readyup {

struct ChatInput {
  std::string msg;
  bool teamOnly = false;
  std::string name;
  bool consumeRuChat = false;
  bool owned = false;  // a loaded plugin owns the first token (never ".ru")
  bool ownedHide = false;
  std::string prefix;
};

struct ChatDecision {
  bool route = false;
  bool swallow = false;
  std::string relayLine;
};

ChatDecision DecideChat(const ChatInput& in);

// First whitespace-delimited token of a chat line, lowercased.
std::string ChatFirstToken(const std::string& msg);

bool HasPrefixAlready(const std::string& name, const std::string& prefix);

}  // namespace readyup

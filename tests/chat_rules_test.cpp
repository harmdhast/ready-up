// Offline tests for core/src/readyup/chat_rules.h: the say / say_team decision shared by the chat
// hooks (.ru routing, plugin command hiding, name-prefix relay). ctest `chat_rules`.
#include "readyup/chat_rules.h"

#include <cstdio>
#include <string>

using namespace readyup;

static int g_failures = 0;

static void Check(const char* what, const ChatInput& in, bool route, bool swallow, const std::string& relay) {
  const ChatDecision d = DecideChat(in);
  if (d.route != route || d.swallow != swallow || d.relayLine != relay) {
    std::fprintf(stderr, "FAIL %s: route=%d swallow=%d relay=\"%s\"\n", what, d.route, d.swallow, d.relayLine.c_str());
    ++g_failures;
  }
}

static ChatInput In(const std::string& msg) {
  ChatInput in;
  in.msg = msg;
  in.name = "Bob";
  return in;
}

int main() {
  {
    ChatInput in = In(".ru status");
    in.consumeRuChat = true;
    Check("ru consumed", in, true, true, "");
    in.consumeRuChat = false;
    Check("ru visible", in, true, false, "");
  }
  {
    ChatInput in = In(".r");
    in.owned = in.ownedHide = true;
    Check("owned hidden", in, true, true, "");
  }
  {
    ChatInput in = In(".pause tech");
    in.owned = true;
    Check("owned shown", in, true, false, "");
  }
  {
    ChatInput in = In("hello");
    in.prefix = "[ADMIN]";
    Check("prefix relay", in, false, true, "[ADMIN] Bob: hello");
    in.teamOnly = true;
    Check("team chat untouched", in, false, false, "");
  }
  {
    ChatInput in = In("hello");
    in.name = "[ADMIN] Bob";
    in.prefix = "[ADMIN]";
    Check("already prefixed", in, false, false, "");
  }
  {
    ChatInput in = In("");
    in.prefix = "[ADMIN]";
    Check("empty msg", in, false, false, "");
  }
  {
    ChatInput in = In(".pause");
    in.owned = true;
    in.prefix = "[C]";
    Check("owned plus relay", in, true, true, "[C] Bob: .pause");
  }
  if (ChatFirstToken(".R now") != ".r") {
    std::fprintf(stderr, "FAIL ChatFirstToken\n");
    ++g_failures;
  }
  return g_failures ? 1 : 0;
}

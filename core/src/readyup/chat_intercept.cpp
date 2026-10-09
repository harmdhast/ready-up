#include "readyup/chat_intercept.h"

#include "readyup/chat.h"
#include "readyup/chat_rules.h"
#include "readyup/config.h"
#include "readyup/logging.h"
#include "readyup/plugin_loader.h"
#include "readyup/ru_router.h"
#include "readyup/slot_registry.h"

#include <cstdint>

namespace readyup {
bool HandlePlayerChat(int slot, const std::string& msg, bool teamOnly) {
  const auto ident = GetSlotIdentity(slot);
  if (!ident || ident->steamid64 == 0) return false;

  ChatInput in;
  in.msg = msg;
  in.teamOnly = teamOnly;
  in.name = ident->name;
  in.consumeRuChat = ConsumeRuChat();

  const std::string first = ChatFirstToken(msg);
  uint32_t flags = 0;
  in.owned = first.size() > 1 && first != ".ru" && plugins::ChatCommandOwned(first, &flags);
  in.ownedHide = in.owned && (flags & RU_CMD_HIDE);
  if (msg.rfind(".ru", 0) == 0) Debug("chat: saw .ru msg=\"%s\"\n", msg.c_str());
  if (in.owned) Debug("chat: plugin chat command \"%s\" (hide=%d)\n", first.c_str(), in.ownedHide ? 1 : 0);

  std::string prefix;
  if (plugins::PluginChatPrefixFor(ident->steamid64, &prefix)) in.prefix = prefix;

  const ChatDecision d = DecideChat(in);
  if (d.route) RouteChatCommand(ident->steamid64, ident->name, msg, slot);
  if (!d.relayLine.empty()) SendRawToChat(d.relayLine.c_str());
  return d.swallow;
}

}  // namespace readyup

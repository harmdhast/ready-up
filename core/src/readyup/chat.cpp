#include "readyup/chat.h"

#include "readyup/chat_colors.h"
#include "readyup/chat_highlight.h"
#include "readyup/client_print.h"
#include "readyup/command_buffer_hook.h"
#include "readyup/config.h"
#include "readyup/logging.h"

#include <dlfcn.h>

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <string_view>
#include <mutex>
#include <string>

namespace readyup {
namespace {

// NOTE: This uses a best-effort symbol lookup for a common engine helper that exists
// in many Source builds. If the symbol does not exist (or signature differs), we
// simply do nothing. This is *only* enabled when ChatDebugEnabled() is true.
using CbufAddTextFn = void (*)(const char* text);

struct ChatState {
  CbufAddTextFn cbuf_add_text = nullptr;
  bool looked_up = false;
};

ChatState& State() {
  static ChatState st;
  return st;
}

static void LookupSymbolsOnce() {
  auto& st = State();
  if (st.looked_up) return;
  st.looked_up = true;

  auto tryLookup = [&](void* handle, const char* sym) -> CbufAddTextFn {
    if (!handle) return nullptr;
    return reinterpret_cast<CbufAddTextFn>(dlsym(handle, sym));
  };

  // 1) Try the default global namespace first.
  st.cbuf_add_text = tryLookup(RTLD_DEFAULT, "Cbuf_AddText");
  if (st.cbuf_add_text) {
    if (DebugEnabled()) PrintLine("chat: found Cbuf_AddText via RTLD_DEFAULT");
    return;
  }

  // 2) Try likely modules explicitly (best-effort; RTLD_NOLOAD avoids loading anything new).
  const char* const libs[] = {
      "libengine2.so",
      "libtier0.so",
      "libhost.so",
      "libserver.so",
      "libvconcomm.so",
  };

  for (const char* lib : libs) {
    void* h = dlopen(lib, RTLD_NOW | RTLD_NOLOAD);
    if (!h) continue;
    st.cbuf_add_text = tryLookup(h, "Cbuf_AddText");
    if (st.cbuf_add_text) {
      if (DebugEnabled()) Print("chat: found Cbuf_AddText in %s\n", lib);
      return;
    }
  }

  // (There used to be a third fallback that searched IVEngineServer's vtable for a dladdr()
  // symbol name containing "ServerCommand". Stripped Valve builds export no such names, and a
  // nearest-symbol match could have picked an unrelated function; it is gone.)
  if (DebugEnabled()) PrintLine("chat: no chat injection method available (Cbuf_AddText missing).");
}

static std::string StripNewlines(std::string s) {
  s.erase(std::remove(s.begin(), s.end(), '\n'), s.end());
  s.erase(std::remove(s.begin(), s.end(), '\r'), s.end());
  return s;
}

static std::string Trim(std::string s) {
  // IMPORTANT: Do not use std::isspace() here.
  // CS2 chat color control bytes include values like 0x09 and 0x0B, which
  // std::isspace() considers whitespace and would incorrectly strip.
  auto is_trim = [](unsigned char c) { return c == ' '; };
  while (!s.empty() && is_trim(static_cast<unsigned char>(s.back()))) s.pop_back();
  size_t i = 0;
  while (i < s.size() && is_trim(static_cast<unsigned char>(s[i]))) i++;
  if (i) s.erase(0, i);
  return s;
}

static std::string SanitizeForSay(std::string s) {
  // We use `say <text>` (without quotes) to avoid in-game chat showing wrapped quotes.
  // That means we must remove command separators / newlines.
  s = StripNewlines(std::move(s));
  for (char& c : s) {
    if (c == ';' || c == '\n' || c == '\r') c = ' ';
    // Quotes can look weird in some builds; also not needed without quoting.
    if (c == '"') c = '\'';
  }
  return Trim(std::move(s));
}

static std::string HexPreview(std::string_view s, size_t maxBytes = 96) {
  static constexpr char kHex[] = "0123456789ABCDEF";
  std::string out;
  const size_t n = (s.size() > maxBytes) ? maxBytes : s.size();
  out.reserve(n * 3 + 16);
  for (size_t i = 0; i < n; ++i) {
    const unsigned char c = static_cast<unsigned char>(s[i]);
    out.push_back(kHex[(c >> 4) & 0xF]);
    out.push_back(kHex[c & 0xF]);
    if (i + 1 != n) out.push_back(' ');
  }
  if (s.size() > maxBytes) out += " ...";
  return out;
}

}  // namespace

static void SendSayPayload(const std::string& payload) {
  std::string p = SanitizeForSay(payload);
  if (p.empty()) return;

  if (DebugEnabled()) {
    Debug("chat: payload hex=%s\n", HexPreview(p).c_str());
  }

  // Prefer real chat printing (CounterStrikeSharp style) to avoid "Console:".
  if (ClientPrintAllChat(p.c_str())) {
    Debug("chat: ClientPrintAllChat ok payload=\"%s\"\n", p.c_str());
    return;
  }
  Debug("chat: ClientPrintAllChat unavailable; fallback payload=\"%s\"\n", p.c_str());
  if (DebugEnabled()) {
    static std::atomic<bool> warned{false};
    if (!warned.exchange(true)) {
      PrintLine("chat: UTIL_ClientPrintAll unavailable; falling back to `say` (will show 'Console:' in chat).");
    }
  }

  // Keep chat lines readable.
  if (p.size() > 180) {
    p.resize(180);
    p += "...";
  }

  const std::string cmd = "say " + p;

  // Prefer the command buffer hook path (works even when Cbuf_AddText is missing).
  if (EnqueueServerCommand(cmd.c_str())) {
    return;
  }

  // Fallbacks (best-effort).
  LookupSymbolsOnce();
  auto& st = State();
  const std::string cmdNl = cmd + "\n";
  if (st.cbuf_add_text) return st.cbuf_add_text(cmdNl.c_str());
}

static void SendToChatImpl(const char* msg) {
  std::string s = msg ? msg : "";
  s = Trim(StripNewlines(std::move(s)));
  if (s.empty()) return;

  // CS2 / CounterStrikeSharp-style chat color control bytes.
  const std::string coloredPrefix = ChatPrefix() + " ";
  Debug("chat: SendToChatImpl msg=\"%s\"\n", s.c_str());
  SendSayPayload(coloredPrefix + HighlightChatText(s));
}

void SendImportantToChat(const char* msg) {
  std::string s = Trim(StripNewlines(msg ? msg : ""));
  if (s.empty()) return;
  const std::string line = ChatPrefix() + " " + std::string(1, ChatColors::Gold) + s;
  for (int i = 0; i < 3; ++i) SendSayPayload(line);
}

void AnnounceToChat(const char* msg) {
  if (!ChatDebugEnabled()) return;
  SendToChatImpl(msg);
}

void SendToChat(const char* msg) {
  SendToChatImpl(msg);
}

bool SendToSlotChat(int slot, const char* msg) {
  if (slot < 0 || !msg || !*msg) return false;
  const std::string p = SanitizeForSay(ChatPrefix() + " " + HighlightChatText(Trim(StripNewlines(msg))));
  return !p.empty() && ClientPrintChat(slot, p.c_str());
}

void SendRawToChat(const char* payload) {
  SendSayPayload(payload ? payload : "");
}

}  // namespace readyup


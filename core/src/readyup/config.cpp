#include "readyup/config.h"

#include "readyup/chat_colors.h"
#include "readyup/logging.h"
#include "readyup/path.h"

#include <atomic>
#include <cstdlib>
#include <fstream>
#include <mutex>
#include <algorithm>
#include <cstdlib>
#include <string>
#include <vector>

namespace readyup {
namespace {

struct CfgState {
  std::mutex mu;
  ReadyUpCfg cfg;
  bool loaded = false;
  // Runtime chat prefix (SetChatPrefixOverride; expanded); survives ReloadCfg.
  std::string chatPrefixOverride;
};

CfgState& State() {
  static CfgState st;
  return st;
}

static std::string CfgPath() {
  const std::string dir = GetThisModuleDir();
  if (dir.empty()) return {};
  return dir + "/readyup.cfg";
}

std::string Trim(std::string s) {
  while (!s.empty() && (s.back() == '\r' || s.back() == '\n' || s.back() == ' ' || s.back() == '\t')) {
    s.pop_back();
  }
  size_t i = 0;
  while (i < s.size() && (s[i] == ' ' || s[i] == '\t')) i++;
  if (i) s.erase(0, i);
  return s;
}

std::string Unquote(std::string s) {
  if (s.size() < 2) return s;
  const char q = s.front();
  if ((q == '"' || q == '\'') && s.back() == q) {
    s.erase(s.begin());
    s.pop_back();
  }
  return s;
}

bool ParseBool(const std::string& v, bool defaultValue) {
  if (v.empty()) return defaultValue;
  const char c = v[0];
  if (c == '0' || c == 'n' || c == 'N' || c == 'f' || c == 'F') return false;
  if (c == '1' || c == 'y' || c == 'Y' || c == 't' || c == 'T') return true;
  return defaultValue;
}

int ParseInt(const std::string& v, int defaultValue) {
  if (v.empty()) return defaultValue;
  const long long x = std::strtoll(v.c_str(), nullptr, 10);
  if (x <= 0 || x > 65535) return defaultValue;
  return static_cast<int>(x);
}

std::string ExpandChatColorTokens(std::string s) {
  struct Tok {
    const char* token;
    char value;
  };
  static constexpr Tok kToks[] = {
      {"<Default>", ChatColors::Default},
      {"<White>", ChatColors::White},
      {"<DarkRed>", ChatColors::DarkRed},
      {"<Green>", ChatColors::Green},
      {"<LightYellow>", ChatColors::LightYellow},
      {"<LightBlue>", ChatColors::LightBlue},
      {"<Olive>", ChatColors::Olive},
      {"<Lime>", ChatColors::Lime},
      {"<Red>", ChatColors::Red},
      {"<LightPurple>", ChatColors::LightPurple},
      {"<Purple>", ChatColors::Purple},
      {"<Grey>", ChatColors::Grey},
      {"<Yellow>", ChatColors::Yellow},
      {"<Gold>", ChatColors::Gold},
      {"<Silver>", ChatColors::Silver},
      {"<Blue>", ChatColors::Blue},
      {"<DarkBlue>", ChatColors::DarkBlue},
      {"<BlueGrey>", ChatColors::BlueGrey},
      {"<Magenta>", ChatColors::Magenta},
      {"<LightRed>", ChatColors::LightRed},
      {"<Orange>", ChatColors::Orange},
  };

  for (const auto& t : kToks) {
    const std::string needle = t.token;
    size_t pos = 0;
    while ((pos = s.find(needle, pos)) != std::string::npos) {
      s.replace(pos, needle.size(), std::string(1, t.value));
      pos += 1;
    }
  }
  return s;
}

ReadyUpCfg DefaultCfg() {
  ReadyUpCfg c;
  // "[Ready Up]" in rainbow letters, so its lines stand out from the players'.
  static constexpr char kRainbow[] = {ChatColors::Red,   ChatColors::Orange, ChatColors::Yellow,
                                      ChatColors::Green, ChatColors::LightBlue, ChatColors::Purple};
  const std::string text = "[Ready Up]";
  size_t n = 0;
  for (char ch : text) {
    if (ch != ' ') c.chat_prefix.push_back(kRainbow[n++ % sizeof(kRainbow)]);
    c.chat_prefix.push_back(ch);
  }
  c.chat_prefix.push_back(ChatColors::Default);
  return c;
}

bool LoadCfgFromDisk(ReadyUpCfg* out, std::string* err, bool allowMissing) {
  if (!out) return false;
  *out = DefaultCfg();

  const std::string path = CfgPath();
  if (path.empty()) {
    if (err) *err = "module dir not resolved (cannot locate readyup.cfg)";
    return false;
  }

  std::ifstream f(path);
  if (!f.good()) {
    if (allowMissing) return false;
    if (err) *err = "could not open: " + path;
    return false;
  }

  std::string line;
  bool inPluginSection = false;  // [name] blocks hold plugin keys (ru_api config_get), not core keys
  while (std::getline(f, line)) {
    line = Trim(line);
    if (line.empty()) continue;
    if (line.rfind("#", 0) == 0) continue;
    if (line.rfind("//", 0) == 0) continue;
    if (line.front() == '[' && line.back() == ']') {
      inPluginSection = true;
      continue;
    }
    if (inPluginSection) continue;

    const auto eq = line.find('=');
    if (eq == std::string::npos) continue;
    std::string key = Trim(line.substr(0, eq));
    std::string val = Trim(line.substr(eq + 1));
    val = Unquote(std::move(val));

    // (Match keys in the same file are read by plugins/match.)
    if (key == "debug") out->debug = ParseBool(val, out->debug);
    else if (key == "banner") out->banner = ParseBool(val, out->banner);
    else if (key == "chat_debug") out->chat_debug = ParseBool(val, out->chat_debug);
    else if (key == "chat_prefix") out->chat_prefix = ExpandChatColorTokens(val);
    else if (key == "consume_ru_chat") out->consume_ru_chat = ParseBool(val, out->consume_ru_chat);
    else if (key == "status_http_enabled") out->status_http_enabled = ParseBool(val, out->status_http_enabled);
    else if (key == "status_http_bind") out->status_http_bind = val;
    else if (key == "status_http_port") out->status_http_port = ParseInt(val, out->status_http_port);
    else if (key == "perf_warn_ms") out->perf_warn_ms = ParseInt(val, out->perf_warn_ms);
    else if (key == "perf_gap_warn_ms") out->perf_gap_warn_ms = ParseInt(val, out->perf_gap_warn_ms);
    else if (key == "status_http_token") out->status_http_token = val;
    else if (key == "status_http_metrics") out->status_http_metrics = ParseBool(val, out->status_http_metrics);
  }
  return true;
}

void EnsureLoadedLocked(CfgState& st) {
  if (st.loaded) return;
  std::string ignored;
  ReadyUpCfg tmp;
  (void)LoadCfgFromDisk(&tmp, &ignored, /*allowMissing=*/true);
  st.cfg = std::move(tmp);
  st.loaded = true;
}

}  // namespace

ReadyUpCfg Cfg() {
  auto& st = State();
  std::lock_guard<std::mutex> lock(st.mu);
  EnsureLoadedLocked(st);
  return st.cfg;
}

bool ReloadCfg(std::string* err) {
  ReadyUpCfg tmp;
  std::string localErr;
  const bool ok = LoadCfgFromDisk(&tmp, &localErr, /*allowMissing=*/false);
  if (!ok) {
    if (err) *err = localErr;
    return false;
  }

  auto& st = State();
  std::lock_guard<std::mutex> lock(st.mu);
  st.cfg = std::move(tmp);
  st.loaded = true;
  if (err) err->clear();
  return true;
}

bool DebugEnabled() {
  const char* v = std::getenv("READYUP_DEBUG");
  if (!v || !*v) return Cfg().debug;
  return !(v[0] == '0' || v[0] == 'n' || v[0] == 'N' || v[0] == 'f' || v[0] == 'F');
}

bool BannerEnabled() {
  const char* v = std::getenv("READYUP_BANNER");
  if (!v || !*v) return Cfg().banner;
  return !(v[0] == '0' || v[0] == 'n' || v[0] == 'N' || v[0] == 'f' || v[0] == 'F');
}

bool ChatDebugEnabled() {
  // Only mirror logs to chat when debug is enabled (guard against accidental spam).
  if (!DebugEnabled()) return false;

  const char* v = std::getenv("READYUP_CHAT_DEBUG");
  if (!v || !*v) return Cfg().chat_debug;
  return !(v[0] == '0' || v[0] == 'n' || v[0] == 'N' || v[0] == 'f' || v[0] == 'F');
}

bool SetChatPrefixOverride(const std::string& tokenText) {
  if (tokenText.size() > 64) return false;
  for (unsigned char ch : tokenText) {
    if (ch < 0x20 || ch == 0x7f) return false;
  }
  auto& st = State();
  std::lock_guard<std::mutex> lock(st.mu);
  st.chatPrefixOverride = tokenText.empty() ? std::string() : ExpandChatColorTokens(tokenText);
  return true;
}

std::string ChatPrefix() {
  {
    auto& st = State();
    std::lock_guard<std::mutex> lock(st.mu);
    if (!st.chatPrefixOverride.empty()) return st.chatPrefixOverride;
  }
  auto c = Cfg();
  if (c.chat_prefix.empty()) c.chat_prefix = DefaultCfg().chat_prefix;
  return c.chat_prefix;
}

bool ConsumeRuChat() {
  return Cfg().consume_ru_chat;
}

}  // namespace readyup

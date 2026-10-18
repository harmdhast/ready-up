// Roster display names, pure logic (roster_names_rules.h).
#include "readyup/roster_names_rules.h"

namespace readyup::roster_names {

namespace {

// Length of the valid UTF-8 sequence at s[i] (1..4), or 0 when it is not one (overlong forms,
// surrogates and code points above U+10FFFF are invalid).
size_t Utf8Len(const std::string& s, size_t i) {
  const auto b = [&](size_t k) { return static_cast<unsigned char>(s[k]); };
  const unsigned char c = b(i);
  if (c < 0x80) return 1;
  size_t n = 0;
  unsigned char lo = 0x80, hi = 0xBF;
  if (c >= 0xC2 && c <= 0xDF) n = 2;
  else if (c == 0xE0) n = 3, lo = 0xA0;
  else if (c == 0xED) n = 3, hi = 0x9F;
  else if (c >= 0xE1 && c <= 0xEF) n = 3;
  else if (c == 0xF0) n = 4, lo = 0x90;
  else if (c >= 0xF1 && c <= 0xF3) n = 4;
  else if (c == 0xF4) n = 4, hi = 0x8F;
  else return 0;
  if (i + n > s.size()) return 0;
  if (b(i + 1) < lo || b(i + 1) > hi) return 0;
  for (size_t k = 2; k < n; ++k) {
    if (b(i + k) < 0x80 || b(i + k) > 0xBF) return 0;
  }
  return n;
}

}  // namespace

std::string Sanitize(const std::string& in, size_t maxBytes) {
  std::string out;
  out.reserve(in.size() < maxBytes ? in.size() : maxBytes);
  bool pendingSpace = false;
  for (size_t i = 0; i < in.size();) {
    const size_t n = Utf8Len(in, i);
    if (n == 0) {  // invalid byte: dropped
      ++i;
      continue;
    }
    if (n == 1) {
      const unsigned char c = static_cast<unsigned char>(in[i]);
      ++i;
      if (c == ' ' || c == '\t' || c == '\n' || c == '\r') {
        pendingSpace = !out.empty();
        continue;
      }
      if (c < 0x20 || c == 0x7F || c == '"' || c == '<' || c == '>') continue;
      const size_t need = (pendingSpace ? 1 : 0) + 1;
      if (out.size() + need > maxBytes) break;
      if (pendingSpace) out.push_back(' ');
      pendingSpace = false;
      out.push_back(static_cast<char>(c));
      continue;
    }
    const size_t need = (pendingSpace ? 1 : 0) + n;
    if (out.size() + need > maxBytes) break;
    if (pendingSpace) out.push_back(' ');
    pendingSpace = false;
    out.append(in, i, n);
    i += n;
  }
  return out;
}

bool Enabled(int matchRule, bool serverSetting, bool matchHasNames) {
  if (!matchHasNames) return false;
  if (matchRule >= 0) return matchRule != 0;
  return serverSetting;
}

std::optional<std::string> DesiredName(uint64_t steamid64, const std::unordered_map<uint64_t, std::string>& names) {
  if (steamid64 == 0) return std::nullopt;
  const auto it = names.find(steamid64);
  if (it == names.end()) return std::nullopt;
  std::string s = Sanitize(it->second);
  if (s.empty()) return std::nullopt;
  return s;
}

Action Decide(const std::string& current, const std::optional<std::string>& desired, PlayerState* st,
              std::string* out) {
  if (desired) {
    if (current == *desired) {
      st->written = *desired;  // already right (e.g. the Steam name is the roster name)
      return Action::None;
    }
    if (current != st->written && !current.empty()) st->original = current;
    st->written = *desired;
    *out = *desired;
    return Action::Write;
  }
  if (st->written.empty()) return Action::None;
  const bool ours = current == st->written;
  st->written.clear();
  if (!ours || st->original.empty() || st->original == current) return Action::None;
  *out = st->original;
  return Action::Restore;
}

}  // namespace readyup::roster_names

#include "readyup/log_relay.h"

#include <algorithm>
#include <cctype>
#include <cstring>

namespace readyup {
namespace {

bool IsWord(char c) { return std::isalnum(static_cast<unsigned char>(c)) != 0 || c == '_'; }

bool IsSpace(char c) { return c == ' ' || c == '\t'; }

bool TokenChar(char c) { return IsWord(c) || c == '-' || c == '=' || c == '+' || c == '/'; }

bool MatchesNoCase(std::string_view s, size_t at, const char* word, size_t n) {
  if (at + n > s.size()) return false;
  for (size_t k = 0; k < n; ++k)
    if (std::tolower(static_cast<unsigned char>(s[at + k])) != word[k]) return false;
  return true;
}

std::string RedactCvars(std::string_view s) {
  static const char* const kNames[] = {"rcon_password", "sv_password", "sv_setsteamaccount", "readyup_license_key"};
  std::string out;
  out.reserve(s.size());
  size_t i = 0;
  while (i < s.size()) {
    bool matched = false;
    if (i == 0 || !IsWord(s[i - 1])) {
      size_t start = i;
      if (s[start] == '+') ++start;
      for (const char* name : kNames) {
        const size_t n = std::strlen(name);
        if (!MatchesNoCase(s, start, name, n)) continue;
        size_t j = start + n;
        if (j < s.size() && IsWord(s[j])) continue;
        size_t v = j;
        while (v < s.size() && IsSpace(s[v])) ++v;
        if (v == j || v >= s.size()) continue;
        size_t end = v;
        if (s[v] == '"') {
          end = s.find('"', v + 1);
          end = end == std::string_view::npos ? s.size() : end + 1;
        } else {
          while (end < s.size() && !IsSpace(s[end])) ++end;
        }
        out.append(s.substr(i, j - i));
        out.append(" ***");
        i = end;
        matched = true;
        break;
      }
    }
    if (!matched) out.push_back(s[i++]);
  }
  return out;
}

std::string RedactTokens(std::string_view s) {
  std::string out;
  out.reserve(s.size());
  size_t i = 0;
  while (i < s.size()) {
    bool matched = false;
    for (const char* pfx : {"rus_", "rfk_", "rhs_", "rst_", "RUE-", "Bearer "}) {
      const size_t n = std::strlen(pfx);
      const bool bearer = pfx[0] == 'B';
      if (s.compare(i, n, pfx) != 0 || !(bearer || i == 0 || !IsWord(s[i - 1]))) continue;
      size_t j = i + n;
      while (j < s.size() && TokenChar(s[j])) ++j;
      out.append(pfx, n);
      out += "***";
      i = j;
      matched = true;
      break;
    }
    if (!matched) out.push_back(s[i++]);
  }
  return out;
}

std::string Truncate(std::string s) {
  if (s.size() <= kLogLineMaxBytes) return s;
  static const char kEllipsis[] = "\xE2\x80\xA6";
  size_t cut = kLogLineMaxBytes - 3;
  while (cut > 0 && (static_cast<unsigned char>(s[cut]) & 0xC0) == 0x80) --cut;
  s.resize(cut);
  s += kEllipsis;
  return s;
}

}  // namespace

std::string RedactLogLine(std::string_view line) { return RedactTokens(RedactCvars(line)); }

void LogRelay::Push(std::string_view line, double now) {
  std::string s = Truncate(RedactLogLine(line));
  ring_.push_back(s);
  if (ring_.size() > kLogRingLines) ring_.pop_front();
  if (!Active(now)) return;
  if (now - windowStart_ >= 1.0 || now < windowStart_) {
    windowStart_ = now;
    windowCount_ = 0;
  }
  if (windowCount_ >= kLogLinesPerSecond) {
    ++dropped_;
    return;
  }
  ++windowCount_;
  queue_.push_back(std::move(s));
}

void LogRelay::Start(double now, bool backlog) {
  if (!Active(now)) {
    queue_.clear();
    windowStart_ = now;
    windowCount_ = 0;
    dropped_ = 0;
  }
  started_ = true;
  expiresAt_ = now + kLogStreamTtlS;
  if (backlog) {
    queue_.clear();
    backlogPending_ = true;
  }
}

void LogRelay::Stop() {
  started_ = false;
  backlogPending_ = false;
  queue_.clear();
  dropped_ = 0;
}

bool LogRelay::Active(double now) const { return started_ && now < expiresAt_; }

std::optional<LogRelay::Batch> LogRelay::Take(double now) {
  if (!Active(now)) {
    Stop();
    return std::nullopt;
  }
  Batch b;
  if (backlogPending_) {
    backlogPending_ = false;
    b.backlog = true;
    b.lines.assign(ring_.begin(), ring_.end());
    return b;
  }
  if (queue_.empty() && dropped_ == 0) return std::nullopt;
  if (now - lastTake_ < kLogFlushS) return std::nullopt;
  const size_t n = std::min(queue_.size(), kLogBatchLines);
  b.lines.assign(std::make_move_iterator(queue_.begin()), std::make_move_iterator(queue_.begin() + n));
  queue_.erase(queue_.begin(), queue_.begin() + n);
  b.dropped = dropped_;
  dropped_ = 0;
  lastTake_ = now;
  return b;
}

}  // namespace readyup

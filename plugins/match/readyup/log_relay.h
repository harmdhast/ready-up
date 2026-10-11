#pragma once

// Console lines for the fleet live log (pure logic; ctest `match_log_relay`): secrets redacted,
// lines capped, the last kRingLines kept for a late viewer, and the stream cut into batches.
// A stream runs from Start() until 90 s without a renewing Start(). Start(now, true) queues the
// ring as one backlog batch, delivered by the next Take() outside the spacing and the rate cap.

#include <cstddef>
#include <deque>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace readyup {

constexpr size_t kLogRingLines = 200;
constexpr size_t kLogLineMaxBytes = 512;
constexpr size_t kLogBatchLines = 100;
constexpr size_t kLogLinesPerSecond = 100;
constexpr double kLogFlushS = 0.25;
constexpr double kLogStreamTtlS = 90.0;

// Values of rcon_password, sv_password, sv_setsteamaccount and readyup_license_key become ***;
// rus_ rfk_ rhs_ rst_ RUE- and Bearer tokens keep their prefix.
std::string RedactLogLine(std::string_view line);

class LogRelay {
 public:
  struct Batch {
    std::vector<std::string> lines;
    bool backlog = false;
    long long dropped = 0;
  };

  void Push(std::string_view line, double now);
  void Start(double now, bool backlog);
  void Stop();
  bool Active(double now) const;
  std::optional<Batch> Take(double now);

 private:
  std::deque<std::string> ring_;
  std::deque<std::string> queue_;
  bool started_ = false;
  double expiresAt_ = 0;
  bool backlogPending_ = false;
  double lastTake_ = -1e9;
  double windowStart_ = 0;
  size_t windowCount_ = 0;
  long long dropped_ = 0;
};

}  // namespace readyup

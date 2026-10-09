#pragma once

// Player headers of server log lines (`"Name<userid><steamid><team>" ...`), parsed by the core's
// logging listener (client_command_hook.cpp). Pure, unit tested (tests/log_header_test.cpp).

#include <string>

namespace readyup {

inline int TeamFromLogName(const std::string& t) {
  if (t == "CT") return 3;
  if (t == "TERRORIST") return 2;
  if (t == "Spectator") return 1;
  if (t == "Unassigned") return 0;
  return -1;
}

// Team is the third <> group of a player header: name<userid><steamid><team>.
inline int TeamFromHeader(const std::string& header) {
  size_t pos = 0;
  for (int group = 0; group < 3; ++group) {
    const size_t a = header.find('<', pos);
    if (a == std::string::npos) return -1;
    const size_t b = header.find('>', a + 1);
    if (b == std::string::npos) return -1;
    if (group == 2) return TeamFromLogName(header.substr(a + 1, b - (a + 1)));
    pos = b + 1;
  }
  return -1;
}

// Second <> group of a player header (the SteamID slot; "BOT" for bots).
inline std::string IdGroupFromHeader(const std::string& header) {
  const size_t a = header.find('<');
  if (a == std::string::npos) return {};
  const size_t b = header.find('>', a + 1);
  if (b == std::string::npos) return {};
  const size_t c = header.find('<', b + 1);
  if (c == std::string::npos) return {};
  const size_t d = header.find('>', c + 1);
  if (d == std::string::npos) return {};
  return header.substr(c + 1, d - (c + 1));
}

// Team from a "switched from team <X> to <Y>" line, or -1.
inline int TeamFromSwitchLine(const std::string& line) {
  const size_t sw = line.find("switched from team <");
  if (sw == std::string::npos) return -1;
  const size_t to = line.find(" to <", sw);
  const size_t end = to == std::string::npos ? std::string::npos : line.find('>', to + 5);
  if (end == std::string::npos) return -1;
  return TeamFromLogName(line.substr(to + 5, end - (to + 5)));
}

}  // namespace readyup

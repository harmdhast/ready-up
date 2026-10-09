#include "readyup/ready_hud_logic.h"

namespace readyup {

std::string ReadyHudDisplayName(uint64_t steamid64, const std::string& connected, const std::string& roster,
                                const std::string& observed) {
  if (!connected.empty()) return connected;
  if (!roster.empty()) return roster;
  if (!observed.empty()) return observed;
  const std::string s = std::to_string(static_cast<unsigned long long>(steamid64));
  return "#" + s.substr(s.size() > 4 ? s.size() - 4 : 0);
}

}  // namespace readyup

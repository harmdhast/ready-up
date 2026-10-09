#include "readyup/ready_hud_logic.h"

namespace readyup {

std::string HudFont(const char* color, const std::string& inner) {
  return std::string("<font color='") + color + "'>" + inner + "</font>";
}

std::string ReadyHudDisplayName(uint64_t steamid64, const std::string& connected, const std::string& roster,
                                const std::string& observed) {
  if (!connected.empty()) return connected;
  if (!roster.empty()) return roster;
  if (!observed.empty()) return observed;
  const std::string s = std::to_string(static_cast<unsigned long long>(steamid64));
  return "#" + s.substr(s.size() > 4 ? s.size() - 4 : 0);
}

std::string ReadyHudTeamLine(bool ct, int ready, int total, const std::vector<std::string>& waiting, int hidden,
                             int bots) {
  std::string s = HudFont(ct ? kCt : kT, ct ? "<b>CT</b>" : "<b>T</b>");
  const bool full = total > 0 && ready >= total;
  if (total > 0) {
    s += " " + HudFont(full ? kOk : kNo, "<b>" + std::to_string(ready) + "/" + std::to_string(total) + "</b>");
  }
  if (full) {
    s += " " + HudFont(kOk, "&#183; ready");
  } else if (!waiting.empty() || hidden > 0) {
    std::string names;
    for (const auto& n : waiting) {
      if (!names.empty()) names += ", ";
      names += n;
    }
    s += " " + HudFont(kGrey, "&#183; waiting: " + names);
    if (hidden > 0) s += " " + HudFont(kGrey, "+" + std::to_string(hidden));
  }
  if (bots > 0) s += " " + HudFont(kOk, "&#10004;" + std::to_string(bots) + (bots == 1 ? " bot" : " bots"));
  if (total == 0 && waiting.empty() && hidden == 0 && bots == 0) s += " " + HudFont(kDim, "-");
  return s;
}

}  // namespace readyup

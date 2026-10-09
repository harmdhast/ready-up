#include "readyup/ready_hud_logic.h"

namespace readyup {

std::string HudFont(const char* color, const std::string& inner) {
  return std::string("<font color='") + color + "'>" + inner + "</font>";
}

std::string HudEscape(const std::string& in, size_t maxBytes) {
  std::string s = in;
  if (s.size() > maxBytes) {
    size_t cut = maxBytes;
    while (cut > 0 && (static_cast<unsigned char>(s[cut]) & 0xC0) == 0x80) --cut;
    s.resize(cut);
    s += "..";
  }
  std::string out;
  out.reserve(s.size() + 8);
  for (char c : s) {
    switch (c) {
      case '<': out += "&lt;"; break;
      case '>': out += "&gt;"; break;
      case '&': out += "&amp;"; break;
      case '"': out += "&quot;"; break;
      case '\'': out += "&#39;"; break;
      default:
        if (static_cast<unsigned char>(c) >= 0x20) out += c;
        break;
    }
  }
  return out;
}

std::string HudHeader(const std::string& brand, const char* statusColor, const std::string& status) {
  const std::string st = HudFont(statusColor, "<b>" + status + "</b>");
  if (brand.empty()) return st;
  return HudFont(kGold, "<b>" + HudEscape(brand, 40) + "</b>") + " " + HudFont(kGrey, "&#183;") + " " + st;
}

std::string ReadyHudCountLine(int ready, int total) {
  const char* color = total > 0 && ready >= total ? kOk : kGold;
  return std::string("<font class='fontSize-l' color='") + color + "'><b>" + std::to_string(ready) + "/" +
         std::to_string(total) + " READY</b></font>";
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

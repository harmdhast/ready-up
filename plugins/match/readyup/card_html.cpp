#include "readyup/card_html.h"

#include <algorithm>

namespace readyup {
namespace {

// Palette (matches the welcome card and the ready HUD).
constexpr const char* kCt = "#5EA8FF";
constexpr const char* kT = "#FF9D3B";
constexpr const char* kGold = "#FFD23F";
constexpr const char* kGrey = "#8A8F98";
constexpr const char* kOk = "#7CD35A";
constexpr const char* kNo = "#F87171";
constexpr const char* kWhite = "#FFFFFF";

constexpr size_t kMaxTeamBytes = 24;
constexpr size_t kMaxNameBytes = 32;
constexpr size_t kMaxMessageBytes = 120;

void Font(std::string& h, const char* size, const char* color, const std::string& body) {
  h += "<font class='fontSize-";
  h += size;
  h += "' color='";
  h += color;
  h += "'>";
  h += body;
  h += "</font>";
}

std::string TeamPart(const std::string& name, int side) {
  std::string h;
  const char* color = side == 3 ? kCt : side == 2 ? kT : kWhite;
  std::string body = CardHtmlEscape(name, kMaxTeamBytes);
  if (side == 3) body += " (CT)";
  else if (side == 2) body += " (T)";
  Font(h, "m", color, body);
  return h;
}

}  // namespace

std::string CardHtmlEscape(const std::string& in, size_t maxBytes) {
  std::string s = in;
  if (s.size() > maxBytes) {
    size_t cut = maxBytes;
    // Don't split a UTF-8 sequence.
    while (cut > 0 && (static_cast<unsigned char>(s[cut]) & 0xC0) == 0x80) --cut;
    s.resize(cut);
    s += "...";
  }
  std::string out;
  out.reserve(s.size() + 16);
  for (char c : s) {
    switch (c) {
      case '<': out += "&lt;"; break;
      case '>': out += "&gt;"; break;
      case '&': out += "&amp;"; break;
      case '"': out += "&quot;"; break;
      case '\'': out += "&#39;"; break;
      default:
        if (static_cast<unsigned char>(c) >= 0x20 && c != 0x7F) out += c;  // drop control bytes (chat colors)
        break;
    }
  }
  return out;
}

std::string GoLiveCommandsLine(const GoLiveCardInfo& info, bool html) {
  const char* b = html ? "<b>" : "";
  const char* e = html ? "</b>" : "";
  const char* dot = html ? " &#183; " : " | ";
  std::string out;
  // What it does first, then the command: ".up resume" read like a command with an argument.
  auto add = [&](const char* what, const char* cmd) {
    if (!out.empty()) out += dot;
    out += std::string(what) + ": " + b + cmd + e;
  };
  if (info.pauses) {
    add("Pause", ".p");
    add("Unpause", ".up");
    add("Timeout", ".tac");
  }
  if (info.adminCall) add("Call an admin", ".admin");
  return out;
}

std::string GoLiveCardHtml(const GoLiveCardInfo& info) {
  std::string h;
  h.reserve(800);
  Font(h, "xl", kOk, "<b>LIVE</b>");
  Font(h, "l", kGold, " &#183; GO GO GO");
  h += "<br>";
  if (!info.team1.empty() || !info.team2.empty()) {
    const int side2 = info.team1Side == 3 ? 2 : info.team1Side == 2 ? 3 : 0;
    h += TeamPart(info.team1.empty() ? std::string("Team 1") : info.team1, info.team1Side);
    Font(h, "m", kGrey, " vs ");
    h += TeamPart(info.team2.empty() ? std::string("Team 2") : info.team2, side2);
    h += "<br>";
  }
  // One line, short forms only (the long ones still work): fewer words to read in a freeze time.
  const std::string cmds = GoLiveCommandsLine(info, /*html=*/true);
  if (!cmds.empty()) {
    Font(h, "sm", kWhite, cmds);
    h += "<br>";
  }
  Font(h, "s", kGrey, "good luck, have fun");
  return h;
}

namespace {

// "de_mirage" -> "Mirage", "cs_office" -> "Office", "workshop/123/de_x" -> "X".
std::string PrettyMap(const std::string& map) {
  std::string m = map;
  const size_t slash = m.find_last_of('/');
  if (slash != std::string::npos) m = m.substr(slash + 1);
  for (const char* p : {"de_", "cs_", "ar_", "dz_"}) {
    if (m.rfind(p, 0) == 0) {
      m = m.substr(3);
      break;
    }
  }
  for (char& c : m) {
    if (c == '_') c = ' ';
  }
  if (!m.empty() && m[0] >= 'a' && m[0] <= 'z') m[0] = static_cast<char>(m[0] - 'a' + 'A');
  return m;
}

std::string ResultTeamName(const ResultCardInfo& info, const std::string& which) {
  const std::string& name = which == "team1" ? info.team1 : info.team2;
  return CardHtmlEscape(name.empty() ? (which == "team1" ? "Team 1" : "Team 2") : name, kMaxTeamBytes);
}

std::string ScoreLine(const ResultCardInfo& info) {
  return std::to_string(info.team1Score) + " - " + std::to_string(info.team2Score);
}

std::string Matchup(const ResultCardInfo& info) {
  return ResultTeamName(info, "team1") + " vs " + ResultTeamName(info, "team2");
}

}  // namespace

// "won on damage: 2,345 - 2,101" (tied after the overtimes).
static std::string DamageLine(const ResultCardInfo& info) {
  return "won on damage: " + std::to_string(info.team1Damage) + " - " + std::to_string(info.team2Damage);
}

std::string MapResultCardHtml(const ResultCardInfo& info) {
  std::string h;
  h.reserve(512);
  std::string head = "MAP " + std::to_string(std::max(1, info.mapNumber));
  if (!info.mapName.empty()) head += " &#183; " + CardHtmlEscape(PrettyMap(info.mapName), kMaxTeamBytes);
  Font(h, "m", kGrey, head);
  h += "<br>";
  if (info.winner == "team1" || info.winner == "team2") {
    Font(h, "xl", kGold, "<b>" + ResultTeamName(info, info.winner) + "</b>");
    Font(h, "l", kWhite, " wins the map");
  } else {
    Font(h, "xl", kGold, "<b>MAP DRAWN</b>");
  }
  h += "<br>";
  Font(h, "l", kWhite, "<b>" + ScoreLine(info) + "</b>");
  h += "<br>";
  if (info.decidedByDamage) {
    Font(h, "sm", kGold, DamageLine(info));
    h += "<br>";
  }
  Font(h, "s", kGrey, Matchup(info));
  if (!info.nextMap.empty()) {
    h += "<br>";
    Font(h, "sm", kOk, "Next: " + CardHtmlEscape(PrettyMap(info.nextMap), kMaxTeamBytes));
  }
  return h;
}

std::string SeriesResultCardHtml(const ResultCardInfo& info) {
  std::string h;
  h.reserve(512);
  if (info.winner == "team1" || info.winner == "team2") {
    Font(h, "xl", kGold, "<b>" + ResultTeamName(info, info.winner) + "</b>");
    h += "<br>";
    Font(h, "l", kOk, "<b>WINS THE SERIES</b>");
  } else {
    Font(h, "xl", kGold, "<b>SERIES DRAWN</b>");
  }
  h += "<br>";
  Font(h, "l", kWhite, info.forfeit ? std::string("by forfeit") : "<b>" + ScoreLine(info) + "</b>");
  h += "<br>";
  Font(h, "s", kGrey, Matchup(info));
  if (info.decidedByDamage) {
    h += "<br>";
    Font(h, "sm", kGold, "Last map " + DamageLine(info));
  }
  h += "<br>";
  // Players looked for what to do next in the game (a live test, 2026-10-15): the platform's
  // match page has it (the next match, the bracket, matchmaking).
  Font(h, "sm", kOk, "Go back to the platform to view your next match");
  return h;
}

std::string AdminCallCardHtml(const std::string& name, const std::string& teamLabel, const std::string& message) {
  std::string h;
  h.reserve(512);
  Font(h, "l", kNo, "<b>ADMIN CALLED</b>");
  h += "<br>";
  std::string who = CardHtmlEscape(name.empty() ? std::string("a player") : name, kMaxNameBytes);
  if (!teamLabel.empty()) who += " (" + CardHtmlEscape(teamLabel, kMaxTeamBytes + 8) + ")";
  Font(h, "m", kWhite, who + " needs an admin");
  if (!message.empty()) {
    h += "<br>";
    Font(h, "sm", kGold, CardHtmlEscape(message, kMaxMessageBytes));
  }
  return h;
}

}  // namespace readyup

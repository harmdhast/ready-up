#pragma once

// One-off center cards of the match plugin, engine-free (ctest `match_live_cards`):
// - the go-live card (golive_card.h): "LIVE · GO GO GO", the teams and their sides, the
//   commands that work now,
// - the admin-call card (admin_call.h) shown to in-game admins.
// What renders in the panel: docs/HUD.md. Both stay well under 1 KB.

#include <cstddef>
#include <string>

namespace readyup {

// HTML-escapes `in` for the center panel: < > & " ' become entities, control bytes (chat colors)
// are dropped. Longer than maxBytes: cut there (never inside a UTF-8 sequence) and "..." added.
std::string CardHtmlEscape(const std::string& in, size_t maxBytes);

struct GoLiveCardInfo {
  std::string team1, team2;  // team names; both empty (scrim): no matchup line
  int team1Side = 0;         // 3 = CT, 2 = T, 0 = unknown (no side shown)
  bool pauses = false;       // .p / .pause / .tech, .up / .unpause, .tac work (a match is loaded)
  bool adminCall = true;     // .admin
};
// The go-live commands, short forms only: "Pause: .p · Unpause: .up · Timeout: .tac · Call an admin: .admin"
// (html: bold commands and middle dots; plain: for chat). Empty when there is nothing to show.
std::string GoLiveCommandsLine(const GoLiveCardInfo& info, bool html);
std::string GoLiveCardHtml(const GoLiveCardInfo& info);

// The result cards (result_card.h), shown to everyone at the end of a map and of the series.
struct ResultCardInfo {
  std::string team1, team2;      // names; empty: "Team 1" / "Team 2"
  std::string winner;            // "team1" | "team2" | "" (a draw)
  int team1Score = 0, team2Score = 0;  // rounds (map card) or maps (series card)
  int mapNumber = 0;             // map card: 1-based
  std::string mapName;           // map card: "de_mirage" -> "Mirage"
  std::string nextMap;           // map card: the next map, if any
  bool forfeit = false;          // series card: won by forfeit
  bool decidedByDamage = false;  // the (last) map was tied after its overtimes and won on damage
  int team1Damage = 0, team2Damage = 0;
};
// "MAP 2 · MIRAGE" / "<winner> wins the map" / "7 - 5" / "Next: Inferno".
std::string MapResultCardHtml(const ResultCardInfo& info);
// "<winner> WINS THE SERIES" / "2 - 1" (or "by forfeit"), or "SERIES DRAWN".
std::string SeriesResultCardHtml(const ResultCardInfo& info);

// "<name> (<teamLabel>) needs an admin" + the message (if any), for admins.
std::string AdminCallCardHtml(const std::string& name, const std::string& teamLabel, const std::string& message);

}  // namespace readyup

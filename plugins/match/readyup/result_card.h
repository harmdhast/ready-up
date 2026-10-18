#pragma once

#include <string>

// Result cards: at the end of a map everyone gets a center card with the map's winner and score
// (and the next map), and at the end of the series one with the series winner and score, until
// the server moves on (the next map, or players are disconnected). Chat still gets its line; the
// card is what players read on screen (they missed it in chat). HTML: card_html.h. Like the
// go-live card, the same HTML is re-sent every hud_resend_ms (the panel only stays steady when it
// is re-sent) at RU_HTML_PRIO_NOTICE; an `.admin` call card for an admin wins.

namespace readyup {

// Show `html` to everyone for `seconds` (replaces a card already up). Thread-safe.
void ResultCardShow(const std::string& html, int seconds, const char* what);

// Stop the card (a new map went live, the match was reset). Thread-safe.
void ResultCardClear();

// Game thread, every frame: send the card while it is up.
void ResultCardTick();

}  // namespace readyup

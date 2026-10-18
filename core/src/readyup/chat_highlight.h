#pragma once

#include <string>

namespace readyup {

// Colors the parts of a chat line a player looks for: `.commands` in green, numbers (scores,
// seconds, rounds) in gold. A line that already carries color bytes is left as it is, and a number
// inside a word (de_dust2, 2v2, MR12) stays plain.
std::string HighlightChatText(const std::string& text);

}  // namespace readyup

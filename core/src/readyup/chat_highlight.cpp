#include "readyup/chat_highlight.h"

#include "readyup/chat_colors.h"

#include <cctype>

namespace readyup {

std::string HighlightChatText(const std::string& text) {
  for (unsigned char c : text) {
    if (c < 0x20) return text;  // colored already: the sender chose
  }
  const std::string green(1, ChatColors::Green), gold(1, ChatColors::Gold), plain(1, ChatColors::Default);
  auto isWordChar = [](unsigned char c) { return std::isalnum(c) || c == '_'; };
  std::string out;
  out.reserve(text.size() + 16);
  size_t i = 0;
  while (i < text.size()) {
    const unsigned char c = static_cast<unsigned char>(text[i]);
    const bool atWordStart = i == 0 || !isWordChar(static_cast<unsigned char>(text[i - 1]));
    // .ready, .pause, .tech: a chat command (a dot at a word start, then letters).
    if (c == '.' && atWordStart && i + 1 < text.size() && std::isalpha(static_cast<unsigned char>(text[i + 1]))) {
      size_t j = i + 1;
      while (j < text.size() && isWordChar(static_cast<unsigned char>(text[j]))) ++j;
      out += green + text.substr(i, j - i) + plain;
      i = j;
      continue;
    }
    // 13, 240s, 13-9: numbers on their own (not inside a word such as "de_dust2" or "MR12").
    if (std::isdigit(c) && atWordStart) {
      size_t j = i;
      while (j < text.size() && (std::isdigit(static_cast<unsigned char>(text[j])) ||
                                 ((text[j] == '-' || text[j] == ':' || text[j] == '.') && j + 1 < text.size() &&
                                  std::isdigit(static_cast<unsigned char>(text[j + 1])))))
        ++j;
      if (j < text.size() && isWordChar(static_cast<unsigned char>(text[j])) &&
          !(text[j] == 's' && (j + 1 == text.size() || !isWordChar(static_cast<unsigned char>(text[j + 1]))))) {
        out += text.substr(i, j - i);  // part of a word (e.g. 2v2): left plain
      } else {
        if (j < text.size() && text[j] == 's') ++j;  // 240s
        out += gold + text.substr(i, j - i) + plain;
      }
      i = j;
      continue;
    }
    out.push_back(static_cast<char>(c));
    ++i;
  }
  return out;
}

}  // namespace readyup

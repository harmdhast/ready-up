#include "readyup/stay_awake.h"

#include <cctype>
#include <cstdlib>

namespace readyup {

namespace {

std::string Trim(const std::string& s) {
  size_t b = 0, e = s.size();
  while (b < e && std::isspace(static_cast<unsigned char>(s[b]))) ++b;
  while (e > b && std::isspace(static_cast<unsigned char>(s[e - 1]))) --e;
  return s.substr(b, e - b);
}

bool CommandTurnsHibernateOn(const std::string& raw) {
  const std::string cmd = Trim(raw);
  static const std::string kName = "sv_hibernate_when_empty";
  if (cmd.compare(0, kName.size(), kName) != 0) return false;
  if (cmd.size() == kName.size() || !std::isspace(static_cast<unsigned char>(cmd[kName.size()]))) return false;
  std::string value = Trim(cmd.substr(kName.size()));
  if (value.size() >= 2 && value.front() == '"' && value.back() == '"') value = Trim(value.substr(1, value.size() - 2));
  if (value.empty()) return false;
  char* end = nullptr;
  const double v = std::strtod(value.c_str(), &end);
  if (end == value.c_str()) return false;
  return v != 0.0;
}

}  // namespace

bool TextTurnsHibernateOn(const std::string& text) {
  std::string cur;
  bool inQuotes = false;
  for (size_t i = 0; i <= text.size(); ++i) {
    const char c = i < text.size() ? text[i] : '\n';
    if (c == '"') inQuotes = !inQuotes;
    const bool lineEnd = c == '\n' || c == '\r';
    if (lineEnd || (c == ';' && !inQuotes)) {
      // `//` starts a comment in cfg text.
      const size_t comment = cur.find("//");
      if (CommandTurnsHibernateOn(comment == std::string::npos ? cur : cur.substr(0, comment))) return true;
      cur.clear();
      if (lineEnd) inQuotes = false;
      continue;
    }
    cur.push_back(c);
  }
  return false;
}

}  // namespace readyup

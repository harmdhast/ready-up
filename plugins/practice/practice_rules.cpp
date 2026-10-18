#include "practice_rules.h"

#include <cctype>
#include <set>

namespace practice {
namespace {
std::string Lower(std::string s) {
  for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  return s;
}
std::string Trim(const std::string& s) {
  size_t b = 0, e = s.size();
  while (b < e && std::isspace(static_cast<unsigned char>(s[b]))) ++b;
  while (e > b && std::isspace(static_cast<unsigned char>(s[e - 1]))) --e;
  return s.substr(b, e - b);
}
}  // namespace

bool IsToolCommand(const std::string& cmd) {
  static const std::set<std::string> k = {
      ".rethrow", ".rt", ".throw", ".savepos", ".loadpos", ".back", ".clear", ".noflash", ".god", ".spawn", ".ctspawn",
      ".tspawn",
      // ME extras: grenade history
      ".last", ".lastindex", ".throwidx", ".throwindex", ".delay", ".throwsmoke", ".rethrowsmoke", ".throwflash",
      ".rethrowflash", ".throwgrenade", ".rethrowgrenade", ".thrownade", ".rethrownade", ".throwmolotov",
      ".rethrowmolotov", ".throwdecoy", ".rethrowdecoy",
      // toggles
      ".impacts", ".traj", ".pip", ".solid", ".break", ".timer",
      // spawns
      ".bestspawn", ".worstspawn", ".bestctspawn", ".worstctspawn", ".besttspawn", ".worsttspawn", ".showspawns",
      ".hidespawns",
      // everyone else to the spectators (ru_api 1.13)
      ".fas", ".watchme",
      // lineup library
      ".savenade", ".sn", ".loadnade", ".ln", ".listnades", ".lin", ".deletenade", ".delnade", ".dn", ".importnade",
      ".in"};
  return k.count(Lower(cmd)) != 0;
}

bool IsBotCommand(const std::string& cmd) {
  static const std::set<std::string> k = {".bot", ".cbot", ".crouchbot", ".boost", ".crouchboost", ".nobots"};
  return k.count(Lower(cmd)) != 0;
}

bool ToolsAllowed(bool practiceActive, const std::string& ruleset) {
  return practiceActive && Lower(Trim(ruleset)) != "valve";
}

bool MatchBlocksPractice(const std::string& ruMode) {
  return ruMode == "match_warmup" || ruMode == "match_knife" || ruMode == "match_live" || ruMode == "knife" ||
         ruMode == "postgame";
}

bool ShouldAutoEnter(bool always, bool practiceActive, const std::string& ruMode) {
  return always && !practiceActive && !MatchBlocksPractice(ruMode);
}

bool ParseBool(const std::string& text, bool def) {
  const std::string t = Lower(Trim(text));
  if (t == "1" || t == "true" || t == "yes" || t == "on") return true;
  if (t == "0" || t == "false" || t == "no" || t == "off") return false;
  return def;
}

SlotEngineState SlotStateFrom(bool entitySystemReady, bool hasController, bool connectedKnown, int connectedState) {
  if (!entitySystemReady) return SlotEngineState::kUnknown;
  if (!hasController) return SlotEngineState::kEmpty;
  if (!connectedKnown) return SlotEngineState::kConnected;
  return connectedState == 0 ? SlotEngineState::kConnected : SlotEngineState::kGone;
}

bool SlotEntryUsable(SlotEngineState state) {
  return state == SlotEngineState::kUnknown || state == SlotEngineState::kConnected;
}

const char* HelpLine() {
  return "Ready Up practice: .bot .cbot .nobots | .savepos/.loadpos [name] .back [N] .last | .spawn/.ctspawn/.tspawn N "
         ".bestspawn .worstspawn | .rethrow .delay .clear .noflash .god .solid .impacts .traj .break .timer | "
         ".savenade .loadnade .listnades | .dryrun | .prac to leave";
}

bool ValidHomeCvar(const std::string& line) {
  if (line.size() < 6 || line.size() > 96) return false;
  if (line.compare(0, 3, "sv_") != 0 && line.compare(0, 3, "mp_") != 0) return false;
  const size_t sp = line.find(' ');
  if (sp == std::string::npos || sp < 4) return false;
  for (size_t i = 0; i < sp; ++i) {
    const char c = line[i];
    if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_')) return false;
  }
  std::string v = line.substr(sp + 1);
  if (v.empty()) return false;
  size_t i = v[0] == '-' ? 1 : 0;
  bool digits = false, dot = false;
  for (; i < v.size(); ++i) {
    if (v[i] >= '0' && v[i] <= '9') {
      digits = true;
    } else if (v[i] == '.' && !dot) {
      dot = true;
    } else {
      return false;
    }
  }
  return digits;
}

}  // namespace practice

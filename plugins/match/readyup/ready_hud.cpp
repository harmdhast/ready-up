#include "readyup/ready_hud.h"
#include "readyup/ready_hud_logic.h"

#include "readyup/plugin_api.h"

#include "readyup/admin_call.h"
#include "readyup/engine.h"
#include "readyup/config.h"
#include "readyup/logging.h"
#include "readyup/match_events.h"
#include "readyup/match_features.h"
#include "readyup/match_rules.h"
#include "readyup/match_state.h"
#include "readyup/modes.h"
#include "readyup/player_registry.h"
#include "readyup/scrim_flow.h"
#include "readyup/players.h"
#include "readyup/server_settings.h"
#include "readyup/webhook.h"
#include "readyup/welcome.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace readyup {
namespace {

using Clock = std::chrono::steady_clock;

constexpr int kMaxNamesPerSide = 6;
constexpr size_t kMaxNameBytes = 12;
// Names in one waiting list, so a side line fits the panel (the rest show as "+N").
constexpr size_t kMaxWaitingBytes = 40;

struct Entry {
  uint64_t sid = 0;
  std::string name;
  int side = 0;  // 2 = T, 3 = CT
  bool ready = false;
  bool connected = true;
};

struct Board {
  std::string title;      // e.g. "scrim warmup"
  std::string countdown;  // e.g. "knife in 3" (empty when none)
  std::vector<Entry> entries;
  int botsCt = 0;
  int botsT = 0;
  int ready = 0;
  int total = 0;
  int sideReady[2] = {0, 0};  // [0] CT, [1] T
  int sideTotal[2] = {0, 0};
  bool tallied = false;  // ready/total set per team (match); else counted from entries (scrim)
  bool scrim = false;
  std::string note;  // e.g. "need players on both CT and T" (empty when none)
};

struct Sent {
  std::string html;
  Clock::time_point at{};
};

std::unordered_map<int, Sent> g_sent;  // GameFrame thread only
Clock::time_point g_lastRun{};
std::atomic<bool> g_showing{false};

// `.ru hud test <n>` requests: steamid64 -> (variant, shown until).
constexpr auto kTestShowFor = std::chrono::seconds(10);
std::mutex g_testMu;
std::unordered_map<uint64_t, std::pair<int, Clock::time_point>> g_tests;

// Public test images for `.ru hud test` (all checked to return 200 with curl).
constexpr const char* kTestSvg = "https://raw.githubusercontent.com/Auto-Tournament/auto-tournament/main/client/public/icon.svg";
constexpr const char* kTestPng = "https://raw.githubusercontent.com/Auto-Tournament/auto-tournament/main/client/public/icon-192.png";
constexpr const char* kTestPngWiki =
    "https://upload.wikimedia.org/wikipedia/commons/thumb/4/47/PNG_transparency_demonstration_1.png/"
    "250px-PNG_transparency_demonstration_1.png";

static std::string Esc(const std::string& in, size_t maxBytes) {
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

static std::string Font(const char* color, const std::string& inner) { return HudFont(color, inner); }

// A URL that can sit inside a single-quoted HTML attribute (http/https only).
static std::string AttrSafeUrl(const std::string& url) {
  if (url.empty() || url.size() > 300) return {};
  if (url.rfind("https://", 0) != 0 && url.rfind("http://", 0) != 0) return {};
  for (unsigned char c : url) {
    if (c <= 0x20 || c == '\'' || c == '"' || c == '<' || c == '>' || c == '\\' || c >= 0x7F) return {};
  }
  return url;
}

static std::string Img(const std::string& url, int height) {
  return "<img src='" + url + "' width='" + std::to_string(height) + "' height='" + std::to_string(height) + "'>";
}

static std::string NameOf(uint64_t sid, const std::unordered_map<uint64_t, std::string>& names,
                          const std::unordered_map<uint64_t, std::string>* rosterNames = nullptr) {
  std::string connected, roster, observed;
  if (auto it = names.find(sid); it != names.end()) connected = it->second;
  if (rosterNames) {
    if (auto it = rosterNames->find(sid); it != rosterNames->end()) roster = it->second;
  }
  if (connected.empty() && roster.empty()) {
    for (const auto& p : ListObservedPlayers()) {
      if (p.steamid64 == sid && !p.name.empty()) observed = p.name;
    }
  }
  return ReadyHudDisplayName(sid, connected, roster, observed);
}

static Board BuildBoard(ReadyUpMode mode, const std::optional<WebhookMatchContext>& ctx,
                        const std::vector<HumanIdentity>& humans) {
  Board b;
  std::unordered_map<uint64_t, std::string> names;
  for (const auto& h : humans) names[h.steamid64] = h.name;

  if (mode == ReadyUpMode::ScrimWarmup || !ctx) {
    b.scrim = true;
    b.title = "scrim warmup";
    const auto roster = BuildScrimRoster();
    for (const auto& kv : roster.teamNum) {
      Entry e;
      e.sid = kv.first;
      e.side = kv.second;
      e.name = NameOf(kv.first, names);
      e.ready = IsReady(kv.first);
      b.entries.push_back(std::move(e));
    }
    for (const auto& kv : roster.devBots) {
      if (kv.second == 3) b.botsCt++;
      else if (kv.second == 2) b.botsT++;
    }
    if (!CountScrimRoster(roster).bothSides) {
      b.note = DevBotsReadyEnabled() ? "need players (or bots) on both CT and T" : "need players on both CT and T";
    }
    const int cd = ScrimCountdownSecondsLeft();
    if (cd >= 0) b.countdown = std::string(settings::Bool("knife_enabled_default") ? "knife in " : "live in ") + std::to_string(cd);
  } else {
    b.scrim = (ctx->slug == "scrim");
    b.title = RecoveryGateEnabled() ? "recovered match" : (b.scrim ? "scrim" : "match warmup");
    const auto ms = MatchStateGet();
    const int mapNum = ms.map_number <= 0 ? 1 : ms.map_number;
    bool team1Ct = true;
    if (mapNum >= 1 && static_cast<size_t>(mapNum) <= ctx->map_sides.size() &&
        ctx->map_sides[static_cast<size_t>(mapNum - 1)] == "team2_ct") {
      team1Ct = false;
    }
    std::unordered_set<uint64_t> connected;
    for (const auto& h : humans) connected.insert(h.steamid64);
    int teamRoster[2] = {0, 0}, teamReady[2] = {0, 0};
    for (const auto& kv : ctx->roster_team) {
      if (kv.first == 0) continue;
      Entry e;
      e.sid = kv.first;
      const bool t1 = (kv.second == WebhookTeam::Team1);
      e.side = (t1 == team1Ct) ? 3 : 2;
      e.name = NameOf(kv.first, names, &ctx->roster_names);
      e.ready = IsReady(kv.first);
      e.connected = connected.count(kv.first) != 0;
      ++teamRoster[t1 ? 0 : 1];
      if (e.ready) ++teamReady[t1 ? 0 : 1];
      b.entries.push_back(std::move(e));
    }
    // Substitutes never raise the total (match_rules.h TeamReadyTally): 5 + 1 sub reads x/5.
    for (int t = 0; t < 2; ++t) {
      const ReadyTally tally = TeamReadyTally(teamRoster[t], teamReady[t], ctx->players_per_team);
      b.ready += tally.ready;
      b.total += tally.total;
      const int sideIdx = ((t == 0) == team1Ct) ? 0 : 1;
      b.sideReady[sideIdx] = tally.ready;
      b.sideTotal[sideIdx] = tally.total;
    }
    b.tallied = true;
    if (DevBotsReadyEnabled()) {
      for (const auto& bot : ListBots()) {
        if (bot.team == 3) b.botsCt++;
        else if (bot.team == 2) b.botsT++;
      }
    }
  }
  std::sort(b.entries.begin(), b.entries.end(),
            [](const Entry& x, const Entry& y) { return x.name < y.name; });
  if (!b.tallied) {
    for (const auto& e : b.entries) {
      b.total++;
      if (e.ready) b.ready++;
      const int sideIdx = e.side == 3 ? 0 : 1;
      b.sideTotal[sideIdx]++;
      if (e.ready) b.sideReady[sideIdx]++;
    }
  }
  return b;
}

// One side's line: the team count and the players still not ready, connected ones first.
static std::string SideLine(const Board& b, int side, uint64_t viewer) {
  std::vector<const Entry*> list;
  for (const auto& e : b.entries) {
    if (e.side == side && !e.ready) list.push_back(&e);
  }
  std::stable_sort(list.begin(), list.end(), [](const Entry* x, const Entry* y) { return x->connected > y->connected; });

  std::vector<std::string> waiting;
  int hidden = 0;
  size_t used = 0;
  for (const Entry* e : list) {
    const size_t len = std::min(e->name.size(), kMaxNameBytes) + (waiting.empty() ? 0 : 2);
    // Keep the viewer visible even past the caps.
    if ((static_cast<int>(waiting.size()) >= kMaxNamesPerSide || used + len > kMaxWaitingBytes) && e->sid != viewer) {
      hidden++;
      continue;
    }
    used += len;
    const std::string nm = Esc(e->name, kMaxNameBytes);
    waiting.push_back(e->sid == viewer ? "<b>" + nm + "</b>" : nm);
  }
  const int idx = side == 3 ? 0 : 1;
  return ReadyHudTeamLine(side == 3, b.sideReady[idx], b.sideTotal[idx], waiting, hidden, side == 3 ? b.botsCt : b.botsT);
}

static std::string ReadyHtml(const Board& b, uint64_t viewer, const std::string& footer) {
  std::string h;
  h.reserve(768);
  const std::string brand = HudBrandHtml(/*imgHeight=*/20, "");
  if (!brand.empty()) h += brand + " " + Font(kGrey, "&#183; " + b.title);
  else h += Font(kGrey, b.title);
  if (!b.countdown.empty()) h += " " + Font(kGold, "&#183; <b>" + b.countdown + "</b>");
  h += "<br>";
  h += Font(b.total > 0 && b.ready == b.total ? kOk : kGold,
            "<b>" + std::to_string(b.ready) + "/" + std::to_string(b.total) + " ready</b>");
  h += "<br><font class='fontSize-sm'>" + SideLine(b, 3, viewer) + "</font><br><font class='fontSize-sm'>" +
       SideLine(b, 2, viewer) + "</font><br>";

  const Entry* me = nullptr;
  for (const auto& e : b.entries) {
    if (e.sid == viewer) me = &e;
  }
  if (!me) {
    h += Font(kGrey, b.scrim ? "join CT or T to play" : "spectating");
  } else if (me->ready) {
    h += Font(kOk, "you are ready &#183; <b>.ur</b> to cancel");
  } else {
    h += Font(kNo, "type <b>.r</b> in chat when ready");
  }
  if (!b.note.empty()) h += "<br>" + Font(kGrey, b.note);
  if (!footer.empty()) h += "<br>" + footer;
  return h;
}

static std::string KnifeHtml(const KnifeHudInfo& k, int viewerSide) {
  std::string h;
  h.reserve(384);
  h += Font(kGold, "<b>KNIFE ROUND</b>") + "<br>";
  if (k.phase == KnifePhase::Starting) {
    h += "knives only &#183; the winning team picks its side";
    return h;
  }
  const char* col = (k.winnerCs == 3) ? kCt : kT;
  h += Font(col, "<b>" + Esc(k.winnerName, 24) + "</b>") + " won";
  if (!k.reasonShort.empty()) h += " " + Font(kGrey, "(" + k.reasonShort + ")");
  h += "<br>";
  const std::string secs = std::to_string(k.secondsLeft) + "s";
  if (viewerSide == k.winnerCs) {
    h += Font(kOk, "type <b>.stay</b> or <b>.switch</b>") + "<br>";
    h += Font(kGrey, "no pick in " + secs + " = stay");
  } else {
    h += Font(kGrey, "waiting for " + Esc(k.winnerName, 24) + " to pick a side &#183; " + secs);
  }
  return h;
}

// Live map: pause state + countdowns, and the team-left forfeit countdown. No images (resent
// every frame, docs/HUD.md). Empty when there is nothing to show.
static std::string LiveHtml(const LiveHudInfo& l) {
  std::string h;
  if (l.paused) {
    if (l.type == "tactical") {
      h += Font(kGold, "<b>TACTICAL TIMEOUT</b>");
      if (!l.byTeam.empty()) h += "<br>" + Esc(l.byTeam, 24);
    } else if (l.type == "technical") {
      h += Font(kNo, "<b>TECHNICAL PAUSE</b>");
      if (!l.byTeam.empty()) h += " " + Font(kGrey, "by " + Esc(l.byTeam, 24));
      if (l.pendingFreeze) {
        h += "<br>" + Font(kGrey, "pausing at freeze time");
      } else if (l.secondsLeft >= 0) {
        h += "<br>auto-unpause in <b>" + std::to_string(l.secondsLeft / 60) + ":" +
             (l.secondsLeft % 60 < 10 ? "0" : "") + std::to_string(l.secondsLeft % 60) + "</b>";
      }
      auto mark = [&](bool ok, const std::string& n) { return Font(ok ? kOk : kDim, (ok ? "&#10004; " : "&#10006; ") + Esc(n, 16)); };
      h += "<br>" + Font(kGrey, l.bothRequired ? "both teams: " : "pausing team: ") + "<b>.unpause</b><br>" +
           mark(l.team1Confirmed, l.team1) + " &#183; " + mark(l.team2Confirmed, l.team2);
    } else if (l.type == "halftime" || l.type == "auto_5v5" || l.type == "restore") {
      // Pauses that belong to nobody (an engine pause, or a round restore): both teams resume
      // them (match_features.h).
      if (l.type == "halftime") {
        h += Font(kGold, "<b>HALFTIME PAUSE</b>");
      } else if (l.type == "restore") {
        h += Font(kGold, "<b>ROUND RESTORED</b>");
      } else {
        h += Font(kNo, "<b>NOT 5v5 - PAUSED</b>");
        if (!l.byTeam.empty()) h += "<br>" + Font(kGrey, Esc(l.byTeam, 24) + " is short");
      }
      auto mark = [&](bool ok, const std::string& n) { return Font(ok ? kOk : kDim, (ok ? "&#10004; " : "&#10006; ") + Esc(n, 16)); };
      h += "<br>" + Font(kGrey, "both teams: ") + "<b>.unpause</b><br>" + mark(l.team1Confirmed, l.team1) +
           " &#183; " + mark(l.team2Confirmed, l.team2);
    } else {
      h += Font(kNo, "<b>PAUSED BY ADMIN</b>") + "<br>" + Font(kGrey, "an admin unpauses (.fup)");
    }
  }
  if (l.forfeit) {
    if (!h.empty()) h += "<br>";
    h += Font(kNo, "<b>" + Esc(l.forfeitTeam, 24) + " LEFT</b>") + "<br>forfeit in <b>" +
         std::to_string(l.forfeitSecondsLeft / 60) + ":" + (l.forfeitSecondsLeft % 60 < 10 ? "0" : "") +
         std::to_string(l.forfeitSecondsLeft % 60) + "</b> unless a player reconnects";
  }
  return h;
}

// `.ru hud test <n>` variants. Each one fits well under ~1 KB.
static std::string TestHtml(int n) {
  const std::string title = Font(kGrey, "hudtest " + std::to_string(n)) + "<br>";
  switch (n) {
    case 1:
      return title +
             "<font class='fontSize-s'>fontSize-s</font> <font class='fontSize-sm'>sm</font> "
             "<font class='fontSize-m'>m</font><br>"
             "<font class='fontSize-l'>fontSize-l</font> <font class='fontSize-xl'>xl</font> "
             "<font class='fontSize-xxl'>xxl</font><br>"
             "<span class='fontWeight-Bold'>fontWeight-Bold</span> <b>b-tag</b> <i>i-tag</i> <u>u-tag</u><br>" +
             Font(kGold, "gold") + " " + Font(kCt, "CT") + " " + Font(kT, "T") + " " + Font(kOk, "ok") + " " +
             Font(kNo, "no") + " <span style='color:#C084FC'>span-style</span> <font color='red'>named-red</font>";
    case 2:
      return title + "A svg " + Img(kTestSvg, 32) + " | B png " + Img(kTestPng, 32) + "<br>C wiki png " +
             Img(kTestPngWiki, 40) + "<br>" + Font(kGrey, "missing = that format does not render");
    case 3:
      return title +
             "raw utf-8: \xE2\x9C\x94 \xE2\x9C\x96 \xE2\x98\x85 \xC2\xB7 \xE2\x80\xA2 \xE2\x86\x92<br>"
             "decimal: &#10004; &#10006; &#9733; &#183; &#8226; &#8594;<br>"
             "hex: &#x2714; &#x2716; &#x2605;<br>"
             "named: &check; &cross; &star; &middot; &bull; &rarr; &amp; &lt;b&gt;<br>"
             "emoji: \xF0\x9F\x8F\x86 \xF0\x9F\x94\xAA \xE2\x9C\x85 \xE2\x9D\x8C";
    case 4:
      return title + "svg (Auto Tournament icon.svg, height 64)<br>" + Img(kTestSvg, 64);
    case 5:
      return title + "png (Auto Tournament icon-192.png, height 64)<br>" + Img(kTestPng, 64);
    case 6:
      return title + "png (wikimedia 250px, height 64)<br>" + Img(kTestPngWiki, 64);
    case 7: {
      const auto c = Cfg();
      const std::string url = AttrSafeUrl(c.hud_logo_url);
      std::string h = title + HudBrandHtml(24, "fontSize-l") + "<br>";
      h += Font(kGrey, "hud_logo_url=" + (url.empty() ? std::string("(none)") : Esc(url, 80)));
      if (!c.hud_logo_url.empty() && url.empty()) h += "<br>" + Font(kNo, "hud_logo_url rejected (needs http(s), no quotes/spaces)");
      return h;
    }
    // 8..11: measuring the center panel (how many lines, how wide, how images scale), so HUDs can
    // be designed to fit. Screenshot each and read the cut-off.
    case 8: {
      std::string h = title;
      for (int i = 1; i <= 18; ++i) h += std::string(i < 10 ? "row 0" : "row ") + std::to_string(i) + (i < 18 ? "<br>" : "");
      return h;
    }
    case 9:
      return title + "0123456789012345678901234567890123456789012345678901234567890123456789<br>" +
             "<font class='fontSize-s'>0123456789012345678901234567890123456789012345678901234567890123456789012345678901234567890</font><br>" +
             "<font class='fontSize-l'>01234567890123456789012345678901234567890123456789</font><br>" +
             "WWWWWWWWWWWWWWWWWWWWWWWWWWWWWWWWWWWWWWWW";
    case 10:
      return title + "a " + Img(kTestPng, 24) + " b " + Img(kTestPng, 48) + " c <img src='" + std::string(kTestPng) +
             "' style='width:32px;height:32px'> d <img src='" + std::string(kTestPng) + "' height='32'>";
    case 11:
      return title + "wide " + "<img src='" + std::string(kTestPngWiki) + "' width='200' height='40'><br>" +
             "e <img src='" + std::string(kTestPngWiki) + "' width='16' height='16'> f " + Img(kTestSvg, 16) + " g " +
             Img(kTestSvg, 96);
    default:
      return {};
  }
}

static const char* TestDescription(int n) {
  switch (n) {
    case 1: return "font classes (fontSize-s..xxl, fontWeight-Bold), b/i/u, colors";
    case 2: return "images: A = Auto Tournament SVG, B = Auto Tournament PNG, C = wikimedia PNG";
    case 3: return "unicode: raw UTF-8, decimal/hex/named entities, emoji";
    case 4: return "SVG only (Auto Tournament icon.svg)";
    case 5: return "PNG only (Auto Tournament icon-192.png)";
    case 6: return "PNG only (wikimedia 250px)";
    case 7: return "configured header (hud_logo_url + hud_brand)";
    case 8: return "height ruler: rows 01-18 (the last row you see = how many lines fit)";
    case 9: return "width ruler: digits in default / s / l fonts and W's (the last digit you see = the width)";
    case 10: return "PNG sizes: a width/height 24, b 48, c style 32px, d height 32 only";
    case 11: return "wide PNG 200x40, tiny PNG 16, SVG 16 and 96";
    default: return nullptr;
  }
}

static bool HudEnabled() {
  return Cfg().ready_hud;
}

// Last per-client send result; assume it works until a send fails.
std::atomic<bool> g_sendOk{true};

}  // namespace

bool HudReplacesChat() { return HudEnabled() && g_sendOk.load(std::memory_order_relaxed); }

std::string HudBrandHtml(int imgHeight, const char* fontClass) {
  if (!FeatureEnabled(Feature::HudBrand)) return {};
  const auto c = Cfg();
  std::string h;
  const std::string url = AttrSafeUrl(c.hud_logo_url);
  if (!url.empty()) h += Img(url, std::max(8, std::min(128, imgHeight)));
  if (!c.hud_brand.empty()) {
    if (!h.empty()) h += " ";
    h += "<font ";
    if (fontClass && *fontClass) h += std::string("class='") + fontClass + "' ";
    h += std::string("color='") + kGold + "'><b>" + Esc(c.hud_brand, 40) + "</b></font>";
  }
  return h;
}

// ---- `.ru hud anim`: how fast does the client redraw the panel? --------------------------------

namespace {

struct Anim {
  int hz = 64;
  Clock::time_point start{}, until{}, next{};
  int frame = 0;
};
std::mutex g_animMu;
std::unordered_map<uint64_t, Anim> g_anims;

std::string Blocks(int n) {
  std::string o;
  for (int i = 0; i < n; ++i) o += "\u2588";  // full block
  return o;
}

// Ease-out cubic over a 2 s loop: fast at the start, settling at the end.
double EaseOut(double p) { return 1.0 - (1.0 - p) * (1.0 - p) * (1.0 - p); }

std::string AnimHtml(const Anim& a, Clock::time_point now) {
  const double t = std::chrono::duration<double>(now - a.start).count();
  const double loop = 2.0;
  const double p = std::fmod(t, loop) / loop;
  const double v = EaseOut(p);
  constexpr int kSegments = 40;
  const int filled = static_cast<int>(v * kSegments + 0.5);
  std::string h = "<font class='fontSize-l' color='#FFFFFF'>anim " + std::to_string(a.hz) + " Hz &#183; frame " +
                  std::to_string(a.frame) + "</font><br>";
  if (filled > 0) h += "<font color='#4ADE80'>" + Blocks(filled) + "</font>";
  if (filled < kSegments) h += "<font color='#3F3F46'>" + Blocks(kSegments - filled) + "</font>";
  char stats[64];
  std::snprintf(stats, sizeof stats, "%5.1f%% &#183; t=%.2f s", v * 100.0, t);
  h += "<br><font class='fontSize-m' color='#A3E635'>" + std::string(stats) + "</font>";
  return h;
}

}  // namespace

std::string ReadyHudRequestAnim(uint64_t steamid64, int hz, int seconds) {
  if (steamid64 == 0) return {};
  hz = std::clamp(hz > 0 ? hz : 64, 1, 64);
  seconds = std::clamp(seconds > 0 ? seconds : 10, 1, 30);
  const auto now = Clock::now();
  Anim a;
  a.hz = hz;
  a.start = now;
  a.next = now;
  a.until = now + std::chrono::seconds(seconds);
  std::lock_guard<std::mutex> lk(g_animMu);
  g_anims[steamid64] = a;
  return std::to_string(hz) + " Hz for " + std::to_string(seconds) + " s";
}

bool ReadyHudAnimActive(uint64_t steamid64) {
  std::lock_guard<std::mutex> lk(g_animMu);
  return g_anims.count(steamid64) != 0;
}

void ReadyHudAnimTick() {
  const auto now = Clock::now();
  struct Send {
    int slot;
    std::string html;
  };
  std::vector<Send> sends;
  {
    std::lock_guard<std::mutex> lk(g_animMu);
    if (g_anims.empty()) return;
    for (auto it = g_anims.begin(); it != g_anims.end();) {
      if (now >= it->second.until) {
        it = g_anims.erase(it);
        continue;
      }
      Anim& a = it->second;
      if (now >= a.next) {
        const int slot = GameEventsSlotForSteam(it->first).value_or(-1);
        if (slot >= 0) {
          ++a.frame;
          sends.push_back(Send{slot, AnimHtml(a, now)});
        }
        // Next frame on the hz grid (no drift); skip missed frames rather than bursting.
        const auto step = std::chrono::duration_cast<Clock::duration>(std::chrono::duration<double>(1.0 / a.hz));
        do a.next += step;
        while (a.next <= now);
      }
      ++it;
    }
  }
  for (const auto& s : sends) SendCenterHtml(s.slot, s.html, 1, RU_HTML_PRIO_NOTICE);
}

bool ReadyHudShowing() {
  return g_showing.load(std::memory_order_relaxed) && HudEnabled();
}

std::string ReadyHudRequestTest(uint64_t steamid64, int variant) {
  const char* desc = TestDescription(variant);
  if (!desc || steamid64 == 0) return {};
  std::lock_guard<std::mutex> lk(g_testMu);
  g_tests[steamid64] = {variant, Clock::now() + kTestShowFor};
  return desc;
}

void ReadyHudTick() {
  if (IsDisabled()) return;
  const auto now = Clock::now();
  if (g_lastRun.time_since_epoch().count() != 0 && (now - g_lastRun) < std::chrono::milliseconds(Cfg().hud_tick_ms)) return;
  g_lastRun = now;

  // Pending `.ru hud test` panels (shown even with ready_hud=0 and outside warmup).
  std::unordered_map<uint64_t, int> tests;
  {
    std::lock_guard<std::mutex> lk(g_testMu);
    for (auto it = g_tests.begin(); it != g_tests.end();) {
      if (now >= it->second.second) {
        it = g_tests.erase(it);
      } else {
        tests[it->first] = it->second.first;
        ++it;
      }
    }
  }

  const bool enabled = HudEnabled();
  if (!enabled) g_showing.store(false, std::memory_order_relaxed);
  if (!enabled && tests.empty()) {
    g_sent.clear();
    return;
  }

  const ReadyUpMode mode = GetMode();
  const auto ctx = WebhookGetMatchContext();

  enum class What { None, Ready, Knife, Live };
  What what = What::None;
  KnifeHudInfo knife;
  std::string liveHtml;
  if (!enabled) {
    what = What::None;
  } else if (mode == ReadyUpMode::ScrimWarmup) {
    what = What::Ready;
  } else if (mode == ReadyUpMode::MatchWarmup && ctx && WarmupEnabled() && !GoLiveTriggered()) {
    what = What::Ready;
  } else if (mode == ReadyUpMode::MatchKnife) {
    knife = KnifeHudSnapshot();
    // knife.cfg has almost no freeze time, so "Starting" lasts about a second.
    // Keep the KNIFE ROUND panel up for hud_knife_hold_s after the knife round
    // begins, also while it is running.
    static Clock::time_point s_knifeSince{};
    if (knife.phase == KnifePhase::None) {
      s_knifeSince = {};
    } else if (s_knifeSince == Clock::time_point{}) {
      s_knifeSince = Clock::now();
    }
    const bool holding = knife.phase == KnifePhase::Running && s_knifeSince != Clock::time_point{} &&
                         (Clock::now() - s_knifeSince) < std::chrono::seconds(Cfg().hud_knife_hold_s);
    if (knife.phase == KnifePhase::Starting || knife.phase == KnifePhase::Picking || holding) {
      what = What::Knife;
      if (holding) knife.phase = KnifePhase::Starting;  // same "knives only" text while held
    }
  } else if (mode == ReadyUpMode::MatchLive) {
    liveHtml = LiveHtml(MatchFeaturesHud());
    if (!liveHtml.empty()) what = What::Live;
  }
  if (what == What::None && tests.empty()) {
    g_sent.clear();
    return;
  }

  const auto humans = ListHumans();
  Board board;
  std::string footer;
  if (what == What::Ready) {
    board = BuildBoard(mode, ctx, humans);
    footer = WarmupHtmlCustom();
    if (footer.size() > 256) footer.clear();  // keep the panel small
  }

  std::unordered_set<int> present;
  static std::atomic<bool> s_warned{false};
  static std::atomic<bool> s_loggedSize{false};
  for (const auto& h : humans) {
    // Engine slot from events when known; else the log `<N>` (the player slot in CS2).
    const int slot = h.slot >= 0 ? h.slot : h.userid;
    if (h.steamid64 == 0 || slot < 0 || slot > 63) continue;
    present.insert(slot);

    std::string html;
    const auto t = tests.find(h.steamid64);
    if (t != tests.end()) {
      html = TestHtml(t->second);  // over the HUD and the welcome card
    } else {
      if (what == What::None) continue;
      // The welcome screen owns the panel while it is up (~5s after joining).
      if (WelcomeActiveFor(slot, h.steamid64)) continue;
      if (ReadyHudAnimActive(h.steamid64)) continue;  // `.ru hud anim` owns this player's panel
      if (AdminCallCardActiveFor(slot)) continue;      // an `.admin` call card (admin_call.h)
      html = (what == What::Ready)   ? ReadyHtml(board, h.steamid64, footer)
             : (what == What::Live) ? liveHtml
                                    : KnifeHtml(knife, h.team);
    }
    if (html.empty()) continue;

    // Same HTML: resend every hud_resend_ms (default 0 = every tick; CS2's center panel ignores
    // the event's duration and only stays steady when re-sent every frame, docs/HUD.md).
    // Changed HTML: send right away.
    auto& s = g_sent[slot];
    const bool changed = (s.html != html);
    if (s.html == html && (now - s.at) < std::chrono::milliseconds(Cfg().hud_resend_ms)) continue;
    // HUD tests are one-off cards (over another plugin's HUD); the ready HUD is the HUD level.
    const int sent = SendCenterHtml(slot, html, Cfg().hud_duration_s,
                                    t != tests.end() ? RU_HTML_PRIO_NOTICE : RU_HTML_PRIO_HUD);
    if (sent < 0) continue;  // another plugin's higher panel (download bar) is up: retry next tick
    const bool ok = sent == 1;
    g_sendOk.store(ok, std::memory_order_relaxed);
    if (ok) {
      s.html = html;
      s.at = now;
      if (t == tests.end()) g_showing.store(true, std::memory_order_relaxed);
      if (t != tests.end() && changed) {
        Debug("ready-hud: hudtest %d -> slot=%d (%zu bytes html)\n", t->second, slot, html.size());
      }
      if (!s_loggedSize.exchange(true)) Debug("ready-hud: first panel sent (%zu bytes html)\n", html.size());
      static std::atomic<bool> s_warnedBig{false};
      if (html.size() > 1000 && !s_warnedBig.exchange(true)) {
        Print("ready-hud: panel is %zu bytes (large; the client may cut it off)\n", html.size());
      }
    } else {
      g_showing.store(false, std::memory_order_relaxed);
      if (!s_warned.exchange(true)) PrintLine("ready-hud: per-client center HTML unavailable; ready HUD not shown.");
    }
  }
  for (auto it = g_sent.begin(); it != g_sent.end();) {
    if (present.count(it->first) == 0) it = g_sent.erase(it);
    else ++it;
  }
}

}  // namespace readyup

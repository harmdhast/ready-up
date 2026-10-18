// readyup-essentials: the everyday server basics, apart from the match flow, so every kind of
// server has them (a practice-only server is core + essentials + practice; an esports server can
// run core + match alone).
//
//   ru admins [list]                         who the Ready Up admins are (admins.json)
//   ru admins add|remove <steamid64|name>    admin; the first admin only from the console
//   ru map change <name|workshop id> [force] changelevel / host_workshop_map (admin)
//   ru map reload [force]                    the current map again (workshop maps by id)
//   ru map restart [force]                   mp_restartgame 1
//   ru map defaults                          the default map per mode (default_maps.json)
//   ru map default <mode> [<map>|clear]      show / set one (admin), e.g. `ru map default ffa aim_map`
//
// While the server downloads a Workshop map (host_workshop_map), everyone sees a progress bar in
// the center panel, resent ~10x a second (core API 1.4 workshop_download_progress).
//
//   .rcon <command>                          a server console command; admins.json admins only (not
//                                            match or platform admins), logged; refuses quit / exit /
//                                            _restart, sv_setsteamaccount, rcon_password, alias, ...
//                                            (essentials_rules.h RconRefusal)
//
// Chat: `.ru admins ...`, `.ru map ...`. Map commands are refused during a knife round or a live
// map (the match plugin's mode) unless `force` is added.
//
// Admins: csgo/readyup/plugins/essentials/admins.json, re-read when it changes; on first load the
// match plugin's old plugins/match/admins.json is copied over. This plugin is an admin provider
// (a player is an admin when any provider says so: the match plugin adds the match config's
// admins, the MAT list and, in fleet mode, the platform's list). admins.json counts in fleet mode
// too (docs/FLEET.md D5 adds the platform's list, it does not replace the local one).
//
// Default maps: csgo/readyup/plugins/essentials/default_maps.json maps a mode name (ffa, tdm,
// practice, warmup, retakes, ...) to a map entry; re-read when it changes. Other plugins get them
// through readyup.essentials.v1 (default_map, plus load_map: a map change with the download bar);
// the deathmatch plugin loads its mode's default map with them. The platform is meant to push the
// file over the fleet link later.
#include <dirent.h>
#include "essentials_rules.h"

#include "readyup/essentials_iface.h"
#include "readyup/fleet_iface.h"
#include "readyup/help_iface.h"
#include "readyup/map_names.h"
#include "readyup/match_iface.h"
#include "readyup/plugin_api.h"
#include "readyup/selftest_iface.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <mutex>
#include <sstream>
#include <string>
#include <sys/stat.h>
#include <vector>

#ifndef ESSENTIALS_VERSION
#define ESSENTIALS_VERSION "dev"
#endif

namespace essentials {
namespace {

const ru_api* g_api = nullptr;

std::mutex g_mu;  // g_admins (the provider runs on any thread)
std::vector<Admin> g_admins;
std::string g_path;
int64_t g_mtime = -1;
double g_nextReload = 0;
double g_nextHelpSync = 0;

// default_maps.json (game thread only).
DefaultMaps g_defaultMaps;
std::string g_mapsPath;
int64_t g_mapsMtime = -1;

// Workshop download being shown (game thread only).
struct Download {
  uint64_t id = 0;
  std::string name;
  double started = 0, nextPoll = 0, nextLog = 0;
  bool announced = false;
} g_dl;
constexpr double kDownloadPollSeconds = 0.1;
constexpr double kDownloadTimeoutSeconds = 600;

int64_t MtimeNs(const std::string& path) {
  struct stat st {};
  if (stat(path.c_str(), &st) != 0) return -1;
  return static_cast<int64_t>(st.st_mtim.tv_sec) * 1000000000LL + st.st_mtim.tv_nsec;
}

std::string ReadFile(const std::string& path) {
  std::ifstream f(path);
  if (!f) return {};
  std::stringstream ss;
  ss << f.rdbuf();
  return ss.str();
}

void LoadAdmins() {
  const int64_t m = MtimeNs(g_path);
  if (m == g_mtime) return;
  bool ok = true;
  auto list = ParseAdmins(ReadFile(g_path), &ok);
  if (!ok) ru_logf(g_api, RU_LOG_WARN, "%s is not valid JSON; keeping the previous list", g_path.c_str());
  std::lock_guard<std::mutex> lk(g_mu);
  if (ok) g_admins = std::move(list);
  g_mtime = m;
}

void LoadDefaultMaps() {
  const int64_t m = MtimeNs(g_mapsPath);
  if (m == g_mapsMtime) return;
  bool ok = true;
  auto maps = ParseDefaultMaps(ReadFile(g_mapsPath), &ok);
  if (!ok) ru_logf(g_api, RU_LOG_WARN, "%s is not valid JSON; keeping the previous default maps", g_mapsPath.c_str());
  else g_defaultMaps = std::move(maps);
  g_mapsMtime = m;
}

bool SaveDefaultMaps() {
  const std::string tmp = g_mapsPath + ".tmp";
  {
    std::ofstream f(tmp, std::ios::trunc);
    if (!f) return false;
    f << DefaultMapsJson(g_defaultMaps);
    if (!f) return false;
  }
  if (std::rename(tmp.c_str(), g_mapsPath.c_str()) != 0) return false;
  g_mapsMtime = MtimeNs(g_mapsPath);
  return true;
}

bool SaveAdmins() {
  std::string text;
  {
    std::lock_guard<std::mutex> lk(g_mu);
    text = AdminsJson(g_admins);
  }
  const std::string tmp = g_path + ".tmp";
  {
    std::ofstream f(tmp, std::ios::trunc);
    if (!f) return false;
    f << text;
    if (!f) return false;
  }
  if (std::rename(tmp.c_str(), g_path.c_str()) != 0) return false;
  g_mtime = MtimeNs(g_path);
  return true;
}

// The match plugin's old admins.json (<plugins>/match/admins.json) becomes ours once.
void MigrateFromMatch(const std::string& dataDir) {
  if (MtimeNs(g_path) >= 0) return;
  const std::string old = dataDir + "/../match/admins.json";
  if (MtimeNs(old) < 0) return;
  std::ifstream in(old, std::ios::binary);
  std::ofstream out(g_path, std::ios::binary);
  out << in.rdbuf();
  if (out) ru_logf(g_api, RU_LOG_INFO, "admins: copied %s to %s", old.c_str(), g_path.c_str());
}

bool FleetMode() {
  const auto* f = static_cast<const ru_fleet_v1*>(g_api->get_interface(g_api->self, RU_FLEET_IFACE_NAME, 1));
  return f && f->connection_state && f->connection_state() != RU_FLEET_LINK_STANDALONE;
}

std::string RuMode() {
  const auto* m = static_cast<const ru_match_v1*>(g_api->get_interface(g_api->self, RU_MATCH_IFACE_NAME, 1));
  if (!m) return {};
  if (RU_API_HAS(m, mode) && m->mode) return m->mode() ? m->mode() : "";
  ru_match_status st{};
  st.struct_size = sizeof(st);
  return m->get_status && m->get_status(&st) == 1 && st.ru_mode ? st.ru_mode : "";
}

// admins.json always counts. In fleet mode the platform's list (match plugin) adds more, so a
// player not in the file is left to it (-1) instead of refused.
int Provider(void*, uint64_t steamid64) {
  {
    std::lock_guard<std::mutex> lk(g_mu);
    if (IsAdmin(g_admins, steamid64)) return 1;
  }
  return FleetMode() ? -1 : 0;
}

bool SenderIsAdmin(const ru_command_ctx* c) { return c->is_console || g_api->is_admin(g_api->self, c->steamid64) == 1; }

void Reply(const ru_command_ctx* c, const std::string& msg) {
  if (c->is_console) ru_logf(g_api, RU_LOG_INFO, "%s", msg.c_str());
  else if (c->slot >= 0) g_api->chat_to_slot(g_api->self, c->slot, (" \x04[ReadyUp]\x01 " + msg).c_str());
}

std::vector<Player> ConnectedPlayers() {
  std::vector<Player> out;
  g_api->for_each_player(
      g_api->self,
      [](void* u, const ru_player* p) -> int {
        if (!p->is_bot && p->steamid64) static_cast<std::vector<Player>*>(u)->push_back(Player{p->steamid64, p->name});
        return 1;
      },
      &out);
  return out;
}

void OnAdmins(const ru_command_ctx* c, const std::string& sub, const std::vector<std::string>& args) {
  if (sub.empty() || sub == "list") {
    std::vector<Admin> list;
    {
      std::lock_guard<std::mutex> lk(g_mu);
      list = g_admins;
    }
    Reply(c, "admins (admins.json): " + std::to_string(list.size()) +
                 (FleetMode() ? " (plus the platform's list)" : ""));
    for (size_t i = 0; i < list.size() && i < 10; ++i) {
      Reply(c, "- " + (list[i].name.empty() ? std::string("?") : list[i].name) + " (" + std::to_string(list[i].steamid64) + ")");
    }
    if (list.size() > 10) Reply(c, "... and " + std::to_string(list.size() - 10) + " more");
    return;
  }
  if (sub == "help") {
    for (const char* l : {".ru admins [list]: the admins", ".ru admins add|remove <steamid64|name>: admin (the first one from the console)"}) Reply(c, l);
    return;
  }
  if (sub != "add" && sub != "remove") return Reply(c, "unknown command. Type .ru help admins for the list.");
  if (!c->is_console) {
    bool empty;
    {
      std::lock_guard<std::mutex> lk(g_mu);
      empty = g_admins.empty();
    }
    if (empty) return Reply(c, "no admins yet: add the first one from the server console (ru admins add <steamid64>)");
    if (!SenderIsAdmin(c)) return Reply(c, "not authorized");
  }
  if (args.empty()) return Reply(c, "usage: .ru admins " + sub + " <steamid64|name>");
  std::string target;
  for (const auto& a : args) target += (target.empty() ? "" : " ") + a;
  Player p;
  if (const uint64_t id = ParseSteamId64(target)) {
    p.steamid64 = id;
    for (const auto& q : ConnectedPlayers()) {
      if (q.steamid64 == id) p.name = q.name;
    }
  } else {
    std::string err;
    if (!FindPlayer(ConnectedPlayers(), target, &p, &err)) return Reply(c, err);
  }
  bool changed;
  {
    std::lock_guard<std::mutex> lk(g_mu);
    changed = sub == "add" ? AddAdmin(&g_admins, p.steamid64, p.name.empty() ? "Admin" : p.name)
                           : RemoveAdmin(&g_admins, p.steamid64);
  }
  const std::string who = (p.name.empty() ? std::string() : p.name + " ") + "(" + std::to_string(p.steamid64) + ")";
  if (!changed) return Reply(c, (sub == "add" ? "already an admin: " : "not an admin: ") + who);
  if (!SaveAdmins()) return Reply(c, "could not save " + g_path);
  Reply(c, (sub == "add" ? "added " : "removed ") + who);
}

// A center panel to everyone at alert level (API 1.6), else the plain call.
void SendAlertAll(const std::string& html, int seconds) {
  if (RU_API_HAS(g_api, center_html_all_prio) && g_api->center_html_all_prio) {
    g_api->center_html_all_prio(g_api->self, html.c_str(), seconds, RU_HTML_PRIO_ALERT);
  } else {
    g_api->center_html_all(g_api->self, html.c_str(), seconds);
  }
}

bool LoadEntry(const std::string& entry) {
  const std::string cmd = readyup::mapnames::LoadCommand(entry);
  if (cmd.empty()) return false;
  readyup::mapnames::MapRef ref;
  if (readyup::mapnames::ParseEntry(entry, &ref) && !ref.workshop_id.empty()) {
    readyup::mapnames::NoteWorkshopLoad(ref.workshop_id);
    g_dl = Download{};
    g_dl.id = std::strtoull(ref.workshop_id.c_str(), nullptr, 10);
    g_dl.name = readyup::mapnames::DisplayName(entry);
  }
  return g_api->server_command(g_api->self, cmd.c_str()) == 1;
}

// "(none)", the entry, or "aim_map (workshop/123/aim_map)".
std::string Shown(const std::string& entry) {
  if (entry.empty()) return "(none)";
  const std::string d = readyup::mapnames::DisplayName(entry);
  return d == entry ? entry : d + " (" + entry + ")";
}

void OnMapDefaults(const ru_command_ctx* c, const std::string& sub, const std::vector<std::string>& args) {
  LoadDefaultMaps();
  if (sub == "defaults" || args.empty()) {
    std::vector<std::string> modes = KnownDefaultMapModes();
    for (const auto& kv : g_defaultMaps) {
      if (std::find(modes.begin(), modes.end(), kv.first) == modes.end()) modes.push_back(kv.first);
    }
    if (sub == "default" && !args.empty()) modes = {args[0]};
    for (const auto& m : modes) Reply(c, "default map " + m + ": " + Shown(DefaultMapFor(g_defaultMaps, m)));
    return;
  }
  if (args.size() == 1) return Reply(c, "default map " + args[0] + ": " + Shown(DefaultMapFor(g_defaultMaps, args[0])));
  if (!SenderIsAdmin(c)) return Reply(c, "not authorized");
  if (args.size() != 2) return Reply(c, "usage: .ru map default <mode> [<name|workshop id|link>|clear]");
  std::string err;
  if (!SetDefaultMap(&g_defaultMaps, args[0], args[1], &err)) return Reply(c, err);
  if (!SaveDefaultMaps()) return Reply(c, "could not save " + g_mapsPath);
  const std::string e = DefaultMapFor(g_defaultMaps, args[0]);
  ru_logf(g_api, RU_LOG_INFO, "default map %s = %s by %s", args[0].c_str(), e.empty() ? "(none)" : e.c_str(),
          c->is_console ? "Console" : (c->name ? c->name : "?"));
  Reply(c, "default map " + args[0] + ": " + Shown(e));
}

// Maps this server has: csgo/maps/<name>.vpk and the community maps in csgo_community_addons/<name>/.
std::string g_csgoDir;  // .../game/csgo
std::vector<std::string> ServerMaps() {
  std::vector<std::string> out;
  auto scan = [&](const std::string& dir, bool dirs) {
    DIR* d = opendir(dir.c_str());
    if (!d) return;
    while (const dirent* e = readdir(d)) {
      std::string n = e->d_name;
      if (n.empty() || n[0] == '.') continue;
      if (!dirs) {
        if (n.size() <= 4 || n.compare(n.size() - 4, 4, ".vpk") != 0) continue;
        n.resize(n.size() - 4);
      } else if (e->d_type != DT_DIR && e->d_type != DT_UNKNOWN) {
        continue;
      }
      const size_t u = n.find('_');
      const bool vanity = n.size() > 7 && n.compare(n.size() - 7, 7, "_vanity") == 0;  // menu backgrounds
      if ((u == 2 || u == 3) && !vanity) out.push_back(n);  // de_ cs_ ar_ ...: playable maps
    }
    closedir(d);
  };
  scan(g_csgoDir + "/maps", false);
  scan(g_csgoDir + "/../csgo_community_addons", true);
  std::sort(out.begin(), out.end());
  out.erase(std::unique(out.begin(), out.end()), out.end());
  return out;
}

// A plain name (not a workshop id or link) -> the server's map it means ("mirage" -> de_mirage).
bool ResolveMapArg(const ru_command_ctx* c, std::string* arg) {
  const std::string& a = *arg;
  const bool plain = !a.empty() && a.find('/') == std::string::npos && a.find("steamcommunity") == std::string::npos &&
                     a.find_first_not_of("0123456789") != std::string::npos;
  if (!plain) return true;
  const std::vector<std::string> maps = ServerMaps();
  if (maps.empty()) return true;  // can't tell: let the engine try the name as typed
  const MapMatch m = ResolveMapName(a, maps);
  if (!m.name.empty()) {
    *arg = m.name;
    return true;
  }
  if (!m.ambiguous.empty()) {
    std::string l;
    for (const auto& n : m.ambiguous) l += (l.empty() ? "" : ", ") + n;
    Reply(c, "\"" + a.substr(0, 32) + "\" could be: " + l + ". Be more specific.");
  } else {
    Reply(c, "no map like \"" + a.substr(0, 32) + "\" on this server (a workshop id or link works too)");
  }
  return false;
}

void OnMap(const ru_command_ctx* c, const std::string& sub, std::vector<std::string> args) {
  if (sub.empty() || sub == "help") {
    for (const char* l : {".ru map change <name|workshop id|link> [force]: change map (admin)",
                          ".ru map reload [force]: load the current map again (admin)",
                          ".ru map restart [force]: restart the game, mp_restartgame 1 (admin)",
                          ".ru map defaults: the default map per mode (ffa, tdm, practice, ...)",
                          ".ru map default <mode> [<map>|clear]: show / set one (admin)"}) {
      Reply(c, l);
    }
    return;
  }
  if (sub == "defaults" || sub == "default") return OnMapDefaults(c, sub, args);
  if (sub != "change" && sub != "reload" && sub != "restart") return Reply(c, "unknown command. Type .ru help map for the list.");
  if (!SenderIsAdmin(c)) return Reply(c, "not authorized");
  const bool force = !args.empty() && args.back() == "force";
  if (force) args.pop_back();
  const std::string mode = RuMode();
  if (MapCommandBlocked(mode) && !force) {
    return Reply(c, "a match is live (" + mode + "): add `force` to " + sub + " anyway");
  }
  const std::string who = c->is_console ? std::string("Console") : std::string(c->name ? c->name : "?");
  if (sub == "restart") {
    g_api->server_command(g_api->self, "mp_restartgame 1");
    ru_logf(g_api, RU_LOG_INFO, "map restart by %s", who.c_str());
    return g_api->chat_all(g_api->self, "Ready Up: game restarting.", 0), void();
  }
  std::string entry;
  if (sub == "change") {
    if (args.size() != 1) return Reply(c, "usage: .ru map change <name|workshop id> [force]");
    if (!ResolveMapArg(c, &args[0])) return;  // "mirage" -> de_mirage; asks when unsure
    entry = MapArgToEntry(args[0]);  // a pasted Workshop link works too
    if (!readyup::mapnames::ValidEntry(entry)) {
      return Reply(c, "\"" + args[0].substr(0, 64) + "\" is not a map name, workshop id or workshop link");
    }
  } else {
    const char* cur = g_api->current_map(g_api->self);
    entry = readyup::mapnames::ReloadEntry(cur ? cur : "");
    if (entry.empty()) return Reply(c, "current map not known yet");
  }
  if (!LoadEntry(entry)) return Reply(c, "map change unavailable yet");
  ru_logf(g_api, RU_LOG_INFO, "map %s by %s: %s", sub.c_str(), who.c_str(), entry.c_str());
  // Over every other panel until the level changes (a cached map loads with no download bar).
  SendAlertAll(MapChangePanelHtml(readyup::mapnames::DisplayName(entry), sub != "change"), 5);
  g_api->chat_all(g_api->self, ("Ready Up: " + std::string(sub == "change" ? "changing map to " : "reloading ") +
                                readyup::mapnames::DisplayName(entry) + ".")
                                   .c_str(),
                  0);
}

void OnRu(void*, const ru_command_ctx* c) {
  const std::string main = c->argc >= 2 && c->argv[1] ? c->argv[1] : "";
  const std::string sub = c->argc >= 3 && c->argv[2] ? c->argv[2] : "";
  std::vector<std::string> args;
  for (int i = 3; i < c->argc; ++i) args.emplace_back(c->argv[i] ? c->argv[i] : "");
  if (main == "admins") OnAdmins(c, sub, args);
  else if (main == "map") OnMap(c, sub, args);
}

// `.map <name>` / `.reloadmap`: chat shortcuts for `.ru map change <name>` / `.ru map reload` (same
// admin check, live-match guard and `force`).
void OnMapChat(void*, const ru_command_ctx* c) {
  const std::string cmd = c->argc >= 1 && c->argv[0] ? c->argv[0] : "";
  std::vector<std::string> args;
  for (int i = 1; i < c->argc; ++i) args.emplace_back(c->argv[i] ? c->argv[i] : "");
  if (cmd == ".reloadmap") return OnMap(c, "reload", args);
  if (args.empty() || args[0] == "help") return Reply(c, ".map <name|workshop id|link> [force]: change map (admin)");
  OnMap(c, "change", args);
}

// `.rcon <command>` (the old plugin's css_rcon / .rcon): runs a server console command. Only for
// the server's own admins (admins.json, the "root" list): not a match config's admins and not the
// platform's fleet-wide list (the platform has root-only cmd exec for that). RconRefusal refuses
// what stops the server, sets a GSLT or touches credentials. Logged with who ran it.
void OnRconChat(void*, const ru_command_ctx* c) {
  if (!c->is_console) {
    bool root;
    {
      std::lock_guard<std::mutex> lk(g_mu);
      root = IsAdmin(g_admins, c->steamid64);
    }
    if (!root) {
      ru_logf(g_api, RU_LOG_INFO, "rcon refused for %s (%llu): not in admins.json", c->name ? c->name : "?",
              static_cast<unsigned long long>(c->steamid64));
      return Reply(c, "not authorized: .rcon is for this server's admins (admins.json)");
    }
  }
  const std::string text = c->text ? c->text : "";
  const size_t sp = text.find_first_of(" \t");
  std::string command = sp == std::string::npos ? std::string() : text.substr(sp + 1);
  command.erase(0, std::min(command.size(), command.find_first_not_of(" \t")));
  const std::string why = RconRefusal(command);
  const std::string who = c->is_console ? std::string("Console")
                                        : std::string(c->name ? c->name : "?") + " (" + std::to_string(c->steamid64) + ")";
  if (!why.empty()) {
    if (!command.empty()) ru_logf(g_api, RU_LOG_WARN, "rcon by %s refused (%s): %s", who.c_str(), why.c_str(), command.c_str());
    return Reply(c, why);
  }
  ru_logf(g_api, RU_LOG_INFO, "rcon by %s: %s", who.c_str(), command.c_str());
  if (g_api->server_command(g_api->self, command.c_str()) != 1) return Reply(c, "server commands are unavailable yet");
  Reply(c, "Command sent successfully!");
}

void OnMapStart(void*, const ru_event* e) {
  g_dl = Download{};  // downloaded (or cached) and loaded
  if (e && e->map && *e->map) readyup::mapnames::NoteMapLoaded(e->map);  // binds a workshop id to its map
}

// Progress bar for the Workshop map being downloaded. Nothing is shown while Steam reports no
// download (an installed map loads straight away).
void PollDownload(double now) {
  if (!g_dl.id || !RU_API_HAS(g_api, workshop_download_progress) || !g_api->workshop_download_progress) return;
  if (g_dl.started == 0) g_dl.started = now;
  if (now - g_dl.started > kDownloadTimeoutSeconds) {
    ru_logf(g_api, RU_LOG_WARN, "workshop %llu: no map start after %.0f s; progress bar stopped",
            static_cast<unsigned long long>(g_dl.id), kDownloadTimeoutSeconds);
    g_dl = Download{};
    return;
  }
  if (now < g_dl.nextPoll) return;
  g_dl.nextPoll = now + kDownloadPollSeconds;
  uint64_t done = 0, total = 0;
  if (!g_api->workshop_download_progress(g_api->self, g_dl.id, &done, &total)) return;
  if (!g_dl.announced) {
    g_dl.announced = true;
    ru_logf(g_api, RU_LOG_INFO, "workshop %llu: downloading", static_cast<unsigned long long>(g_dl.id));
  }
  if (now >= g_dl.nextLog && total > 0) {
    g_dl.nextLog = now + 5.0;
    ru_logf(g_api, RU_LOG_INFO, "workshop %llu: %.1f / %.1f MB", static_cast<unsigned long long>(g_dl.id),
            done / 1048576.0, total / 1048576.0);
  }
  // Over the ready HUD and the welcome card while it downloads (API 1.6: one panel per player).
  SendAlertAll(DownloadPanelHtml(g_dl.name, done, total), 1);
}

// `.help` on a server without the match plugin (which answers it itself): one line per loaded
// plugin that publishes readyup.help.<name>, then the commands everyone has. Owned only while
// match.so is absent (SyncHelpCommand), so a match server keeps match.so's `.help`.
ru_handle g_helpCmd = 0;

void OnHelpChat(void*, const ru_command_ctx* c) {
  if (c->slot < 0) return;
  auto say = [&](const std::string& line) { g_api->chat_to_slot(g_api->self, c->slot, (" \x04[ReadyUp]\x01 " + line).c_str()); };
  say("Commands:");
  for (const char* name : RU_HELP_PLUGINS) {
    const auto* h = static_cast<const ru_help_v1*>(
        g_api->get_interface(g_api->self, (std::string(RU_HELP_IFACE_PREFIX) + name).c_str(), RU_HELP_IFACE_VERSION));
    if (!h || !h->line) continue;
    const char* line = h->line();
    if (line && *line) say(line);
  }
  say(".ru help  every command on this server");
  char buf[256];
  if (RU_API_HAS(g_api, license_player_line) && g_api->license_player_line &&
      g_api->license_player_line(g_api->self, buf, sizeof(buf)) > 0) {
    say(buf);
  }
}

void SyncHelpCommand() {
  const bool match = g_api->get_interface(g_api->self, RU_MATCH_IFACE_NAME, 1) != nullptr;
  if (match && g_helpCmd) {
    g_api->unregister(g_api->self, g_helpCmd);
    g_helpCmd = 0;
  } else if (!match && !g_helpCmd) {
    g_helpCmd = g_api->register_chat_command(g_api->self, ".help", &OnHelpChat, nullptr);
  }
}

void OnTick(void*, const ru_tick_info* t) {
  PollDownload(t->now);
  if (t->now >= g_nextHelpSync) {
    g_nextHelpSync = t->now + 1.0;
    SyncHelpCommand();
  }
  if (t->now < g_nextReload) return;
  g_nextReload = t->now + 30.0;
  LoadAdmins();  // re-read when admins.json changed on disk
  LoadDefaultMaps();
}

// ---- readyup.essentials.v1 ------------------------------------------------------------------------

std::string g_defaultMapRet;  // default_map's return value (valid until the next call)
const char* IfaceDefaultMap(const char* mode) {
  try {
    LoadDefaultMaps();  // a stat(): picks up a hand-edited file right away
    g_defaultMapRet = DefaultMapFor(g_defaultMaps, mode ? mode : "");
  } catch (...) {
    g_defaultMapRet.clear();
  }
  return g_defaultMapRet.c_str();
}
int IfaceLoadMap(const char* entry) {
  try {
    const std::string e = MapArgToEntry(entry ? entry : "");
    if (!readyup::mapnames::ValidEntry(e)) return 0;
    if (!LoadEntry(e)) return 0;
    ru_logf(g_api, RU_LOG_INFO, "map change for another plugin: %s", e.c_str());
    SendAlertAll(MapChangePanelHtml(readyup::mapnames::DisplayName(e), false), 5);  // like `ru map change`
    return 1;
  } catch (...) {
    return 0;
  }
}
int IfaceSetDefaultMap(const char* mode, const char* entry) {
  try {
    LoadDefaultMaps();
    const std::string m = mode ? mode : "", e = entry && *entry ? entry : "clear";
    std::string err;
    if (!SetDefaultMap(&g_defaultMaps, m, e, &err)) {
      ru_logf(g_api, RU_LOG_WARN, "default map %s: %s", m.c_str(), err.c_str());
      return 0;
    }
    if (!SaveDefaultMaps()) return 0;
    const std::string now = DefaultMapFor(g_defaultMaps, m);
    ru_logf(g_api, RU_LOG_INFO, "default map %s = %s (set by another plugin)", m.c_str(), now.empty() ? "(none)" : now.c_str());
    return 1;
  } catch (...) {
    return 0;
  }
}
const ru_essentials_v1 g_iface = {sizeof(ru_essentials_v1), &IfaceDefaultMap, &IfaceLoadMap, &IfaceSetDefaultMap};

void RunSelftest(ru_selftest_add_fn add, void* ctx) {
  std::string d;
  {
    std::lock_guard<std::mutex> lk(g_mu);
    d = std::to_string(g_admins.size()) + " admin(s) in " + g_path;
  }
  if (FleetMode()) d += ", plus the platform's list (fleet mode)";
  d += "; " + std::to_string(g_defaultMaps.size()) + " default map(s)";
  add(ctx, "INFO", "essentials", d.c_str());
}
const ru_selftest_iface_v1 g_selftestIface = {sizeof(ru_selftest_iface_v1), &RunSelftest};

}  // namespace
}  // namespace essentials

using namespace essentials;

extern "C" {

READYUP_PLUGIN_EXPORT const ru_plugin_info* readyup_plugin_info(void) {
  static const ru_plugin_info info = {
      sizeof(ru_plugin_info),
      (1u << 16) | 2u,  // needs API 1.2 (register_ru_subcommand)
      "essentials",
      ESSENTIALS_VERSION,
      "Ready Up",
      "server basics: admins (admins.json), map change / reload / restart, default maps per mode",
  };
  return &info;
}

READYUP_PLUGIN_EXPORT int readyup_plugin_load(const ru_api* api, uint32_t core_api_version) {
  if (RU_API_VERSION_MAJOR(core_api_version) != 1 || !RU_API_HAS(api, register_ru_subcommand)) return 1;
  g_api = api;
  const char* dir = api->data_dir(api->self);
  g_csgoDir = dir ? std::string(dir) : std::string();  // csgo/readyup/plugins/essentials
  while (!g_csgoDir.empty() && g_csgoDir.back() == '/') g_csgoDir.pop_back();
  for (int up = 0; up < 3 && g_csgoDir.rfind('/') != std::string::npos; ++up) g_csgoDir.erase(g_csgoDir.rfind('/'));
  const std::string dataDir = dir && *dir ? dir : ".";
  g_path = dataDir + "/admins.json";
  g_mtime = -1;
  g_nextReload = 0;
  g_nextHelpSync = 0;
  g_helpCmd = 0;
  MigrateFromMatch(dataDir);
  LoadAdmins();
  g_mapsPath = dataDir + "/default_maps.json";
  g_mapsMtime = -1;
  g_defaultMaps.clear();
  LoadDefaultMaps();
  for (const char* m : {"admins", "map"}) {
    if (!api->register_ru_subcommand(api->self, m, &OnRu, nullptr)) {
      ru_logf(api, RU_LOG_WARN, "could not register `ru %s` (another plugin owns it)", m);
    }
  }
  for (const char* m : {".map", ".reloadmap"}) {
    if (!api->register_chat_command(api->self, m, &OnMapChat, nullptr)) {
      ru_logf(api, RU_LOG_WARN, "could not register `%s` (another plugin owns it)", m);
    }
  }
  if (!api->register_chat_command(api->self, ".rcon", &OnRconChat, nullptr)) {
    ru_logf(api, RU_LOG_WARN, "could not register `.rcon` (another plugin owns it)");
  }
  api->set_admin_provider(api->self, &Provider, nullptr);
  api->subscribe(api->self, RU_EVENT_MAP_START, &OnMapStart, nullptr);
  api->on_tick(api->self, &OnTick, nullptr);
  api->provide_interface(api->self, RU_ESSENTIALS_IFACE_NAME, RU_ESSENTIALS_IFACE_VERSION,
                         const_cast<ru_essentials_v1*>(&g_iface));
  api->provide_interface(api->self, RU_SELFTEST_IFACE_PREFIX "essentials", RU_SELFTEST_IFACE_VERSION,
                         const_cast<ru_selftest_iface_v1*>(&g_selftestIface));
  {
    std::lock_guard<std::mutex> lk(g_mu);
    ru_logf(api, RU_LOG_INFO, "loaded " ESSENTIALS_VERSION ": %zu admin(s) in %s", g_admins.size(), g_path.c_str());
  }
  return 0;
}

READYUP_PLUGIN_EXPORT void readyup_plugin_unload(void) {
  ru_logf(g_api, RU_LOG_INFO, "unloaded");
  g_api = nullptr;
}

}  // extern "C"

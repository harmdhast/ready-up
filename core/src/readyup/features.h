#pragma once

// Feature gating on the engine surface.
//
// Every Ready Up feature declares the engine-surface entries and runtime facilities it needs
// (features.cpp, kFeatures). A feature is enabled only while all of them are satisfied. When a
// dependency is definitively unavailable (unresolved signature, unverified vtable slot, schema
// layout mismatch, ...), the feature disables itself with exactly ONE log line and everything
// else keeps working. Dependencies that cannot be known yet (e.g. the entity system before the
// first map) keep the feature "pending" (off, no log) until they resolve either way.

#include <string>
#include <vector>

namespace readyup {

enum class Feature : int {
  ChatCommands = 0,      // `.ru` / `.r` / player chat commands and replies
  MatchFlow,             // mode/scrim/match lifecycle tick (warmup, knife, live, cfg exec)
  Pauses,                // `.pause` / `.unpause`
  WelcomeHtml,           // per-client center HTML welcome screen
  RoundTermSuppression,  // warmup/practice TerminateRound suppression
  Events,                // engine game events (round lifecycle + stats)
  ClientCommandHook,     // console `ru`, chat on builds that send `say` to ClientCommand
  PlayerChatPrint,       // chat to a single player
  ReadyHud,              // per-player ready list / knife pick panel (center HTML)
  HudBrand,              // hud_brand / hud_logo_url header on the welcome card and ready HUD
  Knife,                 // log-driven knife round + side pick (off => knife maps go live with default sides)
  Plugins,               // plugin host (csgo/readyup/plugins/*.so), driven from GameFrame
  kCount
};

const char* FeatureName(Feature f);

// True when every dependency is satisfied. Cheap after the state settled.
bool FeatureEnabled(Feature f);

// Tri-state of one dependency ("fn:Host_Say", "vtable:ISource2Server::GameFrame", "hook:GameFrame",
// "cmdbuf", "loglistener", "schema", "entsys", "eventmgr", "layout:CCommand", "feature:skins").
struct DepStatus {
  enum class State { Ok, Pending, Fail };
  State state = State::Pending;
  std::string detail;
};
DepStatus DependencyStatus(const std::string& dep);

struct FeatureReport {
  std::string name;
  std::string state;                            // "on" | "off" | "pending"
  std::vector<std::vector<std::string>> needs;  // AND of OR-groups
  std::string missing;                          // first unsatisfied dependency (+ detail)
};
std::vector<FeatureReport> FeatureReports();

// ru_api feature_state (v1.2): a feature name ("knife") or a dependency ("fn:Host_Say", "cmdbuf",
// "events_live", ...). 1 = on / ok, 0 = pending, -1 = off / failed / unknown.
int FeatureStateByName(const std::string& name);

}  // namespace readyup

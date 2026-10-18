// The match plugin's cvar snapshot (cvar_snapshot.h): reads through ru_api cvar_query (1.11),
// persistence in the reload stash and state.json.
#include "readyup/cvar_snapshot.h"

#include "readyup/host.h"
#include "readyup/logging.h"
#include "readyup/persisted_match_state.h"
#include "readyup/plugin_api.h"

#include <cctype>
#include <cstdint>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

namespace readyup::cvar_snapshot {
namespace {

std::mutex g_mu;
Snapshot g_snap;
// Bumped whenever the snapshot is taken or discarded: an answer to a query of an older snapshot
// (still in flight) is dropped instead of starting the next one with a match value.
uint64_t g_gen = 1;

bool QueryableName(const std::string& n) {
  if (n.empty() || n.size() > 63) return false;
  // Reading a cvar makes the engine print its value to the console log: never a password
  // (tv_password; docs/design/tv-plugin.md). Such a cvar is not restored after the match.
  if (n.find("password") != std::string::npos) return false;
  for (unsigned char c : n) {
    if (!(std::isalnum(c) != 0 || c == '_' || c == '.')) return false;
  }
  return true;
}

// Must hold g_mu. The values only: a restarted server asks for the rest again.
void PersistLocked() {
  persisted_match_state::PersistCvarSnapshot(g_snap.size() > 0 ? g_snap.ToJson(false) : std::string());
}

bool HaveCvarQuery(const ru_api* a) { return a && RU_API_HAS(a, cvar_query) && a->cvar_query; }

void OnValue(void* user, const char* name, const char* value) {
  const uint64_t gen = static_cast<uint64_t>(reinterpret_cast<uintptr_t>(user));
  std::lock_guard<std::mutex> lk(g_mu);
  if (gen != g_gen || !name) return;
  if (!value) {
    Debug("match: cvar snapshot: %s unknown (not restored)\n", name);
    return;
  }
  g_snap.Record(name, value);
  Debug("match: cvar snapshot: %s = \"%s\"\n", name, value);
  PersistLocked();
}

void Query(const std::vector<std::string>& names, uint64_t gen) {
  const ru_api* a = host::Api();
  if (!HaveCvarQuery(a)) return;
  void* user = reinterpret_cast<void*>(static_cast<uintptr_t>(gen));
  for (const auto& n : names) {
    if (a->cvar_query(a->self, n.c_str(), &OnValue, user) == 1) continue;
    // No command buffer yet (early boot): ask again the next time the match cvars are applied.
    std::lock_guard<std::mutex> lk(g_mu);
    if (gen == g_gen) g_snap.Unask(n);
    Debug("match: cvar snapshot: %s not queued (retried at the next apply)\n", n.c_str());
  }
}

}  // namespace

void CaptureMatchCvars(const std::unordered_map<std::string, std::string>& cvars) {
  if (cvars.empty() || !HaveCvarQuery(host::Api())) return;
  std::vector<std::string> keys;
  for (const auto& kv : cvars) {
    if (QueryableName(kv.first)) keys.push_back(kv.first);
  }
  std::vector<std::string> names;
  uint64_t gen = 0;
  {
    std::lock_guard<std::mutex> lk(g_mu);
    names = g_snap.TakeKeysToQuery(keys);
    gen = g_gen;
  }
  if (names.empty()) return;
  host::RunOnGameThread([names = std::move(names), gen] { Query(names, gen); });
}

std::vector<std::string> TakeRestoreCommands() {
  std::lock_guard<std::mutex> lk(g_mu);
  std::vector<std::string> skipped;
  std::vector<std::string> cmds = g_snap.RestoreCommands(&skipped);
  for (const auto& n : skipped) Print("match: cvar %s not restored (its value cannot be quoted)\n", n.c_str());
  if (!cmds.empty()) Print("match: restoring %zu cvar(s) the match config changed\n", cmds.size());
  g_snap.Clear();
  ++g_gen;
  PersistLocked();
  return cmds;
}

void Discard() {
  std::lock_guard<std::mutex> lk(g_mu);
  if (g_snap.Empty()) return;
  g_snap.Clear();
  ++g_gen;
  PersistLocked();
}

std::string ReloadJson() {
  std::lock_guard<std::mutex> lk(g_mu);
  return g_snap.ToJson(true);
}

void RestoreReloadJson(const std::string& json) {
  std::lock_guard<std::mutex> lk(g_mu);
  if (g_snap.FromJson(json, true)) ++g_gen;
}

void RestorePersisted() {
  const auto json = persisted_match_state::GetCvarSnapshotJson();
  std::lock_guard<std::mutex> lk(g_mu);
  if (json && g_snap.FromJson(*json, false)) {
    ++g_gen;
    if (g_snap.size() > 0) Print("match: recovery: %zu pre-match cvar value(s) kept for the series end\n", g_snap.size());
  }
}

}  // namespace readyup::cvar_snapshot

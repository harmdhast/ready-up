#pragma once

#include <string>

namespace readyup {

// Installs a vtable hook on the server's ISource2GameClients::ClientCommand (console `ru`, chat
// handling for builds that send `say` here).
//
// The slot is verified first (engine-surface vtable_indices "ISource2GameClients::ClientCommand":
// CSource2GameClients vtable located via RTTI, slot target anchors incl. the CCommand argc/argv
// offsets, and `serverGameClientsIface`'s vptr must be that vtable). Unverified => not patched.
// Safe to call repeatedly with any interface pointer; wrong objects are ignored.
void InstallServerGameClientsHook(void* serverGameClientsIface);

// Returns true if the hook was successfully installed.
bool ServerGameClientsHookInstalled();

// True once an install attempt was rejected (slot unverified); detail says why.
bool ServerGameClientsHookFailed(std::string* detail);

// Forcing a client onto a team needs IVEngineServer::ClientCommand, which is not part of the
// verified engine surface (it used to be looked up by dladdr() symbol-name guessing, which never
// resolves on stripped Valve builds). Always returns false; callers already handle that.
bool ForceJoinTeamForSlot(int slot, int joinTeam);

}  // namespace readyup

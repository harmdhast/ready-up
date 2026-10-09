#pragma once

namespace readyup {

// Installs a detour on CS2 server Host_Say, CounterStrikeSharp-style.
// Host_Say runs the shared chat handler before the engine; ClientCommand still does for builds
// that send `say` there.
void InstallHostSayHook();

// Returns true if the Host_Say detour was successfully installed.
bool HostSayHookInstalled();

}  // namespace readyup


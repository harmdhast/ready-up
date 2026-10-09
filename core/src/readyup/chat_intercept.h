#pragma once

#include <string>

namespace readyup {

// Chat handling shared by the Host_Say detour and the ClientCommand hook: routes `.ru` and
// plugin chat commands and relays the plugin name prefix. Returns true when the engine must not
// print the line.
bool HandlePlayerChat(int slot, const std::string& msg, bool teamOnly);

}  // namespace readyup

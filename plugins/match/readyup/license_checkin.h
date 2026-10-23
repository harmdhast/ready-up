#pragma once

// The license check-in of this game server (the website's POST /api/licenses/checkin, product
// "readyup"): with a paid key set, about a minute after load and then every 15 minutes, until
// unload. It sends the key, this server's own UUID (made once, kept next to the plugins' data folders),
// its port and the Ready Up version. The license server turns UUID + port + the address it comes
// from into one server, and counts the servers seen in the last 30 minutes against the license's
// server limit: more than that only sends Auto Tournament an email. Nothing here ever stops a
// match, and a server with no internet just sends nothing.

#include <string>

#include "readyup/plugin_api.h"

namespace readyup::license_checkin {

// Sends one check-in (JSON body); each plugin passes its own HTTP client.
using PostFn = void (*)(const std::string& url, const std::string& body);

// Starts the check-in thread. `dataDir` is the plugin's own data folder; the server id and the
// lock live in the folder above it, shared by every plugin. `version` is the plugin's release.
// No-op on a core without api 1.16 license_key. The match and the fleet plugin both start it;
// only one sends at a time.
void Start(const ru_api* api, const std::string& dataDir, const std::string& version, PostFn post);

// Stops and joins the thread (plugin unload), and hands the sending to the other plugin.
void Stop();

// The license id inside a key ("ATL1.<payload>.<sig>": the payload's "id"), or "".
std::string KeyId(const std::string& token);

// The server's port from its command line (`-port N`), or 27015.
int PortFromCmdline(const std::string& cmdline);

}  // namespace readyup::license_checkin

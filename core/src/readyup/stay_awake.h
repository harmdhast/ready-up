#pragma once

#include <string>

namespace readyup {

// CS2 hibernates an empty server (sv_hibernate_when_empty, default 1): no frames run, so Ready Up
// never loads its plugins and fleet never connects until a player joins. The core keeps the
// server awake: it queues kStayAwakeCommand once at startup and again after any command text
// that turns hibernation back on.
inline constexpr const char* kStayAwakeCommand = "sv_hibernate_when_empty 0";

// True when `text` (one AddText call: lines, `;`-separated commands, optional quotes) sets
// sv_hibernate_when_empty to a non-zero value. A bare query (`sv_hibernate_when_empty`) is not.
bool TextTurnsHibernateOn(const std::string& text);

}  // namespace readyup

#pragma once

#include <string>

namespace readyup {

// Best-effort broadcast to in-game chat (server "say").
// This is intentionally defensive: if the required engine symbol isn't available,
// this becomes a no-op.
void AnnounceToChat(const char* msg);

// Same mechanism as AnnounceToChat(), but NOT gated behind Ready Up debug flags.
// Intended for actual user-facing features (e.g. `ru admins`).
void SendToChat(const char* msg);

// Sends a raw chat payload (no chat prefix such as "[Ready Up]" added).
// Payload may include CS-style color codes (best-effort).
void SendRawToChat(const char* payload);

// A line that must not be missed (RU_CHAT_IMPORTANT): the prefix, then the text in gold, sent
// three times so it fills the chat.
void SendImportantToChat(const char* msg);

// The same line (with the chat prefix) to one player only; false if the slot or the engine
// function is unavailable (callers then fall back to SendToChat).
bool SendToSlotChat(int slot, const char* msg);

}  // namespace readyup


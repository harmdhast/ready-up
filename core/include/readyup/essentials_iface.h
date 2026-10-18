#ifndef READYUP_ESSENTIALS_IFACE_H
#define READYUP_ESSENTIALS_IFACE_H
/*
 * readyup.essentials.v1: the essentials plugin (plugins/essentials, essentials.so) to other
 * plugins. The deathmatch plugin asks it for the default map of a mode (`.ru dm ffa` without a
 * map) and loads maps through it (the Workshop download bar comes with it). Published with
 *
 *   api->provide_interface(api->self, RU_ESSENTIALS_IFACE_NAME, RU_ESSENTIALS_IFACE_VERSION, &iface);
 *
 * Default maps live in csgo/readyup/plugins/essentials/default_maps.json (`ru map default <mode>
 * <map>`, or the platform's practice.set {home_map} for "practice"). A dedicated practice server
 * (practice always=1) goes back to its "practice" default map whenever nobody is on it.
 *
 * Same ABI rules as plugin_api.h: plain C, members only appended, struct_size first. Game thread.
 */
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define RU_ESSENTIALS_IFACE_NAME "readyup.essentials.v1"
#define RU_ESSENTIALS_IFACE_VERSION 1u

typedef struct ru_essentials_v1 {
  uint32_t struct_size; /* sizeof(ru_essentials_v1) of the provider */
  /* The map entry set for `mode` ([a-z0-9_]{1,32}: "ffa", "tdm", "practice", "warmup", "retakes",
   * ...): an installed map name ("de_dust2") or a Workshop entry ("workshop/<id>[/<name>]"), as
   * `ru map change` takes it. "" when none is set. Valid until the next call. Never NULL. */
  const char* (*default_map)(const char* mode);
  /* Loads a map entry (a map name, a Workshop id, ws:<id>, workshop/<id>[/<name>] or a pasted
   * Workshop link): changelevel / host_workshop_map, with the map change card and the download
   * progress bar. No mode check (the caller decides). 1 = queued, 0 = not a valid entry / the command buffer is not ready. */
  int (*load_map)(const char* entry);
  /* Appended (check struct_size): sets the default map of `mode` like `ru map default <mode>
   * <map>` (the fleet's practice.set {home_map} sets "practice"); "" clears it. 1 = stored; 0 = a
   * bad mode or map, or default_maps.json could not be saved. */
  int (*set_default_map)(const char* mode, const char* entry);
} ru_essentials_v1;

#ifdef __cplusplus
}
#endif

#endif /* READYUP_ESSENTIALS_IFACE_H */

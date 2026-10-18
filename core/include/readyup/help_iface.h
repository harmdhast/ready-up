#ifndef READYUP_HELP_IFACE_H
#define READYUP_HELP_IFACE_H
/*
 * readyup.help.<plugin>: one chat line with a plugin's chat commands, for `.help` on servers
 * without the match plugin (which answers `.help` itself). The essentials plugin asks every name
 * in RU_HELP_PLUGINS and sends the lines it gets. Published with
 *
 *   api->provide_interface(api->self, RU_HELP_IFACE_PREFIX "mapvote", RU_HELP_IFACE_VERSION, &iface);
 *
 * Same ABI rules as plugin_api.h: plain C, members only appended, struct_size first. Game thread.
 */
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define RU_HELP_IFACE_PREFIX "readyup.help."
#define RU_HELP_IFACE_VERSION 1u
/* The plugins essentials asks, in this order. */
#define RU_HELP_PLUGINS {"practice", "movement", "mapvote", "deathmatch", "community", "skins"}

typedef struct ru_help_v1 {
  uint32_t struct_size; /* sizeof(ru_help_v1) of the provider */
  /* One chat line, or NULL / "" when the plugin has nothing to offer right now (map vote with
   * no pool, practice off). Valid until the next call. */
  const char* (*line)(void);
} ru_help_v1;

#ifdef __cplusplus
}
#endif

#endif

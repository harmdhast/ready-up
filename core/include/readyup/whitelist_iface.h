#ifndef READYUP_WHITELIST_IFACE_H
#define READYUP_WHITELIST_IFACE_H
/*
 * readyup.whitelist.v1: the whitelist plugin (plugins/whitelist, whitelist.so) to other plugins.
 * The fleet link's `whitelist.set` command replaces the whole list through it. Published with
 *
 *   api->provide_interface(api->self, RU_WHITELIST_IFACE_NAME, RU_WHITELIST_IFACE_VERSION, &iface);
 *
 * Same ABI rules as plugin_api.h: plain C, members only appended, struct_size first. Game thread.
 */
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define RU_WHITELIST_IFACE_NAME "readyup.whitelist.v1"
#define RU_WHITELIST_IFACE_VERSION 1u

typedef struct ru_whitelist_v1 {
  uint32_t struct_size; /* sizeof(ru_whitelist_v1) of the provider */
  /* Replaces the state (on/off + the whole list) and saves it. 1 = saved. */
  int (*set)(int enabled, const uint64_t* steamids, uint32_t count);
  /* 1 = on. *count (if not NULL) = players on the list. */
  int (*get)(uint32_t* count);
  /* ---- appended (tv plugin casters, docs/design/tv-plugin.md). Check struct_size. ---- */
  /* Guests: players another plugin lets in while it runs (tv.so's casters). Replaces `owner`'s
   * whole list (count 0 clears it); not saved, so the owner sets it again after a whitelist reload
   * (re-setting the same list is a no-op). The whitelist never kicks a guest; the match plugin's
   * roster check lets guests in as spectators. owner = [a-z0-9_-]{1,32}, at most 256 ids.
   * 1 = stored. */
  int (*set_guests)(const char* owner, const uint64_t* steamids, uint32_t count);
  /* 1 = a guest of any owner. */
  int (*is_guest)(uint64_t steamid64);
} ru_whitelist_v1;

#ifdef __cplusplus
}
#endif

#endif /* READYUP_WHITELIST_IFACE_H */

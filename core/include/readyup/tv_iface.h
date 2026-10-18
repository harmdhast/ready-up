#ifndef READYUP_TV_IFACE_H
#define READYUP_TV_IFACE_H
/*
 * readyup.tv.v1: the tv plugin (plugins/tv, tv.so, docs/design/tv-plugin.md) to other plugins.
 * fleet.so reads the GOTV state for hello.tv / state.snapshot.tv / server.tv and runs the
 * platform's `cmd tv.set` through it. Published with
 *
 *   api->provide_interface(api->self, RU_TV_IFACE_NAME, RU_TV_IFACE_VERSION, &iface);
 *
 * Same ABI rules as plugin_api.h: plain C, members only appended, struct_size first. Game thread.
 */
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define RU_TV_IFACE_NAME "readyup.tv.v1"
#define RU_TV_IFACE_VERSION 1u

typedef struct ru_tv_v1 {
  uint32_t struct_size; /* sizeof(ru_tv_v1) of the provider */
  /* Bumped whenever state_json would return something else. Cheap: poll it. */
  uint64_t (*state_rev)(void);
  /* tvState (plugins/fleet/protocol/v1/tv.defs.json) as one JSON object. Returns its length (the
   * JSON is truncated to len-1 bytes in buf; call again with a bigger buffer). Never contains a
   * password. */
  int (*state_json)(char* buf, uint32_t len);
  /* cmd tv.set: `args_json` is the cmd's args object ({scope, settings?, casters?}). All or nothing.
   * 1 = applied (and saved for scope "server"). 0 = refused: `code` gets "bad_args" | "bad_phase" |
   * "io" and `message` why (both NUL-terminated, truncated to their lengths). */
  int (*tv_set)(const char* args_json, char* code, uint32_t code_len, char* message, uint32_t message_len);
  /* v1.x: members are appended here. */
} ru_tv_v1;

#ifdef __cplusplus
}
#endif

#endif /* READYUP_TV_IFACE_H */

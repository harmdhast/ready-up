#ifndef READYUP_PRACTICE_IFACE_H
#define READYUP_PRACTICE_IFACE_H
/*
 * readyup.practice.v1: the practice plugin (plugins/practice, practice.so) to other plugins. The
 * match plugin hands `.ru mode practice` to it and shows its help line in `.help` while practice
 * is on; fleet.so (plugins/fleet) runs the platform's practice.set with it. Published with
 *
 *   api->provide_interface(api->self, RU_PRACTICE_IFACE_NAME, RU_PRACTICE_IFACE_VERSION, &iface);
 *
 * Same ABI rules as plugin_api.h: plain C, members only appended, struct_size first. Game thread.
 */
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define RU_PRACTICE_IFACE_NAME "readyup.practice.v1"
#define RU_PRACTICE_IFACE_VERSION 1u

typedef struct ru_practice_v1 {
  uint32_t struct_size; /* sizeof(ru_practice_v1) of the provider */
  /* 1 = practice mode is on. */
  int (*active)(void);
  /* on = 1 / 0: enter / leave practice mode (cvar cfg, everyone respawned). Returns 1 on success;
   * 0 when refused (a match is loaded), and *why (if not NULL) points to a static reason. */
  int (*set_active)(int on, const char** why);
  /* One chat line with the practice commands. Static string. */
  const char* (*help)(void);
  /* Appended (check RU_API_HAS): 1 while a `.dryrun` round is under way. The match plugin lets
   * that round end (practice otherwise suppresses round termination). */
  int (*dry_run)(void);
  /* Appended (check struct_size): the always=1 setting (a dedicated practice server). set_always
   * stores it in the plugin's data dir (always.txt), where it wins over practice.cfg until it is
   * set again; on = 1 also switches practice on when nothing blocks it. Returns 1 when stored. */
  int (*always)(void);
  int (*set_always)(int on);
  /* Appended (check struct_size): settings applied whenever the server is on its home map (the
   * essentials default map for "practice"), after practice's own cfg: "sv_/mp_<name> <number>"
   * lines, at most 24 (a surf map needs sv_airaccelerate 150 ...). n = 0 clears them. Stored in
   * the data dir (home_cvars.txt). Returns 1 when stored; 0 for an invalid line or a write error. */
  int (*set_home_cvars)(const char* const* lines, uint32_t n);
} ru_practice_v1;

#ifdef __cplusplus
}
#endif

#endif /* READYUP_PRACTICE_IFACE_H */

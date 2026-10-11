#ifndef READYUP_FLEET_IFACE_H
#define READYUP_FLEET_IFACE_H
/*
 * readyup.fleet.v1: the interface the fleet plugin (plugins/fleet, fleet.so, docs/FLEET.md)
 * publishes with
 *
 *   api->provide_interface(api->self, RU_FLEET_IFACE_NAME, RU_FLEET_IFACE_VERSION, &iface);
 *
 * Two kinds of readers:
 *   - the core, for the local status endpoint (docs/FLEET.md §17, `platform` in /status). It
 *     looks it up on the game thread whenever it rebuilds the status snapshot (a few times a
 *     second) and only uses get_status. Without the interface, or when get_status returns 0
 *     (fleet.so loaded but no [fleet] url: standalone), it reports `platform: {mode: "standalone"}`.
 *   - other plugins (match, skins), through api->get_interface(). Look it up again in every
 *     callback (fleet.so can be reloaded) and check struct_size before touching a member
 *     after get_status: offsetof(ru_fleet_v1, member) + sizeof(member) <= iface->struct_size.
 *
 * Same ABI rules as plugin_api.h: plain C, members are only ever appended, every struct
 * starts with struct_size and readers check it before touching trailing members.
 * Nothing here blocks. get_status and the members marked "any thread" are thread-safe; the
 * rest are game thread only.
 *
 * Handlers: register_handler(type, fn, user) delivers platform messages of `type` ("*" = every
 * type) on the game thread. A plugin that registers handlers MUST unregister them in its
 * readyup_plugin_unload (fleet.so cannot see other plugins unload). When fleet.so itself
 * reloads every registration is gone: compare instance_id() in your callbacks and register
 * again when it changes. Reliable messages are acked to the platform after the handlers ran.
 *
 * Local pseudo-messages (never on the wire) reach handlers the same way:
 *   "local.connection"      {"state":"online"|"offline"|"rejected"|..., "since":<unix ms>}
 *   "local.offline_timeout" {"offline_s":<n>, "threshold_s":<n>}  once per offline period (D12)
 *
 * fleet.so handles "demo.ack" (demo streaming) and part of "server.config" (offline_pause_minutes,
 * status_http.token) itself; other plugins still get server.config.
 */
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define RU_FLEET_IFACE_NAME "readyup.fleet.v1"
#define RU_FLEET_IFACE_VERSION 1u

typedef enum ru_fleet_state {
  RU_FLEET_STATE_OFFLINE = 0,   /* configured, not connected (reconnecting with backoff) */
  RU_FLEET_STATE_ONLINE = 1,    /* WebSocket up, hello accepted */
  RU_FLEET_STATE_ENROLLING = 2, /* enrollment code present, no credentials yet */
  RU_FLEET_STATE_REJECTED = 3   /* credentials revoked / fenced; needs an operator */
} ru_fleet_state;

/* Finer-grained link state (ru_fleet_status.link_state, connection_state()). */
typedef enum ru_fleet_link_state {
  RU_FLEET_LINK_STANDALONE = 0, /* no [fleet] url: fleet mode off */
  RU_FLEET_LINK_UNENROLLED = 1, /* url set, no credentials and no code/key: `ru fleet enroll <code>` */
  RU_FLEET_LINK_ENROLLING = 2,
  RU_FLEET_LINK_CONNECTING = 3,
  RU_FLEET_LINK_ONLINE = 4,     /* welcome received */
  RU_FLEET_LINK_OFFLINE = 5,    /* waiting between connection attempts */
  RU_FLEET_LINK_REJECTED = 6    /* credentials or enrollment refused (4401/4403, HTTP 401/403) */
} ru_fleet_link_state;

typedef struct ru_fleet_status {
  uint32_t struct_size;    /* set by the CALLER to sizeof(ru_fleet_status); fill only what fits */
  uint32_t state;          /* ru_fleet_state */
  uint64_t since_ms;       /* unix ms of the last state change (/status reports seconds) */
  uint32_t reconnects;     /* reconnect attempts since the process started */
  uint32_t spool_msgs;     /* messages waiting in the offline spool */
  int32_t auto_pause_in_s; /* seconds until the offline auto-pause (D12); -1 = no countdown */
  int32_t update_blocked;  /* 1 = the platform still needs this server (e.g. demo not stored) */
  char server_id[64];      /* NUL-terminated; "" before enrollment */
  /* ---- appended (plugins/fleet step 1) ---- */
  uint32_t link_state;     /* ru_fleet_link_state */
  uint32_t sessions;       /* sessions established (welcome received) since fleet.so loaded */
  int64_t offline_ms;      /* how long the link has been down; 0 when online */
  int64_t tx_seq;          /* highest seq given to an outbound reliable message */
  int64_t acked_seq;       /* highest outbound seq the platform acked */
  int64_t rx_seq;          /* highest contiguous platform seq processed (what we ack) */
  uint64_t spool_bytes;
  char session_id[64];
  char last_error[192];    /* secrets redacted */
} ru_fleet_status;

/* send_event flags */
enum {
  RU_FLEET_RELIABLE = 1u << 0 /* gets a seq, is spooled to disk and replayed until acked */
};

typedef struct ru_fleet_msg {
  uint32_t struct_size;
  const char* type;         /* "match.assign", "local.connection", ... */
  const char* id;           /* envelope id (ULID); "" for local messages */
  int64_t seq;              /* 0 for ephemeral and local messages */
  int64_t epoch;            /* 0 when absent */
  int64_t ts;               /* sender clock, unix ms */
  const char* ref;          /* "" when absent */
  const char* payload_json; /* the payload object as JSON text */
} ru_fleet_msg;

/* Game thread. `msg` is valid only during the call. */
typedef void (*ru_fleet_msg_fn)(void* user, const ru_fleet_msg* msg);

typedef struct ru_fleet_v1 {
  uint32_t struct_size; /* sizeof(ru_fleet_v1) of the provider */
  /* Any thread. Fills *out (up to out->struct_size bytes). Returns 1 on success, 0 when the
   * plugin is standalone (no [fleet] url). */
  int (*get_status)(ru_fleet_status* out);
  /* ---- appended (plugins/fleet step 1) ---- */
  /* Any thread. Random per load of fleet.so. */
  uint64_t (*instance_id)(void);
  /* Any thread. ru_fleet_link_state. */
  int (*connection_state)(void);
  /*
   * Any thread. Queue a server -> platform message: `type` like "event.round_end",
   * `payload_json` a JSON object (NULL = "{}"), `epoch` 0 = none, flags RU_FLEET_*. Never
   * blocks: the network thread assigns the seq and spools it. 1 = queued, 0 = refused
   * (standalone, bad type or JSON, queue full; logged).
   */
  int (*send_event)(const char* type, const char* payload_json, int64_t epoch, uint32_t flags);
  /* Game thread. Handler for platform messages of `type` ("*" = all). 0 = failure. */
  uint64_t (*register_handler)(const char* type, ru_fleet_msg_fn fn, void* user);
  /* Game thread. 1 = removed. */
  int (*unregister_handler)(uint64_t id);
  /*
   * Any thread. The current MatchState JSON (NULL or "null" = idle) and availability
   * ("available" | "busy" | "draining" | "error"; NULL keeps the current one), used in hello
   * and state.snapshot. 1 = stored, 0 = invalid.
   */
  int (*publish_state)(const char* state_json, const char* availability);
  /* Game thread. Adds a capability (FLEET.md §14.2, e.g. "match.v1") to the next hello. */
  int (*add_capability)(const char* capability);
  /* ---- appended (fleet step 3: match state and control) ---- */
  /*
   * Any thread. send_event plus the envelope `ref` (the id of the message this answers, e.g. the
   * cmd a cmd.result acks; NULL or "" = none).
   */
  int (*send_reply)(const char* type, const char* payload_json, int64_t epoch, uint32_t flags, const char* ref);
  /*
   * Any thread. Sends state.snapshot {reason, state, availability, config_rev, admins_rev} built
   * from the last publish_state, plus the members of `extra_json` (an object, e.g. {"map_stats":..};
   * NULL = none). Ephemeral: 1 = queued, 0 = not online (nothing is spooled) or standalone.
   * The envelope epoch and config_rev are taken from the published state.
   */
  int (*send_snapshot)(const char* reason, const char* extra_json);
  /* ---- appended (admins cache version, D13) ---- */
  /*
   * Game thread. The rev of the admins.set list the server has cached (fleet-admins.json);
   * -1 = none. Sent as hello.admins_rev (left out when -1) and state.snapshot.admins_rev (0 when
   * -1), so the platform sends admins.set only when its rev differs. 1 = stored, 0 = invalid.
   */
  int (*set_admins_rev)(int64_t rev);
  /* ---- appended (demo streaming, FLEET.md §12.2) ---- */
  /*
   * Game thread. Stream a GOTV demo to the platform while it records: `spec_json` is
   * {"match_id": str, "epoch": int, "map_number": int, "path": "/abs/path/x.dem", "started_at": unix ms}.
   * fleet.so tails the file, sends it over the link at the lowest priority, resumes from the
   * platform's ack after any reconnect / reload / restart, and deletes the local file only after
   * the platform confirmed the whole file ([fleet] demo_keep_hours later; 0 = never). 1 = fleet.so
   * streams it (do not HTTP-upload it); 0 = not streamed (standalone, not enrolled, bad spec): keep
   * doing what you do without a platform. Never blocks.
   */
  int (*demo_stream_begin)(const char* spec_json);
  /* Game thread. The recording of `path` stopped (tv_stoprecord queued after the GOTV flush):
   * fleet.so sends the rest once the file stops growing, then demo.end. 1 = a known stream. */
  int (*demo_stream_end)(const char* path);
  /* ---- appended (busy reason, FLEET.md §8) ---- */
  /*
   * Game thread. The availability with why the server is busy: `reason` local_match |
   * awaiting_reset | assigned | match_disabled | draining (NULL / "" = none, the server is
   * available), `detail` the match id, local slug or drain reason (NULL / "" = null). Sent in hello
   * (reason, detail) and state.snapshot (availability_reason, availability_detail). Call it after
   * publish_state, which clears both. 1 = stored, 0 = invalid (availability not in the enum,
   * reason > 64 or detail > 128 bytes).
   */
  int (*publish_availability)(const char* availability, const char* reason, const char* detail);
  /* v1.x: members are appended here. */
} ru_fleet_v1;

#ifdef __cplusplus
}
#endif

#endif /* READYUP_FLEET_IFACE_H */

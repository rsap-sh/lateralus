#pragma once
/*
 * vc_api.h — Public API for the voicechat engine.
 *
 * Included by client.c (C) and gui.cpp (C++).
 * All symbols are plain C with extern "C" linkage in C++ translation units.
 */

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>
#include "protocol.h"   /* VC_MAX_USERNAME */

/* Thread-safe snapshot of a single peer's current state. */
typedef struct {
    char    name[VC_MAX_USERNAME];
    int     speaking;       /* non-zero while VAD detects voice  */
    int     direct_ok;      /* confirmed P2P path                */
    int     direct_known;   /* P2P punch in progress             */
    int     jb_ms;          /* jitter buffer fill  (ms)          */
    int     jb_target_ms;   /* adaptive target     (ms)          */
    int     sharing_screen; /* non-zero if peer is screen sharing */
} vc_peer_snapshot_t;

/* Mute mic input (peers are still heard). Thread-safe. */
void        vc_set_muted(int muted);
int         vc_get_muted(void);

/* Debug logging — off by default in GUI builds, on in CLI builds.
 * When off, all [room]/[audio]/[p2p]/etc. output is suppressed. */
void        vc_set_debug(int enabled);
int         vc_get_debug(void);

/* Snapshot all active peers into |out| (max |max_peers| entries).
 * Returns number written. Safe to call from any thread. */
int         vc_snapshot_peers(vc_peer_snapshot_t *out, int max_peers);

/* 1 when TCP + room join are complete. */
int         vc_is_connected(void);

/* Current room name / local username (valid when connected). */
const char *vc_room_name(void);
const char *vc_username(void);

/* Last error string set by vc_run() on failure (empty on success). */
const char *vc_last_error(void);

/* Connect, run audio/network engine, return when done.
 * Blocks the calling thread until vc_quit() or a fatal error.
 * psk may be NULL or "" to disable encryption.
 * Returns 0 on clean shutdown, 1 on error. */
int         vc_run(const char *host, const char *room,
                   const char *username, const char *psk);

/* Signal vc_run() to stop. Thread-safe. */
void        vc_quit(void);

/* Self-update: download the latest CI artifact and atomically replace
 * the running binary.  Returns 0 on success, non-zero on error.
 * Requires a GitHub token (env GITHUB_TOKEN or ~/.config/voicechat/token).
 * The artifact downloaded matches how this binary was built:
 *   VC_GUI_BUILD → voicechat-{linux,macos-arm,macos-x86,windows}
 *   otherwise    → voicechat-client-{linux,macos-arm,macos-x86,windows} */
int         do_update(void);

/* Persist a GitHub token to the config file for future --update calls. */
int         save_token(const char *token);

/* ── Screen sharing ────────────────────────────────────────────────────── */

/* Start/stop sharing the local screen. Thread-safe. */
void        vc_screen_share_start(void);
void        vc_screen_share_stop(void);
int         vc_screen_sharing(void);          /* 1 if local sharing active   */

/* Returns the latest decoded video frame from the sharing peer.
 * Caller provides a buffer; returns 0 on success, -1 if no frame.
 * frame_out must be at least width*height*4 bytes (BGRA).
 * *width_out and *height_out are set to the frame dimensions. */
int         vc_screen_frame_get(uint8_t *frame_out, int buf_size,
                                int *width_out, int *height_out,
                                uint32_t *frame_id_out);

/* Returns the client_id of the peer currently sharing, or 0 if none. */
uint32_t    vc_screen_sharer_id(void);

/* Encoder info string (e.g. "AV1 (hw)" or "VP9 (sw)"). */
const char *vc_encoder_name(void);

#ifdef __cplusplus
}
#endif

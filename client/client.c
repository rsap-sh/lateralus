/*
 * voicechat-client.c
 *
 * Low-latency voice chat client.
 *
 * Dependencies:
 *   libopus      — Opus codec
 *   portaudio    — Cross-platform audio I/O
 *   pthreads     — I/O threads
 *
 * Build (Linux/macOS):
 *   gcc -O2 -o voicechat-client client.c \
 *       $(pkg-config --cflags --libs opus portaudio-2.0) -lpthread -lm
 *
 * Build (Windows with MinGW):
 *   gcc -O2 -o voicechat-client.exe client.c \
 *       -lopus -lportaudio -lpthread -lm -lws2_32
 *
 * Usage:
 *   voicechat-client <server_host> [room] [username]
 */

#define _POSIX_C_SOURCE 200809L

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdatomic.h>
#include <errno.h>
#include <math.h>
#include <pthread.h>
#include <signal.h>
#include <time.h>

/* Platform socket headers */
#ifdef _WIN32
  #include <winsock2.h>
  #include <ws2tcpip.h>
  #pragma comment(lib, "ws2_32.lib")
  typedef SOCKET sock_t;
  #define SOCK_INVALID INVALID_SOCKET
  #define sock_close(s) closesocket(s)
  #define sock_error() WSAGetLastError()
#else
  #include <sys/socket.h>
  #include <netinet/in.h>
  #include <netinet/tcp.h>
  #include <arpa/inet.h>
  #include <netdb.h>
  #include <unistd.h>
  #include <sys/select.h>
  typedef int  sock_t;
  #define SOCK_INVALID (-1)
  #define sock_close(s) close(s)
  #define sock_error() errno
#endif

/* Cross-platform sleep compat */
#ifdef _WIN32
  #include <windows.h>
  #include <winhttp.h>
  #define usleep(us) Sleep((us) / 1000)
#else
  /* usleep was removed from POSIX 2008; provide a nanosleep-based shim */
  #include <time.h>
  static inline void vc_usleep(unsigned long us) {
      struct timespec ts = { (time_t)(us / 1000000UL),
                             (long)((us % 1000000UL) * 1000UL) };
      nanosleep(&ts, NULL);
  }
  #define usleep(us) vc_usleep(us)
#endif

#ifdef __APPLE__
  #include <mach-o/dyld.h>
#endif

#ifndef _WIN32
  #include <dirent.h>
  #include <sys/stat.h>
  #include <strings.h>  /* strncasecmp */
#endif

#include <opus/opus.h>
#include <portaudio.h>

#include "../common/protocol.h"
#include "../common/vad.h"
#include "../common/crypto.h"
#include "../common/vc_api.h"

/* Linux-specific: real-time scheduling, QoS, busy-poll */
#if defined(__linux__)
  #include <sched.h>
#endif
/* IP_TOS = 3 on every POSIX platform; define fallback for strict C99 mode
 * where macOS/BSD extensions are suppressed by _POSIX_C_SOURCE. */
#ifndef IP_TOS
#  define IP_TOS 3
#endif

/* Wall-clock milliseconds — used for latency timestamps in audio packets.
 * Both sender and receiver call this; difference = one-way latency + clock skew.
 * On a LAN with NTP, clock skew is typically < 5ms. */
static uint32_t now_ms_wall(void) {
#ifdef _WIN32
    FILETIME ft;
    GetSystemTimeAsFileTime(&ft);
    uint64_t t = ((uint64_t)ft.dwHighDateTime << 32) | ft.dwLowDateTime;
    return (uint32_t)(t / 10000); /* 100ns ticks → ms */
#else
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    return (uint32_t)((uint64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000);
#endif
}

/* ── Adaptive Jitter Buffer ─────────────────────────────────────────────── *
 * Sequence-ordered buffer of decoded PCM frames per peer.
 *
 * Design:
 *   - 16 slots indexed by sequence number — frames always play in order
 *     regardless of network arrival order (critical for cross-platform)
 *   - Adaptive target depth: measures inter-arrival jitter via EMA and
 *     sets target = 1 + ceil(2σ / 20ms), clamped to [1, 12] frames
 *     → on a clean LAN σ≈0 so target=1 frame (20ms minimum latency)
 *     → on a jittery mobile link σ≈40ms so target≈5 frames (100ms)
 *   - Fast attack (α=0.5) on jitter increases, slow decay (α=0.05) when
 *     network calms — reacts quickly, recovers slowly to avoid glitches
 *   - Gradual drain: if fill > target+1, discards one frame per 200 pops
 *     (~2s) to slowly bring level down without audible artifacts
 *   - Late packets (behind playout head) are silently discarded
 *   - Far-ahead packets reset the buffer to re-sync
 *   - Missing slots use Opus PLC (decode NULL → synthesised audio)
 * ─────────────────────────────────────────────────────────────────────── */

#define JB_FRAMES      16   /* max slots: 16 × 20ms = 320ms; halves RAM  */
#define JB_MIN_TARGET   1   /* adaptive floor: 1 frame = 20ms            */
#define JB_MAX_TARGET   8   /* adaptive ceiling: 8 frames = 160ms        */
#define JB_FRAME_MS    20   /* Opus frame duration in ms                 */
#define JB_FRAME_BYTES (OPUS_FRAME_SIZE * sizeof(int16_t))

typedef struct {
    int16_t  frames[JB_FRAMES][OPUS_FRAME_SIZE];
    uint8_t  valid[JB_FRAMES];  /* whether each slot has a decoded frame */
    uint16_t next_seq;          /* next sequence number to play out      */
    int      count;             /* number of valid frames in buffer      */
    int      started;           /* 1 once we've reached target fill      */
    int      seeded;            /* 1 once first packet sets next_seq     */
    pthread_mutex_t mu;

    /* Adaptive playout delay */
    uint32_t last_push_ms;      /* wall-clock arrival time of prev packet */
    float    jitter_ema;        /* EMA of inter-arrival deviation (ms)    */
    int      target;            /* current target depth in frames         */
    int      pop_count;         /* pops since last drain step             */
} jitter_buf_t;

static void jb_init(jitter_buf_t *jb) {
    memset(jb, 0, sizeof(*jb));
    pthread_mutex_init(&jb->mu, NULL);
    jb->target = JB_MIN_TARGET;
}

static void jb_push(jitter_buf_t *jb, uint16_t seq, const int16_t *pcm) {
    pthread_mutex_lock(&jb->mu);

    /* ── Adaptive target update ──────────────────────────────────────── */
    uint32_t now = now_ms_wall();
    if (jb->seeded && jb->last_push_ms != 0) {
        float gap = (float)((int32_t)(now - jb->last_push_ms));
        float dev = gap - (float)JB_FRAME_MS;
        if (dev < 0.0f) dev = -dev;
        /* Fast attack on spikes, slow decay when network is smooth */
        float alpha = (dev > jb->jitter_ema) ? 0.5f : 0.05f;
        jb->jitter_ema = (1.0f - alpha) * jb->jitter_ema + alpha * dev;
        /* target = 1 frame headroom + frames needed to cover 2σ of jitter */
        int t = 1 + (int)((2.0f * jb->jitter_ema) / (float)JB_FRAME_MS + 0.999f);
        if (t < JB_MIN_TARGET) t = JB_MIN_TARGET;
        if (t > JB_MAX_TARGET) t = JB_MAX_TARGET;
        jb->target = t;
    }
    jb->last_push_ms = now;

    if (!jb->seeded) {
        jb->next_seq = seq;
        jb->seeded = 1;
    }

    int16_t offset = (int16_t)(seq - jb->next_seq);

    if (offset < 0) {
        if (offset < -(int16_t)(JB_FRAMES)) {
            /* Very large negative offset: sender was absent (muted/reconnected)
             * and our playout head advanced far past their seq via PLC.
             * Resync the buffer so their resumed audio is accepted. */
            memset(jb->valid, 0, sizeof(jb->valid));
            jb->count        = 0;
            jb->started      = 0;
            jb->next_seq     = seq;
            jb->last_push_ms = 0;
            jb->jitter_ema   = 0.0f;
            offset           = 0;
            /* Fall through to push */
        } else {
            /* Slightly late — normal reorder/duplicate, discard */
            pthread_mutex_unlock(&jb->mu);
            return;
        }
    }

    if (offset >= JB_FRAMES) {
        /* Too far ahead — reset buffer to re-sync at current target */
        memset(jb->valid, 0, sizeof(jb->valid));
        jb->count    = 0;
        jb->started  = 0;
        jb->next_seq = (uint16_t)(seq - (jb->target > 1 ? jb->target - 1 : 0));
        offset = (int16_t)(seq - jb->next_seq);
    }

    int slot = seq % JB_FRAMES;
    if (!jb->valid[slot])
        jb->count++;
    memcpy(jb->frames[slot], pcm, JB_FRAME_BYTES);
    jb->valid[slot] = 1;

    if (!jb->started && jb->count >= jb->target)
        jb->started = 1;

    pthread_mutex_unlock(&jb->mu);
}

/* Fill a missed slot with a RED recovery frame.
 * Only writes if the slot is ahead of or at the current playout head AND
 * is still empty — never overwrites an already-received frame.
 * Does NOT update the jitter EMA (recovery arrivals skew timing stats). */
static void jb_push_recovery(jitter_buf_t *jb, uint16_t seq, const int16_t *pcm)
{
    pthread_mutex_lock(&jb->mu);
    if (!jb->seeded) { pthread_mutex_unlock(&jb->mu); return; }

    int16_t offset = (int16_t)(seq - jb->next_seq);
    /* Only fill slots the playout head hasn't passed yet */
    if (offset < 0 || offset >= (int16_t)JB_FRAMES) {
        pthread_mutex_unlock(&jb->mu); return;
    }
    int slot = seq % JB_FRAMES;
    if (!jb->valid[slot]) {
        memcpy(jb->frames[slot], pcm, JB_FRAME_BYTES);
        jb->valid[slot] = 1;
        jb->count++;
        if (!jb->started && jb->count >= jb->target)
            jb->started = 1;
    }
    pthread_mutex_unlock(&jb->mu);
}

/* Returns 1 if real frame popped, -1 if PLC needed, 0 if pre-fill silence */
static int jb_pop(jitter_buf_t *jb, int16_t *pcm) {
    pthread_mutex_lock(&jb->mu);

    if (!jb->started) {
        /* Still filling to target — output silence, don't touch read ptr */
        pthread_mutex_unlock(&jb->mu);
        memset(pcm, 0, JB_FRAME_BYTES);
        return 0;
    }

    /* Gradual drain: if fill is more than 1 frame over target, discard the
     * current frame every 200 pops (~2s) to slowly bring depth down.
     * One skipped frame triggers one PLC — inaudible at this rate. */
    jb->pop_count++;
    if (jb->count > jb->target + 1 && jb->pop_count >= 200) {
        jb->pop_count = 0;
        int drain_slot = jb->next_seq % JB_FRAMES;
        if (jb->valid[drain_slot]) {
            jb->valid[drain_slot] = 0;
            jb->count--;
        }
        jb->next_seq++;
        /* Fall through — pop the now-current next_seq */
    }

    int slot = jb->next_seq % JB_FRAMES;
    if (jb->valid[slot]) {
        memcpy(pcm, jb->frames[slot], JB_FRAME_BYTES);
        jb->valid[slot] = 0;
        jb->next_seq++;
        jb->count--;
        pthread_mutex_unlock(&jb->mu);
        return 1;
    }

    /* Missing frame — advance playout head, signal PLC */
    jb->next_seq++;
    pthread_mutex_unlock(&jb->mu);
    return -1;
}

/* ── Peer ───────────────────────────────────────────────────────────────── */

#define MAX_PEERS 16

typedef struct {
    int          active;
    uint32_t     id;
    char         name[VC_MAX_USERNAME];
    OpusDecoder *decoder;
    jitter_buf_t jb;
    int          speaking;       /* VAD flag from server packet */
    uint16_t     last_seq;
    int          packets_lost;
    int          packets_recv;
    int32_t      last_latency_ms; /* approx one-way net latency (may include clock skew) */

    /* P2P hole-punch: direct UDP endpoint */
    struct sockaddr_in direct_addr; /* peer's public IP:port (from server) */
    int                direct_known; /* 1 once we have their addr */
    int                direct_ok;    /* 1 once we confirmed two-way direct path */
} peer_t;

/* ── Client state ───────────────────────────────────────────────────────── */

typedef struct {
    /* Config */
    char     server_host[256];
    char     room[VC_MAX_ROOM_NAME];
    char     username[VC_MAX_USERNAME];

    /* Sockets */
    sock_t   tcp_fd;
    sock_t   udp_fd;
    struct sockaddr_in server_udp_addr;

    /* Identity */
    uint32_t client_id;
    uint16_t room_id;
    int      connected;
    int      in_room;

    /* Audio */
    PaStream    *pa_stream;
    OpusEncoder *encoder;
    vad_state_t  vad;

    /* Peers */
    peer_t   peers[MAX_PEERS];
    int      peer_count;
    pthread_mutex_t peers_lock;

    /* Sequence */
    atomic_uint_fast16_t seq;

    /* Encryption (ChaCha20-Poly1305, PSK) */
    uint8_t  enc_key[32];     /* 32-byte derived session key */
    int      enc_enabled;     /* 1 if PSK was provided */
    uint32_t enc_counter;     /* monotonic nonce counter (not the UDP seq) */

    /* Control */
    volatile int running;

    /* TCP recv buffer */
    char     tcp_buf[8192];
    int      tcp_buf_len;
} vc_client_t;

static vc_client_t g_client;
static volatile int g_quit   = 0;
static atomic_int   g_muted  = 0;   /* 1 = mic silenced, peers still heard */
static atomic_int   g_vc_ready = 0; /* 1 = peers_lock is valid, safe to snapshot */
static char         g_vc_error[256] = "";
/* Current encoder bitrate — adapted down under packet loss, back up on recovery */
static atomic_int   g_opus_bitrate = OPUS_BITRATE;

/* Debug logging — off by default in GUI builds, on for CLI */
#ifdef VC_GUI_BUILD
static atomic_int g_debug_log = 0;
#else
static atomic_int g_debug_log = 1;
#endif
#define DLOG(...) do { if (atomic_load(&g_debug_log)) printf(__VA_ARGS__); } while(0)

/* ── Peer management ────────────────────────────────────────────────────── */

static peer_t *peer_find(uint32_t id) {
    for (int i = 0; i < MAX_PEERS; i++)
        if (g_client.peers[i].active && g_client.peers[i].id == id)
            return &g_client.peers[i];
    return NULL;
}

static peer_t *peer_add(uint32_t id, const char *name) {
    for (int i = 0; i < MAX_PEERS; i++) {
        if (!g_client.peers[i].active) {
            peer_t *p = &g_client.peers[i];
            memset(p, 0, sizeof(*p));
            p->active = 1;
            p->id     = id;
            strncpy(p->name, name, VC_MAX_USERNAME - 1);

            int err;
            p->decoder = opus_decoder_create(OPUS_SAMPLE_RATE,
                                             OPUS_CHANNELS, &err);
            if (err != OPUS_OK) {
                fprintf(stderr, "[opus] decoder create failed: %s\n",
                        opus_strerror(err));
                p->active = 0;
                return NULL;
            }
            jb_init(&p->jb);
            g_client.peer_count++;
            DLOG("[room] + %s (id=%u)\n", name, id);
            return p;
        }
    }
    return NULL;
}

static void peer_remove(uint32_t id) {
    for (int i = 0; i < MAX_PEERS; i++) {
        peer_t *p = &g_client.peers[i];
        if (p->active && p->id == id) {
            DLOG("[room] - %s (id=%u) lost=%d/recv=%d\n",
                   p->name, id, p->packets_lost, p->packets_recv);
            opus_decoder_destroy(p->decoder);
            pthread_mutex_destroy(&p->jb.mu);
            p->active = 0;
            g_client.peer_count--;
            return;
        }
    }
}

/* ── PortAudio callback ─────────────────────────────────────────────────── *
 * Called by PortAudio in a real-time thread.
 * INPUT:  capture mic → VAD → Opus encode → UDP send
 * OUTPUT: mix all peer jitter buffers → playback
 * ─────────────────────────────────────────────────────────────────────── */

static int pa_callback(const void *input_buf, void *output_buf,
                       unsigned long frames,
                       const PaStreamCallbackTimeInfo *time_info,
                       PaStreamCallbackFlags status_flags,
                       void *user_data) {
    (void)time_info; (void)status_flags; (void)user_data;

    vc_client_t *vc = &g_client;
    const int16_t *in  = (const int16_t *)input_buf;
    int16_t       *out = (int16_t *)output_buf;

    /* ── OUTPUT: mix peers ─────────────────────────────────────────── */
    int32_t mix[OPUS_FRAME_SIZE] = {0};

    /* Snapshot peer state under lock, then do all audio work lock-free */
    struct { jitter_buf_t *jb; OpusDecoder *dec; } peers_snap[MAX_PEERS];
    int npeers = 0;
    pthread_mutex_lock(&vc->peers_lock);
    for (int i = 0; i < MAX_PEERS; i++) {
        if (vc->peers[i].active) {
            peers_snap[npeers].jb  = &vc->peers[i].jb;
            peers_snap[npeers].dec = vc->peers[i].decoder;
            npeers++;
        }
    }
    pthread_mutex_unlock(&vc->peers_lock);

    for (int i = 0; i < npeers; i++) {
        int16_t frame[OPUS_FRAME_SIZE];
        int r = jb_pop(peers_snap[i].jb, frame);
        if (r == 1) {
            /* Normal frame */
            for (unsigned long j = 0; j < frames; j++)
                mix[j] += frame[j];
        } else if (r == -1) {
            /* Underrun — ask Opus to conceal (synthesises from last frame) */
            int s = opus_decode(peers_snap[i].dec, NULL, 0,
                                frame, OPUS_FRAME_SIZE, 1);
            if (s > 0)
                for (unsigned long j = 0; j < frames; j++)
                    mix[j] += frame[j];
        }
        /* r == 0: pre-fill silence, add nothing */
    }

    /* Clamp mix to int16 */
    for (unsigned long j = 0; j < frames; j++) {
        int32_t s = mix[j];
        if (s >  32767) s =  32767;
        if (s < -32768) s = -32768;
        out[j] = (int16_t)s;
    }

    /* ── INPUT: encode + send inline ────────────────────────────────── *
     * Opus encode takes ~0.2ms, sendto(UDP) ~0.05ms — safe in callback. *
     * This is the only reliable way to get every frame sent on time.    */
    if (!in || !vc->connected || !vc->in_room) return paContinue;

    /* Muted: output still plays (peers heard), mic is silenced.
     * Track the previous mute state so we can detect the unmute transition
     * and reset the encoder — Opus is stateful and its predictor goes stale
     * during a long mute; resuming without a reset causes robotic glitching. */
    /* RED (Redundant Encoding) state — persist across callback invocations.
     * We keep the previous frame's encoded bytes so we can attach them to
     * the next packet.  Any isolated loss is then recovered by the receiver
     * without adding any latency (the recovery frame is already in-flight). */
    static uint8_t  s_prev_encoded[VC_OPUS_MAX_FRAME];
    static int      s_prev_encoded_len = 0;
    static uint16_t s_prev_sent_seq    = 0;

    static int prev_muted = 0;
    int now_muted = atomic_load(&g_muted);
    if (now_muted) {
        /* Advance seq even while muted so the remote jitter buffer's playout
         * head stays in sync with our sequence counter.  Without this, seq is
         * frozen during mute; on unmute the remote sees packets with seq values
         * that are far behind its current next_seq (advanced by PLC), so it
         * discards every packet and is stuck doing PLC forever (robotic audio). */
        if (vc->connected && vc->in_room)
            atomic_fetch_add(&vc->seq, 1);
        s_prev_encoded_len = 0; /* gap in transmission — no RED for next frame */
        prev_muted = 1;
        return paContinue;
    }
    if (prev_muted) {
        /* Just unmuted: clear the encoder's prediction state so the first
         * encoded frame is clean rather than a garbled continuation of
         * audio from before the mute. */
        opus_encoder_ctl(vc->encoder, OPUS_RESET_STATE);
        s_prev_encoded_len = 0; /* discard pre-mute redundant */
        prev_muted = 0;
    }

    int vad_active = vad_process(&vc->vad, in, (int)frames);

    uint8_t encoded[VC_OPUS_MAX_FRAME];
    opus_int32 enc_len = opus_encode(vc->encoder, in, OPUS_FRAME_SIZE,
                                     encoded, sizeof(encoded));
    if (enc_len <= 0) return paContinue;

    uint32_t send_ms  = now_ms_wall();
    uint16_t pkt_seq  = (uint16_t)atomic_fetch_add(&vc->seq, 1);

    /* Decide whether to attach the previous frame as RED.
     * Conditions: (a) we have a prev frame, (b) no seq gap since last send
     * (gap would mean the prev frame was muted/silent — useless redundant),
     * (c) combined payload fits inside MTU with margin. */
    int has_red = (s_prev_encoded_len > 0 &&
                   pkt_seq == (uint16_t)(s_prev_sent_seq + 1) &&
                   (size_t)(6 + enc_len + s_prev_encoded_len) < 1380u);

    /* Build plaintext payload.
     * Without RED: [4B ts][prim_opus]
     * With RED:    [4B ts][2B prim_len][prim_opus][prev_opus] */
    uint8_t plain[4 + 2 + VC_OPUS_MAX_FRAME * 2];
    size_t plain_len;
    memcpy(plain, &send_ms, 4);
    if (has_red) {
        uint16_t prim16 = (uint16_t)enc_len;
        memcpy(plain + 4, &prim16, 2);
        memcpy(plain + 6, encoded, enc_len);
        memcpy(plain + 6 + enc_len, s_prev_encoded, s_prev_encoded_len);
        plain_len = (size_t)(6 + enc_len + s_prev_encoded_len);
    } else {
        memcpy(plain + 4, encoded, enc_len);
        plain_len = (size_t)(4 + enc_len);
    }

    /* Save this frame as next packet's redundant before building the packet */
    memcpy(s_prev_encoded, encoded, enc_len);
    s_prev_encoded_len = (int)enc_len;
    s_prev_sent_seq    = pkt_seq;

    /* Packet buffer: header + plain (with RED) + encryption tag */
    uint8_t pkt[VC_HEADER_SIZE + 4 + 2 + VC_OPUS_MAX_FRAME * 2 + 16];
    vc_packet_header_t *hdr = (vc_packet_header_t *)pkt;
    hdr->magic     = VC_MAGIC;
    hdr->type      = PKT_AUDIO;
    hdr->flags     = (vad_active ? FLAG_VAD_ACTIVE : 0) |
                     (has_red    ? FLAG_HAS_REDUNDANT : 0);
    hdr->seq       = pkt_seq;
    hdr->client_id = vc->client_id;
    hdr->room_id   = vc->room_id;

    size_t total_payload;
    if (vc->enc_enabled) {
        /* [4B counter][ciphertext + 16B tag] */
        uint32_t ctr = vc->enc_counter++;
        /* Nonce: client_id(4) || counter(4) || zeros(4) */
        uint8_t nonce[12] = {0};
        memcpy(nonce,   &vc->client_id, 4);
        memcpy(nonce+4, &ctr,           4);
        memcpy(pkt + VC_HEADER_SIZE, &ctr, 4);
        chacha20poly1305_encrypt(pkt + VC_HEADER_SIZE + 4,
                                  plain, plain_len,
                                  NULL, 0,
                                  vc->enc_key, nonce);
        total_payload = 4 + plain_len + 16; /* counter + ciphertext + tag */
    } else {
        memcpy(pkt + VC_HEADER_SIZE, plain, plain_len);
        total_payload = plain_len;
    }
    hdr->payload_len = (uint16_t)total_payload;

    size_t pkt_total = VC_HEADER_SIZE + total_payload;

    /* Snapshot send targets under lock, then do all I/O lock-free.
     * Holding peers_lock across sendto() would block the RT audio thread
     * whenever the network stack schedules — even 1ms stall causes glitches. */
    typedef struct { struct sockaddr_in addr; } send_target_t;
    send_target_t targets[MAX_PEERS * 2]; /* worst case: direct + relay per peer */
    int ntargets = 0;
    int no_peers;

    pthread_mutex_lock(&vc->peers_lock);
    no_peers = (vc->peer_count == 0);
    for (int i = 0; i < MAX_PEERS; i++) {
        peer_t *p = &vc->peers[i];
        if (!p->active) continue;
        if (p->direct_ok) {
            targets[ntargets++].addr = p->direct_addr;
        } else if (p->direct_known) {
            /* Hole-punch in progress: send direct AND relay */
            targets[ntargets++].addr = p->direct_addr;
            targets[ntargets++].addr = vc->server_udp_addr;
        } else {
            targets[ntargets++].addr = vc->server_udp_addr;
        }
    }
    pthread_mutex_unlock(&vc->peers_lock);

    /* All sendto() calls happen here, outside the lock */
    if (no_peers) {
        sendto(vc->udp_fd, (char *)pkt, pkt_total, 0,
               (struct sockaddr *)&vc->server_udp_addr,
               sizeof(vc->server_udp_addr));
    }
    for (int i = 0; i < ntargets; i++) {
        sendto(vc->udp_fd, (char *)pkt, pkt_total, 0,
               (struct sockaddr *)&targets[i].addr,
               sizeof(targets[i].addr));
    }

    static int dbg_sent = 0;
    if (dbg_sent < 5) {
        DLOG("[audio] sent pkt seq=%u len=%zu vad=%d enc=%d\n",
               pkt_seq, pkt_total, vad_active, vc->enc_enabled);
        dbg_sent++;
    }

    return paContinue;
}

/* ── UDP receive thread ─────────────────────────────────────────────────── */

static void *udp_recv_thread(void *arg) {
    (void)arg;
    vc_client_t *vc = &g_client;

    uint8_t buf[VC_MAX_PACKET + 16]; /* +16 for possible Poly1305 tag */
    while (vc->running) {
        struct timeval tv = { .tv_sec = 0, .tv_usec = 5000 };  /* 5ms — keeps recv latency low */
        fd_set rfds;
        FD_ZERO(&rfds);
        FD_SET(vc->udp_fd, &rfds);
        int r = select((int)vc->udp_fd + 1, &rfds, NULL, NULL, &tv);
        if (r <= 0) continue;

        /* recvfrom so we know the source address for P2P detection */
        struct sockaddr_in src_addr;
        socklen_t src_len = sizeof(src_addr);
        ssize_t n = recvfrom(vc->udp_fd, (char *)buf, sizeof(buf), 0,
                             (struct sockaddr *)&src_addr, &src_len);
        if (n < (ssize_t)VC_HEADER_SIZE) continue;

        const vc_packet_header_t *hdr = (const vc_packet_header_t *)buf;
        if (hdr->magic != VC_MAGIC) continue;

        /* PKT_KEEPALIVE arriving from a peer's direct addr confirms P2P path.
         * This is the primary hole-punch signal — must NOT be dropped. */
        if (hdr->type == PKT_KEEPALIVE) {
            pthread_mutex_lock(&vc->peers_lock);
            peer_t *kp = peer_find(hdr->client_id);
            if (kp && kp->direct_known && !kp->direct_ok) {
                if (src_addr.sin_addr.s_addr == kp->direct_addr.sin_addr.s_addr &&
                    src_addr.sin_port        == kp->direct_addr.sin_port) {
                    kp->direct_ok = 1;
                    DLOG("[p2p] direct path confirmed with %s (keepalive)\n",
                           kp->name);
                }
            }
            pthread_mutex_unlock(&vc->peers_lock);
            continue;
        }

        if (hdr->type != PKT_AUDIO) continue;

        uint32_t sender_id   = hdr->client_id;
        uint16_t payload_len = hdr->payload_len;
        const uint8_t *payload = buf + VC_HEADER_SIZE;

        if (payload_len < 5) continue;
        if (n < (ssize_t)(VC_HEADER_SIZE + payload_len)) continue;

        /* Decrypt if encryption enabled.
         * Encrypted payload layout: [4B counter][ciphertext + 16B tag]
         * Plain payload layout:     [4B timestamp][Opus data]           */
        uint8_t plain[4 + VC_OPUS_MAX_FRAME];
        size_t  plain_len;
        if (vc->enc_enabled) {
            if (payload_len < 4 + 16 + 5) continue; /* min: ctr+tag+ts+1opus */
            uint32_t ctr;
            memcpy(&ctr, payload, 4);
            /* Nonce: sender_id(4) || counter(4) || zeros(4) */
            uint8_t nonce[12] = {0};
            memcpy(nonce,   &sender_id, 4);
            memcpy(nonce+4, &ctr,       4);
            size_t ct_with_tag = (size_t)(payload_len - 4);
            if (ct_with_tag < 16 + 5) continue;
            plain_len = ct_with_tag - 16;
            if (chacha20poly1305_decrypt(plain, payload + 4, ct_with_tag,
                                         NULL, 0, vc->enc_key, nonce) != 0) {
                /* Tag mismatch — drop packet */
                continue;
            }
        } else {
            /* Unencrypted */
            if (payload_len > (uint16_t)(4 + VC_OPUS_MAX_FRAME)) continue;
            plain_len = payload_len;
            memcpy(plain, payload, plain_len);
        }

        /* Parse plaintext payload.
         *   Normal:  [4B ts][prim_opus]
         *   With RED: [4B ts][2B prim_len][prim_opus][red_opus]       */
        if (plain_len < 5) continue;
        uint32_t send_ms;
        memcpy(&send_ms, plain, 4);
        int32_t latency_ms = (int32_t)(now_ms_wall() - send_ms);

        int            has_red     = (hdr->flags & FLAG_HAS_REDUNDANT) != 0;
        const uint8_t *opus_payload;
        uint16_t       opus_len;
        const uint8_t *red_payload  = NULL;
        uint16_t       red_len      = 0;

        if (has_red && plain_len >= 4 + 2) {
            uint16_t prim_len;
            memcpy(&prim_len, plain + 4, 2);
            if ((size_t)(4 + 2 + prim_len) <= plain_len) {
                opus_payload = plain + 6;
                opus_len     = prim_len;
                red_payload  = plain + 6 + prim_len;
                red_len      = (uint16_t)(plain_len - 6 - prim_len);
            } else {
                /* Malformed RED — treat as plain */
                opus_payload = plain + 4;
                opus_len     = (uint16_t)(plain_len - 4);
            }
        } else {
            opus_payload = plain + 4;
            opus_len     = (uint16_t)(plain_len - 4);
        }

        pthread_mutex_lock(&vc->peers_lock);
        peer_t *p = peer_find(sender_id);
        if (!p) {
            pthread_mutex_unlock(&vc->peers_lock);
            continue;
        }

        /* P2P: if packet arrived from peer's known direct addr, confirm path */
        if (p->direct_known && !p->direct_ok) {
            if (src_addr.sin_addr.s_addr == p->direct_addr.sin_addr.s_addr &&
                src_addr.sin_port        == p->direct_addr.sin_port) {
                p->direct_ok = 1;
                DLOG("[p2p] direct path confirmed with %s\n", p->name);
            }
        }

        if (p->packets_recv > 0) {
            int32_t delta = (int32_t)(uint16_t)(hdr->seq - p->last_seq - 1);
            if (delta > 0 && delta < 1000)
                p->packets_lost += delta;
        }
        p->last_seq        = hdr->seq;
        p->packets_recv++;
        p->speaking        = (hdr->flags & FLAG_VAD_ACTIVE) != 0;
        p->last_latency_ms = latency_ms;

        OpusDecoder  *decoder = p->decoder;
        jitter_buf_t *jb      = &p->jb;
        pthread_mutex_unlock(&vc->peers_lock);

        /* Decode RED recovery frame FIRST (uses decoder state before primary).
         * Decoding in sender-chronological order (N-1 then N) keeps the Opus
         * predictor consistent and gives the best concealment quality.
         * On clean links the slot is already valid; skip the decode entirely
         * to save ~0.1ms of Opus work per speaker per packet. */
        if (red_payload && red_len > 0) {
            uint16_t red_seq = hdr->seq - 1;
            /* Benign racy read: valid[] only ever transitions 0→1, and a
             * false 0 just causes a harmless redundant decode. */
            if (!jb->valid[red_seq % JB_FRAMES]) {
                int16_t red_pcm[OPUS_FRAME_SIZE];
                int red_samples = opus_decode(decoder, red_payload, red_len,
                                              red_pcm, OPUS_FRAME_SIZE, 0);
                if (red_samples > 0)
                    jb_push_recovery(jb, red_seq, red_pcm);
            }
        }

        int16_t pcm[OPUS_FRAME_SIZE];
        int samples = opus_decode(decoder, opus_payload, opus_len,
                                  pcm, OPUS_FRAME_SIZE, 0);
        if (samples > 0) {
            jb_push(jb, hdr->seq, pcm);
            static int dbg_recv = 0;
            if (dbg_recv < 5) {
                DLOG("[audio] recv pkt from peer %u seq=%u samples=%d red=%d\n",
                       sender_id, hdr->seq, samples, has_red);
                dbg_recv++;
            }
        }
    }
    return NULL;
}

/* ── TCP signaling ──────────────────────────────────────────────────────── */

static void tcp_send_str(const char *msg) {
    size_t total = strlen(msg);
    size_t sent  = 0;
    while (sent < total) {
        ssize_t n = send(g_client.tcp_fd, msg + sent, total - sent, 0);
        if (n <= 0) {
            fprintf(stderr, "[tcp] send error: %s\n", strerror(errno));
            break;
        }
        sent += (size_t)n;
    }
}

static void handle_server_message(const char *msg) {
    vc_client_t *vc = &g_client;
    char op[32] = {0};

    /* Extract op */
    char *op_start = strstr(msg, "\"op\":\"");
    if (!op_start) return;
    op_start += 6;
    int i = 0;
    while (*op_start && *op_start != '"' && i < 31)
        op[i++] = *op_start++;

    if (strcmp(op, "hello") == 0) {
        /* Extract client_id */
        char *p = strstr(msg, "\"client_id\":");
        if (p) vc->client_id = (uint32_t)atol(p + 12);
        DLOG("[server] assigned id=%u\n", vc->client_id);
        vc->connected = 1;

        /* Send UDP registration packet */
        uint8_t pkt[VC_HEADER_SIZE];
        vc_packet_header_t *hdr = (vc_packet_header_t *)pkt;
        hdr->magic       = VC_MAGIC;
        hdr->type        = PKT_REGISTER;
        hdr->flags       = 0;
        hdr->seq         = 0;
        hdr->client_id   = vc->client_id;
        hdr->room_id     = 0;
        hdr->payload_len = 0;
        sendto(vc->udp_fd, (char *)pkt, VC_HEADER_SIZE, 0,
               (struct sockaddr *)&vc->server_udp_addr,
               sizeof(vc->server_udp_addr));

        /* Join room */
        char join_msg[512];
        snprintf(join_msg, sizeof(join_msg),
                 "{\"op\":\"join\",\"room\":\"%s\",\"username\":\"%s\"}\n",
                 vc->room, vc->username);
        tcp_send_str(join_msg);

    } else if (strcmp(op, "ok") == 0) {
        char *p = strstr(msg, "\"room_id\":");
        if (p) vc->room_id = (uint16_t)atoi(p + 10);

        /* Parse initial peer list */
        pthread_mutex_lock(&vc->peers_lock);
        char *peers = strstr(msg, "\"peers\":[");
        if (peers) {
            peers += 9;
            while (*peers && *peers != ']') {
                char *id_p  = strstr(peers, "\"id\":");
                char *name_p = strstr(peers, "\"name\":\"");
                if (!id_p || !name_p) break;

                uint32_t pid = (uint32_t)atol(id_p + 5);
                name_p += 8;
                char pname[VC_MAX_USERNAME] = {0};
                int j = 0;
                while (*name_p && *name_p != '"' && j < (int)sizeof(pname)-1)
                    pname[j++] = *name_p++;

                peer_add(pid, pname);

                char *next = strchr(name_p, '}');
                if (!next) break;
                peers = next + 1;
            }
        }
        pthread_mutex_unlock(&vc->peers_lock);

        vc->in_room = 1;
        DLOG("[room] joined '%s' (room_id=%u)\n", vc->room, vc->room_id);

        /* Update UDP registration with room */
        uint8_t pkt[VC_HEADER_SIZE];
        vc_packet_header_t *hdr = (vc_packet_header_t *)pkt;
        hdr->magic       = VC_MAGIC;
        hdr->type        = PKT_REGISTER;
        hdr->flags       = 0;
        hdr->seq         = 0;
        hdr->client_id   = vc->client_id;
        hdr->room_id     = vc->room_id;
        hdr->payload_len = 0;
        sendto(vc->udp_fd, (char *)pkt, VC_HEADER_SIZE, 0,
               (struct sockaddr *)&vc->server_udp_addr,
               sizeof(vc->server_udp_addr));

    } else if (strcmp(op, "joined") == 0) {
        char *id_p   = strstr(msg, "\"id\":");
        char *name_p = strstr(msg, "\"name\":\"");
        if (id_p && name_p) {
            uint32_t pid = (uint32_t)atol(id_p + 5);
            name_p += 8;
            char pname[VC_MAX_USERNAME] = {0};
            int j = 0;
            while (*name_p && *name_p != '"' && j < (int)sizeof(pname)-1)
                pname[j++] = *name_p++;
            pthread_mutex_lock(&vc->peers_lock);
            peer_add(pid, pname);
            pthread_mutex_unlock(&vc->peers_lock);
        }
    } else if (strcmp(op, "left") == 0) {
        char *id_p = strstr(msg, "\"id\":");
        if (id_p) {
            uint32_t pid = (uint32_t)atol(id_p + 5);
            pthread_mutex_lock(&vc->peers_lock);
            peer_remove(pid);
            pthread_mutex_unlock(&vc->peers_lock);
        }
    } else if (strcmp(op, "peer_addr") == 0) {
        /* Server telling us a peer's public UDP endpoint for P2P hole-punch.
         * Format: {"op":"peer_addr","id":X,"addr":"1.2.3.4","port":Y}      */
        char *id_p   = strstr(msg, "\"id\":");
        char *addr_p = strstr(msg, "\"addr\":\"");
        char *port_p = strstr(msg, "\"port\":");
        if (id_p && addr_p && port_p) {
            uint32_t pid  = (uint32_t)atol(id_p + 5);
            addr_p += 8;
            char addr_str[64] = {0};
            int j = 0;
            while (*addr_p && *addr_p != '"' && j < 63)
                addr_str[j++] = *addr_p++;
            uint16_t port = (uint16_t)atoi(port_p + 7);

            pthread_mutex_lock(&vc->peers_lock);
            peer_t *p = peer_find(pid);
            if (p && port > 0 && addr_str[0]) {
                memset(&p->direct_addr, 0, sizeof(p->direct_addr));
                p->direct_addr.sin_family = AF_INET;
                p->direct_addr.sin_port   = htons(port);
                inet_pton(AF_INET, addr_str, &p->direct_addr.sin_addr);
                p->direct_known = 1;
                p->direct_ok    = 0;
                DLOG("[p2p] peer %s endpoint: %s:%u — punching hole...\n",
                       p->name, addr_str, port);
                /* Send 3 punch packets immediately to open our NAT */
                uint8_t punch[VC_HEADER_SIZE];
                vc_packet_header_t *ph = (vc_packet_header_t *)punch;
                ph->magic       = VC_MAGIC;
                ph->type        = PKT_KEEPALIVE;
                ph->flags       = 0;
                ph->seq         = 0;
                ph->client_id   = vc->client_id;
                ph->room_id     = vc->room_id;
                ph->payload_len = 0;
                for (int k = 0; k < 3; k++)
                    sendto(vc->udp_fd, (char *)punch, VC_HEADER_SIZE, 0,
                           (struct sockaddr *)&p->direct_addr,
                           sizeof(p->direct_addr));
            }
            pthread_mutex_unlock(&vc->peers_lock);
        }
    } else if (strcmp(op, "rooms") == 0) {
        DLOG("[rooms] %s\n", msg);
    } else if (strcmp(op, "error") == 0) {
        fprintf(stderr, "[error] %s\n", msg);
    }
}

static void *tcp_recv_thread(void *arg) {
    (void)arg;
    vc_client_t *vc = &g_client;

    while (vc->running) {
        int space = (int)sizeof(vc->tcp_buf) - vc->tcp_buf_len - 1;
        if (space <= 0) { vc->tcp_buf_len = 0; continue; }

        struct timeval tv = { .tv_sec = 0, .tv_usec = 20000 };
        fd_set rfds;
        FD_ZERO(&rfds);
        FD_SET(vc->tcp_fd, &rfds);
        int r = select((int)vc->tcp_fd + 1, &rfds, NULL, NULL, &tv);
        if (r <= 0) continue;

        ssize_t n = recv(vc->tcp_fd, vc->tcp_buf + vc->tcp_buf_len, space, 0);
        if (n <= 0) {
            fprintf(stderr, "[tcp] disconnected\n");
            vc->running = 0;
            break;
        }
        vc->tcp_buf_len += (int)n;
        vc->tcp_buf[vc->tcp_buf_len] = '\0';

        char *start = vc->tcp_buf;
        char *nl;
        while ((nl = strchr(start, '\n')) != NULL) {
            *nl = '\0';
            if (nl > start) handle_server_message(start);
            start = nl + 1;
        }

        int remaining = (int)(vc->tcp_buf + vc->tcp_buf_len - start);
        if (remaining > 0 && start != vc->tcp_buf)
            memmove(vc->tcp_buf, start, remaining);
        vc->tcp_buf_len = remaining;
    }
    g_quit = 1;
    return NULL;
}

/* ── Keep-alive thread ──────────────────────────────────────────────────── */

static void *keepalive_thread(void *arg) {
    (void)arg;
    vc_client_t *vc = &g_client;
    /* Rolling loss measurement: snapshot peer counters each cycle, compute delta */
    int last_recv[MAX_PEERS] = {0};
    int last_lost[MAX_PEERS] = {0};
    int ka_cycle = 0;

    while (vc->running) {
        /* Break sleep into 100ms chunks so we notice running=0 quickly */
        for (int i = 0; i < 50 && vc->running; i++)
            usleep(100000);
        if (!vc->running) break;
        if (!vc->connected) continue;

        uint8_t pkt[VC_HEADER_SIZE];
        vc_packet_header_t *hdr = (vc_packet_header_t *)pkt;
        hdr->magic       = VC_MAGIC;
        hdr->type        = PKT_KEEPALIVE;
        hdr->flags       = 0;
        hdr->seq         = (uint16_t)atomic_fetch_add(&vc->seq, 1);
        hdr->client_id   = vc->client_id;
        hdr->room_id     = vc->room_id;
        hdr->payload_len = 0;
        /* Always keep server hole open */
        sendto(vc->udp_fd, (char *)pkt, VC_HEADER_SIZE, 0,
               (struct sockaddr *)&vc->server_udp_addr,
               sizeof(vc->server_udp_addr));
        /* Also punch toward every known peer to keep NAT pinholes open */
        pthread_mutex_lock(&vc->peers_lock);
        for (int i = 0; i < MAX_PEERS; i++) {
            peer_t *p = &vc->peers[i];
            if (p->active && p->direct_known)
                sendto(vc->udp_fd, (char *)pkt, VC_HEADER_SIZE, 0,
                       (struct sockaddr *)&p->direct_addr,
                       sizeof(p->direct_addr));
        }

        /* Every 2 keepalive cycles (~10s): measure packet loss and tune FEC.
         * Opus INBAND_FEC adds redundancy proportional to the loss estimate —
         * on a clean LAN this stays near 2%; on a mobile hotspot it climbs
         * automatically so the decoder can conceal more losses. */
        ka_cycle++;
        if (ka_cycle % 2 == 0 && vc->encoder) {
            int total_exp = 0, total_rcv = 0;
            for (int i = 0; i < MAX_PEERS; i++) {
                peer_t *p = &vc->peers[i];
                if (p->active) {
                    int dr = p->packets_recv - last_recv[i];
                    int dl = p->packets_lost - last_lost[i];
                    if (dr >= 0 && dl >= 0) {
                        total_rcv += dr;
                        total_exp += dr + dl;
                    }
                    last_recv[i] = p->packets_recv;
                    last_lost[i] = p->packets_lost;
                } else {
                    last_recv[i] = 0;
                    last_lost[i] = 0;
                }
            }
            if (total_exp >= 10) {
                int loss_pct = (int)(100 * (total_exp - total_rcv) / total_exp);
                if (loss_pct < 2)  loss_pct = 2;
                if (loss_pct > 30) loss_pct = 30;
                opus_encoder_ctl(vc->encoder, OPUS_SET_PACKET_LOSS_PERC(loss_pct));

                /* Adaptive bitrate: back off under sustained loss to ease
                 * congestion, recover toward the target ceiling once clear.
                 * 12kbps floor keeps voice intelligible; 32kbps ceiling = target. */
                int cur_br  = atomic_load(&g_opus_bitrate);
                int next_br = cur_br;
                if (loss_pct > 8) {
                    /* Reduce 25% per measurement window while lossy */
                    next_br = cur_br * 3 / 4;
                    if (next_br < 12000) next_br = 12000;
                } else if (loss_pct < 3) {
                    /* Recover 10% per window when network clears */
                    next_br = cur_br + cur_br / 10;
                    if (next_br > OPUS_BITRATE) next_br = OPUS_BITRATE;
                }
                if (next_br != cur_br) {
                    atomic_store(&g_opus_bitrate, next_br);
                    opus_encoder_ctl(vc->encoder, OPUS_SET_BITRATE(next_br));
                }
                DLOG("[audio] loss=%d%% FEC=%d%% bitrate=%dkbps\n",
                     loss_pct, loss_pct, atomic_load(&g_opus_bitrate) / 1000);
            }
        }
        pthread_mutex_unlock(&vc->peers_lock);

        tcp_send_str("{\"op\":\"ping\"}\n");
    }
    return NULL;
}

/* ── Status display thread ──────────────────────────────────────────────── */

static void *status_thread(void *arg) {
    (void)arg;
    vc_client_t *vc = &g_client;
    while (vc->running) {
        /* Break sleep into 100ms chunks so we notice running=0 quickly */
        for (int i = 0; i < 20 && vc->running; i++)
            usleep(100000);
        if (!vc->running) break;
        if (!vc->in_room) continue;

        printf("\r[peers: %d]", vc->peer_count);
        pthread_mutex_lock(&vc->peers_lock);
        for (int i = 0; i < MAX_PEERS; i++) {
            peer_t *p = &vc->peers[i];
            if (p->active) {
                int jb_ms     = p->jb.count  * JB_FRAME_MS;
                int jb_tgt_ms = p->jb.target * JB_FRAME_MS;
                int net_ms    = p->last_latency_ms;
                const char *path = p->direct_ok    ? "[P2P]"
                                 : p->direct_known ? "[relay→P2P]"
                                 : "[relay]";
                /* net_ms is one-way from embedded wall-clock timestamps.
                 * Only meaningful when both hosts have NTP-synced clocks.
                 * Reject values outside 0-2000ms as clock-skew garbage. */
                if (net_ms >= 0 && net_ms < 2000)
                    printf("  %s%s %s net~%dms jb=%d/%dms total~%dms",
                           p->name, p->speaking ? "*" : "",
                           path, net_ms, jb_ms, jb_tgt_ms, net_ms + jb_tgt_ms);
                else
                    printf("  %s%s %s jb=%d/%dms (sync clocks for net latency)",
                           p->name, p->speaking ? "*" : "",
                           path, jb_ms, jb_tgt_ms);
            }
        }
        pthread_mutex_unlock(&vc->peers_lock);
        fflush(stdout);
    }
    return NULL;
}

/* ── Self-update ────────────────────────────────────────────────────────── *
 * Downloads the latest GitHub Actions artifact for this platform and
 * atomically replaces the running binary.
 *
 * Uses only the C standard library + platform sockets (already linked).
 * No libcurl dependency — raw TLS would require a library, so we shell out
 * to curl (macOS/Linux, always present) or PowerShell Invoke-WebRequest
 * (Windows, always present since Win8).
 *
 * Flow:
 *   1. GET /repos/rohanverma2007/lateralus/actions/runs?status=success
 *   2. Parse run id from JSON (manual, no cjson dep)
 *   3. GET /actions/runs/{id}/artifacts → parse artifact id
 *   4. Download zip → extract binary → atomic replace
 * ─────────────────────────────────────────────────────────────────────── */

#define UPDATE_REPO   "rohanverma2007/lateralus"
#define UPDATE_API    "https://api.github.com/repos/" UPDATE_REPO

/* Artifact names must match the CI upload names in build.yml.
 * macOS matrix labels are "arm" (arm64) and "x86" (x86_64).
 * GUI build (VC_GUI_BUILD) targets the "voicechat-*" artifacts;
 * CLI build targets the "voicechat-client-*" artifacts.
 * The single Windows artifact "voicechat-windows" contains both .exe files. */
#ifdef VC_GUI_BUILD
  #ifdef __APPLE__
    #if defined(__arm64__) || defined(__aarch64__)
      #define UPDATE_ARTIFACT "voicechat-macos-arm"
    #else
      #define UPDATE_ARTIFACT "voicechat-macos-x86"
    #endif
    #define UPDATE_BINARY   "voicechat"
  #elif defined(_WIN32)
    #define UPDATE_ARTIFACT "voicechat-windows"
    #define UPDATE_BINARY   "voicechat.exe"
  #else
    #define UPDATE_ARTIFACT "voicechat-linux"
    #define UPDATE_BINARY   "voicechat"
  #endif
#else /* CLI binary */
  #ifdef __APPLE__
    #if defined(__arm64__) || defined(__aarch64__)
      #define UPDATE_ARTIFACT "voicechat-client-macos-arm"
    #else
      #define UPDATE_ARTIFACT "voicechat-client-macos-x86"
    #endif
    #define UPDATE_BINARY   "voicechat-client"
  #elif defined(_WIN32)
    #define UPDATE_ARTIFACT "voicechat-windows"
    #define UPDATE_BINARY   "voicechat-client.exe"
  #else
    #define UPDATE_ARTIFACT "voicechat-client-linux"
    #define UPDATE_BINARY   "voicechat-client"
  #endif
#endif

/* Pull the first occurrence of a JSON value for a given key — used on
 * Windows path where PowerShell returns the id directly as plain text. */

/* Run a shell command, capture stdout into buf (up to bufsz-1 bytes).
 * Returns 0 on success. */
static int run_capture(const char *cmd, char *buf, size_t bufsz) {
#ifdef _WIN32
    FILE *f = _popen(cmd, "r");
#else
    FILE *f = popen(cmd, "r");
#endif
    if (!f) return -1;
    size_t n = fread(buf, 1, bufsz - 1, f);
    buf[n] = '\0';
#ifdef _WIN32
    int rc = _pclose(f);
#else
    int rc = pclose(f);
#endif
    return rc;
}

/* Run a command, discard output, return exit code. (Unix only) */
#ifndef _WIN32
static int run_silent(const char *cmd) {
    char buf[16];
    return run_capture(cmd, buf, sizeof(buf));
}
#endif

/* ── Self-update helpers ────────────────────────────────────────────────── */

#ifdef _WIN32
#include <winhttp.h>
/* MinGW WinHTTP is Unicode-only — no URL_COMPONENTSA exists.
 * Parse URLs manually: all GitHub API URLs are simple https://host/path */

/* Parse "https://host/path?query" into host and path buffers. */
static void url_parse(const char *url, char *host, size_t hsz,
                      char *path, size_t psz) {
    host[0] = path[0] = '\0';
    /* Skip scheme */
    const char *p = strstr(url, "://");
    if (!p) return;
    p += 3;
    /* host ends at first '/' */
    const char *slash = strchr(p, '/');
    if (!slash) {
        strncpy(host, p, hsz - 1);
        strncpy(path, "/", psz - 1);
    } else {
        size_t hlen = (size_t)(slash - p);
        if (hlen >= hsz) hlen = hsz - 1;
        memcpy(host, p, hlen); host[hlen] = '\0';
        strncpy(path, slash, psz - 1);
    }
    path[psz - 1] = '\0';
}

/* Open a WinHTTP request to host/path, adding auth + GitHub headers.
 * Returns request handle or NULL. Caller owns con and ses. */
static HINTERNET wh_open_req(HINTERNET ses, const char *host, const char *path,
                              const char *auth_token, const char *accept,
                              HINTERNET *out_con) {
    wchar_t whost[256] = {0};
    MultiByteToWideChar(CP_UTF8, 0, host, -1, whost, 256);

    HINTERNET con = WinHttpConnect(ses, whost,
                                   INTERNET_DEFAULT_HTTPS_PORT, 0);
    if (!con) return NULL;
    *out_con = con;

    wchar_t wpath[2048] = {0};
    MultiByteToWideChar(CP_UTF8, 0, path, -1, wpath, 2048);

    HINTERNET req = WinHttpOpenRequest(con, L"GET", wpath, NULL,
                                       WINHTTP_NO_REFERER,
                                       WINHTTP_DEFAULT_ACCEPT_TYPES,
                                       WINHTTP_FLAG_SECURE);
    if (!req) { WinHttpCloseHandle(con); return NULL; }

    /* Build Accept header */
    char achdr[256];
    snprintf(achdr, sizeof(achdr), "Accept: %s\r\n",
             accept ? accept : "application/vnd.github+json");
    wchar_t wachdr[256] = {0};
    MultiByteToWideChar(CP_UTF8, 0, achdr, -1, wachdr, 256);
    WinHttpAddRequestHeaders(req, wachdr, (DWORD)-1, WINHTTP_ADDREQ_FLAG_ADD);
    WinHttpAddRequestHeaders(req,
        L"X-GitHub-Api-Version: 2022-11-28\r\n",
        (DWORD)-1, WINHTTP_ADDREQ_FLAG_ADD);

    if (auth_token && auth_token[0]) {
        char hdr8[600];
        snprintf(hdr8, sizeof(hdr8),
                 "Authorization: Bearer %s\r\n", auth_token);
        wchar_t whdr[600] = {0};
        MultiByteToWideChar(CP_UTF8, 0, hdr8, -1, whdr, 600);
        WinHttpAddRequestHeaders(req, whdr, (DWORD)-1, WINHTTP_ADDREQ_FLAG_ADD);
    }
    return req;
}

/* Read entire response body into malloc'd buffer. Sets *out_len. */
static char *wh_read_body(HINTERNET req, size_t *out_len) {
    size_t total = 0, cap = 65536;
    char *buf = (char *)malloc(cap);
    if (!buf) return NULL;
    DWORD avail = 0, nread = 0;
    while (WinHttpQueryDataAvailable(req, &avail) && avail > 0) {
        if (total + avail + 1 > cap) {
            cap = (total + avail + 1) * 2;
            char *nb = (char *)realloc(buf, cap);
            if (!nb) { free(buf); return NULL; }
            buf = nb;
        }
        WinHttpReadData(req, buf + total, avail, &nread);
        total += nread;
    }
    buf[total] = '\0';
    *out_len = total;
    return buf;
}

/* HTTPS GET url with auth. Follows one redirect (GitHub → S3 for zips).
 * Returns malloc'd body or NULL. */
static char *winhttp_get(const char *url, const char *auth_token,
                         const char *accept, size_t *out_len) {
    *out_len = 0;

    HINTERNET ses = WinHttpOpen(L"voicechat-updater/1.0",
                                WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
                                WINHTTP_NO_PROXY_NAME,
                                WINHTTP_NO_PROXY_BYPASS, 0);
    if (!ses) return NULL;

    /* Disable automatic redirects — we handle them manually so we can
     * strip the Authorization header before following to S3 */
    DWORD redirPolicy = WINHTTP_OPTION_REDIRECT_POLICY_NEVER;
    WinHttpSetOption(ses, WINHTTP_OPTION_REDIRECT_POLICY,
                     &redirPolicy, sizeof(redirPolicy));

    char host[256], path[2048];
    url_parse(url, host, sizeof(host), path, sizeof(path));

    HINTERNET con = NULL;
    HINTERNET req = wh_open_req(ses, host, path, auth_token, accept, &con);
    if (!req) { WinHttpCloseHandle(ses); return NULL; }

    char *result = NULL;

    for (int hop = 0; hop < 3; hop++) {
        if (!WinHttpSendRequest(req, WINHTTP_NO_ADDITIONAL_HEADERS, 0,
                                WINHTTP_NO_REQUEST_DATA, 0, 0, 0)) {
            fprintf(stderr, "[http] WinHttpSendRequest failed: %lu\n",
                    GetLastError());
            break;
        }
        if (!WinHttpReceiveResponse(req, NULL)) {
            fprintf(stderr, "[http] WinHttpReceiveResponse failed: %lu\n",
                    GetLastError());
            break;
        }

        DWORD status = 0, slen = sizeof(status);
        WinHttpQueryHeaders(req,
            WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
            NULL, &status, &slen, NULL);
        fprintf(stderr, "[http] status=%lu url=%.80s\n", status, url);

        if (status == 301 || status == 302 || status == 307 || status == 308) {
            /* Get Location header */
            wchar_t wloc[2048] = {0}; DWORD loclen = sizeof(wloc);
            WinHttpQueryHeaders(req, WINHTTP_QUERY_LOCATION,
                                NULL, wloc, &loclen, NULL);
            WinHttpCloseHandle(req);
            WinHttpCloseHandle(con);

            /* Convert to UTF-8 and re-parse */
            char loc[2048] = {0};
            WideCharToMultiByte(CP_UTF8, 0, wloc, -1,
                                loc, sizeof(loc), NULL, NULL);
            fprintf(stderr, "[http] redirect -> %.80s\n", loc);
            char h2[256], p2[2048];
            url_parse(loc, h2, sizeof(h2), p2, sizeof(p2));

            /* No auth on S3 redirect, but keep accept type */
            con = NULL;
            req = wh_open_req(ses, h2, p2, NULL, accept, &con);
            if (!req) break;
            continue;
        }

        if (status == 200)
            result = wh_read_body(req, out_len);
        else {
            /* Read error body for debugging */
            size_t elen = 0;
            char *errbody = wh_read_body(req, &elen);
            fprintf(stderr, "[http] unexpected status %lu\n", status);
            if (errbody && elen > 0)
                fprintf(stderr, "[http] response: %.200s\n", errbody);
            free(errbody);
        }
        break;
    }

    if (req) WinHttpCloseHandle(req);
    if (con) WinHttpCloseHandle(con);
    WinHttpCloseHandle(ses);
    return result;
}

/* Write bytes to file, returns 0 on success */
static int write_file(const char *path, const char *data, size_t len) {
    FILE *f = fopen(path, "wb");
    if (!f) return -1;
    size_t w = fwrite(data, 1, len, f);
    fclose(f);
    return (w == len) ? 0 : -1;
}
#endif /* _WIN32 */

/* Simple JSON: find first numeric value after "key": in haystack */
static int json_num(const char *hay, const char *key, char *out, size_t outsz) {
    const char *p = strstr(hay, key);
    if (!p) return 0;
    p += strlen(key);
    while (*p == ' ' || *p == ':' || *p == '"') p++;
    size_t i = 0;
    while (*p >= '0' && *p <= '9' && i < outsz-1) out[i++] = *p++;
    out[i] = '\0';
    return i > 0;
}

/* Simple JSON: find first string value after "key": in haystack.
 * Handles one level of backslash escaping. Returns 1 on success. */
static int json_str(const char *hay, const char *key, char *out, size_t outsz) {
    char needle[128];
    snprintf(needle, sizeof(needle), "\"%s\"", key);
    const char *p = strstr(hay, needle);
    if (!p) return 0;
    p += strlen(needle);
    while (*p == ' ' || *p == ':') p++;
    if (*p != '"') return 0;
    p++;
    size_t i = 0;
    while (*p && *p != '"' && i < outsz - 1) {
        if (*p == '\\') { p++; if (!*p) break; }
        out[i++] = *p++;
    }
    out[i] = '\0';
    return i > 0;
}

#ifndef _WIN32
/* Read a file into a malloc'd, NUL-terminated buffer. Sets *len. Caller frees. */
static char *read_file_alloc(const char *path, size_t *len) {
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (sz <= 0) { fclose(f); return NULL; }
    char *buf = (char *)malloc((size_t)sz + 1);
    if (!buf) { fclose(f); return NULL; }
    *len = fread(buf, 1, (size_t)sz, f);
    buf[*len] = '\0';
    fclose(f);
    return buf;
}

/* Scan a JSON array for the first object that has field==value (and
 * optionally field2==value2), then extract "id" as a decimal string.
 * Objects are delimited by top-level '{' / '}' pairs.
 * Returns 1 on success. */
static int json_find_obj_id(const char *json,
                             const char *key1, const char *val1,
                             const char *key2, const char *val2,
                             char *id_out, size_t id_outsz)
{
    const char *p = json;
    while ((p = strchr(p, '{')) != NULL) {
        /* find matching '}' tracking nesting depth */
        int depth = 1;
        const char *start = p++;
        while (*p && depth > 0) {
            if (*p == '{') { depth++; p++; }
            else if (*p == '}') { depth--; p++; }
            else if (*p == '"') {
                p++;
                while (*p && *p != '"') { if (*p == '\\') p++; if (*p) p++; }
                if (*p) p++;
            } else { p++; }
        }
        if (depth != 0) break;

        size_t obj_len = (size_t)(p - start);
        char *obj = (char *)malloc(obj_len + 1);
        if (!obj) break;
        memcpy(obj, start, obj_len);
        obj[obj_len] = '\0';

        char got1[128] = {0};
        int m1 = json_str(obj, key1, got1, sizeof(got1)) && strcmp(got1, val1) == 0;
        int m2 = 1;
        if (key2) {
            char got2[128] = {0};
            m2 = json_str(obj, key2, got2, sizeof(got2)) && strcmp(got2, val2) == 0;
        }
        if (m1 && m2) {
            int ok = json_num(obj, "\"id\"", id_out, id_outsz);
            free(obj);
            return ok;
        }
        free(obj);
    }
    return 0;
}
#endif /* !_WIN32 */

/* ── Token config file ──────────────────────────────────────────────────── *
 * Stored at:                                                                *
 *   Windows : %APPDATA%\voicechat\token                                     *
 *   Unix    : ~/.config/voicechat/token                                     *
 * ─────────────────────────────────────────────────────────────────────── */
static void config_path(char *out, size_t sz) {
#ifdef _WIN32
    char *appdata = getenv("APPDATA");
    if (appdata && appdata[0])
        snprintf(out, sz, "%s\\voicechat\\token", appdata);
    else
        snprintf(out, sz, "voicechat_token.txt");
#else
    char *home = getenv("HOME");
    if (home && home[0])
        snprintf(out, sz, "%s/.config/voicechat/token", home);
    else
        snprintf(out, sz, ".voicechat_token");
#endif
}

/* Save token to config file. Returns 0 on success. */
int save_token(const char *token) {
    char path[512];
    config_path(path, sizeof(path));

    /* mkdir -p the directory */
#ifdef _WIN32
    char dir[512]; strncpy(dir, path, sizeof(dir)-1);
    char *last = strrchr(dir, '\\');
    if (last) { *last = '\0'; CreateDirectoryA(dir, NULL); }
#else
    char dir[512]; strncpy(dir, path, sizeof(dir)-1);
    char *last = strrchr(dir, '/');
    if (last) {
        *last = '\0';
        char cmd[600]; snprintf(cmd, sizeof(cmd), "mkdir -p '%s'", dir);
        (void)system(cmd);
    }
#endif

    FILE *f = fopen(path, "w");
    if (!f) {
        fprintf(stderr, "[token] failed to write %s: %s\n", path, strerror(errno));
        return -1;
    }
    fprintf(f, "%s\n", token);
    fclose(f);
    DLOG("[token] saved to %s\n", path);
    return 0;
}

/* Load token from config file into buf. Returns buf on success, NULL if not found. */
static char *load_token(char *buf, size_t sz) {
    char path[512];
    config_path(path, sizeof(path));
    FILE *f = fopen(path, "r");
    if (!f) return NULL;
    if (!fgets(buf, (int)sz, f)) { fclose(f); return NULL; }
    fclose(f);
    /* Strip all trailing whitespace */
    size_t n = strlen(buf);
    while (n > 0 && (buf[n-1] == '\r' || buf[n-1] == '\n' ||
                     buf[n-1] == ' '  || buf[n-1] == '\t'))
        buf[--n] = '\0';
    return buf[0] ? buf : NULL;
}

int do_update(void) {
    /* Token priority: env var → config file */
    char token_buf[256] = {0};
    const char *token = getenv("GITHUB_TOKEN");
    if (!token || !token[0])
        token = load_token(token_buf, sizeof(token_buf));

    if (!token || !token[0]) {
        fprintf(stderr,
            "[update] No GitHub token found.\n"
            "  Set it once with: --token <your_token>\n"
            "  Or set env var:   GITHUB_TOKEN=ghp_...\n"
            "  Token needs: Actions=Read (+ Contents=Read for private repos)\n"
            "  Create one at: https://github.com/settings/tokens\n");
        return 1;
    }

    DLOG("[update] fetching latest build info...\n");

#ifdef _WIN32
    /* ── Windows: use WinHTTP directly — no PowerShell quoting hell ── */
    fprintf(stderr, "[update] token source: %s, prefix: %.12s... len=%zu\n",
            getenv("GITHUB_TOKEN") ? "env" : "config file",
            token, strlen(token));
    fprintf(stderr, "[update] hitting: %s\n",
            UPDATE_API "/actions/runs?status=success&per_page=10");
    size_t len = 0;
    char *runs_json = winhttp_get(
        UPDATE_API "/actions/runs?status=success&per_page=10", token,
        "application/vnd.github+json", &len);
    if (!runs_json || len == 0) {
        fprintf(stderr, "[update] failed to fetch run list\n");
        free(runs_json);
        return 1;
    }

    char run_id[64] = {0};
    /* Each run object starts with "id": followed by "head_branch":"main" within ~200 bytes */
    const char *p = runs_json;
    while ((p = strstr(p, "\"id\":")) != NULL) {
        char candidate[64] = {0};
        json_num(p, "\"id\":", candidate, sizeof(candidate));
        /* Check if head_branch=main appears within next 300 bytes */
        char window[301] = {0};
        size_t wlen = strlen(p);
        if (wlen > 300) wlen = 300;
        memcpy(window, p, wlen);
        if (strstr(window, "\"head_branch\"") && strstr(window, "\"main\"")) {
            strncpy(run_id, candidate, sizeof(run_id) - 1);
            break;
        }
        p++;
    }
    free(runs_json);

    if (!run_id[0]) {
        fprintf(stderr, "[update] no successful run found on main branch\n");
        return 1;
    }
    DLOG("[update] latest run id: %s\n", run_id);

    /* ── Fetch artifacts ─────────────────────────────────────────── */
    char art_url[512];
    snprintf(art_url, sizeof(art_url),
             UPDATE_API "/actions/runs/%s/artifacts", run_id);
    char *art_json = winhttp_get(art_url, token, "application/vnd.github+json", &len);
    if (!art_json || len == 0) {
        fprintf(stderr, "[update] failed to fetch artifact list\n");
        free(art_json);
        return 1;
    }

    char artifact_id[64] = {0};
    /* GitHub artifact JSON: {"id":NNN,...,"name":"TARGET","workflow_run":{"id":RUN,...}}
     * Find "name":"TARGET", scan BACKWARDS tracking brace depth.
     * The artifact's own "id" is at depth 0; nested ids (workflow_run) are depth>0. */
    p = art_json;
    char name_needle[128];
    snprintf(name_needle, sizeof(name_needle), "\"name\":\"%s\"", UPDATE_ARTIFACT);
    fprintf(stderr, "[update] searching for: %s\n", name_needle);
    const char *namepos = strstr(p, name_needle);
    if (namepos) {
        const char *scan = namepos - 1;
        int depth = 0;
        while (scan >= art_json) {
            if (*scan == '}') depth++;
            else if (*scan == '{') { if (depth > 0) depth--; }
            if (depth == 0 && strncmp(scan, "\"id\":", 5) == 0) {
                json_num(scan, "\"id\":", artifact_id, sizeof(artifact_id));
                break;
            }
            scan--;
        }
    }
    fprintf(stderr, "[update] found artifact id: %s\n", artifact_id);
    free(art_json);

    if (!artifact_id[0]) {
        fprintf(stderr, "[update] artifact '%s' not found\n", UPDATE_ARTIFACT);
        return 1;
    }
    DLOG("[update] artifact id: %s\n", artifact_id);

    /* ── Download zip ─────────────────────────────────────────────── */
    char dl_url[512];
    snprintf(dl_url, sizeof(dl_url),
             UPDATE_API "/actions/artifacts/%s/zip", artifact_id);

    DLOG("[update] downloading...\n");
    char *zip_data = winhttp_get(dl_url, token, "application/vnd.github+json", &len);
    if (!zip_data || len < 4 ||
        (unsigned char)zip_data[0] != 0x50 ||
        (unsigned char)zip_data[1] != 0x4B) {
        fprintf(stderr, "[update] download failed or not a zip\n");
        free(zip_data);
        return 1;
    }

    char tmpdir[MAX_PATH];
    GetTempPathA(sizeof(tmpdir), tmpdir);
    char zippath[MAX_PATH], extdir[MAX_PATH];
    snprintf(zippath, sizeof(zippath), "%svoicechat-update.zip", tmpdir);
    snprintf(extdir,  sizeof(extdir),  "%svoicechat-update",    tmpdir);

    if (write_file(zippath, zip_data, len) != 0) {
        fprintf(stderr, "[update] failed to write zip\n");
        free(zip_data);
        return 1;
    }
    free(zip_data);
    DLOG("[update] saved zip (%zu bytes)\n", len);

    /* ── Extract via PowerShell (only used for unzip, no auth needed) */
    char cmd[1024];
    snprintf(cmd, sizeof(cmd),
        "powershell -NoProfile -Command "
        "\"Remove-Item -Recurse -Force '%s' -ErrorAction SilentlyContinue; "
        "Expand-Archive -Path '%s' -DestinationPath '%s' -Force\"",
        extdir, zippath, extdir);
    if (system(cmd) != 0) {
        fprintf(stderr, "[update] extraction failed\n");
        return 1;
    }

    /* ── Find binary ──────────────────────────────────────────────── */
    char newbin[MAX_PATH];
    snprintf(newbin, sizeof(newbin), "%s\\%s", extdir, UPDATE_BINARY);
    if (GetFileAttributesA(newbin) == INVALID_FILE_ATTRIBUTES) {
        /* Try one level deeper */
        snprintf(cmd, sizeof(cmd),
            "powershell -NoProfile -Command "
            "\"(Get-ChildItem -Path '%s' -Filter '%s' -Recurse "
            "| Select-Object -First 1).FullName\"",
            extdir, UPDATE_BINARY);
        char found[MAX_PATH] = {0};
        FILE *f = _popen(cmd, "r");
        if (f) { fgets(found, sizeof(found), f); _pclose(f); }
        found[strcspn(found, "\r\n")] = '\0';
        if (found[0]) strncpy(newbin, found, MAX_PATH-1);
    }
    if (GetFileAttributesA(newbin) == INVALID_FILE_ATTRIBUTES) {
        fprintf(stderr, "[update] binary not found in artifact\n");
        return 1;
    }
    DLOG("[update] found: %s\n", newbin);

    /* ── Atomic replace ───────────────────────────────────────────── */
    char self[MAX_PATH];
    GetModuleFileNameA(NULL, self, sizeof(self));
    char bakpath[MAX_PATH];
    snprintf(bakpath, sizeof(bakpath), "%s.bak", self);

    /* Strategy: rename running exe → .bak (Windows allows renaming a running
     * exe), then copy new exe into the original name. The running process
     * keeps using the renamed .bak file descriptor until it exits. */
    DeleteFileA(bakpath); /* remove old backup if exists */
    if (!MoveFileExA(self, bakpath, MOVEFILE_REPLACE_EXISTING)) {
        fprintf(stderr, "[update] failed to rename current exe (err=%lu)\n",
                GetLastError());
        return 1;
    }
    if (!CopyFileA(newbin, self, FALSE)) {
        /* Restore on failure */
        MoveFileExA(bakpath, self, MOVEFILE_REPLACE_EXISTING);
        fprintf(stderr, "[update] failed to copy new exe (err=%lu)\n",
                GetLastError());
        return 1;
    }
    DLOG("[update] done! Old binary saved as %s.bak\n", bakpath);
    DLOG("[update] Updated successfully — close and reopen to use new version.\n");
    return 0;

#else
    /* ── Unix: curl is always present on macOS, usually on Linux ─── *
     * JSON parsed in pure C — no python3/jq dependency required.    */
    char cmd[2048];

    /* ── Step 1: Find latest successful run on main branch ────────── */
    DLOG("[update] fetching run list...\n");
    snprintf(cmd, sizeof(cmd),
        "curl -sf "
        "-H 'Accept: application/vnd.github+json' "
        "-H 'Authorization: Bearer %s' "
        "-H 'X-GitHub-Api-Version: 2022-11-28' "
        "-H 'User-Agent: voicechat-updater' "
        "'%s/actions/runs?branch=main&status=success&per_page=10' "
        "-o /tmp/voicechat-runs.json",
        token, UPDATE_API);
    if (run_silent(cmd) != 0) {
        fprintf(stderr,
            "[update] failed to fetch run list.\n"
            "  Check your token has Actions=Read scope.\n");
        return 1;
    }
    size_t runs_len = 0;
    char *runs_json = read_file_alloc("/tmp/voicechat-runs.json", &runs_len);
    if (!runs_json || runs_len == 0) {
        fprintf(stderr, "[update] empty response for run list\n");
        free(runs_json);
        return 1;
    }
    char run_id[64] = {0};
    if (!json_find_obj_id(runs_json, "head_branch", "main",
                          "conclusion", "success", run_id, sizeof(run_id))) {
        fprintf(stderr,
            "[update] no successful run found on main branch\n"
            "  Response snippet: %.200s\n", runs_json);
        free(runs_json);
        return 1;
    }
    free(runs_json);
    DLOG("[update] latest successful run: %s\n", run_id);

    /* ── Step 2: Find the right artifact for this platform ────────── */
    snprintf(cmd, sizeof(cmd),
        "curl -sf "
        "-H 'Accept: application/vnd.github+json' "
        "-H 'Authorization: Bearer %s' "
        "-H 'X-GitHub-Api-Version: 2022-11-28' "
        "-H 'User-Agent: voicechat-updater' "
        "'%s/actions/runs/%s/artifacts?per_page=30' "
        "-o /tmp/voicechat-artifacts.json",
        token, UPDATE_API, run_id);
    if (run_silent(cmd) != 0) {
        fprintf(stderr, "[update] failed to fetch artifact list\n");
        return 1;
    }
    size_t arts_len = 0;
    char *arts_json = read_file_alloc("/tmp/voicechat-artifacts.json", &arts_len);
    if (!arts_json || arts_len == 0) {
        fprintf(stderr, "[update] empty artifact list response\n");
        free(arts_json);
        return 1;
    }
    /* Locate "name":"<TARGET>" then scan backwards to its object's "id" field.
     * json_find_obj_id only matches the outermost {…} which contains every
     * artifact — using strstr it would match the FIRST artifact's name only.
     * The backward-scan approach correctly locates any artifact by name. */
    char artifact_id[64] = {0};
    {
        char name_needle[128];
        snprintf(name_needle, sizeof(name_needle),
                 "\"name\":\"%s\"", UPDATE_ARTIFACT);
        const char *nm = strstr(arts_json, name_needle);
        if (!nm) {
            fprintf(stderr, "[update] artifact '%s' not found in run %s\n",
                    UPDATE_ARTIFACT, run_id);
            free(arts_json);
            return 1;
        }
        /* Scan backward from the name match to the enclosing object's "id": */
        int depth = 0;
        const char *scan = nm;
        while (scan > arts_json) {
            scan--;
            if (*scan == '}') depth++;
            else if (*scan == '{') { if (depth > 0) depth--; }
            if (depth == 0 && strncmp(scan, "\"id\":", 5) == 0) {
                json_num(scan, "\"id\":", artifact_id, sizeof(artifact_id));
                break;
            }
        }
    }
    free(arts_json);
    if (!artifact_id[0]) {
        fprintf(stderr, "[update] could not extract id for artifact '%s'\n",
                UPDATE_ARTIFACT);
        return 1;
    }
    DLOG("[update] artifact id: %s  (%s)\n", artifact_id, UPDATE_ARTIFACT);

    /* ── Step 3: Download the artifact zip ────────────────────────── *
     * GitHub redirects to a pre-signed S3 URL.  curl -L would forward *
     * the Authorization header to S3 which returns 400 "only one auth  *
     * mechanism allowed".  Fix: capture the redirect Location header   *
     * first, then download from S3 without auth.                       */
    const char zippath[] = "/tmp/voicechat-update.zip";
    const char extdir[]  = "/tmp/voicechat-update";

    DLOG("[update] fetching download URL...\n");
    snprintf(cmd, sizeof(cmd),
        "curl -sf "
        "-H 'Accept: application/vnd.github+json' "
        "-H 'Authorization: Bearer %s' "
        "-H 'X-GitHub-Api-Version: 2022-11-28' "
        "-H 'User-Agent: voicechat-updater' "
        "-D /tmp/voicechat-dl-headers.txt "
        "-o /dev/null "
        "'%s/actions/artifacts/%s/zip'; true",
        token, UPDATE_API, artifact_id);
    run_silent(cmd);

    /* Extract Location header value — pure C, no grep/sed */
    char s3url[2048] = {0};
    {
        size_t hdr_len = 0;
        char *hdr_buf = read_file_alloc("/tmp/voicechat-dl-headers.txt", &hdr_len);
        if (hdr_buf) {
            /* Scan for last "Location:" line (case-insensitive prefix) */
            char *line = hdr_buf;
            while (*line) {
                char *eol = line + strcspn(line, "\r\n");
                if ((eol - line) > 9 &&
                    (line[0]=='L'||line[0]=='l') &&
                    (line[1]=='o'||line[1]=='O') &&
                    strncasecmp(line, "location:", 9) == 0) {
                    char *val = line + 9;
                    while (*val == ' ' || *val == '\t') val++;
                    size_t vlen = (size_t)(eol - val);
                    if (vlen > 0 && vlen < sizeof(s3url)) {
                        memcpy(s3url, val, vlen);
                        s3url[vlen] = '\0';
                    }
                }
                /* advance past line ending */
                while (*eol == '\r' || *eol == '\n') eol++;
                line = eol;
            }
            free(hdr_buf);
        }
    }

    DLOG("[update] downloading from S3...\n");
    if (s3url[0]) {
        /* Download pre-signed S3 URL — no auth header, just follow redirects */
        snprintf(cmd, sizeof(cmd), "curl -fL '%s' -o '%s'", s3url, zippath);
    } else {
        /* Fallback: try direct download with auth (works for some token types) */
        fprintf(stderr, "[update] no redirect URL found, trying direct download...\n");
        snprintf(cmd, sizeof(cmd),
            "curl -fL "
            "-H 'Accept: application/vnd.github+json' "
            "-H 'Authorization: Bearer %s' "
            "-H 'X-GitHub-Api-Version: 2022-11-28' "
            "-H 'User-Agent: voicechat-updater' "
            "'%s/actions/artifacts/%s/zip' -o '%s'",
            token, UPDATE_API, artifact_id, zippath);
    }
    if (run_silent(cmd) != 0) {
        fprintf(stderr, "[update] download failed\n");
        return 1;
    }

    /* ── Step 4: Extract ──────────────────────────────────────────── */
#ifdef __APPLE__
    snprintf(cmd, sizeof(cmd),
        "rm -rf '%s' && ditto -xk '%s' '%s'", extdir, zippath, extdir);
#else
    snprintf(cmd, sizeof(cmd),
        "rm -rf '%s' && unzip -qo '%s' -d '%s'", extdir, zippath, extdir);
#endif
    if (run_silent(cmd) != 0) {
        fprintf(stderr, "[update] extraction failed\n");
        return 1;
    }

    /* ── Step 5: Find the binary inside the extracted dir ─────────── *
     * Pure C: check direct path, then scan one level of subdirectory  *
     * (GitHub artifacts are often wrapped in a single subdirectory).  */
    char newbin[512] = {0};
    {
        struct stat st;
        /* Try 1: extdir/UPDATE_BINARY */
        snprintf(newbin, sizeof(newbin), "%s/%s", extdir, UPDATE_BINARY);
        if (stat(newbin, &st) != 0 || !S_ISREG(st.st_mode)) {
            newbin[0] = '\0';
#ifdef __APPLE__
            /* Try 2: extdir/Lateralus.app/Contents/MacOS/UPDATE_BINARY
             * (artifact uploaded as dist/ preserving the .app bundle tree) */
            snprintf(newbin, sizeof(newbin),
                     "%s/Lateralus.app/Contents/MacOS/%s", extdir, UPDATE_BINARY);
            if (stat(newbin, &st) != 0 || !S_ISREG(st.st_mode))
                newbin[0] = '\0';
#endif
        }
        if (!newbin[0]) {
            /* Try 3: extdir/<subdir>/UPDATE_BINARY (one-level wrap) */
            DIR *d = opendir(extdir);
            if (d) {
                struct dirent *ent;
                while ((ent = readdir(d)) != NULL) {
                    if (ent->d_name[0] == '.') continue;
                    char cand[512];
                    snprintf(cand, sizeof(cand), "%s/%s/%s",
                             extdir, ent->d_name, UPDATE_BINARY);
                    if (stat(cand, &st) == 0 && S_ISREG(st.st_mode)) {
                        strncpy(newbin, cand, sizeof(newbin) - 1);
                        break;
                    }
                }
                closedir(d);
            }
        }
    }
    if (!newbin[0]) {
        fprintf(stderr, "[update] '%s' not found inside artifact\n", UPDATE_BINARY);
        return 1;
    }
    DLOG("[update] found binary: %s\n", newbin);

    /* ── Step 6: Resolve own path ─────────────────────────────────── */
    char self[1024] = {0};
  #ifdef __APPLE__
    uint32_t sz = sizeof(self); _NSGetExecutablePath(self, &sz);
  #else
    ssize_t rlen = readlink("/proc/self/exe", self, sizeof(self)-1);
    if (rlen > 0) self[rlen] = '\0';
  #endif
    if (!self[0]) { fprintf(stderr, "[update] could not resolve own path\n"); return 1; }

    /* ── Step 7: Strip quarantine from extracted binary (macOS) ────── */
  #ifdef __APPLE__
    snprintf(cmd, sizeof(cmd),
        "xattr -cr '%s' 2>/dev/null; true", newbin);
    run_silent(cmd);
  #endif

    /* ── Step 8: Atomic replace ───────────────────────────────────── */
    char tmp[1100]; snprintf(tmp, sizeof(tmp), "%s.new", self);
    char bak[1100]; snprintf(bak, sizeof(bak), "%s.bak", self);

    snprintf(cmd, sizeof(cmd), "cp '%s' '%s' && chmod +x '%s'", newbin, tmp, tmp);
    if (run_silent(cmd) != 0) { fprintf(stderr, "[update] copy failed\n"); return 1; }

    /* Backup old binary */
    snprintf(cmd, sizeof(cmd), "cp '%s' '%s'", self, bak);
    run_silent(cmd);

    /* rename() is atomic on the same filesystem */
    if (rename(tmp, self) != 0) { perror("[update] rename failed"); return 1; }

  #ifdef __APPLE__
    /* If running inside a .app bundle, use the bundle as the target for
     * quarantine removal and Gatekeeper registration (required for bundles). */
    char gk_target[1100];
    strncpy(gk_target, self, sizeof(gk_target) - 1);
    gk_target[sizeof(gk_target) - 1] = '\0';
    {
        char *dot_app = strstr(gk_target, ".app/");
        if (dot_app) dot_app[4] = '\0'; /* truncate to "…/Lateralus.app" */
    }

    snprintf(cmd, sizeof(cmd),
        "xattr -rd com.apple.quarantine '%s' 2>/dev/null; true", gk_target);
    run_silent(cmd);

    /* Register with Gatekeeper so it doesn't block the updated binary.
     * spctl --add requires admin rights; osascript shows the native
     * macOS password prompt so the user never has to touch System Settings. */
    snprintf(cmd, sizeof(cmd),
        "osascript -e 'do shell script \"spctl --add \\\"%s\\\"\" "
        "with prompt \"Allow Lateralus to run after update?\" "
        "with administrator privileges' 2>/dev/null; true",
        gk_target);
    run_silent(cmd);
  #endif

    /* Cleanup temp files */
    snprintf(cmd, sizeof(cmd), "rm -f '%s' '%s' /tmp/voicechat-dl-headers.txt", zippath, tmp);
    run_silent(cmd);

    DLOG("[update] done! Old binary saved as %s\n", bak);
    DLOG("[update] Restart to use the new version.\n");
    return 0;
#endif /* !_WIN32 */
}

/* ── Main ───────────────────────────────────────────────────────────────── */

static void handle_signal(int s) { (void)s; g_quit = 1; }

/* ── Engine API ─────────────────────────────────────────────────────────── */

void vc_set_muted(int m)   { atomic_store(&g_muted, m); }
int  vc_get_muted(void)    { return atomic_load(&g_muted); }
void vc_set_debug(int v)   { atomic_store(&g_debug_log, v); }
int  vc_get_debug(void)    { return atomic_load(&g_debug_log); }
void vc_quit(void)         { g_quit = 1; }
int  vc_is_connected(void) { return g_client.connected && g_client.in_room; }
const char *vc_room_name(void)  { return g_client.room; }
const char *vc_username(void)   { return g_client.username; }
const char *vc_last_error(void) { return g_vc_error; }

int vc_snapshot_peers(vc_peer_snapshot_t *out, int max)
{
    if (!atomic_load(&g_vc_ready)) return 0;
    int n = 0;
    pthread_mutex_lock(&g_client.peers_lock);
    for (int i = 0; i < MAX_PEERS && n < max; i++) {
        peer_t *p = &g_client.peers[i];
        if (!p->active) continue;
        snprintf(out[n].name, VC_MAX_USERNAME, "%s", p->name);
        out[n].speaking     = p->speaking;
        out[n].direct_ok    = p->direct_ok;
        out[n].direct_known = p->direct_known;
        out[n].jb_ms        = p->jb.count  * JB_FRAME_MS;
        out[n].jb_target_ms = p->jb.target * JB_FRAME_MS;
        n++;
    }
    pthread_mutex_unlock(&g_client.peers_lock);
    return n;
}

/* Connect, run audio/network engine, return when done.
 * Blocks the calling thread. Called from main() (CLI) or a background
 * thread (GUI via gui.cpp). May be called multiple times sequentially. */
int vc_run(const char *host, const char *room,
           const char *username, const char *psk)
{
    /* Reset volatile state before anything so the GUI never sees stale data */
    g_quit             = 0;
    g_client.connected = 0;
    g_client.in_room   = 0;
    atomic_store(&g_vc_ready, 0);
    atomic_store(&g_muted,    0);
    g_vc_error[0] = '\0';

    signal(SIGINT,  handle_signal);
    signal(SIGTERM, handle_signal);
#ifndef _WIN32
    signal(SIGPIPE, SIG_IGN);
#else
    SetConsoleOutputCP(65001);
#endif

#ifdef _WIN32
    WSADATA wsa;
    WSAStartup(MAKEWORD(2, 2), &wsa);
#endif

    vc_client_t *vc = &g_client;
    memset(vc, 0, sizeof(*vc));

    strncpy(vc->server_host, host,     sizeof(vc->server_host) - 1);
    strncpy(vc->room,        room,     sizeof(vc->room) - 1);
    strncpy(vc->username,    username, sizeof(vc->username) - 1);

    pthread_mutex_init(&vc->peers_lock, NULL);
    atomic_init(&vc->seq, 0);
    vad_init(&vc->vad);
    vc->running     = 1;
    vc->enc_counter = 0;

    /* Mutex is live — GUI can now safely call vc_snapshot_peers() */
    atomic_store(&g_vc_ready, 1);

    /* Encryption setup */
    if (psk && psk[0]) {
        derive_key(vc->enc_key, psk);
        vc->enc_enabled = 1;
        DLOG("[crypto] ChaCha20-Poly1305 encryption enabled\n");
    }

    /* ── Resolve server ──────────────────────────────────────────── */
    {
        struct addrinfo hints;
        struct addrinfo *res;
        struct sockaddr_in srv_addr;
        char port_str[16];

        memset(&hints, 0, sizeof(hints));
        hints.ai_family   = AF_INET;
        hints.ai_socktype = SOCK_STREAM;

        snprintf(port_str, sizeof(port_str), "%d", VC_TCP_PORT);
        if (getaddrinfo(vc->server_host, port_str, &hints, &res) != 0) {
            snprintf(g_vc_error, sizeof(g_vc_error),
                     "Could not resolve '%s'", vc->server_host);
            fprintf(stderr, "[error] %s\n", g_vc_error);
            goto fail_early;
        }
        srv_addr = *(struct sockaddr_in *)res->ai_addr;
        freeaddrinfo(res);

        /* UDP server address uses same IP, different port */
        vc->server_udp_addr = srv_addr;
        vc->server_udp_addr.sin_port = htons(VC_UDP_PORT);

        /* ── TCP connect ─────────────────────────────────────────── */
        vc->tcp_fd = socket(AF_INET, SOCK_STREAM, 0);
        if (vc->tcp_fd == SOCK_INVALID) {
            snprintf(g_vc_error, sizeof(g_vc_error), "socket(tcp) failed");
            perror("socket(tcp)");
            goto fail_early;
        }
        {
            int one = 1;
            setsockopt((int)vc->tcp_fd, IPPROTO_TCP, TCP_NODELAY,
                       (char *)&one, sizeof(one));
        }
        if (connect(vc->tcp_fd, (struct sockaddr *)&srv_addr,
                    sizeof(srv_addr)) < 0) {
            snprintf(g_vc_error, sizeof(g_vc_error),
                     "Connection refused: %s:%d", host, VC_TCP_PORT);
            perror("connect");
            sock_close(vc->tcp_fd);
            goto fail_early;
        }
        DLOG("[client] connected to %s:%d\n", vc->server_host, VC_TCP_PORT);
    }

    /* ── UDP socket ──────────────────────────────────────────────── */
    vc->udp_fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (vc->udp_fd == SOCK_INVALID) {
        snprintf(g_vc_error, sizeof(g_vc_error), "socket(udp) failed");
        perror("socket(udp)");
        sock_close(vc->tcp_fd);
        goto fail_early;
    }

    /* DSCP Expedited Forwarding (EF) — 0xB8 — marks voice as highest
     * priority on managed networks (routers honour DSCP for QoS). */
#if defined(__linux__) || defined(__APPLE__)
    {
        int tos = 0xB8; /* DSCP EF (46 << 2) */
        setsockopt((int)vc->udp_fd, IPPROTO_IP, IP_TOS,
                   &tos, sizeof(tos));
    }
#endif
#if defined(__linux__)
    /* SO_PRIORITY 6: kernel gives this socket higher TX priority */
    {
        int prio = 6;
        setsockopt((int)vc->udp_fd, SOL_SOCKET, SO_PRIORITY,
                   &prio, sizeof(prio));
    }
    /* SO_BUSY_POLL: spin up to 50µs before blocking — reduces wakeup latency */
    {
        int busy_us = 50;
        setsockopt((int)vc->udp_fd, SOL_SOCKET, SO_BUSY_POLL,
                   &busy_us, sizeof(busy_us));
    }
#endif

    /* ── Opus encoder ────────────────────────────────────────────── */
    {
        int opus_err;
        vc->encoder = opus_encoder_create(OPUS_SAMPLE_RATE, OPUS_CHANNELS,
                                          OPUS_APPLICATION, &opus_err);
        if (opus_err != OPUS_OK) {
            snprintf(g_vc_error, sizeof(g_vc_error),
                     "Opus encoder: %s", opus_strerror(opus_err));
            fprintf(stderr, "[opus] encoder create failed: %s\n",
                    opus_strerror(opus_err));
            sock_close(vc->tcp_fd);
            sock_close(vc->udp_fd);
            goto fail_early;
        }
        opus_encoder_ctl(vc->encoder, OPUS_SET_BITRATE(OPUS_BITRATE));
        opus_encoder_ctl(vc->encoder, OPUS_SET_COMPLEXITY(5));
        opus_encoder_ctl(vc->encoder, OPUS_SET_SIGNAL(OPUS_SIGNAL_VOICE));
        opus_encoder_ctl(vc->encoder, OPUS_SET_DTX(1));
        opus_encoder_ctl(vc->encoder, OPUS_SET_INBAND_FEC(1));
        opus_encoder_ctl(vc->encoder, OPUS_SET_PACKET_LOSS_PERC(5));
    }

    /* ── PortAudio ───────────────────────────────────────────────── */
    {
        PaError pa_err = Pa_Initialize();
        if (pa_err != paNoError) {
            snprintf(g_vc_error, sizeof(g_vc_error),
                     "PortAudio init: %s", Pa_GetErrorText(pa_err));
            fprintf(stderr, "[pa] init failed: %s\n", Pa_GetErrorText(pa_err));
            opus_encoder_destroy(vc->encoder);
            sock_close(vc->tcp_fd);
            sock_close(vc->udp_fd);
            goto fail_early;
        }

        PaDeviceIndex in_dev  = Pa_GetDefaultInputDevice();
        PaDeviceIndex out_dev = Pa_GetDefaultOutputDevice();
        PaStreamParameters in_params, out_params;
        memset(&in_params,  0, sizeof(in_params));
        memset(&out_params, 0, sizeof(out_params));
        in_params.device                    = in_dev;
        in_params.channelCount              = OPUS_CHANNELS;
        in_params.sampleFormat              = paInt16;
        in_params.suggestedLatency          = Pa_GetDeviceInfo(in_dev)->defaultLowInputLatency;
        out_params.device                   = out_dev;
        out_params.channelCount             = OPUS_CHANNELS;
        out_params.sampleFormat             = paInt16;
        out_params.suggestedLatency         = Pa_GetDeviceInfo(out_dev)->defaultLowOutputLatency;

        DLOG("[pa] input:  %s\n", Pa_GetDeviceInfo(in_params.device)->name);
        DLOG("[pa] output: %s\n", Pa_GetDeviceInfo(out_params.device)->name);

        pa_err = Pa_OpenStream(&vc->pa_stream,
                               &in_params, &out_params,
                               OPUS_SAMPLE_RATE, OPUS_FRAME_SIZE,
                               paClipOff | paDitherOff,
                               pa_callback, NULL);
        if (pa_err != paNoError) {
            snprintf(g_vc_error, sizeof(g_vc_error),
                     "PA open stream: %s", Pa_GetErrorText(pa_err));
            fprintf(stderr, "[pa] open stream failed: %s\n", Pa_GetErrorText(pa_err));
            Pa_Terminate();
            opus_encoder_destroy(vc->encoder);
            sock_close(vc->tcp_fd);
            sock_close(vc->udp_fd);
            goto fail_early;
        }
        pa_err = Pa_StartStream(vc->pa_stream);
        if (pa_err != paNoError) {
            snprintf(g_vc_error, sizeof(g_vc_error),
                     "PA start stream: %s", Pa_GetErrorText(pa_err));
            fprintf(stderr, "[pa] start stream failed: %s\n", Pa_GetErrorText(pa_err));
            Pa_CloseStream(vc->pa_stream);
            Pa_Terminate();
            opus_encoder_destroy(vc->encoder);
            sock_close(vc->tcp_fd);
            sock_close(vc->udp_fd);
            goto fail_early;
        }
    }

    /* ── Threads ─────────────────────────────────────────────────── */
    {
        pthread_t t_tcp, t_udp, t_ka;
#ifndef VC_GUI_BUILD
        pthread_t t_status;
#endif
        pthread_create(&t_tcp, NULL, tcp_recv_thread,  NULL);
        pthread_create(&t_udp, NULL, udp_recv_thread,  NULL);
        pthread_create(&t_ka,  NULL, keepalive_thread, NULL);
#ifndef VC_GUI_BUILD
        pthread_create(&t_status, NULL, status_thread, NULL);
#endif

        /* SCHED_FIFO real-time priority for UDP recv thread on Linux.
         * This eliminates OS scheduling jitter from the hot audio path.
         * Requires CAP_SYS_NICE or running as root — silently ignored if denied. */
#if defined(__linux__)
        {
            struct sched_param sp = { .sched_priority = 10 };
            if (pthread_setschedparam(t_udp, SCHED_FIFO, &sp) != 0)
                fprintf(stderr, "[rt] SCHED_FIFO not granted (run as root for best latency)\n");
            else
                DLOG("[rt] UDP recv thread: SCHED_FIFO priority 10\n");
        }
#endif

        DLOG("[client] running — press Ctrl+C to quit\n");
        DLOG("[client] room='%s' username='%s'\n", vc->room, vc->username);

        /* ── Main wait loop ──────────────────────────────────────── */
        while (!g_quit)
            usleep(100000); /* 100ms */

        /* ── Cleanup ─────────────────────────────────────────────── */
        printf("\n[client] shutting down...\n");
        vc->running = 0;
        atomic_store(&g_vc_ready, 0);

        /* Print stats */
        pthread_mutex_lock(&vc->peers_lock);
        for (int i = 0; i < MAX_PEERS; i++) {
            peer_t *p = &vc->peers[i];
            if (p->active) {
                float loss_pct = p->packets_recv > 0
                    ? 100.0f * p->packets_lost / (p->packets_recv + p->packets_lost)
                    : 0.0f;
                DLOG("[stats] %s: recv=%d lost=%d (%.1f%% loss)\n",
                       p->name, p->packets_recv, p->packets_lost, loss_pct);
            }
        }
        pthread_mutex_unlock(&vc->peers_lock);

        Pa_StopStream(vc->pa_stream);
        Pa_CloseStream(vc->pa_stream);
        Pa_Terminate();

        /* Join threads before closing sockets — threads may still be in
         * recv()/select() and need the fds to be valid until they exit. */
        pthread_join(t_tcp, NULL);
        pthread_join(t_udp, NULL);
        pthread_join(t_ka,  NULL);
#ifndef VC_GUI_BUILD
        pthread_join(t_status, NULL);
#endif

        opus_encoder_destroy(vc->encoder);
        pthread_mutex_destroy(&vc->peers_lock);
        sock_close(vc->tcp_fd);
        sock_close(vc->udp_fd);

#ifdef _WIN32
        WSACleanup();
#endif
        return 0;
    }

fail_early:
    atomic_store(&g_vc_ready, 0);
    pthread_mutex_destroy(&vc->peers_lock);
#ifdef _WIN32
    WSACleanup();
#endif
    return 1;
}

/* ── CLI entry point (excluded from GUI builds) ─────────────────────────── */
#ifndef VC_GUI_BUILD

int main(int argc, char *argv[])
{
    /* --token <tok> : save token to config file and exit */
    if (argc >= 3 && strcmp(argv[1], "--token") == 0) {
#ifdef _WIN32
        WSADATA wsa; WSAStartup(MAKEWORD(2,2), &wsa);
#endif
        return save_token(argv[2]);
    }

    /* --update : self-update from latest CI build */
    if (argc >= 2 && strcmp(argv[1], "--update") == 0) {
#ifdef _WIN32
        WSADATA wsa; WSAStartup(MAKEWORD(2,2), &wsa);
#endif
        return do_update();
    }

    /* --psk <passphrase> : enable ChaCha20-Poly1305 encryption */
    char psk[256] = {0};
    for (int a = 1; a < argc - 1; a++) {
        if (strcmp(argv[a], "--psk") == 0) {
            strncpy(psk, argv[a+1], sizeof(psk)-1);
            /* Shift remaining args left by 2 */
            for (int b = a; b < argc - 2; b++) argv[b] = argv[b+2];
            argc -= 2;
            break;
        }
    }

    if (argc < 2) {
        fprintf(stderr,
                "Usage: %s <server_host> [room] [username] [--psk <key>]\n"
                "       %s --update\n"
                "       %s --token <github_token>\n"
                "  Default room:     general\n"
                "  Default username: user<pid>\n"
                "  --psk <key>:      enable end-to-end ChaCha20-Poly1305 encryption\n",
                argv[0], argv[0], argv[0]);
        return 1;
    }

    char default_user[VC_MAX_USERNAME];
    if (argc >= 4)
        snprintf(default_user, sizeof(default_user), "%s", argv[3]);
    else
        snprintf(default_user, sizeof(default_user), "user%d", (int)getpid());

    return vc_run(argv[1],
                  argc >= 3 ? argv[2] : "general",
                  default_user,
                  psk);
}

#endif /* !VC_GUI_BUILD */

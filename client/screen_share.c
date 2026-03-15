/*
 * screen_share.c — Screen sharing orchestration.
 *
 * Sender:  capture → encode → fragment → send via UDP
 * Receiver: reassemble fragments → decode → present to GUI
 *
 * Called from the engine (client.c) via vc_screen_share_*() API.
 */

#include "../common/screen_capture.h"
#include "../common/encoder.h"
#include "../common/protocol.h"

#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <stdatomic.h>
#include <pthread.h>

#ifdef _WIN32
  #include <winsock2.h>
  #include <windows.h>
  typedef SOCKET sock_t;
#else
  #include <sys/socket.h>
  #include <netinet/in.h>
  #include <unistd.h>
  typedef int sock_t;
#endif

/* ── Forward declarations for engine integration ──────────────────────── */
/* These are resolved at link time from client.c */

/* ── Sender state ─────────────────────────────────────────────────────── */

typedef struct {
    sc_ctx_t       *capture;
    venc_t         *encoder;
    pthread_t       thread;
    volatile int    running;
    atomic_int      active;        /* 1 while sharing */

    /* Network context (set by ss_sender_start) */
    sock_t          udp_fd;
    uint32_t        client_id;
    uint16_t        room_id;
    struct sockaddr_in server_addr;

    uint32_t        frame_id;
    uint16_t        seq;
} ss_sender_t;

/* ── Receiver state ───────────────────────────────────────────────────── */

#define SS_MAX_FRAGS 2048  /* max fragments per frame (2MB / 1300 ≈ 1578) */

typedef struct {
    uint32_t    frame_id;
    uint16_t    frag_count;
    uint16_t    received;
    uint16_t    width, height;
    uint8_t     codec;
    uint8_t     keyframe;
    uint8_t    *data;
    int         data_cap;
    int        *frag_sizes;   /* size of each fragment's payload */
    int        *frag_offsets; /* offset into data for each fragment */
    int         total_bytes;
} ss_frame_assembly_t;

typedef struct {
    vdec_t      *decoder;
    pthread_mutex_t lock;

    /* Frame assembly */
    ss_frame_assembly_t asm_buf;

    /* Decoded frame for display */
    uint8_t    *display_frame;
    int         display_w, display_h;
    uint32_t    display_frame_id;
    int         display_valid;
    int         display_buf_size;

    /* Sharer tracking */
    uint32_t    sharer_id;     /* client_id of current sharer, 0 if none */
} ss_receiver_t;

/* ── Global state ─────────────────────────────────────────────────────── */

static ss_sender_t   g_sender;
static ss_receiver_t g_receiver;
static int           g_receiver_init = 0;
static char          g_encoder_name[64] = "none";

/* ── Sender: capture + encode + fragment + send ───────────────────────── */

static void send_video_fragments(ss_sender_t *s, const uint8_t *encoded,
                                  int encoded_len, int codec_id, int is_key,
                                  int width, int height)
{
    int frag_payload = VC_VIDEO_MTU - (int)VC_HEADER_SIZE - (int)VC_VIDEO_FRAG_SIZE;
    int frag_count = (encoded_len + frag_payload - 1) / frag_payload;
    if (frag_count > 65535) return;

    uint8_t pkt[VC_MAX_PACKET + 128];
    int offset = 0;

    for (int i = 0; i < frag_count; i++) {
        int chunk = encoded_len - offset;
        if (chunk > frag_payload) chunk = frag_payload;

        vc_packet_header_t *hdr = (vc_packet_header_t *)pkt;
        hdr->magic      = VC_MAGIC;
        hdr->type       = (i == frag_count - 1) ? PKT_VIDEO_FIN : PKT_VIDEO;
        hdr->flags      = 0;
        hdr->seq        = s->seq++;
        hdr->client_id  = s->client_id;
        hdr->room_id    = s->room_id;
        hdr->payload_len = (uint16_t)(VC_VIDEO_FRAG_SIZE + chunk);

        vc_video_frag_t *frag = (vc_video_frag_t *)(pkt + VC_HEADER_SIZE);
        frag->frame_id   = s->frame_id;
        frag->frag_idx   = (uint16_t)i;
        frag->frag_count = (uint16_t)frag_count;
        frag->width      = (uint16_t)width;
        frag->height     = (uint16_t)height;
        frag->codec      = (uint8_t)codec_id;
        frag->keyframe   = (uint8_t)is_key;

        memcpy(pkt + VC_HEADER_SIZE + VC_VIDEO_FRAG_SIZE, encoded + offset, (size_t)chunk);
        offset += chunk;

        int total = (int)VC_HEADER_SIZE + (int)VC_VIDEO_FRAG_SIZE + chunk;
        sendto(s->udp_fd, (char *)pkt, (size_t)total, 0,
               (struct sockaddr *)&s->server_addr, sizeof(s->server_addr));
    }
    s->frame_id++;
}

static void *sender_thread(void *arg)
{
    ss_sender_t *s = (ss_sender_t *)arg;
    uint8_t *enc_buf = (uint8_t *)malloc(VC_VIDEO_MAX_FRAME);
    if (!enc_buf) return NULL;

    /* Target ~15 fps for screen share (balance between smooth + bandwidth) */
    int target_ms = 66;  /* ~15 fps */

    while (s->running) {
        int w, h;
        const uint8_t *frame = sc_capture_frame(s->capture, &w, &h);
        if (!frame) {
#ifdef _WIN32
            Sleep(target_ms);
#else
            struct timespec ts = { 0, target_ms * 1000000L };
            nanosleep(&ts, NULL);
#endif
            continue;
        }

        int codec_id, is_key;
        int enc_len = venc_encode(s->encoder, frame, w, h,
                                   enc_buf, VC_VIDEO_MAX_FRAME,
                                   &codec_id, &is_key);
        if (enc_len > 0)
            send_video_fragments(s, enc_buf, enc_len, codec_id, is_key, w, h);

#ifdef _WIN32
        Sleep(target_ms);
#else
        struct timespec ts = { 0, target_ms * 1000000L };
        nanosleep(&ts, NULL);
#endif
    }

    free(enc_buf);
    return NULL;
}

/* ── Receiver: reassemble + decode ────────────────────────────────────── */

static void receiver_init(void)
{
    if (g_receiver_init) return;
    memset(&g_receiver, 0, sizeof(g_receiver));
    pthread_mutex_init(&g_receiver.lock, NULL);
    g_receiver.decoder = vdec_create();
    g_receiver_init = 1;
}

static void asm_reset(ss_frame_assembly_t *a)
{
    a->received    = 0;
    a->total_bytes = 0;
    if (a->frag_sizes) memset(a->frag_sizes, 0, (size_t)SS_MAX_FRAGS * sizeof(int));
}

/* Called from udp_recv_thread in client.c for PKT_VIDEO / PKT_VIDEO_FIN */
void ss_recv_fragment(const uint8_t *pkt, int pkt_len, uint32_t sender_id)
{
    if (pkt_len < (int)(VC_HEADER_SIZE + VC_VIDEO_FRAG_SIZE)) return;

    receiver_init();

    const vc_video_frag_t *frag =
        (const vc_video_frag_t *)(pkt + VC_HEADER_SIZE);
    int payload_offset = (int)VC_HEADER_SIZE + (int)VC_VIDEO_FRAG_SIZE;
    int payload_len    = pkt_len - payload_offset;
    if (payload_len <= 0) return;

    pthread_mutex_lock(&g_receiver.lock);

    g_receiver.sharer_id = sender_id;
    ss_frame_assembly_t *a = &g_receiver.asm_buf;

    /* New frame? Reset assembly */
    if (frag->frame_id != a->frame_id || a->frag_count != frag->frag_count) {
        a->frame_id   = frag->frame_id;
        a->frag_count = frag->frag_count;
        a->width      = frag->width;
        a->height     = frag->height;
        a->codec      = frag->codec;
        a->keyframe   = frag->keyframe;

        int needed = (int)frag->frag_count * (VC_VIDEO_MTU + 64);
        if (needed > a->data_cap) {
            free(a->data);
            a->data = (uint8_t *)malloc((size_t)needed);
            a->data_cap = needed;
        }
        if (!a->frag_sizes) {
            a->frag_sizes  = (int *)calloc(SS_MAX_FRAGS, sizeof(int));
            a->frag_offsets = (int *)calloc(SS_MAX_FRAGS, sizeof(int));
        }
        asm_reset(a);
    }

    /* Store fragment */
    if (frag->frag_idx < SS_MAX_FRAGS && a->frag_sizes[frag->frag_idx] == 0) {
        int off = frag->frag_idx * (VC_VIDEO_MTU - (int)VC_HEADER_SIZE - (int)VC_VIDEO_FRAG_SIZE + 64);
        if (off + payload_len <= a->data_cap) {
            memcpy(a->data + off, pkt + payload_offset, (size_t)payload_len);
            a->frag_offsets[frag->frag_idx] = off;
            a->frag_sizes[frag->frag_idx]   = payload_len;
            a->total_bytes += payload_len;
            a->received++;
        }
    }

    /* All fragments received? Reassemble and decode */
    if (a->received == a->frag_count) {
        /* Linearize encoded data */
        int total = 0;
        for (int i = 0; i < a->frag_count; i++)
            total += a->frag_sizes[i];

        uint8_t *encoded = (uint8_t *)malloc((size_t)total);
        if (encoded) {
            int pos = 0;
            for (int i = 0; i < a->frag_count; i++) {
                memcpy(encoded + pos, a->data + a->frag_offsets[i],
                       (size_t)a->frag_sizes[i]);
                pos += a->frag_sizes[i];
            }

            /* Decode */
            int w = a->width, h = a->height;
            int frame_size = w * h * 4;
            if (frame_size > g_receiver.display_buf_size) {
                free(g_receiver.display_frame);
                g_receiver.display_frame = (uint8_t *)malloc((size_t)frame_size);
                g_receiver.display_buf_size = frame_size;
            }

            if (g_receiver.display_frame && g_receiver.decoder) {
                int rc = vdec_decode(g_receiver.decoder, encoded, total,
                                      a->codec, w, h,
                                      g_receiver.display_frame, frame_size);
                if (rc == 0) {
                    g_receiver.display_w  = w;
                    g_receiver.display_h  = h;
                    g_receiver.display_frame_id = a->frame_id;
                    g_receiver.display_valid = 1;
                }
            }
            free(encoded);
        }
        asm_reset(a);
    }

    pthread_mutex_unlock(&g_receiver.lock);
}

/* ── Public API ───────────────────────────────────────────────────────── */

void ss_start(sock_t udp_fd, uint32_t client_id, uint16_t room_id,
              struct sockaddr_in server_addr)
{
    if (atomic_load(&g_sender.active)) return;

    g_sender.capture = sc_init();
    if (!g_sender.capture) {
        fprintf(stderr, "[screen] capture init failed\n");
        return;
    }

    /* Probe screen size for encoder init */
    int w, h;
    const uint8_t *test = sc_capture_frame(g_sender.capture, &w, &h);
    if (!test) {
        fprintf(stderr, "[screen] initial capture failed\n");
        sc_destroy(g_sender.capture);
        g_sender.capture = NULL;
        return;
    }

    /* Create encoder — probes HW codecs, falls back to VP9 SW */
    g_sender.encoder = venc_create(w, h, 15, 2000);
    if (!g_sender.encoder) {
        fprintf(stderr, "[screen] encoder init failed\n");
        sc_destroy(g_sender.capture);
        g_sender.capture = NULL;
        return;
    }

    snprintf(g_encoder_name, sizeof(g_encoder_name), "%s",
             venc_name(g_sender.encoder));

    g_sender.udp_fd      = udp_fd;
    g_sender.client_id   = client_id;
    g_sender.room_id     = room_id;
    g_sender.server_addr = server_addr;
    g_sender.frame_id    = 0;
    g_sender.seq         = 0;
    g_sender.running     = 1;
    atomic_store(&g_sender.active, 1);

    pthread_create(&g_sender.thread, NULL, sender_thread, &g_sender);
    fprintf(stderr, "[screen] sharing started (%s)\n", g_encoder_name);
}

void ss_stop(void)
{
    if (!atomic_load(&g_sender.active)) return;

    g_sender.running = 0;
    pthread_join(g_sender.thread, NULL);
    atomic_store(&g_sender.active, 0);

    venc_destroy(g_sender.encoder);
    g_sender.encoder = NULL;
    sc_destroy(g_sender.capture);
    g_sender.capture = NULL;

    fprintf(stderr, "[screen] sharing stopped\n");
}

int ss_is_active(void)
{
    return atomic_load(&g_sender.active);
}

int ss_get_frame(uint8_t *out, int buf_size, int *w, int *h, uint32_t *frame_id)
{
    receiver_init();
    pthread_mutex_lock(&g_receiver.lock);
    if (!g_receiver.display_valid) {
        pthread_mutex_unlock(&g_receiver.lock);
        return -1;
    }
    int frame_size = g_receiver.display_w * g_receiver.display_h * 4;
    if (buf_size < frame_size) {
        pthread_mutex_unlock(&g_receiver.lock);
        return -1;
    }
    memcpy(out, g_receiver.display_frame, (size_t)frame_size);
    *w = g_receiver.display_w;
    *h = g_receiver.display_h;
    *frame_id = g_receiver.display_frame_id;
    pthread_mutex_unlock(&g_receiver.lock);
    return 0;
}

uint32_t ss_sharer_id(void)
{
    if (!g_receiver_init) return 0;
    return g_receiver.sharer_id;
}

const char *ss_encoder_name(void)
{
    return g_encoder_name;
}

void ss_receiver_clear(void)
{
    if (!g_receiver_init) return;
    pthread_mutex_lock(&g_receiver.lock);
    g_receiver.display_valid = 0;
    g_receiver.sharer_id     = 0;
    pthread_mutex_unlock(&g_receiver.lock);
}

#pragma once
#include <stdint.h>
#include <stddef.h>

/* ── VoiceChat Protocol ──────────────────────────────────────────────────── *
 *
 *  Signaling:  TCP/WebSocket  (JSON, newline-delimited)
 *  Audio:      UDP            (binary, fixed-header packets)
 *
 *  UDP Packet layout:
 *    [4B magic][1B type][1B flags][2B seq][4B client_id][2B room_id]
 *    [2B payload_len][payload…]
 *
 *  Total header: 16 bytes
 * ─────────────────────────────────────────────────────────────────────── */

#define VC_MAGIC           0x56434855u  /* "VCHU" */
#define VC_VERSION         1

/* Packet types */
#define PKT_AUDIO          0x01   /* Opus-encoded audio frame          */
#define PKT_KEEPALIVE      0x02   /* UDP hole-punch / keep-alive        */
#define PKT_REGISTER       0x03   /* Client registers UDP endpoint      */
#define PKT_REGISTER_ACK   0x04   /* Server confirms registration       */

/* Flags */
#define FLAG_VAD_ACTIVE    0x01   /* Speaker is actively talking        */
#define FLAG_LAST_FRAME    0x02   /* End of speech burst                */
#define FLAG_HAS_REDUNDANT 0x04   /* Previous frame appended (RED)      *
                                   * Payload layout when set:           *
                                   *  [4B ts][2B prim_len][prim_opus]   *
                                   *  [red_opus…]                       *
                                   * red_seq = hdr->seq - 1             */

/* Limits */
#define VC_MAX_ROOMS       64
#define VC_MAX_CLIENTS     256
#define VC_MAX_ROOM_NAME   32
#define VC_MAX_USERNAME    32
#define VC_MAX_PACKET      1400   /* Stay under typical MTU             */
#define VC_OPUS_MAX_FRAME  1275   /* Max Opus packet for 120ms @ 510kbps */

/* Opus settings for low latency */
#define OPUS_SAMPLE_RATE   48000
#define OPUS_CHANNELS      1      /* Mono — halves bandwidth            */
#define OPUS_FRAME_SIZE    960    /* 20ms @ 48kHz — better FEC on lossy links */
#define OPUS_BITRATE       32000  /* 32kbps — excellent quality         */
#define OPUS_APPLICATION   OPUS_APPLICATION_VOIP

/* UDP port */
#define VC_UDP_PORT        7331
#define VC_TCP_PORT        7332   /* Signaling                          */

#pragma pack(push, 1)
typedef struct {
    uint32_t magic;
    uint8_t  type;
    uint8_t  flags;
    uint16_t seq;
    uint32_t client_id;
    uint16_t room_id;
    uint16_t payload_len;
    /* payload follows */
} vc_packet_header_t;
#pragma pack(pop)

#define VC_HEADER_SIZE  sizeof(vc_packet_header_t)  /* 16 bytes */

/* Signaling messages (newline-terminated JSON over TCP) */
/* JOIN:    {"op":"join","room":"roomname","username":"nick"}\n  */
/* LEAVE:   {"op":"leave"}\n                                     */
/* LIST:    {"op":"list"}\n                                      */
/* Server responses:                                             */
/* OK:      {"op":"ok","client_id":1234,"room_id":5}\n           */
/* PEERS:   {"op":"peers","peers":[{"id":1,"name":"x"}]}\n       */
/* JOINED:  {"op":"joined","id":2,"name":"y"}\n                  */
/* LEFT:    {"op":"left","id":2}\n                               */
/* ROOMS:   {"op":"rooms","rooms":[{"name":"x","count":3}]}\n    */
/* ERROR:   {"op":"error","msg":"..."}\n                         */

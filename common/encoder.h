#pragma once
/*
 * encoder.h — Video encoder/decoder abstraction with hardware cascade.
 *
 * Encoder probe order (best → worst):
 *   1. AV1  hardware (Windows MF / macOS VT / Linux VA-API)
 *   2. H.265 hardware
 *   3. H.264 hardware
 *   4. VP9  software (libvpx — always available, statically linked)
 *
 * The encoder auto-selects the best available codec at init time.
 * The decoder accepts any of the four codecs (identified by codec_id
 * in the packet header) and creates decoders on demand.
 */

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Codec IDs — match VC_VIDEO_CODEC_* in protocol.h */
#define VENC_VP9     0x01
#define VENC_H264    0x02
#define VENC_H265    0x03
#define VENC_AV1     0x04

typedef struct venc venc_t;
typedef struct vdec vdec_t;

/* ── Encoder ─────────────────────────────────────────────────── */

/* Create encoder. Probes HW codecs in order, falls back to VP9 SW.
 * target_kbps: target bitrate (e.g. 2000 for 1080p screen share).
 * fps: target framerate.
 * Returns NULL only if VP9 SW also fails (shouldn't happen). */
venc_t *venc_create(int width, int height, int fps, int target_kbps);

/* Reconfigure for new dimensions (e.g. window resize). */
int     venc_reconfigure(venc_t *enc, int width, int height);

/* Request a keyframe on the next encode call. */
void    venc_request_keyframe(venc_t *enc);

/* Encode one BGRA frame. Returns encoded bytes written to out_buf,
 * or 0 on error/skip. *codec_out set to VENC_* id. *is_key set to 1
 * if the output is a keyframe. */
int     venc_encode(venc_t *enc, const uint8_t *bgra, int width, int height,
                    uint8_t *out_buf, int out_buf_size,
                    int *codec_out, int *is_key);

/* Human-readable name: "AV1 (hw)", "VP9 (sw)", etc. */
const char *venc_name(const venc_t *enc);

void    venc_destroy(venc_t *enc);

/* ── Decoder ─────────────────────────────────────────────────── */

/* Create decoder (auto-selects based on codec_id). */
vdec_t *vdec_create(void);

/* Decode one frame. Input is encoded data + codec_id.
 * Output is BGRA written to out_bgra (must be >= w*h*4).
 * Returns 0 on success, -1 on error. */
int     vdec_decode(vdec_t *dec, const uint8_t *data, int data_len,
                    int codec_id, int width, int height,
                    uint8_t *out_bgra, int out_bgra_size);

void    vdec_destroy(vdec_t *dec);

#ifdef __cplusplus
}
#endif

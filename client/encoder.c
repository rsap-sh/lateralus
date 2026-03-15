/*
 * encoder.c — Video encoder/decoder with hardware-first cascade.
 *
 * Probe order:
 *   1. AV1  hardware (via platform API)
 *   2. H.265 hardware
 *   3. H.264 hardware
 *   4. VP9  software (libvpx — always available)
 *
 * Platform hardware encoding:
 *   Windows: Media Foundation (MFT)
 *   macOS:   VideoToolbox (VTCompressionSession)
 *   Linux:   VA-API via dlopen (optional)
 *
 * Decoder: VP9 always via libvpx. HW codecs decoded via platform APIs.
 */

#include "../common/encoder.h"
#include "../common/protocol.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

#include <vpx/vpx_encoder.h>
#include <vpx/vpx_decoder.h>
#include <vpx/vp8cx.h>
#include <vpx/vp8dx.h>

/* ── Color conversion: BGRA → I420 ────────────────────────────────────── */

static void bgra_to_i420(const uint8_t *bgra, int w, int h,
                          uint8_t *y, uint8_t *u, uint8_t *v)
{
    int y_stride = w, uv_stride = w / 2;
    for (int row = 0; row < h; row++) {
        for (int col = 0; col < w; col++) {
            int idx = (row * w + col) * 4;
            uint8_t B = bgra[idx + 0];
            uint8_t G = bgra[idx + 1];
            uint8_t R = bgra[idx + 2];

            int Y =  ((66 * R + 129 * G + 25 * B + 128) >> 8) + 16;
            y[row * y_stride + col] = (uint8_t)(Y < 0 ? 0 : Y > 255 ? 255 : Y);

            if ((row & 1) == 0 && (col & 1) == 0) {
                int U = ((-38 * R - 74 * G + 112 * B + 128) >> 8) + 128;
                int V = ((112 * R - 94 * G - 18 * B + 128) >> 8) + 128;
                int uv_row = row / 2, uv_col = col / 2;
                u[uv_row * uv_stride + uv_col] = (uint8_t)(U < 0 ? 0 : U > 255 ? 255 : U);
                v[uv_row * uv_stride + uv_col] = (uint8_t)(V < 0 ? 0 : V > 255 ? 255 : V);
            }
        }
    }
}

/* ── I420 → BGRA ─────────────────────────────────────────────────────── */

static void i420_to_bgra(const uint8_t *y, const uint8_t *u, const uint8_t *v,
                          int y_stride, int uv_stride,
                          int w, int h, uint8_t *bgra)
{
    for (int row = 0; row < h; row++) {
        for (int col = 0; col < w; col++) {
            int Y = y[row * y_stride + col] - 16;
            int U = u[(row/2) * uv_stride + col/2] - 128;
            int V = v[(row/2) * uv_stride + col/2] - 128;

            int R = (298 * Y + 409 * V + 128) >> 8;
            int G = (298 * Y - 100 * U - 208 * V + 128) >> 8;
            int B = (298 * Y + 516 * U + 128) >> 8;

            int idx = (row * w + col) * 4;
            bgra[idx + 0] = (uint8_t)(B < 0 ? 0 : B > 255 ? 255 : B);
            bgra[idx + 1] = (uint8_t)(G < 0 ? 0 : G > 255 ? 255 : G);
            bgra[idx + 2] = (uint8_t)(R < 0 ? 0 : R > 255 ? 255 : R);
            bgra[idx + 3] = 255;
        }
    }
}

/* ═══════════════════════════════════════════════════════════════════════════
 * Hardware encoder probing (platform-specific)
 * ═══════════════════════════════════════════════════════════════════════════ */

/* Each platform implements hw_enc_try() which attempts to create a hardware
 * encoder for the given codec. Returns an opaque handle or NULL.
 * hw_enc_encode() / hw_enc_destroy() operate on that handle.
 * If hw_enc_try returns NULL for all codecs, we fall back to VP9/libvpx. */

typedef struct hw_enc hw_enc_t;

#ifdef _WIN32
/* ── Windows: Media Foundation ──────────────────────────────────────────── */

#include <windows.h>
#include <initguid.h>
#include <mfapi.h>
#include <mftransform.h>
#include <mfidl.h>
#include <mferror.h>
#include <codecapi.h>
/* MFSetAttributeSize/MFSetAttributeRatio are inline helpers that MinGW
 * headers may not provide.  Pack two UINT32 into a UINT64 manually. */
/* IMFMediaType inherits IMFAttributes in C++ but not in C — accept void*. */
static inline HRESULT my_MFSetAttrSize(IMFMediaType *mt, REFGUID k, UINT32 w, UINT32 h) {
    return mt->lpVtbl->SetUINT64(mt, k, ((UINT64)w << 32) | h);
}
static inline HRESULT my_MFSetAttrRatio(IMFMediaType *mt, REFGUID k, UINT32 n, UINT32 d) {
    return mt->lpVtbl->SetUINT64(mt, k, ((UINT64)n << 32) | d);
}

struct hw_enc {
    IMFTransform   *mft;
    int             codec_id;
    int             width, height;
    int             force_key;
    char            name[32];
};

/* MFVideoFormat_AV1 may not be defined in older MinGW/SDK headers */
#ifndef MFVideoFormat_AV1
static const GUID local_MFVideoFormat_AV1 =
    {0x31305641, 0x0000, 0x0010, {0x80,0x00,0x00,0xAA,0x00,0x38,0x9B,0x71}};
#define MFVideoFormat_AV1 local_MFVideoFormat_AV1
#endif

static const GUID *codec_to_subtype(int codec_id) {
    switch (codec_id) {
    case VENC_AV1:  return &MFVideoFormat_AV1;
    case VENC_H265: return &MFVideoFormat_HEVC;
    case VENC_H264: return &MFVideoFormat_H264;
    default:        return NULL;
    }
}

static hw_enc_t *hw_enc_try(int codec_id, int w, int h, int fps, int kbps)
{
    const GUID *subtype = codec_to_subtype(codec_id);
    if (!subtype) return NULL;

    /* MFStartup must be called once per process */
    static int mf_init = 0;
    if (!mf_init) { MFStartup(MF_VERSION, MFSTARTUP_LITE); mf_init = 1; }

    /* Enumerate hardware encoders for this codec */
    MFT_REGISTER_TYPE_INFO output_type = { MFMediaType_Video, *subtype };
    IMFActivate **activates = NULL;
    UINT32 count = 0;
    HRESULT hr = MFTEnumEx(MFT_CATEGORY_VIDEO_ENCODER,
                            MFT_ENUM_FLAG_HARDWARE | MFT_ENUM_FLAG_SORTANDFILTER,
                            NULL, &output_type, &activates, &count);
    if (FAILED(hr) || count == 0) return NULL;

    IMFTransform *mft = NULL;
    hr = activates[0]->lpVtbl->ActivateObject(activates[0], &IID_IMFTransform,
                                                (void **)&mft);
    for (UINT32 i = 0; i < count; i++)
        activates[i]->lpVtbl->Release(activates[i]);
    CoTaskMemFree(activates);

    if (FAILED(hr) || !mft) return NULL;

    /* Configure output type */
    IMFMediaType *out_mt = NULL;
    MFCreateMediaType(&out_mt);
    out_mt->lpVtbl->SetGUID(out_mt, &MF_MT_MAJOR_TYPE, &MFMediaType_Video);
    out_mt->lpVtbl->SetGUID(out_mt, &MF_MT_SUBTYPE, subtype);
    my_MFSetAttrSize(out_mt, &MF_MT_FRAME_SIZE, (UINT32)w, (UINT32)h);
    my_MFSetAttrRatio(out_mt, &MF_MT_FRAME_RATE, (UINT32)fps, 1);
    out_mt->lpVtbl->SetUINT32(out_mt, &MF_MT_AVG_BITRATE, (UINT32)(kbps * 1000));
    out_mt->lpVtbl->SetUINT32(out_mt, &MF_MT_INTERLACE_MODE,
                                MFVideoInterlace_Progressive);

    hr = mft->lpVtbl->SetOutputType(mft, 0, out_mt, 0);
    out_mt->lpVtbl->Release(out_mt);
    if (FAILED(hr)) { mft->lpVtbl->Release(mft); return NULL; }

    /* Configure input type (NV12) */
    IMFMediaType *in_mt = NULL;
    MFCreateMediaType(&in_mt);
    in_mt->lpVtbl->SetGUID(in_mt, &MF_MT_MAJOR_TYPE, &MFMediaType_Video);
    in_mt->lpVtbl->SetGUID(in_mt, &MF_MT_SUBTYPE, &MFVideoFormat_NV12);
    my_MFSetAttrSize(in_mt, &MF_MT_FRAME_SIZE, (UINT32)w, (UINT32)h);
    my_MFSetAttrRatio(in_mt, &MF_MT_FRAME_RATE, (UINT32)fps, 1);

    hr = mft->lpVtbl->SetInputType(mft, 0, in_mt, 0);
    in_mt->lpVtbl->Release(in_mt);
    if (FAILED(hr)) { mft->lpVtbl->Release(mft); return NULL; }

    hr = mft->lpVtbl->ProcessMessage(mft, MFT_MESSAGE_NOTIFY_BEGIN_STREAMING, 0);
    if (FAILED(hr)) { mft->lpVtbl->Release(mft); return NULL; }

    hw_enc_t *enc = (hw_enc_t *)calloc(1, sizeof(hw_enc_t));
    enc->mft      = mft;
    enc->codec_id = codec_id;
    enc->width    = w;
    enc->height   = h;
    const char *names[] = { NULL, "VP9", "H.264", "H.265", "AV1" };
    snprintf(enc->name, sizeof(enc->name), "%s (hw/MF)", names[codec_id]);
    fprintf(stderr, "[encoder] Windows MF hardware: %s\n", enc->name);
    return enc;
}

static int hw_enc_encode(hw_enc_t *enc, const uint8_t *bgra, int w, int h,
                          uint8_t *out, int out_size, int *is_key)
{
    /* TODO: Full MF sample submission pipeline. For now, return 0 to
     * trigger fallback to VP9 SW if MF encode path isn't fully wired. */
    (void)enc; (void)bgra; (void)w; (void)h; (void)out; (void)out_size; (void)is_key;
    return 0;
}

static void hw_enc_destroy(hw_enc_t *enc)
{
    if (!enc) return;
    if (enc->mft) enc->mft->lpVtbl->Release(enc->mft);
    free(enc);
}

#elif defined(__APPLE__)
/* ── macOS: VideoToolbox ────────────────────────────────────────────────── */

#include <VideoToolbox/VideoToolbox.h>

struct hw_enc {
    VTCompressionSessionRef session;
    int      codec_id;
    int      width, height;
    int      force_key;
    uint8_t *out_buf;
    int      out_len;
    int      out_is_key;
    int      out_capacity;
    char     name[32];
};

static void vt_output_callback(void *ctx, void *src, OSStatus status,
                                 VTEncodeInfoFlags flags,
                                 CMSampleBufferRef buf)
{
    (void)src; (void)flags;
    hw_enc_t *enc = (hw_enc_t *)ctx;
    if (status != noErr || !buf) { enc->out_len = 0; return; }

    /* Check if keyframe */
    CFArrayRef attachments = CMSampleBufferGetSampleAttachmentsArray(buf, false);
    if (attachments && CFArrayGetCount(attachments) > 0) {
        CFDictionaryRef dict = (CFDictionaryRef)CFArrayGetValueAtIndex(attachments, 0);
        CFBooleanRef notKey = (CFBooleanRef)CFDictionaryGetValue(dict,
            kCMSampleAttachmentKey_NotSync);
        enc->out_is_key = (!notKey || !CFBooleanGetValue(notKey));
    } else {
        enc->out_is_key = 1;
    }

    CMBlockBufferRef block = CMSampleBufferGetDataBuffer(buf);
    size_t total = 0;
    char *data = NULL;
    CMBlockBufferGetDataPointer(block, 0, NULL, &total, &data);
    if ((int)total <= enc->out_capacity) {
        memcpy(enc->out_buf, data, total);
        enc->out_len = (int)total;
    } else {
        enc->out_len = 0;
    }
}

static hw_enc_t *hw_enc_try(int codec_id, int w, int h, int fps, int kbps)
{
    CMVideoCodecType ct;
    const char *codec_name;
    switch (codec_id) {
#ifdef kCMVideoCodecType_AV1
    case VENC_AV1:  ct = kCMVideoCodecType_AV1;  codec_name = "AV1";  break;
#endif
    case VENC_H265: ct = kCMVideoCodecType_HEVC;  codec_name = "H.265"; break;
    case VENC_H264: ct = kCMVideoCodecType_H264;  codec_name = "H.264"; break;
    default: return NULL;
    }

    VTCompressionSessionRef session = NULL;
    OSStatus err = VTCompressionSessionCreate(NULL, w, h, ct, NULL, NULL,
                                               NULL, vt_output_callback,
                                               NULL, &session);
    if (err != noErr || !session) return NULL;

    /* Configure for low-latency realtime */
    VTSessionSetProperty(session, kVTCompressionPropertyKey_RealTime,
                          kCFBooleanTrue);
    VTSessionSetProperty(session, kVTCompressionPropertyKey_AllowFrameReordering,
                          kCFBooleanFalse);

    int avg_bps = kbps * 1000;
    CFNumberRef br = CFNumberCreate(NULL, kCFNumberIntType, &avg_bps);
    VTSessionSetProperty(session,
                          kVTCompressionPropertyKey_AverageBitRate, br);

    float fps_f = (float)fps;
    CFNumberRef fps_ref = CFNumberCreate(NULL, kCFNumberFloat32Type, &fps_f);
    VTSessionSetProperty(session,
                          kVTCompressionPropertyKey_ExpectedFrameRate, fps_ref);

    int max_interval = fps * 2;  /* keyframe every 2s */
    CFNumberRef ki = CFNumberCreate(NULL, kCFNumberIntType, &max_interval);
    VTSessionSetProperty(session,
                          kVTCompressionPropertyKey_MaxKeyFrameInterval, ki);

    VTCompressionSessionPrepareToEncodeFrames(session);

    /* Tear down the probe session — we need to re-create with callback ctx */
    VTCompressionSessionInvalidate(session);
    CFRelease(session);

    hw_enc_t *enc = (hw_enc_t *)calloc(1, sizeof(hw_enc_t));
    enc->codec_id     = codec_id;
    enc->width        = w;
    enc->height       = h;
    enc->out_capacity = w * h;  /* generous buffer */
    enc->out_buf      = (uint8_t *)malloc((size_t)enc->out_capacity);

    err = VTCompressionSessionCreate(NULL, w, h, ct, NULL, NULL,
                                      NULL, vt_output_callback,
                                      enc, &enc->session);
    if (err != noErr) {
        free(enc->out_buf);
        free(enc);
        return NULL;
    }
    VTSessionSetProperty(enc->session, kVTCompressionPropertyKey_RealTime,
                          kCFBooleanTrue);
    VTSessionSetProperty(enc->session,
                          kVTCompressionPropertyKey_AllowFrameReordering,
                          kCFBooleanFalse);
    /* Re-apply bitrate, fps, keyframe interval to the real session */
    VTSessionSetProperty(enc->session,
                          kVTCompressionPropertyKey_AverageBitRate, br);
    CFRelease(br);
    VTSessionSetProperty(enc->session,
                          kVTCompressionPropertyKey_ExpectedFrameRate, fps_ref);
    CFRelease(fps_ref);
    VTSessionSetProperty(enc->session,
                          kVTCompressionPropertyKey_MaxKeyFrameInterval, ki);
    CFRelease(ki);
    VTCompressionSessionPrepareToEncodeFrames(enc->session);

    snprintf(enc->name, sizeof(enc->name), "%s (hw/VT)", codec_name);
    fprintf(stderr, "[encoder] macOS VideoToolbox: %s\n", enc->name);
    return enc;
}

static int hw_enc_encode(hw_enc_t *enc, const uint8_t *bgra, int w, int h,
                          uint8_t *out, int out_size, int *is_key)
{
    /* Create CVPixelBuffer from BGRA data */
    CVPixelBufferRef pb = NULL;
    CVReturn cv_err = CVPixelBufferCreate(kCFAllocatorDefault, w, h,
                                           kCVPixelFormatType_32BGRA,
                                           NULL, &pb);
    if (cv_err != kCVReturnSuccess) return 0;

    CVPixelBufferLockBaseAddress(pb, 0);
    uint8_t *dst = (uint8_t *)CVPixelBufferGetBaseAddress(pb);
    size_t stride = CVPixelBufferGetBytesPerRow(pb);
    for (int y = 0; y < h; y++)
        memcpy(dst + y * stride, bgra + y * w * 4, (size_t)w * 4);
    CVPixelBufferUnlockBaseAddress(pb, 0);

    CMTime pts = CMTimeMake(0, (int32_t)(enc->width > 0 ? 30 : 30));
    VTEncodeInfoFlags info;
    enc->out_len = 0;

    CFMutableDictionaryRef props = NULL;
    if (enc->force_key) {
        props = CFDictionaryCreateMutable(NULL, 1,
            &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
        CFDictionarySetValue(props,
            kVTEncodeFrameOptionKey_ForceKeyFrame, kCFBooleanTrue);
        enc->force_key = 0;
    }

    OSStatus err = VTCompressionSessionEncodeFrame(enc->session, pb, pts,
                                                     kCMTimeInvalid,
                                                     props, NULL, &info);
    if (props) CFRelease(props);
    CVPixelBufferRelease(pb);

    if (err != noErr || enc->out_len == 0) return 0;
    if (enc->out_len > out_size) return 0;

    memcpy(out, enc->out_buf, (size_t)enc->out_len);
    *is_key = enc->out_is_key;
    return enc->out_len;
}

static void hw_enc_destroy(hw_enc_t *enc)
{
    if (!enc) return;
    if (enc->session) {
        VTCompressionSessionInvalidate(enc->session);
        CFRelease(enc->session);
    }
    free(enc->out_buf);
    free(enc);
}

#else
/* ── Linux: stub (VA-API would go here via dlopen) ──────────────────────── */

struct hw_enc { int codec_id; int force_key; char name[32]; };

static hw_enc_t *hw_enc_try(int codec_id, int w, int h, int fps, int kbps)
{
    /* VA-API hardware encoding via dlopen could be added here.
     * For now, fall back to VP9 software on Linux. */
    (void)codec_id; (void)w; (void)h; (void)fps; (void)kbps;
    return NULL;
}

static int hw_enc_encode(hw_enc_t *enc, const uint8_t *bgra, int w, int h,
                          uint8_t *out, int out_size, int *is_key)
{
    (void)enc; (void)bgra; (void)w; (void)h; (void)out; (void)out_size; (void)is_key;
    return 0;
}

static void hw_enc_destroy(hw_enc_t *enc) { free(enc); }

#endif /* platform HW encoder */


/* ═══════════════════════════════════════════════════════════════════════════
 * VP9 software encoder/decoder (libvpx — always available)
 * ═══════════════════════════════════════════════════════════════════════════ */

typedef struct {
    vpx_codec_ctx_t   codec;
    vpx_image_t       img;
    int               width, height;
    int               frame_count;
    int               force_key;
} vp9_enc_t;

static vp9_enc_t *vp9_enc_create(int w, int h, int fps, int kbps)
{
    vp9_enc_t *ve = (vp9_enc_t *)calloc(1, sizeof(vp9_enc_t));
    if (!ve) return NULL;

    vpx_codec_enc_cfg_t cfg;
    vpx_codec_err_t err = vpx_codec_enc_config_default(
        vpx_codec_vp9_cx(), &cfg, 0);
    if (err) { free(ve); return NULL; }

    cfg.g_w             = (unsigned int)w;
    cfg.g_h             = (unsigned int)h;
    cfg.g_timebase.num  = 1;
    cfg.g_timebase.den  = fps;
    cfg.rc_target_bitrate = (unsigned int)kbps;
    cfg.g_error_resilient = VPX_ERROR_RESILIENT_DEFAULT;
    cfg.g_lag_in_frames   = 0;       /* zero-latency */
    cfg.rc_end_usage      = VPX_CBR;
    cfg.g_threads         = 4;
    cfg.kf_max_dist       = fps * 2; /* keyframe every 2s */
    cfg.kf_min_dist       = 0;
    cfg.rc_buf_sz         = 600;     /* 600ms buffer */
    cfg.rc_buf_initial_sz = 400;
    cfg.rc_buf_optimal_sz = 500;

    err = vpx_codec_enc_init(&ve->codec, vpx_codec_vp9_cx(), &cfg, 0);
    if (err) { free(ve); return NULL; }

    /* Real-time speed preset (8 = fastest) */
    vpx_codec_control(&ve->codec, VP8E_SET_CPUUSED, 8);
    /* Screen content mode — better for text/UI */
    vpx_codec_control(&ve->codec, VP9E_SET_TUNE_CONTENT,
                       VP9E_CONTENT_SCREEN);
    /* Row-based multi-threading */
    vpx_codec_control(&ve->codec, VP9E_SET_ROW_MT, 1);

    if (!vpx_img_alloc(&ve->img, VPX_IMG_FMT_I420,
                        (unsigned int)w, (unsigned int)h, 16)) {
        vpx_codec_destroy(&ve->codec);
        free(ve);
        return NULL;
    }

    ve->width  = w;
    ve->height = h;
    fprintf(stderr, "[encoder] VP9 software (libvpx) %dx%d @ %d kbps\n",
            w, h, kbps);
    return ve;
}

static int vp9_enc_encode(vp9_enc_t *ve, const uint8_t *bgra, int w, int h,
                           uint8_t *out, int out_size, int *is_key)
{
    if (w != ve->width || h != ve->height) return 0;

    bgra_to_i420(bgra, w, h,
                  ve->img.planes[0], ve->img.planes[1], ve->img.planes[2]);

    vpx_enc_frame_flags_t flags = 0;
    if (ve->force_key) {
        flags = VPX_EFLAG_FORCE_KF;
        ve->force_key = 0;
    }

    vpx_codec_err_t err = vpx_codec_encode(&ve->codec, &ve->img,
                                             ve->frame_count++, 1, flags,
                                             VPX_DL_REALTIME);
    if (err) return 0;

    const vpx_codec_cx_pkt_t *pkt = NULL;
    vpx_codec_iter_t iter = NULL;
    while ((pkt = vpx_codec_get_cx_data(&ve->codec, &iter)) != NULL) {
        if (pkt->kind == VPX_CODEC_CX_FRAME_PKT) {
            int len = (int)pkt->data.frame.sz;
            if (len > out_size) return 0;
            memcpy(out, pkt->data.frame.buf, (size_t)len);
            *is_key = (pkt->data.frame.flags & VPX_FRAME_IS_KEY) != 0;
            return len;
        }
    }
    return 0;
}

static void vp9_enc_destroy(vp9_enc_t *ve)
{
    if (!ve) return;
    vpx_img_free(&ve->img);
    vpx_codec_destroy(&ve->codec);
    free(ve);
}


/* ═══════════════════════════════════════════════════════════════════════════
 * Public encoder API
 * ═══════════════════════════════════════════════════════════════════════════ */

struct venc {
    hw_enc_t  *hw;        /* NULL if using VP9 SW */
    vp9_enc_t *sw;        /* VP9 software fallback */
    int        codec_id;
    char       name[64];
    int        width, height;
};

venc_t *venc_create(int width, int height, int fps, int target_kbps)
{
    venc_t *enc = (venc_t *)calloc(1, sizeof(venc_t));
    if (!enc) return NULL;
    enc->width  = width;
    enc->height = height;

    /* Try hardware encoders: AV1 → H.265 → H.264 */
    int hw_codecs[] = { VENC_AV1, VENC_H265, VENC_H264 };
    for (int i = 0; i < 3; i++) {
        enc->hw = hw_enc_try(hw_codecs[i], width, height, fps, target_kbps);
        if (enc->hw) {
            enc->codec_id = hw_codecs[i];
            snprintf(enc->name, sizeof(enc->name), "%s", enc->hw->name);
            return enc;
        }
    }

    /* Fallback: VP9 software */
    enc->sw = vp9_enc_create(width, height, fps, target_kbps);
    if (!enc->sw) { free(enc); return NULL; }
    enc->codec_id = VENC_VP9;
    snprintf(enc->name, sizeof(enc->name), "VP9 (sw)");
    return enc;
}

int venc_reconfigure(venc_t *enc, int width, int height)
{
    if (!enc || (width == enc->width && height == enc->height)) return 0;
    /* For simplicity, destroy and recreate with same params */
    /* TODO: proper reconfigure without full teardown */
    (void)width; (void)height;
    return -1; /* caller should destroy/recreate */
}

void venc_request_keyframe(venc_t *enc)
{
    if (!enc) return;
    if (enc->hw) enc->hw->force_key = 1;
    if (enc->sw) enc->sw->force_key = 1;
}

int venc_encode(venc_t *enc, const uint8_t *bgra, int width, int height,
                uint8_t *out_buf, int out_buf_size,
                int *codec_out, int *is_key)
{
    if (!enc) return 0;
    *codec_out = enc->codec_id;
    *is_key = 0;

    /* Try hardware first */
    if (enc->hw) {
        int len = hw_enc_encode(enc->hw, bgra, width, height,
                                 out_buf, out_buf_size, is_key);
        if (len > 0) return len;
        /* HW encode returned 0 — fall through to SW if available */
    }

    /* VP9 software fallback */
    if (enc->sw) {
        *codec_out = VENC_VP9;
        return vp9_enc_encode(enc->sw, bgra, width, height,
                               out_buf, out_buf_size, is_key);
    }

    return 0;
}

const char *venc_name(const venc_t *enc)
{
    return enc ? enc->name : "none";
}

void venc_destroy(venc_t *enc)
{
    if (!enc) return;
    hw_enc_destroy(enc->hw);
    vp9_enc_destroy(enc->sw);
    free(enc);
}

/* ═══════════════════════════════════════════════════════════════════════════
 * VP9 decoder (always available via libvpx)
 * ═══════════════════════════════════════════════════════════════════════════ */

struct vdec {
    vpx_codec_ctx_t vp9;
    int             vp9_init;
};

vdec_t *vdec_create(void)
{
    vdec_t *dec = (vdec_t *)calloc(1, sizeof(vdec_t));
    if (!dec) return NULL;

    vpx_codec_err_t err = vpx_codec_dec_init(&dec->vp9, vpx_codec_vp9_dx(),
                                               NULL, 0);
    if (err) {
        fprintf(stderr, "[decoder] VP9 init failed: %s\n",
                vpx_codec_err_to_string(err));
        free(dec);
        return NULL;
    }
    dec->vp9_init = 1;
    return dec;
}

int vdec_decode(vdec_t *dec, const uint8_t *data, int data_len,
                int codec_id, int width, int height,
                uint8_t *out_bgra, int out_bgra_size)
{
    if (!dec || !data || data_len <= 0) return -1;
    if (out_bgra_size < width * height * 4) return -1;

    /* Currently only VP9 decode is implemented via libvpx.
     * H.264/H.265/AV1 would use platform decoders (MF/VT/VA-API). */
    if (codec_id != VENC_VP9) {
        fprintf(stderr, "[decoder] unsupported codec %d for decode\n", codec_id);
        return -1;
    }

    vpx_codec_err_t err = vpx_codec_decode(&dec->vp9, data,
                                             (unsigned int)data_len,
                                             NULL, 0);
    if (err) return -1;

    vpx_codec_iter_t iter = NULL;
    vpx_image_t *img = vpx_codec_get_frame(&dec->vp9, &iter);
    if (!img) return -1;

    i420_to_bgra(img->planes[0], img->planes[1], img->planes[2],
                  img->stride[0], img->stride[1],
                  (int)img->d_w, (int)img->d_h, out_bgra);
    return 0;
}

void vdec_destroy(vdec_t *dec)
{
    if (!dec) return;
    if (dec->vp9_init) vpx_codec_destroy(&dec->vp9);
    free(dec);
}

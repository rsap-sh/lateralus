#pragma once
/*
 * screen_capture.h — Cross-platform screen capture abstraction.
 *
 * Platform backends:
 *   Windows: DXGI Desktop Duplication (zero-copy GPU capture)
 *   macOS:   CGDisplayStream / ScreenCaptureKit
 *   Linux:   X11 XShm (Xorg) with PipeWire fallback (Wayland)
 *
 * All backends deliver BGRA frames via sc_capture_frame().
 */

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct sc_ctx sc_ctx_t;

/* Initialize screen capture. Returns NULL on failure. */
sc_ctx_t *sc_init(void);

/* Capture one frame. Returns pointer to BGRA pixel data (valid until next
 * sc_capture_frame call or sc_destroy). Sets *w and *h to dimensions.
 * Returns NULL if capture fails. */
const uint8_t *sc_capture_frame(sc_ctx_t *ctx, int *w, int *h);

/* Release resources. */
void sc_destroy(sc_ctx_t *ctx);

#ifdef __cplusplus
}
#endif

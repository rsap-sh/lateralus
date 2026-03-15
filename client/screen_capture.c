/*
 * screen_capture.c — Cross-platform screen capture.
 *
 * Windows: DXGI Desktop Duplication (DX11)
 * macOS:   CGWindowListCreateImage (CGDisplayStream requires async)
 * Linux:   X11 XShm (fast shared-memory capture)
 *
 * All backends output BGRA pixel buffers.
 */

#include "../common/screen_capture.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

/* ═══════════════════════════════════════════════════════════════════════════
 * Windows: DXGI Desktop Duplication
 * ═══════════════════════════════════════════════════════════════════════════ */
#ifdef _WIN32

#include <d3d11.h>
#include <dxgi1_2.h>
#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "dxgi.lib")

struct sc_ctx {
    ID3D11Device           *device;
    ID3D11DeviceContext    *context;
    IDXGIOutputDuplication *dup;
    ID3D11Texture2D        *staging;
    uint8_t                *frame_buf;
    int                     width, height;
    int                     initialized;
};

sc_ctx_t *sc_init(void)
{
    sc_ctx_t *ctx = (sc_ctx_t *)calloc(1, sizeof(sc_ctx_t));
    if (!ctx) return NULL;

    D3D_FEATURE_LEVEL fl;
    HRESULT hr = D3D11CreateDevice(NULL, D3D_DRIVER_TYPE_HARDWARE, NULL,
                                    0, NULL, 0, D3D11_SDK_VERSION,
                                    &ctx->device, &fl, &ctx->context);
    if (FAILED(hr)) { free(ctx); return NULL; }

    IDXGIDevice *dxgi_dev = NULL;
    ctx->device->lpVtbl->QueryInterface(ctx->device, &IID_IDXGIDevice,
                                         (void **)&dxgi_dev);
    IDXGIAdapter *adapter = NULL;
    dxgi_dev->lpVtbl->GetAdapter(dxgi_dev, &adapter);
    IDXGIOutput *output = NULL;
    adapter->lpVtbl->EnumOutputs(adapter, 0, &output);

    IDXGIOutput1 *output1 = NULL;
    output->lpVtbl->QueryInterface(output, &IID_IDXGIOutput1,
                                    (void **)&output1);

    hr = output1->lpVtbl->DuplicateOutput(output1, (IUnknown *)ctx->device,
                                            &ctx->dup);
    output1->lpVtbl->Release(output1);
    output->lpVtbl->Release(output);
    adapter->lpVtbl->Release(adapter);
    dxgi_dev->lpVtbl->Release(dxgi_dev);

    if (FAILED(hr)) {
        ctx->device->lpVtbl->Release(ctx->device);
        ctx->context->lpVtbl->Release(ctx->context);
        free(ctx);
        return NULL;
    }

    /* Get output dimensions */
    DXGI_OUTDUPL_DESC dd;
    ctx->dup->lpVtbl->GetDesc(ctx->dup, &dd);
    ctx->width  = (int)dd.ModeDesc.Width;
    ctx->height = (int)dd.ModeDesc.Height;

    /* Create staging texture for CPU readback */
    D3D11_TEXTURE2D_DESC td = {0};
    td.Width              = (UINT)ctx->width;
    td.Height             = (UINT)ctx->height;
    td.MipLevels          = 1;
    td.ArraySize          = 1;
    td.Format             = DXGI_FORMAT_B8G8R8A8_UNORM;
    td.SampleDesc.Count   = 1;
    td.Usage              = D3D11_USAGE_STAGING;
    td.CPUAccessFlags     = D3D11_CPU_ACCESS_READ;

    hr = ctx->device->lpVtbl->CreateTexture2D(ctx->device, &td, NULL,
                                               &ctx->staging);
    if (FAILED(hr)) {
        sc_destroy(ctx);
        return NULL;
    }

    ctx->frame_buf = (uint8_t *)malloc((size_t)ctx->width * ctx->height * 4);
    ctx->initialized = 1;
    return ctx;
}

const uint8_t *sc_capture_frame(sc_ctx_t *ctx, int *w, int *h)
{
    if (!ctx || !ctx->initialized) return NULL;

    IDXGIResource *resource = NULL;
    DXGI_OUTDUPL_FRAME_INFO fi;
    HRESULT hr = ctx->dup->lpVtbl->AcquireNextFrame(ctx->dup, 100,
                                                      &fi, &resource);
    if (FAILED(hr)) return NULL;

    ID3D11Texture2D *tex = NULL;
    resource->lpVtbl->QueryInterface(resource, &IID_ID3D11Texture2D,
                                      (void **)&tex);
    ctx->context->lpVtbl->CopyResource(ctx->context,
                                         (ID3D11Resource *)ctx->staging,
                                         (ID3D11Resource *)tex);
    tex->lpVtbl->Release(tex);
    resource->lpVtbl->Release(resource);

    D3D11_MAPPED_SUBRESOURCE map;
    hr = ctx->context->lpVtbl->Map(ctx->context,
                                    (ID3D11Resource *)ctx->staging, 0,
                                    D3D11_MAP_READ, 0, &map);
    if (SUCCEEDED(hr)) {
        /* Copy row by row (pitch may differ) */
        size_t row_bytes = (size_t)ctx->width * 4;
        for (int y = 0; y < ctx->height; y++)
            memcpy(ctx->frame_buf + y * row_bytes,
                   (uint8_t *)map.pData + y * map.RowPitch, row_bytes);
        ctx->context->lpVtbl->Unmap(ctx->context,
                                     (ID3D11Resource *)ctx->staging, 0);
    }

    ctx->dup->lpVtbl->ReleaseFrame(ctx->dup);

    *w = ctx->width;
    *h = ctx->height;
    return ctx->frame_buf;
}

void sc_destroy(sc_ctx_t *ctx)
{
    if (!ctx) return;
    if (ctx->staging) ctx->staging->lpVtbl->Release(ctx->staging);
    if (ctx->dup)     ctx->dup->lpVtbl->Release(ctx->dup);
    if (ctx->context) ctx->context->lpVtbl->Release(ctx->context);
    if (ctx->device)  ctx->device->lpVtbl->Release(ctx->device);
    free(ctx->frame_buf);
    free(ctx);
}

/* ═══════════════════════════════════════════════════════════════════════════
 * macOS: Core Graphics screen capture
 * ═══════════════════════════════════════════════════════════════════════════ */
#elif defined(__APPLE__)

#include <CoreGraphics/CoreGraphics.h>
#include <ApplicationServices/ApplicationServices.h>

struct sc_ctx {
    uint8_t *frame_buf;
    int      width, height;
    size_t   buf_size;
};

sc_ctx_t *sc_init(void)
{
    sc_ctx_t *ctx = (sc_ctx_t *)calloc(1, sizeof(sc_ctx_t));
    if (!ctx) return NULL;

    CGDirectDisplayID display = CGMainDisplayID();
    ctx->width  = (int)CGDisplayPixelsWide(display);
    ctx->height = (int)CGDisplayPixelsHigh(display);
    ctx->buf_size = (size_t)ctx->width * ctx->height * 4;
    ctx->frame_buf = (uint8_t *)malloc(ctx->buf_size);
    if (!ctx->frame_buf) { free(ctx); return NULL; }
    return ctx;
}

const uint8_t *sc_capture_frame(sc_ctx_t *ctx, int *w, int *h)
{
    if (!ctx) return NULL;

    CGImageRef img = CGWindowListCreateImage(
        CGRectInfinite,
        kCGWindowListOptionOnScreenOnly,
        kCGNullWindowID,
        kCGWindowImageDefault);
    if (!img) return NULL;

    size_t iw = CGImageGetWidth(img);
    size_t ih = CGImageGetHeight(img);

    /* Resize buffer if display changed */
    if ((int)iw != ctx->width || (int)ih != ctx->height) {
        ctx->width  = (int)iw;
        ctx->height = (int)ih;
        ctx->buf_size = iw * ih * 4;
        free(ctx->frame_buf);
        ctx->frame_buf = (uint8_t *)malloc(ctx->buf_size);
        if (!ctx->frame_buf) { CGImageRelease(img); return NULL; }
    }

    CGColorSpaceRef cs = CGColorSpaceCreateDeviceRGB();
    CGContextRef cg = CGBitmapContextCreate(
        ctx->frame_buf, iw, ih, 8, iw * 4, cs,
        kCGBitmapByteOrder32Little | kCGImageAlphaPremultipliedFirst);
    CGColorSpaceRelease(cs);

    if (cg) {
        CGContextDrawImage(cg, CGRectMake(0, 0, iw, ih), img);
        CGContextRelease(cg);
    }
    CGImageRelease(img);

    *w = ctx->width;
    *h = ctx->height;
    return ctx->frame_buf;
}

void sc_destroy(sc_ctx_t *ctx)
{
    if (!ctx) return;
    free(ctx->frame_buf);
    free(ctx);
}

/* ═══════════════════════════════════════════════════════════════════════════
 * Linux: X11 XShm screen capture
 * ═══════════════════════════════════════════════════════════════════════════ */
#else

#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <sys/ipc.h>
#include <sys/shm.h>
#include <X11/extensions/XShm.h>

struct sc_ctx {
    Display            *dpy;
    Window              root;
    XShmSegmentInfo     shm;
    XImage             *img;
    uint8_t            *frame_buf;
    int                 width, height;
    int                 shm_attached;
};

sc_ctx_t *sc_init(void)
{
    sc_ctx_t *ctx = (sc_ctx_t *)calloc(1, sizeof(sc_ctx_t));
    if (!ctx) return NULL;

    ctx->dpy = XOpenDisplay(NULL);
    if (!ctx->dpy) {
        fprintf(stderr, "[screen] cannot open X display\n");
        free(ctx);
        return NULL;
    }

    int screen  = DefaultScreen(ctx->dpy);
    ctx->root   = RootWindow(ctx->dpy, screen);
    ctx->width  = DisplayWidth(ctx->dpy, screen);
    ctx->height = DisplayHeight(ctx->dpy, screen);

    /* Try XShm for fast capture */
    if (!XShmQueryExtension(ctx->dpy)) {
        fprintf(stderr, "[screen] XShm not available, using slow path\n");
        ctx->frame_buf = (uint8_t *)malloc((size_t)ctx->width * ctx->height * 4);
        return ctx;
    }

    ctx->img = XShmCreateImage(ctx->dpy, DefaultVisual(ctx->dpy, screen),
                                (unsigned int)DefaultDepth(ctx->dpy, screen),
                                ZPixmap, NULL, &ctx->shm,
                                (unsigned int)ctx->width,
                                (unsigned int)ctx->height);
    if (!ctx->img) {
        ctx->frame_buf = (uint8_t *)malloc((size_t)ctx->width * ctx->height * 4);
        return ctx;
    }

    ctx->shm.shmid = shmget(IPC_PRIVATE,
                              (size_t)ctx->img->bytes_per_line * ctx->img->height,
                              IPC_CREAT | 0600);
    ctx->shm.shmaddr = ctx->img->data = (char *)shmat(ctx->shm.shmid, NULL, 0);
    ctx->shm.readOnly = False;
    XShmAttach(ctx->dpy, &ctx->shm);
    ctx->shm_attached = 1;

    /* Mark segment for removal after detach */
    shmctl(ctx->shm.shmid, IPC_RMID, NULL);

    ctx->frame_buf = (uint8_t *)malloc((size_t)ctx->width * ctx->height * 4);
    return ctx;
}

const uint8_t *sc_capture_frame(sc_ctx_t *ctx, int *w, int *h)
{
    if (!ctx || !ctx->dpy) return NULL;

    if (ctx->shm_attached && ctx->img) {
        XShmGetImage(ctx->dpy, ctx->root, ctx->img, 0, 0, AllPlanes);
        /* XImage is typically BGRA on modern X11 — copy directly */
        size_t row = (size_t)ctx->width * 4;
        for (int y = 0; y < ctx->height; y++)
            memcpy(ctx->frame_buf + y * row,
                   ctx->img->data + y * ctx->img->bytes_per_line, row);
    } else {
        /* Slow path: XGetImage */
        XImage *img = XGetImage(ctx->dpy, ctx->root, 0, 0,
                                 (unsigned int)ctx->width,
                                 (unsigned int)ctx->height,
                                 AllPlanes, ZPixmap);
        if (!img) return NULL;
        size_t row = (size_t)ctx->width * 4;
        for (int y = 0; y < ctx->height; y++)
            memcpy(ctx->frame_buf + y * row,
                   img->data + y * img->bytes_per_line, row);
        XDestroyImage(img);
    }

    *w = ctx->width;
    *h = ctx->height;
    return ctx->frame_buf;
}

void sc_destroy(sc_ctx_t *ctx)
{
    if (!ctx) return;
    if (ctx->shm_attached) {
        XShmDetach(ctx->dpy, &ctx->shm);
        shmdt(ctx->shm.shmaddr);
    }
    if (ctx->img) XDestroyImage(ctx->img);
    if (ctx->dpy) XCloseDisplay(ctx->dpy);
    free(ctx->frame_buf);
    free(ctx);
}

#endif /* platform */

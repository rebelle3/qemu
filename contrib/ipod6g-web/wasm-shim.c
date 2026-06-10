/*
 * Browser glue for the QEMU ipod6g machine, compiled to WebAssembly.
 *
 * This file is NOT part of the QEMU build: build-web.sh compiles it
 * against the QEMU tree and links the object into the emscripten build
 * via --extra-ldflags, so no QEMU sources or build files are modified.
 *
 * It exports three functions to JavaScript:
 *
 *   qemu_wasm_fb_info()           -> address of the FbState struct
 *   qemu_wasm_request_frame()     -> schedule a framebuffer copy on the
 *                                    QEMU main loop (cross-thread safe)
 *   qemu_wasm_key_event(q, down)  -> inject a key on the QEMU main loop
 *
 * The browser main thread calls the latter two; both only touch
 * aio_bh_schedule_oneshot(), which is the documented thread-safe way to
 * poke the QEMU main loop.  The framebuffer copy itself runs as a
 * bottom half on the QEMU thread, converting the console surface to
 * RGBA8888 in a static buffer that JavaScript reads directly from the
 * (SharedArrayBuffer-backed) wasm heap.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "qemu/main-loop.h"
#include "qemu/aio.h"
#include "hw/qdev-core.h"
#include "hw/irq.h"
#include "ui/console.h"
#include "ui/surface.h"
#include "ui/input.h"

#include <emscripten.h>

#define FB_MAX_W 1024
#define FB_MAX_H 768

typedef struct FbState {
    uint32_t frame;     /* bumped after each completed copy */
    uint32_t width;
    uint32_t height;
    uint32_t pixels;    /* wasm heap address of the RGBA buffer */
} FbState;

static uint32_t fb_pixels[FB_MAX_W * FB_MAX_H];
static FbState fb_state;
static bool frame_pending;

EMSCRIPTEN_KEEPALIVE uint32_t qemu_wasm_fb_info(void)
{
    fb_state.pixels = (uint32_t)(uintptr_t)fb_pixels;
    return (uint32_t)(uintptr_t)&fb_state;
}

static void fb_copy_bh(void *opaque)
{
    QemuConsole *con = qemu_console_lookup_by_index(0);
    DisplaySurface *surface;
    int w, h, x, y;

    frame_pending = false;
    if (!con) {
        return;
    }
    graphic_hw_update(con);
    surface = qemu_console_surface(con);
    if (!surface) {
        return;
    }
    w = MIN(surface_width(surface), FB_MAX_W);
    h = MIN(surface_height(surface), FB_MAX_H);

    for (y = 0; y < h; y++) {
        uint32_t *src = (uint32_t *)(surface_data(surface)
                                     + y * surface_stride(surface));
        uint32_t *dst = &fb_pixels[y * w];

        for (x = 0; x < w; x++) {
            uint32_t pix = src[x];  /* xRGB8888 */

            /* to ABGR words = RGBA byte order for canvas ImageData */
            dst[x] = 0xff000000u
                     | (pix & 0x0000ff00u)
                     | ((pix >> 16) & 0xffu)
                     | ((pix & 0xffu) << 16);
        }
    }
    fb_state.width = w;
    fb_state.height = h;
    qatomic_inc(&fb_state.frame);

    /*
     * Publish the frame through MEMFS: the emscripten FS object is part
     * of the exported runtime, while direct heap views are not.
     */
    {
        FILE *f = fopen("/fbdump.tmp", "wb");

        if (f) {
            fwrite(&fb_state, sizeof(fb_state), 1, f);
            fwrite(fb_pixels, 4, (size_t)w * h, f);
            fclose(f);
            rename("/fbdump.tmp", "/fbdump");
        }
    }
}

EMSCRIPTEN_KEEPALIVE void qemu_wasm_request_frame(void)
{
    AioContext *ctx = qemu_get_aio_context();

    if (!ctx) {
        return;     /* QEMU main loop not initialised yet */
    }
    if (qatomic_xchg(&frame_pending, true)) {
        return;     /* one in flight is enough */
    }
    aio_bh_schedule_oneshot(ctx, fb_copy_bh, NULL);
}

static void key_bh(void *opaque)
{
    uintptr_t v = (uintptr_t)opaque;

    qemu_input_event_send_key_qcode(NULL, (QKeyCode)(v >> 1), v & 1);
}

EMSCRIPTEN_KEEPALIVE void qemu_wasm_key_event(int qcode, int down)
{
    AioContext *ctx = qemu_get_aio_context();
    uintptr_t v = ((uintptr_t)qcode << 1) | (down ? 1 : 0);

    if (ctx) {
        aio_bh_schedule_oneshot(ctx, key_bh, (void *)v);
    }
}

/* ---- absolute click-wheel input (touch front-ends) ---- */

static void wheel_bh(void *opaque)
{
    static DeviceState *wheel;
    uintptr_t v = (uintptr_t)opaque;
    int pos = (v >> 1) & 0x7f;
    bool touched = v & 1;

    if (!wheel) {
        Object *o = object_resolve_path_type("", "s5l8702-wheel", NULL);

        if (!o) {
            return;
        }
        wheel = DEVICE(o);
    }
    /* order matters: position first so the touch packet carries it */
    qemu_set_irq(qdev_get_gpio_in_named(wheel, "wheel-pos", 0), pos);
    qemu_set_irq(qdev_get_gpio_in_named(wheel, "wheel-touch", 0), touched);
}

EMSCRIPTEN_KEEPALIVE void qemu_wasm_wheel(int pos, int touched)
{
    AioContext *ctx = qemu_get_aio_context();
    uintptr_t v = (((uintptr_t)pos & 0x7f) << 1) | (touched ? 1 : 0);

    if (ctx) {
        aio_bh_schedule_oneshot(ctx, wheel_bh, (void *)v);
    }
}

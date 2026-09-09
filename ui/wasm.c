/*
 * QEMU wasm display/input backend (for emscripten builds).
 *
 * Needed because a browser build has none of the host toolkits (no
 * sdl/gtk/curses): the page in front of the emulator needs (a) the guest
 * framebuffer on the wasm heap where JS can read it, (b) a way to inject
 * key events from the browser main thread while the vCPU runs on a
 * pthread, and (c) a thread-safe quit. All three are provided here as
 * EMSCRIPTEN_KEEPALIVE exports.
 *
 * Exposes the guest framebuffer to JavaScript and accepts linux-keycode
 * input + a quit request from JavaScript. All JS entry points are safe to
 * call from the browser main thread while QEMU runs on a pthread:
 *  - the framebuffer is converted into a private XRGB8888 staging buffer
 *    that JS reads directly (a "dirty" flag guards redraws),
 *  - key events are pushed into a single-producer/single-consumer ring and
 *    drained on the QEMU main loop via a bottom half,
 *  - quit goes through the thread-safe qemu_system_shutdown_request().
 *
 * Enable with: qemu-system-arm ... -display wasm
 */
#include "qemu/osdep.h"

#include <emscripten.h>
#include <pixman.h>

#include "qemu/atomic.h"
#include "qemu/error-report.h"
#include "qemu/main-loop.h"
#include "qemu/timer.h"
#include "system/runstate.h"
#include "ui/console.h"
#include "ui/input.h"
#include "ui/surface.h"

/* ------------------------------------------------------------------ */
/* Framebuffer shared state (read by JS through exported accessors)    */
/* ------------------------------------------------------------------ */

static struct {
    int32_t width;
    int32_t height;
    int32_t stride;         /* bytes per row of staging buffer */
    int32_t dirty;          /* 1 => staging buffer changed since last take */
    uint64_t updates;       /* total blits (refresh-rate control) */
} wasm_fb;

static uint8_t *wasm_fb_data;   /* XRGB8888, width*height*4, wasm heap */

/*
 * Why a staging copy instead of exposing the console surface directly:
 * JS reads a fixed linear XRGB8888 layout at a stable address on the wasm
 * heap (HEAPU32 view), while the pixman surface's format/layout/stride is
 * an implementation detail and can change per guest mode switch.
 */

static void wasm_fb_free(void)
{
    g_free(wasm_fb_data);
    wasm_fb_data = NULL;
}

static void wasm_fb_resize(int width, int height)
{
    wasm_fb_free();
    wasm_fb.width = width;
    wasm_fb.height = height;
    wasm_fb.stride = width * 4;
    wasm_fb_data = g_malloc0_n(width, height * 4);
    qatomic_set(&wasm_fb.dirty, 1);
}

/* Copy the given region of the console surface into the staging buffer. */
static void wasm_fb_blit(DisplaySurface *surface, int x, int y, int w, int h)
{
    pixman_image_t *dst;

    if (!wasm_fb_data || !surface || !surface->image) {
        return;
    }
    if (x < 0 || y < 0 || w <= 0 || h <= 0 ||
        x + w > surface_width(surface) || y + h > surface_height(surface) ||
        surface_width(surface) != wasm_fb.width ||
        surface_height(surface) != wasm_fb.height) {
        return;
    }

    dst = pixman_image_create_bits(PIXMAN_x8r8g8b8, wasm_fb.width,
                                   wasm_fb.height,
                                   (uint32_t *)wasm_fb_data,
                                   wasm_fb.stride);
    if (!dst) {
        return;
    }

    pixman_image_composite(PIXMAN_OP_SRC, surface->image, NULL, dst,
                           x, y, 0, 0, x, y, w, h);
    pixman_image_unref(dst);

    wasm_fb.updates++;
    qatomic_set(&wasm_fb.dirty, 1);
}

/* Exported to JS: address of the staging buffer (wasm heap offset) */
EMSCRIPTEN_KEEPALIVE
uint8_t *wasm_fb_ptr(void)
{
    return wasm_fb_data;
}

EMSCRIPTEN_KEEPALIVE
int32_t wasm_fb_width(void)
{
    return wasm_fb.width;
}

EMSCRIPTEN_KEEPALIVE
int32_t wasm_fb_height(void)
{
    return wasm_fb.height;
}

EMSCRIPTEN_KEEPALIVE
int32_t wasm_fb_stride(void)
{
    return wasm_fb.stride;
}

/* Returns 1 (and clears the flag) when the framebuffer was updated. */
EMSCRIPTEN_KEEPALIVE
int32_t wasm_fb_take_dirty(void)
{
    return qatomic_xchg(&wasm_fb.dirty, 0);
}

/* Total number of blits performed (diagnostics/perf). */
EMSCRIPTEN_KEEPALIVE
uint64_t wasm_fb_updates(void)
{
    return wasm_fb.updates;
}

/*
 * Per-TB execution statistics (diagnostics): fed once per executed TB
 * from the TCI interpreter's TB header op (see the wasm-tci-tb-chaining
 * patch); wasm_tbs()/wasm_insns() are what the page and the benchmark
 * tooling read as the guest-throughput metric.
 */
static struct {
    uint64_t tbs;
    uint64_t insns;
} wasm_tb_stats;

void wasm_tb_account(unsigned insns)
{
    wasm_tb_stats.tbs++;
    wasm_tb_stats.insns += insns;
}

EMSCRIPTEN_KEEPALIVE
uint64_t wasm_tbs(void)
{
    return wasm_tb_stats.tbs;
}

EMSCRIPTEN_KEEPALIVE
uint64_t wasm_insns(void)
{
    return wasm_tb_stats.insns;
}

/* Diagnostics: memory-subsystem counters (see include/qemu/wasm-diag.h). */
#include "qemu/wasm-diag.h"
EMSCRIPTEN_KEEPALIVE
uint64_t wasm_memstat(int32_t idx)
{
    return idx >= 0 && idx < WASM_DIAG_N ? wasm_diag_stat[idx] : 0;
}

/* Guest virtual clock in ns (diagnostics: boot progress). */
EMSCRIPTEN_KEEPALIVE
int64_t wasm_vclock(void)
{
    return qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
}

/* ------------------------------------------------------------------ */
/* Key input: SPSC ring filled from the JS main thread, drained by BH  */
/* ------------------------------------------------------------------ */

#define KEY_RING_SIZE 64        /* power of two */

static struct {
    uint32_t lnx;
    uint8_t down;
} key_ring[KEY_RING_SIZE];
static unsigned int key_ring_head;   /* consumer (main loop) */
static unsigned int key_ring_tail;   /* producer (JS thread) */

static QEMUBH *key_bh;

/*
 * Why the ring + bottom half: wasm_send_key() runs on the browser main
 * thread, but qemu_input_event_send_* must run on the QEMU main loop under
 * the BQL. The lock-free SPSC ring hands the events over; the BH drains
 * them in QEMU's context. (A mutex would need cross-thread blocking,
 * which Asyncify cannot do on the JS main thread.)
 */

static void wasm_key_bh(void *opaque)
{
    while (qatomic_read(&key_ring_tail) != key_ring_head) {
        unsigned int i = key_ring_head & (KEY_RING_SIZE - 1);
        qemu_input_event_send_key_linux(NULL, key_ring[i].lnx,
                                        key_ring[i].down);
        key_ring_head++;
    }
}

/* Exported to JS: enqueue a key event (linux keycode). */
EMSCRIPTEN_KEEPALIVE
void wasm_send_key(uint32_t lnx, int32_t down)
{
    unsigned int tail = qatomic_read(&key_ring_tail);
    unsigned int head = qatomic_read(&key_ring_head);

    if (tail - head >= KEY_RING_SIZE) {
        return; /* full: drop */
    }
    key_ring[tail & (KEY_RING_SIZE - 1)].lnx = lnx;
    key_ring[tail & (KEY_RING_SIZE - 1)].down = !!down;
    qatomic_set(&key_ring_tail, tail + 1);

    qemu_bh_schedule(key_bh);
}

/* ------------------------------------------------------------------ */
/* Quit: thread-safe shutdown request                                  */
/* ------------------------------------------------------------------ */

EMSCRIPTEN_KEEPALIVE
void wasm_quit(void)
{
    qemu_system_shutdown_request(SHUTDOWN_CAUSE_HOST_QMP_QUIT);
}

/* ------------------------------------------------------------------ */
/* DisplayChangeListener                                               */
/* ------------------------------------------------------------------ */

#define WASM_REFRESH_INTERVAL_MIN  16    /* ~60 fps when busy   */
#define WASM_REFRESH_INTERVAL_MAX  500   /* idle polling        */

static void wasm_refresh(DisplayChangeListener *dcl)
{
    uint64_t before = wasm_fb.updates;

    /*
     * Why polling (and why adaptive): the pmb887x LCD model has no dirty
     * interrupt — it only repaints while GraphicHwOps.gfx_update is called,
     * so the display goes stale unless we poll. Polling at max rate would
     * burn the single-threaded-ish browser main loop; the adaptive interval
     * backs off to 500 ms when nothing changed and spins up to ~60 fps as
     * soon as the guest animates (boot splash, menus).
     */
    qemu_console_hw_update(dcl->con);

    if (wasm_fb.updates != before) {
        if (dcl->update_interval > WASM_REFRESH_INTERVAL_MIN) {
            qemu_console_listener_set_refresh(
                dcl, dcl->update_interval / 2 > WASM_REFRESH_INTERVAL_MIN
                        ? dcl->update_interval / 2
                        : WASM_REFRESH_INTERVAL_MIN);
        }
    } else if (dcl->update_interval < WASM_REFRESH_INTERVAL_MAX) {
        qemu_console_listener_set_refresh(
            dcl, dcl->update_interval * 2 < WASM_REFRESH_INTERVAL_MAX
                    ? dcl->update_interval * 2
                    : WASM_REFRESH_INTERVAL_MAX);
    }
}

static void wasm_gfx_update(DisplayChangeListener *dcl,
                            int x, int y, int w, int h)
{
    DisplaySurface *surface = qemu_console_surface(dcl->con);

    wasm_fb_blit(surface, x, y, w, h);
}

static void wasm_gfx_switch(DisplayChangeListener *dcl,
                            struct DisplaySurface *new_surface)
{
    if (!new_surface) {
        return;
    }
    if (wasm_fb.width != surface_width(new_surface) ||
        wasm_fb.height != surface_height(new_surface)) {
        wasm_fb_resize(surface_width(new_surface), surface_height(new_surface));
    }
    wasm_fb_blit(new_surface, 0, 0, surface_width(new_surface),
                 surface_height(new_surface));
}

static const DisplayChangeListenerOps wasm_dcl_ops = {
    .dpy_name             = "wasm",
    .dpy_refresh          = wasm_refresh,
    .dpy_gfx_update       = wasm_gfx_update,
    .dpy_gfx_switch       = wasm_gfx_switch,
};

static DisplayChangeListener wasm_dcl;

static void wasm_display_init(DisplayState *ds, DisplayOptions *opts)
{
    QemuConsole *con;

    key_bh = qemu_bh_new(wasm_key_bh, NULL);

    /* listen on the first (graphic) console: the phone LCD */
    con = qemu_console_lookup_by_index(0);
    if (!con) {
        error_report("wasm display: no console found");
        exit(1);
    }
    qemu_console_register_listener(con, &wasm_dcl, &wasm_dcl_ops);
    qemu_console_listener_set_refresh(&wasm_dcl, WASM_REFRESH_INTERVAL_MIN);
}

static QemuDisplay qemu_display_wasm = {
    .type   = DISPLAY_TYPE_WASM,
    .init   = wasm_display_init,
};

static void register_wasm_display(void)
{
    qemu_display_register(&qemu_display_wasm);
}
type_init(register_wasm_display);

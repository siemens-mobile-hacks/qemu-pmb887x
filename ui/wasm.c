/*
 * QEMU wasm display/input backend (for emscripten builds).
 *
 * A browser build has none of the host toolkits (no sdl/gtk/curses), so
 * this file gives the page in front of the emulator, as
 * EMSCRIPTEN_KEEPALIVE exports that are safe to call from the browser main
 * thread while QEMU runs on pthreads:
 *  - the guest framebuffer, converted into a private XRGB8888 staging
 *    buffer on the wasm heap that JS reads directly (a "dirty" flag guards
 *    redraws),
 *  - linux-keycode input, pushed into a single-producer/single-consumer
 *    ring and drained on the QEMU main loop via a bottom half,
 *  - a quit request, through the thread-safe qemu_system_shutdown_request(),
 *  - the accessory plugged into the phone, swapped by a bottom half,
 *  - a network port on the GSM cell's hub, with a ring each way,
 *  - progress counters and the cell's state, read as racy snapshots or
 *    through a bottom half.
 *
 * Enable with: qemu-system-arm ... -display wasm
 */
#include "qemu/osdep.h"

#include <emscripten.h>
#include <pixman.h>

#include "qemu/atomic.h"
#include "chardev/char.h"
#include "qemu/error-report.h"
#include "qemu/main-loop.h"
#include "qemu/timer.h"
#include "exec/icount.h"
#include "hw/core/cpu.h"
#include "hw/arm/pmb887x/card-assist.h"
#include "hw/arm/pmb887x/dsp/afe-mic.h"
#include "hw/misc/phone/accessory-connector.h"
#include "net/hub.h"
#include "net/net.h"
#include "qapi/error.h"
#include "qapi/qapi-commands-block.h"
#include "qemu/seqlock.h"
#include "system/cpu-timers.h"
#include "system/cpu-timers-internal.h"
#include "system/runstate.h"
#include "ui/console.h"
#include "ui/input.h"
#include "ui/surface.h"
#include "console-priv.h"

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

static void wasm_fb_resize(int width, int height)
{
    g_free(wasm_fb_data);
    wasm_fb_data = NULL;
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
 * Guest instructions executed on a board without icount, counted by the
 * wasm64 TB prologue (w64_acct_addr).  Under icount the count is exact in
 * the icount state and wasm_insns() reads it there instead.
 */
#ifndef wasm_guest_insns
uint64_t wasm_guest_insns;
#endif

/*
 * Guest instructions executed so far.  Under icount: the finished
 * slices in timers_state.qemu_icount plus what the running slice has
 * consumed of its budget (the same arithmetic as icount_get_executed).
 * This is read from the JS main thread while the vCPU runs, so it is a
 * racy snapshot that can be off by up to one slice at a slice boundary
 * - adequate for the 0.5-1 s progress meters that use it.
 */
EMSCRIPTEN_KEEPALIVE
uint64_t wasm_insns(void)
{
    if (icount_enabled()) {
        CPUState *cpu = first_cpu;
        int64_t slice = 0;

        if (cpu) {
            slice = cpu->icount_budget -
                    (cpu->neg.icount_decr.u16.low + cpu->icount_extra);
        }
        return qatomic_read(&timers_state.qemu_icount) + slice;
    }
    return wasm_guest_insns;
}

/* vCPU idle entries: the page's halt rate and stall detector */
EMSCRIPTEN_KEEPALIVE
uint64_t wasm_halts(void)
{
    return qatomic_read(&wasm_vcpu_idle_count);
}

/* Guest virtual clock in ns (diagnostics: boot progress). */
EMSCRIPTEN_KEEPALIVE
int64_t wasm_vclock(void)
{
    return qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
}

/*
 * 1 while the guest is still in its vcpu-timers-bank window: it catches up
 * on time lost to the host, so v/wall below 1 is not yet "slow".
 */
EMSCRIPTEN_KEEPALIVE
int32_t wasm_clock_banked(void)
{
    return cpu_clock_banked();
}

/* ------------------------------------------------------------------ */
/* Key input: SPSC ring filled from the JS main thread, drained by BH  */
/* ------------------------------------------------------------------ */

#define KEY_RING_SIZE 256       /* power of two */

static struct {
    uint32_t lnx;
    uint8_t down;
    int64_t after;      /* ns of guest time after the edge before, or -1 */
} key_ring[KEY_RING_SIZE];
static unsigned int key_ring_head;   /* consumer (main loop) */
static unsigned int key_ring_tail;   /* producer (JS thread) */

static QEMUBH *key_bh;
static QEMUTimer *key_timer;
/* guest time the last edge was handed to the guest */
static int64_t key_last_at;
/*
 * Release-published once @key_bh exists.  A separate flag rather than an
 * atomic read of @key_bh itself: QEMUBH is opaque here, and qatomic's
 * typeof_strip_qual() needs a complete type to form (expr)+0.
 */
static int key_bh_ready;

/*
 * Why the ring + bottom half: wasm_send_key() runs on the browser main
 * thread, but qemu_input_event_send_* must run on the QEMU main loop under
 * the BQL. The lock-free SPSC ring hands the events over; the BH drains
 * them in QEMU's context. (A mutex would need cross-thread blocking,
 * which Asyncify cannot do on the JS main thread.)
 */

/*
 * An edge with a spacing waits on a QEMU_CLOCK_VIRTUAL timer until that
 * much guest time has passed since the edge before it.  The page types a
 * letter as taps a few tens of ms apart, and how fast the phone runs
 * against the page's clock varies with the host: timing them on the page
 * and handing them over as they come would let a late hand-over shorten a
 * hold or a gap the firmware then misses.  An edge that arrives late still
 * goes out at once; the ones after it keep their spacing from it.
 */
static void wasm_key_pump(void *opaque)
{
    while (qatomic_load_acquire(&key_ring_tail) != key_ring_head) {
        unsigned int i = key_ring_head & (KEY_RING_SIZE - 1);
        int64_t now = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);

        if (key_ring[i].after >= 0 && key_last_at + key_ring[i].after > now) {
            timer_mod(key_timer, key_last_at + key_ring[i].after);
            return;
        }
        qemu_input_event_send_key_linux(NULL, key_ring[i].lnx,
                                        key_ring[i].down);
        qatomic_set(&key_last_at, now);
        qatomic_store_release(&key_ring_head, key_ring_head + 1);
    }
}

static void wasm_key_bh(void *opaque)
{
    /* an edge waiting on the timer keeps the ones behind it */
    if (!timer_pending(key_timer)) {
        wasm_key_pump(NULL);
    }
}

static void wasm_key_push(uint32_t lnx, int32_t down, int64_t after)
{
    unsigned int tail, head;
    /*
     * The export is callable from JS the moment the module instantiates,
     * which is long before wasm_display_init() creates the bottom half --
     * and the page's keypad is live from its first render, so a pointer
     * resting where a key lands (or a touch during the boot) delivers a
     * key event to a machine that does not exist yet.
     *
     * That used to take the whole module down.  With a NULL @bh,
     * qemu_bh_schedule()'s atomic on bh->flags succeeds -- offset 40 is a
     * perfectly valid wasm address -- so nothing catches it there; bh->ctx
     * then reads address 0 and the list insert at ctx+184 traps with
     * "memory access out of bounds".  A guest key press cannot be
     * delivered before there is a guest, so drop it.
     */
    if (!qatomic_load_acquire(&key_bh_ready)) {
        return;
    }

    tail = qatomic_read(&key_ring_tail);
    head = qatomic_read(&key_ring_head);

    if (tail - head >= KEY_RING_SIZE) {
        return; /* full: drop */
    }
    key_ring[tail & (KEY_RING_SIZE - 1)].lnx = lnx;
    key_ring[tail & (KEY_RING_SIZE - 1)].down = !!down;
    key_ring[tail & (KEY_RING_SIZE - 1)].after = after;
    /* release: publish the slot stores before the tail, so the consumer
     * can never observe the new tail with stale slot contents */
    qatomic_store_release(&key_ring_tail, tail + 1);

    qemu_bh_schedule(key_bh);
}

/* Exported to JS: enqueue a key event (linux keycode), for the guest now. */
EMSCRIPTEN_KEEPALIVE
void wasm_send_key(uint32_t lnx, int32_t down)
{
    wasm_key_push(lnx, down, -1);
}

/*
 * Exported to JS: enqueue a key event for when @after_us of guest time
 * has passed since the edge queued before it went to the guest (see
 * wasm_key_pump).
 */
EMSCRIPTEN_KEEPALIVE
void wasm_send_key_after(uint32_t lnx, int32_t down, uint32_t after_us)
{
    wasm_key_push(lnx, down, (int64_t)after_us * 1000);
}

/* Exported to JS: edges queued and not yet handed to the guest. */
EMSCRIPTEN_KEEPALIVE
uint32_t wasm_keys_queued(void)
{
    return qatomic_load_acquire(&key_ring_tail) -
           qatomic_load_acquire(&key_ring_head);
}

/* Exported to JS: the guest time (ns) the last edge was handed over. */
EMSCRIPTEN_KEEPALIVE
int64_t wasm_key_last_at(void)
{
    return qatomic_read(&key_last_at);
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
/* Accessory: what is plugged into the phone's system connector        */
/* ------------------------------------------------------------------ */

/*
 * The page writes a profile name of the board's accessory-connector
 * ("headset-stereo", or the unplugged one, "none") into this buffer and
 * calls wasm_accessory_plug(). The name is copied there, on the JS thread,
 * so the page may reuse the buffer at once; the QOM property is set by a
 * bottom half on the main loop, under the BQL. Replacing one accessory
 * with another goes through the connector's own unplugged gap.
 */
static char accessory_name[64];

EMSCRIPTEN_KEEPALIVE
char *wasm_accessory_buf(void)
{
    return accessory_name;
}

static void wasm_accessory_bh(void *opaque)
{
    g_autofree char *name = opaque;
    Object *connector = object_resolve_path_type("", TYPE_ACCESSORY_CONNECTOR,
                                                 NULL);
    Error *err = NULL;

    if (!connector) {
        error_report("wasm: the board has no accessory connector");
        return;
    }
    if (!object_property_set_str(connector, "accessory", name, &err)) {
        error_report_err(err);
    }
}

EMSCRIPTEN_KEEPALIVE
void wasm_accessory_plug(void)
{
    /* the machine exists once the display does (see wasm_send_key) */
    if (!qatomic_load_acquire(&key_bh_ready)) {
        return;
    }
    aio_bh_schedule_oneshot(qemu_get_aio_context(), wasm_accessory_bh,
                            g_strndup(accessory_name,
                                      sizeof(accessory_name) - 1));
}

/* ------------------------------------------------------------------ */
/* Memory card: eject / insert the SD image                            */
/* ------------------------------------------------------------------ */

/*
 * WASM PORT (removable): the page boots with "-drive if=sd,format=raw,
 * file=<MEMFS path>" and syncs that image with a host folder. To change it
 * under a running phone it calls wasm_card_eject(), waits until
 * wasm_card_done() has counted it, reads or rewrites the MEMFS file, writes
 * its path into wasm_card_buf() and calls wasm_card_insert(). Each runs as a
 * bottom half under the BQL: the card leaves the sd-slot (through the card
 * assist when the page made one, which tells the firmware: PMB887X-WEB
 * FEATURE, hw/arm/pmb887x/card-assist.c), then the drive "sd0" is closed;
 * an insert opens the new image, then puts the card back.
 */
static char card_path[512];
static int32_t card_done;

EMSCRIPTEN_KEEPALIVE
char *wasm_card_buf(void)
{
    return card_path;
}

/* write commands the card answered so far: changes when the phone writes */
EMSCRIPTEN_KEEPALIVE
int32_t wasm_card_writes(void)
{
    return qatomic_read(&pmb887x_card_writes);
}

/*
 * what the firmware knows of the card (card-assist.c): 0 in (read again
 * after the last insert), 1 out, 2 going out (the firmware has not seen the
 * removal yet), 3 reading (inserted, the firmware has not read it yet), 4 not
 * seen (the assist gave up telling the firmware)
 */
EMSCRIPTEN_KEEPALIVE
int32_t wasm_card_state(void)
{
    return qatomic_read(&pmb887x_card_state);
}

/* operations finished so far (eject and insert count one each) */
EMSCRIPTEN_KEEPALIVE
int32_t wasm_card_done(void)
{
    return qatomic_read(&card_done);
}

static void wasm_card_bh(void *opaque)
{
    g_autofree char *path = opaque;
    Error *err = NULL;

    if (path) {
        qmp_blockdev_change_medium("sd0", NULL, path, "raw", false, false,
                                   false, 0, &err);
        if (!err) {
            pmb887x_card_set_inserted(true, &err);
        }
    } else if (pmb887x_card_set_inserted(false, &err)) {
        qmp_eject("sd0", NULL, true, true, &err);
    }
    if (err) {
        error_report_err(err);
    }
    qatomic_inc(&card_done);
}

EMSCRIPTEN_KEEPALIVE
void wasm_card_eject(void)
{
    if (!qatomic_load_acquire(&key_bh_ready)) {
        return;
    }
    aio_bh_schedule_oneshot(qemu_get_aio_context(), wasm_card_bh, NULL);
}

/*
 * clean unmount before a change (card-assist.c): the phone writes its FAT
 * and drops its cache, as with "Eject card", so the change that follows
 * gives no "Data lost" note. wasm_card_unmount_state(): 0 none, 1 pending,
 * 2 sent, 3 not possible (no card assist, no GBS_SendMessage found, no card
 * server)
 */
static void wasm_card_unmount_bh(void *opaque)
{
    pmb887x_card_unmount();
}

EMSCRIPTEN_KEEPALIVE
void wasm_card_unmount(void)
{
    if (!qatomic_load_acquire(&key_bh_ready)) {
        return;
    }
    qatomic_set(&pmb887x_card_unmount_state, 1);
    aio_bh_schedule_oneshot(qemu_get_aio_context(), wasm_card_unmount_bh, NULL);
}

EMSCRIPTEN_KEEPALIVE
int32_t wasm_card_unmount_state(void)
{
    return qatomic_read(&pmb887x_card_unmount_state);
}

EMSCRIPTEN_KEEPALIVE
void wasm_card_insert(void)
{
    if (!qatomic_load_acquire(&key_bh_ready)) {
        return;
    }
    aio_bh_schedule_oneshot(qemu_get_aio_context(), wasm_card_bh,
                            g_strndup(card_path, sizeof(card_path) - 1));
}

/* ------------------------------------------------------------------ */
/* Network: the page's port on the GSM cell's hub                      */
/* ------------------------------------------------------------------ */

/*
 * WASM PORT (removable): a browser gives slirp no sockets, so the page runs
 * the IP side itself. It boots the cell with "-netdev hubport,id=net0,
 * hubid=0" and calls wasm_net_attach(), which adds the page as a second port
 * of hub 0. Frames the hub delivers here wait in net_out until the JS thread
 * takes them with wasm_net_take(); frames the page hands over with
 * wasm_net_put() wait in net_in for a bottom half that sends them under the
 * BQL. Both rings have one producer and one consumer, like key_ring.
 */
#define NET_FRAME_MAX 1600
#define NET_RING_SIZE 64        /* power of two */

typedef struct {
    struct {
        uint16_t len;
        uint8_t data[NET_FRAME_MAX];
    } slot[NET_RING_SIZE];
    unsigned int head;
    unsigned int tail;
} WasmNetRing;

static WasmNetRing net_out, net_in;
static uint8_t net_buf[NET_FRAME_MAX];  /* the page's staging buffer */
static NetClientState *net_nc;
static QEMUBH *net_bh;
static int net_ready;

static bool net_ring_push(WasmNetRing *r, const uint8_t *data, size_t len)
{
    unsigned int tail = qatomic_read(&r->tail);

    if (len > NET_FRAME_MAX ||
        tail - qatomic_load_acquire(&r->head) >= NET_RING_SIZE) {
        return false;
    }
    r->slot[tail & (NET_RING_SIZE - 1)].len = len;
    memcpy(r->slot[tail & (NET_RING_SIZE - 1)].data, data, len);
    qatomic_store_release(&r->tail, tail + 1);
    return true;
}

static size_t net_ring_pop(WasmNetRing *r, uint8_t *out)
{
    unsigned int head = qatomic_read(&r->head);
    size_t len;

    if (qatomic_load_acquire(&r->tail) == head) {
        return 0;
    }
    len = r->slot[head & (NET_RING_SIZE - 1)].len;
    memcpy(out, r->slot[head & (NET_RING_SIZE - 1)].data, len);
    qatomic_store_release(&r->head, head + 1);
    return len;
}

static ssize_t wasm_net_receive(NetClientState *nc, const uint8_t *buf,
                                size_t size)
{
    net_ring_push(&net_out, buf, size);
    return size;
}

static NetClientInfo wasm_net_info = {
    .type = NET_CLIENT_DRIVER_NONE,
    .size = sizeof(NetClientState),
    .receive = wasm_net_receive,
};

static void wasm_net_bh(void *opaque)
{
    uint8_t frame[NET_FRAME_MAX];
    size_t len;

    while ((len = net_ring_pop(&net_in, frame))) {
        qemu_send_packet(net_nc, frame, len);
    }
}

static void wasm_net_attach_bh(void *opaque)
{
    if (net_nc) {
        return;
    }
    net_nc = qemu_new_net_client(&wasm_net_info, NULL, "wasm", "wasm-net");
    net_hub_add_port(0, NULL, net_nc);
    net_bh = qemu_bh_new(wasm_net_bh, NULL);
    qatomic_store_release(&net_ready, 1);
}

/* Exported to JS: 0 while there is no machine yet (see wasm_send_key). */
EMSCRIPTEN_KEEPALIVE
int32_t wasm_net_attach(void)
{
    if (!qatomic_load_acquire(&key_bh_ready)) {
        return 0;
    }
    aio_bh_schedule_oneshot(qemu_get_aio_context(), wasm_net_attach_bh, NULL);
    return 1;
}

EMSCRIPTEN_KEEPALIVE
uint8_t *wasm_net_buf(void)
{
    return net_buf;
}

/* Exported to JS: the next frame for the page, copied to net_buf; 0 if none. */
EMSCRIPTEN_KEEPALIVE
int32_t wasm_net_take(void)
{
    return net_ring_pop(&net_out, net_buf);
}

/* Exported to JS: send the frame of @len bytes in net_buf; 0 if dropped. */
EMSCRIPTEN_KEEPALIVE
int32_t wasm_net_put(int32_t len)
{
    if (!qatomic_load_acquire(&net_ready) || len <= 0 ||
        !net_ring_push(&net_in, net_buf, len)) {
        return 0;
    }
    qemu_bh_schedule(net_bh);
    return 1;
}

/*
 * WASM PORT (removable): the phone as the GSM cell sees it, for the page's
 * indicator: the gsm-bts object's read-only "ms-state" (GSM_BTS_MS_* in
 * hw/gsm/gsm-bts.h: 1 registered, 2 GPRS attached, 4 PDP context active).
 * The object is read under the BQL in a bottom half; the export returns
 * what the last one read and asks for the next, so a page polling it once
 * a second sees a change within two polls.
 */
static int gsm_state;
static int gsm_bh_pending;

static void wasm_gsm_state_bh(void *opaque)
{
    static Object *bts;

    if (!bts) {
        bts = object_resolve_path_type("", "gsm-bts", NULL);
    }
    if (bts) {
        qatomic_set(&gsm_state,
                    object_property_get_uint(bts, "ms-state", NULL));
    }
    qatomic_store_release(&gsm_bh_pending, 0);
}

/* Exported to JS: GSM_BTS_MS_* bits; 0 before the machine or with no cell. */
EMSCRIPTEN_KEEPALIVE
int32_t wasm_gsm_state(void)
{
    if (qatomic_load_acquire(&key_bh_ready) &&
        !qatomic_xchg(&gsm_bh_pending, 1)) {
        aio_bh_schedule_oneshot(qemu_get_aio_context(), wasm_gsm_state_bh,
                                NULL);
    }
    return qatomic_read(&gsm_state);
}

/* ------------------------------------------------------------------ */
/* GSM relay: the cell's link to the PC relay, through the page        */
/* ------------------------------------------------------------------ */

/*
 * WASM PORT (removable): the gsm-bts cell's services (hw/gsm/gsm-relay.c,
 * doc/gsm-relay.md) talk to a relay server over a chardev; a browser has no TCP,
 * so the page boots "-chardev wasm-gsm,id=gsmrelay -object gsm-bts,...,
 * relay=gsmrelay" and carries the bytes over a WebSocket (site/gsm-relay.js).
 * What the cell writes waits in relay_out until the JS thread takes it with
 * wasm_gsmrelay_take(); what the page puts with wasm_gsmrelay_put() waits in
 * relay_in for a bottom half that hands it to the cell under the BQL. The
 * page says when its WebSocket opens or closes (wasm_gsmrelay_open), which
 * the cell sees as the chardev's OPENED/CLOSED. Byte rings with one
 * producer and one consumer each, like key_ring.
 */
#define RELAY_RING_SIZE 65536   /* power of two */
#define RELAY_BUF_SIZE  4096    /* the page's staging buffer */

typedef struct {
    uint8_t data[RELAY_RING_SIZE];
    unsigned int head;
    unsigned int tail;
} WasmByteRing;

static WasmByteRing relay_out, relay_in;
static uint8_t relay_buf[RELAY_BUF_SIZE];
static Chardev *relay_chr;
static QEMUBH *relay_bh;
static int relay_want_open;     /* what the page asked for */
static bool relay_is_open;      /* what the cell was told (BQL) */

static size_t byte_ring_push(WasmByteRing *r, const uint8_t *data, size_t len)
{
    unsigned int tail = qatomic_read(&r->tail);
    size_t room = RELAY_RING_SIZE - (tail - qatomic_load_acquire(&r->head));
    size_t i;

    len = MIN(len, room);
    for (i = 0; i < len; i++) {
        r->data[(tail + i) & (RELAY_RING_SIZE - 1)] = data[i];
    }
    qatomic_store_release(&r->tail, tail + len);
    return len;
}

static size_t byte_ring_pop(WasmByteRing *r, uint8_t *out, size_t max)
{
    unsigned int head = qatomic_read(&r->head);
    size_t len = MIN(max, qatomic_load_acquire(&r->tail) - head);
    size_t i;

    for (i = 0; i < len; i++) {
        out[i] = r->data[(head + i) & (RELAY_RING_SIZE - 1)];
    }
    qatomic_store_release(&r->head, head + len);
    return len;
}

#define TYPE_CHARDEV_WASM_GSM "chardev-wasm-gsm"

static int wasm_gsm_chr_write(Chardev *chr, const uint8_t *buf, int len)
{
    /*
     * No page: dropped. A full ring (a page that has not read for a while)
     * takes what fits and says so; the cell keeps the rest and its frames
     * stay whole (gsm_relay_flush).
     */
    if (!relay_is_open) {
        return len;
    }
    return byte_ring_push(&relay_out, buf, len);
}

static void wasm_gsmrelay_bh(void *opaque)
{
    uint8_t chunk[RELAY_BUF_SIZE];
    bool open = qatomic_load_acquire(&relay_want_open);
    size_t len;

    if (!relay_chr) {
        return;
    }
    if (open != relay_is_open) {
        relay_is_open = open;
        if (!open) {
            /* bytes of the old link are not the new link's */
            qatomic_store_release(&relay_in.head, qatomic_read(&relay_in.tail));
        }
        qemu_chr_be_event(relay_chr, open ? CHR_EVENT_OPENED : CHR_EVENT_CLOSED);
    }
    while (relay_is_open) {
        size_t room = qemu_chr_be_can_write(relay_chr);

        if (!room || !(len = byte_ring_pop(&relay_in, chunk, MIN(room, sizeof(chunk))))) {
            break;
        }
        qemu_chr_be_write(relay_chr, chunk, len);
    }
}

/* not opened here: the cell hears OPENED when the page's WebSocket opens */
static bool wasm_gsm_chr_open(Chardev *chr, ChardevBackend *backend,
                              Error **errp)
{
    if (relay_chr) {
        error_setg(errp, "only one wasm-gsm chardev");
        return false;
    }
    relay_chr = chr;
    relay_bh = qemu_bh_new(wasm_gsmrelay_bh, NULL);
    return true;
}

static void wasm_gsm_chr_class_init(ObjectClass *oc, const void *data)
{
    ChardevClass *cc = CHARDEV_CLASS(oc);

    cc->chr_open = wasm_gsm_chr_open;
    cc->chr_write = wasm_gsm_chr_write;
}

static const TypeInfo wasm_gsm_chr_type = {
    .name = TYPE_CHARDEV_WASM_GSM,
    .parent = TYPE_CHARDEV,
    .class_init = wasm_gsm_chr_class_init,
};

static void register_wasm_gsm_chr(void)
{
    type_register_static(&wasm_gsm_chr_type);
}
type_init(register_wasm_gsm_chr);

/* Exported to JS: 1 once the machine has the chardev, else 0. */
EMSCRIPTEN_KEEPALIVE
int32_t wasm_gsmrelay_ready(void)
{
    return qatomic_load_acquire(&key_bh_ready) && relay_bh ? 1 : 0;
}

EMSCRIPTEN_KEEPALIVE
uint8_t *wasm_gsmrelay_buf(void)
{
    return relay_buf;
}

/* Exported to JS: the page's link to the relay is up (1) or down (0). */
EMSCRIPTEN_KEEPALIVE
void wasm_gsmrelay_open(int32_t open)
{
    if (!relay_bh) {
        return;
    }
    qatomic_store_release(&relay_want_open, !!open);
    qemu_bh_schedule(relay_bh);
}

/* Exported to JS: bytes for the relay copied to relay_buf, 0 if none. */
EMSCRIPTEN_KEEPALIVE
int32_t wasm_gsmrelay_take(void)
{
    return byte_ring_pop(&relay_out, relay_buf, sizeof(relay_buf));
}

/* Exported to JS: hand the cell @len bytes of relay_buf; returns how many fit. */
EMSCRIPTEN_KEEPALIVE
int32_t wasm_gsmrelay_put(int32_t len)
{
    size_t n;

    if (!relay_bh || len <= 0 || len > RELAY_BUF_SIZE) {
        return 0;
    }
    n = byte_ring_push(&relay_in, relay_buf, len);
    qemu_bh_schedule(relay_bh);
    return n;
}

/* ------------------------------------------------------------------ */
/* Microphone: the page's getUserMedia into the phone's mic ADC        */
/* ------------------------------------------------------------------ */

/*
 * WASM PORT (removable): the page captures the microphone at 8 kHz
 * (site/mic.js), writes signed 16-bit samples to mic_buf and calls
 * wasm_mic_put(); the AFE model (hw/arm/pmb887x/dsp/peripheral/afe.c) takes
 * them as the mic ADC's input (afe-mic.c). Nothing put = silence, as before.
 */

#define MIC_BUF_SAMPLES 2048
static int16_t mic_buf[MIC_BUF_SAMPLES];

EMSCRIPTEN_KEEPALIVE
int16_t *wasm_mic_buf(void)
{
    return mic_buf;
}

/* Exported to JS: hand the ADC @count samples of mic_buf; returns how many it kept. */
EMSCRIPTEN_KEEPALIVE
int32_t wasm_mic_put(int32_t count)
{
    if (count <= 0 || count > MIC_BUF_SAMPLES) {
        return 0;
    }
    return pmb887x_afe_mic_push(mic_buf, count);
}

/* ------------------------------------------------------------------ */
/* DisplayChangeListener                                               */
/* ------------------------------------------------------------------ */

#define WASM_REFRESH_INTERVAL_MIN  16    /* ~60 fps when busy   */
#define WASM_REFRESH_INTERVAL_MAX  500   /* idle polling        */

/*
 * Why polling (and why adaptive): the pmb887x LCD model has no dirty
 * interrupt — it only repaints while GraphicHwOps.gfx_update is called,
 * so the display goes stale unless we poll. Polling at max rate would
 * burn the single-threaded-ish browser main loop; the adaptive interval
 * backs off to 500 ms when nothing changed and spins up to ~60 fps as
 * soon as the guest animates (boot splash, menus).
 *
 * Returns the interval to poll at next, after one at @interval.
 */
static int wasm_refresh_poll(DisplayChangeListener *dcl, int interval)
{
    uint64_t before = wasm_fb.updates;

    qemu_console_hw_update(dcl->con);

    if (wasm_fb.updates != before) {
        return MAX(interval / 2, WASM_REFRESH_INTERVAL_MIN);
    }
    return MIN(interval * 2, WASM_REFRESH_INTERVAL_MAX);
}

static void wasm_refresh(DisplayChangeListener *dcl)
{
    int interval = wasm_refresh_poll(dcl, dcl->update_interval);

    if (interval != dcl->update_interval) {
        qemu_console_listener_set_refresh(dcl, interval);
    }
}

/*
 * WASM PORT FAST PATH (removable): the refresh as a timer of the virtual
 * clock, where the vCPU thread runs those (qemu_virtual_timers_on_vcpu).
 * The console's own timer runs on the main loop under the BQL, which the
 * vCPU takes thousands of times a second: on a phone, whose scheduler
 * keeps the main loop on a slow core, the vCPU waited out every refresh
 * (3.3 % of its time on a Pixel 8 Pro, and the wake after it).  On its
 * own thread the refresh meets nobody.  It follows guest time, so a
 * guest at half speed is also polled half as often.
 *
 * Remove: this timer and wasm_vcpu_dcl_ops; wasm_dcl_ops serves alone.
 */
static QEMUTimer *wasm_refresh_timer;
static int wasm_refresh_interval = WASM_REFRESH_INTERVAL_MIN;

static void wasm_refresh_vcpu(void *opaque)
{
    DisplayChangeListener *dcl = opaque;

    /*
     * An update wakes the screendumps waiting for one, and a coroutine
     * cannot run on this thread: while one waits, the main loop's bottom
     * half (qemu_console_co_wait_update) does the update.
     */
    if (qemu_co_queue_empty(&dcl->con->dump_queue)) {
        wasm_refresh_interval = wasm_refresh_poll(dcl, wasm_refresh_interval);
    }
    timer_mod(wasm_refresh_timer,
              qemu_clock_get_ms(QEMU_CLOCK_VIRTUAL) + wasm_refresh_interval);
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

/* no dpy_refresh: the console then keeps no timer of its own */
static const DisplayChangeListenerOps wasm_vcpu_dcl_ops = {
    .dpy_name             = "wasm",
    .dpy_gfx_update       = wasm_gfx_update,
    .dpy_gfx_switch       = wasm_gfx_switch,
};

static DisplayChangeListener wasm_dcl;

static void wasm_display_init(DisplayState *ds, DisplayOptions *opts)
{
    QemuConsole *con;

    key_bh = qemu_bh_new(wasm_key_bh, NULL);
    key_timer = timer_new_ns(QEMU_CLOCK_VIRTUAL, wasm_key_pump, NULL);
    /* release: pairs with the acquire in wasm_send_key() */
    qatomic_store_release(&key_bh_ready, 1);

    /* listen on the first (graphic) console: the phone LCD */
    con = qemu_console_lookup_by_index(0);
    if (!con) {
        error_report("wasm display: no console found");
        exit(1);
    }
    if (qemu_virtual_timers_on_vcpu) {
        qemu_console_register_listener(con, &wasm_dcl, &wasm_vcpu_dcl_ops);
        wasm_refresh_timer = timer_new_ms(QEMU_CLOCK_VIRTUAL,
                                          wasm_refresh_vcpu, &wasm_dcl);
        timer_mod(wasm_refresh_timer, qemu_clock_get_ms(QEMU_CLOCK_VIRTUAL));
        return;
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

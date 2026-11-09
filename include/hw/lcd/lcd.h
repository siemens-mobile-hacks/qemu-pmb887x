/* SPDX-License-Identifier: GPL-2.0-or-later */
#pragma once

#include "hw/core/qdev.h"
#include "qapi-builtin-types.h"
#include "qom/object.h"
#include "hw/ssi/ssi.h"
#include "ui/console.h"
#include "qemu/thread.h"
#include "qemu/debug-regs.h"
#include "qemu/fifo8.h"
#include "hw/lcd/lcd-format.h"

#define TYPE_LCD "lcd"
OBJECT_DECLARE_TYPE(LCDState, LCDClass, LCD);

#define LCD_DATA_IS_CMD (1 << 8)

typedef struct LCDRect LCDRect;
typedef struct LCDGeometry LCDGeometry;

typedef enum LCDWriteState LCDWriteState;

enum LCDWriteState {
    LCD_WR_STATE_NONE,
    LCD_WR_STATE_CMD,
    LCD_WR_STATE_PARAM,
    LCD_WR_STATE_RAM,
    LCD_WR_STATE_IGNORE,
};

typedef enum LCDCounterDirection LCDCounterDirection;

enum LCDCounterDirection {
    LCD_AC_DEC,
    LCD_AC_INC,
};

typedef enum LCDAddressMode LCDAddressMode;

enum LCDAddressMode {
    LCD_AM_HORIZONTAL,
    LCD_AM_VERTICAL,
};

struct LCDRect {
    int x1;
    int y1;
    int x2;
    int y2;
};

struct LCDGeometry {
    uint32_t width;
    uint32_t height;
    uint32_t gram_width;
    uint32_t gram_height;
    uint32_t gram_x;
    uint32_t gram_y;
};

struct LCDState {
    SSIPeripheral parent;
    LCDClass *k;
    DebugChannel dbg;

    Fifo8 fifo;
    bool cd;
    bool read_active;
    bool reset_active;

    uint32_t width;
    uint32_t height;
    uint32_t gram_width;
    uint32_t gram_height;
    uint32_t gram_x;
    uint32_t gram_y;

    uint32_t rotation;
    bool flip_horizontal;
    bool flip_vertical;

    uint32_t default_rotation;
    bool default_flip_horizontal;
    bool default_flip_vertical;
    bool bgr_filter;

    uint8_t bpp;
    uint8_t byte_pp;
    LCDPixelFormat pixel_format;
    uint32_t (*decode_pixel)(uint32_t);
    uint32_t (*encode_pixel)(uint32_t);
    bool output_bgr;

    bool mirror_xy;

    uint32_t tmp_pixel;
    uint32_t tmp_index;
    uint32_t gram_read_dummy_bytes;

    uint32_t *gram; /* 0x00RRGGBB */

    /*
     * GRAM write bytes are decoded by ram_writer. Only the BQL holder appends, and it drains the queue
     * before anything else reads or changes the state the writer uses.
     */
    QemuThread ram_writer;
    QemuMutex ram_queue_lock;
    QemuCond ram_queue_drained;
    QemuEvent ram_queue_ready;
    uint8_t *ram_queue;
    uint32_t ram_queue_head;
    uint32_t ram_queue_tail;

    int buffer_x;
    int buffer_y;

    LCDRect window;
    LCDRect dirty;

    LCDWriteState wr_state;
    uint32_t current_cmd;
    int current_cmd_params;

    LCDCounterDirection ac_x;
    LCDCounterDirection ac_y;
    LCDAddressMode am;

    uint8_t *shadow_buffer;
    DisplaySurface *shadow_surface;

    DisplaySurface *surface;
    QemuConsole *console;

    bool invalidate;
};

struct LCDClass {
    SSIPeripheralClass parent_class;
    const LCDGeometry *geometry;
    const DebugRegs *debug_regs;
    uint16_t cmd_width;
    uint16_t param_width;
    uint8_t gram_read_dummy_pixels;
    bool direct_data_write;
    bool cd_polarity;
    int (*on_cmd)(LCDState *, uint32_t);
    void (*on_cmd_with_params)(LCDState *, uint32_t, const uint32_t *, uint32_t);
    uint32_t (*on_read)(LCDState *, uint32_t);
    void (*reset)(LCDState *);
    void (*realize)(LCDState *, Error **errp);
};

void lcd_set_pixel_format(LCDState *lcd, LCDPixelFormat format);
void lcd_set_output_bgr(LCDState *lcd, bool enabled);
void lcd_set_transform(LCDState *lcd, bool flip_h_pins, bool flip_v_pins);
void lcd_set_ram_mode(LCDState *lcd, bool flag);
void lcd_set_addr_mode(LCDState *lcd, LCDAddressMode am, LCDCounterDirection ac_x, LCDCounterDirection ac_y);

static inline bool lcd_get_cd(LCDState *lcd) {
    return lcd->cd;
}

static inline void lcd_set_cd(LCDState *lcd, bool value) {
    lcd->cd = value;
}

static inline void lcd_set_window_y1(LCDState *lcd, uint32_t value) {
    lcd->window.y1 = MIN(value, lcd->gram_height - 1);
}

static inline void lcd_set_window_y2(LCDState *lcd, uint32_t value) {
    lcd->window.y2 = MIN(value, lcd->gram_height - 1);
}

static inline void lcd_set_window_x1(LCDState *lcd, uint32_t value) {
    lcd->window.x1 = MIN(value, lcd->gram_width - 1);
}

static inline void lcd_set_window_x2(LCDState *lcd, uint32_t value) {
    lcd->window.x2 = MIN(value, lcd->gram_width - 1);
}

static inline void lcd_set_x(LCDState *lcd, uint32_t value) {
    lcd->buffer_x = MIN(value, lcd->gram_width - 1);
}

static inline void lcd_set_y(LCDState *lcd, uint32_t value) {
    lcd->buffer_y = MIN(value, lcd->gram_height - 1);
}

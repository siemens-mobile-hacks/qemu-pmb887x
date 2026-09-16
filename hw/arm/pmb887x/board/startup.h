#pragma once

#include "qemu/typedefs.h"

void pmb887x_board_startup_init(DeviceState *keypad);
void pmb887x_board_keyseq_init(void);
/* Reactive IDLE detection: capcom register 0x5C is polled at the IDLE screen. */
void pmb887x_keyseq_idle_poll(void);
/* Reactive IDLE detection: the boot chime (startup PCM) plays when the phone
 * reaches the IDLE; fire the media-player sequence on its stop. */
void pmb887x_keyseq_bootchime_stop(void);
/* Continuous fire: press the media-player sequence every 2s until it opens. */
void pmb887x_keyseq_start_continuous(void);
void pmb887x_keyseq_stop_continuous(void);
/* Screen-hash IDLE detection: the LCD calls this each tick with a hash of the
 * GRAM region below the status bar (clock-independent). When it matches the
 * "Network search" IDLE signature the media-player sequence is fired once. */
void pmb887x_keyseq_screen_idle(uint64_t region_hash);

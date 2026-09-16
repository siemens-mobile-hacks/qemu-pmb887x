#include "qemu/osdep.h"

#include "hw/arm/pmb887x/board/startup.h"

#include "hw/arm/pmb887x/board/board.h"
#include "hw/arm/pmb887x/board/keyboard.h"
#include "hw/arm/pmb887x/utils/toml.h"
#include "hw/core/irq.h"
#include "hw/core/qdev.h"
#include "qemu/error-report.h"
#include "qemu/log.h"
#include "qemu/timer.h"
#include "ui/input.h"

#include <stdio.h>
#include <stdlib.h>
#include <inttypes.h>

typedef struct pmb887x_startup_sequence_t pmb887x_startup_sequence_t;

struct pmb887x_startup_sequence_t {
	const char *name;
	QKeyCode keys[Q_KEY_CODE__MAX];
	size_t keys_count;
	uint32_t duration_ms;
	QEMUTimer *activation_timer;
	QEMUTimer *release_timer;
};

static pmb887x_startup_sequence_t startup_sequence;

static void pmb887x_board_startup_release(void *opaque) {
	pmb887x_startup_sequence_t *sequence = opaque;

	for (size_t i = 0; i < sequence->keys_count; i++)
		qemu_input_event_send_key_linux(NULL, qemu_input_map_qcode_to_linux[sequence->keys[i]], false);
	qemu_log("Startup: %s deactivated\n", sequence->name);
}

static void pmb887x_board_startup_activate(void *opaque) {
	pmb887x_startup_sequence_t *sequence = opaque;

	for (size_t i = 0; i < sequence->keys_count; i++)
		qemu_input_event_send_key_linux(NULL, qemu_input_map_qcode_to_linux[sequence->keys[i]], true);
	qemu_log("Startup: %s activated\n", sequence->name);
}

static void pmb887x_board_startup_keypad_ready(void *opaque, int line, int level) {
	pmb887x_startup_sequence_t *sequence = opaque;

	if (!level)
		return;

	timer_mod(sequence->release_timer, qemu_clock_get_ms(QEMU_CLOCK_VIRTUAL) + sequence->duration_ms);
	qemu_log("Startup: %s release scheduled\n", sequence->name);
}

void pmb887x_board_startup_init(DeviceState *keypad) {
	pmb887x_board_t *board = pmb887x_board();
	toml_datum_t startup = toml_table_get(board->config, TOML_TABLE, "startup", false);

	if (startup.type == TOML_UNKNOWN)
		return;

	const char *scenario_name = getenv("PMB887X_STARTUP");
	if (!scenario_name || !scenario_name[0])
		scenario_name = "ONLINE";

	toml_datum_t scenario = toml_table_get(startup, TOML_TABLE, scenario_name, true);
	toml_datum_t keys = toml_table_get(scenario, TOML_ARRAY, "keys", true);

	if (keys.u.arr.size == 0) {
		qemu_log("Startup: %s\n", scenario_name);
		return;
	}

	int duration_seconds = toml_table_get_int32(scenario, "duration", 0, true);
	if ((size_t) keys.u.arr.size > ARRAY_SIZE(startup_sequence.keys)) {
		error_report("Too many keys in startup scenario '%s'", scenario_name);
		exit(EXIT_FAILURE);
	}
	if (duration_seconds <= 0 || duration_seconds > UINT32_MAX / 1000) {
		error_report("Invalid duration in startup scenario '%s'", scenario_name);
		exit(EXIT_FAILURE);
	}

	for (int i = 0; i < keys.u.arr.size; i++) {
		const char *key_name = toml_array_get_string(keys, i, NULL, true);
		QKeyCode qcode;

		if (!pmb887x_board_find_keycode(key_name, &qcode)) {
			error_report("Unknown key '%s' in startup scenario '%s'", key_name, scenario_name);
			exit(EXIT_FAILURE);
		}
		for (size_t j = 0; j < startup_sequence.keys_count; j++) {
			if (startup_sequence.keys[j] == qcode) {
				error_report("Duplicate key '%s' in startup scenario '%s'", key_name, scenario_name);
				exit(EXIT_FAILURE);
			}
		}
		startup_sequence.keys[startup_sequence.keys_count++] = qcode;
	}

	startup_sequence.name = scenario_name;
	startup_sequence.duration_ms = (uint32_t) duration_seconds * 1000;
	startup_sequence.activation_timer = timer_new_ms(QEMU_CLOCK_VIRTUAL, pmb887x_board_startup_activate, &startup_sequence);
	startup_sequence.release_timer = timer_new_ms(QEMU_CLOCK_VIRTUAL, pmb887x_board_startup_release, &startup_sequence);
	qdev_connect_gpio_out_named(keypad, "KEYPAD_READY_OUT", 0,
		qemu_allocate_irq(pmb887x_board_startup_keypad_ready, &startup_sequence, 0));
	timer_mod(startup_sequence.activation_timer, qemu_clock_get_ms(QEMU_CLOCK_VIRTUAL));
	qemu_log("Startup: %s (%d s after KEYPAD_READY)\n", scenario_name, duration_seconds);
}

/*
 * Timed key-sequence injector for deterministic UI navigation.
 *
 * PMB887X_KEYSEQ="name@ms[:hold_ms],name@ms[:hold_ms],..."
 *
 * Each entry presses the named key at virtual time (ms) and releases it after
 * hold_ms (default 400ms, >= 300ms so the keypad scan registers it). Timers run
 * on QEMU_CLOCK_VIRTUAL so the sequence is deterministic under
 * "-icount precise-clocks=on". Key names accept the board keymap names or the
 * short aliases (center/down/up/left/right/0-9/star/hash/...). The start time is
 * relative to board init (boot start); add enough offset for the phone to reach
 * the IDLE screen first.
 */
#define KEYSEQ_MAX_PRESSES 1024

typedef struct pmb887x_keyseq_press_t {
	QKeyCode qcode;
	char name[32];
	uint32_t at_ms;
	uint32_t hold_ms;
	QEMUTimer *down;
	QEMUTimer *up;
} pmb887x_keyseq_press_t;

static pmb887x_keyseq_press_t keyseq_presses[KEYSEQ_MAX_PRESSES];
static size_t keyseq_press_count;
static int64_t keyseq_start_ns; /* wall-clock at arming; presses are offsets from this */

static void pmb887x_keyseq_down(void *opaque) {
	pmb887x_keyseq_press_t *k = opaque;

	qemu_input_event_send_key_linux(NULL, qemu_input_map_qcode_to_linux[k->qcode], true);
	qemu_log("KEYSEQ down  %-10s virt=%ums t+%.3fs\n", k->name,
		 (unsigned) qemu_clock_get_ms(QEMU_CLOCK_VIRTUAL),
		 (double) (qemu_clock_get_ns(QEMU_CLOCK_REALTIME) - keyseq_start_ns) / 1e9);
}

static void pmb887x_keyseq_up(void *opaque) {
	pmb887x_keyseq_press_t *k = opaque;

	qemu_input_event_send_key_linux(NULL, qemu_input_map_qcode_to_linux[k->qcode], false);
	qemu_log("KEYSEQ up    %-10s virt=%ums t+%.3fs\n", k->name,
		 (unsigned) qemu_clock_get_ms(QEMU_CLOCK_VIRTUAL),
		 (double) (qemu_clock_get_ns(QEMU_CLOCK_REALTIME) - keyseq_start_ns) / 1e9);
}

static const char *pmb887x_keyseq_canon(const char *s) {
	static const struct {
		const char *alias;
		const char *name;
	} aliases[] = {
		{ "center", "NAV_CENTER" }, { "down", "NAV_DOWN" }, { "up", "NAV_UP" },
		{ "left", "NAV_LEFT" }, { "right", "NAV_RIGHT" },
		{ "0", "NUM0" }, { "1", "NUM1" }, { "2", "NUM2" }, { "3", "NUM3" }, { "4", "NUM4" },
		{ "5", "NUM5" }, { "6", "NUM6" }, { "7", "NUM7" }, { "8", "NUM8" }, { "9", "NUM9" },
		{ "star", "STAR" }, { "hash", "HASH" },
		{ "send", "SEND" }, { "end", "END_CALL" }, { "clear", "CLEAR" },
		{ "music", "MUSIC" }, { "play", "PLAY_PAUSE" },
		{ "softl", "SOFT_LEFT" }, { "softr", "SOFT_RIGHT" }, { "browser", "BROWSER" },
	};

	for (size_t i = 0; i < ARRAY_SIZE(aliases); i++) {
		if (strcmp(s, aliases[i].alias) == 0)
			return aliases[i].name;
	}
	return s;
}

void pmb887x_board_keyseq_init(void) {
	const char *spec = getenv("PMB887X_KEYSEQ");
	keyseq_start_ns = qemu_clock_get_ns(QEMU_CLOCK_REALTIME);

	if (getenv("PMB887X_KEYSEQ_CONTINUOUS"))
		pmb887x_keyseq_start_continuous();

	if (!spec || !spec[0])
		return;

	char *copy = g_strdup(spec);
	char *saveptr = NULL;

	for (char *tok = strtok_r(copy, ",", &saveptr); tok; tok = strtok_r(NULL, ",", &saveptr)) {
		char *at = strchr(tok, '@');

		if (!at) {
			error_report("PMB887X_KEYSEQ: bad token '%s' (want name@ms[:hold])", tok);
			g_free(copy);
			return;
		}
		*at = 0;

		const char *name = pmb887x_keyseq_canon(tok);
		char *colon = strchr(at + 1, ':');
		uint32_t at_ms = (uint32_t) strtoul(at + 1, NULL, 10);
		uint32_t hold_ms = 400;

		if (colon) {
			*colon = 0;
			hold_ms = (uint32_t) strtoul(colon + 1, NULL, 10);
		}

		QKeyCode qcode;
		if (!pmb887x_board_find_keycode(name, &qcode)) {
			error_report("PMB887X_KEYSEQ: unknown key '%s'", name);
			g_free(copy);
			return;
		}
		if (keyseq_press_count >= ARRAY_SIZE(keyseq_presses)) {
			error_report("PMB887X_KEYSEQ: too many presses (max %d)", KEYSEQ_MAX_PRESSES);
			g_free(copy);
			return;
		}

		pmb887x_keyseq_press_t *k = &keyseq_presses[keyseq_press_count++];
		k->qcode = qcode;
		k->at_ms = at_ms;
		k->hold_ms = hold_ms;
		snprintf(k->name, sizeof(k->name), "%s", name);
		k->down = timer_new_ms(QEMU_CLOCK_VIRTUAL, pmb887x_keyseq_down, k);
		k->up = timer_new_ms(QEMU_CLOCK_VIRTUAL, pmb887x_keyseq_up, k);
		timer_mod(k->down, qemu_clock_get_ms(QEMU_CLOCK_VIRTUAL) + at_ms);
		timer_mod(k->up, qemu_clock_get_ms(QEMU_CLOCK_VIRTUAL) + at_ms + hold_ms);
	}

	g_free(copy);
	qemu_log("KEYSEQ: armed %zu press(es) from '%s'\n", keyseq_press_count, spec);
}

/*
 * Reactive IDLE detection: capcom register 0x5C is read (polled) at the IDLE
 * screen. The IDLE's virtual time is non-deterministic, so instead of firing a
 * fixed-time key sequence we react to the first 0x5C poll and press
 * center,0,center,center (open the media player). A cooldown + fire cap keeps
 * it from re-firing on every poll.
 */
static int64_t keyseq_idle_last_fire = -100000; /* virt ms of last reactive fire */
static int keyseq_idle_fires;

/* The capcom/bootchime reactive triggers are disabled unless explicitly
 * enabled, so the screen-hash trigger can run clean. */
static int keyseq_reactive_enabled(void) {
	static int enabled = -1;
	if (enabled < 0)
		enabled = (getenv("PMB887X_IDLE_REACTIVE") != NULL);
	return enabled;
}

/*
 * Continuous fire: press center,0,center,center every 2s until the media
 * player actually opens (the first PCM_SUBMIT) or the fire cap is reached.
 * This sidesteps the non-deterministic IDLE virtual time entirely.
 */
static QEMUTimer *keyseq_continuous_timer;
static int keyseq_continuous_fires;
#define KEYSEQ_CONTINUOUS_MAX 130
#define KEYSEQ_CONTINUOUS_PERIOD_MS 5000 /* > sequence length (~4s) so presses don't overlap */

static void keyseq_continuous_tick(void *opaque) {
	if (keyseq_continuous_fires >= KEYSEQ_CONTINUOUS_MAX)
		return;
	keyseq_continuous_fires++;

	int64_t now = qemu_clock_get_ms(QEMU_CLOCK_VIRTUAL);
	const char *seq[] = { "center", "0", "center", "center" };
	for (int i = 0; i < 4 && keyseq_press_count < ARRAY_SIZE(keyseq_presses); i++) {
		pmb887x_keyseq_press_t *k = &keyseq_presses[keyseq_press_count++];
		const char *name = pmb887x_keyseq_canon(seq[i]);
		if (!pmb887x_board_find_keycode(name, &k->qcode))
			continue;
		k->at_ms = 500 + i * 1000;
		k->hold_ms = 450;
		snprintf(k->name, sizeof(k->name), "%s", name);
		k->down = timer_new_ms(QEMU_CLOCK_VIRTUAL, pmb887x_keyseq_down, k);
		k->up = timer_new_ms(QEMU_CLOCK_VIRTUAL, pmb887x_keyseq_up, k);
		timer_mod(k->down, now + k->at_ms);
		timer_mod(k->up, now + k->at_ms + k->hold_ms);
	}
	if (keyseq_continuous_fires < KEYSEQ_CONTINUOUS_MAX)
		timer_mod(keyseq_continuous_timer, now + KEYSEQ_CONTINUOUS_PERIOD_MS);
}

void pmb887x_keyseq_start_continuous(void) {
	if (keyseq_continuous_timer)
		return;
	keyseq_continuous_timer =
		timer_new_ms(QEMU_CLOCK_VIRTUAL, keyseq_continuous_tick, NULL);
	timer_mod(keyseq_continuous_timer,
		      qemu_clock_get_ms(QEMU_CLOCK_VIRTUAL) + 5000);
	qemu_log("KEYSEQ: continuous fire armed (every %dms, max %d)\n",
		 KEYSEQ_CONTINUOUS_PERIOD_MS, KEYSEQ_CONTINUOUS_MAX);
}

void pmb887x_keyseq_stop_continuous(void) {
	if (!keyseq_continuous_timer)
		return;
	timer_del(keyseq_continuous_timer);
	keyseq_continuous_timer = NULL;
	qemu_log("KEYSEQ: continuous fire stopped after %d fire(s)\n",
		 keyseq_continuous_fires);
}

void pmb887x_keyseq_idle_poll(void) {
	if (!keyseq_reactive_enabled())
		return;
	if (keyseq_idle_fires >= 30)
		return;

	int64_t now = qemu_clock_get_ms(QEMU_CLOCK_VIRTUAL);
	if (now - keyseq_idle_last_fire < 8000)
		return; /* cooldown: at most one sequence per 8s */
	keyseq_idle_last_fire = now;
	keyseq_idle_fires++;

	const char *seq[] = { "center", "0", "center", "center" };
	for (int i = 0; i < 4 && keyseq_press_count < ARRAY_SIZE(keyseq_presses); i++) {
		pmb887x_keyseq_press_t *k = &keyseq_presses[keyseq_press_count++];
		const char *name = pmb887x_keyseq_canon(seq[i]);
		if (!pmb887x_board_find_keycode(name, &k->qcode))
			continue;
		k->at_ms = 1000 + i * 1000; /* 1s,2s,3s,4s from now */
		k->hold_ms = 500;
		snprintf(k->name, sizeof(k->name), "%s", name);
		k->down = timer_new_ms(QEMU_CLOCK_VIRTUAL, pmb887x_keyseq_down, k);
		k->up = timer_new_ms(QEMU_CLOCK_VIRTUAL, pmb887x_keyseq_up, k);
		timer_mod(k->down, now + k->at_ms);
		timer_mod(k->up, now + k->at_ms + k->hold_ms);
		qemu_log("KEYSEQ:   %s down@%lld up@%lld (now=%lld)\n", k->name,
			(long long) (now + k->at_ms), (long long) (now + k->at_ms + k->hold_ms), (long long) now);
	}
	qemu_log("KEYSEQ: idle detected (fire %d) center,0,center,center virt=%lldms\n",
		 keyseq_idle_fires, (long long) now);
}

/*
 * Fire the media-player-opening sequence a short while after the boot chime
 * stops (the chime plays when the phone reaches the IDLE). Reuses the idle
 * fire counter/cooldown so the capcom and bootchime triggers share a budget.
 */
/*
 * Schedule a single named keypress at a virtual time (ms), held for hold_ms.
 * Returns true on success.
 */
static bool keyseq_schedule(const char *name, int64_t at_virt_ms, uint32_t hold_ms) {
	if (keyseq_press_count >= ARRAY_SIZE(keyseq_presses))
		return false;

	pmb887x_keyseq_press_t *k = &keyseq_presses[keyseq_press_count++];
	const char *canon = pmb887x_keyseq_canon(name);
	if (!pmb887x_board_find_keycode(canon, &k->qcode))
		return false;
	k->at_ms = (uint32_t) (at_virt_ms - qemu_clock_get_ms(QEMU_CLOCK_VIRTUAL));
	k->hold_ms = hold_ms;
	snprintf(k->name, sizeof(k->name), "%s", canon);
	k->down = timer_new_ms(QEMU_CLOCK_VIRTUAL, pmb887x_keyseq_down, k);
	k->up = timer_new_ms(QEMU_CLOCK_VIRTUAL, pmb887x_keyseq_up, k);
	timer_mod(k->down, at_virt_ms);
	timer_mod(k->up, at_virt_ms + hold_ms);
	return true;
}

/*
 * Screen-hash IDLE detection: the LCD calls pmb887x_keyseq_screen_idle() each
 * tick with a hash of the GRAM region below the status bar (rows 32+), which is
 * clock-independent. The "Network search" IDLE screen has a stable signature,
 * so when the region hash matches we fire the media-player-opening sequence
 * exactly once: center (open menu grid), 0 (select Files), center (open Files),
 * center (play audio). Spacing (1.5s) is wide enough for each UI transition to
 * complete before the next key lands.
 */
static int keyseq_screen_idle_fired;
#define KEYSEQ_IDLE_DEFAULT_SIG 0x11a86f0a19fb52fdULL /* "Network search" text rows 44-58 */

void pmb887x_keyseq_screen_idle(uint64_t region_hash) {
	static uint64_t target = (uint64_t) -1;
	static const char *env = NULL;
	static uint64_t last_region = (uint64_t) -1;

	if (target == (uint64_t) -1) {
		target = KEYSEQ_IDLE_DEFAULT_SIG;
		env = getenv("PMB887X_IDLE_SIG");
		if (env && env[0])
			target = strtoull(env, NULL, 16);
	}

	if (region_hash != last_region) {
		last_region = region_hash;
		qemu_log("KEYSEQ: region=%016" PRIx64 "%s\n", region_hash,
			region_hash == target ? "  <- IDLE signature" : "");
	}

	if (region_hash != target || keyseq_screen_idle_fired)
		return;
	keyseq_screen_idle_fired = 1;

	int64_t now = qemu_clock_get_ms(QEMU_CLOCK_VIRTUAL);
	struct { const char *name; int64_t at; } seq[] = {
		{ "center", now + 1000 },  /* open menu icon grid */
		{ "0",      now + 2500 },  /* move selection to Files */
		{ "center", now + 4000 },  /* open Files */
		{ "center", now + 5500 },  /* play the audio file */
		{ "down",   now + 12000 }, /* AAC: step to next track */
		{ "down",   now + 13500 },
	};
	for (size_t i = 0; i < ARRAY_SIZE(seq); i++) {
		if (!keyseq_schedule(seq[i].name, seq[i].at, 500))
			break;
	}
	qemu_log("KEYSEQ: 'Network search' IDLE detected (region=%016" PRIx64 "), firing "
		"center,0,center,center virt=%lldms\n", region_hash, (long long) now);
}

void pmb887x_keyseq_bootchime_stop(void) {
	if (!keyseq_reactive_enabled())
		return;
	if (keyseq_idle_fires >= 30)
		return;

	int64_t now = qemu_clock_get_ms(QEMU_CLOCK_VIRTUAL);
	if (now - keyseq_idle_last_fire < 8000)
		return;
	keyseq_idle_last_fire = now;
	keyseq_idle_fires++;

	const char *seq[] = { "center", "0", "center", "center" };
	for (int i = 0; i < 4 && keyseq_press_count < ARRAY_SIZE(keyseq_presses); i++) {
		pmb887x_keyseq_press_t *k = &keyseq_presses[keyseq_press_count++];
		const char *name = pmb887x_keyseq_canon(seq[i]);
		if (!pmb887x_board_find_keycode(name, &k->qcode))
			continue;
		k->at_ms = 2000 + i * 1000; /* 2s,3s,4s,5s from now */
		k->hold_ms = 500;
		snprintf(k->name, sizeof(k->name), "%s", name);
		k->down = timer_new_ms(QEMU_CLOCK_VIRTUAL, pmb887x_keyseq_down, k);
		k->up = timer_new_ms(QEMU_CLOCK_VIRTUAL, pmb887x_keyseq_up, k);
		timer_mod(k->down, now + k->at_ms);
		timer_mod(k->up, now + k->at_ms + k->hold_ms);
	}
	qemu_log("KEYSEQ: bootchime stop (fire %d) center,0,center,center virt=%lldms\n",
		 keyseq_idle_fires, (long long) now);
}

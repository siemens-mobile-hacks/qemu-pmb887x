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
		qemu_input_event_send_key_qcode(NULL, sequence->keys[i], false);
	qemu_log("Startup: %s deactivated\n", sequence->name);
}

static void pmb887x_board_startup_activate(void *opaque) {
	pmb887x_startup_sequence_t *sequence = opaque;

	for (size_t i = 0; i < sequence->keys_count; i++)
		qemu_input_event_send_key_qcode(NULL, sequence->keys[i], true);
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

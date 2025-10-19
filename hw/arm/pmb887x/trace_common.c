#include "hw/arm/pmb887x/trace_common.h"
#include "qemu/error-report.h"

typedef struct pmb887x_debug_channel_t pmb887x_debug_channel_t;

struct pmb887x_debug_channel_t {
	const char name[32];
	uint64_t mask;
};

static uint64_t parse_trace_spec(const char *spec, uint64_t mask);

static const pmb887x_debug_channel_t debug_channels[] = {
	// modules
	{ "gptu",		PMB887X_TRACE_GPTU },
	{ "tpu",		PMB887X_TRACE_TPU },
	{ "dmac",		PMB887X_TRACE_DMAC },
	{ "ebu",		PMB887X_TRACE_EBU },
	{ "stm",		PMB887X_TRACE_STM },
	{ "pll",		PMB887X_TRACE_PLL },
	{ "adc",		PMB887X_TRACE_ADC },
	{ "capcom",		PMB887X_TRACE_CAPCOM },
	{ "dif",		PMB887X_TRACE_DIF },
	{ "dsp",		PMB887X_TRACE_DSP },
	{ "mod",		PMB887X_TRACE_MOD },
	{ "vic",		PMB887X_TRACE_VIC },
	{ "pcl",		PMB887X_TRACE_PCL },
	{ "rtc",		PMB887X_TRACE_RTC },
	{ "scu",		PMB887X_TRACE_SCU },
	{ "usart",		PMB887X_TRACE_USART },
	{ "keypad",		PMB887X_TRACE_KEYPAD },
	{ "i2c",		PMB887X_TRACE_I2C },
	{ "sccu",		PMB887X_TRACE_SCCU },
	{ "mmci",		PMB887X_TRACE_MMCI },
	{ "ssc",		PMB887X_TRACE_SSC },
	{ "tcm",		PMB887X_TRACE_TCM },

	// peripherals
	{ "acodec",		PMB887X_TRACE_ACODEC },
	{ "gimmick",	PMB887X_TRACE_GIMMICK },
	{ "fm_radio",	PMB887X_TRACE_FM_RADIO },
	{ "flash",		PMB887X_TRACE_FLASH },
	{ "lcd",		PMB887X_TRACE_LCD },
	{ "pmic",		PMB887X_TRACE_PMIC },
	{ "unknown",	PMB887X_TRACE_UNKNOWN },

	// misc
	{ "all",		PMB887X_TRACE_ALL },

	// end
	{ }
};

uint64_t pmb887x_trace_io_mask = 0;
uint64_t pmb887x_trace_log_mask = 0;

void pmb887x_trace_init(void) {
	pmb887x_trace_io_mask = parse_trace_spec(getenv("PMB887X_TRACE_IO"), 0);
	pmb887x_trace_log_mask = parse_trace_spec(getenv("PMB887X_TRACE_LOG"), 0);
	pmb887x_trace_io_mask = parse_trace_spec(getenv("PMB887X_TRACE_ALL"), pmb887x_trace_io_mask);
	pmb887x_trace_log_mask = parse_trace_spec(getenv("PMB887X_TRACE_ALL"), pmb887x_trace_log_mask);
}

static uint64_t parse_trace_spec(const char *spec, uint64_t mask) {
	char token[32] = {};
	int token_idx = 0;

	if (!spec)
		return 0;

	for (const char *p = spec; ; p++) {
		char c = *p;
		if (c == ',' || c == '\0') {
			if (token_idx > 0) {
				token[token_idx] = '\0';

				char op;
				const char *name;
				if (token[0] == '+' || token[0] == '-') {
					op = token[0];
					name = token + 1;
				} else {
					op = '+';
					name = token;
				}

				char name_lower[32] = {};
				for (int i = 0; name[i] && i < sizeof(name_lower) - 1; i++)
					name_lower[i] = tolower(name[i]);

				const pmb887x_debug_channel_t *chan = debug_channels;
				int found = 0;
				while (chan->mask) {
					if (strcmp(name_lower, chan->name) == 0) {
						if (op == '+') {
							mask |= chan->mask;
						} else {
							mask &= ~chan->mask;
						}
						found = 1;
						break;
					}
					chan++;
				}

				if (!found) {
					error_report("Unknown debug channel name: %s", name_lower);
					exit(EXIT_FAILURE);
				}

				token_idx = 0;
			}

			if (c == '\0')
				break;
		} else if (!isspace(c)) {
			if (token_idx < sizeof(token) - 1) {
				token[token_idx++] = c;
				token[token_idx] = '\0';
			} else {
				error_report("Invalid debug channel name: %s", token);
				exit(EXIT_FAILURE);
			}
		}
	}

	return mask;
}

#include "qemu/osdep.h"

#include "hw/arm/pmb887x/gen/dsp.h"
#include "hw/audio/audio-line.h"
#include "hw/arm/pmb887x/dsp/peripheral.h"
#include "hw/gsm/iq-bus.h"
#include "qemu/debug-channel.h"

#define TEST_INTERRUPT_BASE	0x1000
#define TEST_MCS_BASE		0x1020
#define TEST_DSP_BASE		0x1030
#define TEST_MODULATOR_BASE	0x1040
#define TEST_AFE_BASE		0x1050
#define TEST_UNKNOWN_BASE	0x1060
#define TEST_I2S_BASE		0x1070
#define TEST_I2S_TX_BASE	0x1080
#define TEST_TIMER1_BASE	0x1090
#define TEST_TIMER2_BASE	0x10A0
#define TEST_AFE_DAC_RING_OFFSET	0x40
#define TEST_I2S_RX_RAM_BASE	(TEAK_I2S_RWADDR_RDADDR + 1U)

typedef struct test_audio_line_t {
	AudioLineFormat format;
	int gain_mb;
	size_t frame_count;
	int16_t last_sample;
} test_audio_line_t;

/* The host-unit test replaces external frontends, never DSP model code. */
static test_audio_line_t test_afe_ep_line;
static test_audio_line_t test_afe_eppa_line;

void audio_line_start(AudioLineBus *bus, unsigned stream, const AudioLineFormat *format) {
	g_assert_cmpuint(stream, ==, 0);
	((test_audio_line_t *) bus)->format = *format;
}

void audio_line_set_gain(AudioLineBus *bus, unsigned stream, int gain_mb) {
	g_assert_cmpuint(stream, ==, 0);
	((test_audio_line_t *) bus)->gain_mb = gain_mb;
}

void audio_line_stop(AudioLineBus *bus, unsigned stream) {
	g_assert_cmpuint(stream, ==, 0);
	((test_audio_line_t *) bus)->format = (AudioLineFormat) { };
}

void audio_line_write(AudioLineBus *bus, unsigned stream, const int16_t *frames, size_t frame_count) {
	test_audio_line_t *line = (test_audio_line_t *) bus;

	g_assert_cmpuint(stream, ==, 0);
	g_assert_cmpuint(line->format.channels, !=, 0);
	line->frame_count += frame_count;
	line->last_sample = frames[frame_count * line->format.channels - 1];
}

bool iq_bus_has_source(const IQBus *bus) {
	g_assert_null(bus);
	return false;
}

IQSample iq_bus_read(IQBus *bus, int64_t _timestamp, uint64_t _sample_index) {
	g_assert_null(bus);
	return (IQSample) { 0, 0 };
}

void iq_bus_transmit(IQBus *bus, int64_t _timestamp, bool _symbol) {
	g_assert_null(bus);
}

void iq_bus_transmit_until(IQBus *bus, int64_t _timestamp) {
	g_assert_null(bus);
}

typedef struct test_host_t {
	int64_t time_ns;
	uint16_t page;
	uint32_t pc;
	bool core_disabled;
	uint16_t data[128];
	uint32_t i2s_sample_rate_hz;
	unsigned i2s_channels;
	size_t data_read_count;
	uint16_t data_read_address;
	size_t i2s_configure_count;
	bool i2s_transmit;
	int16_t i2s_output[2];
	size_t i2s_frame_count;
	int16_t i2s_input[4];
	size_t i2s_input_frame_count;
	size_t i2s_input_position;
} test_host_t;

DEBUG_CHANNEL_INFO(test_dsp_info, .name = "dsp")

static DebugChannel test_dsp_dbg;

static void test_set_page(void *opaque, uint16_t value) {
	test_host_t *host = opaque;

	host->page = value;
}

static uint32_t test_get_pc(void *opaque) {
	test_host_t *host = opaque;

	return host->pc;
}

static void test_set_core_disabled(void *opaque, bool disabled) {
	test_host_t *host = opaque;

	host->core_disabled = disabled;
}

static int64_t test_get_time_ns(void *opaque) {
	test_host_t *host = opaque;
	return host->time_ns;
}

static uint16_t test_data_read(void *opaque, uint16_t address) {
	test_host_t *host = opaque;
	host->data_read_count++;
	host->data_read_address = address;
	return address < ARRAY_SIZE(host->data) ? host->data[address] : 0;
}

static void test_data_write(void *opaque, uint16_t address, uint16_t value) {
	test_host_t *host = opaque;

	if (address < ARRAY_SIZE(host->data))
		host->data[address] = value;
}

static void test_i2s_configure(test_host_t *host, bool transmit, const I2SFormat *format) {
	host->i2s_sample_rate_hz = format->sample_rate_hz;
	host->i2s_channels = format->channels;
	host->i2s_configure_count++;
	host->i2s_transmit = transmit;
}

void i2s_tx_configure(I2STxBus *bus, const I2SFormat *format) {
	test_i2s_configure((test_host_t *) bus, true, format);
}

void i2s_rx_configure(I2SRxBus *bus, const I2SFormat *format) {
	test_i2s_configure((test_host_t *) bus, false, format);
}

void i2s_tx_write(I2STxBus *bus, const int16_t *samples, size_t frame_count) {
	test_host_t *host = (test_host_t *) bus;
	host->i2s_frame_count += frame_count;
	memcpy(host->i2s_output, samples, host->i2s_channels * sizeof(*samples));
}

size_t i2s_rx_read(I2SRxBus *bus, int16_t *samples, size_t frame_capacity) {
	test_host_t *host = (test_host_t *) bus;
	size_t available_frames = host->i2s_input_position < host->i2s_input_frame_count ?
		host->i2s_input_frame_count - host->i2s_input_position : 0;
	size_t read_frames = MIN(frame_capacity, available_frames);

	memcpy(samples, &host->i2s_input[host->i2s_input_position * host->i2s_channels],
		read_frames * host->i2s_channels * sizeof(*samples));
	host->i2s_input_position += read_frames;
	return read_frames;
}

static dsp_bus_t *test_bus_create(test_host_t *host) {
	static const pmb887x_dsp_peripheral_config_t peripherals[] = {
		{ "INT", PMB887X_DSP_PERIPHERAL_INTERRUPT, TEST_INTERRUPT_BASE, 0x16 },
		{ "MCS", PMB887X_DSP_PERIPHERAL_MCS, TEST_MCS_BASE, 0x06 },
		{ "DSP", PMB887X_DSP_PERIPHERAL_DSP, TEST_DSP_BASE, 0x09 },
		{ "MOD", PMB887X_DSP_PERIPHERAL_MODULATOR, TEST_MODULATOR_BASE, 0x0B },
		{ "AFE", PMB887X_DSP_PERIPHERAL_AFE, TEST_AFE_BASE, 0x10 },
		{ "I2S1", PMB887X_DSP_PERIPHERAL_I2S, TEST_I2S_BASE, 0x0B },
		{ "I2S3", PMB887X_DSP_PERIPHERAL_I2S_TX, TEST_I2S_TX_BASE, 0x0B },
		{ "TIMER1", PMB887X_DSP_PERIPHERAL_TIMER1, TEST_TIMER1_BASE, 0x04 },
		{ "TIMER2", PMB887X_DSP_PERIPHERAL_TIMER2, TEST_TIMER2_BASE, 0x03 },
	};
	static const pmb887x_dsp_config_t config = {
		.hardware_version = PMB887X_DSP_HW_VERSION_E101,
		.mmio_base = TEST_INTERRUPT_BASE,
		.mmio_size = 0x100,
		.peripherals = peripherals,
		.peripheral_count = ARRAY_SIZE(peripherals),
	};
	const dsp_ports_t ports = {
		.i2s = {
			{
				.tx_bus = (I2STxBus *) host,
				.rx_bus = (I2SRxBus *) host,
			},
		},
		.afe_ep = (AudioLineBus *) &test_afe_ep_line,
		.afe_eppa = (AudioLineBus *) &test_afe_eppa_line,
	};
	dsp_host_t peripheral_host = {
		.opaque = host,
		.set_page = test_set_page,
		.set_core_disabled = test_set_core_disabled,
		.get_time_ns = test_get_time_ns,
		.get_pc = test_get_pc,
		.data_read = test_data_read,
		.data_write = test_data_write,
	};

	return dsp_bus_create(&config, &peripheral_host, &ports, &test_dsp_dbg);
}

static void test_timer_clock(void) {
	test_host_t host = {};
	dsp_bus_t *bus = test_bus_create(&host);

	dsp_bus_reset(bus);
	dsp_bus_write(bus, TEST_TIMER1_BASE + TEAK_TMR1_CTRL, TEAK_TMR1_CTRL_DT1ENA);
	dsp_bus_write(bus, TEST_TIMER1_BASE + TEAK_TMR1_CTRL, TEAK_TMR1_CTRL_DT1ENA | TEAK_TMR1_CTRL_RESTART);
	dsp_bus_write(bus, TEST_TIMER2_BASE + TEAK_TMR2_CTRL, TEAK_TMR2_CTRL_DT2ACT);
	dsp_bus_advance_idle(bus, 104000);
	g_assert_cmphex(dsp_bus_read(bus, TEST_TIMER1_BASE + TEAK_TMR1_CNT), ==, 270);
	g_assert_cmphex(dsp_bus_read(bus, TEST_TIMER2_BASE + TEAK_TMR2_CNT), ==, 1083);

	dsp_bus_destroy(bus);
}

static void test_bus_horizon(void) {
	test_host_t host = { .data = { 0x8000, 0 } };
	dsp_bus_t *bus = test_bus_create(&host);

	dsp_bus_reset(bus);
	dsp_bus_set_clock_hz(bus, 26000000);
	dsp_bus_set_ms_clock_hz(bus, 104000000);
	dsp_bus_write(bus, TEST_TIMER2_BASE + TEAK_TMR2_MAX, 3);
	dsp_bus_write(bus, TEST_TIMER2_BASE + TEAK_TMR2_CTRL, TEAK_TMR2_CTRL_DT2ACT);
	for (size_t cycles = 7; cycles < 3 * 96; cycles += 7) {
		dsp_bus_advance_idle(bus, 7);
		g_assert_cmphex(dsp_bus_get_irq_flags(bus, 2), ==, 0);
		if (cycles == 98)
			g_assert_cmphex(dsp_bus_read(bus, TEST_TIMER2_BASE + TEAK_TMR2_CNT), ==, 1);
	}
	dsp_bus_advance_idle(bus, 1);
	g_assert_cmphex(dsp_bus_get_irq_flags(bus, 2), ==, TEAK_INT_FINT1_TMR2);
	g_assert_cmphex(dsp_bus_read(bus, TEST_TIMER2_BASE + TEAK_TMR2_CNT), ==, 3);

	dsp_bus_write(bus, TEST_I2S_BASE + TEAK_I2S_NUM0, 441);
	dsp_bus_write(bus, TEST_I2S_BASE + TEAK_I2S_DEN0, 32500);
	dsp_bus_write(bus, TEST_I2S_BASE + TEAK_I2S_TXCONF, TEAK_I2S_TXCONF_PERIOD_CLOCKS_32);
	dsp_bus_write(bus, TEST_I2S_BASE + TEAK_I2S_CTRL, TEAK_I2S_CTRL_I2SON);
	dsp_bus_write(bus, TEST_I2S_BASE + TEAK_I2S_TXINTADDR, 6);
	dsp_bus_write(bus, TEST_I2S_BASE + TEAK_I2S_CTRL, TEAK_I2S_CTRL_I2SON | TEAK_I2S_CTRL_I2STXSTART);
	for (size_t cycles = 53; cycles < 9434; cycles += 53) {
		dsp_bus_advance_idle(bus, 53);
		g_assert_cmphex(dsp_bus_get_irq_flags(bus, 1), ==, 0);
	}
	dsp_bus_advance_idle(bus, 53);
	g_assert_cmphex(dsp_bus_get_irq_flags(bus, 1), ==, TEAK_INT_FINTB0_I2S1TX);
	g_assert_cmphex(dsp_bus_read(bus, TEST_I2S_BASE + TEAK_I2S_RWADDR), ==, 6);

	dsp_bus_destroy(bus);
}

static void test_control(void) {
	test_host_t host = { .page = UINT16_MAX, .pc = 0x1234 };
	dsp_bus_t *bus = test_bus_create(&host);

	dsp_bus_reset(bus);
	g_assert_cmphex(host.page, ==, 0);
	g_assert_false(host.core_disabled);
	g_assert_cmphex(dsp_bus_read(bus, TEST_DSP_BASE), ==, 0xE101);
	g_assert_cmphex(dsp_bus_read(bus, TEST_DSP_BASE + 2), ==, 0);
	dsp_bus_write(bus, TEST_DSP_BASE + 1, 0);
	g_assert_true(host.core_disabled);
	dsp_bus_write(bus, TEST_DSP_BASE + 3, 5);
	g_assert_cmphex(host.page, ==, 5);
	g_assert_cmphex(dsp_bus_read(bus, TEST_DSP_BASE + 3), ==, 5);
	dsp_bus_destroy(bus);
}

static void test_pads(void) {
	test_host_t host = {};
	dsp_bus_t *bus = test_bus_create(&host);

	dsp_bus_reset(bus);
	dsp_bus_write(bus, TEST_DSP_BASE + 8, 5);
	g_assert_cmphex(dsp_bus_get_outputs(bus), ==, 5);
	g_assert_cmphex(dsp_bus_take_output_events(bus), ==, 5);
	g_assert_cmphex(dsp_bus_take_output_events(bus), ==, 0);
	g_assert_cmphex(dsp_bus_read(bus, TEST_DSP_BASE + 8), ==, 5);

	dsp_bus_write(bus, TEST_INTERRUPT_BASE + 9, 0x0030);
	dsp_bus_set_input(bus, 0, true);
	g_assert_cmphex(dsp_bus_read(bus, TEST_DSP_BASE + 8), ==, 0x000D);
	g_assert_cmphex(dsp_bus_read(bus, TEST_INTERRUPT_BASE + 8), ==, 0x0010);
	g_assert_cmphex(dsp_bus_get_irq_lines(bus), ==, 2);
	dsp_bus_write(bus, TEST_INTERRUPT_BASE + 0x0A, 0x0010);
	dsp_bus_set_input(bus, 0, true);
	g_assert_cmphex(dsp_bus_read(bus, TEST_INTERRUPT_BASE + 8), ==, 0);
	dsp_bus_set_input(bus, 0, false);
	g_assert_cmphex(dsp_bus_read(bus, TEST_INTERRUPT_BASE + 8), ==, 0x0020);

	dsp_bus_write(bus, TEST_DSP_BASE + 8, UINT16_MAX);
	g_assert_cmphex(dsp_bus_get_outputs(bus), ==, 7);
	g_assert_cmphex(dsp_bus_read(bus, TEST_DSP_BASE + 8), ==, 7);
	dsp_bus_destroy(bus);
}

static void test_mcs(void) {
	test_host_t host = {};
	dsp_bus_t *bus = test_bus_create(&host);

	dsp_bus_reset(bus);
	dsp_bus_set_comm(bus, 3);
	g_assert_cmphex(dsp_bus_read(bus, TEST_MCS_BASE), ==, 3);
	dsp_bus_write(bus, TEST_MCS_BASE + 2, 1);
	g_assert_cmphex(dsp_bus_get_comm(bus), ==, 2);
	g_assert_cmphex(dsp_bus_take_comm_clear(bus), ==, 1);
	g_assert_cmphex(dsp_bus_take_comm_clear(bus), ==, 0);
	dsp_bus_clear_comm(bus, 2);
	g_assert_cmphex(dsp_bus_get_comm(bus), ==, 0);
	g_assert_cmphex(dsp_bus_read(bus, TEST_MCS_BASE + 3), ==, UINT16_MAX);
	dsp_bus_write(bus, TEST_MCS_BASE + 4, 5);
	g_assert_cmphex(dsp_bus_read(bus, TEST_MCS_BASE + 3), ==, 0xFFFA);
	dsp_bus_write(bus, TEST_MCS_BASE + 5, 1);
	g_assert_cmphex(dsp_bus_read(bus, TEST_MCS_BASE + 3), ==, 0xFFFB);

	dsp_bus_request_mcu_semaphores(bus, 3);
	dsp_bus_write(bus, TEST_MCS_BASE + 4, 1);
	dsp_bus_reset(bus);
	g_assert_cmphex(dsp_bus_get_mcu_semaphores(bus), ==, 0xFFFC);
	g_assert_cmphex(dsp_bus_read(bus, TEST_MCS_BASE + 3), ==, UINT16_MAX);
	dsp_bus_destroy(bus);
}

static void test_interrupt(void) {
	test_host_t host = {};
	dsp_bus_t *bus = test_bus_create(&host);

	dsp_bus_reset(bus);
	dsp_bus_write(bus, TEST_INTERRUPT_BASE + 1, 1);
	dsp_bus_set_request(bus, 0, true);
	g_assert_cmphex(dsp_bus_read(bus, TEST_INTERRUPT_BASE), ==, 1);
	g_assert_cmphex(dsp_bus_get_irq_lines(bus), ==, 1);
	dsp_bus_write(bus, TEST_INTERRUPT_BASE + 2, 1);
	g_assert_cmphex(dsp_bus_get_irq_lines(bus), ==, 0);
	dsp_bus_write(bus, TEST_INTERRUPT_BASE + 0x10, 5);
	g_assert_cmphex(dsp_bus_take_mcu_irqs(bus), ==, 5);
	g_assert_cmphex(dsp_bus_take_mcu_irqs(bus), ==, 0);
	dsp_bus_destroy(bus);
}

static void test_modulator(void) {
	test_host_t host = {};
	dsp_bus_t *bus = test_bus_create(&host);

	dsp_bus_reset(bus);
	g_assert_cmphex(dsp_bus_read(bus, TEST_MODULATOR_BASE + 6), ==, 0xFF);
	dsp_bus_write(bus, TEST_MODULATOR_BASE, UINT16_MAX);
	g_assert_cmphex(dsp_bus_read(bus, TEST_MODULATOR_BASE), ==, 0x071F);
	dsp_bus_write(bus, TEST_MODULATOR_BASE + 4, UINT16_MAX);
	g_assert_cmphex(dsp_bus_read(bus, TEST_MODULATOR_BASE + 4), ==, 0x0FFF);
	dsp_bus_destroy(bus);
}

static void test_afe_clock(void) {
	static const struct {
		uint16_t rate;
		size_t word_cycles;
	} receive_rates[] = {
		{ TEAK_AFE_BCON_RXRATE_KHZ_8, 1000 },
		{ TEAK_AFE_BCON_RXRATE_KHZ_16, 500 },
		{ TEAK_AFE_BCON_RXRATE_KHZ_31_746, 126 },
		{ TEAK_AFE_BCON_RXRATE_KHZ_44_444, 90 },
		{ TEAK_AFE_BCON_RXRATE_KHZ_47_619, 84 },
	};
	static const struct {
		uint16_t rate;
		size_t sample_cycles;
	} transmit_rates[] = {
		{ TEAK_AFE_BCON_TXRATE_KHZ_8, 1000 },
		{ TEAK_AFE_BCON_TXRATE_KHZ_16, 500 },
	};
	test_host_t host = {};
	dsp_bus_t *bus = test_bus_create(&host);

	/* With these clocks one DSP cycle is one AFE cycle. */
	dsp_bus_set_clock_hz(bus, 8000000);
	dsp_bus_set_ms_clock_hz(bus, 104000000);
	host.data[TEST_AFE_DAC_RING_OFFSET] = 0x1234;
	test_afe_ep_line = (test_audio_line_t) { };
	dsp_bus_reset(bus);
	dsp_bus_write(bus, TEST_AFE_BASE + TEAK_AFE_VRXCTRL2, TEAK_AFE_VRXCTRL2_RXDAC | TEAK_AFE_VRXCTRL2_VEPSP);
	dsp_bus_write(bus, TEST_AFE_BASE + TEAK_AFE_INTPTR, 0x3F3F);
	dsp_bus_write(bus, TEST_AFE_BASE + TEAK_AFE_BCON,
		TEAK_AFE_BCON_MODE | TEAK_AFE_BCON_RXSTART | TEAK_AFE_BCON_RXRATE_KHZ_8);
	dsp_bus_advance_idle(bus, 1000);
	g_assert_cmpuint(host.data_read_count, ==, 1);
	g_assert_cmphex(host.data_read_address, ==, TEST_AFE_DAC_RING_OFFSET);
	g_assert_cmpuint(test_afe_ep_line.format.sample_rate_hz, ==, 8000);
	g_assert_cmpuint(test_afe_ep_line.format.channels, ==, 1);
	g_assert_cmpuint(test_afe_ep_line.frame_count, ==, 1);
	g_assert_cmphex((uint16_t) test_afe_ep_line.last_sample, ==, 0x1234);

	for (size_t i = 0; i < ARRAY_SIZE(receive_rates); i++) {
		dsp_bus_reset(bus);
		dsp_bus_write(bus, TEST_AFE_BASE + TEAK_AFE_INTPTR, 0x3F3F);
		dsp_bus_write(bus, TEST_AFE_BASE + TEAK_AFE_BCON,
			TEAK_AFE_BCON_MODE | TEAK_AFE_BCON_RXSTART | receive_rates[i].rate);
		dsp_bus_advance_idle(bus, receive_rates[i].word_cycles - 1);
		g_assert_cmphex(dsp_bus_read(bus, TEST_AFE_BASE + TEAK_AFE_RWADDR), ==, 0);
		dsp_bus_advance_idle(bus, 1);
		g_assert_cmphex(dsp_bus_read(bus, TEST_AFE_BASE + TEAK_AFE_RWADDR), ==, 1);
	}

	dsp_bus_reset(bus);
	dsp_bus_write(bus, TEST_AFE_BASE + TEAK_AFE_INTPTR, 0x0101);
	g_assert_cmphex(dsp_bus_read(bus, TEST_AFE_BASE + TEAK_AFE_INTPTR), ==, 0x0101);
	dsp_bus_write(bus, TEST_AFE_BASE + TEAK_AFE_BCON,
		TEAK_AFE_BCON_MODE | TEAK_AFE_BCON_RXSTART | TEAK_AFE_BCON_RXRATE_KHZ_47_619);
	dsp_bus_advance_idle(bus, 84);
	g_assert_cmphex(dsp_bus_get_irq_flags(bus, 1), ==, TEAK_INT_FINTB0_VBRX);

	dsp_bus_reset(bus);
	dsp_bus_write(bus, TEST_AFE_BASE + TEAK_AFE_INTPTR, 0x3F3F);
	dsp_bus_write(bus, TEST_AFE_BASE + TEAK_AFE_BCON,
		TEAK_AFE_BCON_MODE | TEAK_AFE_BCON_RXSTART | TEAK_AFE_BCON_RXRATE_KHZ_8);
	dsp_bus_advance_idle(bus, 255);
	dsp_bus_write(bus, TEST_AFE_BASE + TEAK_AFE_BCON,
		TEAK_AFE_BCON_MODE | TEAK_AFE_BCON_RXSTART | TEAK_AFE_BCON_RXRATE_KHZ_47_619);
	dsp_bus_advance_idle(bus, 1);
	g_assert_cmphex(dsp_bus_read(bus, TEST_AFE_BASE + TEAK_AFE_RWADDR), ==, 1);

	dsp_bus_reset(bus);
	dsp_bus_write(bus, TEST_AFE_BASE + TEAK_AFE_INTPTR, 0x3F3F);
	dsp_bus_write(bus, TEST_AFE_BASE + TEAK_AFE_BCON,
		TEAK_AFE_BCON_MODE | TEAK_AFE_BCON_TXSTART | TEAK_AFE_BCON_TXRATE_KHZ_8);
	dsp_bus_advance_idle(bus, 750);
	dsp_bus_write(bus, TEST_AFE_BASE + TEAK_AFE_BCON,
		TEAK_AFE_BCON_MODE | TEAK_AFE_BCON_TXSTART | TEAK_AFE_BCON_TXRATE_KHZ_16);
	dsp_bus_advance_idle(bus, 1);
	g_assert_cmphex(dsp_bus_read(bus, TEST_AFE_BASE + TEAK_AFE_RWADDR), ==, 0x0100);

	for (size_t i = 0; i < ARRAY_SIZE(transmit_rates); i++) {
		dsp_bus_reset(bus);
		dsp_bus_write(bus, TEST_AFE_BASE + TEAK_AFE_INTPTR, 0x0101);
		dsp_bus_write(bus, TEST_AFE_BASE + TEAK_AFE_BCON,
			TEAK_AFE_BCON_MODE | TEAK_AFE_BCON_TXSTART | transmit_rates[i].rate);
		dsp_bus_advance_idle(bus, transmit_rates[i].sample_cycles - 1);
		g_assert_cmphex(dsp_bus_read(bus, TEST_AFE_BASE + TEAK_AFE_RWADDR), ==, 0);
		dsp_bus_advance_idle(bus, 1);
		g_assert_cmphex(dsp_bus_read(bus, TEST_AFE_BASE + TEAK_AFE_RWADDR), ==, 0x0100);
		g_assert_cmphex(dsp_bus_get_irq_flags(bus, 1), ==, TEAK_INT_FINTB0_VBTX);
	}
	dsp_bus_destroy(bus);
}

static void test_afe_outputs(void) {
	static const struct {
		uint16_t receive_control;
		bool line_out;
		bool power_audio;
	} cases[] = {
		{ TEAK_AFE_VRXCTRL2_RXDAC | TEAK_AFE_VRXCTRL2_VEPSP, true, false },
		{ TEAK_AFE_VRXCTRL2_RXDAC2 | TEAK_AFE_VRXCTRL2_VEPSP, true, false },
		{ TEAK_AFE_VRXCTRL2_RXDAC | (1 << TEAK_AFE_VRXCTRL2_VEPPA_SHIFT), false, true },
		{ TEAK_AFE_VRXCTRL2_RXDAC | (2 << TEAK_AFE_VRXCTRL2_VEPPA_SHIFT), false, true },
		{ TEAK_AFE_VRXCTRL2_RXDAC | TEAK_AFE_VRXCTRL2_VEPPA, false, true },
		{ TEAK_AFE_VRXCTRL2_VEPSP | TEAK_AFE_VRXCTRL2_VEPPA, false, false },
		{ TEAK_AFE_VRXCTRL2_RXDAC | TEAK_AFE_VRXCTRL2_VEPSP | TEAK_AFE_VRXCTRL2_RINGSELSP, false, false },
		{ TEAK_AFE_VRXCTRL2_RXDAC | TEAK_AFE_VRXCTRL2_VEPPA | TEAK_AFE_VRXCTRL2_RINGSELPA, false, false },
		{ TEAK_AFE_VRXCTRL2_RXDAC | TEAK_AFE_VRXCTRL2_VEPSP | TEAK_AFE_VRXCTRL2_VEPPA, true, true },
	};
	test_host_t host = {};
	dsp_bus_t *bus = test_bus_create(&host);

	dsp_bus_set_clock_hz(bus, 8000000);
	dsp_bus_set_ms_clock_hz(bus, 104000000);
	host.data[TEST_AFE_DAC_RING_OFFSET] = 0x1234;
	for (size_t i = 0; i < ARRAY_SIZE(cases); i++) {
		test_afe_ep_line = (test_audio_line_t) { };
		test_afe_eppa_line = (test_audio_line_t) { };
		dsp_bus_reset(bus);
		dsp_bus_write(bus, TEST_AFE_BASE + TEAK_AFE_VRXCTRL2, cases[i].receive_control);
		dsp_bus_write(bus, TEST_AFE_BASE + TEAK_AFE_INTPTR, 0x3F3F);
		dsp_bus_write(bus, TEST_AFE_BASE + TEAK_AFE_BCON,
			TEAK_AFE_BCON_MODE | TEAK_AFE_BCON_RXSTART | TEAK_AFE_BCON_RXRATE_KHZ_8);
		dsp_bus_advance_idle(bus, 1000);
		g_assert_cmpuint(test_afe_ep_line.frame_count, ==, cases[i].line_out);
		g_assert_cmpuint(test_afe_eppa_line.frame_count, ==, cases[i].power_audio);

		dsp_bus_write(bus, TEST_AFE_BASE + TEAK_AFE_BCON, TEAK_AFE_BCON_MODE);
		g_assert_cmpuint(test_afe_ep_line.format.channels, ==, 0);
		g_assert_cmpuint(test_afe_eppa_line.format.channels, ==, 0);
	}

	dsp_bus_destroy(bus);
}

static void test_afe_gain(void) {
	static const struct {
		uint16_t gain_code;
		int gain_mb;
	} cases[] = {
		{ 0, 0 },
		{ 1, -300 },
		{ 2, -600 },
		{ 3, -900 },
	};
	test_host_t host = {};
	dsp_bus_t *bus = test_bus_create(&host);
	uint16_t receive_control = TEAK_AFE_VRXCTRL2_RXDAC | TEAK_AFE_VRXCTRL2_VEPSP | TEAK_AFE_VRXCTRL2_VEPPA;

	dsp_bus_reset(bus);
	dsp_bus_write(bus, TEST_AFE_BASE + TEAK_AFE_VRXCTRL2, receive_control);
	dsp_bus_write(bus, TEST_AFE_BASE + TEAK_AFE_BCON, TEAK_AFE_BCON_MODE | TEAK_AFE_BCON_RXSTART);
	for (size_t i = 0; i < ARRAY_SIZE(cases); i++) {
		uint16_t line_out_gain = cases[i].gain_code << TEAK_AFE_VRXCTRL1_RXGAINSP_SHIFT;
		uint16_t power_audio_gain = (3 - cases[i].gain_code) << TEAK_AFE_VRXCTRL1_RXGAINPA_SHIFT;

		dsp_bus_write(bus, TEST_AFE_BASE + TEAK_AFE_VRXCTRL1, line_out_gain | power_audio_gain);
		g_assert_cmpint(test_afe_ep_line.gain_mb, ==, cases[i].gain_mb);
		g_assert_cmpint(test_afe_eppa_line.gain_mb, ==, -900 - cases[i].gain_mb);
	}

	dsp_bus_destroy(bus);
}

static void test_i2s_clock(void) {
	test_host_t host = { .data = { 0x8000, 0 } };
	dsp_bus_t *bus = test_bus_create(&host);

	dsp_bus_reset(bus);
	dsp_bus_set_clock_hz(bus, 26000000);
	dsp_bus_set_ms_clock_hz(bus, 104000000);
	dsp_bus_write(bus, TEST_I2S_BASE + TEAK_I2S_CTRL, TEAK_I2S_CTRL_I2SON);
	dsp_bus_write(bus, TEST_I2S_BASE + TEAK_I2S_TXINTADDR, 6);
	dsp_bus_write(bus, TEST_I2S_BASE + TEAK_I2S_CTRL, TEAK_I2S_CTRL_I2SON | TEAK_I2S_CTRL_I2STXSTART);
	dsp_bus_advance_idle(bus, 512);
	g_assert_cmphex(dsp_bus_read(bus, TEST_I2S_BASE + TEAK_I2S_RWADDR), ==, 6);
	g_assert_cmphex(dsp_bus_get_irq_flags(bus, 1), ==, TEAK_INT_FINTB0_I2S1TX);

	dsp_bus_reset(bus);
	dsp_bus_write(bus, TEST_I2S_BASE + TEAK_I2S_NUM0, 441);
	dsp_bus_write(bus, TEST_I2S_BASE + TEAK_I2S_DEN0, 32500);
	dsp_bus_write(bus, TEST_I2S_BASE + TEAK_I2S_TXCONF, TEAK_I2S_TXCONF_PERIOD_CLOCKS_32);
	dsp_bus_write(bus, TEST_I2S_BASE + TEAK_I2S_CTRL, TEAK_I2S_CTRL_I2SON | TEAK_I2S_CTRL_I2STXSTART);
	size_t configure_count = host.i2s_configure_count;
	dsp_bus_advance_idle(bus, 9433);
	g_assert_cmpuint(host.i2s_configure_count, ==, configure_count + 1);
	g_assert_true(host.i2s_transmit);
	g_assert_cmphex(dsp_bus_read(bus, TEST_I2S_BASE + TEAK_I2S_RWADDR), ==, 4);
	dsp_bus_advance_idle(bus, 1);
	g_assert_cmpuint(host.i2s_configure_count, ==, configure_count + 1);
	g_assert_cmphex(dsp_bus_read(bus, TEST_I2S_BASE + TEAK_I2S_RWADDR), ==, 6);

	dsp_bus_write(bus, TEST_I2S_BASE + TEAK_I2S_CTRL, 0);
	dsp_bus_write(bus, TEST_I2S_BASE + TEAK_I2S_NUM0, TEAK_I2S_NUM0_FREF_CLOCK_104MHZ | 441);
	dsp_bus_write(bus, TEST_I2S_BASE + TEAK_I2S_CTRL, TEAK_I2S_CTRL_I2SON | TEAK_I2S_CTRL_I2STXSTART);
	dsp_bus_advance_idle(bus, 2358);
	g_assert_cmphex(dsp_bus_read(bus, TEST_I2S_BASE + TEAK_I2S_RWADDR), ==, 4);
	dsp_bus_advance_idle(bus, 1);
	g_assert_cmphex(dsp_bus_read(bus, TEST_I2S_BASE + TEAK_I2S_RWADDR), ==, 6);

	dsp_bus_write(bus, TEST_I2S_BASE + TEAK_I2S_CTRL, 0);
	dsp_bus_write(bus, TEST_I2S_BASE + TEAK_I2S_NUM1, TEAK_I2S_NUM1_FREF_CLOCK_104MHZ | 441);
	dsp_bus_write(bus, TEST_I2S_BASE + TEAK_I2S_DEN1, 8125);
	dsp_bus_write(bus, TEST_I2S_BASE + TEAK_I2S_CSEL, TEAK_I2S_CSEL_TXCLKSEL);
	dsp_bus_write(bus, TEST_I2S_BASE + TEAK_I2S_CTRL, TEAK_I2S_CTRL_I2SON | TEAK_I2S_CTRL_I2STXSTART);
	dsp_bus_advance_idle(bus, 589);
	g_assert_cmphex(dsp_bus_read(bus, TEST_I2S_BASE + TEAK_I2S_RWADDR), ==, 4);
	dsp_bus_advance_idle(bus, 1);
	g_assert_cmphex(dsp_bus_read(bus, TEST_I2S_BASE + TEAK_I2S_RWADDR), ==, 6);

	dsp_bus_write(bus, TEST_I2S_BASE + TEAK_I2S_CTRL, 0);
	dsp_bus_set_ms_clock_hz(bus, 26000000);
	dsp_bus_write(bus, TEST_I2S_BASE + TEAK_I2S_CTRL, TEAK_I2S_CTRL_I2SON | TEAK_I2S_CTRL_I2STXSTART);
	dsp_bus_advance_idle(bus, 2358);
	g_assert_cmphex(dsp_bus_read(bus, TEST_I2S_BASE + TEAK_I2S_RWADDR), ==, 4);
	dsp_bus_advance_idle(bus, 1);
	g_assert_cmphex(dsp_bus_read(bus, TEST_I2S_BASE + TEAK_I2S_RWADDR), ==, 6);

	dsp_bus_write(bus, TEST_I2S_BASE + TEAK_I2S_CTRL, 0);
	dsp_bus_set_ms_clock_hz(bus, 0);
	dsp_bus_write(bus, TEST_I2S_BASE + TEAK_I2S_CTRL, TEAK_I2S_CTRL_I2SON | TEAK_I2S_CTRL_I2STXSTART);
	dsp_bus_advance_idle(bus, 10000);
	g_assert_cmphex(dsp_bus_read(bus, TEST_I2S_BASE + TEAK_I2S_RWADDR), ==, 4);
	dsp_bus_set_ms_clock_hz(bus, 104000000);

	dsp_bus_write(bus, TEST_I2S_BASE + TEAK_I2S_CTRL, TEAK_I2S_CTRL_I2SON);
	dsp_bus_write(bus, TEST_I2S_BASE + TEAK_I2S_CTRL, TEAK_I2S_CTRL_I2SON | TEAK_I2S_CTRL_I2STXSTART);
	g_assert_cmphex(dsp_bus_read(bus, TEST_I2S_BASE + TEAK_I2S_RWADDR), ==, 4);

	dsp_bus_write(bus, TEST_I2S_BASE + TEAK_I2S_CTRL, 0);
	dsp_bus_write(bus, TEST_I2S_BASE + TEAK_I2S_CSEL, TEAK_I2S_CSEL_TXCLKSEL);
	dsp_bus_write(bus, TEST_I2S_BASE + TEAK_I2S_TXCONF,
		TEAK_I2S_TXCONF_PERIOD_CLOCKS_32 | TEAK_I2S_TXCONF_WIDTH_BITS_32);
	dsp_bus_write(bus, TEST_I2S_BASE + TEAK_I2S_CTRL, TEAK_I2S_CTRL_I2SON | TEAK_I2S_CTRL_I2STXSTART);
	dsp_bus_advance_idle(bus, 590);
	g_assert_cmphex(dsp_bus_read(bus, TEST_I2S_BASE + TEAK_I2S_RWADDR), ==, 8);

	dsp_bus_write(bus, TEST_I2S_BASE + TEAK_I2S_CTRL, 0);
	dsp_bus_write(bus, TEST_I2S_BASE + TEAK_I2S_TXCONF, TEAK_I2S_TXCONF_PERIOD_CLOCKS_32);
	dsp_bus_write(bus, TEST_I2S_BASE + TEAK_I2S_CTRL,
		TEAK_I2S_CTRL_I2SON | TEAK_I2S_CTRL_I2STXSTART | TEAK_I2S_CTRL_TXPCM);
	g_assert_cmphex(dsp_bus_read(bus, TEST_I2S_BASE + TEAK_I2S_RWADDR), ==, 1);

	dsp_bus_reset(bus);
	dsp_bus_write(bus, TEST_I2S_BASE + TEAK_I2S_NUM1, TEAK_I2S_NUM1_FREF_CLOCK_104MHZ | 441);
	dsp_bus_write(bus, TEST_I2S_BASE + TEAK_I2S_DEN1, 8125);
	dsp_bus_write(bus, TEST_I2S_BASE + TEAK_I2S_RXCONF, TEAK_I2S_RXCONF_PERIOD_CLOCKS_32);
	host.i2s_input[0] = 0x1234;
	host.i2s_input[1] = (int16_t) 0xFEDC;
	host.i2s_input_frame_count = 1;
	host.i2s_input_position = 0;
	dsp_bus_write(bus, TEST_I2S_BASE + TEAK_I2S_CTRL, TEAK_I2S_CTRL_I2SON);
	dsp_bus_write(bus, TEST_I2S_BASE + TEAK_I2S_RXINTADDR, 4);
	dsp_bus_write(bus, TEST_I2S_BASE + TEAK_I2S_CTRL, TEAK_I2S_CTRL_I2SON | TEAK_I2S_CTRL_I2SRXSTART);
	dsp_bus_advance_idle(bus, 1180);
	g_assert_cmpuint(host.i2s_sample_rate_hz, ==, 44100);
	g_assert_cmpuint(host.i2s_channels, ==, 2);
	g_assert_cmphex(host.data[0], ==, 0x8000);
	g_assert_cmphex(host.data[TEST_I2S_RX_RAM_BASE], ==, 0x1234);
	g_assert_cmphex(host.data[TEST_I2S_RX_RAM_BASE + 1], ==, 0xFEDC);
	g_assert_cmphex(host.data[TEST_I2S_RX_RAM_BASE + 2], ==, 0);
	g_assert_cmphex(host.data[TEST_I2S_RX_RAM_BASE + 3], ==, 0);
	g_assert_cmphex(dsp_bus_read(bus, TEST_I2S_BASE + TEAK_I2S_RWADDR), ==, 0x0400);
	g_assert_cmphex(dsp_bus_get_irq_flags(bus, 1), ==, TEAK_INT_FINTB0_I2S1RX);

	host.data[0] = 0x8000;
	host.data[1] = 0;
	dsp_bus_reset(bus);
	dsp_bus_write(bus, TEST_I2S_BASE + TEAK_I2S_TXCONF, TEAK_I2S_TXCONF_MONO_LEFT);
	dsp_bus_write(bus, TEST_I2S_BASE + TEAK_I2S_CTRL, TEAK_I2S_CTRL_I2SON | TEAK_I2S_CTRL_I2STXSTART);
	dsp_bus_advance_idle(bus, 512);
	g_assert_cmpuint(host.i2s_sample_rate_hz, ==, 50781);
	g_assert_cmpuint(host.i2s_channels, ==, 2);
	g_assert_cmpint(host.i2s_output[0], ==, INT16_MIN);
	g_assert_cmpint(host.i2s_output[1], ==, INT16_MIN);

	dsp_bus_write(bus, TEST_I2S_BASE + TEAK_I2S_CTRL, 0);
	dsp_bus_write(bus, TEST_I2S_BASE + TEAK_I2S_TXCONF, TEAK_I2S_TXCONF_MONO_STEREO);
	dsp_bus_write(bus, TEST_I2S_BASE + TEAK_I2S_CTRL, TEAK_I2S_CTRL_I2SON | TEAK_I2S_CTRL_I2STXSTART);
	dsp_bus_advance_idle(bus, 512);
	g_assert_cmpint(host.i2s_output[0], ==, INT16_MIN);
	g_assert_cmpint(host.i2s_output[1], ==, 0);
	dsp_bus_destroy(bus);
}

static void test_i2s_tx_clock(void) {
	test_host_t host = {};
	dsp_bus_t *bus = test_bus_create(&host);

	dsp_bus_reset(bus);
	dsp_bus_set_clock_hz(bus, 26000000);
	dsp_bus_set_ms_clock_hz(bus, 104000000);
	dsp_bus_write(bus, TEST_I2S_TX_BASE + TEAK_I2S3_NUM, TEAK_I2S3_NUM_FREF_CLOCK_104MHZ | 441);
	dsp_bus_write(bus, TEST_I2S_TX_BASE + TEAK_I2S3_DEN, 32500);
	dsp_bus_write(bus, TEST_I2S_TX_BASE + TEAK_I2S3_TXCONF, TEAK_I2S3_TXCONF_PERIOD_CLOCKS_32);
	dsp_bus_write(bus, TEST_I2S_TX_BASE + TEAK_I2S3_CTRL, TEAK_I2S3_CTRL_I2SON | TEAK_I2S3_CTRL_I2STXSTART);
	dsp_bus_advance_idle(bus, 2359);
	g_assert_cmphex(dsp_bus_read(bus, TEST_I2S_TX_BASE + TEAK_I2S3_RADDR), ==, 2);

	dsp_bus_write(bus, TEST_I2S_TX_BASE + TEAK_I2S3_CTRL, 0);
	dsp_bus_set_ms_clock_hz(bus, 26000000);
	dsp_bus_write(bus, TEST_I2S_TX_BASE + TEAK_I2S3_CTRL, TEAK_I2S3_CTRL_I2SON | TEAK_I2S3_CTRL_I2STXSTART);
	dsp_bus_advance_idle(bus, 9433);
	g_assert_cmphex(dsp_bus_read(bus, TEST_I2S_TX_BASE + TEAK_I2S3_RADDR), ==, 0);
	dsp_bus_advance_idle(bus, 1);
	g_assert_cmphex(dsp_bus_read(bus, TEST_I2S_TX_BASE + TEAK_I2S3_RADDR), ==, 2);

	dsp_bus_write(bus, TEST_I2S_TX_BASE + TEAK_I2S3_CTRL, 0);
	dsp_bus_set_ms_clock_hz(bus, 0);
	dsp_bus_write(bus, TEST_I2S_TX_BASE + TEAK_I2S3_CTRL, TEAK_I2S3_CTRL_I2SON | TEAK_I2S3_CTRL_I2STXSTART);
	dsp_bus_advance_idle(bus, 10000);
	g_assert_cmphex(dsp_bus_read(bus, TEST_I2S_TX_BASE + TEAK_I2S3_RADDR), ==, 0);

	dsp_bus_write(bus, TEST_I2S_TX_BASE + TEAK_I2S3_CTRL, 0);
	dsp_bus_set_ms_clock_hz(bus, 104000000);
	dsp_bus_write(bus, TEST_I2S_TX_BASE + TEAK_I2S3_CSEL, TEAK_I2S3_CSEL_TXCLKSEL);
	dsp_bus_write(bus, TEST_I2S_TX_BASE + TEAK_I2S3_CTRL, TEAK_I2S3_CTRL_I2SON | TEAK_I2S3_CTRL_I2STXSTART);
	dsp_bus_advance_idle(bus, 2359);
	g_assert_cmphex(dsp_bus_read(bus, TEST_I2S_TX_BASE + TEAK_I2S3_RADDR), ==, 0);
	dsp_bus_destroy(bus);
}

static void test_unknown(void) {
	test_host_t host = {};
	dsp_bus_t *bus = test_bus_create(&host);

	dsp_bus_reset(bus);
	dsp_bus_write(bus, TEST_UNKNOWN_BASE + 2, 0x1234);
	g_assert_cmphex(dsp_bus_read(bus, TEST_UNKNOWN_BASE + 2), ==, 0);
	dsp_bus_write(bus, TEST_DSP_BASE + 1, 0x5678);
	g_assert_cmphex(dsp_bus_read(bus, TEST_DSP_BASE + 1), ==, 0);
	dsp_bus_destroy(bus);
}

int main(int argc, char **argv) {
	g_test_init(&argc, &argv, NULL);
	debug_channel_init(&test_dsp_dbg, &test_dsp_info, "DSP");
	g_test_add_func("/pmb887x/dsp/peripheral/control", test_control);
	g_test_add_func("/pmb887x/dsp/peripheral/pads", test_pads);
	g_test_add_func("/pmb887x/dsp/peripheral/mcs", test_mcs);
	g_test_add_func("/pmb887x/dsp/peripheral/interrupt", test_interrupt);
	g_test_add_func("/pmb887x/dsp/peripheral/modulator", test_modulator);
	g_test_add_func("/pmb887x/dsp/peripheral/afe-clock", test_afe_clock);
	g_test_add_func("/pmb887x/dsp/peripheral/afe-outputs", test_afe_outputs);
	g_test_add_func("/pmb887x/dsp/peripheral/afe-gain", test_afe_gain);
	g_test_add_func("/pmb887x/dsp/peripheral/i2s-clock", test_i2s_clock);
	g_test_add_func("/pmb887x/dsp/peripheral/i2s-tx-clock", test_i2s_tx_clock);
	g_test_add_func("/pmb887x/dsp/peripheral/timer-clock", test_timer_clock);
	g_test_add_func("/pmb887x/dsp/peripheral/bus-horizon", test_bus_horizon);
	g_test_add_func("/pmb887x/dsp/peripheral/unknown", test_unknown);
	return g_test_run();
}

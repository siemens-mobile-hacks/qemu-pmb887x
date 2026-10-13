#include "qemu/osdep.h"

#include "hw/arm/pmb887x/gen/dsp.h"
#include "hw/arm/pmb887x/dsp/peripheral/afe-audio.h"
#include "hw/arm/pmb887x/dsp/peripheral.h"
#include "hw/arm/pmb887x/rf.h"
#include "hw/arm/pmb887x/regs_dump.h"
#include "hw/arm/pmb887x/trace_common.h"

#define TEST_INTERRUPT_BASE	0x1000
#define TEST_MCS_BASE		0x1020
#define TEST_DSP_BASE		0x1030
#define TEST_MODULATOR_BASE	0x1040
#define TEST_AFE_BASE		0x1050
#define TEST_UNKNOWN_BASE	0x1060
#define TEST_I2S_BASE		0x1070
#define TEST_I2S_TX_BASE	0x1080
#define TEST_AFE_DAC_RING_OFFSET	0x40
#define TEST_I2S_RX_RAM_BASE	(TEAK_I2S_RWADDR_RDADDR + 1U)

uint64_t pmb887x_trace_io_mask;
uint64_t pmb887x_trace_log_mask;

/* The host-unit test replaces external frontends, never DSP model code. */
static size_t test_audio_sample_count;
static uint16_t test_audio_sample;

afe_audio_t *afe_audio_create(void) {
	return NULL;
}

void afe_audio_destroy(afe_audio_t *_audio) {
}

void afe_audio_reset(afe_audio_t *_audio) {
}

void afe_audio_set_rate(afe_audio_t *_audio, unsigned _frequency_hz) {
}

void afe_audio_produce(afe_audio_t *_audio, uint16_t sample_word) {
	test_audio_sample_count++;
	test_audio_sample = sample_word;
}

bool pmb887x_iq_bus_has_source(const pmb887x_iq_bus_t *bus) {
	g_assert_null(bus);
	return false;
}

pmb887x_iq_sample_t pmb887x_iq_bus_read(pmb887x_iq_bus_t *bus, int64_t _timestamp, uint64_t _sample_index) {
	g_assert_null(bus);
	return (pmb887x_iq_sample_t) { 0, 0 };
}

typedef struct test_host_t {
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

typedef struct test_trace_state_t {
	size_t reads;
	size_t writes;
	uint32_t address;
	uint32_t value;
	uint32_t pc;
} test_trace_state_t;

static test_trace_state_t trace_state;

void pmb887x_dump_io_read_ex(pmb887x_trace_io_t trace_io, uint32_t addr, uint32_t size, uint32_t value, uint32_t pc, uint32_t lr) {
	g_assert_cmpint(trace_io, ==, PMB887X_TRACE_IO_DSP);
	g_assert_cmpuint(size, ==, sizeof(uint16_t));
	g_assert_cmpuint(lr, ==, 0);
	trace_state.reads++;
	trace_state.address = addr;
	trace_state.value = value;
	trace_state.pc = pc;
}

void pmb887x_dump_io_write_ex(pmb887x_trace_io_t trace_io, uint32_t addr, uint32_t size, uint32_t value, uint32_t pc, uint32_t lr) {
	g_assert_cmpint(trace_io, ==, PMB887X_TRACE_IO_DSP);
	g_assert_cmpuint(size, ==, sizeof(uint16_t));
	g_assert_cmpuint(lr, ==, 0);
	trace_state.writes++;
	trace_state.address = addr;
	trace_state.value = value;
	trace_state.pc = pc;
}

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

static pmb887x_dsp_peripheral_bus_t *test_bus_create(test_host_t *host) {
	static const pmb887x_dsp_peripheral_config_t peripherals[] = {
		{ "INT", PMB887X_DSP_PERIPHERAL_INTERRUPT, TEST_INTERRUPT_BASE, 0x16 },
		{ "MCS", PMB887X_DSP_PERIPHERAL_MCS, TEST_MCS_BASE, 0x06 },
		{ "DSP", PMB887X_DSP_PERIPHERAL_DSP, TEST_DSP_BASE, 0x09 },
		{ "MOD", PMB887X_DSP_PERIPHERAL_MODULATOR, TEST_MODULATOR_BASE, 0x0B },
		{ "AFE", PMB887X_DSP_PERIPHERAL_AFE, TEST_AFE_BASE, 0x10 },
		{ "I2S1", PMB887X_DSP_PERIPHERAL_I2S, TEST_I2S_BASE, 0x0B },
		{ "I2S3", PMB887X_DSP_PERIPHERAL_I2S_TX, TEST_I2S_TX_BASE, 0x0B },
	};
	static const pmb887x_dsp_config_t config = {
		.hardware_version = PMB887X_DSP_HW_VERSION_E101,
		.mmio_base = TEST_INTERRUPT_BASE,
		.mmio_size = 0x100,
		.peripherals = peripherals,
		.peripherals_count = ARRAY_SIZE(peripherals),
	};
	const dsp_ports_t ports = {
		.i2s = {
			{
				.tx_bus = (I2STxBus *) host,
				.rx_bus = (I2SRxBus *) host,
			},
		},
	};
	pmb887x_dsp_peripheral_host_t peripheral_host = {
		.opaque = host,
		.set_page = test_set_page,
		.set_core_disabled = test_set_core_disabled,
		.get_pc = test_get_pc,
		.data_read = test_data_read,
		.data_write = test_data_write,
	};

	return pmb887x_dsp_peripheral_bus_create(&config, &peripheral_host, &ports);
}

static void test_control(void) {
	test_host_t host = { .page = UINT16_MAX, .pc = 0x1234 };
	pmb887x_dsp_peripheral_bus_t *bus = test_bus_create(&host);

	pmb887x_dsp_peripheral_bus_reset(bus);
	g_assert_cmphex(host.page, ==, 0);
	g_assert_false(host.core_disabled);
	g_assert_cmphex(pmb887x_dsp_peripheral_bus_read(bus, TEST_DSP_BASE), ==, 0xE101);
	g_assert_cmphex(pmb887x_dsp_peripheral_bus_read(bus, TEST_DSP_BASE + 2), ==, 0);
	pmb887x_dsp_peripheral_bus_write(bus, TEST_DSP_BASE + 1, 0);
	g_assert_true(host.core_disabled);
	pmb887x_dsp_peripheral_bus_write(bus, TEST_DSP_BASE + 3, 5);
	g_assert_cmphex(host.page, ==, 5);
	g_assert_cmphex(pmb887x_dsp_peripheral_bus_read(bus, TEST_DSP_BASE + 3), ==, 5);
	pmb887x_dsp_peripheral_bus_destroy(bus);
}

static void test_pads(void) {
	test_host_t host = {};
	pmb887x_dsp_peripheral_bus_t *bus = test_bus_create(&host);

	pmb887x_dsp_peripheral_bus_reset(bus);
	pmb887x_dsp_peripheral_bus_write(bus, TEST_DSP_BASE + 8, 5);
	g_assert_cmphex(pmb887x_dsp_peripheral_bus_get_outputs(bus), ==, 5);
	g_assert_cmphex(pmb887x_dsp_peripheral_bus_take_output_events(bus), ==, 5);
	g_assert_cmphex(pmb887x_dsp_peripheral_bus_take_output_events(bus), ==, 0);
	g_assert_cmphex(pmb887x_dsp_peripheral_bus_read(bus, TEST_DSP_BASE + 8), ==, 5);

	pmb887x_dsp_peripheral_bus_write(bus, TEST_INTERRUPT_BASE + 9, 0x0030);
	pmb887x_dsp_peripheral_bus_set_input(bus, 0, true);
	g_assert_cmphex(pmb887x_dsp_peripheral_bus_read(bus, TEST_DSP_BASE + 8), ==, 0x000D);
	g_assert_cmphex(pmb887x_dsp_peripheral_bus_read(bus, TEST_INTERRUPT_BASE + 8), ==, 0x0010);
	g_assert_cmphex(pmb887x_dsp_peripheral_bus_get_interrupt_lines(bus), ==, 2);
	pmb887x_dsp_peripheral_bus_write(bus, TEST_INTERRUPT_BASE + 0x0A, 0x0010);
	pmb887x_dsp_peripheral_bus_set_input(bus, 0, true);
	g_assert_cmphex(pmb887x_dsp_peripheral_bus_read(bus, TEST_INTERRUPT_BASE + 8), ==, 0);
	pmb887x_dsp_peripheral_bus_set_input(bus, 0, false);
	g_assert_cmphex(pmb887x_dsp_peripheral_bus_read(bus, TEST_INTERRUPT_BASE + 8), ==, 0x0020);

	pmb887x_dsp_peripheral_bus_write(bus, TEST_DSP_BASE + 8, UINT16_MAX);
	g_assert_cmphex(pmb887x_dsp_peripheral_bus_get_outputs(bus), ==, 7);
	g_assert_cmphex(pmb887x_dsp_peripheral_bus_read(bus, TEST_DSP_BASE + 8), ==, 7);
	pmb887x_dsp_peripheral_bus_destroy(bus);
}

static void test_mcs(void) {
	test_host_t host = {};
	pmb887x_dsp_peripheral_bus_t *bus = test_bus_create(&host);

	pmb887x_dsp_peripheral_bus_reset(bus);
	pmb887x_dsp_peripheral_bus_set_communication_flags(bus, 3);
	g_assert_cmphex(pmb887x_dsp_peripheral_bus_read(bus, TEST_MCS_BASE), ==, 3);
	pmb887x_dsp_peripheral_bus_write(bus, TEST_MCS_BASE + 2, 1);
	g_assert_cmphex(pmb887x_dsp_peripheral_bus_get_communication_flags(bus), ==, 2);
	g_assert_cmphex(pmb887x_dsp_peripheral_bus_take_communication_clear(bus), ==, 1);
	g_assert_cmphex(pmb887x_dsp_peripheral_bus_take_communication_clear(bus), ==, 0);
	pmb887x_dsp_peripheral_bus_clear_communication_flags(bus, 2);
	g_assert_cmphex(pmb887x_dsp_peripheral_bus_get_communication_flags(bus), ==, 0);
	g_assert_cmphex(pmb887x_dsp_peripheral_bus_read(bus, TEST_MCS_BASE + 3), ==, UINT16_MAX);
	pmb887x_dsp_peripheral_bus_write(bus, TEST_MCS_BASE + 4, 5);
	g_assert_cmphex(pmb887x_dsp_peripheral_bus_read(bus, TEST_MCS_BASE + 3), ==, 0xFFFA);
	pmb887x_dsp_peripheral_bus_write(bus, TEST_MCS_BASE + 5, 1);
	g_assert_cmphex(pmb887x_dsp_peripheral_bus_read(bus, TEST_MCS_BASE + 3), ==, 0xFFFB);

	pmb887x_dsp_peripheral_bus_request_mcu_semaphores(bus, 3);
	pmb887x_dsp_peripheral_bus_write(bus, TEST_MCS_BASE + 4, 1);
	pmb887x_dsp_peripheral_bus_reset(bus);
	g_assert_cmphex(pmb887x_dsp_peripheral_bus_get_mcu_semaphore_status(bus), ==, 0xFFFC);
	g_assert_cmphex(pmb887x_dsp_peripheral_bus_read(bus, TEST_MCS_BASE + 3), ==, UINT16_MAX);
	pmb887x_dsp_peripheral_bus_destroy(bus);
}

static void test_interrupt(void) {
	test_host_t host = {};
	pmb887x_dsp_peripheral_bus_t *bus = test_bus_create(&host);

	pmb887x_dsp_peripheral_bus_reset(bus);
	pmb887x_dsp_peripheral_bus_write(bus, TEST_INTERRUPT_BASE + 1, 1);
	pmb887x_dsp_peripheral_bus_set_request(bus, 0, true);
	g_assert_cmphex(pmb887x_dsp_peripheral_bus_read(bus, TEST_INTERRUPT_BASE), ==, 1);
	g_assert_cmphex(pmb887x_dsp_peripheral_bus_get_interrupt_lines(bus), ==, 1);
	pmb887x_dsp_peripheral_bus_write(bus, TEST_INTERRUPT_BASE + 2, 1);
	g_assert_cmphex(pmb887x_dsp_peripheral_bus_get_interrupt_lines(bus), ==, 0);
	pmb887x_dsp_peripheral_bus_write(bus, TEST_INTERRUPT_BASE + 0x10, 5);
	g_assert_cmphex(pmb887x_dsp_peripheral_bus_take_mcu_interrupt_events(bus), ==, 5);
	g_assert_cmphex(pmb887x_dsp_peripheral_bus_take_mcu_interrupt_events(bus), ==, 0);
	pmb887x_dsp_peripheral_bus_destroy(bus);
}

static void test_modulator(void) {
	test_host_t host = {};
	pmb887x_dsp_peripheral_bus_t *bus = test_bus_create(&host);

	pmb887x_dsp_peripheral_bus_reset(bus);
	g_assert_cmphex(pmb887x_dsp_peripheral_bus_read(bus, TEST_MODULATOR_BASE + 6), ==, 0xFF);
	pmb887x_dsp_peripheral_bus_write(bus, TEST_MODULATOR_BASE, UINT16_MAX);
	g_assert_cmphex(pmb887x_dsp_peripheral_bus_read(bus, TEST_MODULATOR_BASE), ==, 0x0101);
	pmb887x_dsp_peripheral_bus_write(bus, TEST_MODULATOR_BASE + 4, UINT16_MAX);
	g_assert_cmphex(pmb887x_dsp_peripheral_bus_read(bus, TEST_MODULATOR_BASE + 4), ==, 0x0FFF);
	pmb887x_dsp_peripheral_bus_destroy(bus);
}

static void test_afe_clock(void) {
	static const struct {
		uint16_t rate;
		size_t sample_cycles;
	} receive_rates[] = {
		{ TEAK_AFE_BCON_RXRATE_KHZ_8, 1000 },
		{ TEAK_AFE_BCON_RXRATE_KHZ_16, 500 },
		{ TEAK_AFE_BCON_RXRATE_KHZ_31_746, 252 },
		{ TEAK_AFE_BCON_RXRATE_KHZ_44_444, 180 },
		{ TEAK_AFE_BCON_RXRATE_KHZ_47_619, 168 },
	};
	static const struct {
		uint16_t rate;
		size_t sample_cycles;
	} transmit_rates[] = {
		{ TEAK_AFE_BCON_TXRATE_KHZ_8, 1000 },
		{ TEAK_AFE_BCON_TXRATE_KHZ_16, 500 },
	};
	test_host_t host = {};
	pmb887x_dsp_peripheral_bus_t *bus = test_bus_create(&host);

	host.data[TEST_AFE_DAC_RING_OFFSET] = 0x1234;
	test_audio_sample_count = 0;
	pmb887x_dsp_peripheral_bus_reset(bus);
	pmb887x_dsp_peripheral_bus_write(bus, TEST_AFE_BASE + TEAK_AFE_INTPTR, 0x3F3F);
	pmb887x_dsp_peripheral_bus_write(bus, TEST_AFE_BASE + TEAK_AFE_BCON,
		TEAK_AFE_BCON_MODE | TEAK_AFE_BCON_RXSTART | TEAK_AFE_BCON_RXRATE_KHZ_8);
	pmb887x_dsp_peripheral_bus_advance_afe(bus, 1000);
	g_assert_cmpuint(host.data_read_count, ==, 1);
	g_assert_cmphex(host.data_read_address, ==, TEST_AFE_DAC_RING_OFFSET);
	g_assert_cmpuint(test_audio_sample_count, ==, 1);
	g_assert_cmphex(test_audio_sample, ==, 0x1234);

	for (size_t i = 0; i < ARRAY_SIZE(receive_rates); i++) {
		pmb887x_dsp_peripheral_bus_reset(bus);
		pmb887x_dsp_peripheral_bus_write(bus, TEST_AFE_BASE + TEAK_AFE_INTPTR, 0x3F3F);
		pmb887x_dsp_peripheral_bus_write(bus, TEST_AFE_BASE + TEAK_AFE_BCON,
			TEAK_AFE_BCON_MODE | TEAK_AFE_BCON_RXSTART | receive_rates[i].rate);
		pmb887x_dsp_peripheral_bus_advance_afe(bus, receive_rates[i].sample_cycles - 1);
		g_assert_cmphex(pmb887x_dsp_peripheral_bus_read(bus, TEST_AFE_BASE + TEAK_AFE_RWADDR), ==, 0);
		pmb887x_dsp_peripheral_bus_advance_afe(bus, 1);
		g_assert_cmphex(pmb887x_dsp_peripheral_bus_read(bus, TEST_AFE_BASE + TEAK_AFE_RWADDR), ==, 1);
	}

	pmb887x_dsp_peripheral_bus_reset(bus);
	pmb887x_dsp_peripheral_bus_write(bus, TEST_AFE_BASE + TEAK_AFE_INTPTR, 0x0101);
	g_assert_cmphex(pmb887x_dsp_peripheral_bus_read(bus, TEST_AFE_BASE + TEAK_AFE_INTPTR), ==, 0x0101);
	pmb887x_dsp_peripheral_bus_write(bus, TEST_AFE_BASE + TEAK_AFE_BCON,
		TEAK_AFE_BCON_MODE | TEAK_AFE_BCON_RXSTART | TEAK_AFE_BCON_RXRATE_KHZ_47_619);
	pmb887x_dsp_peripheral_bus_advance_afe(bus, 168);
	g_assert_cmphex(pmb887x_dsp_peripheral_bus_get_interrupt_flags(bus, 1), ==, TEAK_INT_FINTB0_VBRX);

	pmb887x_dsp_peripheral_bus_reset(bus);
	pmb887x_dsp_peripheral_bus_write(bus, TEST_AFE_BASE + TEAK_AFE_INTPTR, 0x3F3F);
	pmb887x_dsp_peripheral_bus_write(bus, TEST_AFE_BASE + TEAK_AFE_BCON,
		TEAK_AFE_BCON_MODE | TEAK_AFE_BCON_RXSTART | TEAK_AFE_BCON_RXRATE_KHZ_8);
	pmb887x_dsp_peripheral_bus_advance_afe(bus, 255);
	pmb887x_dsp_peripheral_bus_write(bus, TEST_AFE_BASE + TEAK_AFE_BCON,
		TEAK_AFE_BCON_MODE | TEAK_AFE_BCON_RXSTART | TEAK_AFE_BCON_RXRATE_KHZ_47_619);
	pmb887x_dsp_peripheral_bus_advance_afe(bus, 1);
	g_assert_cmphex(pmb887x_dsp_peripheral_bus_read(bus, TEST_AFE_BASE + TEAK_AFE_RWADDR), ==, 1);

	pmb887x_dsp_peripheral_bus_reset(bus);
	pmb887x_dsp_peripheral_bus_write(bus, TEST_AFE_BASE + TEAK_AFE_INTPTR, 0x3F3F);
	pmb887x_dsp_peripheral_bus_write(bus, TEST_AFE_BASE + TEAK_AFE_BCON,
		TEAK_AFE_BCON_MODE | TEAK_AFE_BCON_TXSTART | TEAK_AFE_BCON_TXRATE_KHZ_8);
	pmb887x_dsp_peripheral_bus_advance_afe(bus, 750);
	pmb887x_dsp_peripheral_bus_write(bus, TEST_AFE_BASE + TEAK_AFE_BCON,
		TEAK_AFE_BCON_MODE | TEAK_AFE_BCON_TXSTART | TEAK_AFE_BCON_TXRATE_KHZ_16);
	pmb887x_dsp_peripheral_bus_advance_afe(bus, 1);
	g_assert_cmphex(pmb887x_dsp_peripheral_bus_read(bus, TEST_AFE_BASE + TEAK_AFE_RWADDR), ==, 0x0100);

	for (size_t i = 0; i < ARRAY_SIZE(transmit_rates); i++) {
		pmb887x_dsp_peripheral_bus_reset(bus);
		pmb887x_dsp_peripheral_bus_write(bus, TEST_AFE_BASE + TEAK_AFE_INTPTR, 0x0101);
		pmb887x_dsp_peripheral_bus_write(bus, TEST_AFE_BASE + TEAK_AFE_BCON,
			TEAK_AFE_BCON_MODE | TEAK_AFE_BCON_TXSTART | transmit_rates[i].rate);
		pmb887x_dsp_peripheral_bus_advance_afe(bus, transmit_rates[i].sample_cycles - 1);
		g_assert_cmphex(pmb887x_dsp_peripheral_bus_read(bus, TEST_AFE_BASE + TEAK_AFE_RWADDR), ==, 0);
		pmb887x_dsp_peripheral_bus_advance_afe(bus, 1);
		g_assert_cmphex(pmb887x_dsp_peripheral_bus_read(bus, TEST_AFE_BASE + TEAK_AFE_RWADDR), ==, 0x0100);
		g_assert_cmphex(pmb887x_dsp_peripheral_bus_get_interrupt_flags(bus, 1), ==, TEAK_INT_FINTB0_VBTX);
	}
	pmb887x_dsp_peripheral_bus_destroy(bus);
}

static void test_i2s_clock(void) {
	test_host_t host = { .data = { 0x8000, 0 } };
	pmb887x_dsp_peripheral_bus_t *bus = test_bus_create(&host);

	pmb887x_dsp_peripheral_bus_reset(bus);
	pmb887x_dsp_peripheral_bus_set_clock_hz(bus, 26000000);
	pmb887x_dsp_peripheral_bus_set_ms_clock_hz(bus, 104000000);
	pmb887x_dsp_peripheral_bus_write(bus, TEST_I2S_BASE + TEAK_I2S_TXINTADDR, 2);
	pmb887x_dsp_peripheral_bus_write(bus, TEST_I2S_BASE + TEAK_I2S_CTRL,
		TEAK_I2S_CTRL_I2SON | TEAK_I2S_CTRL_I2STXSTART);
	pmb887x_dsp_peripheral_bus_advance_idle(bus, 512);
	g_assert_cmphex(pmb887x_dsp_peripheral_bus_read(bus, TEST_I2S_BASE + TEAK_I2S_RWADDR), ==, 2);
	g_assert_cmphex(pmb887x_dsp_peripheral_bus_get_interrupt_flags(bus, 1), ==, TEAK_INT_FINTB0_I2S1TX);

	pmb887x_dsp_peripheral_bus_reset(bus);
	pmb887x_dsp_peripheral_bus_write(bus, TEST_I2S_BASE + TEAK_I2S_NUM0, 441);
	pmb887x_dsp_peripheral_bus_write(bus, TEST_I2S_BASE + TEAK_I2S_DEN0, 32500);
	pmb887x_dsp_peripheral_bus_write(bus, TEST_I2S_BASE + TEAK_I2S_TXCONF,
		TEAK_I2S_TXCONF_PERIOD_CLOCKS_32);
	pmb887x_dsp_peripheral_bus_write(bus, TEST_I2S_BASE + TEAK_I2S_CTRL,
		TEAK_I2S_CTRL_I2SON | TEAK_I2S_CTRL_I2STXSTART);
	size_t configure_count = host.i2s_configure_count;
	pmb887x_dsp_peripheral_bus_advance_idle(bus, 9433);
	g_assert_cmpuint(host.i2s_configure_count, ==, configure_count + 1);
	g_assert_true(host.i2s_transmit);
	g_assert_cmphex(pmb887x_dsp_peripheral_bus_read(bus, TEST_I2S_BASE + TEAK_I2S_RWADDR), ==, 0);
	pmb887x_dsp_peripheral_bus_advance_idle(bus, 1);
	g_assert_cmpuint(host.i2s_configure_count, ==, configure_count + 1);
	g_assert_cmphex(pmb887x_dsp_peripheral_bus_read(bus, TEST_I2S_BASE + TEAK_I2S_RWADDR), ==, 2);

	pmb887x_dsp_peripheral_bus_write(bus, TEST_I2S_BASE + TEAK_I2S_CTRL, 0);
	pmb887x_dsp_peripheral_bus_write(bus, TEST_I2S_BASE + TEAK_I2S_NUM0,
		TEAK_I2S_NUM0_FREF_CLOCK_104MHZ | 441);
	pmb887x_dsp_peripheral_bus_write(bus, TEST_I2S_BASE + TEAK_I2S_CTRL,
		TEAK_I2S_CTRL_I2SON | TEAK_I2S_CTRL_I2STXSTART);
	pmb887x_dsp_peripheral_bus_advance_idle(bus, 2358);
	g_assert_cmphex(pmb887x_dsp_peripheral_bus_read(bus, TEST_I2S_BASE + TEAK_I2S_RWADDR), ==, 0);
	pmb887x_dsp_peripheral_bus_advance_idle(bus, 1);
	g_assert_cmphex(pmb887x_dsp_peripheral_bus_read(bus, TEST_I2S_BASE + TEAK_I2S_RWADDR), ==, 2);

	pmb887x_dsp_peripheral_bus_write(bus, TEST_I2S_BASE + TEAK_I2S_CTRL, 0);
	pmb887x_dsp_peripheral_bus_write(bus, TEST_I2S_BASE + TEAK_I2S_NUM1,
		TEAK_I2S_NUM1_FREF_CLOCK_104MHZ | 441);
	pmb887x_dsp_peripheral_bus_write(bus, TEST_I2S_BASE + TEAK_I2S_DEN1, 8125);
	pmb887x_dsp_peripheral_bus_write(bus, TEST_I2S_BASE + TEAK_I2S_CSEL,
		TEAK_I2S_CSEL_TXCLKSEL);
	pmb887x_dsp_peripheral_bus_write(bus, TEST_I2S_BASE + TEAK_I2S_CTRL,
		TEAK_I2S_CTRL_I2SON | TEAK_I2S_CTRL_I2STXSTART);
	pmb887x_dsp_peripheral_bus_advance_idle(bus, 589);
	g_assert_cmphex(pmb887x_dsp_peripheral_bus_read(bus, TEST_I2S_BASE + TEAK_I2S_RWADDR), ==, 0);
	pmb887x_dsp_peripheral_bus_advance_idle(bus, 1);
	g_assert_cmphex(pmb887x_dsp_peripheral_bus_read(bus, TEST_I2S_BASE + TEAK_I2S_RWADDR), ==, 2);

	pmb887x_dsp_peripheral_bus_write(bus, TEST_I2S_BASE + TEAK_I2S_CTRL, 0);
	pmb887x_dsp_peripheral_bus_set_ms_clock_hz(bus, 26000000);
	pmb887x_dsp_peripheral_bus_write(bus, TEST_I2S_BASE + TEAK_I2S_CTRL,
		TEAK_I2S_CTRL_I2SON | TEAK_I2S_CTRL_I2STXSTART);
	pmb887x_dsp_peripheral_bus_advance_idle(bus, 2358);
	g_assert_cmphex(pmb887x_dsp_peripheral_bus_read(bus, TEST_I2S_BASE + TEAK_I2S_RWADDR), ==, 0);
	pmb887x_dsp_peripheral_bus_advance_idle(bus, 1);
	g_assert_cmphex(pmb887x_dsp_peripheral_bus_read(bus, TEST_I2S_BASE + TEAK_I2S_RWADDR), ==, 2);

	pmb887x_dsp_peripheral_bus_write(bus, TEST_I2S_BASE + TEAK_I2S_CTRL, 0);
	pmb887x_dsp_peripheral_bus_set_ms_clock_hz(bus, 0);
	pmb887x_dsp_peripheral_bus_write(bus, TEST_I2S_BASE + TEAK_I2S_CTRL,
		TEAK_I2S_CTRL_I2SON | TEAK_I2S_CTRL_I2STXSTART);
	pmb887x_dsp_peripheral_bus_advance_idle(bus, 10000);
	g_assert_cmphex(pmb887x_dsp_peripheral_bus_read(bus, TEST_I2S_BASE + TEAK_I2S_RWADDR), ==, 0);
	pmb887x_dsp_peripheral_bus_set_ms_clock_hz(bus, 104000000);

	pmb887x_dsp_peripheral_bus_reset(bus);
	pmb887x_dsp_peripheral_bus_write(bus, TEST_I2S_BASE + TEAK_I2S_NUM1,
		TEAK_I2S_NUM1_FREF_CLOCK_104MHZ | 441);
	pmb887x_dsp_peripheral_bus_write(bus, TEST_I2S_BASE + TEAK_I2S_DEN1, 8125);
	pmb887x_dsp_peripheral_bus_write(bus, TEST_I2S_BASE + TEAK_I2S_RXCONF,
		TEAK_I2S_RXCONF_PERIOD_CLOCKS_32);
	host.i2s_input[0] = 0x1234;
	host.i2s_input[1] = (int16_t) 0xFEDC;
	host.i2s_input_frame_count = 1;
	host.i2s_input_position = 0;
	pmb887x_dsp_peripheral_bus_write(bus, TEST_I2S_BASE + TEAK_I2S_RXINTADDR, 4);
	pmb887x_dsp_peripheral_bus_write(bus, TEST_I2S_BASE + TEAK_I2S_CTRL,
		TEAK_I2S_CTRL_I2SON | TEAK_I2S_CTRL_I2SRXSTART);
	pmb887x_dsp_peripheral_bus_advance_idle(bus, 1180);
	g_assert_cmpuint(host.i2s_sample_rate_hz, ==, 44100);
	g_assert_cmpuint(host.i2s_channels, ==, 2);
	g_assert_cmphex(host.data[0], ==, 0x8000);
	g_assert_cmphex(host.data[TEST_I2S_RX_RAM_BASE], ==, 0x1234);
	g_assert_cmphex(host.data[TEST_I2S_RX_RAM_BASE + 1], ==, 0xFEDC);
	g_assert_cmphex(host.data[TEST_I2S_RX_RAM_BASE + 2], ==, 0);
	g_assert_cmphex(host.data[TEST_I2S_RX_RAM_BASE + 3], ==, 0);
	g_assert_cmphex(pmb887x_dsp_peripheral_bus_read(bus, TEST_I2S_BASE + TEAK_I2S_RWADDR), ==, 0x0400);
	g_assert_cmphex(pmb887x_dsp_peripheral_bus_get_interrupt_flags(bus, 1), ==, TEAK_INT_FINTB0_I2S1RX);

	host.data[0] = 0x8000;
	host.data[1] = 0;
	pmb887x_dsp_peripheral_bus_reset(bus);
	pmb887x_dsp_peripheral_bus_write(bus, TEST_I2S_BASE + TEAK_I2S_TXCONF, TEAK_I2S_TXCONF_MONO_LEFT);
	pmb887x_dsp_peripheral_bus_write(bus, TEST_I2S_BASE + TEAK_I2S_CTRL,
		TEAK_I2S_CTRL_I2SON | TEAK_I2S_CTRL_I2STXSTART);
	pmb887x_dsp_peripheral_bus_advance_idle(bus, 512);
	g_assert_cmpuint(host.i2s_sample_rate_hz, ==, 50781);
	g_assert_cmpuint(host.i2s_channels, ==, 2);
	g_assert_cmpint(host.i2s_output[0], ==, INT16_MIN);
	g_assert_cmpint(host.i2s_output[1], ==, INT16_MIN);

	pmb887x_dsp_peripheral_bus_write(bus, TEST_I2S_BASE + TEAK_I2S_CTRL, 0);
	pmb887x_dsp_peripheral_bus_write(bus, TEST_I2S_BASE + TEAK_I2S_TXCONF, TEAK_I2S_TXCONF_MONO_STEREO);
	pmb887x_dsp_peripheral_bus_write(bus, TEST_I2S_BASE + TEAK_I2S_CTRL,
		TEAK_I2S_CTRL_I2SON | TEAK_I2S_CTRL_I2STXSTART);
	pmb887x_dsp_peripheral_bus_advance_idle(bus, 512);
	g_assert_cmpint(host.i2s_output[0], ==, INT16_MIN);
	g_assert_cmpint(host.i2s_output[1], ==, 0);
	pmb887x_dsp_peripheral_bus_destroy(bus);
}

static void test_i2s_tx_clock(void) {
	test_host_t host = {};
	pmb887x_dsp_peripheral_bus_t *bus = test_bus_create(&host);

	pmb887x_dsp_peripheral_bus_reset(bus);
	pmb887x_dsp_peripheral_bus_set_clock_hz(bus, 26000000);
	pmb887x_dsp_peripheral_bus_set_ms_clock_hz(bus, 104000000);
	pmb887x_dsp_peripheral_bus_write(bus, TEST_I2S_TX_BASE + TEAK_I2S3_NUM,
		TEAK_I2S3_NUM_FREF_CLOCK_104MHZ | 441);
	pmb887x_dsp_peripheral_bus_write(bus, TEST_I2S_TX_BASE + TEAK_I2S3_DEN, 32500);
	pmb887x_dsp_peripheral_bus_write(bus, TEST_I2S_TX_BASE + TEAK_I2S3_TXCONF,
		TEAK_I2S3_TXCONF_PERIOD_CLOCKS_32);
	pmb887x_dsp_peripheral_bus_write(bus, TEST_I2S_TX_BASE + TEAK_I2S3_CTRL,
		TEAK_I2S3_CTRL_I2SON | TEAK_I2S3_CTRL_I2STXSTART);
	pmb887x_dsp_peripheral_bus_advance_idle(bus, 2359);
	g_assert_cmphex(pmb887x_dsp_peripheral_bus_read(bus, TEST_I2S_TX_BASE + TEAK_I2S3_RADDR), ==, 2);

	pmb887x_dsp_peripheral_bus_write(bus, TEST_I2S_TX_BASE + TEAK_I2S3_CTRL, 0);
	pmb887x_dsp_peripheral_bus_set_ms_clock_hz(bus, 26000000);
	pmb887x_dsp_peripheral_bus_write(bus, TEST_I2S_TX_BASE + TEAK_I2S3_CTRL,
		TEAK_I2S3_CTRL_I2SON | TEAK_I2S3_CTRL_I2STXSTART);
	pmb887x_dsp_peripheral_bus_advance_idle(bus, 9433);
	g_assert_cmphex(pmb887x_dsp_peripheral_bus_read(bus, TEST_I2S_TX_BASE + TEAK_I2S3_RADDR), ==, 0);
	pmb887x_dsp_peripheral_bus_advance_idle(bus, 1);
	g_assert_cmphex(pmb887x_dsp_peripheral_bus_read(bus, TEST_I2S_TX_BASE + TEAK_I2S3_RADDR), ==, 2);

	pmb887x_dsp_peripheral_bus_write(bus, TEST_I2S_TX_BASE + TEAK_I2S3_CTRL, 0);
	pmb887x_dsp_peripheral_bus_set_ms_clock_hz(bus, 0);
	pmb887x_dsp_peripheral_bus_write(bus, TEST_I2S_TX_BASE + TEAK_I2S3_CTRL,
		TEAK_I2S3_CTRL_I2SON | TEAK_I2S3_CTRL_I2STXSTART);
	pmb887x_dsp_peripheral_bus_advance_idle(bus, 10000);
	g_assert_cmphex(pmb887x_dsp_peripheral_bus_read(bus, TEST_I2S_TX_BASE + TEAK_I2S3_RADDR), ==, 0);

	pmb887x_dsp_peripheral_bus_write(bus, TEST_I2S_TX_BASE + TEAK_I2S3_CTRL, 0);
	pmb887x_dsp_peripheral_bus_set_ms_clock_hz(bus, 104000000);
	pmb887x_dsp_peripheral_bus_write(bus, TEST_I2S_TX_BASE + TEAK_I2S3_CSEL,
		TEAK_I2S3_CSEL_TXCLKSEL);
	pmb887x_dsp_peripheral_bus_write(bus, TEST_I2S_TX_BASE + TEAK_I2S3_CTRL,
		TEAK_I2S3_CTRL_I2SON | TEAK_I2S3_CTRL_I2STXSTART);
	pmb887x_dsp_peripheral_bus_advance_idle(bus, 2359);
	g_assert_cmphex(pmb887x_dsp_peripheral_bus_read(bus, TEST_I2S_TX_BASE + TEAK_I2S3_RADDR), ==, 0);
	pmb887x_dsp_peripheral_bus_destroy(bus);
}

static void test_unknown(void) {
	test_host_t host = {};
	pmb887x_dsp_peripheral_bus_t *bus = test_bus_create(&host);

	pmb887x_dsp_peripheral_bus_reset(bus);
	pmb887x_dsp_peripheral_bus_write(bus, TEST_UNKNOWN_BASE + 2, 0x1234);
	g_assert_cmphex(pmb887x_dsp_peripheral_bus_read(bus, TEST_UNKNOWN_BASE + 2), ==, 0);
	pmb887x_dsp_peripheral_bus_write(bus, TEST_DSP_BASE + 1, 0x5678);
	g_assert_cmphex(pmb887x_dsp_peripheral_bus_read(bus, TEST_DSP_BASE + 1), ==, 0);
	pmb887x_dsp_peripheral_bus_destroy(bus);
}

static void test_trace(void) {
	test_host_t host = { .pc = 0x12345 };
	pmb887x_dsp_peripheral_bus_t *bus = test_bus_create(&host);

	g_assert_cmphex(PMB887X_TRACE_ALL & PMB887X_TRACE_DSP_ALL, ==, 0);
	memset(&trace_state, 0, sizeof(trace_state));
	pmb887x_trace_io_mask = PMB887X_TRACE_DSP_ALL & ~PMB887X_TRACE_DSP_MCS;
	pmb887x_dsp_peripheral_bus_write(bus, TEST_MCS_BASE + 1, 1);
	g_assert_cmpuint(trace_state.writes, ==, 0);
	pmb887x_dsp_peripheral_bus_read(bus, TEST_DSP_BASE);
	g_assert_cmpuint(trace_state.reads, ==, 1);
	g_assert_cmphex(trace_state.address, ==, TEST_DSP_BASE);
	g_assert_cmphex(trace_state.value, ==, 0xE101);
	g_assert_cmphex(trace_state.pc, ==, host.pc);
	pmb887x_dsp_peripheral_bus_write(bus, TEST_MODULATOR_BASE, 1);
	g_assert_cmpuint(trace_state.writes, ==, 1);
	g_assert_cmphex(trace_state.address, ==, TEST_MODULATOR_BASE);
	pmb887x_dsp_peripheral_bus_write(bus, TEST_UNKNOWN_BASE, 1);
	g_assert_cmpuint(trace_state.writes, ==, 2);
	g_assert_cmphex(trace_state.address, ==, TEST_UNKNOWN_BASE);
	pmb887x_dsp_peripheral_bus_write(bus, TEST_UNKNOWN_BASE, 2);
	g_assert_cmpuint(trace_state.writes, ==, 3);
	pmb887x_trace_io_mask = 0;
	pmb887x_dsp_peripheral_bus_destroy(bus);
}

int main(int argc, char **argv) {
	g_test_init(&argc, &argv, NULL);
	g_test_add_func("/pmb887x/dsp/peripheral/control", test_control);
	g_test_add_func("/pmb887x/dsp/peripheral/pads", test_pads);
	g_test_add_func("/pmb887x/dsp/peripheral/mcs", test_mcs);
	g_test_add_func("/pmb887x/dsp/peripheral/interrupt", test_interrupt);
	g_test_add_func("/pmb887x/dsp/peripheral/modulator", test_modulator);
	g_test_add_func("/pmb887x/dsp/peripheral/afe-clock", test_afe_clock);
	g_test_add_func("/pmb887x/dsp/peripheral/i2s-clock", test_i2s_clock);
	g_test_add_func("/pmb887x/dsp/peripheral/i2s-tx-clock", test_i2s_tx_clock);
	g_test_add_func("/pmb887x/dsp/peripheral/unknown", test_unknown);
	g_test_add_func("/pmb887x/dsp/peripheral/trace", test_trace);
	return g_test_run();
}

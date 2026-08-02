#include "qemu/osdep.h"

#include "hw/arm/pmb887x/dsp/peripheral.h"
#include "hw/arm/pmb887x/regs_dump.h"
#include "hw/arm/pmb887x/trace_common.h"

#define TEST_INTERRUPT_BASE	0x1000
#define TEST_MCS_BASE		0x1020
#define TEST_DSP_BASE		0x1030
#define TEST_MODULATOR_BASE	0x1040
#define TEST_AFE_BASE		0x1050
#define TEST_UNKNOWN_BASE	0x1060

uint64_t pmb887x_trace_io_mask;
uint64_t pmb887x_trace_log_mask;

typedef struct test_host_t {
	uint16_t page;
	uint32_t pc;
	bool core_disabled;
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

static pmb887x_dsp_peripheral_bus_t *test_bus_create(test_host_t *host) {
	static const pmb887x_dsp_peripheral_config_t peripherals[] = {
		{ "INT", PMB887X_DSP_PERIPHERAL_INTERRUPT, TEST_INTERRUPT_BASE, 0x16 },
		{ "MCS", PMB887X_DSP_PERIPHERAL_MCS, TEST_MCS_BASE, 0x06 },
		{ "DSP", PMB887X_DSP_PERIPHERAL_DSP, TEST_DSP_BASE, 0x09 },
		{ "MOD", PMB887X_DSP_PERIPHERAL_MODULATOR, TEST_MODULATOR_BASE, 0x0B },
		{ "AFE", PMB887X_DSP_PERIPHERAL_AFE, TEST_AFE_BASE, 0x10 },
	};
	static const pmb887x_dsp_config_t config = {
		.mmio_base = TEST_INTERRUPT_BASE,
		.mmio_size = 0x100,
		.peripherals = peripherals,
		.peripherals_count = ARRAY_SIZE(peripherals),
	};
	pmb887x_dsp_peripheral_host_t peripheral_host = {
		.opaque = host,
		.set_page = test_set_page,
		.set_core_disabled = test_set_core_disabled,
		.get_pc = test_get_pc,
	};

	return pmb887x_dsp_peripheral_bus_create(&config, &peripheral_host);
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
	g_test_add_func("/pmb887x/dsp/peripheral/unknown", test_unknown);
	g_test_add_func("/pmb887x/dsp/peripheral/trace", test_trace);
	return g_test_run();
}

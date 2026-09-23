#include "qemu/osdep.h"

#include "hw/arm/pmb887x/dsp/peripheral.h"
#include "hw/arm/pmb887x/gen/dsp.h"
#include "hw/arm/pmb887x/regs_dump.h"
#include "hw/arm/pmb887x/trace_common.h"

#define TEST_INTERRUPT_BASE	0x1000
#define TEST_MCS_BASE		0x1020
#define TEST_DSP_BASE		0x1030
#define TEST_MODULATOR_BASE	0x1040
#define TEST_AFE_BASE		0x1050
#define TEST_TIMER1_BASE	0x1060
#define TEST_TIMER2_BASE	0x1064
#define TEST_I2S_BASE		0x1070
#define TEST_I2S_TX_BASE	0x1080
#define TEST_AFE_RAM_BASE	0x2000
#define TEST_I2S_RAM_BASE	0x2100
#define TEST_I2S_TX_RAM_BASE	0x2200

#define TEST_INT_GROUP_STRIDE	(TEAK_INT_FINTB0 - TEAK_INT_FINTA0)
#define TEST_TIMER_GROUP	2
#define TEST_AUDIO_GROUP	1
#define TEST_MODULATOR_GROUP	0

uint64_t pmb887x_trace_io_mask;
uint64_t pmb887x_trace_log_mask;

void pmb887x_dump_io_read_ex(pmb887x_trace_io_t trace_io, uint32_t addr, uint32_t size, uint32_t value, uint32_t pc, uint32_t lr) {
}

void pmb887x_dump_io_write_ex(pmb887x_trace_io_t trace_io, uint32_t addr, uint32_t size, uint32_t value, uint32_t pc, uint32_t lr) {
}

static void test_set_page(void *opaque, uint16_t value) {
}

static void test_set_core_disabled(void *opaque, bool disabled) {
}

static uint32_t test_get_pc(void *opaque) {
	return 0;
}

static uint16_t test_data_read(void *opaque, uint16_t address) {
	return 0;
}

static void test_data_write(void *opaque, uint16_t address, uint16_t value) {
}

static dsp_bus_t *test_bus_create(void) {
	static const pmb887x_dsp_peripheral_config_t peripherals[] = {
		{ "INT", PMB887X_DSP_PERIPHERAL_INTERRUPT, TEST_INTERRUPT_BASE, 0x16 },
		{ "MCS", PMB887X_DSP_PERIPHERAL_MCS, TEST_MCS_BASE, 0x06 },
		{ "DSP", PMB887X_DSP_PERIPHERAL_DSP, TEST_DSP_BASE, 0x09 },
		{ "MOD", PMB887X_DSP_PERIPHERAL_MODULATOR, TEST_MODULATOR_BASE, 0x0B },
		{ "AFE", PMB887X_DSP_PERIPHERAL_AFE, TEST_AFE_BASE, 0x10, TEST_AFE_RAM_BASE, 0x80 },
		{ "TMR1", PMB887X_DSP_PERIPHERAL_TIMER1, TEST_TIMER1_BASE, 0x04 },
		{ "TMR2", PMB887X_DSP_PERIPHERAL_TIMER2, TEST_TIMER2_BASE, 0x03 },
		{ "I2S", PMB887X_DSP_PERIPHERAL_I2S, TEST_I2S_BASE, TEAK_I2S_TXINTADDR + 1, TEST_I2S_RAM_BASE, 0x40 },
		{ "I2S3", PMB887X_DSP_PERIPHERAL_I2S_TX, TEST_I2S_TX_BASE, TEAK_I2S3_TXINTADDR + 1, TEST_I2S_TX_RAM_BASE, 0x40 },
	};
	static const pmb887x_dsp_config_t config = {
		.mmio_base = TEST_INTERRUPT_BASE,
		.mmio_size = 0x100,
		.peripherals = peripherals,
		.peripheral_count = ARRAY_SIZE(peripherals),
	};
	dsp_host_t host = {
		.set_page = test_set_page,
		.set_core_disabled = test_set_core_disabled,
		.get_pc = test_get_pc,
		.data_read = test_data_read,
		.data_write = test_data_write,
	};
	dsp_bus_t *bus = dsp_bus_create(&config, &host);

	dsp_bus_reset(bus);
	return bus;
}

static uint16_t test_flags(dsp_bus_t *bus, size_t group) {
	return dsp_bus_read(bus, TEST_INTERRUPT_BASE + group * TEST_INT_GROUP_STRIDE + TEAK_INT_FINTA0);
}

/*
 * Step from event to event until @flag comes up, checking at each step that
 * it is not there a cycle early. Returns how many cycles that took.
 */
static size_t test_cycles_to_flag(dsp_bus_t *bus, size_t group, uint16_t flag) {
	size_t total = 0;

	for (;;) {
		size_t cycles = dsp_bus_next_event(bus);

		g_assert_cmpuint(cycles, !=, SIZE_MAX);
		g_assert_cmpuint(cycles, >, 0);
		dsp_bus_advance(bus, cycles - 1);
		g_assert_cmphex(test_flags(bus, group) & flag, ==, 0);
		dsp_bus_advance(bus, 1);
		total += cycles;
		if ((test_flags(bus, group) & flag) != 0)
			break;
	}

	dsp_bus_write(bus, TEST_INTERRUPT_BASE + group * TEST_INT_GROUP_STRIDE + TEAK_INT_RINTA0, flag);
	return total;
}

static void test_timer1(void) {
	dsp_bus_t *bus = test_bus_create();

	dsp_bus_write(bus, TEST_TIMER1_BASE + TEAK_TMR1_INT0, 5);
	dsp_bus_write(bus, TEST_TIMER1_BASE + TEAK_TMR1_INT1, 7);
	dsp_bus_write(bus, TEST_TIMER1_BASE + TEAK_TMR1_CTRL, TEAK_TMR1_CTRL_DT1ENA | TEAK_TMR1_CTRL_RESTART);
	g_assert_cmpuint(test_cycles_to_flag(bus, TEST_TIMER_GROUP, TEAK_INT_FINT1_TMR10), ==, 3 + 5 * 384);
	g_assert_cmpuint(test_cycles_to_flag(bus, TEST_TIMER_GROUP, TEAK_INT_FINT1_TMR11), ==, 2 * 384);
	dsp_bus_destroy(bus);
}

static void test_timer2(void) {
	dsp_bus_t *bus = test_bus_create();

	dsp_bus_set_clock(bus, true);
	dsp_bus_write(bus, TEST_TIMER2_BASE + TEAK_TMR2_MAX, 10);
	dsp_bus_write(bus, TEST_TIMER2_BASE + TEAK_TMR2_CTRL, TEAK_TMR2_CTRL_DT2ACT);
	g_assert_cmpuint(test_cycles_to_flag(bus, TEST_TIMER_GROUP, TEAK_INT_FINT1_TMR2), ==, 10 * 96);
	/* From the maximum the counter wraps to 0 on the next tick. */
	g_assert_cmpuint(test_cycles_to_flag(bus, TEST_TIMER_GROUP, TEAK_INT_FINT1_TMR2), ==, 11 * 96);
	dsp_bus_destroy(bus);
}

static void test_modulator(void) {
	dsp_bus_t *bus = test_bus_create();

	dsp_bus_write(bus, TEST_MODULATOR_BASE + TEAK_MOD_INT_ADDR, 3);
	dsp_bus_write(bus, TEST_MODULATOR_BASE + TEAK_MOD_CTRL, TEAK_MOD_CTRL_MSWACT);
	g_assert_cmpuint(test_cycles_to_flag(bus, TEST_MODULATOR_GROUP, TEAK_INT_FINTA0_MODU), ==, 3 * 16);
	g_assert_cmpuint(test_cycles_to_flag(bus, TEST_MODULATOR_GROUP, TEAK_INT_FINTA0_MODU), ==,
		(TEAK_MOD_INT_ADDR_MINT_ADDR + 1) * 16);
	dsp_bus_destroy(bus);
}

static void test_i2s_tx(void) {
	dsp_bus_t *bus = test_bus_create();

	dsp_bus_write(bus, TEST_I2S_TX_BASE + TEAK_I2S3_TXINTADDR, 4);
	dsp_bus_write(bus, TEST_I2S_TX_BASE + TEAK_I2S3_CTRL, TEAK_I2S3_CTRL_I2SON | TEAK_I2S3_CTRL_I2STXSTART);
	g_assert_cmpuint(test_cycles_to_flag(bus, TEST_AUDIO_GROUP, TEAK_INT_FINTB0_I2S3TX), ==, 4 * 16);
	g_assert_cmpuint(test_cycles_to_flag(bus, TEST_AUDIO_GROUP, TEAK_INT_FINTB0_I2S3TX), ==,
		(TEAK_I2S3_RADDR_RDADDR + 1) * 16);
	dsp_bus_destroy(bus);
}

static void test_afe(void) {
	dsp_bus_t *bus = test_bus_create();

	/* 125 cycles a sample. */
	dsp_bus_set_frequency(bus, 1000000);
	dsp_bus_write(bus, TEST_AFE_BASE + TEAK_AFE_INTPTR, 4 << TEAK_AFE_INTPTR_TXINTPTR_SHIFT);
	dsp_bus_write(bus, TEST_AFE_BASE + TEAK_AFE_BCON, TEAK_AFE_BCON_MODE | TEAK_AFE_BCON_TXSTART);
	g_assert_cmpuint(test_cycles_to_flag(bus, TEST_AUDIO_GROUP, TEAK_INT_FINTB0_VBTX), ==, 4 * 125);

	/* A new DSP clock, or a gap in it, leaves the converters where they were in the sample. */
	dsp_bus_advance(bus, 60);
	g_assert_cmpuint(dsp_bus_next_event(bus), ==, 65);
	dsp_bus_set_frequency(bus, 2000000);
	g_assert_cmpuint(dsp_bus_next_event(bus), ==, 130);
	dsp_bus_set_frequency(bus, 0);
	dsp_bus_set_frequency(bus, 2000000);
	g_assert_cmpuint(dsp_bus_next_event(bus), ==, 130);
	dsp_bus_destroy(bus);
}

static void test_i2s(void) {
	dsp_bus_t *bus = test_bus_create();

	/* After reset NUM0/DEN0 = 1/2 of the module clock, 64 clocks a frame: 256 cycles a word. */
	dsp_bus_set_frequency(bus, 1000000);
	dsp_bus_write(bus, TEST_I2S_BASE + TEAK_I2S_TXINTADDR, 2);
	dsp_bus_write(bus, TEST_I2S_BASE + TEAK_I2S_CTRL, TEAK_I2S_CTRL_I2SON | TEAK_I2S_CTRL_I2STXSTART);
	g_assert_cmpuint(test_cycles_to_flag(bus, TEST_AUDIO_GROUP, TEAK_INT_FINTB0_I2S1TX), ==, 2 * 256);
	dsp_bus_destroy(bus);
}

/* The word clock runs while the unit is on, and a started transmitter waits for the left word. */
static void test_i2s_frame_sync(void) {
	dsp_bus_t *bus = test_bus_create();

	dsp_bus_set_frequency(bus, 1000000);
	dsp_bus_write(bus, TEST_I2S_BASE + TEAK_I2S_TXINTADDR, 1);
	dsp_bus_write(bus, TEST_I2S_BASE + TEAK_I2S_CTRL, TEAK_I2S_CTRL_I2SON);
	g_assert_cmpuint(dsp_bus_next_event(bus), ==, SIZE_MAX);
	dsp_bus_advance(bus, 256 + 100);
	dsp_bus_write(bus, TEST_I2S_BASE + TEAK_I2S_CTRL, TEAK_I2S_CTRL_I2SON | TEAK_I2S_CTRL_I2STXSTART);
	g_assert_cmpuint(test_cycles_to_flag(bus, TEST_AUDIO_GROUP, TEAK_INT_FINTB0_I2S1TX), ==, 156 + 256);

	/* TXPCM stops the transmitter after a left word; a restart picks up at the next frame. */
	dsp_bus_write(bus, TEST_I2S_BASE + TEAK_I2S_CTRL, TEAK_I2S_CTRL_I2SON | TEAK_I2S_CTRL_I2STXSTART |
		TEAK_I2S_CTRL_TXPCM);
	dsp_bus_write(bus, TEST_I2S_BASE + TEAK_I2S_TXINTADDR, 3);
	g_assert_cmpuint(test_cycles_to_flag(bus, TEST_AUDIO_GROUP, TEAK_INT_FINTB0_I2S1TX), ==, 2 * 256);
	g_assert_cmphex(dsp_bus_read(bus, TEST_I2S_BASE + TEAK_I2S_CTRL) & TEAK_I2S_CTRL_I2STXSTART, ==, 0);
	dsp_bus_write(bus, TEST_I2S_BASE + TEAK_I2S_TXINTADDR, 4);
	dsp_bus_write(bus, TEST_I2S_BASE + TEAK_I2S_CTRL, TEAK_I2S_CTRL_I2SON | TEAK_I2S_CTRL_I2STXSTART |
		TEAK_I2S_CTRL_TXPCM);
	g_assert_cmpuint(test_cycles_to_flag(bus, TEST_AUDIO_GROUP, TEAK_INT_FINTB0_I2S1TX), ==, 2 * 256);
	dsp_bus_destroy(bus);
}

/* Off the fixed reference, a word lasts as long whatever the DSP clock. */
static void test_i2s_fixed_reference(void) {
	dsp_bus_t *bus = test_bus_create();

	dsp_bus_set_frequency(bus, 26000000);
	dsp_bus_write(bus, TEST_I2S_BASE + TEAK_I2S_NUM0, TEAK_I2S_NUM0_FREF_CLOCK_104MHZ | 1);
	dsp_bus_write(bus, TEST_I2S_BASE + TEAK_I2S_DEN0, 1625);
	dsp_bus_write(bus, TEST_I2S_BASE + TEAK_I2S_TXCONF, TEAK_I2S_TXCONF_PERIOD_CLOCKS_32);
	dsp_bus_write(bus, TEST_I2S_BASE + TEAK_I2S_TXINTADDR, TEAK_I2S_RWADDR_RDADDR);
	dsp_bus_write(bus, TEST_I2S_BASE + TEAK_I2S_CTRL, TEAK_I2S_CTRL_I2SON | TEAK_I2S_CTRL_I2STXSTART);
	g_assert_cmpuint(dsp_bus_next_event(bus), ==, 26000);
	dsp_bus_advance(bus, 13000);
	dsp_bus_set_frequency(bus, 52000000);
	g_assert_cmpuint(dsp_bus_next_event(bus), ==, 26000);
	dsp_bus_set_frequency(bus, 0);
	dsp_bus_set_frequency(bus, 52000000);
	g_assert_cmpuint(dsp_bus_next_event(bus), ==, 26000);
	dsp_bus_destroy(bus);
}

int main(int argc, char **argv) {
	g_test_init(&argc, &argv, NULL);
	g_test_add_func("/pmb887x/dsp/timing/timer1", test_timer1);
	g_test_add_func("/pmb887x/dsp/timing/timer2", test_timer2);
	g_test_add_func("/pmb887x/dsp/timing/modulator", test_modulator);
	g_test_add_func("/pmb887x/dsp/timing/i2s-tx", test_i2s_tx);
	g_test_add_func("/pmb887x/dsp/timing/afe", test_afe);
	g_test_add_func("/pmb887x/dsp/timing/i2s", test_i2s);
	g_test_add_func("/pmb887x/dsp/timing/i2s-frame-sync", test_i2s_frame_sync);
	g_test_add_func("/pmb887x/dsp/timing/i2s-fixed-reference", test_i2s_fixed_reference);
	return g_test_run();
}

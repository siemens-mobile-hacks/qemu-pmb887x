#include "qemu/osdep.h"
#include "qemu/error-report.h"

#include "hw/arm/pmb887x/boards.h"
#include "hw/arm/pmb887x/regs.h"
#include "hw/arm/pmb887x/brom_data.h"

const uint8_t *pmb887x_get_brom_image(uint32_t cpu, size_t *size) {
	switch (cpu) {
		case CPU_PMB8875:
			*size = sizeof(pmb8875_brom);
			return pmb8875_brom;
		case CPU_PMB8876:
			*size = sizeof(pmb8876_brom);
			return pmb8876_brom;
		default:
			error_report("Unknown CPU!");
			exit(1);
	}
}

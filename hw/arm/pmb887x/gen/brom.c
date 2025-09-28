#include "hw/arm/pmb887x/gen/brom.h"

#include "qemu/osdep.h"
#include "qemu/error-report.h"
#include "hw/hw.h"

#include "hw/arm/pmb887x/gen/cpu_regs.h"
#include "hw/arm/pmb887x/gen/brom_data.h"

const uint8_t *pmb887x_get_brom_image(uint32_t cpu_id, size_t *size) {
	switch (cpu_id) {
		case CPU_PMB8875:
			*size = sizeof(pmb8875_brom);
			return pmb8875_brom;
		case CPU_PMB8876:
			*size = sizeof(pmb8876_brom);
			return pmb8876_brom;
		default:
			hw_error("Invalid CPU type: %d", cpu_id);
	}
	return NULL;
}

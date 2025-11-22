#include "hw/arm/pmb887x/gen/brom.h"

#include "qemu/osdep.h"
#include "qemu/error-report.h"
#include "hw/hw.h"

#include "hw/arm/pmb887x/gen/cpu_regs.h"
#include "hw/arm/pmb887x/gen/brom_data.h"

const uint8_t *pmb887x_get_brom_image(uint32_t cpu_id, size_t *size, uint32_t rev) {
	switch (cpu_id) {
		case CPU_PMB8875:
			*size = sizeof(pmb8875_brom);
			return pmb8875_brom;
		case CPU_PMB8876:
			if (rev == 0) {
				*size = sizeof(pmb8876_brom_r0);
				return pmb8876_brom_r0;
			} else if (rev >= 17) {
				*size = sizeof(pmb8876_brom_r17);
				return pmb8876_brom_r17;
			} else {
				*size = sizeof(pmb8876_brom_r16);
				return pmb8876_brom_r16;
			}
		default:
			hw_error("Invalid CPU type: %d", cpu_id);
	}
	return NULL;
}

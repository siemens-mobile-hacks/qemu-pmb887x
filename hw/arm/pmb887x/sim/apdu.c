#include "qemu/osdep.h"
#include "qemu/module.h"

#include "hw/arm/pmb887x/sim/apdu.h"

void pmb887x_apdu_backend_card_reset(pmb887x_apdu_backend_t *backend) {
	pmb887x_apdu_backend_class_t *klass = PMB887X_APDU_BACKEND_GET_CLASS(backend);
	if (klass->card_reset)
		klass->card_reset(backend);
}

const uint8_t *pmb887x_apdu_backend_get_atr(pmb887x_apdu_backend_t *backend, size_t *size) {
	pmb887x_apdu_backend_class_t *klass = PMB887X_APDU_BACKEND_GET_CLASS(backend);
	if (!klass->get_atr) {
		*size = 0;
		return NULL;
	}
	return klass->get_atr(backend, size);
}

void pmb887x_apdu_backend_exchange(
	pmb887x_apdu_backend_t *backend,
	const uint8_t *command,
	size_t command_size,
	pmb887x_apdu_complete_fn complete,
	void *opaque
) {
	pmb887x_apdu_backend_class_t *klass = PMB887X_APDU_BACKEND_GET_CLASS(backend);
	if (klass->exchange) {
		klass->exchange(backend, command, command_size, complete, opaque);
	} else {
		pmb887x_apdu_response_t response = { 0 };
		pmb887x_apdu_response_set_status(&response, PMB887X_APDU_STATUS_TECHNICAL_PROBLEM);
		complete(opaque, &response);
	}
}

static const TypeInfo pmb887x_apdu_backend_info = {
	.name = TYPE_PMB887X_APDU_BACKEND,
	.parent = TYPE_DEVICE,
	.instance_size = sizeof(pmb887x_apdu_backend_t),
	.class_size = sizeof(pmb887x_apdu_backend_class_t),
	.abstract = true,
};

static void pmb887x_apdu_backend_register_types(void) {
	type_register_static(&pmb887x_apdu_backend_info);
}
type_init(pmb887x_apdu_backend_register_types)

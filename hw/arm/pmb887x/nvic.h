#pragma once
#include "qemu/osdep.h"

enum {
    PMB887X_NVIC_ACTION_FIRE = 0,
    PMB887X_NVIC_ACTION_ACK,
};

struct pmb887x_nvic_t;
typedef struct pmb887x_nvic_t pmb887x_nvic_t;
typedef void (*pmb887x_nvic_callback_t)(void *user_data, int action, int irq);

pmb887x_nvic_t *pmb887x_nvic_get_self(DeviceState *dev);
void pmb887x_nvic_set_callback(pmb887x_nvic_t *p, int irq, pmb887x_nvic_callback_t callback, void *user_data);
int pmb887x_nvic_get_irq_id(pmb887x_nvic_t *p, qemu_irq irq);

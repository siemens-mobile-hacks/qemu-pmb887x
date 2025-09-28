#pragma once

#include "qemu/osdep.h"

struct pmb887x_pcl_t;
typedef struct pmb887x_pcl_t pmb887x_pcl_t;

void pmb887x_pcl_init_exti(pmb887x_pcl_t *p, qemu_irq *irqs, size_t irqs_n);

uint32_t pmb887x_pcl_exti_read(pmb887x_pcl_t *p);
void pmb887x_pcl_exti_write(pmb887x_pcl_t *p, uint32_t value);

uint32_t pmb887x_pcl_exti_src_read(pmb887x_pcl_t *p, uint32_t index);
void pmb887x_pcl_exti_src_write(pmb887x_pcl_t *p, uint32_t index, uint32_t value);

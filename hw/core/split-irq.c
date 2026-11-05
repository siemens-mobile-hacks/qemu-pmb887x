/*
 * IRQ splitter device.
 *
 * Copyright (c) 2018 Linaro Limited.
 * Written by Peter Maydell
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
 * THE SOFTWARE.
 */

#include "qemu/osdep.h"
#include "hw/core/split-irq.h"
#include "hw/core/irq.h"
#include "hw/core/qdev-properties.h"
#include "qapi/error.h"
#include "qemu/module.h"

static void split_irq_handler(void *opaque, int n, int level)
{
    SplitIRQ *s = SPLIT_IRQ(opaque);
    int i;

    for (i = 0; i < s->num_lines; i++) {
        qemu_set_irq(s->out_irq[i], level);
    }
}

static void split_irq_connect_line(DeviceState *dev, const char *name, int n,
                                  qemu_irq input)
{
    qdev_connect_gpio_out_named(dev, name, n, input);
    assert(qdev_get_gpio_out_connector(dev, name, n) == input);
}

static void split_irq_add_output(SplitIRQ *s, qemu_irq input)
{
    assert(s->num_lines < MAX_SPLIT_LINES);
    qdev_init_gpio_out(DEVICE(s), &s->out_irq[s->num_lines], 1);
    split_irq_connect_line(DEVICE(s), NULL, s->num_lines, input);
    s->num_lines++;
}

void split_irq_connect_gpio_out(DeviceState *dev, const char *name, int n,
                                qemu_irq input)
{
    g_autofree char *split_name =
        g_strdup_printf("%s-split[%d]", name ? name : "unnamed-gpio-out", n);
    Object *split = object_resolve_path_component(OBJECT(dev), split_name);

    if (split) {
        split_irq_add_output(SPLIT_IRQ(split), input);
        return;
    }

    qemu_irq connected = qdev_get_gpio_out_connector(dev, name, n);
    if (!connected) {
        split_irq_connect_line(dev, name, n, input);
        return;
    }

    DeviceState *split_dev = qdev_new(TYPE_SPLIT_IRQ);
    object_property_add_child(OBJECT(dev), split_name, OBJECT(split_dev));
    qdev_realize_and_unref(split_dev, NULL, &error_abort);
    split_irq_connect_line(split_dev, NULL, 0, connected);
    split_irq_add_output(SPLIT_IRQ(split_dev), input);
    split_irq_connect_line(dev, name, n, qdev_get_gpio_in(split_dev, 0));
}

static void split_irq_init(Object *obj)
{
    qdev_init_gpio_in(DEVICE(obj), split_irq_handler, 1);
}

static void split_irq_realize(DeviceState *dev, Error **errp)
{
    SplitIRQ *s = SPLIT_IRQ(dev);

    if (s->num_lines < 1 || s->num_lines >= MAX_SPLIT_LINES) {
        error_setg(errp,
                   "IRQ splitter number of lines %d is not between 1 and %d",
                   s->num_lines, MAX_SPLIT_LINES);
        return;
    }

    qdev_init_gpio_out(dev, s->out_irq, s->num_lines);
}

static const Property split_irq_properties[] = {
    DEFINE_PROP_UINT16("num-lines", SplitIRQ, num_lines, 1),
};

static void split_irq_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);

    /* No state to reset or migrate */
    device_class_set_props(dc, split_irq_properties);
    dc->realize = split_irq_realize;

    /* Reason: Needs to be wired up to work */
    dc->user_creatable = false;
}

static const TypeInfo split_irq_type_info = {
   .name = TYPE_SPLIT_IRQ,
   .parent = TYPE_DEVICE,
   .instance_size = sizeof(SplitIRQ),
   .instance_init = split_irq_init,
   .class_init = split_irq_class_init,
};

static void split_irq_register_types(void)
{
    type_register_static(&split_irq_type_info);
}

type_init(split_irq_register_types)

#include "qemu/osdep.h"
#include "hw/core/irq.h"
#include "hw/core/qdev-properties.h"
#include "hw/core/split-irq.h"
#include "qom/object.h"
#include "qapi/error.h"
#include "qapi/visitor.h"


#define TYPE_MY_DEV "my-dev"
typedef struct MyDev MyDev;
DECLARE_INSTANCE_CHECKER(MyDev, STATIC_TYPE,
                         TYPE_MY_DEV)

struct MyDev {
    DeviceState parent_obj;

    uint32_t prop_u32;
    char *prop_string;
    uint32_t *prop_array_u32;
    uint32_t prop_array_u32_nb;
    qemu_irq sda_out;
    int sda_in_level;
    int sda_in_order;
};

static const Property my_dev_props[] = {
    DEFINE_PROP_UINT32("u32", MyDev, prop_u32, 100),
    DEFINE_PROP_STRING("string", MyDev, prop_string),
    DEFINE_PROP_ARRAY("array-u32", MyDev, prop_array_u32_nb, prop_array_u32,
                     qdev_prop_uint32, uint32_t),
};

static int sda_in_sequence;

static void my_dev_sda_in(void *opaque, int _n, int level)
{
    MyDev *mt = opaque;

    mt->sda_in_level = level;
    mt->sda_in_order = ++sda_in_sequence;
}

static void my_dev_init(Object *obj)
{
    MyDev *mt = STATIC_TYPE(obj);

    qdev_init_gpio_in_named(DEVICE(obj), my_dev_sda_in, "sda", 1);
    qdev_init_gpio_out_named(DEVICE(obj), &mt->sda_out, "sda", 1);
}

static void my_dev_class_init(ObjectClass *oc, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(oc);

    dc->realize = NULL;
    device_class_set_props(dc, my_dev_props);
}

static const TypeInfo my_dev_type_info = {
    .name = TYPE_MY_DEV,
    .parent = TYPE_DEVICE,
    .instance_size = sizeof(MyDev),
    .instance_init = my_dev_init,
    .class_init = my_dev_class_init,
};

#define TYPE_MY_CONTAINER "my-container"

static const TypeInfo my_container_type_info = {
    .name = TYPE_MY_CONTAINER,
    .parent = TYPE_DEVICE,
    .instance_size = sizeof(DeviceState),
};

/*
 * Initialize a fake machine, being prepared for future tests.
 *
 * Realization of anonymous qdev (with no parent object) requires both
 * the machine object and its "unattached" container to be at least present.
 */
static void test_init_machine(void)
{
    /* This is a fake machine - it doesn't need to be a machine object */
    Object *machine = object_property_add_new_container(
        object_get_root(), "machine");

    /* This container must exist for anonymous qdevs to realize() */
    object_property_add_new_container(machine, "unattached");
}

static void test_qdev_free_properties(void)
{
    MyDev *mt;

    mt = STATIC_TYPE(object_new(TYPE_MY_DEV));
    object_set_props(OBJECT(mt), &error_fatal,
                     "string", "something",
                     "array-u32", "12,13",
                     NULL);
    qdev_realize(DEVICE(mt), NULL, &error_fatal);

    g_assert_cmpuint(mt->prop_u32, ==, 100);
    g_assert_cmpstr(mt->prop_string, ==, "something");
    g_assert_cmpuint(mt->prop_array_u32_nb, ==, 2);
    g_assert_cmpuint(mt->prop_array_u32[0], ==, 12);
    g_assert_cmpuint(mt->prop_array_u32[1], ==, 13);

    object_unparent(OBJECT(mt));
    object_unref(mt);
}

static MyDev *test_new_dev(void)
{
    MyDev *mt = STATIC_TYPE(object_new(TYPE_MY_DEV));

    qdev_realize(DEVICE(mt), NULL, &error_fatal);
    return mt;
}

static void test_free_dev(MyDev *mt)
{
    object_unparent(OBJECT(mt));
    object_unref(mt);
}

static void test_qdev_gpio_shared_name(void)
{
    MyDev *a = test_new_dev();
    MyDev *b = test_new_dev();
    MyDev *c = test_new_dev();
    qemu_irq b_sda_in = qdev_get_gpio_in_named(DEVICE(b), "sda", 0);
    qemu_irq c_sda_in = qdev_get_gpio_in_named(DEVICE(c), "sda", 0);

    g_assert_true(qdev_has_gpio_in_named(DEVICE(a), "sda", 0));
    g_assert_true(qdev_has_gpio_out_named(DEVICE(a), "sda", 0));
    g_assert_false(qdev_has_gpio_out_named(DEVICE(a), "sda", 1));

    qdev_connect_gpio_out_named(DEVICE(a), "sda", 0, b_sda_in);
    g_assert_true(qdev_get_gpio_out_connector(DEVICE(a), "sda", 0) ==
                  b_sda_in);
    qemu_set_irq(a->sda_out, 1);
    g_assert_cmpint(b->sda_in_level, ==, 1);

    g_assert_true(qdev_intercept_gpio_out(DEVICE(a), c_sda_in, "sda", 0) ==
                  b_sda_in);
    b->sda_in_level = -1;
    qemu_set_irq(a->sda_out, 1);
    g_assert_cmpint(c->sda_in_level, ==, 1);
    g_assert_cmpint(b->sda_in_level, ==, -1);

    test_free_dev(a);
    test_free_dev(b);
    test_free_dev(c);
}

static void test_qdev_gpio_pass_shared_name(void)
{
    DeviceState *container = qdev_new(TYPE_MY_CONTAINER);
    MyDev *mt = test_new_dev();
    MyDev *peer = test_new_dev();

    qdev_pass_gpios(DEVICE(mt), container, "sda");
    qdev_connect_gpio_out_named(container, "sda", 0,
                                qdev_get_gpio_in_named(DEVICE(peer), "sda", 0));
    qemu_set_irq(mt->sda_out, 1);
    g_assert_cmpint(peer->sda_in_level, ==, 1);

    qemu_set_irq(qdev_get_gpio_in_named(container, "sda", 0), 1);
    g_assert_cmpint(mt->sda_in_level, ==, 1);

    object_unref(container);
    test_free_dev(mt);
    test_free_dev(peer);
}

static void test_split_irq_connect_gpio_out(void)
{
    MyDev *mt = test_new_dev();
    MyDev *peers[] = { test_new_dev(), test_new_dev(), test_new_dev() };

    for (size_t i = 0; i < ARRAY_SIZE(peers); i++) {
        split_irq_connect_gpio_out(DEVICE(mt), "sda", 0,
                                   qdev_get_gpio_in_named(DEVICE(peers[i]),
                                                          "sda", 0));
    }
    sda_in_sequence = 0;
    qemu_set_irq(mt->sda_out, 1);

    for (size_t i = 0; i < ARRAY_SIZE(peers); i++) {
        g_assert_cmpint(peers[i]->sda_in_level, ==, 1);
        g_assert_cmpint(peers[i]->sda_in_order, ==, i + 1);
    }

    test_free_dev(mt);
    for (size_t i = 0; i < ARRAY_SIZE(peers); i++) {
        test_free_dev(peers[i]);
    }
}

int main(int argc, char **argv)
{
    g_test_init(&argc, &argv, NULL);

    module_call_init(MODULE_INIT_QOM);
    type_register_static(&my_dev_type_info);
    type_register_static(&my_container_type_info);
    test_init_machine();

    g_test_add_func("/qdev/free-properties",
                    test_qdev_free_properties);
    g_test_add_func("/qdev/gpio-shared-name", test_qdev_gpio_shared_name);
    g_test_add_func("/qdev/gpio-pass-shared-name",
                    test_qdev_gpio_pass_shared_name);
    g_test_add_func("/qdev/split-irq-connect-gpio-out",
                    test_split_irq_connect_gpio_out);

    g_test_run();

    return 0;
}

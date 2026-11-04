/* SPDX-License-Identifier: GPL-2.0-or-later */

#include "qemu/osdep.h"
#include "qapi/error.h"
#include "qapi/util.h"
#include "qemu/debug-channel.h"

typedef struct DebugSpecItem {
    uint8_t flag;
    bool enable;
    bool used;
    char *name;
} DebugSpecItem;

static GPtrArray *debug_infos;
static GPtrArray *debug_spec;
static bool debug_has_enabled;

void debug_channel_info_register(DebugChannelInfo *info)
{
    if (!debug_infos) {
        debug_infos = g_ptr_array_new();
    }
    g_ptr_array_add(debug_infos, info);
}

static bool debug_is_name_in_list(const char *name, const char *const *list)
{
    for (; list && *list; list++) {
        if (!g_ascii_strcasecmp(name, *list)) {
            return true;
        }
    }
    return false;
}

static bool debug_info_has_name(const DebugChannelInfo *info, const char *name)
{
    if (!g_ascii_strcasecmp(name, "all")) {
        return !info->not_in_all;
    }
    return !g_ascii_strcasecmp(name, info->name) ||
           debug_is_name_in_list(name, info->aliases) ||
           debug_is_name_in_list(name, info->groups);
}

static void debug_spec_item_free(gpointer data)
{
    DebugSpecItem *item = data;

    g_free(item->name);
    g_free(item);
}

static GPtrArray *debug_spec_split(const char *spec, uint8_t flag,
                                   Error **errp)
{
    g_autoptr(GPtrArray) items =
        g_ptr_array_new_with_free_func(debug_spec_item_free);
    g_auto(GStrv) tokens = g_strsplit(spec, ",", -1);

    for (char **token = tokens; *token; token++) {
        g_autoptr(GString) name = g_string_new(NULL);
        bool enable = true;

        for (const char *c = *token; *c; c++) {
            if (!g_ascii_isspace(*c)) {
                g_string_append_c(name, *c);
            }
        }
        if (!name->len) {
            continue;
        }
        if (name->str[0] == '+' || name->str[0] == '-') {
            enable = name->str[0] == '+';
            g_string_erase(name, 0, 1);
        }
        if (!name->len) {
            error_setg(errp, "Invalid debug channel name: %s", *token);
            return NULL;
        }

        DebugSpecItem *item = g_new0(DebugSpecItem, 1);
        item->flag = flag;
        item->enable = enable;
        item->name = g_string_free(g_steal_pointer(&name), false);
        g_ptr_array_add(items, item);
    }
    return g_steal_pointer(&items);
}

bool debug_channels_parse(const char *spec, uint8_t flag, Error **errp)
{
    g_autoptr(GPtrArray) items = debug_spec_split(spec, flag, errp);

    if (!items) {
        return false;
    }
    if (!debug_spec) {
        debug_spec = g_ptr_array_new_with_free_func(debug_spec_item_free);
    }
    g_ptr_array_extend_and_steal(debug_spec, g_steal_pointer(&items));
    return true;
}

static bool debug_option_token(const char *token, uint8_t *flag,
                               Error **errp)
{
    const char *eq = strchr(token, '=');

    if (!eq) {
        if (*flag) {
            return debug_channels_parse(token, *flag, errp);
        }
        for (const char *c = token; *c; c++) {
            if (!g_ascii_isspace(*c)) {
                error_setg(errp, "Expected log=, io= or time= before '%s'",
                           token);
                return false;
            }
        }
        return true;
    }

    g_autofree char *key = g_strstrip(g_strndup(token, eq - token));

    if (!g_ascii_strcasecmp(key, "time")) {
        g_autofree char *on_off = g_strstrip(g_strdup(eq + 1));
        bool on;

        *flag = 0;
        if (!qapi_bool_parse("time", on_off, &on, errp)) {
            return false;
        }
        debug_channels_set_time(on);
        return true;
    }
    if (!g_ascii_strcasecmp(key, "log")) {
        *flag = DEBUG_CH_LOG;
    } else if (!g_ascii_strcasecmp(key, "io")) {
        *flag = DEBUG_CH_IO;
    } else {
        error_setg(errp, "Unknown debug channels key '%s'", key);
        return false;
    }
    return debug_channels_parse(eq + 1, *flag, errp);
}

bool debug_channels_parse_option(const char *str, Error **errp)
{
    g_auto(GStrv) tokens = g_strsplit(str, ",", -1);
    uint8_t flag = 0;

    for (char **token = tokens; *token; token++) {
        if (!debug_option_token(*token, &flag, errp)) {
            return false;
        }
    }
    return true;
}

static bool debug_spec_item_is_known(const DebugSpecItem *item)
{
    if (item->used || !g_ascii_strcasecmp(item->name, "all")) {
        return true;
    }
    for (guint i = 0; debug_infos && i < debug_infos->len; i++) {
        if (debug_info_has_name(g_ptr_array_index(debug_infos, i),
                                item->name)) {
            return true;
        }
    }
    return false;
}

bool debug_channels_check_unused(Error **errp)
{
    for (guint i = 0; debug_spec && i < debug_spec->len; i++) {
        DebugSpecItem *item = g_ptr_array_index(debug_spec, i);

        if (!debug_spec_item_is_known(item)) {
            error_setg(errp, "Unknown debug channel name: %s", item->name);
            return false;
        }
    }
    return true;
}

void debug_channels_reset(void)
{
    g_clear_pointer(&debug_spec, g_ptr_array_unref);
    debug_has_enabled = false;
}

bool debug_channels_has_enabled(void)
{
    return debug_has_enabled;
}

bool debug_channels_is_requested(void)
{
    for (guint i = 0; debug_spec && i < debug_spec->len; i++) {
        DebugSpecItem *item = g_ptr_array_index(debug_spec, i);

        if (item->enable) {
            return true;
        }
    }
    return false;
}

void debug_channel_init(DebugChannel *ch, const DebugChannelInfo *info,
                        const char *instance)
{
    *ch = (DebugChannel) {
        .name = g_strdup(instance ?: info->name),
    };

    for (guint i = 0; debug_spec && i < debug_spec->len; i++) {
        DebugSpecItem *item = g_ptr_array_index(debug_spec, i);
        bool is_instance = instance &&
                           !g_ascii_strcasecmp(item->name, instance);

        if (!is_instance && !debug_info_has_name(info, item->name)) {
            continue;
        }
        item->used = true;
        if (item->enable) {
            ch->flags |= item->flag;
        } else {
            ch->flags &= ~item->flag;
        }
    }
    debug_has_enabled |= ch->flags != 0;
}

void debug_channel_cleanup(DebugChannel *ch)
{
    /* Queued records, guest errors included, still point at the name */
    debug_channels_flush();
    g_clear_pointer(&ch->name, g_free);
    ch->flags = 0;
}

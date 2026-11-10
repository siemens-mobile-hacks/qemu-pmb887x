/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "qemu/osdep.h"

#include "hw/audio/audio-router.h"

typedef struct AudioRouterSource {
    unsigned input;
    unsigned stream;
    AudioLineFormat format;
    int gain_mb;
    bool *active;
} AudioRouterSource;

static void audio_router_free_source(gpointer data)
{
    AudioRouterSource *source = data;

    g_free(source->active);
    g_free(source);
}

void audio_router_init(AudioRouter *router, AudioLineBus **outputs,
                       size_t output_count, size_t input_count,
                       const DebugChannel *dbg)
{
    *router = (AudioRouter) {
        .outputs = outputs,
        .output_count = output_count,
        .input_count = input_count,
        .routes = g_new0(AudioRouterRoute, output_count * input_count),
        .sources = g_ptr_array_new_with_free_func(audio_router_free_source),
        .dbg = dbg,
    };
}

void audio_router_cleanup(AudioRouter *router)
{
    g_ptr_array_unref(router->sources);
    g_free(router->routes);
}

static AudioRouterSource *audio_router_get_source(AudioRouter *router,
                                                  unsigned index)
{
    return g_ptr_array_index(router->sources, index);
}

static int audio_router_find_source(AudioRouter *router, unsigned input,
                                    unsigned stream)
{
    for (unsigned i = 0; i < router->sources->len; i++) {
        AudioRouterSource *source = audio_router_get_source(router, i);

        if (source->input == input && source->stream == stream) {
            return i;
        }
    }
    return -1;
}

static unsigned audio_router_add_source(AudioRouter *router, unsigned input,
                                        unsigned stream)
{
    AudioRouterSource *source = g_new0(AudioRouterSource, 1);

    assert(input < router->input_count);
    source->input = input;
    source->stream = stream;
    source->active = g_new0(bool, router->output_count);
    g_ptr_array_add(router->sources, source);
    return router->sources->len - 1;
}

static AudioRouterSource *audio_router_find_or_add_source(AudioRouter *router,
                                                          unsigned input,
                                                          unsigned stream)
{
    int index = audio_router_find_source(router, input, stream);

    if (index < 0) {
        index = audio_router_add_source(router, input, stream);
    }
    return audio_router_get_source(router, index);
}

static AudioRouterRoute *audio_router_get_route(AudioRouter *router,
                                                unsigned output,
                                                unsigned input)
{
    return &router->routes[output * router->input_count + input];
}

void audio_router_set_route(AudioRouter *router, unsigned output,
                            unsigned input, bool enabled, int gain_mb)
{
    assert(output < router->output_count);
    assert(input < router->input_count);
    *audio_router_get_route(router, output, input) = (AudioRouterRoute) {
        .enabled = enabled,
        .gain_mb = gain_mb,
    };
}

static void audio_router_update_source(AudioRouter *router, unsigned index)
{
    AudioRouterSource *source = audio_router_get_source(router, index);

    for (unsigned output = 0; output < router->output_count; output++) {
        AudioLineBus *bus = router->outputs[output];
        AudioRouterRoute *route = audio_router_get_route(router, output,
                                                         source->input);
        bool routed = route->enabled && source->format.channels != 0;

        if (routed) {
            audio_line_set_gain(bus, index, source->gain_mb + route->gain_mb);
            audio_line_start(bus, index, &source->format);
        } else if (source->active[output]) {
            audio_line_stop(bus, index);
        }
        if (routed != source->active[output]) {
            DEBUG_LOG(router->dbg, "audio input %u.%u -> output %u: %s\n",
                      source->input, source->stream, output,
                      routed ? "on" : "off");
        }
        source->active[output] = routed;
    }
}

void audio_router_update(AudioRouter *router)
{
    for (unsigned i = 0; i < router->sources->len; i++) {
        audio_router_update_source(router, i);
    }
}

void audio_router_reset(AudioRouter *router)
{
    memset(router->routes, 0,
           router->output_count * router->input_count * sizeof(*router->routes));
    for (unsigned i = 0; i < router->sources->len; i++) {
        AudioRouterSource *source = audio_router_get_source(router, i);

        source->format = (AudioLineFormat) { };
        source->gain_mb = 0;
    }
    audio_router_update(router);
}

void audio_router_start(AudioRouter *router, unsigned input, unsigned stream,
                        const AudioLineFormat *format)
{
    audio_router_find_or_add_source(router, input, stream)->format = *format;
    audio_router_update(router);
}

void audio_router_stop(AudioRouter *router, unsigned input, unsigned stream)
{
    int index = audio_router_find_source(router, input, stream);

    if (index < 0) {
        return;
    }
    audio_router_get_source(router, index)->format = (AudioLineFormat) { };
    audio_router_update(router);
}

void audio_router_set_gain(AudioRouter *router, unsigned input,
                           unsigned stream, int gain_mb)
{
    audio_router_find_or_add_source(router, input, stream)->gain_mb = gain_mb;
    audio_router_update(router);
}

void audio_router_write(AudioRouter *router, unsigned input, unsigned stream,
                        const int16_t *frames, size_t frame_count)
{
    int index = audio_router_find_source(router, input, stream);

    if (index < 0) {
        return;
    }

    AudioRouterSource *source = audio_router_get_source(router, index);

    for (unsigned output = 0; output < router->output_count; output++) {
        if (source->active[output]) {
            audio_line_write(router->outputs[output], index, frames,
                             frame_count);
        }
    }
}

static AudioRouterInput *audio_router_port_get_input(AudioLineSinkPort *port)
{
    return container_of(port, AudioRouterInput, port);
}

static void audio_router_line_start(AudioLineSinkPort *port, unsigned stream,
                                    const AudioLineFormat *format)
{
    AudioRouterInput *input = audio_router_port_get_input(port);

    audio_router_start(input->router, input->input, stream, format);
}

static void audio_router_line_stop(AudioLineSinkPort *port, unsigned stream)
{
    AudioRouterInput *input = audio_router_port_get_input(port);

    audio_router_stop(input->router, input->input, stream);
}

static void audio_router_line_set_gain(AudioLineSinkPort *port,
                                       unsigned stream, int gain_mb)
{
    AudioRouterInput *input = audio_router_port_get_input(port);

    audio_router_set_gain(input->router, input->input, stream, gain_mb);
}

static void audio_router_line_write(AudioLineSinkPort *port, unsigned stream,
                                    const int16_t *frames, size_t frame_count)
{
    AudioRouterInput *input = audio_router_port_get_input(port);

    audio_router_write(input->router, input->input, stream, frames,
                       frame_count);
}

void audio_router_input_init(AudioRouterInput *input, DeviceState *dev,
                             const char *name, AudioRouter *router,
                             unsigned index)
{
    input->router = router;
    input->input = index;
    audio_line_sink_port_init(&input->port, dev, name, AUDIO_LINE_SINK(dev));
}

void audio_router_sink_class_init(AudioLineSinkClass *klass)
{
    klass->start = audio_router_line_start;
    klass->stop = audio_router_line_stop;
    klass->set_gain = audio_router_line_set_gain;
    klass->write = audio_router_line_write;
}

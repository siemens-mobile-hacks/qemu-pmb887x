/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef HW_AUDIO_AUDIO_ROUTER_H
#define HW_AUDIO_AUDIO_ROUTER_H

#include "hw/audio/audio-line.h"
#include "qemu/debug-channel.h"

typedef struct AudioRouter AudioRouter;
typedef struct AudioRouterInput AudioRouterInput;
typedef struct AudioRouterRoute AudioRouterRoute;

struct AudioRouterRoute {
    bool enabled;
    int gain_mb;
};

struct AudioRouter {
    AudioLineBus **outputs;
    size_t output_count;
    size_t input_count;
    AudioRouterRoute *routes;
    GPtrArray *sources;
    const DebugChannel *dbg;
};

struct AudioRouterInput {
    AudioLineSinkPort port;
    AudioRouter *router;
    unsigned input;
};

void audio_router_init(AudioRouter *router, AudioLineBus **outputs,
                       size_t output_count, size_t input_count,
                       const DebugChannel *dbg);
void audio_router_cleanup(AudioRouter *router);
void audio_router_reset(AudioRouter *router);
void audio_router_set_route(AudioRouter *router, unsigned output,
                            unsigned input, bool enabled, int gain_mb);
void audio_router_update(AudioRouter *router);
void audio_router_start(AudioRouter *router, unsigned input, unsigned stream,
                        const AudioLineFormat *format);
void audio_router_stop(AudioRouter *router, unsigned input, unsigned stream);
void audio_router_set_gain(AudioRouter *router, unsigned input,
                           unsigned stream, int gain_mb);
void audio_router_write(AudioRouter *router, unsigned input, unsigned stream,
                        const int16_t *frames, size_t frame_count);

void audio_router_input_init(AudioRouterInput *input, DeviceState *dev,
                             const char *name, AudioRouter *router,
                             unsigned index);
void audio_router_sink_class_init(AudioLineSinkClass *klass);

#endif

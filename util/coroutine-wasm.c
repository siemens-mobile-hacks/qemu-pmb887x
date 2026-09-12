/*
 * emscripten fiber coroutine initialization code
 * based on coroutine-ucontext.c
 *
 * Copyright (C) 2006  Anthony Liguori <anthony@codemonkey.ws>
 * Copyright (C) 2011  Kevin Wolf <kwolf@redhat.com>
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.0 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, see <http://www.gnu.org/licenses/>.
 */

#include "qemu/osdep.h"
#include "qemu/coroutine_int.h"
#include "qemu/coroutine-tls.h"

#include <emscripten/fiber.h>
#include <emscripten/em_js.h>

static __thread bool co_forbidden_thread;

void qemu_coroutine_forbid_current_thread(void)
{
    co_forbidden_thread = true;
}

/*
 * QEMU_COSTACK=1: log every distinct JS/wasm call stack at a coroutine
 * switch (Asyncify must instrument exactly these frames; the set feeds
 * -sASYNCIFY_ONLY).  Frames print as wasm-function[N], resolved with the
 * .symbols sidecar.
 */
EM_JS(void, wasm_costack_trace, (void), {
    var lim = Error.stackTraceLimit;
    Error.stackTraceLimit = 400;
    var s = new Error().stack;
    Error.stackTraceLimit = lim;
    var key = s.replace(/:0x[0-9a-f]+[\x29]?/g, "").replace(/https?:[\x2f][\x2f][^ \x29]*[\x2f]/g, "");
    if (!globalThis.__costacks) globalThis.__costacks = new Set();
    if (!globalThis.__costacks.has(key)) {
        globalThis.__costacks.add(key);
        console.log("[qemu] COSTACK " + key.split("\n").slice(1).map(function (l) {
            return l.trim().replace(/^at /, "");
        }).join(" | "));
    }
});

typedef struct {
    Coroutine base;
    void *stack;
    size_t stack_size;

    void *asyncify_stack;
    size_t asyncify_stack_size;

    CoroutineAction action;

    emscripten_fiber_t fiber;
} CoroutineEmscripten;

/**
 * Per-thread coroutine bookkeeping
 */
QEMU_DEFINE_STATIC_CO_TLS(Coroutine *, current);
QEMU_DEFINE_STATIC_CO_TLS(CoroutineEmscripten *, leader);
size_t leader_asyncify_stack_size = COROUTINE_STACK_SIZE;

static void coroutine_trampoline(void *co_)
{
    Coroutine *co = co_;

    while (true) {
        co->entry(co->entry_arg);
        qemu_coroutine_switch(co, co->caller, COROUTINE_TERMINATE);
    }
}

Coroutine *qemu_coroutine_new(void)
{
    CoroutineEmscripten *co;

    co = g_malloc0(sizeof(*co));

    co->stack_size = COROUTINE_STACK_SIZE;
    co->stack = qemu_alloc_stack(&co->stack_size);

    co->asyncify_stack_size = COROUTINE_STACK_SIZE;
    co->asyncify_stack = g_malloc0(co->asyncify_stack_size);
    emscripten_fiber_init(&co->fiber, coroutine_trampoline, &co->base,
                          co->stack, co->stack_size, co->asyncify_stack,
                          co->asyncify_stack_size);

    return &co->base;
}

void qemu_coroutine_delete(Coroutine *co_)
{
    CoroutineEmscripten *co = DO_UPCAST(CoroutineEmscripten, base, co_);

    qemu_free_stack(co->stack, co->stack_size);
    g_free(co->asyncify_stack);
    g_free(co);
}

CoroutineAction qemu_coroutine_switch(Coroutine *from_, Coroutine *to_,
                      CoroutineAction action)
{
    CoroutineEmscripten *from = DO_UPCAST(CoroutineEmscripten, base, from_);
    CoroutineEmscripten *to = DO_UPCAST(CoroutineEmscripten, base, to_);

    if (co_forbidden_thread) {
        /*
         * Only the frames in configs/meson/asyncify-only.txt can be
         * unwound; a vCPU thread stack (JIT'd TB frames, invoke_*
         * wrappers) never can, so fail loudly instead of derailing.
         */
        fprintf(stderr, "qemu: coroutine switch on the vCPU thread "
                "(not Asyncify-instrumented) — aborting\n");
        abort();
    }
    set_current(to_);
    to->action = action;
    if (getenv("QEMU_COSTACK")) {
        wasm_costack_trace();
    }
    emscripten_fiber_swap(&from->fiber, &to->fiber);
    return from->action;
}

Coroutine *qemu_coroutine_self(void)
{
    Coroutine *self = get_current();

    if (!self) {
        CoroutineEmscripten *leaderp = get_leader();
        if (!leaderp) {
            leaderp = g_malloc0(sizeof(*leaderp));
            leaderp->asyncify_stack = g_malloc0(leader_asyncify_stack_size);
            leaderp->asyncify_stack_size = leader_asyncify_stack_size;
            emscripten_fiber_init_from_current_context(
                &leaderp->fiber,
                leaderp->asyncify_stack,
                leaderp->asyncify_stack_size);
            leaderp->stack = leaderp->fiber.stack_limit;
            leaderp->stack_size =
                leaderp->fiber.stack_base - leaderp->fiber.stack_limit;
            set_leader(leaderp);
        }
        self = &leaderp->base;
        set_current(self);
    }
    return self;
}

bool qemu_in_coroutine(void)
{
    Coroutine *self = get_current();

    return self && self->caller;
}

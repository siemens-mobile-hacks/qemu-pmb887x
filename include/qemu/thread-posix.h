#ifndef QEMU_THREAD_POSIX_H
#define QEMU_THREAD_POSIX_H

#include <pthread.h>
#include <semaphore.h>

struct QemuMutex {
    pthread_mutex_t lock;
#ifdef CONFIG_DEBUG_MUTEX
    const char *file;
    int line;
#endif
    bool initialized;
};

/*
 * QemuRecMutex cannot be a typedef of QemuMutex lest we have two
 * compatible cases in _Generic.  See qemu/lockable.h.
 */
typedef struct QemuRecMutex {
    QemuMutex m;
} QemuRecMutex;

#ifdef __EMSCRIPTEN__
/*
 * Asyncify-safe futex-based condition variable (see util/qemu-thread-posix.c
 * for the rationale: pthread_cond signals are unreliable under Asyncify).
 */
struct QemuCond {
    int seq;      /* bumped by every signal/broadcast; futex word */
    int waiters;  /* number of threads blocked in qemu_cond_wait */
};
#else
struct QemuCond {
    pthread_cond_t cond;
    bool initialized;
};
#endif

struct QemuSemaphore {
    QemuMutex mutex;
    QemuCond cond;
    unsigned int count;
};

struct QemuThread {
    pthread_t thread;
};

#endif

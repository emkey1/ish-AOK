// pthread synchronisation: pthread_once, a mutex handed between threads, timed
// lock and condition-variable timeouts, condvar signal, and readers then a
// writer on an rwlock.
#define _GNU_SOURCE

#include <errno.h>
#include <pthread.h>
#include <sched.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "test_common.h"

static void sleep_ms(int ms) {
    struct timespec ts;
    ts.tv_sec = ms / 1000;
    ts.tv_nsec = (long) (ms % 1000) * 1000000L;
    nanosleep(&ts, NULL);
}

static struct timespec timespec_after_ms(int ms) {
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    ts.tv_sec += ms / 1000;
    ts.tv_nsec += (long) (ms % 1000) * 1000000L;
    if (ts.tv_nsec >= 1000000000L) {
        ts.tv_sec++;
        ts.tv_nsec -= 1000000000L;
    }
    return ts;
}

static pthread_once_t once_control = PTHREAD_ONCE_INIT;
static int once_counter;

static void reset_once_control(void) {
    static const pthread_once_t once_init = PTHREAD_ONCE_INIT;
    memcpy(&once_control, &once_init, sizeof(once_control));
}

static int mutex_timedlock_portable(pthread_mutex_t *mutex, const struct timespec *deadline) {
#if defined(__APPLE__)
    for (;;) {
        struct timespec now;
        int rc = pthread_mutex_trylock(mutex);
        if (rc != EBUSY)
            return rc;
        clock_gettime(CLOCK_REALTIME, &now);
        if (now.tv_sec > deadline->tv_sec ||
            (now.tv_sec == deadline->tv_sec && now.tv_nsec >= deadline->tv_nsec))
            return ETIMEDOUT;
        sleep_ms(1);
    }
#else
    return pthread_mutex_timedlock(mutex, deadline);
#endif
}

static void once_init(void) {
    once_counter++;
}

static void *once_thread(void *arg) {
    (void) arg;
    if (pthread_once(&once_control, once_init) != 0) {
        perror("pthread_once");
        exit(1);
    }
    return NULL;
}

static void test_pthread_once(void) {
    pthread_t threads[4];
    int failed;

    reset_once_control();
    once_counter = 0;
    for (int i = 0; i < 4; i++) {
        if (pthread_create(&threads[i], NULL, once_thread, NULL) != 0) {
            perror("pthread_create");
            exit(1);
        }
    }
    for (int i = 0; i < 4; i++)
        pthread_join(threads[i], NULL);

    failed = once_counter != 1;
    test_log_if(failed, "pthread once: counter=%d\n", once_counter);
    if (failed)
        failf("pthread once", once_counter, 0, 0, 1, 0, 0);
}

struct mutex_handoff_state {
    pthread_mutex_t mutex;
    int value;
    int done;
};

static void *mutex_handoff_thread(void *arg) {
    struct mutex_handoff_state *state = arg;
    pthread_mutex_lock(&state->mutex);
    state->value++;
    state->done = 1;
    pthread_mutex_unlock(&state->mutex);
    return NULL;
}

static void test_mutex_handoff(void) {
    struct mutex_handoff_state state = {
        .mutex = PTHREAD_MUTEX_INITIALIZER,
        .value = 0,
        .done = 0,
    };
    pthread_t thread;
    int failed;

    pthread_mutex_lock(&state.mutex);
    if (pthread_create(&thread, NULL, mutex_handoff_thread, &state) != 0) {
        perror("pthread_create");
        exit(1);
    }
    sleep_ms(20);
    failed = state.done != 0;
    test_log_if(failed, "pthread mutex handoff pre-unlock: done=%d value=%d\n",
                state.done, state.value);
    if (failed)
        failf("pthread mutex handoff pre-unlock", state.done, state.value, 0, 0, 0, 0);

    pthread_mutex_unlock(&state.mutex);
    pthread_join(thread, NULL);

    failed = state.done != 1 || state.value != 1;
    test_log_if(failed, "pthread mutex handoff: done=%d value=%d\n",
                state.done, state.value);
    if (failed)
        failf("pthread mutex handoff", state.done, state.value, 0, 1, 1, 0);
}

struct timedlock_state {
    pthread_mutex_t mutex;
    int ready;
};

static void *timedlock_holder_thread(void *arg) {
    struct timedlock_state *state = arg;
    pthread_mutex_lock(&state->mutex);
    __atomic_store_n(&state->ready, 1, __ATOMIC_RELEASE);
    sleep_ms(200);
    pthread_mutex_unlock(&state->mutex);
    return NULL;
}

static void test_mutex_timedlock_timeout(void) {
    struct timedlock_state state = {
        .mutex = PTHREAD_MUTEX_INITIALIZER,
        .ready = 0,
    };
    pthread_t thread;
    struct timespec deadline;
    int rc;
    int failed;

    if (pthread_create(&thread, NULL, timedlock_holder_thread, &state) != 0) {
        perror("pthread_create");
        exit(1);
    }
    while (!__atomic_load_n(&state.ready, __ATOMIC_ACQUIRE))
        sched_yield();

    deadline = timespec_after_ms(50);
    rc = mutex_timedlock_portable(&state.mutex, &deadline);
    pthread_join(thread, NULL);

    failed = rc != ETIMEDOUT;
    test_log_if(failed, "pthread mutex timedlock timeout: rc=%d\n", rc);
    if (failed)
        failf("pthread mutex timedlock timeout", rc, 0, 0, ETIMEDOUT, 0, 0);
}

struct condvar_signal_state {
    pthread_mutex_t mutex;
    pthread_cond_t cond;
    int ready;
    int go;
    int seen;
};

static void *condvar_wait_thread(void *arg) {
    struct condvar_signal_state *state = arg;
    pthread_mutex_lock(&state->mutex);
    state->ready = 1;
    while (!state->go)
        pthread_cond_wait(&state->cond, &state->mutex);
    state->seen = state->go;
    pthread_mutex_unlock(&state->mutex);
    return NULL;
}

static void test_condvar_signal(void) {
    struct condvar_signal_state state = {
        .mutex = PTHREAD_MUTEX_INITIALIZER,
        .cond = PTHREAD_COND_INITIALIZER,
        .ready = 0,
        .go = 0,
        .seen = 0,
    };
    pthread_t thread;
    int failed;

    if (pthread_create(&thread, NULL, condvar_wait_thread, &state) != 0) {
        perror("pthread_create");
        exit(1);
    }
    while (!__atomic_load_n(&state.ready, __ATOMIC_ACQUIRE))
        sched_yield();

    pthread_mutex_lock(&state.mutex);
    state.go = 1234;
    pthread_cond_signal(&state.cond);
    pthread_mutex_unlock(&state.mutex);
    pthread_join(thread, NULL);

    failed = state.seen != 1234;
    test_log_if(failed, "pthread condvar signal: seen=%d\n", state.seen);
    if (failed)
        failf("pthread condvar signal", state.seen, 0, 0, 1234, 0, 0);
}

static void test_condvar_timedwait_timeout(void) {
    pthread_mutex_t mutex = PTHREAD_MUTEX_INITIALIZER;
    pthread_cond_t cond = PTHREAD_COND_INITIALIZER;
    struct timespec deadline;
    int rc;
    int failed;

    pthread_mutex_lock(&mutex);
    deadline = timespec_after_ms(50);
    rc = pthread_cond_timedwait(&cond, &mutex, &deadline);
    pthread_mutex_unlock(&mutex);

    failed = rc != ETIMEDOUT;
    test_log_if(failed, "pthread condvar timedwait timeout: rc=%d\n", rc);
    if (failed)
        failf("pthread condvar timedwait timeout", rc, 0, 0, ETIMEDOUT, 0, 0);
}

struct rwlock_state {
    pthread_rwlock_t lock;
    pthread_mutex_t gate;
    pthread_cond_t cond;
    int active_readers;
    int max_readers;
    int readers_ready;
    int release_readers;
    int writer_acquired;
    int shared_value;
};

static void *rwlock_reader_thread(void *arg) {
    struct rwlock_state *state = arg;
    pthread_rwlock_rdlock(&state->lock);
    pthread_mutex_lock(&state->gate);
    state->active_readers++;
    if (state->active_readers > state->max_readers)
        state->max_readers = state->active_readers;
    state->readers_ready++;
    pthread_cond_broadcast(&state->cond);
    while (!state->release_readers)
        pthread_cond_wait(&state->cond, &state->gate);
    state->active_readers--;
    pthread_mutex_unlock(&state->gate);
    pthread_rwlock_unlock(&state->lock);
    return NULL;
}

static void *rwlock_writer_thread(void *arg) {
    struct rwlock_state *state = arg;
    pthread_rwlock_wrlock(&state->lock);
    pthread_mutex_lock(&state->gate);
    state->writer_acquired = 1;
    state->shared_value = 77;
    pthread_cond_broadcast(&state->cond);
    pthread_mutex_unlock(&state->gate);
    pthread_rwlock_unlock(&state->lock);
    return NULL;
}

static void test_rwlock_readers_then_writer(void) {
    struct rwlock_state state = {
        .lock = PTHREAD_RWLOCK_INITIALIZER,
        .gate = PTHREAD_MUTEX_INITIALIZER,
        .cond = PTHREAD_COND_INITIALIZER,
        .active_readers = 0,
        .max_readers = 0,
        .readers_ready = 0,
        .release_readers = 0,
        .writer_acquired = 0,
        .shared_value = 0,
    };
    pthread_t readers[2];
    pthread_t writer;
    int failed;

    for (int i = 0; i < 2; i++) {
        if (pthread_create(&readers[i], NULL, rwlock_reader_thread, &state) != 0) {
            perror("pthread_create");
            exit(1);
        }
    }

    pthread_mutex_lock(&state.gate);
    while (state.readers_ready != 2)
        pthread_cond_wait(&state.cond, &state.gate);
    pthread_mutex_unlock(&state.gate);

    if (pthread_create(&writer, NULL, rwlock_writer_thread, &state) != 0) {
        perror("pthread_create");
        exit(1);
    }
    sleep_ms(20);

    failed = state.writer_acquired != 0;
    test_log_if(failed, "pthread rwlock pre-release: writer_acquired=%d\n",
                state.writer_acquired);
    if (failed)
        failf("pthread rwlock pre-release", state.writer_acquired, 0, 0, 0, 0, 0);

    pthread_mutex_lock(&state.gate);
    state.release_readers = 1;
    pthread_cond_broadcast(&state.cond);
    pthread_mutex_unlock(&state.gate);

    for (int i = 0; i < 2; i++)
        pthread_join(readers[i], NULL);
    pthread_join(writer, NULL);

    failed = state.max_readers < 2 || state.writer_acquired != 1 || state.shared_value != 77;
    test_log_if(failed, "pthread rwlock final: max_readers=%d writer_acquired=%d shared=%d\n",
                state.max_readers, state.writer_acquired, state.shared_value);
    if (failed)
        failf("pthread rwlock final", state.max_readers, state.writer_acquired,
              state.shared_value, 2, 1, 77);
}

int main(int argc, char **argv) {
    test_init(argc, argv);
    test_pthread_once();
    test_mutex_handoff();
    test_mutex_timedlock_timeout();
    test_condvar_signal();
    test_condvar_timedwait_timeout();
    test_rwlock_readers_then_writer();
    return finish_suite("pthread_sync");
}

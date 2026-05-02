/*
 * Bounded MPMC queue + worker pool.  See work.h for the contract.
 *
 * Implementation: a circular ring of strings, mutex-protected, two
 * condvars (`not_full` / `not_empty`).  Producer waits on `not_full`
 * when slots are exhausted; workers wait on `not_empty` while the
 * ring is empty AND `closed` is false.  When the producer is done it
 * sets `closed=true` and broadcasts `not_empty`; workers exit once
 * they see `closed` and the ring is drained.
 *
 * One global `out_mutex` serialises stdout flushes so per-file blocks
 * never interleave across threads.  We don't need it for the shared
 * counters because `total_match_count` is folded back into the base
 * grep_state under the same flush mutex, so the lock is held for the
 * whole "flush + accounting" critical section.
 */

#include "work.h"
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

struct work_pool {
    /* Bounded ring. */
    char           **slots;     /* strdup'd paths */
    int              cap;       /* power of 2 not required */
    int              n;         /* number of slots in use */
    int              head;      /* dequeue index */
    int              tail;      /* enqueue index */

    pthread_mutex_t  q_mutex;
    pthread_cond_t   not_full;
    pthread_cond_t   not_empty;
    bool             closed;    /* producer says no more work */

    /* Worker plumbing. */
    pthread_t       *threads;
    void           **worker_args;   /* one per thread, from `setup` */
    int              n_workers;
    work_fn          fn;
    work_setup_fn    setup;
    work_teardown_fn teardown;
    void            *setup_user;

    /* Output serialisation + shared match counter (folded under the
     * same mutex at flush time — read by the producer after join to
     * decide the exit code). */
    pthread_mutex_t  out_mutex;
    long             total_match_count;
};

static void *
worker_main(void *arg)
{
    work_pool_t *p = (work_pool_t *)arg;

    /* Pick up our slot index by atomic stamp.  Cheaper than passing
     * an explicit index in: each thread races to claim the next free
     * `worker_args` slot under the queue mutex. */
    pthread_mutex_lock(&p->q_mutex);
    int my_slot = -1;
    for (int i = 0; i < p->n_workers; i++) {
        if (p->worker_args[i] == (void *)-1) {
            p->worker_args[i] = NULL;   /* claimed; will fill below */
            my_slot = i;
            break;
        }
    }
    pthread_mutex_unlock(&p->q_mutex);

    void *self = NULL;
    if (p->setup) self = p->setup(p, p->setup_user);
    if (my_slot >= 0) p->worker_args[my_slot] = self;

    for (;;) {
        char *task = NULL;
        pthread_mutex_lock(&p->q_mutex);
        while (p->n == 0 && !p->closed) {
            pthread_cond_wait(&p->not_empty, &p->q_mutex);
        }
        if (p->n == 0 && p->closed) {
            pthread_mutex_unlock(&p->q_mutex);
            break;
        }
        task = p->slots[p->head];
        p->slots[p->head] = NULL;
        p->head = (p->head + 1) % p->cap;
        p->n--;
        pthread_cond_signal(&p->not_full);
        pthread_mutex_unlock(&p->q_mutex);

        p->fn(task, self);
        free(task);
    }

    if (p->teardown) p->teardown(self);
    return NULL;
}

work_pool_t *
work_pool_create(int n_workers,
                 work_fn        fn,
                 work_setup_fn  setup,
                 work_teardown_fn teardown,
                 void          *setup_user)
{
    if (n_workers < 1) n_workers = 1;
    work_pool_t *p = (work_pool_t *)calloc(1, sizeof(*p));
    if (!p) return NULL;

    p->cap   = n_workers * 4;
    if (p->cap < 8) p->cap = 8;
    p->slots = (char **)calloc((size_t)p->cap, sizeof(char *));

    pthread_mutex_init(&p->q_mutex,   NULL);
    pthread_mutex_init(&p->out_mutex, NULL);
    pthread_cond_init (&p->not_full,  NULL);
    pthread_cond_init (&p->not_empty, NULL);

    p->n_workers   = n_workers;
    p->fn          = fn;
    p->setup       = setup;
    p->teardown    = teardown;
    p->setup_user  = setup_user;
    p->threads     = (pthread_t *)calloc((size_t)n_workers, sizeof(pthread_t));
    p->worker_args = (void **)    calloc((size_t)n_workers, sizeof(void *));
    /* Sentinel `(void*)-1` means "slot claimable"; workers swap it
     * for their `self` pointer the first time they run.  This avoids
     * passing an explicit per-thread index to the worker — we don't
     * need ordering, just unique ownership. */
    for (int i = 0; i < n_workers; i++) p->worker_args[i] = (void *)-1;

    for (int i = 0; i < n_workers; i++) {
        pthread_create(&p->threads[i], NULL, worker_main, p);
    }
    return p;
}

void
work_pool_submit(work_pool_t *p, const char *path)
{
    char *dup = strdup(path);
    if (!dup) return;

    pthread_mutex_lock(&p->q_mutex);
    while (p->n == p->cap) {
        pthread_cond_wait(&p->not_full, &p->q_mutex);
    }
    p->slots[p->tail] = dup;
    p->tail = (p->tail + 1) % p->cap;
    p->n++;
    pthread_cond_signal(&p->not_empty);
    pthread_mutex_unlock(&p->q_mutex);
}

long
work_pool_join_and_destroy(work_pool_t *p)
{
    pthread_mutex_lock(&p->q_mutex);
    p->closed = true;
    pthread_cond_broadcast(&p->not_empty);
    pthread_mutex_unlock(&p->q_mutex);

    for (int i = 0; i < p->n_workers; i++) pthread_join(p->threads[i], NULL);

    /* All workers exited → no more flushes can race; safe to read
     * the counter without the out_mutex. */
    const long total = p->total_match_count;

    pthread_mutex_destroy(&p->q_mutex);
    pthread_mutex_destroy(&p->out_mutex);
    pthread_cond_destroy (&p->not_full);
    pthread_cond_destroy (&p->not_empty);
    free(p->slots);
    free(p->threads);
    free(p->worker_args);
    free(p);
    return total;
}

void
work_pool_flush_buf(work_pool_t *p, const char *buf, size_t len, long delta_matches)
{
    pthread_mutex_lock(&p->out_mutex);
    if (len) fwrite(buf, 1, len, stdout);
    p->total_match_count += delta_matches;
    pthread_mutex_unlock(&p->out_mutex);
}

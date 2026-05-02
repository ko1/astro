/*
 * Bounded MPMC work queue + worker pool with shared-stdout output
 * batching.  See work.h for the design rationale.
 *
 * One `pthread_mutex_t` protects the queue (producer ↔ workers); a
 * second protects stdout + the total match counter.  Workers call
 * `work_pool_flush_batch` once per batch (rather than once per file)
 * which drops mutex acquisition rate to roughly 1/k where k is the
 * number of files per batch.
 */

#include "work.h"
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

struct work_pool {
    /* ─── Work queue ─────────────────────────────────────────── */
    char           **slots;
    int              cap;
    int              n;
    int              head;
    int              tail;
    pthread_mutex_t  q_mutex;
    pthread_cond_t   not_full;
    pthread_cond_t   not_empty;
    bool             closed;

    /* ─── Worker plumbing ────────────────────────────────────── */
    pthread_t       *threads;
    void           **worker_args;
    int              n_workers;
    work_fn          fn;
    work_setup_fn    setup;
    work_teardown_fn teardown;
    void            *setup_user;

    /* ─── Output batching ────────────────────────────────────── */
    pthread_mutex_t  out_mutex;
    long             total_match_count;
};

static void *
worker_main(void *arg)
{
    work_pool_t *p = (work_pool_t *)arg;

    /* Claim a worker_args slot under q_mutex (race-free). */
    pthread_mutex_lock(&p->q_mutex);
    int my_slot = -1;
    for (int i = 0; i < p->n_workers; i++) {
        if (p->worker_args[i] == (void *)-1) {
            p->worker_args[i] = NULL;
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

void
work_pool_flush_batch(work_pool_t *p, const char *buf, size_t len, long delta_matches)
{
    if (len == 0 && delta_matches == 0) return;
    pthread_mutex_lock(&p->out_mutex);
    if (len) fwrite(buf, 1, len, stdout);
    p->total_match_count += delta_matches;
    pthread_mutex_unlock(&p->out_mutex);
}

long
work_pool_join_and_destroy(work_pool_t *p)
{
    pthread_mutex_lock(&p->q_mutex);
    p->closed = true;
    pthread_cond_broadcast(&p->not_empty);
    pthread_mutex_unlock(&p->q_mutex);

    for (int i = 0; i < p->n_workers; i++) pthread_join(p->threads[i], NULL);

    /* All workers have finished their teardown — including the
     * final flush of any leftover batch — so the counter is fully
     * accumulated and stable. */
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

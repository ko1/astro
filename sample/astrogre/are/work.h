/*
 * are — bounded MPMC work queue for the parallel file walker.
 *
 * The recursive walker (main thread) acts as the producer: it enqueues
 * one task per file it decides to scan.  N worker threads consume,
 * each calling `process_file` with its own private `grep_state_t`
 * whose `out` is an `open_memstream` buffer.  When a worker finishes
 * a task it acquires the global stdout mutex, dumps its buffer to the
 * real stdout, then resets the buffer for the next task.
 *
 * Bound: the queue caps at `4 × n_workers` slots so the producer
 * blocks early enough that we don't accumulate millions of pending
 * `strdup`'d paths in RAM on huge trees, but the workers always have
 * something to grab.
 *
 * `-j 1` bypasses the pool entirely (saves the per-task FILE * setup
 * + mutex round-trip), so the cost of "have parallelism support" is
 * zero on the hot single-threaded path.
 */

#ifndef ARE_WORK_H
#define ARE_WORK_H

#include <pthread.h>
#include <stdio.h>
#include <stdbool.h>
#include <stddef.h>

/* Forward decl — we don't pull in the full grep_state_t header here
 * to avoid coupling.  `worker_arg` is a `grep_state_t *` from the
 * caller's perspective; it stays opaque to the queue. */

typedef void (*work_fn)(const char *path, void *worker_arg);

typedef struct work_pool work_pool_t;

/* Per-worker context: the worker's grep_state and a thread handle.
 * The pool keeps an array of these; each thread reads tasks from the
 * shared queue.  `setup` is run once per worker before the loop, and
 * `teardown` once after the loop exits — so per-thread allocations
 * (the memstream FILE *, the `open_memstream` backing buffer) live
 * for the full session, not per-task.
 *
 * `setup` receives the pool itself in addition to the caller's
 * `setup_user`, so the worker can call `work_pool_flush_buf` from
 * its task without an additional indirection.  This also avoids a
 * subtle race where `setup_user` is built around a not-yet-assigned
 * `pool` pointer. */
typedef void *(*work_setup_fn)   (work_pool_t *pool, void *setup_user);
typedef void  (*work_teardown_fn)(void *worker_arg);

/* Spin up `n_workers` threads.  `setup`/`teardown` may be NULL — in
 * which case `worker_arg` passed to `fn` is just `setup_user`. */
work_pool_t *work_pool_create(int n_workers,
                              work_fn        fn,
                              work_setup_fn  setup,
                              work_teardown_fn teardown,
                              void          *setup_user);

/* Enqueue a path (the queue takes ownership of the strdup'd copy
 * internally).  Blocks while the queue is full. */
void         work_pool_submit(work_pool_t *p, const char *path);

/* Signal that no more tasks are coming, wait for workers to finish
 * draining, free all resources, and return the accumulated total
 * match count.  Pool must not be used after. */
long         work_pool_join_and_destroy(work_pool_t *p);

/* Flush `buf[0..len)` to real stdout under the pool's shared output
 * mutex.  Used by the worker's `fn` after each task completes — this
 * is what keeps per-file output blocks from interleaving across
 * threads.  Safe to call before `work_pool_join_and_destroy`.
 *
 * `delta_matches` is added to a shared counter under the same mutex
 * (so the per-thread match count is folded back into the base
 * grep_state at flush time, exit-code semantics preserved). */
void         work_pool_flush_buf(work_pool_t *p, const char *buf, size_t len,
                                 long delta_matches);

#endif  /* ARE_WORK_H */

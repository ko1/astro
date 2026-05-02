/*
 * are — bounded MPMC work queue for the parallel file walker.
 *
 * The recursive walker (main thread) acts as the producer: it
 * enqueues one task per file it decides to scan.  N worker threads
 * consume, each calling `process_file` with its own private
 * `grep_state_t`.  Workers accumulate per-file output into a
 * thread-local batch buffer; when the buffer crosses a threshold
 * (or the worker exits) they take a single shared stdout mutex,
 * fwrite the whole batch, and release.
 *
 * Why batching: the previous "fwrite per file under shared mutex"
 * design serialised the actual fwrite call, causing futex storms
 * once N workers all started competing for the lock.  Batching N
 * files of output into one fwrite drops mutex acquisition rate by
 * the same factor.  The order within each batch is preserved
 * (per-thread FIFO); across batches output may be interleaved at
 * batch boundaries — same as ripgrep's default `--sort=none`.
 *
 * `-j 1` bypasses the entire pool (no threads, no queues), so the
 * cost of "have parallelism support" is zero on the hot single-
 * threaded path.
 */

#ifndef ARE_WORK_H
#define ARE_WORK_H

#include <pthread.h>
#include <stdio.h>
#include <stdbool.h>
#include <stddef.h>

typedef struct work_pool work_pool_t;

/* Per-worker context: see worker_state in main.c.  `setup` runs once
 * per worker before the first task, and `teardown` once after the
 * loop exits — so per-thread allocations (the batch buffer) live for
 * the full session.
 *
 * `setup` receives the pool itself in addition to `setup_user`,
 * which avoids a race where setup_user references the pool before
 * it's been assigned. */
typedef void (*work_fn)         (const char *path, void *worker_arg);
typedef void *(*work_setup_fn)  (work_pool_t *pool, void *setup_user);
typedef void  (*work_teardown_fn)(void *worker_arg);

work_pool_t *work_pool_create(int n_workers,
                              work_fn        fn,
                              work_setup_fn  setup,
                              work_teardown_fn teardown,
                              void          *setup_user);

/* Enqueue a path (queue takes ownership of the strdup'd copy
 * internally).  Blocks while the queue is full. */
void         work_pool_submit(work_pool_t *p, const char *path);

/* Atomically fwrite `buf[0..len)` to stdout under the pool's shared
 * output mutex, and add `delta_matches` to the shared total counter
 * under the same mutex.  Workers call this with their batched
 * buffer when it fills (or in teardown for the leftover).
 *
 * Buffer ownership stays with the caller; the function only reads
 * from it.  Safe to call before or after `work_pool_join_and_destroy`
 * on a per-worker buffer the caller manages. */
void         work_pool_flush_batch(work_pool_t *p, const char *buf, size_t len,
                                   long delta_matches);

/* Signal that no more tasks are coming, wait for workers to finish
 * draining (any leftover batches get flushed in their teardown
 * callbacks), free all resources, return the accumulated total
 * match count. */
long         work_pool_join_and_destroy(work_pool_t *p);

#endif  /* ARE_WORK_H */

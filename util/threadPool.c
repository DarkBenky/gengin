#include "threadPool.h"
#include <stdlib.h>
#include <string.h>

static void *worker(void *arg) {
    ThreadPool *p = arg;

    while (1) {
        // Taking a task needs no lock: one atomic index hands every worker a
        // different task, and the acquire load of `published` pairs with the
        // release store in poolWait(), so a task is built before it is claimed.
        long long i = atomic_load_explicit(&p->claim, memory_order_relaxed);
        if (i < atomic_load_explicit(&p->published, memory_order_acquire)) {
            if (atomic_compare_exchange_weak_explicit(&p->claim, &i, i + 1,
                                                      memory_order_relaxed, memory_order_relaxed)) {
                Task t = p->queue[i & p->mask];
                t.fn(t.arg);
                // Only the worker that drains the region pays a lock here.
                if (atomic_fetch_sub_explicit(&p->pending, 1, memory_order_acq_rel) == 1) {
                    pthread_mutex_lock(&p->lock);
                    pthread_cond_signal(&p->done_cond);
                    pthread_mutex_unlock(&p->lock);
                }
            }
            continue;
        }

        pthread_mutex_lock(&p->lock);
        while (atomic_load_explicit(&p->published, memory_order_relaxed) <=
                       atomic_load_explicit(&p->claim, memory_order_relaxed) &&
               !p->stop)
            pthread_cond_wait(&p->work_cond, &p->lock);
        int stop = p->stop;
        pthread_mutex_unlock(&p->lock);
        if (stop) return NULL;
    }
}

ThreadPool *poolCreate(int nthreads, int queue_cap) {
    ThreadPool *p = calloc(1, sizeof *p);
    if (!p) return NULL;

    int capacity = 1;
    while (capacity < queue_cap) capacity <<= 1;

    p->queue = malloc((size_t)capacity * sizeof(Task));
    if (!p->queue) { free(p); return NULL; }

    p->threads = malloc((size_t)nthreads * sizeof(pthread_t));
    if (!p->threads) { free(p->queue); free(p); return NULL; }

    p->capacity = capacity;
    p->mask = capacity - 1;
    p->nthreads = nthreads;
    atomic_init(&p->pending, 0);
    atomic_init(&p->claim, 0);
    atomic_init(&p->published, 0);

    pthread_mutex_init(&p->lock, NULL);
    pthread_cond_init(&p->work_cond, NULL);
    pthread_cond_init(&p->done_cond, NULL);

    for (int i = 0; i < nthreads; i++)
        pthread_create(&p->threads[i], NULL, worker, p);

    return p;
}

void poolAdd(ThreadPool *p, task_fn fn, void *arg) {
    pthread_mutex_lock(&p->lock);
    p->queue[p->queued & p->mask] = (Task){ fn, arg };
    p->queued++;
    pthread_mutex_unlock(&p->lock);
}

void poolWait(ThreadPool *p) {
    pthread_mutex_lock(&p->lock);

    // Hand the whole region over at once: one broadcast wakes the parked workers
    // for every task in it, instead of one lock/signal pair per task.
    long long queued = p->queued;
    long long published = atomic_load_explicit(&p->published, memory_order_relaxed);
    if (queued > published) {
        atomic_fetch_add_explicit(&p->pending, queued - published, memory_order_relaxed);
        atomic_store_explicit(&p->published, queued, memory_order_release);
        pthread_cond_broadcast(&p->work_cond);
    }

    while (atomic_load_explicit(&p->pending, memory_order_acquire) > 0)
        pthread_cond_wait(&p->done_cond, &p->lock);

    pthread_mutex_unlock(&p->lock);
}

void poolDestroy(ThreadPool *p) {
    pthread_mutex_lock(&p->lock);
    p->stop = 1;
    pthread_cond_broadcast(&p->work_cond);
    pthread_mutex_unlock(&p->lock);

    for (int i = 0; i < p->nthreads; i++)
        pthread_join(p->threads[i], NULL);

    pthread_mutex_destroy(&p->lock);
    pthread_cond_destroy(&p->work_cond);
    pthread_cond_destroy(&p->done_cond);

    free(p->queue);
    free(p->threads);
    free(p);
}

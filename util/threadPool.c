#include "threadPool.h"
#include <stdlib.h>
#include <string.h>

static void *worker(void *arg) {
    ThreadPool *p = arg;
    Task t;

    while (1) {
        pthread_mutex_lock(&p->lock);

        while (!p->count && !p->stop)
            pthread_cond_wait(&p->work_cond, &p->lock);

        if (p->stop && !p->count) {
            pthread_mutex_unlock(&p->lock);
            return NULL;
        }

        t = p->queue[p->head];
        p->head = (p->head + 1) % p->capacity;
        p->count--;

        pthread_mutex_unlock(&p->lock);

        t.fn(t.arg);

        pthread_mutex_lock(&p->lock);
        if (--p->pending == 0)
            pthread_cond_signal(&p->done_cond);
        pthread_mutex_unlock(&p->lock);
    }
}

ThreadPool *poolCreate(int nthreads, int queue_cap) {
    ThreadPool *p = calloc(1, sizeof *p);
    if (!p) return NULL;

    p->queue = malloc(queue_cap * sizeof(Task));
    if (!p->queue) { free(p); return NULL; }

    p->threads = malloc(nthreads * sizeof(pthread_t));
    if (!p->threads) { free(p->queue); free(p); return NULL; }

    p->capacity = queue_cap;
    p->nthreads = nthreads;

    pthread_mutex_init(&p->lock, NULL);
    pthread_cond_init(&p->work_cond, NULL);
    pthread_cond_init(&p->done_cond, NULL);

    for (int i = 0; i < nthreads; i++)
        pthread_create(&p->threads[i], NULL, worker, p);

    return p;
}

void poolAdd(ThreadPool *p, task_fn fn, void *arg) {
    pthread_mutex_lock(&p->lock);
    p->queue[p->tail] = (Task){ fn, arg };
    p->tail = (p->tail + 1) % p->capacity;
    p->count++;
    p->pending++;
    pthread_cond_signal(&p->work_cond);
    pthread_mutex_unlock(&p->lock);
}

void poolWait(ThreadPool *p) {
    pthread_mutex_lock(&p->lock);
    while (p->pending > 0)
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
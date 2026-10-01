#pragma once
#include <pthread.h>
#include <stdatomic.h>
#include <stddef.h>

typedef void (*task_fn)(void *arg);

typedef struct {
    task_fn fn;
    void   *arg;
} Task;

typedef struct {
    // A region is the work one poolWait() drains: poolAdd() appends to `queue`
    // without publishing it, poolWait() releases the whole region at once.
    // queue[i & mask] holds task i and the indices only ever grow, so the
    // counters need no reset between regions; a region must fit in `capacity`.
    Task           *queue;
    int             capacity;   // power of two, at least the largest region
    int             mask;
    long long       queued;     // next task index the producer appends at
    atomic_llong    pending;    // published tasks not finished yet
    atomic_llong    claim;      // next task index a worker may take
    atomic_llong    published;  // end of the tasks visible to the workers
    pthread_t      *threads;
    int             nthreads;
    pthread_mutex_t lock;
    pthread_cond_t  work_cond;
    pthread_cond_t  done_cond;
    int             stop;
} ThreadPool;

ThreadPool *poolCreate(int nthreads, int queue_cap);
void        poolAdd(ThreadPool *p, task_fn fn, void *arg);
void        poolWait(ThreadPool *p);
void        poolDestroy(ThreadPool *p);

#pragma once
#include <pthread.h>

typedef void (*task_fn)(void *arg);

typedef struct {
    task_fn fn;
    void   *arg;
} Task;

typedef struct {
    Task           *queue;
    int             capacity;
    int             head, tail, count;
    pthread_t      *threads;
    int             nthreads;
    int             pending;
    pthread_mutex_t lock;
    pthread_cond_t  work_cond;
    pthread_cond_t  done_cond;
    int             stop;
} ThreadPool;

ThreadPool *poolCreate(int nthreads, int queue_cap);
void        poolAdd(ThreadPool *p, task_fn fn, void *arg);
void        poolWait(ThreadPool *p);
void        poolDestroy(ThreadPool *p);
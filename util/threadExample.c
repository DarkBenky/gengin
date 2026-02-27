#include "threadpool.h"
#include <stdio.h>

#define N 16

typedef struct {
    int       n;
    long long result;
} FactArgs;

static void factTask(void *arg) {
    FactArgs *a = arg;
    long long r = 1;
    for (int i = 2; i <= a->n; i++) r *= i;
    a->result = r;
}

int main(void) {
    ThreadPool *pool = poolCreate(4, N);

    FactArgs args[N];
    for (int i = 0; i < N; i++) {
        args[i].n      = i;
        args[i].result = 0;
        poolAdd(pool, factTask, &args[i]);
    }

    poolWait(pool);

    for (int i = 0; i < N; i++)
        printf("%2d! = %lld\n", args[i].n, args[i].result);

    poolDestroy(pool);
    return 0;
}
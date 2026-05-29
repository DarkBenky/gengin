#ifndef EXAMPLE_BENCH_H
#define EXAMPLE_BENCH_H

#include <stdint.h>

/* Baseline: naive loop sum */
uint64_t sumBaseline(const int *arr, int n);

/* Optimized: loop unrolled by 4 */
uint64_t sumOptimized(const int *arr, int n);

#endif

#ifndef EXAMPLE_BENCH_H
#define EXAMPLE_BENCH_H

#include <stdint.h>

/* Baseline: naive loop sum */
uint64_t sumBaseline(const int *arr, int n);

/* Optimised: loop unrolled by 4 */
uint64_t sumOptimised(const int *arr, int n);

#endif

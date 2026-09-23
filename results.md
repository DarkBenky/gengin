# CPU ray pass: thread-pool work granularity

Scene: 136292 triangles (bench harness output; wall time in seconds).

| configuration | tasks | avg | median | min | max | p99 | variance |
|---|---|---|---|---|---|---|---|
| single-threaded | 1 | 0.026903 | 0.027284 | 0.025808 | 0.027431 | 0.027431 | 0.000000 |
| multi-threaded, 1 row/task | 600 | 0.005078 | 0.004794 | 0.004677 | 0.007933 | 0.007933 | 0.000001 |
| multi-threaded, 2 row/task | 300 | 0.005364 | 0.005025 | 0.004868 | 0.009057 | 0.009057 | 0.000001 |
| multi-threaded, 4 row/task | 150 | 0.005452 | 0.005324 | 0.004900 | 0.008086 | 0.008086 | 0.000000 |
| multi-threaded, 16 row/task | 38 | 0.006851 | 0.006668 | 0.006153 | 0.009899 | 0.009899 | 0.000001 |
| multi-threaded, 32 row/task | 19 | 0.007200 | 0.007292 | 0.006357 | 0.008172 | 0.008172 | 0.000000 |
| multi-threaded, 64 row/task | 10 | 0.010450 | 0.010735 | 0.008400 | 0.012504 | 0.012504 | 0.000002 |
| multi-threaded, 1 row/task (repeat) | 600 | 0.005038 | 0.004943 | 0.004715 | 0.006126 | 0.006126 | 0.000000 |

`multi-threaded, 1 row/task` (600 tasks) is the fastest configuration: about
5.3x faster than single-threaded, and its repeat run lands within 1% of it.
Coarser granularity is monotonically slower (64 rows/task costs 2.07x the
best), so scheduling overhead is not the bottleneck; the harness reported a
passing correctness check.

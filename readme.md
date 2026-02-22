```c 
// SLOW — pageable host memory, extra memcpy internally
float* buf = malloc(size);
clEnqueueReadBuffer(..., buf, ...);  // ~2-3x slower

// FAST — pinned memory, DMA direct transfer
cl_mem pinned = clCreateBuffer(ctx, CL_MEM_ALLOC_HOST_PTR, size, NULL, NULL);
float* buf = clEnqueueMapBuffer(..., pinned, ...);  // near theoretical bandwidth
```
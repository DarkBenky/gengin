# Codebase Architecture & Performance Analysis

## Overall Architecture

The engine is a real-time hybrid CPU/GPU ray tracer with deferred shading and post-processing. It renders a 1080×720 scene each frame.

### Key Modules (by directory)

| Directory | Responsibility |
|-----------|---------------|
| `main.c` + `render/core/` | Frame orchestration: clear, shadow, ray trace, skybox, SSR, clouds, bloom, dither, present |
| `render/cpu/` | CPU ray tracer (ray.c), screen-space reflections (ssr.c), bloom (bloom.c) |
| `render/gpu/` | OpenCL wrapper (format.h/c), cloud volume renderer (kernels/cloadrendering/) |
| `render/color/` | Color packing, unpacking, tone mapping, gamma, HSL ops |
| `object/` | Scene objects, BVH tree (object.c/h), materials (material/), scene graph (scene.h/c) |
| `math/` | Vector3, transform, scalar utility |
| `skybox/` | JPEG-based cube map loader and sampler |
| `load/` | Binary mesh loader (.bin format) |
| `util/` | Thread pool, benchmarking harness, bounding box helpers |
| `tests/` | SIMD prototypes: rayAABB_inv (AVX2/SSE), RayBoxIntersect (SSE/AVX2), ObjectBehindCamera (SSE/AVX2) |

### Frame Render Loop (in main.c `renderFrame()`)

```
clearBuffers(camera)
ShadowPostProcess(objects, objectCount, camera, shadowResolution, shadowFrameInterval)
RayTraceScene(objects, objectCount, camera, lib, &taskQueue, &threadPool, skybox)
applySkybox(skybox, camera, &threadPool, &skyTaskQueue)
SSRPostProcess(camera, &threadPool, ssrTasks, rowsPerTask)
CloudRenderer_Render(...)  [if clouds enabled, OpenCL]
CloudRenderer_Composite(...) [if clouds enabled, OpenCL]
BloomPostProcess(camera)
DitherPostProcess(camera, frame)
VisualizeBuffer(camera, viewMode)
Present(display, framebuffer)
```

Each parallel stage uses a `ThreadPool` (pthreads) that dispatches rows or tiles as tasks. The thread pool spins up worker threads once at startup.

---

## Performance-Critical Functions

### 1. `RayTraceRowFunc` (render/cpu/ray.c)
- **What it does**: The per-row entry point for ray tracing. For each pixel (777,600 pixels/frame):
  - Computes ray direction from camera
  - Iterates frustum-passed objects: `rayAABB_inv` world AABB test → `IntersectBVH` for hits
  - On hit: transforms normal to world space, fetches material, evaluates textures + normal mapping, computes diffuse + GGX specular + Fresnel
  - Computes sky reflection blend
  - Every `REFLECTION_RESOLUTION` columns: runs shadow ray, emission sampling (top-N emissive objects with BVH occlusion), reflection ray
  - Applies box blur across the row for accumulated reflection/emission/shadow
  - Writes to 8+ buffers per pixel (framebuffer, depth, normal, position, reflect, bloom, uv, triangleId, objectId)
- **Why it is slow**:
  - O(pixels × objects × BVH depth) – the inner loop over objects does AABB + BVH traversal per ray
  - High memory bandwidth: scattered writes to 9 separate buffers, each 1080×720
  - Texture sampling from 4096×4096 2D arrays (`Textures.colorMap`, `normalMap`, `MaterialMap`) – these are huge and cause cache misses
  - Per-pixel normal mapping requires building tangent frame from UV derivatives, TBN rotation – expensive trig/linear algebra
  - The row-wide box blur on reflection/emission/shadow arrays adds O(width × radius) per row
  - Emission sampling per pixel (every N columns) does BVH shadow test for each top-N emitter
- **Call graph**: `RayTraceScene` → `poolAdd` → `RayTraceRowFunc` → `rayAABB_inv`, `IntersectBVH`, `SampleSkybox`, `Float3_Normalize`, `Float3_Cross`, `Float3_Dot`, `hdrToLDR`, `PackColor`, `InverseTransformPointTRS`, `calculateUvCoordinates`, `calculateUvCoordinatesForTriangle`, `RandomObjectHitRay`, `RayCast`, `SampleEmission`

### 2. `IntersectBVH` (object/object.c)
- **What it does**: Traverses a BVH tree for a single ray. Transforms ray to object local space using cached inverse TRS. Uses precomputed `invDir` and `bias`. Visits nodes via an explicit stack. For internal nodes: tests both children via SSE `rayAABB_inv_x2_soa`. For leaves: loops triangles and calls `rayTriangle` (Möller–Trumbore).
- **Why it is slow**:
  - Called per (ray, object) pair; each traverses up to ~O(log N) nodes
  - Each leaf test loops over up to 4 triangles with the full ray-triangle intersection (many FMAs, divisions, conditionals)
  - The stack is small (64) but branch mispredictions on miss/hit are common
  - The BVH nodes are 64 bytes (1 cache line), but traversals are pointer-chasing random access
- **Call graph**: `IntersectBVH` → `rayAABB_inv_x2_soa` (SSE), `rayTriangle`

### 3. `IntersectBVH_Shadow` (object/object.c)
- Same as above but early-exits on first hit. Used for shadow rays and occlusion queries.

### 4. `rayAABB_inv` / `rayAABB_inv_x2_soa` (object/object.h)
- **What it does**: Branchless slab AABB test using precomputed bias and invDir. The `_x2_soa` variant tests two boxes simultaneously using SSE.
- **Why it is slow**: Still called once per object per ray. The scalar `rayAABB_inv` is not SIMD-vectorized; the SSE variant is only used inside BVH traversal (child pair). The outer world AABB loop in `rayCollision` and `RayTraceRowFunc` still uses the scalar version one object at a time. There are commented-out TODOs (ray.c:598) to replace this with AVX2 8-wide test.

### 5. `SampleSkybox` (skybox/skybox.c)
- **What it does**: Given a direction, selects the appropriate cube face then does nearest-point sampling into the loaded JPEG image.
- **Why it is slow**: Called twice per pixel (once for sky background, once for reflection). Uses nearest-point (no bilinear) but still does a division and branches on face selection. The skybox images are full-res JPEGs loaded into 32-bit RGBA arrays; cache footprint is 6 × image width × height × 4 bytes (e.g., 6×1024×1024×4 = 24 MB).

### 6. `SSRPostProcess` / `SSRProcessRow` (render/cpu/ssr.c)
- **What it does**: Per-pixel screen-space ray marching to find reflection hits. For each pixel with reflectivity > 5%, marches 96 steps at 0.22 step size. Each step transforms from world to screen space, tests depth buffer, and on hit does a bilinear fetch from `tempFramebuffer`.
- **Why it is slow**:
  - 96 steps per reflective pixel (~50% of pixels have some reflectivity)
  - Each step: world-to-screen transform (vector ops, div), 2D clamp + bilinear color fetch (4 color reads + interpolation)
  - The entire framebuffer is snapshotted to `tempFramebuffer` (777,600 color copies) before processing
  - `tempFramebuffer` is read for every step, causing high bandwidth
- **Call graph**: `SSRPostProcess` → memcpy → `poolAdd` → `SSRRowTask` → `SSRProcessRow`

### 7. `ShadowPostProcess` (render/cpu/ray.c)
- **What it does**: Runs every `frameInterval` frames. For each pixel at reduced resolution (step > 1), computes shadow by testing shadow ray against all objects, and computes a reflection probe by intersecting a single bounce ray with object AABBs. Results are blurred and cached.
- **Why it is slow**: When it recomputes, it does an AABB test for each object per sample point (at sub-sampled resolution). The shadow test is essentially `IntersectAnyBBox` which loops all objects. Blurring is done via separable box blur.

### 8. `BlurBuffer` / `BlurColorBuffer` (render/cpu/ray.c)
- **What it does**: Separable box blur on float and color arrays. Has early-exit skip if all values are 1.0f (shadow buffer). Each pass (horizontal then vertical) is O(width × height × radius) with a sliding window.
- **Why it is slow**: The sliding-window approach is O(n) per pixel, but the inner loop over the window includes branch-heavy early-exit checks and many 8/16-bit color channel operations.

### 9. `CloudRenderer_Render` / `CloudRenderer_Composite` (render/gpu/cload.c)
- **What it does**: Uploads CPU depth buffer to pinned GPU buffer, dispatches OpenCL kernels (cloud ray march, god rays, blur, composite), reads blended framebuffer back.
- **Why it is slow**: GPU-CPU boundary crossing: depth upload, framebuffer download, plus 4 kernel dispatches per frame. Each dispatch has overhead (argument setting, queue latency). The kernel itself (`render.cl`) ray marches 64 steps per pixel with trilinear density sampling from a 3D volume. Shadow marching adds 8 steps per sample. Henyey-Greenstein phase function uses a divide + pow.
- **Call graph**: `CloudRenderer_Render` → `CL_Buffer_Map`/`Unmap` for depth, `clSetKernelArg` (30+ calls), `CL_Dispatch2D` (4×), `CL_Buffer_Fill`, `CL_Finish`. `CloudRenderer_Composite` → map framebuffer, dispatch composite, unmap readback.

### 10. `createObjectBVH` (object/object.c)
- **What it does**: Builds a BVH tree for an object's triangles. Uses median-split on centroid, partitions in-place, computes bounding boxes for children.
- **Why it is slow**: Called once per object load, not per frame. O(N log N). The inner loops over triangles to compute centroids and bounding boxes can be expensive for large meshes but are not in the hot path.

### 11. `CalculateFaceEmissions` (object/object.c)
- **What it does**: For emissive objects, rasterizes 32×32 emission maps for 6 faces by raytracing against the object's own BVH. This is essentially rendering an orthographic shadow map for each face, using the same ray-triangle intersection code.
- **Why it is slow**: O(6 × 1024 rays × BVH depth). Called once per emissive object on load. Could be optimized but not per-frame.

---

## Critical Data Structures

### `Camera` (object/format.h)
- 20+ separate per-pixel buffers: `framebuffer`, `depthBuffer`, `normalBuffer`, `positionBuffer`, `reflectBuffer`, `bloomBuffer`, `accumulationBuffer`, `shadowCache`, `reflectCache`, `tempFramebuffer`, `tempBuffer_1/2`, `objectIdBuffer`, `triangleIdBuffer`, `uvBuffer`, etc.
- All allocated with `aligned_alloc(64)` for cache line alignment.
- Precomputed per-frame values: `right`, `up`, `viewDir`, `halfVec`, `renderLightDir`, `aspect`, `fovScale`.

### `Object` (object/object.h)
- Triangle geometry: `v1`, `v2`, `v3` (separate float3 arrays), `normals`, `uvs`, `materialIds`.
- World-space AABB: `worldBBmin/max`.
- Cached inverse TRS matrix: `_invScale`, `_invRotSin`, `_invRotCos` (rows of M⁻¹).
- Cached forward rotation: `_fwdRot0/1/2` (transpose of inverse).
- BVH tree (`BVH` struct): nodes with `soa[12]` (per-axis {mn0, mx0, mn1, mx1}) for two children, `leftFirst`, `triCount`, `triStart`.
- Pre-baked `_temp` color for AABB-only rendering mode.
- Emission maps: 6 × 32×32 float3 arrays.

### `BVHNode` (object/object.h)
- 64-byte struct (exactly one cache line):
  - `soa[12]` (48 bytes) – floats in SoA layout for SSE ray-AABB test
  - `leftFirst` / `triStart` and `triCount` (4 bytes each)
  - `_pad[2]` to fill 64 bytes

### `MaterialLib` (material/material.h)
- Array of `Material`: `color`, `roughness`, `metallic`, `emission`, `Textures*`.
- `Textures` contains three 4096×4096 2D arrays: `colorMap` (uint32), `normalMap` (uint32), `MaterialMap` (uint16).

### `ThreadPool` (util/threadPool.h)
- Bounded queue, pthreads, condvars. Used for all CPU parallel stages.

### Data Flow in Hot Path
```
Camera (precomputes right/up/fovScale)
  │
  v
RayTraceRowFunc [for each row in parallel]:
  │
  ├── Camera.position → ray origin
  ├── Camera.forward/right/up/fovScale → ray direction
  │
  ├── [for each frustum-passed Object]:
  │     ├── Object.worldBBmin/max → rayAABB_inv (world-space AABB test)
  │     └── Object.bvh → IntersectBVH:
  │            ├── Object._invScale/_invRotSin/_invRotCos → local transform
  │            ├── BVHNode.soa → rayAABB_inv_x2_soa (SSE child test)
  │            └── Object.v1/v2/v3 + Object.normals → rayTriangle
  │
  ├── On hit:
  │     ├── Object._fwdRot0/1/2 × local normal → world normal
  │     ├── MaterialLib.entries[matId] → color, roughness, metallic, emission
  │     ├── Textures.colorMap[uv][uv] → texture color
  │     ├── Textures.normalMap[uv][uv] + TBN → perturbed normal
  │     ├── Camera.lightDir → diffuse + GGX specular
  │     ├── Camera.forward → Fresnel
  │     └── Skybox.front/back/... → sky reflection
  │
  ├── Writes to:
  │     ├── Camera.framebuffer[idx]
  │     ├── Camera.depthBuffer[idx] (view-z)
  │     ├── Camera.normalBuffer[idx]
  │     ├── Camera.positionBuffer[idx]
  │     ├── Camera.reflectBuffer[idx] (reflect dir + gloss in .w)
  │     ├── Camera.bloomBuffer[idx]
  │     ├── Camera.uvBuffer[idx]
  │     ├── Camera.objectIdBuffer[idx]
  │     └── Camera.triangleIdBuffer[idx]
  │
  └── Row-wide blur of accumulated reflection/emission/shadow → Camera.framebuffer[idx] += blended
```

---

## Known TODOs & Low-Hanging Fruit

### From source TODOs:

1. **`object/object.c:755`**: "TODO: potential vectorization" – in `SampleEmission`, the loop over objects for `RayBoxItersect` is scalar.

2. **`render/cpu/ray.c:310`**: "TODO: can be vectorized by sampling color for multiple pixels at once" – in `SkyBoxTaskFunc`, the skybox sample loop is per-pixel.

3. **`render/cpu/ray.c:351`**: "TODO: replace with RayBoxIntersectV4 to use vectorization" – in `rayCollision`, the world AABB loop uses scalar `RayBoxItersect`. There's an SSE/AVX2 version in `tests/RayBoxItersect.h` that processes 4 boxes at once.

4. **`render/cpu/ray.c:368`**: Same as above, another AABB loop.

5. **`render/cpu/ray.c:598`**: "TODO: Use SIMD test against 8 boundingBoxes at once" – in `RayTraceRowFunc`'s per-pixel object loop. Commented-out code shows the intended AVX2 approach using `rayAABB_invV4_avx2` from `tests/rayAABB_inv.h`. This is the 
   single biggest vectorization opportunity: testing 8 world AABBs per SIMD instruction instead of 1.

6. **`tests/rayAABB_inv.h:47,90`**: TODOs to use `rayAABB_invV4_avx2` and `rayAABB_invV5_sse_x4` in ray tracer.

7. **`tests/RayBoxItersect.h:68,125`**: TODOs for `RayBoxIntersectV2` (SSE) and `RayBoxIntersectV4` (AVX2) that process 2/4 objects at once but are not yet integrated.

### Other Low-Hanging Fruit:

8. **Unused accumulation buffer**: `camera->accumulationBuffer` is allocated but never written or read in the current code. `clearBuffers` explicitly skips it.

9. **`clearBuffers` is a no-op**: All `memset` calls are commented out. The function is empty. This means buffers retain stale data between frames, though each pixel is fully overwritten in the ray trace stage. Not a performance issue, but confusing.

10. **`tempFramebuffer` double usage**: In `SSRPostProcess`, `tempFramebuffer` holds a snapshot of the framebuffer. In `ShadowPostProcess`, `tempFramebuffer` is used as a blur scratch buffer. These are non-overlapping passes so it's safe but could be renamed.

11. **`hdrToLDR` called redundantly**: In `RayTraceRowFunc`, the initial shading result is HDR → LDR packed to `framebuffer`. Then later, the reflection/emission accumulation calls `hdrToLDR` again on the blend. This is a double tone-mapping which may darken the image.

12. **Texture array layout**: The `Textures` struct uses 2D static arrays `[4096][4096]` which are 64 MB each. This causes TLB and cache pressure. Consider tiled or mipmapped access.

13. **Color packing/unpacking overhead**: Many operations pack to `Color` (uint32) then immediately unpack to `float3`. For example, the skybox blend in `SkyBoxTaskFunc` and `RayTraceRowFunc` do `UnpackColor`/`PackColor` in non-inlined functions. Using `float3` throughout with a final pack would reduce conversions.

14. **`frustumPassIndices` stored in task**: In `RayTraceScene`, `frustumPassIndices` is a stack-local array passed to each task struct by pointer. But `RayTraceRowFunc` reads this pointer – since all tasks share the same pointer and the data is on `RayTraceScene`'s stack, this works only because `poolWait` blocks before returning. It's fragile but works.

15. **`RandomFloat`/`RandomHemisphereDirection`**: Used in emission sampling and path tracing experiments. The `fmodf` in `RandomFloat` may be slow; could use integer LCG.

16. **SSR bilinear fetch macros**: The `#define CH` and `#define BLERP` macros in `ssr.c` expand inline but still do 4 color reads per step. Consider pre-fetching the row.

17. **OpenCL kernel overhead**: `render.cl` uses `__attribute__((reqd_work_group_size(LOCAL_W, 1, 1)))` for the blur kernel which requires rounding width up to 128. The main cloud kernel uses 8×8 work groups. 4 separate dispatch calls per frame add latency. Could fuse god rays + composite into a single kernel.

18. **Density volume upload**: `UploadVolumeToGpu` allocates a `temp` buffer, packs dimensions + density, then calls `CL_Buffer_CreateFromData` which copies again. The double copy is wasteful.

19. **`Bench` framework**: In `util/bench.h`, when `BENCH_MODE` is defined, frame hashes and base64-encoded frames are written to `bench_results.json`. This is expensive (base64 encode of entire framebuffer each frame). The `benchCaptureFrame` function allocates memory each call.

20. **Thread pool contention**: The thread pool's `poolAdd`/`poolWait` uses a single mutex and condvars. For fine-grained tasks (e.g., 720 rows), the lock contention may be visible. Consider batching rows into larger tiles.

---

## Call Graph (Hot Path)

```
main loop
 ├── ShadowPostProcess
 │    ├── IntersectAnyBBox → RayBoxItersect (scalar loop over all objects)
 │    ├── IntersectBBoxColor → RayBoxItersect
 │    ├── BlurColorBuffer
 │    └── BlurBuffer
 │
 ├── RayTraceScene
 │    ├── Frustum_FromCamera
 │    ├── Frustum_TestAABB (cull per object)
 │    └── for each row:
 │         ├── poolAdd → RayTraceRowFunc
 │         │    ├── (for each pixel) ComputeRayDirection
 │         │    ├── (for each frustum-passed object)
 │         │    │    ├── rayAABB_inv (scalar world AABB test)
 │         │    │    └── IntersectBVH (per-object BVH traversal)
 │         │    │         ├── rayAABB_inv_x2_soa (SSE, per node)
 │         │    │         └── rayTriangle (Möller–Trumbore, per leaf triangle)
 │         │    ├── InverseTransformPointTRS
 │         │    ├── calculateUvCoordinates/calculateUvCoordinatesForTriangle
 │         │    ├── SampleSkybox
 │         │    ├── Float3_Normalize, Float3_Reflect, Float3_Dot, Float3_Cross
 │         │    ├── hdrToLDR, PackColor
 │         │    ├── (every N columns)
 │         │    │    ├── rayCollision (shadow ray) → IntersectBVH_Shadow
 │         │    │    ├── SampleEmission (→ BVH traversal for each emitter)
 │         │    │    └── RayCast (reflection ray) → IntersectBVH + material
 │         │    └── (row-wide box blur on accumulated arrays)
 │         └── poolWait
 │
 ├── applySkybox
 │    └── for each row:
 │         ├── poolAdd → SkyBoxTaskFunc
 │         │    ├── ComputeRayDirection
 │         │    └── SampleSkybox (+ blend with framebuffer)
 │         └── poolWait
 │
 ├── SSRPostProcess
 │    ├── memcpy (framebuffer snapshot)
 │    └── for each row:
 │         ├── poolAdd → SSRRowTask → SSRProcessRow
 │         │    ├── (for each pixel) world→screen transform
 │         │    ├── depth test against scene depth
 │         │    └── bilinear color fetch from tempFramebuffer
 │         └── poolWait
 │
 ├── CloudRenderer_Render (OpenCL, if enabled)
 │    ├── CL_Buffer_Map/Unmap (depth upload)
 │    ├── clSetKernelArg (30×)
 │    ├── CL_Dispatch2D (renderClouds, 8×8 work groups)
 │    ├── CL_Dispatch2D (blur, 128×1 work groups) or clEnqueueCopyBuffer
 │    ├── CL_Dispatch2D (godRays, 8×8 work groups) or CL_Buffer_Fill
 │    └── CL_Finish
 │
 ├── CloudRenderer_Composite (OpenCL, if enabled)
 │    ├── CL_Buffer_Map/Unmap (framebuffer upload)
 │    ├── CL_Dispatch2D (compositeFrame)
 │    └── CL_Buffer_Map/Unmap (framebuffer readback)
 │
 ├── BloomPostProcess
 │    ├── BloomExtract
 │    └── BloomBlur (separable, multi-pass)
 │
 ├── DitherPostProcess
 │    └── (per pixel) Bayer matrix lookup + channel add
 │
 └── VisualizeBuffer (if debug mode)
```

---

## Recommended Optimization Order

### Tier 1 (Highest Impact – SIMD vectorization of hot loops)
1. **Replace scalar world AABB loop in `RayTraceRowFunc` with AVX2 8-wide `rayAABB_invV4_avx2`** (ref: `tests/rayAABB_inv.h`, comment in `ray.c:598`). This would test 8 objects per SIMD instruction instead of 1, directly speeding up the inner per-pixel loop.
2. **Replace scalar world AABB loops in `rayCollision` (lines 351, 368) with SSE `RayBoxIntersectV2` or AVX2 `RayBoxIntersectV4`** from `tests/RayBoxItersect.h`. This accelerates shadow rays and reflection rays.
3. **Vectorize `SkyBoxTaskFunc` pixel loop** (TODO: ray.c:310): sample 4+ pixels at once using SSE/AVX gather or structured loads.

### Tier 2 (Memory & Cache)
4. **Fuse the 9 per-pixel buffer writes**: The ray trace stage writes to 9 separate buffers per pixel. Grouping these into a single struct-of-arrays or writing to a single cache-line-aligned struct per pixel would reduce write bandwidth and improve spatial locality.
5. **Tile the texture access**: The 4096×4096 `colorMap`/`normalMap`/`MaterialMap` cause TLB misses. Implement tiled access (e.g., 128×128 tiles) to exploit spatial locality during row traversal.
6. **Move `tempFramebuffer` snapshot in SSR to use pinned/async copy** or avoid the full copy by double-buffering framebuffer.

### Tier 3 (Algorithmic)
7. **Reduce shadow resolution further or use temporal reprojection**: ShadowPostProcess already skips frames; make the step adaptive based on distance/camera motion.
8. **Fuse OpenCL kernels**: Combine god rays + composite into a single kernel to avoid extra dispatch and intermediate buffer read/write.
9. **Replace `fmodf` in `RandomFloat` with integer LCG**: The fmodf is ~30 cycles; an integer LCG can be ~4 cycles.
10. **Precompute inverse TRS for world AABB tests**: The `rayAABB_inv` function computes t = mn[i]*invRd[i] - bias[i]. Currently each object's AABB is tested in world space. If we transform rays to a common space, we could batch more.

### Tier 4 (Low Effort)
11. **Remove unused buffers**: `accumulationBuffer`, `tempBuffer_3` (if any), etc. saves memory and allocation time.
12. **Inline color pack/unpack functions**: Replacing function calls with inline macros/math reduces call overhead in tight loops.
13. **Fix `hdrToLDR` double-call**: Accumulated emission/reflection blend calls hdrToLDR again on an already-tonemapped value; remove one.
14. **Use `__attribute__((always_inline))` on tiny functions** like `Float3_Dot`, `Float3_Cross`, `PackColor`.

---

This report captures the full architecture, all performance-critical paths, known TODOs, and a prioritized optimization plan. The largest gains will come from SIMD-ifying the per-pixel object AABB loop and reducing memory bandwidth from the many per-pixel buffer writes.

---

## Session Insights (2026-05-30 12:48)

**Summary**: Applied divisionless early-out pattern to rayTriangle, achieving 2.0% improvement. Proceeding to apply similar pattern to rayAABB_inv and analyze IntersectBVH and sampleFace.

### Confirmed Wins
  - rayTriangle: divisionless early-out pattern -> 2.0% improvement

### Architectural Insights
  - Scatter/gather overhead renders AVX2 batched AABB ineffective; SIMD batching requires contiguous data.
  - Divisionless early-out pattern is an effective micro-optimization for ray-primitive tests in this codebase.

### Remaining Hotspots
  - IntersectBVH (17.67%)
  - rayAABB_inv (8.29%)
  - rayAABB_inv_x2_soa (5.11%)
  - sampleFace (3.89%)
  - RayBoxItersect (3.65%)

### Techniques to Try
  - Apply divisionless early-out pattern to rayAABB_inv.
  - Analyze and optimize IntersectBVH algorithm.
  - Micro-optimize sampleFace for lower instruction count.

### Techniques to Avoid
  - AVX2 batched AABB for ray-primitive tests due to scatter/gather overhead.

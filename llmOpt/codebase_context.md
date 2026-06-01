# Codebase Architecture Report

## Project Overview
This is a real-time CPU ray tracer with GPU-accelerated cloud rendering, aircraft simulation, client/server networking, and an OpenCL compute backend. The main render loop uses CPU ray tracing for scene geometry and OpenCL for cloud volume rendering, then composites on GPU.

## Directory Structure
- **main.c** — Entry point, game loop, input, scene construction, timing, bench
- **render/** — Render subsystem
  - `render.c` — Rasterizer (RenderObject, unused), vector math with SSE/SSE4.1, TestFunctions
  - `render.h` — Public API
  - `cpu/ray.c` — CPU ray tracer core (~1100 lines): RayTraceScene, RayTraceRowFunc, rayCollision, RayCast, ShadowPostProcess, Dither, SkyBoxTask
  - `cpu/ray.h` — Types: RayTraceTaskQueue, RayHit
  - `cpu/ssr.c` — Screen-space reflections (SSRPostProcess, SSRRowTask) — COMMENTED OUT IN MAIN
  - `cpu/tile.c` — Tile drawing utilities
  - `cpu/font.c` — Bitmap font rendering
  - `color/color.c` — Color packing/unpacking, tone mapping, correction utilities
  - `gpu/format.c` — OpenCL helper wrappers (CL_Buffer_Create, CL_Dispatch2D, etc.)
  - `gpu/kernels/cloadrendering/` — Cloud rendering OpenCL kernels + host code
    - `cload.c` — CloudRenderer_Init/Render/Composite — hosts GPU cloud pipeline
    - `render.cl` — OpenCL kernels: renderClouds, blur, godRays, compositeFrame
- **object/** — Scene objects, geometry, BVH
  - `object.c` — BVH creation/intersection (CreateObjectBVH, IntersectBVH, IntersectBVH_Shadow), rayTriangle, rayAABB variants, Frustum, SampleEmission, CalculateFaceEmissions
  - `object.h` — Object, BVH, BVHNode, EmissionMap, Frustum types, inline rayAABB_inv/rayAABB_inv_x2_soa
  - `format.c` — Camera struct, buffer allocation (16 buffers!), camera movement/rotation
  - `format.h` — Camera, float3, Color, uvMap, constants (WIDTH=1080, HEIGHT=720)
  - `scene.c` — Scene building, ObjectList management, merging
  - `material/` — Material/MaterialLib, Textures (4096x4096 RGBA texture maps)
- **math/** — Inline math: vector3.h, scalar.h, transform.h
- **skybox/** — Skybox loading (JPEG via libjpeg), sampleFace/SampleSkybox
- **load/** — Binary .obj file loader (LoadObj)
- **simulation/** — Aircraft simulation, neural network training (not on hot path)
- **client/** — HTTP client for server communication
- **server/** — HTTP server, game server with interpolated state
- **util/** — threadPool.c/h, bench.h (frame capture + timing), bbox.c, saveImage.c
- **tests/** — Variant benchmarks: rayAABB_inv (SSE/AVX2 versions), rayTriangle variants, testBlur, testSSR, testRay, ObjectBehindCamera

## Build System (Makefile)
- Compiler: clang with -O3 -march=native -mtune=native -flto -ffast-math -funroll-loops -finline-functions -fomit-frame-pointer -mllvm -polly
- PGO: `make pgo` generates profile data, reused on subsequent builds
- Linking: -flto, gc-sections, as-needed
- Profiling: `make flame` produces perf.data + flamegraph.svg + callgraph.svg

## Main Render Loop Call Graph
```
main()  [main.c]
  ├── initCamera() [16 buffers allocated with aligned_alloc]
  ├── Scene setup: CreateCube, LoadObj, CreateObjectBVH, MaterialLib_Init
  ├── benchInit()
  └── while(1):
      ├── getObjects() — HTTP fetch server state (not hot)
      ├── clearBuffers() — *** EMPTY FUNCTION *** (comment says buffers fully written each frame)
      ├── Input_Poll() + camera movement
      ├── Object_UpdateWorldBounds(plane)
      ├── RenderSetup() — camera frustum precomputation
      ├── RayTraceScene() — *** PRIMARY HOT PATH ***
      │   ├── Frustum_FromCamera() — builds 5 frustum planes
      │   ├── Frustum_TestAABB() — cull objects, produces frustumPassIndices[]
      │   ├── poolAdd() × HEIGHT=720 — submit one row task per row
      │   └── poolWait()
      │       └── RayTraceRowFunc() — *** HOTTEST FUNCTION *** [per row, 32 threads]
      │           ├── [per pixel] Compute ray direction
      │           ├── [per pixel] rayAABB_inv() — scalar AABB test against each frustum-pass object
      │           ├── [per pixel] IntersectBVH() — BVH traversal per object hit
      │           │   ├── rayAABB_inv_x2_soa() — SSE dual-AABB BVH node test
      │           │   └── rayTriangle() — Möller-Trumbore (scalar)
      │           ├── [per pixel] Normal/material/lighting computation (GGX specular)
      │           ├── [per pixel] Sky pixels: SampleSkybox()
      │           ├── [per pixel] Texture: InverseTransformPointTRS + calculateUvCoordinatesForTriangle + TBN
      │           ├── [per REFLECTION_RESOLUTION=4 column] rayCollision() for shadow
      │           │   ├── RayBoxItersect() — *** SCALAR, HAS SSE VARIANTS ***
      │           │   └── IntersectBVH_Shadow() — early-out BVH
      │           ├── [per REFLECTION_RESOLUTION column] rayCollision() for reflection
      │           │   └── RayCast() — full resolution (normal + material)
      │           └── [per pixel] Box blur across row (BLUR_RADIUS=3)
      ├── CloudRenderer_Render() — GPU cloud via OpenCL
      │   ├── CL_Buffer_Map() + memcpy [depth upload, ~720KB]
      │   ├── CL_Dispatch2D(renderClouds) — 64-step raymarch
      │   ├── CL_Dispatch2D(blur) — separable 128-wide horizontal blur
      │   ├── CL_Dispatch2D(godRays) — 64-step radial march
      │   └── CL_Finish()
      ├── CloudRenderer_Composite() — GPU composite
      │   ├── CL_Buffer_Map() + memcpy [framebuffer upload, ~3.1MB]
      │   ├── CL_Dispatch2D(compositeFrame)
      │   └── CL_Buffer_Map() + memcpy [framebuffer readback, ~3.1MB]
      ├── RenderText() — FPS overlay
      ├── mfb_update() — window present
      └── benchFrameEnd()
```

## Performance-Critical Data Structures

### Camera (object/format.h:62-97)
- Holds 16 buffers (framebuffer, depthBuffer, normalBuffer, positionBuffer, reflectBuffer, bloomBuffer, bloomTemp, bloomDst, accumulationBuffer, reflectCache, tempFramebuffer, tempBuffer_1/2, shadowCache, objectIdBuffer, uvBuffer, triangleIdBuffer)
- `float3` has `.w` padding — SIMD-friendly size (16 bytes)
- All buffers are 64-byte aligned (aligned_alloc)
- Flow: RayTraceRowFunc → writes framebuffer/depthBuffer/normalBuffer/positionBuffer/reflectBuffer/bloomBuffer → CloudRenderer reads depthBuffer/writes framebuffer → composite blends → mfb_update

### Object + BVH (object/object.h)
- BVHNode 64 bytes (1 cache line): soa[12] (48B) + leftFirst + triCount + _pad[2]
- SoA layout enables SSE dual-AABB test (rayAABB_inv_x2_soa) — 3 load + SIMD min/max
- Cached inverse TRS matrix (_invScale, _invRotSin, _invRotCos) avoids trig per intersection
- Cached forward rotation matrix (_fwdRot0/1/2) avoids trig for normal transforms
- BVH built with median-split, leaves ≤4 triangles

### ThreadPool (util/threadPool.h)
- 32 threads, queue_cap=HEIGHT=720
- Per-task: mutex lock + cond_signal + mutex unlock (3 lock ops per task)
- 720 tasks per RayTraceScene call = 2160 lock/unlock pairs per frame

## Performance-Critical Functions

### 1. RayTraceRowFunc [render/cpu/ray.c]
- **What:** Per-row ray tracing kernel. Processes all pixels in one row.
- **Why slow:** O(pixels × objects × BVH_nodes). For 1080×720 = 777,600 pixels × ~10-50 frustum-pass objects × BVH traversal. Heavy per-pixel work: ray-AABB, BVH, lighting, texture, skybox, sub-ray casting for shadow/reflection.
- **Memory:** Scattered reads from Object array, MaterialLib, Skybox faces. Writes to Camera buffers.
- **Branching:** Heavy — sky vs geometry, material type, texture present, reflection sub-sampling.
- **SIMD missed opportunities:**
  - World AABB test uses scalar `rayAABB_inv()` (line ~608) — TODO comment shows AVX2 8-box version exists in tests/rayAABB_inv.h
  - BVH node traversal uses SSE (rayAABB_inv_x2_soa) — already optimized
  - rayTriangle is scalar — SSE/AVX variants exist in tests/rayTriangle.h
  - Reflection blur loop across row is scalar
- **Calls per pixel:** SampleSkybox (1-2×), IntersectBVH (≥1), rayTriangle (many per BVH), IntersectBVH_Shadow (1 per REFLECTION_RESOLUTION columns)

### 2. IntersectBVH [object/object.c]
- **What:** BVH traversal returning closest triangle hit
- **Why slow:** Recursive/node-stack traversal. For each internal node: 2 SSE AABB tests. For each leaf: N triangle tests (scalar rayTriangle). Stack push/pop on array.
- **Optimizations:** SSE dual-AABB per node, precomputed invDir + bias, early termination via bestT comparison

### 3. IntersectBVH_Shadow [object/object.c]
- **What:** Early-out BVH traversal for shadow rays
- **Why slow:** Same traversal pattern but returns on first hit. Still traverses full tree in worst case.

### 4. rayTriangle [object/object.c:490]
- **What:** Möller-Trumbore intersection
- **Why slow:** Scalar, many divisions, cross products. Called for every triangle in BVH leaves.
- **TODO:** "test different implementations" — 10+ variants exist in tests/rayTriangle.h (V1-V10)

### 5. SampleSkybox [skybox/skybox.c]
- **What:** Cubemap lookup from 6 JPEG textures (front/back/left/right/top/bottom)
- **Why slow:** Called 2× per pixel (direct + reflection). Per call: absolute value × 3, 6 float comparisons, face selection, division, linear-to-pixel mapping, pixel fetch.
- **Optimization opportunity:** Pre-filtered cubemap or bilinear interpolation

### 6. CloudRenderer_Composite [render/gpu/kernels/cloadrendering/cload.c]
- **What:** Upload framebuffer → GPU composite → readback framebuffer
- **Why slow:** 3 GPU-CPU transfers per frame: 2 × upload (~3.8MB total) + 1 × download (~3.1MB). OpenCL queue synchronization. ~6.9MB cross PCIe bus per frame.

### 7. cloud kernels (render.cl)
- **renderClouds:** 64-step raymarch with trilinear density lookup + shadow march (8 steps) + Henyey-Greenstein phase
- **godRays:** 64-step radial march with cloud transmittance occlusion
- **blur:** 128-wide work-group with local memory tile
- **compositeFrame:** Per-pixel blend with transmittance

## Low-Hanging Fruit / TODOs

### Immediate (code already written, just needs integration)
1. **`RayTraceRowFunc` line 598:** Use `rayAABB_invV4_avx2` for 8-box batch AABB test — code and comment already present (commented out block). The scalar loop `rayAABB_inv` for each frustum-pass object is the biggest single optimization opportunity.
2. **`rayCollision` lines 351/368:** Replace `RayBoxItersect` (scalar world AABB) with `RayBoxIntersectV2/V4` SIMD variants — existing in tests/RayBoxItersect.h
3. **`IntersectBVH` line 490:** Test and integrate `rayTriangleNewV10` (likely fastest variant from tests/rayTriangle.h)
4. **`SkyBoxTaskFunc` line 310:** Vectorize skybox sampling for multiple pixels
5. **`SampleEmission` line 756:** Vectorize AABB pre-filter loop

### Threading Overhead
6. **Thread pool locking:** Each poolAdd does lock → signal → unlock. 720 per frame. Consider batching rows (e.g., one task per 4-8 rows) to reduce lock contention.
7. **`frustumPassIndices`** is stack-allocated with `int frustumPassIndices[objectCount]` — VLA on stack. For scenes with many objects this could overflow.

### GPU-CPU Transfer
8. **Cloud depth + framebuffer transfers:** ~6.9MB per frame across PCIe. Consider async transfers, double-buffering, or moving entire rendering to GPU.

### Memory / Cache
9. **Camera has 16 buffers (~200MB total for 1080×720):** Many are rarely used (reflectCache, shadowCache). `clearBuffers()` is empty — relies on ray tracer fully writing every pixel.
10. **Texture maps are 4096×4096 RGBA = 64MB each** (color + normal + material). Only needed when hasTexture is true.
11. **`frustumPassIndices`** computed per frame — could be cached if camera doesn't move much.

### Code Quality / Cleanup
12. **Duplicated math functions:** `render/render.c` has its own `Float3_Add` etc. (static inline) while `math/vector3.h` has the canonical versions. Some inconsistency.
13. **Several post-process passes are commented out** (ShadowPostProcess, SSRPostProcess, DitherPostProcess) but their buffers are still allocated.
14. **`TestFunctions()`** in render.c is benchmarking code that should be in tests/.

## Optimization Order Recommendation

1. **Integrate AVX2 8-box AABB batch test** in `RayTraceRowFunc` (the commented-out block at line ~598). This alone could 2-4× the object AABB pre-filter.
2. **Use existing SIMD ray-box intersection** in `rayCollision` (tests/RayBoxItersect.h).
3. **Replace `rayTriangle` with fastest variant** from tests/rayTriangle.h (likely V10).
4. **Batch thread pool tasks** (fewer rows per task) to reduce mutex pressure.
5. **Precompute skybox mipmaps** or cache recent lookups to reduce SampleSkybox cost.
6. **Investigate OpenCL async transfers** to overlap GPU compute with CPU work.
7. **Profile with `make flame`** to identify actual hot lines before further optimization.

## Benchmark Infrastructure
- `make bench` builds with BENCH_MODE, runs 10 seconds, captures frame hashes + timings + frame images (base64) to bench_results.json
- `make flame` runs perf sampling at 99Hz with frame pointer
- `bench/` directory contains micro-benchmarks for individual functions

---

## Session Insights (2026-06-01 15:45)

**Summary**: Targeted top hotspots (IntersectBVH, rayTriangle, RayTraceRowFunc, rayAABB_inv) over 100+ iterations. Only the tmin>=0 correctness clamp in rayAABB_inv was successfully merged. The session revealed the compiler's dominance at instruction-level optimization and the unreliable nature of micro-benchmarking for this multi-threaded codebase, where four out of five promising structural/micro optimizations regressed the real workload due to increased memory pressure and cache contention.

### Confirmed Wins
  - rayAABB_inv / rayAABB_inv_x2_soa: Added missing tmin>=0 clamp for correctness (prevents traversal of nodes behind camera origin) with zero measurable performance regression (p99 tail latency stable/improved within noise, image_mse=0.00).

### Architectural Insights
  - The compiler at -O3 -march=native -flto -ffast-math is highly effective; almost all micro-optimizations (branchless ordering, software prefetch, hoisted loads) in the BVH traversal and triangle intersection loops either regressed or showed no benefit.
  - Single-threaded micro-benchmarks are deeply unreliable for this multi-threaded codebase. Four out of five structurally promising optimizations (AVX2 batch AABB, VLA combine, prefix-sum blur, BVH prefetch) showed clean speedups in micro-benchmarks but regressed the real renderer by 0.3-2.8% due to increased memory pressure, cache eviction, or TLB contention.
  - The remaining large exclusive time in RayTraceRowFunc (~21.6%) is fundamentally structural (branch-heavy pixel loop, blur logic, scatter VLA writes) and cannot be addressed without a significant architectural redesign of the shading pipeline.

### Remaining Hotspots
  - IntersectBVH (17.77% excl, 36.19% incl) – Dominant inclusive hotspot, traversal logic is compiler-optimized.
  - RayTraceRowFunc (21.6% excl) – Structural overhead in pixel loop and blur operations, resistant to targeted micro-optimizations.
  - rayTriangle (16.29% excl) – Triangle intersection self-time, compiler-optimized near-ideally.
  - rayAABB_inv (9.1% excl) – AABB test scalar loop overhead, batched approaches previously regressed.

### Techniques to Try
  - Packet traversal: batching multiple rays through the BVH to exploit coherence and better utilize SIMD/SIMT hardware.
  - Architectural redesign of RayTraceRowFunc: coherent tile-based rendering or deferred-style batch processing to amortize VLA setup and improve cache behavior.
  - Perf event profiling (cache-misses, TLB misses) to structurally validate memory pressure hypotheses before committing to layout changes.
  - Spatial sorting or tighter bounding volume hierarchies to reduce the total intersection workload per ray.

### Techniques to Avoid
  - Micro-optimizations in functions aggressively compiled with -O3 -flto -ffast-math (branchless ordering, software prefetch, hoisted loads in IntersectBVH/rayAABB_inv/rayTriangle).
  - Structural changes to RayTraceRowFunc that increase stack size or working set (e.g., combined VLAs, prefix-sum buffers). These consistently regress the multi-threaded bench due to cache/TLB pressure.
  - AVX2 batch AABB integrations with gather/scatter overhead. Despite strong micro-bench speedups, they regressed the real workload where AABB testing is not the dominant bottleneck.
  - Triangle data layout interleaving (AoS for vertices). Micro-bench showed no measurable improvement (1.00x) and introduces widespread structural plumbing changes.

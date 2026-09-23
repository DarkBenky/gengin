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

## Node map

Session-maintained candidate queue.  One row per hotspot node:
`path:line Func` — flame share from the most recent `make_flame` — status —
hypothesis — what a verifier must check.  Statuses: `untried`,
`tried-failed(<numbers>, <date>)`, `shipped(PR #n)`, `stale` (a fresh profile
contradicts the row).  Work `untried` rows largest flame percent first; keep
this under ~30 rows and prune `shipped` ones.  The percentages below are from
the 2026-09-22 profile — re-profile before trusting them.

- `object/object.c IntersectBVH` — 18.4% excl / 37.4% incl | untried |
  hypothesis: thread the caller's `bestT` into the traversal as the initial
  bound so the early-out fires from the first node (no extra instructions) |
  verify: V1 semantics and every call site's bestT
- `render/cpu/ray.c RayTraceRowFunc` — 20% excl (structural) | untried |
  hypothesis: row-level work balance / blur-loop buffer redesign (the VLA
  version already failed under 32 threads, 2026-09-20) | verify: must be proven
  with a MULTI-THREADED bench — single-thread wins do not transfer
- `object/object.c:490 rayTriangle` — 16.9% excl | untried | hypothesis:
  integrate the fastest variant from tests/rayTriangle.h (V10 was the pick) |
  verify: bit-identical image and still correct under -ffast-math
- `render/cpu/ray.c:598 rayAABB_inv` (8-box frustum batch) — 7.9% excl |
  untried | hypothesis: use `rayAABB_invV4_avx2` for the batch test (the code
  sits commented out at that line) | verify: identical results to the scalar
  loop
- `rayAABB_inv_x2_soa` (BVH node traversal) — 5.8% excl, already SSE | untried
  | verify: resolve the real source with `lsp_definition` first
- `skybox/skybox.c sampleFace` — 4.6% excl (cvttss2si + clamps) | untried |
  hypothesis: reuse the int conversions across the direct+reflection lookups,
  drop clamps where u,v are provably in range | verify: image-risky, needs a
  real-cubemap bench
- `skybox/skybox.c SampleSkybox` — shared-reciprocal variant | tried-failed
  (micro +13%, frame +1.1% avg / -4.3% p99, 2026-09-23) | note: that p99 was a
  single-frame artifact — re-measure before rejecting it again
- `object/object.c IntersectBVH_Shadow` — 2.4% excl / 5.5% incl | untried
- `render/cpu/ray.c:756 SampleEmission` — 1.3% excl / 5.0% incl | untried |
  hypothesis: vectorize the AABB pre-filter loop (fans out into RayBoxItersect
  / RayBoxIntersectV4 / IntersectBVH_Shadow)

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

## Session Insights (2026-09-21)

**Summary**: Fresh `make_flame` on HEAD 138e2e7. Three candidates profiled, micro-benchmarked (AO reduction under real 32-thread contention, sampleFace both ways), and measured with warm interleaved A/B on the full bench. All three REFUTED with measured evidence — no PR this session.

### Fresh profile (HEAD 138e2e7)
  - IntersectBVH 18.7% excl / 37.8% incl, rayTriangle 17.0% excl, RayTraceRowFunc 20.7% excl, rayAABB_inv 8.1%, AO_V2Row_Pixel 8.1% excl / 9.6% incl, sampleFace 3.7%, rayAABB_inv_x2_soa 5.8%, IntersectBVH_Shadow 2.4% excl / 5.6% incl, SampleEmission 1.3% excl / 5.1% incl.

### REFUTED this session (do not retry)
  - **AO_V2Row_Pixel rsqrt reduction.** Replaced `dist = sqrtf(dist2); occlusion += (nd/dist)*(1-dist*invWorldRadius)` with `invd = AO_Rsqrt(dist2)` (SSE rsqrtps + 1 Newton-Raphson) and `occlusion += (nd*invd)*(1-dist2*invd*invWorldRadius)`. Micro-bench (MT, 1280x720, real pool): **1.616x** on the AO function, maxDiff 1.79e-07, 0 pixels > 1e-5. But the full-frame warm interleaved A/B (8 rounds, medians 15.43 vs 15.33 ms) showed **no gain** — the 1.6x micro-bench win did NOT transfer. The make_bench "baseline" had been COLD (19.5 ms vs warm 15.4 ms) which made the first comparison show +19%; the warm A/B kills it. AO is memory-bound (scattered positionBuffer taps); the sqrt latency was hidden behind the tap loads. Same class as the 2026-06-01 "micro-bench wins regress/vanish in 32 threads" lesson.
  - **sampleFace clamp removal.** The 4 clamps in `sampleFace` (skybox/skybox.c:88-89) are provably dead: `u,v = 0.5 + 0.5*(comp/maxAbs)` with |comp|<=maxAbs from SampleSkybox, so u,v in [0,1] exactly and (int)(t*(w-1)+0.5f) lands in [0,w-1]. Micro-bench (4M realistic dirs): **1.289x**, 0 mismatches. But the full-frame warm A/B: only **~1.5%** (medians 15.73 vs 15.50 ms) — below the 3% gate. The face-pixel load dominates; the branch removal doesn't cut the critical path at 3.7%-of-CPU share.
  - **sampleFace roundps (SSE4.1).** Replacing add-0.5+`cvttss2si` with `roundps`-to-nearest-even: micro-bench **1.193x — SLOWER** than the clamp-free version. The scalar `+0.5` + truncate is what clang emits best; explicit roundps adds a vector-op latency. Also note: `roundps` returns a float; `_mm_cvttps_epi32` on it returns an INT vector — do NOT chain `_mm_cvtss_f32(_mm_cvttps_epi32(...))` (converts int→float); cast the rounded float directly.

### Gotchas recorded for next sessions
  - **make_bench baseline can be COLD**: the baseline run was 19.5 ms while every warm re-run is ~15.4 ms — the first make_bench of a session can include JIT/PGO/GPU-alloc warmup. A +19% "improvement" against that baseline is an artifact. ALWAYS re-seed the baseline warm (stash change, make_bench, pop) and/or run a manual warm interleaved A/B before trusting a make_bench delta > ~3%.
  - The magic-number rsqrt (0x5f35b5bu trick + 1 NR) is NOT safe under -ffast-math: it diverges (NaN/inf) at the low end of the AO dist2 range (x=0.1681) because the compiler reassociates the NR expression. If an approximate rsqrt is ever needed, use `_mm_rsqrt_ps` (hardware) + NR, not the magic float-bit trick.
  - `_MM_FROUND_TO_NEAREST_EVEN` needs the define `0x00` if immintrin.h doesn't expose it under the LSP's plain flags (the real build with -march=native is fine).

### Status: sandbox clean, no PR. Remaining hotspots (IntersectBVH/rayTriangle/RayTraceRowFunc/rayAABB_inv) remain compiler-saturated per prior sessions; the out-of-scope OpenCL cloud pass dominates the frame and dilutes any further CPU-side win.

---

## Session Insights (2026-09-22)

**Summary**: Fresh `make_flame` on HEAD 138e2e7. Found and removed per-pixel dead code in `RayTraceRowFunc`: writes to `camera->uvBuffer` (via `calculateUvCoordinates`, 1.1% excl in profile), `camera->triangleIdBuffer`, and `camera->motionVectorBuffer` (transform block ~3.4% of CPU per 2026-09-20) — all three buffers are write-only (grep-verified repo-wide: no .c/.h/.cl reader, no CL upload, bench hashes only the framebuffer). Also removed the now-dead per-row `motPrevRot[objectCount][9]` table, the hoisted prev-camera normalize block, and the sky-pixel zero-writes (row + column twins). Micro-bench (bench/deadpix): full block 0.02 ns/pixel measured degenerate; the real gate is the frame bench. Adversarial review PASS (all framebuffer/depth/reflect/bloom/normal/position/objectId writes byte-identical). make_bench: avg 19.471 -> 15.252 ms, p99 26.503 -> 18.559 ms, image_mse 0.00, all 10 frame hashes match. PR opened.

### Confirmed Wins
  - Dead per-pixel buffer writes in RayTraceRowFunc (ray.c:700-988 region): removed calculateUvCoordinates call, triangleIdBuffer write, motion-vector transform block, motPrevRot table build (30 trig/row), and prev-camera hoisted normalizes. No image change (MSE 0.00, hashes match). The baseline was COLD (19.47 ms vs warm ~15.2-15.4 ms) so the +21.7% avg is inflated; the true win is the removed CPU share (~4.5% of frame CPU per fresh profile: calculateUvCoordinates 1.1% + motion vector block ~3.4%).

### REFUTED / artifacts this session (do not retry)
  - **`Float3_Dot` 1.7% excl in the flame graph is a PROFILE ARTIFACT.** `make flame` builds `main_flame` with `-fno-inline-functions -fno-lto`, so the 2-line `Float3_Dot` in math/vector3.h appears as out-of-line calls (52 call sites, spills in the AO loop). The real binary inlines it everywhere. Do not "fix" this.
  - Leftover `bench/uvtrs.*` from a prior session is broken (uv_cached validation fails 657/200000: it precomputes d00/d01/d11 per-triangle from T_V0/V1/V2 but validates per-pixel where triangles repeat; timing also degenerates to ~0 ns) — deleted.

### Gotchas for next sessions
  - `run_func_bench` builds via `make build/bench/<name>` which does NOT depend on the .h header — after editing a bench header, `rm -f build/bench/<name>` first or you benchmark a stale binary.
  - Bench micro-loops that accumulate into a non-volatile global get eliminated by LTO (prints 0.00 ns) — make the sink `volatile`.
  - The flame build (no-inline) misattributes inlined inline math to out-of-line symbols (Float3_Dot 1.7%, [unknown] 2.7%); cross-check any "un-inlined function" hotspot against the normal binary's symbols before investing.
  - `hot_annotate_func` resolves the first textual definition, which can be a test-file copy (IntersectBVH resolved to tests/testSSR.c) — use `hot_annotate_file` for the real source.

### Status: sandbox clean after PR. Remaining hotspots unchanged: IntersectBVH 18.4% excl / 37.4% incl, rayTriangle 16.9%, RayTraceRowFunc 20% excl (structural), rayAABB_inv 7.9%, rayAABB_inv_x2_soa 5.8% — all compiler-saturated per prior sessions.

---

## Pending Work (archived 2026-09, from the old planner board)
- Next planned experiment: a multi-threaded micro-benchmark for the RayTraceRowFunc blur loop (current stack-VLA version vs pre-allocated global buffers). Single-threaded wins regressed in the 32-threaded renderer, so any blur redesign must be proven under thread contention before integration.
- Open task list from the last session (all unstarted): read RayTraceRowFunc blur code + BlurBuffer + Camera struct; build the MT micro-benchmark; apply global-buffer blur if proven; build + make_bench (avg_ms, p99, image_mse); PR on success.
- The sandbox working tree carries a leftover `tmin-clamp-optimization` branch with uncommitted changes to render/cpu/ray.c and extra bench files; `git_pull_project` discards it.

---

## Session Insights (2026-09-18)

**Summary**: Fresh `make_flame` profile surfaced `CalculateAmbientOcclusionV2Row` (render/cpu/AO.h) as a NEW top-4 hotspot (9.6% excl, 10.9% incl) that prior sessions never touched. Benchmarked 3 variants in bench/aoRow (MT, real thread pool, 1080x720): pre-rotated pattern table (+3.4%), interior/border loop split (+4.1%), combined (+11.4% micro-bench). Applied the combined variant and it SURVIVED the real 32-thread bench: avg_ms 17.85 -> 15.53 (+13.0%), p99 26.97 -> 18.91 (+29.9%), image_mse = 0.00, all frame hashes match. PR opened.

### Confirmed Wins
  - CalculateAmbientOcclusionV2Row: (1) pre-rotated sample pattern table AO_PRE_ROT[ROTATIONS][VARIATIONS][SAMPLES] built once in CalculateAmbientOcclusionV2Mp with the same float expression (drops 4 mul + 2 add per sample); (2) interior/border split: pixels inside a 64 px margin skip the 4-compare bounds check (guarantee: max |pattern| = 1.243 x pixelRadius<=48 = 59.7 px < 64). perf stat: -20.4% instructions, -12.3% cycles, cache-misses unchanged -> pure compute reduction, zero added memory pressure. Full bench +13.0% avg, image_mse 0.00.

### Why it survived 32 threads (pre-mortem validated)
  - Only +7 KB read-only shared const table (vs ~15 MB buffer working set); no VLAs, no new indirection/gathers, no extra working set; the interior guarantee is scene-independent (pure geometry), so it transfers to any scene. Same lesson as the blur-loop pending item: zero-memory-pressure compute reductions transfer; anything touching memory layout does not.

### Gotchas recorded for next sessions
  - The interior split MUST be gated on `width >= 2*AO_MARGIN` (not just rows): for width < 128 the column spans go negative/overlap and corrupt the previous row's pixels (adversarial review found this with a differential test at 50x200; latent bug, never triggers at 1080x720).
  - "Bit-identical" is approximate: the combined variant lets clang vectorize the 8-sample loop incl. the occlusion reduction (fast-math reassociation), so <=2 ulp diffs on ~10% of pixels (max 1.19e-07). Image-level result: image_mse 0.00.
  - The AO init flag in CalculateAmbientOcclusionV2Mp is safe only because that function is main-thread-only (verified: single call site ray.c:1099 + test mains).
  - perf-stat note: `run_perf_stat` runs the bench binary WITHOUT args, so counters aggregate the whole suite; for per-variant counters run `perf stat ... build/bench/<name> <variant>` manually.
  - bench/ files were deleted after validation per workflow (AO bench harness lives on in tests/AOBench.c, which exercises the real edited path).

### Remaining Hotspots (fresh profile, post-AO)
  - IntersectBVH 18.2% excl / 36.8% incl, rayTriangle 16.5% excl, RayTraceRowFunc 17.2% excl (structural), rayAABB_inv 7.5%, rayAABB_inv_x2_soa 5.7%, sampleFace 4.6% (fp->int conversion dominated), IntersectBVH_Shadow 2.4% excl / 5.5% incl, SampleEmission 1.3% excl / 5.0% incl.
  - sampleFace hot lines are cvttss2si conversions + clamps; candidates: reuse (int) conversions across direct+reflection lookups, clamp-free face indexing if u,v provably in range. Image-risky, needs bench with real cubemap.
  - SampleEmission (incl 5.0%) fans out into RayBoxItersect + RayBoxIntersectV4 + IntersectBVH_Shadow (~2.4% total in those calls); the AABB pre-filter loop there is the vectorization candidate the old TODO list names.

---

## Session Insights (2026-09-20)

**Summary**: Fresh `make_flame` on HEAD d077fad. Three candidates micro-benchmarked; two REFUTED with measured evidence, one applied and validated. Applied: the per-pixel motion-vector transform in `RayTraceRowFunc` no longer calls `InverseTransformPointTRS` + `TransformPointTRS` (12 `sinf`/`cosf` per geometry pixel) — it now uses the rotation rows `Object_UpdateWorldBounds` already caches plus per-row previous-rotation rows. Micro-bench 5.94x on the block (27.58 -> 4.64 ns/pixel), full bench avg 15.661 -> 15.558 ms (+0.7%, noise), p99 20.513 -> 19.921 ms (+2.9%), image_mse 0.00, all 10 frame hashes match. PR opened.

### Confirmed Wins
  - Motion-vector transform in RayTraceRowFunc (ray.c:932): replaced InverseTransformPointTRS + TransformPointTRS (each 6 sinf/cosf; these take a raw `rotation` float3, NOT the cached matrix) with (a) world->local via `obj->_invScale/_invRotSin/_invRotCos` (the same rows IntersectBVH uses) and (b) local->prev-world via `motPrevRot[objectCount][9]`, the previous-frame forward rotation rows built once per row task (~30 trig per row instead of ~13k). Bench: 5.94x on the block, max output diff 3.8e-06 (motionVectorBuffer is read by nothing, so image is bit-identical: MSE 0.00, all frame hashes match).

### REFUTED this session (do not retry)
  - **rayTriangle eager division.** Hypothesis: `f = 1.0f/a` is computed before any barycentric reject, so every rejected triangle pays a reciprocal. Deferred-division + sign-normalised rejects (V2) measured **0.977x**, branchless accept/reject (V3) **0.784x**. The reciprocal is fully hidden by OoO overlap. Confirms "rayTriangle is compiler-optimized" from 2026-06-01.
  - **Per-pixel object AABB pre-filter (ray.c:684 loop) redesign.** Hypothesis: staged SoA + 4-wide SSE slab filter with a single `movemask` per group beats the scalar loop. Measured **0.999x** (staged scalar) and **0.998x** (SSE4) vs the original, all at 3.60 ns/pixel = ~3 cycles per object test. The 5 independent slab tests already pipeline perfectly; the loop is NOT compute-bound and staging/SIMD cannot help. Also proves the loop is only ~1.5% of frame CPU despite `hot_annotate` showing 11.8% on that line.

### Gotchas recorded for next sessions
  - **`hot_annotate_func` percentages are relative to the annotated function's own sample base, not to total samples.** Within RayTraceRowFunc the thresholded lines sum to ~34% while the function's exclusive share is 17.3% — the line % already includes samples of inlined callees. Do NOT read a line % as a global frame share; always cross-check with a micro-benchmark before investing in a rewrite.
  - The per-frame pixel loop exists TWICE: `RayTraceRowFunc` (ray.c:577-1117, hot, row-based) and a column-based twin (ray.c:~1100-1600). `patch` needs extra context to disambiguate; only the row version is on the hot path (it is what `make flame` attributes to).
  - Any per-pixel `InverseTransformPointTRS`/`TransformPointTRS` call is a trig trap: `math/transform.h` re-derives sin/cos from the raw `rotation` float3. The cached rows only exist on `Object` (`_invScale/_invRotSin/_invRotCos`, `_fwdRot0/1/2`); there is no cached *previous-frame* matrix, hence the per-row table.
  - The frame is now dominated by the out-of-scope OpenCL cloud path (renderClouds/godRays/composite on the RTX 3090), which dilutes CPU-side wins: a 5.94x micro-bench win on a ~3.4%-of-CPU block moved avg_ms by only 0.7%. Expect small frame deltas from pure CPU compute reductions until the GPU passes are addressed.
  - Baseline: `make_bench` refused to run because `baseline_cache.json` was keyed to the pre-AO SHA `2ac0475` while HEAD was `d077fad`. Established a legitimate baseline by `git stash push -- render/cpu/ray.c` (tree then clean at the pinned HEAD; untracked files and `deps/cute_headers` do not count as dirt — see `_projectGitHead()` which passes `--untracked-files=no --ignore-submodules=dirty`), running `make_bench` to seed the cache, then `git stash pop`.

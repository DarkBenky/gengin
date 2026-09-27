# OpenCL Rasterizer — Alternative GPU Rendering API ( + Hybrid notes)

Status: PLANNED · Created 2026-09-27 · Links from README task "Crate open cl rendering kernels for
alternative API for rendering and Hybrid approach"

Goal: a self-contained OpenCL rasterizer API that renders the scene geometrically identical to the CPU
ray tracer (`RayTraceScene`) and color/texture-wise as close as the "not fancy" subset allows. Integration
(toggle, near/far split, AO policy) is caller-owned. Hybrid compositing is a documented future stage; the
API is designed so it stays possible (view-Z depth, same G-buffer outputs).

## 0. TL;DR

- The algorithm already exists: `RenderObject` (render/render.c:301, unused) is a full CPU rasterizer
  (barycentric coverage + depth test against camera->depthBuffer + G-buffer writes). The CL port is a
  1:1 translation of its pixel math (but NOT its transform order, see P1).
- All inputs are upload-ready: Object holds per-triangle SoA arrays (v1/v2/v3/normals/UvCords/materialIds),
  rotation rows are cached per frame (Object._fwdRot0..2), textures are plain uint32 nearest-sampled maps.
- CL plumbing exists + living template: render/gpu/format.h + CloudRenderer (cloadrendering/).
- Deliverable: `render/gpu/kernels/rasterizer/` = raster.cl + raster.{h,c} with a small API:
  Create/UploadScene/Render/Destroy. 3 kernels: clear, depth-scatter, shade-scatter.
- Biggest risks are correctness traps: transform order, float atomics, payload races, texture sampling
  exactness, depth-buffer consumers — full checklist below.

## 1. Scope

### In (this work)
- New module `render/gpu/kernels/rasterizer/` implementing the alternative GPU rendering API.
- Static scene upload (geometry, materials, later textures), per-frame TRS + camera upload.
- Renders into caller-provided `Camera` G-buffers (framebuffer, depth, normal, position; optional
  objectId/triangleId/uv) — same formats and units as the ray tracer.
- Offscreen verification harness (tests/) comparing raster vs ray tracer from the same camera.

### Caller-owned (user integrates)
- Where/when to call it, toggle UX, near/far split policy, AO handling for raster pixels, cloud/depth interplay.

### Out (deliberately)
- Shadows, reflections, emissive indirect light, GGX/fresnel, bloom on the raster path.
- Skybox on GPU (raster v1 leaves sky pixels to the caller / RT; see P19).
- Transparency, FSR, atmosphere.

## 2. Ground truth to copy (exact references)

| What | Where | Notes |
|---|---|---|
| world = R·(S·v)+T | object/object.c:828-830 | scale applied FIRST, then rotation, then translation |
| R rows Rz·Ry·Rx cached | object/object.c:239-245 | Object._fwdRot0/1/2 — upload, no trig on GPU |
| projection + screen map | render/render.c:337-360 | z=dot(vCam,fwd); xNdc=dot(vCam,right)/(z·fovScale·aspect); yNdc=dot(vCam,up)/(z·fovScale); sx=(x+1)·0.5·W+jitter.x; sy=(1-y)·0.5·H+jitter.y |
| coverage+depth+G-buffer | render/render.c:395-490 | edge fns, area sign, invZ=(w0·invZ0+w1·invZ1+w2·invZ2)·invArea, depth=1/invZ |
| Camera fields | object/format.h:72 | position/right/up/forward/fovScale/aspect/jitter/screenW/H/framebuffer/depthBuffer/normalBuffer/positionBuffer/reflectBuffer/objectIdBuffer/triangleIdBuffer/uvBuffer |
| RenderSetup (right/up/aspect/fovScale) | render/render.c:494 | must run before raster (main.c:272) |
| depth semantics | ray.c framebuffer write ~925 | depthBuffer = dot(hitPos-orig, fwd) = view Z positive float |
| texture uv math | render/cpu/ray.c:550-575 | barycentric over uint16 uv, u·(TEXTURE_SIZE-1)/65535, truncate |
| texture unpack+boost | render/cpu/ray.c:774-815 | nearest colorMap[y][x]; &0xFF→red, >>8, >>16; ×1.25 boost |
| base lighting | render/cpu/ray.c:860-915 | lit=(0.02+0.98·max(0,n·lightDir))·(1-metallic); hdrToLDR(ray.c:27); pack 0xFF<<24|r<<16|g<<8|b |
| default material | render/cpu/ray.c:469 | {0.8,0.8,0.8, rough 0.8} when materialId invalid |
| model bin layout | tools/parseObj.go:843-931 | 108 B/tri: float4 v1,v2,v3,normal + rough/met/emis + color+pad + 6×uint16 uv + pad |
| materials/textures | object/material/material.h | TEXTURE_SIZE 4096; colorMap/normalMap uint32, MaterialMap uint16 |
| CL API + template | render/gpu/format.h; cloadrendering/cload.c | pinned buffers + Map/Unmap + SetArg + Dispatch |
| frame hooks (reference only) | main.c:272 RenderSetup, :279 RayTraceScene, :299 CloudRenderer_Render (uploads cam->depthBuffer!), :318 composite, :339 present; clearBuffers (:239) NO-OP |
| jitter | main.c:242 | ±1.5 px 4-frame pattern; apply same in raster |

## 3. Architecture

### 3.1 static upload (Raster_UploadScene)
64 B/tri record: float4 v1,v2,v3,n; uint uv01,uv23,uv45; int materialId.
Buffers: triBuf, objectOfTri (int per tri), material table {color, roughness, metallic, emission,
textureSlot}. Textures (phase 2): concatenate all colorMaps into one buffer + offset table; nearest only.

### 3.2 per-frame upload (Raster_Render)
- camera as individual kernel args (cloud-renderer pattern): position/right/up/forward/fovScale/aspect/
  jitter(float2)/screenW/H/lightDir.
- GpuObject buffer: {float3 position; float3 scale; float4 rot0,rot1,rot2} from Object.position/scale/
  _fwdRot0..2. Upload all every frame (tiny). Caller must have run Object_UpdateWorldBounds / RenderSetup.

### 3.3 kernels (raster.cl)
1. clear: CL_Buffer_Fill depthU=0xFFFFFFFF (+ objectId=-1 if used).
2. rasterizeDepth: 1D over triangles; world=R·(S·v)+T; project (+jitter, P5/P6); reject all-behind/
   degenerate (P3/P4); bbox; edge coverage; enc=as_int(z) (z>0 monotonic); atomic_min on int-cast depthU (P7).
3. rasterizeShade: same scatter; write only where enc==depthU[idx] (P8): depthBuffer (float view Z),
   normalBuffer, positionBuffer (perspective-correct), framebuffer (shaded), optional objectId/triangleId/uv.

### 3.4 API surface (raster.h)
```c
typedef struct Raster Raster; // opaque; owns its CL_Context + buffers
void Raster_Init(Raster *r, const char *kernelPath, int width, int height);
void Raster_UploadScene(Raster *r, const ObjectList *scene, const MaterialLib *lib); // after scene build/merge
void Raster_Render(Raster *r, const ObjectList *scene, const MaterialLib *lib, Camera *cam); // per frame
void Raster_Destroy(Raster *r);
```
Notes: writes directly into cam->{framebuffer,depthBuffer,normalBuffer,positionBuffer}; readback via pinned
map (CL_Buffer_CreatePinned + CL_Buffer_Map — project rule: pinned, no pageable staging). No per-frame
allocations. `Raster_Render` is synchronous (CL_Finish inside) — async variant is a phase-4 item.

### 3.5 Integration example (caller-owned; doc snippet only)
```
Raster_Init(&raster, "render/gpu/kernels/rasterizer/raster.cl", WIDTH, HEIGHT);
... after scene build: Raster_UploadScene(&raster, &scene, &matLib);
... per frame, after RenderSetup(...): Raster_Render(&raster, &scene, &matLib, &camera);
```
Caveats to document: run after RenderSetup; if the cloud pass runs, it uploads cam->depthBuffer from CPU,
so raster must run before it (or the depth readback must have happened); toggle UX is the caller's choice.

### 3.6 Hybrid (future stage; keep compatible)
Both writers share camera->depthBuffer (view Z). Order A (no ray.c change): RT first, raster after with
strict depth test. Order B (savings): raster first, RT only for a caller-chosen subset with
`if (hitZ >= depthBuffer[idx]) skip` guard in ray.c. Needs a subset list (caller-owned; no Object struct
change without explicit user approval) and AO/consumer decisions. The API staying view-Z + G-buffer-
compatible is the only requirement from this phase.

## 4. Shading parity (phase 2)
base = texture ? texel*1.25 : mat.color; ndl=max(0,n·lightDir); lit=(0.02+0.98*ndl)*(1-metallic);
rgb=hdrToLDR(base*lit); pack 0xFF000000|r<<16|g<<8|b.
Deliberately skipped: specular/fresnel/GGX, sky reflection, shadows, emission add, reflections.
Optional cheap add-ons: + base*emission; Blinn spec with (1-roughness)*128+1.

## 5. Pitfalls & fixes (checklist)

P1 transform order: RenderObject uses S·R, ray tracer R·(S·v)+T — use the ray tracer order; unit-check.
P2 normals: rotate only with R (match CPU, even though not textbook inverse-transpose).
P3 backface culling: RT is two-sided; v1 no culling (match), cull later as a flag.
P4 near-plane: skip if ANY vertex z<0.01 in v1 (RenderObject only skips all-behind); clip later.
P5 jitter: apply camera.jitter in screen mapping to match RT subpixel phase.
P6 ordering: raster must run after RenderSetup (right/up/aspect/fovScale).
P7 no float atomic_min in CL 1.2 → encode z>0 as as_int bits (monotonic), atomic_min on int.
P8 payload race → two-pass (depth-only then shade where enc==depthU); pass 2 recomputes bit-identical z.
P9 clearBuffers no-op → clear depthU via CL_Buffer_Fill every frame.
P10 depth units: write view Z (1/invZ), same as ray.c.
P11 consumers: CloudRenderer uploads cam->depthBuffer each frame; AO consumes normal/position/depth →
    readback depth (and optionally normal/position) with the framebuffer (pinned).
P12 texture sampling: nearest + exact unpack (&0xFF→red etc.) + ×1.25 + clamp [0,4095]; no filtering.
P13 UV: perspective-correct barycentrics then exact ray.c formula (uint16 truncation).
P14 textures 64 MB each → one concatenated buffer + offsets; upload only textured materials.
P15 empty dispatch guard (triCount==0).
P16 atomic contention measure first; fallbacks: k tris/item or tile-binned z-buffer (phase 4).
P17 materialId -1 → default material; skip objects with v1==NULL.
P18 ObjectList_Merge bakes transforms → Raster_UploadScene hook after rebuilds/merges.
P19 sky pixels: raster writes covered pixels only; caller decides (clear color / keep RT sky).
P20 fancy effects missing — expected diffs on emissive/mirror/shadow regions.
P21 no -cl-fast-relaxed-math for parity runs (NULL options like CloudRenderer).
P22 add a WNOW phase when integrated; keep the >10% regression rule; optional bench (testRayColumnBench pattern).
P23 hdrToLDR copy + alpha byte 0xFF (mfb/composite assume it).

## 6. Phases
Phase 1 — API core: raster.cl (clear/depth/shade, material color), raster.{h,c}, offscreen parity harness.
Phase 2 — textures + material table + base lighting.
Phase 3 — (caller-owned) integration; docs/examples; hybrid notes.
Phase 4 — extras: normal maps, specular, culling flag, tile binning, GPU AO/skybox, async render variant.

## 7. Verification harness
tests/rasterParity.c (pattern: tests/testRay.c): same camera → RT vs raster dumps (saveImage.h) + depth
grayscale dumps + diff stats; llmOpt/image_compare.py for image diffs. Standalone, no main.c changes needed.

## 8. File map
New: render/gpu/kernels/rasterizer/raster.cl, raster.c, raster.h; tests/rasterParity.c.
Later (caller-owned): main.c integration; render/cpu/ray.c hybrid guard.
Docs: todo/opencl-raster-hybrid-plan.md; README link.

## 9. Open questions (remaining)
1. Sky in raster mode: caller's problem, or should the API take an optional clear color / GPU skybox later?
2. Textures scope: colorMap only, or also MaterialMap (roughness/metallic), normalMap last?
3. Sun direction: camera->lightDir (ray tracer) or camera->renderLightDir (RenderObject)? Recommend RT's.
4. Output target: write into Camera G-buffers directly (recommended) vs caller-provided buffers?
5. Pure-raster offscreen mode wanted for benchmarks/screenshots, or always render into Camera buffers?

## 10. Reference index (as of 2026-09-27)
render/render.c:301,337-360,395-490,494 · object/object.c:233-245,828-830 · ray.c:27,469,515-575,774-815,
860-915,925-940,1137 · object/format.h:8-9,72 · object/object.h · object/material/material.h ·
render/gpu/format.h · cloadrendering/cload.c · main.c:239,242,272,279,299,318,339 ·
tools/parseObj.go:843-931 · load/loadObj.c

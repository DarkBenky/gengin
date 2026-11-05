// OpenCL rasterizer — two-pass (depth scatter, then shade scatter) triangle raster.
// Pixel math is a 1:1 port of the CPU reference in render/render.c (RenderObject) with the
// ray tracer's transform order world = R*(S*v)+T, and the shading terms of
// render/cpu/ray.c that need no extra geometry: base lighting, normal maps, the GGX
// specular and the sky reflection. Shadows come from a light-space depth map (the cheap
// stand-in for ray.c's shadow rays) and screen-space ambient occlusion from AO.h.
//
// The record layouts below must match RasterTri / RasterObject / RasterMaterial /
// RasterTexture / RasterShadow in raster.h — the kernel is compiled from this source
// string, so the structs are duplicated instead of shared.

#define RASTER_FLAG_TEXTURE 1
#define RASTER_FLAG_CULL 2

// A triangle is rejected when any vertex is closer than this (no near-plane clipping yet).
#define NEAR_Z 0.01f
// Degenerate screen area (matches the CPU reference).
#define MIN_AREA 1e-8f

// render/cpu/ray.h: tangent-space normal map intensity.
#define NORMAL_MAP_STRENGTH 2.0f

// ray.c darkens a shadowed pixel with 0.5 + 0.5*shadow, i.e. 50% at full shadow.
#define SHADOW_AMBIENT 0.5f

// The depth pass packs (depth bits, triangle id) into one 64-bit word and resolves it with
// a single atomic_min, so the shade pass can read the winning triangle per pixel instead of
// re-rasterizing every triangle.
#pragma OPENCL EXTENSION cl_khr_int64_base_atomics : enable
#define NO_HIT 0xFFFFFFFFFFFFFFFFul

// render/cpu/AO.h: sample count, world radius and the depth-reject scale of the blur.
// The CPU reaches out to 20.5 world units, which on this scene is wider than the objects:
// at grazing angles a screen-space sample that far away lands on unrelated geometry and
// the estimate turns into a flat dim over the whole floor. Keeping the reach to roughly
// one object size confines the term to the contact points, which is what reads as a shadow.
#define AO_SAMPLES 8
#define AO_WORLD_RADIUS 6.0f
#define AO_DEPTH_REJECT_SCALE 0.02f
#define AO_GOLDEN_ANGLE 2.39996323f
#define AO_BIAS2 0.0144f							  // (worldRadius * 0.02)^2
#define AO_RADIUS2 36.0f							  // worldRadius^2
#define AO_INV_RADIUS 0.16666667f					  // 1 / worldRadius
// Cap on the screen-space sample spread. Without it a grazing surface pulls samples from
// far across the frame and the estimate collapses; 24 px still covers a contact shadow.
#define AO_MAX_PIXEL_RADIUS 24.0f
static constant float AO_KERNEL[5] = {				  // lineKernelvaluse
	0.054488685f, 0.244201342f, 0.402619947f, 0.244201342f, 0.054488685f};

// object/format.h: the value the host fills the depth buffer with before the pass.
#define DEPTH_FAR 1e30f

// Skybox faces in the order Raster_SetSkybox uploads them.
#define SKY_RIGHT 0
#define SKY_LEFT 1
#define SKY_TOP 2
#define SKY_BOTTOM 3
#define SKY_FRONT 4
#define SKY_BACK 5

typedef struct {
	float4 v1;
	float4 v2;
	float4 v3;
	float4 normal;
	uint uv01; // uv1.x | (uv1.y << 16)
	uint uv23;
	uint uv45;
	int materialId;
} RasterTri;

typedef struct {
	float4 position;
	float4 scale;
	float4 rot0; // rows of Rz*Ry*Rx, cached per frame by Object_UpdateWorldBounds
	float4 rot1;
	float4 rot2;
	int triStart;
	int triCount;
	int flags;
	int sceneIndex; // index into the caller's ObjectList, echoed into objectIdBuffer
} RasterObject;

typedef struct {
	float4 color;
	float roughness;
	float metallic;
	float emission;
	int textureSlot; // -1 = untextured
} RasterMaterial;

typedef struct {
	int offset; // first texel of this map inside the concatenated map buffers
	int size;   // edge length
} RasterTexture;

// One emissive object reduced to a point light. ray.c samples the emitter's per-face
// EmissionMap and rejects the contribution with a shadow ray; a point light with no
// occlusion is the cheap stand-in - it keeps the inverse-square pool of light the emitters
// throw on the floor, and only leaks through geometry that sits between the two.
typedef struct {
	float4 position;
	float4 colorEmission; // xyz = material color * material emission
} RasterEmitter;

// Orthographic light-space frame used by the shadow map. The reciprocals and the box
// minimum are packed into .w so the shade kernel needs one argument instead of five.
typedef struct {
	float4 center; // xyz = centre of the scene bounds
	float4 right;  // xyz = basis right,   w = 1/halfWidth
	float4 up;     // xyz = basis up,      w = 1/halfHeight
	float4 fwd;    // xyz = light forward, w = min depth along it
	float4 params; // x = map size, y = world units per texel, z = 1 when enabled
} RasterShadow;

static float edgeFunction(float ax, float ay, float bx, float by, float cx, float cy) {
	return (cx - ax) * (by - ay) - (cy - ay) * (bx - ax);
}

// 'vertex' not 'local' — local is a reserved address-space keyword in OpenCL C.
static float3 toWorld(float3 vertex, float3 scl, float3 position,
					  float4 rot0, float4 rot1, float4 rot2) {
	float3 s = (float3)(vertex.x * scl.x, vertex.y * scl.y, vertex.z * scl.z);
	return (float3)(
		dot(rot0.xyz, s) + position.x,
		dot(rot1.xyz, s) + position.y,
		dot(rot2.xyz, s) + position.z);
}

// Per-triangle setup shared by both passes: world verts, screen verts, incremental
// edge-function deltas and the screen bounding box. Returns 0 when the triangle
// contributes no pixel (behind the near plane, degenerate, culled, off screen).
typedef struct {
	float3 w1, w2, w3; // world-space verts
	float3 nrm;		   // world-space face normal
	float sx0, sy0, sx1, sy1, sx2, sy2;
	float invZ0, invZ1, invZ2;
	float areaSign, invArea;
	// edge-function deltas: e0 = edge(v2,v3), e1 = edge(v3,v1), e2 = edge(v1,v2)
	float e0dx, e0dy, e1dx, e1dy, e2dx, e2dy;
	int minX, minY, maxX, maxY;
} TriScan;

static int setupTriangle(__global const RasterTri *tri, __global const RasterObject *obj,
						 float3 camPos, float3 right, float3 up, float3 fwd,
						 float fovScale, float aspect, float2 jitter,
						 int screenWidth, int screenHeight, TriScan *scan) {
	float3 scl = obj->scale.xyz;
	float3 position = obj->position.xyz;

	scan->w1 = toWorld(tri->v1.xyz, scl, position, obj->rot0, obj->rot1, obj->rot2);
	scan->w2 = toWorld(tri->v2.xyz, scl, position, obj->rot0, obj->rot1, obj->rot2);
	scan->w3 = toWorld(tri->v3.xyz, scl, position, obj->rot0, obj->rot1, obj->rot2);
	scan->nrm = (float3)(
		dot(obj->rot0.xyz, tri->normal.xyz),
		dot(obj->rot1.xyz, tri->normal.xyz),
		dot(obj->rot2.xyz, tri->normal.xyz));

	float3 c1 = scan->w1 - camPos;
	float3 c2 = scan->w2 - camPos;
	float3 c3 = scan->w3 - camPos;
	float z1 = dot(c1, fwd);
	float z2 = dot(c2, fwd);
	float z3 = dot(c3, fwd);
	if (z1 < NEAR_Z || z2 < NEAR_Z || z3 < NEAR_Z)
		return 0;

	float x1 = dot(c1, right) / (z1 * fovScale * aspect);
	float y1 = dot(c1, up) / (z1 * fovScale);
	float x2 = dot(c2, right) / (z2 * fovScale * aspect);
	float y2 = dot(c2, up) / (z2 * fovScale);
	float x3 = dot(c3, right) / (z3 * fovScale * aspect);
	float y3 = dot(c3, up) / (z3 * fovScale);

	scan->sx0 = (x1 + 1.0f) * 0.5f * screenWidth + jitter.x;
	scan->sy0 = (1.0f - y1) * 0.5f * screenHeight + jitter.y;
	scan->sx1 = (x2 + 1.0f) * 0.5f * screenWidth + jitter.x;
	scan->sy1 = (1.0f - y2) * 0.5f * screenHeight + jitter.y;
	scan->sx2 = (x3 + 1.0f) * 0.5f * screenWidth + jitter.x;
	scan->sy2 = (1.0f - y3) * 0.5f * screenHeight + jitter.y;

	float area = edgeFunction(scan->sx0, scan->sy0, scan->sx1, scan->sy1, scan->sx2, scan->sy2);
	if (fabs(area) <= MIN_AREA)
		return 0;
	// Screen area < 0 is the winding the ray tracer keeps when an object culls backfaces:
	// its Möller-Trumbore test requires dot(rayDir, cross(e1, e2)) < 0, and the flipped
	// screen Y makes that exactly area < 0 (checked against the cube's near and far faces).
	if ((obj->flags & RASTER_FLAG_CULL) && area > 0.0f)
		return 0;

	// Bounding box is only a superset of the covered pixels — coverage itself is
	// decided by the edge functions, so floor/ceil here is enough.
	float minXf = fmin(fmin(scan->sx0, scan->sx1), scan->sx2);
	float maxXf = fmax(fmax(scan->sx0, scan->sx1), scan->sx2);
	float minYf = fmin(fmin(scan->sy0, scan->sy1), scan->sy2);
	float maxYf = fmax(fmax(scan->sy0, scan->sy1), scan->sy2);
	scan->minX = max(0, (int)floor(minXf));
	scan->maxX = min(screenWidth - 1, (int)ceil(maxXf));
	scan->minY = max(0, (int)floor(minYf));
	scan->maxY = min(screenHeight - 1, (int)ceil(maxYf));
	if (scan->minX > scan->maxX || scan->minY > scan->maxY)
		return 0;

	scan->areaSign = area > 0.0f ? 1.0f : -1.0f;
	scan->invArea = 1.0f / area;
	scan->invZ0 = 1.0f / z1;
	scan->invZ1 = 1.0f / z2;
	scan->invZ2 = 1.0f / z3;
	scan->e0dx = scan->sy2 - scan->sy1;
	scan->e0dy = -(scan->sx2 - scan->sx1);
	scan->e1dx = scan->sy0 - scan->sy2;
	scan->e1dy = -(scan->sx0 - scan->sx2);
	scan->e2dx = scan->sy1 - scan->sy0;
	scan->e2dy = -(scan->sx1 - scan->sx0);
	return 1;
}

// Pass 1: scatter the closest view-Z per pixel. CL 1.2 has no float atomic_min, so a
// positive float's bits are monotonic in unsigned order; putting them in the high word of a
// 64-bit value also lets the low word carry the winning triangle id.
__kernel void rasterizeDepth(__global const RasterTri *restrict tris,
							 __global const int *restrict triObjectIdx,
							 __global const RasterObject *restrict objects,
							 int triCount,
							 float3 camPos, float3 right, float3 up, float3 fwd,
							 float fovScale, float aspect, float2 jitter,
							 int screenWidth, int screenHeight,
							 __global ulong *restrict depthU) {
	int tid = get_global_id(0);
	if (tid >= triCount)
		return;

	const __global RasterTri *tri = &tris[tid];
	const __global RasterObject *obj = &objects[triObjectIdx[tid]];

	TriScan scan;
	if (!setupTriangle(tri, obj, camPos, right, up, fwd, fovScale, aspect, jitter,
					   screenWidth, screenHeight, &scan))
		return;

	float areaSign = scan.areaSign;
	float invArea = scan.invArea;
	float invZ0 = scan.invZ0, invZ1 = scan.invZ1, invZ2 = scan.invZ2;

	float startX = scan.minX + 0.5f;
	float startY = scan.minY + 0.5f;
	float e0Row = edgeFunction(scan.sx1, scan.sy1, scan.sx2, scan.sy2, startX, startY);
	float e1Row = edgeFunction(scan.sx2, scan.sy2, scan.sx0, scan.sy0, startX, startY);
	float e2Row = edgeFunction(scan.sx0, scan.sy0, scan.sx1, scan.sy1, startX, startY);

	for (int y = scan.minY; y <= scan.maxY; y++) {
		float e0 = e0Row, e1 = e1Row, e2 = e2Row;
		int idx = y * screenWidth + scan.minX;
		for (int x = scan.minX; x <= scan.maxX; x++, idx++) {
			if (e0 * areaSign >= 0.0f && e1 * areaSign >= 0.0f && e2 * areaSign >= 0.0f) {
				float invZ = (e0 * invZ0 + e1 * invZ1 + e2 * invZ2) * invArea;
				float depth = 1.0f / invZ;
				atom_min(&depthU[idx], ((ulong)as_uint(depth) << 32) | (ulong)(uint)tid);
			}
			e0 += scan.e0dx;
			e1 += scan.e1dx;
			e2 += scan.e2dx;
		}
		e0Row += scan.e0dy;
		e1Row += scan.e1dy;
		e2Row += scan.e2dy;
	}
}

// Nearest texel of the concatenated color maps, unpacked the way ray.c does
// (R in bits 0..7, G 8..15, B 16..23).
static float3 sampleColorMap(__global const uint *restrict colorMaps, RasterTexture tex,
							 uint u, uint v) {
	uint tx = u * (uint)(tex.size - 1) / 65535u;
	uint ty = v * (uint)(tex.size - 1) / 65535u;
	uint c = colorMaps[tex.offset + ty * (uint)tex.size + tx];
	return (float3)((c & 0xFFu) / 255.0f, ((c >> 8) & 0xFFu) / 255.0f, ((c >> 16) & 0xFFu) / 255.0f);
}

static float2 sampleMaterialMap(__global const ushort *restrict materialMaps, RasterTexture tex,
								uint u, uint v) {
	uint tx = u * (uint)(tex.size - 1) / 65535u;
	uint ty = v * (uint)(tex.size - 1) / 65535u;
	ushort rm = materialMaps[tex.offset + ty * (uint)tex.size + tx];
	float roughness = (float)(rm & 0xFFu) / 255.0f;
	float metallic = (float)((rm >> 8) & 0xFFu) / 255.0f;
	// Same metallic compression as the ray tracer: keep metals from dominating lighting.
	if (metallic > 0.25f)
		metallic = 0.25f + (metallic - 0.25f) * 0.75f;
	return (float2)(roughness, metallic);
}

// Tangent-space normal map. Textures_LoadFromFile stores it in the same R=bits0..7
// layout as the color map, so the unpack is shared.
static float3 sampleNormalMap(__global const uint *restrict normalMaps, RasterTexture tex,
							  uint u, uint v) {
	uint tx = u * (uint)(tex.size - 1) / 65535u;
	uint ty = v * (uint)(tex.size - 1) / 65535u;
	uint c = normalMaps[tex.offset + ty * (uint)tex.size + tx];
	return (float3)((c & 0xFFu) / 255.0f, ((c >> 8) & 0xFFu) / 255.0f, ((c >> 16) & 0xFFu) / 255.0f);
}

static uint skyFaceIndex(float3 dir) {
	float ax = fabs(dir.x), ay = fabs(dir.y), az = fabs(dir.z);
	if (ax >= ay && ax >= az)
		return dir.x > 0.0f ? SKY_RIGHT : SKY_LEFT;
	if (ay >= ax && ay >= az)
		return dir.y > 0.0f ? SKY_TOP : SKY_BOTTOM;
	return dir.z > 0.0f ? SKY_FRONT : SKY_BACK;
}

// Port of SampleSkybox (skybox/skybox.c) over the six faces Raster_SetSkybox uploaded
// back to back. The skybox pixels hold R in bits 16..23 (UnpackColor's order), unlike the
// material color maps, so the channels are read from the top byte down.
static float3 sampleSkybox(__global const uint *restrict sky, int faceWidth, int faceHeight,
						   float3 dir) {
	float ax = fabs(dir.x), ay = fabs(dir.y), az = fabs(dir.z);
	uint face;
	float u, v;
	if (ax >= ay && ax >= az) {
		face = dir.x > 0.0f ? SKY_RIGHT : SKY_LEFT;
		float su = dir.x > 0.0f ? -dir.z : dir.z;
		u = 0.5f + 0.5f * (su / ax);
		v = 0.5f + 0.5f * (-dir.y / ax);
	} else if (ay >= ax && ay >= az) {
		face = dir.y > 0.0f ? SKY_TOP : SKY_BOTTOM;
		u = 0.5f + 0.5f * (dir.x / ay);
		float sv = dir.y > 0.0f ? dir.z : -dir.z;
		v = 0.5f + 0.5f * (sv / ay);
	} else {
		face = dir.z > 0.0f ? SKY_FRONT : SKY_BACK;
		float su = dir.z > 0.0f ? dir.x : -dir.x;
		u = 0.5f + 0.5f * (su / az);
		v = 0.5f + 0.5f * (-dir.y / az);
	}

	int x = (int)(u * (float)(faceWidth - 1) + 0.5f);
	int y = (int)(v * (float)(faceHeight - 1) + 0.5f);
	x = clamp(x, 0, faceWidth - 1);
	y = clamp(y, 0, faceHeight - 1);
	uint c = sky[(size_t)face * faceWidth * faceHeight + y * faceWidth + x];
	return (float3)(((c >> 16) & 0xFFu) / 255.0f, ((c >> 8) & 0xFFu) / 255.0f, (c & 0xFFu) / 255.0f);
}

// Fraction of the 3x3 taps that see the light. ray.c does this with one shadow ray per
// pixel (sampled on a grid and blurred afterwards); a depth-map compare is a single
// texture fetch, which is what makes shadows affordable on the raster path.
static float sampleShadow(__global const uint *restrict shadowMap, RasterShadow sh,
						  float3 worldPos, float3 n, float ndl) {
	// Normal offset: push the sample off the surface by a couple of texels before
	// projecting. A scalar bias alone cannot cover the depth spread a curved surface has
	// inside one texel, and that spread is what makes an object shadow itself.
	float3 pos = worldPos + n * (sh.params.y * 2.0f);
	float3 lv = pos - sh.center.xyz;
	float x = dot(lv, sh.right.xyz) * sh.right.w;
	float y = dot(lv, sh.up.xyz) * sh.up.w;
	float z = dot(lv, sh.fwd.xyz) - sh.fwd.w;

	int size = (int)sh.params.x;
	float u = (x + 1.0f) * 0.5f * (float)size;
	float v = (1.0f - (y + 1.0f) * 0.5f) * (float)size;
	int iu = (int)floor(u);
	int iv = (int)floor(v);
	// Outside the fitted box nothing was rendered, so nothing can occlude.
	if (iu < 0 || iv < 0 || iu >= size || iv >= size)
		return 1.0f;

	// Small extra bias for the grazing case the normal offset covers least.
	float bias = sh.params.y * (0.5f + 1.5f * (1.0f - ndl));

	int litTaps = 0;
	for (int dy = -1; dy <= 1; dy++) {
		int sy = clamp(iv + dy, 0, size - 1);
		for (int dx = -1; dx <= 1; dx++) {
			int sx = clamp(iu + dx, 0, size - 1);
			if (z - bias <= as_float(shadowMap[sy * size + sx]))
				litTaps++;
		}
	}
	return (float)litTaps * (1.0f / 9.0f);
}

// Pass 2: one thread per pixel. The depth pass already resolved which triangle owns each
// pixel, so this pass only reads that id and shades - it never walks a bounding box. Running
// the shading per pixel (rather than per triangle) keeps a large triangle from being shaded
// serially by a single thread, which was the whole cost of the previous two-scatter design.
__kernel void rasterizeShade(__global const RasterTri *restrict tris,
							 __global const int *restrict triObjectIdx,
							 __global const RasterObject *restrict objects,
							 __global const RasterMaterial *restrict materials,
							 __global const RasterTexture *restrict textures,
							 __global const uint *restrict colorMaps,
							 __global const uint *restrict normalMaps,
							 __global const ushort *restrict materialMaps,
							 __global const uint *restrict sky,
							 int skyWidth, int skyHeight,
							 RasterShadow shadow,
							 __global const uint *restrict shadowMap,
							 __global const RasterEmitter *restrict emitters,
							 int emitterCount,
							 float3 camPos, float3 right, float3 up, float3 fwd,
							 float fovScale, float aspect, float2 jitter,
							 float3 lightDir, int screenWidth, int screenHeight,
							 __global const ulong *restrict depthU,
							 __global float4 *restrict framebuffer,
							 __global float *restrict depthOut,
							 __global float4 *restrict normalOut,
							 __global float4 *restrict positionOut,
							 __global int *restrict objectIdOut,
							 __global int *restrict triangleIdOut) {
	int x = get_global_id(0);
	int y = get_global_id(1);
	if (x >= screenWidth || y >= screenHeight)
		return;

	int idx = y * screenWidth + x;
	ulong hit = depthU[idx];
	if (hit == NO_HIT)
		return;

	int tid = (int)(uint)hit;
	const __global RasterTri *tri = &tris[tid];
	const int objIdx = triObjectIdx[tid];
	const __global RasterObject *obj = &objects[objIdx];

	TriScan scan;
	if (!setupTriangle(tri, obj, camPos, right, up, fwd, fovScale, aspect, jitter,
					   screenWidth, screenHeight, &scan))
		return;

	float areaSign = scan.areaSign;
	float invArea = scan.invArea;
	float invZ0 = scan.invZ0, invZ1 = scan.invZ1, invZ2 = scan.invZ2;
	float3 nrm = scan.nrm;
	float nlen = sqrt(dot(nrm, nrm));
	nrm = nlen > 1e-6f ? nrm / nlen : nrm;

	int matId = tri->materialId;
	RasterMaterial mat = (RasterMaterial){(float4)(0.8f, 0.8f, 0.8f, 0.0f), 0.8f, 0.0f, 0.0f, -1};
	if (matId >= 0)
		mat = materials[matId];

	// Textures are an object flag: a merged object can point at a material that still
	// carries a texture block while its own UV array is gone.
	int textureSlot = (obj->flags & RASTER_FLAG_TEXTURE) ? mat.textureSlot : -1;
	RasterTexture tex = (RasterTexture){0, 0};
	if (textureSlot >= 0)
		tex = textures[textureSlot];

	float3 light = normalize(lightDir);

	uint uv1x = tri->uv01 & 0xFFFFu, uv1y = tri->uv01 >> 16;
	uint uv2x = tri->uv23 & 0xFFFFu, uv2y = tri->uv23 >> 16;
	uint uv3x = tri->uv45 & 0xFFFFu, uv3y = tri->uv45 >> 16;

	// Coverage and depth straight at this pixel; no bounding-box walk needed.
	float px = (float)x + 0.5f;
	float py = (float)y + 0.5f;
	float e0 = edgeFunction(scan.sx1, scan.sy1, scan.sx2, scan.sy2, px, py);
	float e1 = edgeFunction(scan.sx2, scan.sy2, scan.sx0, scan.sy0, px, py);
	float e2 = edgeFunction(scan.sx0, scan.sy0, scan.sx1, scan.sy1, px, py);
	if (!(e0 * areaSign >= 0.0f && e1 * areaSign >= 0.0f && e2 * areaSign >= 0.0f))
		return;

	float invZ = (e0 * invZ0 + e1 * invZ1 + e2 * invZ2) * invArea;
	float depth = 1.0f / invZ;

	// Perspective-correct barycentrics, same pSum/invPSum form as render.c.
	float p1 = e0 * invZ0, p2 = e1 * invZ1, p3 = e2 * invZ2;
	float invPSum = invArea * depth;
	float3 worldPos = (float3)(
		(scan.w1.x * p1 + scan.w2.x * p2 + scan.w3.x * p3) * invPSum,
		(scan.w1.y * p1 + scan.w2.y * p2 + scan.w3.y * p3) * invPSum,
		(scan.w1.z * p1 + scan.w2.z * p2 + scan.w3.z * p3) * invPSum);

	float3 color = mat.color.xyz;
	float roughness = mat.roughness;
	float metallic = mat.metallic;
	float3 n = nrm;
	if (textureSlot >= 0) {
		// Interpolate the stored [0..65535] UVs, then truncate to 16 bits exactly
		// like calculateUvCoordinatesForTriangle (the wrap past 65535 included).
		float u = (p1 * (float)uv1x + p2 * (float)uv2x + p3 * (float)uv3x) * invPSum;
		float v = (p1 * (float)uv1y + p2 * (float)uv2y + p3 * (float)uv3y) * invPSum;
		uint ui = (uint)(ushort)u;
		uint vi = (uint)(ushort)v;
		color = sampleColorMap(colorMaps, tex, ui, vi);
		float2 rm = sampleMaterialMap(materialMaps, tex, ui, vi);
		roughness = rm.x;
		metallic = rm.y;

		// Tangent-space normal map. The decode, the Y flip that matches the V
		// direction of the loaded maps, the tangent frame from the UV derivatives
		// and the strength are all lifted from ray.c.
		float3 nTex = sampleNormalMap(normalMaps, tex, ui, vi) * 2.0f - 1.0f;
		nTex.y = -nTex.y;
		float nTexLen = length(nTex);
		if (nTexLen > 1e-3f) {
			float3 edge1 = tri->v2.xyz - tri->v1.xyz;
			float3 edge2 = tri->v3.xyz - tri->v1.xyz;
			float dU1 = (float)((int)uv2x - (int)uv1x) / 65535.0f;
			float dV1 = (float)((int)uv2y - (int)uv1y) / 65535.0f;
			float dU2 = (float)((int)uv3x - (int)uv1x) / 65535.0f;
			float dV2 = (float)((int)uv3y - (int)uv1y) / 65535.0f;
			float det = dU1 * dV2 - dU2 * dV1;
			float3 T, B;
			if (fabs(det) > 1e-6f) {
				float inv = 1.0f / det;
				float3 tRaw = (dV2 * edge1 - dV1 * edge2) * inv;
				// The derivative is in local space, so rotate it with the object.
				float3 tWorld = (float3)(
					dot(obj->rot0.xyz, tRaw),
					dot(obj->rot1.xyz, tRaw),
					dot(obj->rot2.xyz, tRaw));
				tWorld -= n * dot(tWorld, n);
				float tLen = length(tWorld);
				T = tLen > 1e-6f ? tWorld / tLen : tWorld;
				B = cross(n, T);
			} else {
				// Degenerate UVs: build an arbitrary frame from the face normal.
				float3 upAxis = fabs(n.y) < 0.9f ? (float3)(0.0f, 1.0f, 0.0f) : (float3)(1.0f, 0.0f, 0.0f);
				T = normalize(cross(upAxis, n));
				B = cross(n, T);
			}
			float3 nTexN = normalize((float3)(nTex.x * NORMAL_MAP_STRENGTH / nTexLen,
											  nTex.y * NORMAL_MAP_STRENGTH / nTexLen,
											  nTex.z / nTexLen));
			n = normalize(T * nTexN.x + B * nTexN.y + n * nTexN.z);
		}
	}

	// Base lighting of ray.c: metals absorb no diffuse, ambient floor is 0.02.
	float ndl = fmax(0.0f, dot(n, light));
	float emission = mat.emission * 0.01f;
	float lit = (0.02f + 0.98f * ndl) * (1.0f - metallic);

	float3 viewDir = worldPos - camPos;
	float viewLen = length(viewDir);
	viewDir = viewLen > 1e-6f ? viewDir / viewLen : (float3)(0.0f, 0.0f, 1.0f);
	float ndv = fmax(1e-4f, -dot(n, viewDir));
	float invNdv = 1.0f - ndv;
	float inv2 = invNdv * invNdv;
	float f0 = 0.04f + 0.96f * metallic;
	float fresnel = f0 + (1.0f - f0) * (inv2 * inv2 * invNdv);

	// GGX specular against the single scene light, as ray.c evaluates it.
	float3 halfVec = light - viewDir;
	float halfLen = length(halfVec);
	halfVec = halfLen > 1e-6f ? halfVec / halfLen : n;
	float ndh = fmax(0.0f, dot(n, halfVec));
	float alpha = roughness * roughness;
	float alpha2 = alpha * alpha;
	float dg = ndh * ndh * (alpha2 - 1.0f) + 1.0f;
	float D = alpha2 / fmax(dg * dg, 1e-5f);
	float k = alpha * 0.5f;
	float gv = ndv / fmax(ndv * (1.0f - k) + k, 1e-5f);
	float gl = ndl / fmax(ndl * (1.0f - k) + k, 1e-5f);
	float specular = ndl > 0.0f ? (D * gv * gl * fresnel / fmax(4.0f * ndl * ndv, 1e-5f)) : 0.0f;
	float3 specularColor = specular * (1.0f - metallic) + specular * metallic * color;

	float3 shaded = color * lit + specularColor + color * emission;

	// Sky reflection: the same Fresnel term, damped by roughness and by how much
	// the surface faces the light, tinted by the albedo for metals.
	if (skyWidth > 0) {
		float reflectStrength = fresnel * (1.0f - roughness) * 0.18f *
								(0.15f + 0.85f * ndl) * (1.0f - fmin(emission, 1.0f));
		float st = fmin(reflectStrength, 1.0f);
		// Below this the blend moves a channel by less than one 8-bit step, so the texture
		// fetch is pure cost - and most of this scene is rough enough to land here.
		if (st > 0.002f) {
			float3 reflDir = viewDir - n * (2.0f * dot(n, viewDir));
			float3 skyColor = sampleSkybox(sky, skyWidth, skyHeight, reflDir);
			float3 tint = (float3)(1.0f - metallic, 1.0f - metallic, 1.0f - metallic) + metallic * color;
			shaded = shaded * (1.0f - st) + skyColor * tint * st;
		}
	}

	// Shadows last, over the reflection blend, exactly where ray.c multiplies its
	// blurred shadow term into the framebuffer.
	float3 emitted = (float3)(0.0f, 0.0f, 0.0f);
	if (emitterCount > 0) {
		// Sum the emission over the nearest emitters, same inverse-square falloff and
		// |N.L| that ray.c uses in its accumulatedEmission term.
		for (int e = 0; e < emitterCount; e++) {
			float3 toEmitter = emitters[e].position.xyz - worldPos;
			float dist2 = dot(toEmitter, toEmitter) + 1e-6f;
			float3 toEmitterN = toEmitter * rsqrt(dist2);
			float nd = fabs(dot(n, toEmitterN));
			if (nd <= 0.0f)
				continue;
			emitted += emitters[e].colorEmission.xyz * (nd / dist2);
		}
	}

	if (shadow.params.z > 0.0f) {
		float litFraction = sampleShadow(shadowMap, shadow, worldPos, n, ndl);
		shaded = shaded * (SHADOW_AMBIENT + (1.0f - SHADOW_AMBIENT) * litFraction);
	}

	// Emissive indirect is additive, after the shadow term, matching ray.c's composite.
	shaded += emitted;

	framebuffer[idx] = (float4)(shaded.x, shaded.y, shaded.z, 0.0f);
	depthOut[idx] = depth;
	normalOut[idx] = (float4)(n.x, n.y, n.z, 0.0f);
	positionOut[idx] = (float4)(worldPos.x, worldPos.y, worldPos.z, 0.0f);
	objectIdOut[idx] = obj->sceneIndex;
	triangleIdOut[idx] = tid - obj->triStart;
}

// Fills the pixels the scene did not cover with the skybox, sampled along the primary
// ray of that pixel. Matches what the row kernel of ray.c writes for a miss.
__kernel void rasterizeSky(__global const uint *restrict sky,
						   int skyWidth, int skyHeight,
						   __global const ulong *restrict depthU,
						   float3 right, float3 up, float3 fwd,
						   float fovScale, float aspect, float2 jitter,
						   int screenWidth, int screenHeight,
						   __global float4 *restrict framebuffer) {
	int x = get_global_id(0);
	int y = get_global_id(1);
	if (x >= screenWidth || y >= screenHeight)
		return;

	int idx = y * screenWidth + x;
	// depthU is cleared to all ones, so a pixel still holding that value was missed.
	if (depthU[idx] != NO_HIT)
		return;

	// Invert the projection used by setupTriangle so the ray lands on the pixel centre.
	float ndcX = ((float)x + 0.5f - jitter.x) * 2.0f / (float)screenWidth - 1.0f;
	float ndcY = 1.0f - ((float)y + 0.5f - jitter.y) * 2.0f / (float)screenHeight;
	float3 dir = normalize(fwd + right * (ndcX * aspect * fovScale) + up * (ndcY * fovScale));

	float3 c = sampleSkybox(sky, skyWidth, skyHeight, dir);
	framebuffer[idx] = (float4)(c.x, c.y, c.z, 0.0f);
}

static uint aoHash(int x, int y) {
	uint h = (uint)x * 0x9E3779B1u ^ (uint)y * 0x85EBCA67u;
	h ^= h >> 16;
	h *= 0x7FEB352Du;
	h ^= h >> 13;
	h *= 0x846CA68Bu;
	h ^= h >> 16;
	return h;
}

// Screen-space ambient occlusion, ported from render/cpu/AO.h. The world radius, bias,
// sample count, occlusion formula and the per-pixel rotation are the CPU ones; the 8
// sample directions are a golden-angle spiral instead of the CPU's baked Poisson table,
// which the wide smoothing below removes the visual difference of.
__kernel void rasterizeAo(__global const float *restrict depthBuf,
						  __global const float4 *restrict normalBuf,
						  __global const float4 *restrict positionBuf,
						  int screenWidth, int screenHeight, float fovScale,
						  __global float *restrict aoOut) {
	int x = get_global_id(0);
	int y = get_global_id(1);
	if (x >= screenWidth || y >= screenHeight)
		return;

	int idx = y * screenWidth + x;
	float viewDepth = depthBuf[idx];
	if (viewDepth >= DEPTH_FAR || viewDepth <= 0.0f) {
		aoOut[idx] = 1.0f; // sky is never occluded
		return;
	}

	float3 normal = normalBuf[idx].xyz;
	float3 position = positionBuf[idx].xyz;

	// Sample spread in pixels follows the projected world radius, clamped like the CPU.
	float focalLengthPixels = ((float)screenHeight * 0.5f) / fovScale;
	float pixelRadius = clamp(focalLengthPixels * AO_WORLD_RADIUS / viewDepth, 2.0f, AO_MAX_PIXEL_RADIUS);

	float rotation = (float)(aoHash(x, y) & 0xFFFFu) * (6.2831853f / 65535.0f);
	float occlusion = 0.0f;
	int validSamples = 0;

	for (int k = 0; k < AO_SAMPLES; k++) {
		float angle = (float)k * AO_GOLDEN_ANGLE + rotation;
		float radius = sqrt(((float)k + 0.5f) / (float)AO_SAMPLES) * pixelRadius;
		float sampleX = (float)x + cos(angle) * radius;
		float sampleY = (float)y + sin(angle) * radius;
		if (sampleX < 0.0f || sampleX >= (float)screenWidth || sampleY < 0.0f || sampleY >= (float)screenHeight)
			continue;

		int sampleIdx = (int)sampleY * screenWidth + (int)sampleX;
		if (depthBuf[sampleIdx] >= DEPTH_FAR || depthBuf[sampleIdx] <= 0.0f)
			continue;

		float3 dir = positionBuf[sampleIdx].xyz - position;
		float nd = dot(normal, dir);
		if (nd <= 0.0f)
			continue;

		float dist2 = dot(dir, dir);
		if (dist2 < AO_BIAS2 || dist2 > AO_RADIUS2)
			continue;

		float dist = sqrt(dist2);
		occlusion += (nd / dist) * (1.0f - dist * AO_INV_RADIUS);
		validSamples++;
	}

	// Average over the samples that actually landed on geometry (INV_VALID in AO.h).
	float ao = validSamples > 0 ? 1.0f - occlusion / (float)validSamples : 1.0f;
	aoOut[idx] = clamp(ao, 0.0f, 1.0f);
}

// One axis of the separable 5-tap gaussian. depthGated reproduces the edge-preserving
// weighting of AO.h (a tap fades out as its depth difference grows); the wide smoothing
// rounds run it ungated with a stride matching the CPU's 4x downsample, so the blur gets
// the same support without a separate pyramid.
__kernel void rasterizeAoBlur(__global const float *restrict aoIn,
							  __global const float *restrict depthBuf,
							  __global float *restrict aoOut,
							  int screenWidth, int screenHeight,
							  int step, int alongX, int depthGated) {
	int x = get_global_id(0);
	int y = get_global_id(1);
	if (x >= screenWidth || y >= screenHeight)
		return;

	int idx = y * screenWidth + x;
	float center = aoIn[idx];
	float centerDepth = depthBuf[idx];

	float sum = 0.0f, weightSum = 0.0f;
	float scaledDepth = centerDepth * AO_DEPTH_REJECT_SCALE;
	float invT = scaledDepth > 1e-30f ? 1.0f / scaledDepth : 0.0f;

	for (int k = -2; k <= 2; k++) {
		int sx = alongX ? x + k * step : x;
		int sy = alongX ? y : y + k * step;
		sx = clamp(sx, 0, screenWidth - 1);
		sy = clamp(sy, 0, screenHeight - 1);
		int tapIdx = sy * screenWidth + sx;

		float w = AO_KERNEL[k + 2];
		if (depthGated && invT > 0.0f) {
			float depthDiff = fabs(depthBuf[tapIdx] - centerDepth);
			w *= clamp(1.0f - depthDiff * invT, 0.0f, 1.0f);
		}
		sum += aoIn[tapIdx] * w;
		weightSum += w;
	}

	aoOut[idx] = weightSum > 1e-5f ? sum / weightSum : center;
}

// Darkens the shaded frame by the AO term. Same remap and strength as
// applyAmbientOcclusion in AO.h, which the ray pass runs at the end of RayTraceScene.
__kernel void rasterizeAoApply(__global float4 *restrict framebuffer,
							   __global const float *restrict aoIn,
							   float strength, int pixelCount) {
	int idx = get_global_id(0);
	if (idx >= pixelCount)
		return;

	float ao = clamp(1.0f - (1.0f - aoIn[idx]) * strength, 0.0f, 1.0f);
	framebuffer[idx] = (float4)(framebuffer[idx].x * ao, framebuffer[idx].y * ao,
								framebuffer[idx].z * ao, 0.0f);
}

// Shadow map: the scene rasterized from the light in an orthographic frame, keeping the
// nearest light-space depth per texel. Depth is affine under an orthographic projection,
// so the barycentric weights need no perspective divide. Nothing is culled - a depth map
// only needs the closest surface, and culling would leak light through single-sided faces.
typedef struct {
	float z0, z1, z2;
	float sx0, sy0, sx1, sy1, sx2, sy2;
	float areaSign, invArea;
	float e0dx, e0dy, e1dx, e1dy, e2dx, e2dy;
	int minX, minY, maxX, maxY;
} OrthoScan;

static int setupOrthoTriangle(__global const RasterTri *tri, __global const RasterObject *obj,
							  RasterShadow sh, OrthoScan *scan) {
	float3 scl = obj->scale.xyz;
	float3 position = obj->position.xyz;
	float3 w0 = toWorld(tri->v1.xyz, scl, position, obj->rot0, obj->rot1, obj->rot2);
	float3 w1 = toWorld(tri->v2.xyz, scl, position, obj->rot0, obj->rot1, obj->rot2);
	float3 w2 = toWorld(tri->v3.xyz, scl, position, obj->rot0, obj->rot1, obj->rot2);

	float3 c0 = w0 - sh.center.xyz;
	float3 c1 = w1 - sh.center.xyz;
	float3 c2 = w2 - sh.center.xyz;

	float size = sh.params.x;
	float x0 = dot(c0, sh.right.xyz) * sh.right.w;
	float y0 = dot(c0, sh.up.xyz) * sh.up.w;
	float x1 = dot(c1, sh.right.xyz) * sh.right.w;
	float y1 = dot(c1, sh.up.xyz) * sh.up.w;
	float x2 = dot(c2, sh.right.xyz) * sh.right.w;
	float y2 = dot(c2, sh.up.xyz) * sh.up.w;

	scan->sx0 = (x0 + 1.0f) * 0.5f * size;
	scan->sy0 = (1.0f - y0) * 0.5f * size;
	scan->sx1 = (x1 + 1.0f) * 0.5f * size;
	scan->sy1 = (1.0f - y1) * 0.5f * size;
	scan->sx2 = (x2 + 1.0f) * 0.5f * size;
	scan->sy2 = (1.0f - y2) * 0.5f * size;

	// Stored relative to the box minimum so the value stays non-negative and its float
	// bits compare monotonically as unsigned.
	scan->z0 = dot(c0, sh.fwd.xyz) - sh.fwd.w;
	scan->z1 = dot(c1, sh.fwd.xyz) - sh.fwd.w;
	scan->z2 = dot(c2, sh.fwd.xyz) - sh.fwd.w;

	float area = edgeFunction(scan->sx0, scan->sy0, scan->sx1, scan->sy1, scan->sx2, scan->sy2);
	if (fabs(area) <= MIN_AREA)
		return 0;

	float minXf = fmin(fmin(scan->sx0, scan->sx1), scan->sx2);
	float maxXf = fmax(fmax(scan->sx0, scan->sx1), scan->sx2);
	float minYf = fmin(fmin(scan->sy0, scan->sy1), scan->sy2);
	float maxYf = fmax(fmax(scan->sy0, scan->sy1), scan->sy2);
	scan->minX = max(0, (int)floor(minXf));
	scan->maxX = min((int)size - 1, (int)ceil(maxXf));
	scan->minY = max(0, (int)floor(minYf));
	scan->maxY = min((int)size - 1, (int)ceil(maxYf));
	if (scan->minX > scan->maxX || scan->minY > scan->maxY)
		return 0;

	scan->areaSign = area > 0.0f ? 1.0f : -1.0f;
	scan->invArea = 1.0f / area;
	scan->e0dx = scan->sy2 - scan->sy1;
	scan->e0dy = -(scan->sx2 - scan->sx1);
	scan->e1dx = scan->sy0 - scan->sy2;
	scan->e1dy = -(scan->sx0 - scan->sx2);
	scan->e2dx = scan->sy1 - scan->sy0;
	scan->e2dy = -(scan->sx1 - scan->sx0);
	return 1;
}

__kernel void rasterizeShadow(__global const RasterTri *restrict tris,
							  __global const int *restrict triObjectIdx,
							  __global const RasterObject *restrict objects,
							  int triCount, RasterShadow shadow,
							  __global uint *restrict shadowMap) {
	int tid = get_global_id(0);
	if (tid >= triCount)
		return;

	const __global RasterTri *tri = &tris[tid];
	const __global RasterObject *obj = &objects[triObjectIdx[tid]];

	OrthoScan scan;
	if (!setupOrthoTriangle(tri, obj, shadow, &scan))
		return;

	int size = (int)shadow.params.x;
	float areaSign = scan.areaSign;
	float invArea = scan.invArea;
	float z0 = scan.z0, z1 = scan.z1, z2 = scan.z2;

	float startX = scan.minX + 0.5f;
	float startY = scan.minY + 0.5f;
	float e0Row = edgeFunction(scan.sx1, scan.sy1, scan.sx2, scan.sy2, startX, startY);
	float e1Row = edgeFunction(scan.sx2, scan.sy2, scan.sx0, scan.sy0, startX, startY);
	float e2Row = edgeFunction(scan.sx0, scan.sy0, scan.sx1, scan.sy1, startX, startY);

	for (int y = scan.minY; y <= scan.maxY; y++) {
		float e0 = e0Row, e1 = e1Row, e2 = e2Row;
		int idx = y * size + scan.minX;
		for (int x = scan.minX; x <= scan.maxX; x++, idx++) {
			if (e0 * areaSign >= 0.0f && e1 * areaSign >= 0.0f && e2 * areaSign >= 0.0f) {
				float z = (e0 * z0 + e1 * z1 + e2 * z2) * invArea;
				atomic_min(&shadowMap[idx], as_uint(fmax(z, 0.0f)));
			}
			e0 += scan.e0dx;
			e1 += scan.e1dx;
			e2 += scan.e2dx;
		}
		e0Row += scan.e0dy;
		e1Row += scan.e1dy;
		e2Row += scan.e2dy;
	}
}

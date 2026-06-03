#include "../object/format.h"
#include <math.h>
#include "timings.h"
#include <immintrin.h>
#include "../skybox/skybox.h"

static inline Color sampleFace(const uint32 *face, int w, int h, float u, float v) {
	if (!face) return 0xFF101010u;
	int x = (int)(u * (float)(w - 1) + 0.5f);
	int y = (int)(v * (float)(h - 1) + 0.5f);
	if (x < 0) x = 0; else if (x >= w) x = w - 1;
	if (y < 0) y = 0; else if (y >= h) y = h - 1;
	return face[y * w + x];
}

static inline Color SampleSkyboxOld(const Skybox *skybox, const float3 dir) {
	if (!skybox) return 0xFF000000u;

	int w = skybox->imageWidth;
	int h = skybox->imageHeight;

	float ax = dir.x < 0 ? -dir.x : dir.x;
	float ay = dir.y < 0 ? -dir.y : dir.y;
	float az = dir.z < 0 ? -dir.z : dir.z;

	float u, v;

	if (ax >= ay && ax >= az) {
		// ±X face
		if (dir.x > 0) {
			// right: u = -Z/X, v = -Y/X
			u = 0.5f + 0.5f * (-dir.z / ax);
			v = 0.5f + 0.5f * (-dir.y / ax);
			return sampleFace(skybox->right, w, h, u, v);
		} else {
			// left: u = +Z/X, v = -Y/X
			u = 0.5f + 0.5f * (dir.z / ax);
			v = 0.5f + 0.5f * (-dir.y / ax);
			return sampleFace(skybox->left, w, h, u, v);
		}
	} else if (ay >= ax && ay >= az) {
		// ±Y face
		if (dir.y > 0) {
			// top: u = +X/Y, v = +Z/Y
			u = 0.5f + 0.5f * (dir.x / ay);
			v = 0.5f + 0.5f * (dir.z / ay);
			return sampleFace(skybox->top, w, h, u, v);
		} else {
			// bottom: u = +X/Y, v = -Z/Y
			u = 0.5f + 0.5f * (dir.x / ay);
			v = 0.5f + 0.5f * (-dir.z / ay);
			return sampleFace(skybox->bottom, w, h, u, v);
		}
	} else {
		// ±Z face
		if (dir.z > 0) {
			// front: u = +X/Z, v = -Y/Z
			u = 0.5f + 0.5f * (dir.x / az);
			v = 0.5f + 0.5f * (-dir.y / az);
			return sampleFace(skybox->front, w, h, u, v);
		} else {
			// back: u = -X/Z, v = -Y/Z
			u = 0.5f + 0.5f * (-dir.x / az);
			v = 0.5f + 0.5f * (-dir.y / az);
			return sampleFace(skybox->back, w, h, u, v);
		}
	}
}


static inline Color SampleSkyboxAVX(const Skybox *skybox, const float3 dir) {
	if (!skybox) return 0xFF000000u;

	float ax = fabsf(dir.x), ay = fabsf(dir.y), az = fabsf(dir.z);

	int face_idx;
	if (ax >= ay && ax >= az)  face_idx = dir.x > 0.0f ? 0 : 1;
	else if (ay >= az)         face_idx = dir.y > 0.0f ? 2 : 3;
	else                       face_idx = dir.z > 0.0f ? 4 : 5;

	float inv_ax = 0.5f / ax, inv_ay = 0.5f / ay, inv_az = 0.5f / az;
	float x = dir.x, y = dir.y, z = dir.z;

	__m256 half   = _mm256_set1_ps(0.5f);
	// u numerators  [right, left, top, bot,  front, back, -, -]
	__m256 u_comp = _mm256_set_ps(0, 0, -x,  x,    x,     x,  z, -z);
	// denominators  [inv_ax, inv_ax, inv_ay, inv_ay, inv_az, inv_az, 0, 0]
	__m256 denom  = _mm256_set_ps(0, 0, inv_az, inv_az, inv_ay, inv_ay, inv_ax, inv_ax);
	__m256 u_all  = _mm256_fmadd_ps(u_comp, denom, half);

	// v numerators  [right, left, top,  bot,  front, back, -, -]
	__m256 v_comp = _mm256_set_ps(0, 0, -y,  -y,   -z,    z,  -y, -y);
	__m256 v_all  = _mm256_fmadd_ps(v_comp, denom, half);

	__m256i sel = _mm256_set1_epi32(face_idx);
	float u = _mm256_cvtss_f32(_mm256_permutevar8x32_ps(u_all, sel));
	float v = _mm256_cvtss_f32(_mm256_permutevar8x32_ps(v_all, sel));

	const uint32 *faces[6] = {
		skybox->right, skybox->left, skybox->top,
		skybox->bottom, skybox->front, skybox->back
	};
	return sampleFace(faces[face_idx], skybox->imageWidth, skybox->imageHeight, u, v);
}

// Processes 8 rays per call using AVX2. dirs_x/y/z are SoA arrays of 8 floats each.
static inline void SampleSkybox8(
    const Skybox *skybox,                               
    const float *dirs_x, // 8 float
    const float *dirs_y, // 8 float
    const float *dirs_z, // 8 float
    Color *out // 8 uint32
)
{
	__m256 vx = _mm256_loadu_ps(dirs_x);
	__m256 vy = _mm256_loadu_ps(dirs_y);
	__m256 vz = _mm256_loadu_ps(dirs_z);

	__m256 sign_mask = _mm256_set1_ps(-0.0f);
	__m256 half      = _mm256_set1_ps(0.5f);
	__m256 one       = _mm256_set1_ps(1.0f);

	__m256 ax = _mm256_andnot_ps(sign_mask, vx);
	__m256 ay = _mm256_andnot_ps(sign_mask, vy);
	__m256 az = _mm256_andnot_ps(sign_mask, vz);

	// Extract ±1.0 signs per component
	__m256 sx = _mm256_or_ps(one, _mm256_and_ps(sign_mask, vx));
	__m256 sy = _mm256_or_ps(one, _mm256_and_ps(sign_mask, vy));
	__m256 sz = _mm256_or_ps(one, _mm256_and_ps(sign_mask, vz));

	__m256 inv_ax = _mm256_div_ps(half, ax);
	__m256 inv_ay = _mm256_div_ps(half, ay);
	__m256 inv_az = _mm256_div_ps(half, az);

	// UV for each dominant-axis case (all 8 lanes in parallel)
	// X: u = 0.5 - sx*z*inv_ax,  v = 0.5 - y*inv_ax
	__m256 u_x = _mm256_fnmadd_ps(_mm256_mul_ps(sx, vz), inv_ax, half);
	__m256 v_x = _mm256_fnmadd_ps(vy, inv_ax, half);
	// Y: u = 0.5 + x*inv_ay,     v = 0.5 + sy*z*inv_ay
	__m256 u_y = _mm256_fmadd_ps(vx, inv_ay, half);
	__m256 v_y = _mm256_fmadd_ps(_mm256_mul_ps(sy, vz), inv_ay, half);
	// Z: u = 0.5 + sz*x*inv_az,  v = 0.5 - y*inv_az
	__m256 u_z = _mm256_fmadd_ps(_mm256_mul_ps(sz, vx), inv_az, half);
	__m256 v_z = _mm256_fnmadd_ps(vy, inv_az, half);

	__m256 x_dom = _mm256_and_ps(_mm256_cmp_ps(ax, ay, _CMP_GE_OQ),
	                              _mm256_cmp_ps(ax, az, _CMP_GE_OQ));
	__m256 y_dom = _mm256_and_ps(_mm256_cmp_ps(ay, ax, _CMP_GE_OQ),
	                              _mm256_cmp_ps(ay, az, _CMP_GE_OQ));

	// Blend: z default, y overrides, x overrides last (preserves if/else priority)
	__m256 u = _mm256_blendv_ps(_mm256_blendv_ps(u_z, u_y, y_dom), u_x, x_dom);
	__m256 v = _mm256_blendv_ps(_mm256_blendv_ps(v_z, v_y, y_dom), v_x, x_dom);

	__m256 wf = _mm256_set1_ps((float)(skybox->imageWidth  - 1));
	__m256 hf = _mm256_set1_ps((float)(skybox->imageHeight - 1));
	__m256i px = _mm256_cvttps_epi32(_mm256_fmadd_ps(u, wf, half));
	__m256i py = _mm256_cvttps_epi32(_mm256_fmadd_ps(v, hf, half));

	__m256i zero_i = _mm256_setzero_si256();
	px = _mm256_min_epi32(_mm256_max_epi32(px, zero_i), _mm256_set1_epi32(skybox->imageWidth  - 1));
	py = _mm256_min_epi32(_mm256_max_epi32(py, zero_i), _mm256_set1_epi32(skybox->imageHeight - 1));

	__m256i idx = _mm256_add_epi32(_mm256_mullo_epi32(py, _mm256_set1_epi32(skybox->imageWidth)), px);

	int indices[8];
	_mm256_storeu_si256((__m256i *)indices, idx);

	// Scalar face selection — each lane may hit a different face pointer
	int xd = _mm256_movemask_ps(x_dom);
	int yd = _mm256_movemask_ps(y_dom);
	int xp = _mm256_movemask_ps(_mm256_cmp_ps(vx, _mm256_setzero_ps(), _CMP_GT_OQ));
	int yp = _mm256_movemask_ps(_mm256_cmp_ps(vy, _mm256_setzero_ps(), _CMP_GT_OQ));
	int zp = _mm256_movemask_ps(_mm256_cmp_ps(vz, _mm256_setzero_ps(), _CMP_GT_OQ));

	const uint32 *faces[6] = {
		skybox->right, skybox->left,
		skybox->top,   skybox->bottom,
		skybox->front, skybox->back
	};

	for (int i = 0; i < 8; i++) {
		int b  = 1 << i;
		int fi = (xd & b) ? ((xp & b) ? 0 : 1)
		       : (yd & b) ? ((yp & b) ? 2 : 3)
		       :             ((zp & b) ? 4 : 5);
		const uint32 *face = faces[fi];
		out[i] = face ? face[indices[i]] : 0xFF101010u;
	}
}


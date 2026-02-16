#ifndef MATH_MATRIX_H
#define MATH_MATRIX_H

#include <math.h>
#include <string.h>
#include "../object/format.h"

typedef struct float4x4 {
	float m[16];
} float4x4;

static inline float4x4 Matrix_Identity(void) {
	float4x4 result;
	memset(result.m, 0, sizeof(result.m));
	result.m[0] = 1.0f;
	result.m[5] = 1.0f;
	result.m[10] = 1.0f;
	result.m[15] = 1.0f;
	return result;
}

static inline float4x4 Matrix_Multiply(float4x4 a, float4x4 b) {
	float4x4 result;
	for (int row = 0; row < 4; row++) {
		for (int col = 0; col < 4; col++) {
			float sum = 0.0f;
			for (int k = 0; k < 4; k++) {
				sum += a.m[row * 4 + k] * b.m[k * 4 + col];
			}
			result.m[row * 4 + col] = sum;
		}
	}
	return result;
}

static inline float4x4 Matrix_Translation(float3 translation) {
	float4x4 result = Matrix_Identity();
	result.m[12] = translation.x;
	result.m[13] = translation.y;
	result.m[14] = translation.z;
	return result;
}

static inline float4x4 Matrix_Scale(float3 scale) {
	float4x4 result = Matrix_Identity();
	result.m[0] = scale.x;
	result.m[5] = scale.y;
	result.m[10] = scale.z;
	return result;
}

static inline float4x4 Matrix_RotationX(float angle) {
	float4x4 result = Matrix_Identity();
	float c = cosf(angle);
	float s = sinf(angle);
	result.m[5] = c;
	result.m[6] = -s;
	result.m[9] = s;
	result.m[10] = c;
	return result;
}

static inline float4x4 Matrix_RotationY(float angle) {
	float4x4 result = Matrix_Identity();
	float c = cosf(angle);
	float s = sinf(angle);
	result.m[0] = c;
	result.m[2] = s;
	result.m[8] = -s;
	result.m[10] = c;
	return result;
}

static inline float4x4 Matrix_RotationZ(float angle) {
	float4x4 result = Matrix_Identity();
	float c = cosf(angle);
	float s = sinf(angle);
	result.m[0] = c;
	result.m[1] = -s;
	result.m[4] = s;
	result.m[5] = c;
	return result;
}

static inline float4x4 Matrix_RotationXYZ(float3 rotation) {
	float4x4 rx = Matrix_RotationX(rotation.x);
	float4x4 ry = Matrix_RotationY(rotation.y);
	float4x4 rz = Matrix_RotationZ(rotation.z);
	return Matrix_Multiply(Matrix_Multiply(rz, ry), rx);
}

static inline float4x4 Matrix_TRS(float3 translation, float3 rotation, float3 scale) {
	float4x4 t = Matrix_Translation(translation);
	float4x4 r = Matrix_RotationXYZ(rotation);
	float4x4 s = Matrix_Scale(scale);
	return Matrix_Multiply(Matrix_Multiply(t, r), s);
}

static inline float3 Matrix_TransformPoint(float4x4 matrix, float3 point) {
	float x = matrix.m[0] * point.x + matrix.m[4] * point.y + matrix.m[8] * point.z + matrix.m[12];
	float y = matrix.m[1] * point.x + matrix.m[5] * point.y + matrix.m[9] * point.z + matrix.m[13];
	float z = matrix.m[2] * point.x + matrix.m[6] * point.y + matrix.m[10] * point.z + matrix.m[14];
	return (float3){x, y, z};
}

static inline float3 Matrix_TransformVector(float4x4 matrix, float3 vector) {
	float x = matrix.m[0] * vector.x + matrix.m[4] * vector.y + matrix.m[8] * vector.z;
	float y = matrix.m[1] * vector.x + matrix.m[5] * vector.y + matrix.m[9] * vector.z;
	float z = matrix.m[2] * vector.x + matrix.m[6] * vector.y + matrix.m[10] * vector.z;
	return (float3){x, y, z};
}

static inline float4x4 Matrix_Invert(float4x4 m) {
	float4x4 inv;
	float det;
	
	inv.m[0] = m.m[5] * m.m[10] * m.m[15] - m.m[5] * m.m[11] * m.m[14] - m.m[9] * m.m[6] * m.m[15] + 
	           m.m[9] * m.m[7] * m.m[14] + m.m[13] * m.m[6] * m.m[11] - m.m[13] * m.m[7] * m.m[10];
	
	inv.m[4] = -m.m[4] * m.m[10] * m.m[15] + m.m[4] * m.m[11] * m.m[14] + m.m[8] * m.m[6] * m.m[15] - 
	           m.m[8] * m.m[7] * m.m[14] - m.m[12] * m.m[6] * m.m[11] + m.m[12] * m.m[7] * m.m[10];
	
	inv.m[8] = m.m[4] * m.m[9] * m.m[15] - m.m[4] * m.m[11] * m.m[13] - m.m[8] * m.m[5] * m.m[15] + 
	           m.m[8] * m.m[7] * m.m[13] + m.m[12] * m.m[5] * m.m[11] - m.m[12] * m.m[7] * m.m[9];
	
	inv.m[12] = -m.m[4] * m.m[9] * m.m[14] + m.m[4] * m.m[10] * m.m[13] + m.m[8] * m.m[5] * m.m[14] - 
	            m.m[8] * m.m[6] * m.m[13] - m.m[12] * m.m[5] * m.m[10] + m.m[12] * m.m[6] * m.m[9];
	
	inv.m[1] = -m.m[1] * m.m[10] * m.m[15] + m.m[1] * m.m[11] * m.m[14] + m.m[9] * m.m[2] * m.m[15] - 
	           m.m[9] * m.m[3] * m.m[14] - m.m[13] * m.m[2] * m.m[11] + m.m[13] * m.m[3] * m.m[10];
	
	inv.m[5] = m.m[0] * m.m[10] * m.m[15] - m.m[0] * m.m[11] * m.m[14] - m.m[8] * m.m[2] * m.m[15] + 
	           m.m[8] * m.m[3] * m.m[14] + m.m[12] * m.m[2] * m.m[11] - m.m[12] * m.m[3] * m.m[10];
	
	inv.m[9] = -m.m[0] * m.m[9] * m.m[15] + m.m[0] * m.m[11] * m.m[13] + m.m[8] * m.m[1] * m.m[15] - 
	           m.m[8] * m.m[3] * m.m[13] - m.m[12] * m.m[1] * m.m[11] + m.m[12] * m.m[3] * m.m[9];
	
	inv.m[13] = m.m[0] * m.m[9] * m.m[14] - m.m[0] * m.m[10] * m.m[13] - m.m[8] * m.m[1] * m.m[14] + 
	            m.m[8] * m.m[2] * m.m[13] + m.m[12] * m.m[1] * m.m[10] - m.m[12] * m.m[2] * m.m[9];
	
	inv.m[2] = m.m[1] * m.m[6] * m.m[15] - m.m[1] * m.m[7] * m.m[14] - m.m[5] * m.m[2] * m.m[15] + 
	           m.m[5] * m.m[3] * m.m[14] + m.m[13] * m.m[2] * m.m[7] - m.m[13] * m.m[3] * m.m[6];
	
	inv.m[6] = -m.m[0] * m.m[6] * m.m[15] + m.m[0] * m.m[7] * m.m[14] + m.m[4] * m.m[2] * m.m[15] - 
	           m.m[4] * m.m[3] * m.m[14] - m.m[12] * m.m[2] * m.m[7] + m.m[12] * m.m[3] * m.m[6];
	
	inv.m[10] = m.m[0] * m.m[5] * m.m[15] - m.m[0] * m.m[7] * m.m[13] - m.m[4] * m.m[1] * m.m[15] + 
	            m.m[4] * m.m[3] * m.m[13] + m.m[12] * m.m[1] * m.m[7] - m.m[12] * m.m[3] * m.m[5];
	
	inv.m[14] = -m.m[0] * m.m[5] * m.m[14] + m.m[0] * m.m[6] * m.m[13] + m.m[4] * m.m[1] * m.m[14] - 
	            m.m[4] * m.m[2] * m.m[13] - m.m[12] * m.m[1] * m.m[6] + m.m[12] * m.m[2] * m.m[5];
	
	inv.m[3] = -m.m[1] * m.m[6] * m.m[11] + m.m[1] * m.m[7] * m.m[10] + m.m[5] * m.m[2] * m.m[11] - 
	           m.m[5] * m.m[3] * m.m[10] - m.m[9] * m.m[2] * m.m[7] + m.m[9] * m.m[3] * m.m[6];
	
	inv.m[7] = m.m[0] * m.m[6] * m.m[11] - m.m[0] * m.m[7] * m.m[10] - m.m[4] * m.m[2] * m.m[11] + 
	           m.m[4] * m.m[3] * m.m[10] + m.m[8] * m.m[2] * m.m[7] - m.m[8] * m.m[3] * m.m[6];
	
	inv.m[11] = -m.m[0] * m.m[5] * m.m[11] + m.m[0] * m.m[7] * m.m[9] + m.m[4] * m.m[1] * m.m[11] - 
	            m.m[4] * m.m[3] * m.m[9] - m.m[8] * m.m[1] * m.m[7] + m.m[8] * m.m[3] * m.m[5];
	
	inv.m[15] = m.m[0] * m.m[5] * m.m[10] - m.m[0] * m.m[6] * m.m[9] - m.m[4] * m.m[1] * m.m[10] + 
	            m.m[4] * m.m[2] * m.m[9] + m.m[8] * m.m[1] * m.m[6] - m.m[8] * m.m[2] * m.m[5];
	
	det = m.m[0] * inv.m[0] + m.m[1] * inv.m[4] + m.m[2] * inv.m[8] + m.m[3] * inv.m[12];
	
	if (fabsf(det) < 1e-8f) {
		return Matrix_Identity();
	}
	
	det = 1.0f / det;
	
	for (int i = 0; i < 16; i++) {
		inv.m[i] *= det;
	}
	
	return inv;
}

#endif // MATH_MATRIX_H

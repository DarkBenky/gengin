// Face emission maps: the same six orthographic sweeps CalculateFaceEmissions does in object.c,
// but sampling the painted emission and multiplying by the per triangle emission scalar. The
// result matches Object.h EmissionMap: one float3 grid of resolution x resolution per face.

"use strict";

const FaceMaps = {
	// name, u axis, v axis, depth axis, whether the sweep starts at the max side
	faces: [
		{ name: "front", u: 0, v: 1, w: 2, fromMax: false },
		{ name: "back", u: 0, v: 1, w: 2, fromMax: true },
		{ name: "right", u: 2, v: 1, w: 0, fromMax: false },
		{ name: "left", u: 2, v: 1, w: 0, fromMax: true },
		{ name: "top", u: 0, v: 2, w: 1, fromMax: true },
		{ name: "bottom", u: 0, v: 2, w: 1, fromMax: false },
	],
	defaultResolution: 16,
	minResolution: 2,
	maxResolution: 256,
	headerSize: 16,
	version: 3,
	magic: 0x334D4546, // 'FEM3'
};

function modelBounds(model) {
	const triView = model.triView;
	const floats = new Float32Array(triView.buffer, triView.byteOffset, triView.byteLength >> 2);
	const lo = [Infinity, Infinity, Infinity];
	const hi = [-Infinity, -Infinity, -Infinity];
	for (let i = 0; i < model.triCount; i++) {
		const f = i * 27;
		for (const base of [f, f + 4, f + 8]) {
			for (let axis = 0; axis < 3; axis++) {
				const value = floats[base + axis];
				if (value < lo[axis]) lo[axis] = value;
				if (value > hi[axis]) hi[axis] = value;
			}
		}
	}
	return { lo: lo, hi: hi };
}

// buildFaceGrids returns {resolution, faces: [{name, cells}]} where cells[v][u] is an [r,g,b]
// triple in 0..1 (values above 1 are possible when the triangle scalar is large)
function buildFaceGrids(model, emissionRgb, resolution, onProgress) {
	const size = model.textureSize;
	const triView = model.triView;
	const floats = new Float32Array(triView.buffer, triView.byteOffset, triView.byteLength >> 2);
	const uvs = new Uint16Array(triView.buffer, triView.byteOffset, triView.byteLength >> 1);
	const bounds = modelBounds(model);
	const cellsPerFace = resolution * resolution;

	let scalarPeak = 0;
	for (let i = 0; i < model.triCount; i++) {
		const value = floats[i * 27 + 18];
		if (value > scalarPeak) scalarPeak = value;
	}
	const useScalar = scalarPeak > 0;

	const state = FaceMaps.faces.map(function (face) {
		const planes = { face: face, depth: new Float64Array(cellsPerFace), cells: new Array(cellsPerFace) };
		// sweeps from the max side look for the largest depth, the others for the smallest
		planes.depth.fill(face.fromMax ? -Infinity : Infinity);
		return planes;
	});

	function range(start, end) {
		for (let t = start; t < end; t++) {
			const f = t * 27;
			const px = [floats[f], floats[f + 1], floats[f + 2]];
			const py = [floats[f + 4], floats[f + 5], floats[f + 6]];
			const pz = [floats[f + 8], floats[f + 9], floats[f + 10]];
			const points = [px, py, pz];
			const base = t * 54;
			const uvA = [uvs[base + 46], uvs[base + 46 + 1]];
			const uvB = [uvs[base + 48], uvs[base + 48 + 1]];
			const uvC = [uvs[base + 50], uvs[base + 50 + 1]];
			const emission = useScalar ? floats[f + 18] : 1;

			for (let faceIndex = 0; faceIndex < state.length; faceIndex++) {
				const planes = state[faceIndex];
				const face = planes.face;
				const lo = bounds.lo, hi = bounds.hi;
				const uSize = hi[face.u] - lo[face.u];
				const vSize = hi[face.v] - lo[face.v];
				if (uSize <= 0 || vSize <= 0) continue;

				const u = points.map(function (p) { return p[face.u]; });
				const v = points.map(function (p) { return p[face.v]; });
				const w = points.map(function (p) { return p[face.w]; });

				const det = (v[1] - v[2]) * (u[0] - u[2]) + (u[2] - u[1]) * (v[0] - v[2]);
				if (det === 0) continue;

				const u0 = Math.max(0, Math.floor((Math.min(u[0], u[1], u[2]) - lo[face.u]) / uSize * resolution));
				const u1 = Math.min(resolution - 1, Math.floor((Math.max(u[0], u[1], u[2]) - lo[face.u]) / uSize * resolution));
				const v0 = Math.max(0, Math.floor((Math.min(v[0], v[1], v[2]) - lo[face.v]) / vSize * resolution));
				const v1 = Math.min(resolution - 1, Math.floor((Math.max(v[0], v[1], v[2]) - lo[face.v]) / vSize * resolution));

				for (let cv = v0; cv <= v1; cv++) {
					const cellV = lo[face.v] + (cv + 0.5) / resolution * vSize;
					for (let cu = u0; cu <= u1; cu++) {
						const cellU = lo[face.u] + (cu + 0.5) / resolution * uSize;
						const w1 = ((v[1] - v[2]) * (cellU - u[2]) + (u[2] - u[1]) * (cellV - v[2])) / det;
						if (w1 < 0 || w1 > 1) continue;
						const w2 = ((v[2] - v[0]) * (cellU - u[2]) + (u[0] - u[2]) * (cellV - v[2])) / det;
						if (w2 < 0 || w1 + w2 > 1) continue;
						const w3 = 1 - w1 - w2;
						const depth = w[0] * w1 + w[1] * w2 + w[2] * w3;
						const index = cv * resolution + cu;
						if (face.fromMax ? depth <= planes.depth[index] : depth >= planes.depth[index]) continue;
						planes.depth[index] = depth;

						const u16 = uvA[0] * w1 + uvB[0] * w2 + uvC[0] * w3;
						const v16 = uvA[1] * w1 + uvB[1] * w2 + uvC[1] * w3;
						const tx = clamp(Math.floor(u16 * (size - 1) / 65535), 0, size - 1);
						const ty = clamp(Math.floor(v16 * (size - 1) / 65535), 0, size - 1);
						const e = (ty * size + tx) * 3;
						planes.cells[index] = [
							(emissionRgb[e] / 255) * emission,
							(emissionRgb[e + 1] / 255) * emission,
							(emissionRgb[e + 2] / 255) * emission,
						];
					}
				}
			}
		}
	}

	return runChunked(model.triCount, 4096, range, function (done, total) {
		if (onProgress) onProgress(done, total);
	}).then(function () {
		return {
			resolution: resolution,
			usedTriangleScalar: useScalar,
			faces: state.map(function (planes) {
				const rows = [];
				for (let v = 0; v < resolution; v++) {
					const row = [];
					for (let u = 0; u < resolution; u++) {
						row.push(planes.cells[v * resolution + u] || [0, 0, 0]);
					}
					rows.push(row);
				}
				return { name: planes.face.name, cells: rows };
			}),
		};
	});
}

// writeFaceHeader fills the 16 byte header: magic, version, fileSize, resolution
function writeFaceHeader(view, fileSize, resolution) {
	view.setUint32(0, FaceMaps.magic, true);
	view.setUint32(4, FaceMaps.version, true);
	view.setUint32(8, fileSize, true);
	view.setUint32(12, resolution, true);
}

// writeFaceMap writes the six faces into one file: header, then face, row (top first), cell RGB
// as float32, so a loader copies each face straight into an EmissionMap
function writeFaceMap(grids) {
	const resolution = grids.resolution;
	const floatsPerFace = resolution * resolution * 3;
	const total = FaceMaps.headerSize + grids.faces.length * floatsPerFace * 4;
	const bytes = new Uint8Array(total);
	const view = new DataView(bytes.buffer);
	writeFaceHeader(view, total, resolution);
	let offset = FaceMaps.headerSize;
	grids.faces.forEach(function (face) {
		face.cells.forEach(function (row) {
			row.forEach(function (cell) {
				view.setFloat32(offset, cell[0], true);
				view.setFloat32(offset + 4, cell[1], true);
				view.setFloat32(offset + 8, cell[2], true);
				offset += 12;
			});
		});
	});
	return new Blob([bytes], { type: "application/octet-stream" });
}

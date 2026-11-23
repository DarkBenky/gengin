// OBJ/MTL import. Records are written straight into the 108 byte layout, mirroring
// tools/parseObj.go so the output matches the canonical converter.

"use strict";

const ObjImport = {
	defaultTextureSize: 4096,
	minTextureSize: 256,
	maxTextureSize: 16384,
};

const DefaultMaterial = {
	name: "default",
	color: [0.8, 0.8, 0.8],
	emission: 0,
	roughness: 0.5,
	metallic: 0.5,
	baseColor: null, normal: null, roughMap: null, metalMap: null, metalRough: null,
	slot: -1, cx: 0, cy: 0,
};

// Growable buffer of fixed size records; the view is handed to the model without a copy.
function RecordWriter() {
	this.capacity = 4096;
	this.buffer = new ArrayBuffer(this.capacity * Rec.size);
	this.bytes = new Uint8Array(this.buffer);
	this.view = new DataView(this.buffer);
	this.count = 0;
}

RecordWriter.prototype.ensure = function () {
	if (this.count < this.capacity) return;
	const next = this.capacity * 2;
	const buffer = new ArrayBuffer(next * Rec.size);
	new Uint8Array(buffer).set(this.bytes);
	this.buffer = buffer;
	this.bytes = new Uint8Array(buffer);
	this.view = new DataView(buffer);
	this.capacity = next;
};

RecordWriter.prototype.push = function (v1, v2, v3, normal, material, uv1, uv2, uv3) {
	this.ensure();
	const base = this.count * Rec.size;
	const dv = this.view;
	dv.setFloat32(base + Rec.v1, v1[0], true);
	dv.setFloat32(base + Rec.v1 + 4, v1[1], true);
	dv.setFloat32(base + Rec.v1 + 8, v1[2], true);
	dv.setFloat32(base + Rec.v2, v2[0], true);
	dv.setFloat32(base + Rec.v2 + 4, v2[1], true);
	dv.setFloat32(base + Rec.v2 + 8, v2[2], true);
	dv.setFloat32(base + Rec.v3, v3[0], true);
	dv.setFloat32(base + Rec.v3 + 4, v3[1], true);
	dv.setFloat32(base + Rec.v3 + 8, v3[2], true);
	dv.setFloat32(base + Rec.normal, normal[0], true);
	dv.setFloat32(base + Rec.normal + 4, normal[1], true);
	dv.setFloat32(base + Rec.normal + 8, normal[2], true);
	dv.setFloat32(base + Rec.roughness, material.roughness, true);
	dv.setFloat32(base + Rec.metallic, material.metallic, true);
	dv.setFloat32(base + Rec.emission, material.emission, true);
	dv.setFloat32(base + Rec.color, material.color[0], true);
	dv.setFloat32(base + Rec.color + 4, material.color[1], true);
	dv.setFloat32(base + Rec.color + 8, material.color[2], true);
	dv.setUint16(base + Rec.uv, uv1[0], true);
	dv.setUint16(base + Rec.uv + 2, uv1[1], true);
	dv.setUint16(base + Rec.uv + 4, uv2[0], true);
	dv.setUint16(base + Rec.uv + 6, uv2[1], true);
	dv.setUint16(base + Rec.uv + 8, uv3[0], true);
	dv.setUint16(base + Rec.uv + 10, uv3[1], true);
	this.count++;
};

RecordWriter.prototype.recordsView = function () {
	return new Uint8Array(this.buffer, 0, this.count * Rec.size);
};

function normalize3(v) {	const length = Math.sqrt(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);
	if (length === 0) return [0, 0, 0];
	return [v[0] / length, v[1] / length, v[2] / length];
}

// listNames keeps a warning readable when a material file has many entries
function listNames(names) {
	if (names.length <= 3) return names.join(", ");
	return names.slice(0, 3).join(", ") + " (+" + (names.length - 3) + " more)";
}

function geometricNormal(v1, v2, v3) {
	const e1 = [v2[0] - v1[0], v2[1] - v1[1], v2[2] - v1[2]];
	const e2 = [v3[0] - v1[0], v3[1] - v1[1], v3[2] - v1[2]];
	return normalize3([
		e1[1] * e2[2] - e1[2] * e2[1],
		e1[2] * e2[0] - e1[0] * e2[2],
		e1[0] * e2[1] - e1[1] * e2[0],
	]);
}

// encodeUV mirrors encodeUV in parseObj.go: u clamped, v flipped because OBJ is bottom-up.
function encodeUv(u, v) {
	return [Math.round(clamp(u, 0, 1) * 65535), Math.round(clamp(1 - v, 0, 1) * 65535)];
}

function clipPolygonUv(poly, inside, intersect) {
	if (poly.length === 0) return [];
	const out = [];
	const n = poly.length;
	for (let i = 0; i < n; i++) {
		const cur = poly[i];
		const prev = poly[(i + n - 1) % n];
		const curIn = inside(cur);
		const prevIn = inside(prev);
		if (curIn) {
			if (!prevIn) out.push(intersect(prev, cur));
			out.push(cur);
		} else if (prevIn) {
			out.push(intersect(prev, cur));
		}
	}
	return out;
}

function clipPolygonUvToRect(poly, u0, u1, v0, v1) {
	const lerp = function (a, b, t) { return [a[0] + (b[0] - a[0]) * t, a[1] + (b[1] - a[1]) * t]; };
	poly = clipPolygonUv(poly, function (p) { return p[0] >= u0; }, function (a, b) {
		const d = b[0] - a[0];
		return d === 0 ? a : lerp(a, b, (u0 - a[0]) / d);
	});
	poly = clipPolygonUv(poly, function (p) { return p[0] <= u1; }, function (a, b) {
		const d = b[0] - a[0];
		return d === 0 ? a : lerp(a, b, (u1 - a[0]) / d);
	});
	poly = clipPolygonUv(poly, function (p) { return p[1] >= v0; }, function (a, b) {
		const d = b[1] - a[1];
		return d === 0 ? a : lerp(a, b, (v0 - a[1]) / d);
	});
	poly = clipPolygonUv(poly, function (p) { return p[1] <= v1; }, function (a, b) {
		const d = b[1] - a[1];
		return d === 0 ? a : lerp(a, b, (v1 - a[1]) / d);
	});
	return poly;
}

// appendUnfoldedTriangle mirrors the Go helper: a triangle whose UVs cross an integer tile
// border is clipped into per tile pieces and folded back into one tile, otherwise tiled UVs
// clamp to the atlas edge and smear. Splitting raises the triangle count.
function appendUnfoldedTriangle(writer, v1, v2, v3, normal, uv1, uv2, uv3, unfold, material, remap, cellInfo) {
	const push = function (a, b, c, t1, t2, t3) {
		writer.push(a, b, c, normal, material,
			remapUvPair(remap, t1, cellInfo), remapUvPair(remap, t2, cellInfo), remapUvPair(remap, t3, cellInfo));
	};

	if (!unfold) {
		push(v1, v2, v3, uv1, uv2, uv3);
		return 1;
	}

	const baseU = Math.floor(Math.min(uv1[0], Math.min(uv2[0], uv3[0])));
	const baseV = Math.floor(Math.min(uv1[1], Math.min(uv2[1], uv3[1])));
	const a1 = [uv1[0] - baseU, uv1[1] - baseV];
	const a2 = [uv2[0] - baseU, uv2[1] - baseV];
	const a3 = [uv3[0] - baseU, uv3[1] - baseV];
	const maxU = Math.max(a1[0], Math.max(a2[0], a3[0]));
	const maxV = Math.max(a1[1], Math.max(a2[1], a3[1]));

	if (maxU <= 1.0001 && maxV <= 1.0001) {
		push(v1, v2, v3, a1, a2, a3);
		return 1;
	}

	const denom = (a2[1] - a3[1]) * (a1[0] - a3[0]) + (a3[0] - a2[0]) * (a1[1] - a3[1]);
	const iMax = Math.floor(maxU);
	const jMax = Math.floor(maxV);
	if (denom === 0 || (iMax + 1) * (jMax + 1) > 256) {
		push(v1, v2, v3, a1, a2, a3);
		return 1;
	}

	const triUv = [a1, a2, a3];
	let emitted = 0;
	for (let i = 0; i <= iMax; i++) {
		for (let j = 0; j <= jMax; j++) {
			const poly = clipPolygonUvToRect(triUv.slice(), i, i + 1, j, j + 1);
			if (poly.length < 3) continue;
			for (let k = 1; k < poly.length - 1; k++) {
				const p = [poly[0], poly[k], poly[k + 1]];
				const verts = [];
				const uvs = [];
				for (let m = 0; m < 3; m++) {
					const w1 = ((a2[1] - a3[1]) * (p[m][0] - a3[0]) + (a3[0] - a2[0]) * (p[m][1] - a3[1])) / denom;
					const w2 = ((a3[1] - a1[1]) * (p[m][0] - a3[0]) + (a1[0] - a3[0]) * (p[m][1] - a3[1])) / denom;
					const w3 = 1 - w1 - w2;
					verts.push([
						v1[0] * w1 + v2[0] * w2 + v3[0] * w3,
						v1[1] * w1 + v2[1] * w2 + v3[1] * w3,
						v1[2] * w1 + v2[2] * w2 + v3[2] * w3,
					]);
					uvs.push([p[m][0] - i, p[m][1] - j]);
				}
				push(verts[0], verts[1], verts[2], uvs[0], uvs[1], uvs[2]);
				emitted++;
			}
		}
	}
	if (emitted === 0) {
		push(v1, v2, v3, a1, a2, a3);
		return 1;
	}
	return emitted;
}

function remapUvPair(remap, uv, cellInfo) {
	if (!remap) return encodeUv(uv[0], uv[1]);
	const encoded = encodeUv(uv[0], uv[1]);
	return [remap(encoded[0], cellInfo.cx), remap(encoded[1], cellInfo.cy)];
}

// parseObjText is the tokenizer; it keeps faces as flat index arrays so the whole file is read
// once before any geometry work starts.
function parseObjText(text) {
	const vertices = [];
	const normals = [];
	const uvCoords = [];
	const faces = [];
	const faceNormals = [];
	const faceUvs = [];
	const faceMaterials = [];
	let currentMaterial = "";
	let mtllib = "";

	const lines = text.split(/\r?\n/);
	for (let i = 0; i < lines.length; i++) {
		const line = lines[i].trim();
		if (line === "" || line.charAt(0) === "#") continue;
		const tokens = line.split(/\s+/);
		const key = tokens[0];

		if (key === "v" && tokens.length >= 4) {
			const x = parseFloat(tokens[1]), y = parseFloat(tokens[2]), z = parseFloat(tokens[3]);
			if (isFinite(x) && isFinite(y) && isFinite(z)) vertices.push([x, y, z]);
		} else if (key === "vt" && tokens.length >= 3) {
			const u = parseFloat(tokens[1]), v = parseFloat(tokens[2]);
			if (isFinite(u) && isFinite(v)) uvCoords.push([u, v]);
		} else if (key === "vn" && tokens.length >= 4) {
			const x = parseFloat(tokens[1]), y = parseFloat(tokens[2]), z = parseFloat(tokens[3]);
			if (isFinite(x) && isFinite(y) && isFinite(z)) normals.push(normalize3([x, y, z]));
		} else if (key === "usemtl" && tokens.length >= 2) {
			currentMaterial = tokens.slice(1).join(" ");
		} else if (key === "mtllib" && tokens.length >= 2) {
			// only the file name is used, to find the .mtl among the supplied companion files
			mtllib = tokens[tokens.length - 1];
		} else if (key === "f" && tokens.length >= 4) {
			const vi = [], ni = [], ti = [];
			for (let t = 1; t < tokens.length; t++) {
				const parts = tokens[t].split("/");
				if (parts.length > 0 && parts[0] !== "") {
					let idx = parseInt(parts[0], 10);
					if (isFinite(idx)) {
						if (idx < 0) idx = vertices.length + idx + 1;
						if (idx > 0 && idx <= vertices.length) vi.push(idx - 1);
					}
				}
				if (parts.length >= 2 && parts[1] !== "") {
					let uvIndex = parseInt(parts[1], 10);
					if (isFinite(uvIndex)) {
						if (uvIndex < 0) uvIndex = uvCoords.length + uvIndex + 1;
						ti.push(uvIndex - 1);
					}
				}
				if (parts.length >= 3 && parts[2] !== "") {
					let nIndex = parseInt(parts[2], 10);
					if (isFinite(nIndex)) {
						if (nIndex < 0) nIndex = normals.length + nIndex + 1;
						ni.push(nIndex - 1);
					}
				}
			}
			if (vi.length >= 3) {
				faces.push(vi);
				faceNormals.push(ni);
				faceUvs.push(ti);
				faceMaterials.push(currentMaterial);
			}
		}
	}

	return { vertices: vertices, normals: normals, uvCoords: uvCoords, faces: faces, faceNormals: faceNormals, faceUvs: faceUvs, faceMaterials: faceMaterials, mtllib: mtllib };
}

// findMtlFile tries the OBJ base name first, then the name the OBJ declares, then any single
// .mtl that was supplied: a differently named material file is still better than nothing
function findMtlFile(objFileName, mtllib, filesByName) {
	const base = baseNameOf(objFileName);
	const companion = filesByName.get(base + ".mtl");
	if (companion) return companion;
	if (mtllib) {
		const declared = filesByName.get(baseNameOf(mtllib) + ".mtl");
		if (declared) return declared;
	}
	let only = null;
	let count = 0;
	filesByName.forEach(function (file, name) {
		if (!/\.mtl$/i.test(name)) return;
		only = file;
		count++;
	});
	return count === 1 ? only : null;
}

// importObjFile turns an OBJ plus its companion files into a model in the in-memory container
// form. Progress is reported per face chunk so a 260k face import stays responsive.
function importObjFile(objFile, otherFiles, textureSize, onProgress) {
	return readFileAsText(objFile).then(function (objText) {
		return importObjFromText(objText, objFile.name, otherFiles, textureSize, onProgress);
	});
}

function importObjFromText(objText, objFileName, otherFiles, textureSize, onProgress) {
	const requested = textureSize || ObjImport.defaultTextureSize;
	if (requested < ObjImport.minTextureSize || requested > ObjImport.maxTextureSize) {
		return Promise.reject(FormatError("texture size must be between " + ObjImport.minTextureSize +
			" and " + ObjImport.maxTextureSize));
	}

	const filesByName = new Map();
	const availableImages = new Map();
	for (let i = 0; i < otherFiles.length; i++) {
		const file = otherFiles[i];
		const rel = relativePathOf(file);
		const name = rel.slice(rel.lastIndexOf("/") + 1);
		filesByName.set(name, file);
		if (/\.(png|jpg|jpeg)$/i.test(name)) {
			availableImages.set(name, file);
			availableImages.set(rel, file);
		}
	}

	const obj = parseObjText(objText);
	if (obj.faces.length === 0) throw FormatError("no faces found in " + objFileName);

	const mtlFile = findMtlFile(objFileName, obj.mtllib, filesByName);
	const mtlPromise = mtlFile ? readFileAsText(mtlFile) : Promise.resolve(null);

	return mtlPromise.then(function (mtlText) {
		const infos = mtlText ? parseMtl(mtlText, availableImages, "") : [];
		const warnings = [];
		if (!mtlText) {
			warnings.push("no .mtl supplied for " + objFileName +
				", using default material; add it with \"Add .mtl / images\"");
		} else if (mtlFile.name.toLowerCase() !== baseNameOf(objFileName).toLowerCase() + ".mtl") {
			warnings.push("paired " + objFileName + " with " + mtlFile.name);
		}

		const slotByName = new Map();
		for (let i = 0; i < infos.length; i++) slotByName.set(infos[i].name, i);

		// an MTL only applies to faces whose usemtl names a material it defines, so report the
		// mismatch instead of silently falling back to the default material
		const usedNames = new Set();
		for (let i = 0; i < obj.faceMaterials.length; i++) {
			if (obj.faceMaterials[i] !== "") usedNames.add(obj.faceMaterials[i]);
		}
		const unmatched = [];
		usedNames.forEach(function (name) { if (!slotByName.has(name)) unmatched.push(name); });
		if (unmatched.length > 0) {
			infos.push(Object.assign({}, DefaultMaterial, { name: "__default__" }));
			slotByName.set("__default__", infos.length - 1);
			warnings.push("no material named " + listNames(unmatched) + " in " +
				(mtlFile ? mtlFile.name : "the .mtl") + "; those faces use the default material");
		}
		const anyMatched = usedNames.size > unmatched.length;
		const useAtlas = anyMatched && atlasHasSources(infos);
		if (!anyMatched && mtlText && atlasHasSources(infos)) {
			warnings.push("the .mtl materials are not used by this .obj, so no atlas was built");
		}
		const missingMaps = new Set();
		infos.forEach(function (info) {
			(info.missingMaps || []).forEach(function (name) { missingMaps.add(name); });
		});
		if (missingMaps.size > 0) {
			warnings.push("missing texture files: " + listNames(Array.from(missingMaps)) +
				"; add them with \"Add .mtl / images\"");
		}
		const slots = useAtlas ? assignAtlasSlots(infos, requested) : { grid: 1, cell: requested };
		const remap = useAtlas ? makeUvRemapper(slots.cell, requested) : null;

		const writer = new RecordWriter();
		let unfoldedExtra = 0;

		function range(start, end) {
			for (let faceIndex = start; faceIndex < end; faceIndex++) {
				const face = obj.faces[faceIndex];
				let material = DefaultMaterial;
				const materialName = faceIndex < obj.faceMaterials.length ? obj.faceMaterials[faceIndex] : "";
				if (materialName !== "") {
					const slot = slotByName.get(materialName);
					if (slot !== undefined) material = infos[slot];
					else {
						const fallback = slotByName.get("__default__");
						if (fallback !== undefined) material = infos[fallback];
					}
				}

				const faceNormalIndices = obj.faceNormals[faceIndex];
				const faceUvIndices = obj.faceUvs[faceIndex];
				const count = face.length;

				for (let i = 1; i < count - 1; i++) {
					const i0 = 0, i1 = i, i2 = i + 1;
					const v1 = obj.vertices[face[i0]];
					const v2 = obj.vertices[face[i1]];
					const v3 = obj.vertices[face[i2]];

					let normal;
					if (faceNormalIndices.length === count &&
						faceNormalIndices[i0] >= 0 && faceNormalIndices[i0] < obj.normals.length &&
						faceNormalIndices[i1] >= 0 && faceNormalIndices[i1] < obj.normals.length &&
						faceNormalIndices[i2] >= 0 && faceNormalIndices[i2] < obj.normals.length) {
						const n0 = obj.normals[faceNormalIndices[i0]];
						const n1 = obj.normals[faceNormalIndices[i1]];
						const n2 = obj.normals[faceNormalIndices[i2]];
						normal = normalize3([(n0[0] + n1[0] + n2[0]) / 3, (n0[1] + n1[1] + n2[1]) / 3, (n0[2] + n1[2] + n2[2]) / 3]);
					} else {
						normal = geometricNormal(v1, v2, v3);
					}

					const uvA = uvAt(obj, faceUvIndices, i0);
					const uvB = uvAt(obj, faceUvIndices, i1);
					const uvC = uvAt(obj, faceUvIndices, i2);
					// without an atlas the OBJ UVs are still stored (u clamped, v flipped), so the
					// model can be textured later instead of collapsing onto one texel
					const unfold = useAtlas && uvA && uvB && uvC;
					const emitted = appendUnfoldedTriangle(writer, v1, v2, v3, normal,
						uvA || [0, 0], uvB || [0, 0], uvC || [0, 0],
						unfold, material, remap, material);
					unfoldedExtra += emitted - 1;
				}
			}
		}

		return runChunked(obj.faces.length, 8192, range, function (done, total) {
			if (onProgress) onProgress("Building triangles: " + formatCount(done) + " / " + formatCount(total) + " faces");
		}).then(function () {
			const model = {
				fileName: baseNameOf(objFileName) + ".bin",
				legacy: false,
				buffer: null,
				triView: writer.recordsView(),
				triCount: writer.count,
				atlasView: null,
				textureSize: useAtlas ? requested : 0,
				flags: 0,
				cullBackfaces: false,
				groups: null,
				sourceHandle: null,
			};

			if (!useAtlas) {
				if (unfoldedExtra > 0) warnings.push("tiled UVs split " + formatCount(unfoldedExtra) + " extra triangles");
				model.warnings = warnings;
				return model;
			}

			const atlasBytes = requested * requested * BinFormat.bytesPerTexel;
			if (atlasBytes > BinFormat.maxAtlasBytes) throw FormatError("atlas too large: " + formatBytes(atlasBytes));
			const atlasView = new Uint8Array(atlasBytes);
			return buildAtlas(atlasView, requested, infos, slots.cell, availableImages, function (done, total) {
				if (onProgress) onProgress("Packing atlas: material " + done + " / " + total);
			}).then(function (loadedAny) {
				if (!loadedAny) {
					model.textureSize = 0;
					if (onProgress) onProgress("no texture image could be decoded, writing without a texture block");
				} else {
					model.atlasView = atlasView;
					model.flags |= BinFormat.flagTextured;
				}
				if (unfoldedExtra > 0) warnings.push("tiled UVs split " + formatCount(unfoldedExtra) + " extra triangles");
				warnings.push("atlas grid " + slots.grid + "x" + slots.grid + ", cells " + slots.cell + "px");
				model.warnings = warnings;
				return model;
			});
		});
	});
}

function uvAt(obj, faceUvIndices, slot) {
	if (slot < faceUvIndices.length && faceUvIndices[slot] >= 0 && faceUvIndices[slot] < obj.uvCoords.length) {
		return obj.uvCoords[faceUvIndices[slot]];
	}
	return null;
}

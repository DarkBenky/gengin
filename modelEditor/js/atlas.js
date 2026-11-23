// Atlas packing: one square cell per material, written straight into the 9 byte per texel
// layout the .bin uses (colorMap RGBA, normalMap RGB, materialMap [rough, metal]).

"use strict";

const FlatNormal = [128, 128, 255];
const ImageExtensions = [".png", ".jpg", ".jpeg"];

// probeName mirrors probeFile in parseObj.go: Blender writes map_Bump but not map_Kd when the
// image is not linked to Base Color, so the glTF style file names are probed as well.
function probeName(availableImages, base) {
	if (!base) return null;
	for (let i = 0; i < ImageExtensions.length; i++) {
		const candidate = availableImages.get(base + ImageExtensions[i]);
		if (candidate) return candidate;
	}
	return null;
}

// MTL map lines may carry options ("-bm 1.0 file.png"), so the last token is the file name.
function mapValueFromTokens(tokens) {
	return tokens.length >= 2 ? tokens[tokens.length - 1] : "";
}

function parseMtl(text, availableImages, mtlDir) {
	const materials = [];
	let current = null;

	function finish() {
		if (current) materials.push(current);
		current = null;
	}

	const lines = text.split(/\r?\n/);
	for (let i = 0; i < lines.length; i++) {
		const line = lines[i].trim();
		if (line === "" || line.charAt(0) === "#") continue;
		const tokens = line.split(/\s+/);
		const key = tokens[0];

		if (key === "newmtl") {
			finish();
			if (tokens.length < 2) continue;
			current = {
				name: tokens.slice(1).join(" "),
				color: [0.8, 0.8, 0.8],
				emission: 0,
				Ns: 0,
				Pr: null,
				Pm: null,
				mapKd: "", mapBump: "", mapNs: "", mapRefl: "", mapPr: "", mapPm: "",
			};
			continue;
		}
		if (!current) continue;

		if (key === "Kd" && tokens.length >= 4) {
			current.color = [parseFloat(tokens[1]), parseFloat(tokens[2]), parseFloat(tokens[3])];
		} else if (key === "Ke" && tokens.length >= 4) {
			// parseObj.go reads Ke and drops it; the editor keeps it as the scalar emission
			current.emission = Math.max(parseFloat(tokens[1]), parseFloat(tokens[2]), parseFloat(tokens[3]));
		} else if (key === "Ns" && tokens.length >= 2) {
			current.Ns = parseFloat(tokens[1]);
		} else if (key === "Pr" && tokens.length >= 2) {
			current.Pr = parseFloat(tokens[1]);
		} else if (key === "Pm" && tokens.length >= 2) {
			current.Pm = parseFloat(tokens[1]);
		} else if (key === "map_Kd") {
			current.mapKd = mapValueFromTokens(tokens);
		} else if (key === "map_Bump" || key === "bump") {
			current.mapBump = mapValueFromTokens(tokens);
		} else if (key === "map_Ns") {
			current.mapNs = mapValueFromTokens(tokens);
		} else if (key === "map_refl") {
			current.mapRefl = mapValueFromTokens(tokens);
		} else if (key === "map_Pr") {
			current.mapPr = mapValueFromTokens(tokens);
		} else if (key === "map_Pm") {
			current.mapPm = mapValueFromTokens(tokens);
		}
	}
	finish();

	const infos = [];
	for (let i = 0; i < materials.length; i++) {
		const mat = materials[i];
		let roughness = 0.5;
		if (mat.Pr !== null && isFinite(mat.Pr)) roughness = clamp(mat.Pr, 0, 1);
		else if (mat.Ns > 0) roughness = clamp(1.0 - mat.Ns / 1000.0, 0.02, 1);
		let metallic = 0;
		if (mat.Pm !== null && isFinite(mat.Pm)) metallic = clamp(mat.Pm, 0, 1);

		const baseColor = resolveTexture(availableImages, mtlDir, mat.mapKd) ||
			probeName(availableImages, mat.name + "_baseColor");
		const normal = resolveTexture(availableImages, mtlDir, mat.mapBump) ||
			probeName(availableImages, mat.name + "_normal");
		let roughMap = resolveTexture(availableImages, mtlDir, mat.mapNs) ||
			resolveTexture(availableImages, mtlDir, mat.mapPr);
		let metalMap = resolveTexture(availableImages, mtlDir, mat.mapRefl) ||
			resolveTexture(availableImages, mtlDir, mat.mapPm);
		let metalRough = null;
		if (!roughMap && !metalMap) metalRough = probeName(availableImages, mat.name + "_metallicRoughness");

		const missingMaps = [];
		[mat.mapKd, mat.mapBump, mat.mapNs, mat.mapPr, mat.mapRefl, mat.mapPm].forEach(function (name) {
			if (name && !resolveTexture(availableImages, mtlDir, name)) missingMaps.push(name);
		});

		infos.push({
			name: mat.name,
			color: mat.color,
			emission: mat.emission,
			roughness: roughness,
			metallic: metallic,
			baseColor: baseColor,
			normal: normal,
			roughMap: roughMap,
			metalMap: metalMap,
			metalRough: metalRough,
			missingMaps: missingMaps,
			slot: -1, cx: 0, cy: 0,
		});
	}
	return infos;
}

// Texture names in an MTL are relative to the MTL; a browser drop only gives file names, so
// match on the base name and keep the relative path for display.
function resolveTexture(availableImages, mtlDir, name) {
	if (!name) return null;
	const direct = availableImages.get(name);
	if (direct) return direct;
	const slash = Math.max(name.lastIndexOf("/"), name.lastIndexOf("\\"));
	const base = slash >= 0 ? name.slice(slash + 1) : name;
	return availableImages.get(base) || null;
}

function atlasHasSources(infos) {
	for (let i = 0; i < infos.length; i++) {
		const inf = infos[i];
		if (inf.baseColor || inf.normal || inf.metalRough || inf.roughMap || inf.metalMap) return true;
	}
	return false;
}

// assignAtlasSlots mirrors assignAtlasSlots in parseObj.go, including the 64 grid cap.
function assignAtlasSlots(infos, texSize) {
	let grid = 1;
	while (grid * grid < infos.length) grid++;
	while (texSize % grid !== 0 && grid < 64) grid++;
	const cell = Math.floor(texSize / grid);
	for (let i = 0; i < infos.length; i++) {
		infos[i].slot = i;
		infos[i].cx = i % grid;
		infos[i].cy = Math.floor(i / grid);
	}
	return { grid: grid, cell: cell };
}

function decodeImage(file) {
	return createImageBitmap(file).then(function (bitmap) {
		const canvas = document.createElement("canvas");
		canvas.width = bitmap.width;
		canvas.height = bitmap.height;
		const ctx = canvas.getContext("2d", { willReadFrequently: true });
		ctx.drawImage(bitmap, 0, 0);
		bitmap.close();
		const data = ctx.getImageData(0, 0, canvas.width, canvas.height);
		canvas.width = canvas.height = 0;
		return { width: canvas.width || data.width, height: data.height, pixels: data.data };
	});
}

// Same half pixel bilinear as resizeBilinear in parseObj.go, so the atlas matches the Go tool.
function resampleBilinear(src, srcW, srcH, outW, outH) {
	const out = new Uint8Array(outW * outH * 4);
	for (let y = 0; y < outH; y++) {
		const srcY = (y + 0.5) * srcH / outH - 0.5;
		let y0 = Math.floor(srcY);
		let y1 = y0 + 1;
		if (y0 < 0) y0 = 0;
		if (y1 >= srcH) y1 = srcH - 1;
		let fy = srcY - Math.floor(srcY);
		if (fy < 0) fy = 0;
		for (let x = 0; x < outW; x++) {
			const srcX = (x + 0.5) * srcW / outW - 0.5;
			let x0 = Math.floor(srcX);
			let x1 = x0 + 1;
			if (x0 < 0) x0 = 0;
			if (x1 >= srcW) x1 = srcW - 1;
			let fx = srcX - Math.floor(srcX);
			if (fx < 0) fx = 0;

			const p00 = (y0 * srcW + x0) * 4;
			const p10 = (y0 * srcW + x1) * 4;
			const p01 = (y1 * srcW + x0) * 4;
			const p11 = (y1 * srcW + x1) * 4;
			const dst = (y * outW + x) * 4;
			for (let c = 0; c < 4; c++) {
				const top = src[p00 + c] + (src[p10 + c] - src[p00 + c]) * fx;
				const bottom = src[p01 + c] + (src[p11 + c] - src[p01 + c]) * fx;
				out[dst + c] = top + (bottom - top) * fy;
			}
		}
	}
	return out;
}

function fillCellColor(atlasView, texSize, ox, oy, cell, rgba) {
	for (let y = 0; y < cell; y++) {
		let dst = ((oy + y) * texSize + ox) * 4;
		for (let x = 0; x < cell; x++, dst += 4) {
			atlasView[dst] = rgba[0];
			atlasView[dst + 1] = rgba[1];
			atlasView[dst + 2] = rgba[2];
			atlasView[dst + 3] = rgba[3];
		}
	}
}

function fillCellNormal(atlasView, texSize, ox, oy, cell, rgb) {
	const plane = texSize * texSize * 4;
	for (let y = 0; y < cell; y++) {
		let dst = plane + ((oy + y) * texSize + ox) * 3;
		for (let x = 0; x < cell; x++, dst += 3) {
			atlasView[dst] = rgb[0];
			atlasView[dst + 1] = rgb[1];
			atlasView[dst + 2] = rgb[2];
		}
	}
}

function fillCellMaterial(atlasView, texSize, ox, oy, cell, roughByte, metalByte) {
	const plane = texSize * texSize * 7;
	for (let y = 0; y < cell; y++) {
		let dst = plane + ((oy + y) * texSize + ox) * 2;
		for (let x = 0; x < cell; x++, dst += 2) {
			atlasView[dst] = roughByte;
			atlasView[dst + 1] = metalByte;
		}
	}
}

function overlayCellColor(atlasView, texSize, ox, oy, cell, pixels) {
	for (let y = 0; y < cell; y++) {
		let dst = ((oy + y) * texSize + ox) * 4;
		let src = (y * cell) * 4;
		for (let x = 0; x < cell; x++, dst += 4, src += 4) {
			atlasView[dst] = pixels[src];
			atlasView[dst + 1] = pixels[src + 1];
			atlasView[dst + 2] = pixels[src + 2];
			atlasView[dst + 3] = pixels[src + 3];
		}
	}
}

function overlayCellNormal(atlasView, texSize, ox, oy, cell, pixels) {
	const plane = texSize * texSize * 4;
	for (let y = 0; y < cell; y++) {
		let dst = plane + ((oy + y) * texSize + ox) * 3;
		let src = (y * cell) * 4;
		for (let x = 0; x < cell; x++, dst += 3, src += 4) {
			atlasView[dst] = pixels[src];
			atlasView[dst + 1] = pixels[src + 1];
			atlasView[dst + 2] = pixels[src + 2];
		}
	}
}

// buildAtlas loads every material's images one at a time and resamples them into its cell, so
// only one decoded image is resident at a time on top of the atlas itself.
function buildAtlas(atlasView, texSize, infos, cell, availableImages, onProgress) {
	const texelCount = texSize * texSize;
	for (let i = 0; i < texelCount; i++) {
		const dst = texelCount * 4 + i * 3;
		atlasView[dst] = FlatNormal[0];
		atlasView[dst + 1] = FlatNormal[1];
		atlasView[dst + 2] = FlatNormal[2];
	}

	let loadedAny = false;
	let index = 0;

	function loadResampled(entry) {
		if (!entry) return Promise.resolve(null);
		// parseMtl already resolves names to File handles; only bare names need the lookup
		const file = typeof entry === "string" ? availableImages.get(entry) : entry;
		if (!file) return Promise.resolve(null);
		return decodeImage(file).then(function (img) {
			return resampleBilinear(img.pixels, img.width, img.height, cell, cell);
		}).catch(function () { return null; });
	}

	function step() {
		if (index >= infos.length) {
			if (onProgress) onProgress(infos.length, infos.length);
			return Promise.resolve(loadedAny);
		}
		const inf = infos[index];
		index++;
		const ox = inf.cx * cell;
		const oy = inf.cy * cell;

		const kdR = clamp(inf.color[0], 0, 1) * 255;
		const kdG = clamp(inf.color[1], 0, 1) * 255;
		const kdB = clamp(inf.color[2], 0, 1) * 255;
		fillCellColor(atlasView, texSize, ox, oy, cell, [kdR, kdG, kdB, 255]);
		fillCellNormal(atlasView, texSize, ox, oy, cell, FlatNormal);
		const roughByte = clamp(inf.roughness, 0, 1) * 255;
		const metalByte = clamp(inf.metallic, 0, 1) * 255;
		fillCellMaterial(atlasView, texSize, ox, oy, cell, roughByte, metalByte);

		return loadResampled(inf.baseColor).then(function (pixels) {
			if (pixels) { loadedAny = true; overlayCellColor(atlasView, texSize, ox, oy, cell, pixels); }
			return loadResampled(inf.normal);
		}).then(function (pixels) {
			if (pixels) { loadedAny = true; overlayCellNormal(atlasView, texSize, ox, oy, cell, pixels); }
			if (inf.metalRough) {
				return loadResampled(inf.metalRough).then(function (packed) {
					if (!packed) return null;
					loadedAny = true;
					const plane = texSize * texSize * 7;
					for (let y = 0; y < cell; y++) {
						for (let x = 0; x < cell; x++) {
							const src = (y * cell + x) * 4;
							const dst = plane + ((oy + y) * texSize + ox + x) * 2;
							atlasView[dst] = packed[src + 1];
							atlasView[dst + 1] = packed[src + 2];
						}
					}
					return null;
				});
			}
			if (!inf.roughMap && !inf.metalMap) return null;
			return loadResampled(inf.roughMap).then(function (rough) {
				return loadResampled(inf.metalMap).then(function (metal) {
					if (!rough && !metal) return null;
					loadedAny = true;
					const plane = texSize * texSize * 7;
					for (let y = 0; y < cell; y++) {
						for (let x = 0; x < cell; x++) {
							const j = y * cell + x;
							const dst = plane + ((oy + y) * texSize + ox + x) * 2;
							atlasView[dst] = rough ? rough[j * 4] : roughByte;
							atlasView[dst + 1] = metal ? metal[j * 4] : metalByte;
						}
					}
					return null;
				});
			});
		}).then(function () {
			if (onProgress) onProgress(index, infos.length);
			return new Promise(function (resolve) { setTimeout(function () { resolve(step()); }, 0); });
		});
	}

	return step();
}

// remapTriangleUVs mirrors the Go helper: work in texture pixels so a cell edge lands on the
// cell's first/last pixel instead of bleeding into the neighbour.
function makeUvRemapper(cell, texSize) {
	const edge = 0.9995;
	const lastPixel = texSize - 1;
	const storedPerPixel = 65535.0 / lastPixel;
	return function (storedUv, cellIndex) {
		let t = storedUv / 65535.0;
		if (t > edge) t = edge;
		let p = cellIndex * cell + t * cell;
		if (p > lastPixel) p = lastPixel;
		let s = Math.floor(p * storedPerPixel) + 1;
		if (s > 65535) s = 65535;
		return s;
	};
}

// modelHasUvSpread reports whether the UVs cover more than a single texel. A model without vt
// data collapses every triangle onto one corner, and an atlas would be unusable.
function modelHasUvSpread(model, texSize) {
	const dv = new DataView(model.triView.buffer, model.triView.byteOffset, model.triView.byteLength);
	let minU = 65535, maxU = 0, minV = 65535, maxV = 0;
	const limit = Math.min(model.triCount, 4096);
	for (let t = 0; t < limit; t++) {
		const base = t * Rec.size + Rec.uv;
		for (let corner = 0; corner < 3; corner++) {
			const u = dv.getUint16(base + corner * 4, true);
			const v = dv.getUint16(base + corner * 4 + 2, true);
			if (u < minU) minU = u;
			if (u > maxU) maxU = u;
			if (v < minV) minV = v;
			if (v > maxV) maxV = v;
		}
	}
	return uvToTexel(maxU, texSize) - uvToTexel(minU, texSize) > 0 ||
		uvToTexel(maxV, texSize) - uvToTexel(minV, texSize) > 0;
}

// bakeAtlasFromTriangles gives an untextured model an atlas built from what it already has: every
// texel a triangle covers takes that triangle's color, roughness and metallic, so the brush has
// something to paint on and the preview looks the same as before the bake.
function bakeAtlasFromTriangles(model, texSize, onProgress) {
	const texelCount = texSize * texSize;
	const atlas = new Uint8Array(texelCount * BinFormat.bytesPerTexel);
	for (let i = 0; i < texelCount; i++) {
		const n = texelCount * AtlasPlane.normal + i * 3;
		atlas[n] = FlatNormal[0];
		atlas[n + 1] = FlatNormal[1];
		atlas[n + 2] = FlatNormal[2];
	}

	const dv = new DataView(model.triView.buffer, model.triView.byteOffset, model.triView.byteLength);
	const last = texSize - 1;
	const edgeEpsilon = -0.0001;

	function bakeRange(start, end) {
		for (let t = start; t < end; t++) {
			const base = t * Rec.size;
			const u1 = uvToTexel(dv.getUint16(base + Rec.uv, true), texSize);
			const v1 = uvToTexel(dv.getUint16(base + Rec.uv + 2, true), texSize);
			const u2 = uvToTexel(dv.getUint16(base + Rec.uv + 4, true), texSize);
			const v2 = uvToTexel(dv.getUint16(base + Rec.uv + 6, true), texSize);
			const u3 = uvToTexel(dv.getUint16(base + Rec.uv + 8, true), texSize);
			const v3 = uvToTexel(dv.getUint16(base + Rec.uv + 10, true), texSize);

			const det = (v2 - v3) * (u1 - u3) + (u3 - u2) * (v1 - v3);
			if (det === 0) continue;

			const x0 = Math.max(0, Math.min(u1, Math.min(u2, u3)));
			const x1 = Math.min(last, Math.max(u1, Math.max(u2, u3)));
			const y0 = Math.max(0, Math.min(v1, Math.min(v2, v3)));
			const y1 = Math.min(last, Math.max(v1, Math.max(v2, v3)));

			const red = clamp(dv.getFloat32(base + Rec.color, true), 0, 1) * 255;
			const green = clamp(dv.getFloat32(base + Rec.color + 4, true), 0, 1) * 255;
			const blue = clamp(dv.getFloat32(base + Rec.color + 8, true), 0, 1) * 255;
			const rough = clamp(dv.getFloat32(base + Rec.roughness, true), 0, 1) * 255;
			const metal = clamp(dv.getFloat32(base + Rec.metallic, true), 0, 1) * 255;

			for (let y = y0; y <= y1; y++) {
				const py = y + 0.5;
				for (let x = x0; x <= x1; x++) {
					const px = x + 0.5;
					const w1 = ((v2 - v3) * (px - u3) + (u3 - u2) * (py - v3)) / det;
					if (w1 < edgeEpsilon || w1 > 1) continue;
					const w2 = ((v3 - v1) * (px - u3) + (u1 - u3) * (py - v3)) / det;
					if (w2 < edgeEpsilon || w1 + w2 > 1 + edgeEpsilon) continue;

					const texel = y * texSize + x;
					const c = texel * 4;
					atlas[c] = red;
					atlas[c + 1] = green;
					atlas[c + 2] = blue;
					atlas[c + 3] = 255;
					const m = texelCount * AtlasPlane.material + texel * 2;
					atlas[m] = rough;
					atlas[m + 1] = metal;
				}
			}
		}
	}

	return runChunked(model.triCount, 4096, bakeRange, function (done, total) {
		if (onProgress) onProgress(done, total);
	}).then(function () {
		return atlas;
	});
}

// .bin reader and writer. The whole file lives in one ArrayBuffer and every access goes through
// views over it, so a load/save round trip copies bytes instead of re-encoding floats.

"use strict";

const BinFormat = {
	headerSize: 20,
	legacyHeaderSize: 16,
	recordSize: 108,
	obsoleteRecordSize: 92,
	flagTextured: 1,
	flagCullBackfaces: 2,
	minTextureSize: 256,
	maxTextureSize: 16384,
	legacyTextureSize: 4096,
	bytesPerTexel: 9,
	maxAtlasBytes: 16384 * 16384 * 9,
};

// byte offsets inside one 108 byte triangle record
const Rec = {
	v1: 0,
	v2: 16,
	v3: 32,
	normal: 48,
	roughness: 64,
	metallic: 68,
	emission: 72,
	color: 76,
	uv: 92,
	size: 108,
};

const AtlasPlane = { color: 0, normal: 4, material: 7 };

function expectedCurrentSize(triangleCount, flags, textureSize) {
	const atlas = (flags & BinFormat.flagTextured) ? textureSize * textureSize * BinFormat.bytesPerTexel : 0;
	return BinFormat.headerSize + triangleCount * BinFormat.recordSize + atlas;
}

function expectedLegacySize(triangleCount, flags) {
	const atlas = (flags & BinFormat.flagTextured)
		? BinFormat.legacyTextureSize * BinFormat.legacyTextureSize * BinFormat.bytesPerTexel
		: 0;
	return BinFormat.legacyHeaderSize + triangleCount * BinFormat.recordSize + atlas;
}

// Mirrors LoadObj: the textureSize field is only trusted when it makes the size equation hold,
// otherwise the file is re-read as a legacy 16 byte header with a fixed 4096 atlas.
function resolveLayout(fileSize, recordSize, triangleCount, flags, sizeField) {
	const textured = flags & BinFormat.flagTextured;
	if (recordSize === BinFormat.obsoleteRecordSize) {
		return { ok: false, message: "old/unsupported .bin: triangle records are " + recordSize +
			" bytes, this editor only reads the 108 byte layout" };
	}
	if (recordSize !== BinFormat.recordSize) {
		return { ok: false, message: "not a gengin .bin: record size " + recordSize +
			" is neither the current 108 nor the obsolete 92 byte layout" };
	}
	if (sizeField >= BinFormat.minTextureSize && sizeField <= BinFormat.maxTextureSize) {
		const expected = expectedCurrentSize(triangleCount, flags, sizeField);
		if (expected === fileSize) return { ok: true, headerSize: BinFormat.headerSize, textureSize: sizeField };
		if (textured) {
			return { ok: false, message: "truncated or corrupt .bin: header says " + formatBytes(fileSize) +
				" bytes but " + formatCount(triangleCount) + " triangles plus a " + sizeField +
				" atlas need " + formatBytes(expected) };
		}
	}
	const legacyExpected = expectedLegacySize(triangleCount, flags);
	if (legacyExpected === fileSize) {
		return { ok: true, headerSize: BinFormat.legacyHeaderSize, textureSize: BinFormat.legacyTextureSize, legacy: true };
	}
	return { ok: false, message: "truncated or corrupt .bin: header says " + formatBytes(fileSize) +
		" bytes but " + formatCount(triangleCount) + " triangles need " + formatBytes(legacyExpected) +
		" in the legacy layout" };
}

function readBinModel(arrayBuffer, fileName) {
	if (arrayBuffer.byteLength < BinFormat.headerSize) {
		throw FormatError("not a model file: only " + arrayBuffer.byteLength + " bytes");
	}
	const header = new DataView(arrayBuffer);
	const fileSize = header.getUint32(0, true);
	const recordSize = header.getUint32(4, true);
	const triangleCount = header.getUint32(8, true);
	const flags = header.getUint32(12, true);
	const sizeField = header.getUint32(16, true);

	const layout = resolveLayout(fileSize, recordSize, triangleCount, flags, sizeField);
	if (!layout.ok) throw FormatError(layout.message);
	if (arrayBuffer.byteLength !== fileSize) {
		throw FormatError("truncated or corrupt .bin: file is " + formatBytes(arrayBuffer.byteLength) +
			" bytes but the header declares " + formatBytes(fileSize));
	}
	if (triangleCount > 0 && triangleCount * BinFormat.recordSize > arrayBuffer.byteLength) {
		throw FormatError("truncated or corrupt .bin: triangle count " + formatCount(triangleCount) +
			" does not fit the file");
	}

	const textured = (flags & BinFormat.flagTextured) !== 0;
	const atlasBytes = textured ? layout.textureSize * layout.textureSize * BinFormat.bytesPerTexel : 0;
	const triangleBytes = triangleCount * BinFormat.recordSize;
	if (layout.headerSize + atlasBytes + triangleBytes !== arrayBuffer.byteLength) {
		throw FormatError("truncated or corrupt .bin: data blocks do not fill the file");
	}

	const atlasView = textured ? new Uint8Array(arrayBuffer, layout.headerSize, atlasBytes) : null;
	const triView = new Uint8Array(arrayBuffer, layout.headerSize + atlasBytes, triangleBytes);

	return {
		fileName: fileName,
		legacy: layout.legacy === true,
		buffer: arrayBuffer,
		triView: triView,
		triCount: triangleCount,
		atlasView: atlasView,
		textureSize: textured ? layout.textureSize : 0,
		flags: flags,
		cullBackfaces: (flags & BinFormat.flagCullBackfaces) !== 0,
		groups: null,
		sourceHandle: null,
	};
}

function buildBinBlob(model) {
	const atlasBytes = model.atlasView ? model.textureSize * model.textureSize * BinFormat.bytesPerTexel : 0;
	const triangleBytes = model.triCount * BinFormat.recordSize;
	const total = BinFormat.headerSize + atlasBytes + triangleBytes;

	const header = new Uint8Array(BinFormat.headerSize);
	const outView = new DataView(header.buffer);
	const flags = (model.atlasView ? BinFormat.flagTextured : 0) |
		(model.cullBackfaces ? BinFormat.flagCullBackfaces : 0) |
		(model.flags & ~(BinFormat.flagTextured | BinFormat.flagCullBackfaces));

	outView.setUint32(0, total, true);
	outView.setUint32(4, BinFormat.recordSize, true);
	outView.setUint32(8, model.triCount, true);
	outView.setUint32(12, flags, true);
	// the engine's writer always emits a texture edge here, even with no atlas block, and its
	// reader rejects a value below 256, so an untextured export must still carry a valid size
	outView.setUint32(16, model.atlasView ? model.textureSize : BinFormat.legacyTextureSize, true);

	// the header, atlas and triangle block are handed to the Blob as they are: a 173 MiB model
	// would otherwise be copied again just to be written out
	const parts = model.atlasView ? [header, model.atlasView, model.triView] : [header, model.triView];
	return new Blob(parts, { type: "application/octet-stream" });
}

function atlasPlaneOffset(model, plane) {
	return plane * model.textureSize * model.textureSize;
}

// colorMap is RGBA8, normalMap RGB8, materialMap two bytes [roughness, metallic]
function readTexelColor(atlasView, texel) {
	const o = texel * 4;
	return [atlasView[o], atlasView[o + 1], atlasView[o + 2], atlasView[o + 3]];
}

function readTexelNormal(atlasView, texelCount, texel) {
	const o = texelCount * 4 + texel * 3;
	return [atlasView[o], atlasView[o + 1], atlasView[o + 2]];
}

function readTexelMaterial(atlasView, texelCount, texel) {
	const o = texelCount * 7 + texel * 2;
	return [atlasView[o], atlasView[o + 1]];
}

// The engine samples with texX = uv * (size - 1) / 65535, so the same integer lookup is used
// everywhere the editor touches texels.
function uvToTexel(uvValue, textureSize) {
	return (uvValue * (textureSize - 1)) / 65535;
}

// Triangles carrying identical scalar sets share one material entry in the engine, so the editor
// groups them the same way and reports the count per signature. groupIds keeps the per triangle
// group so later passes do not have to rebuild signature strings.
function scanMaterialGroups(model, onProgress) {
	const dv = new DataView(model.triView.buffer, model.triView.byteOffset, model.triView.byteLength);
	const groups = new Map();
	const list = [];
	const groupIds = new Uint32Array(model.triCount);
	model.groupIds = groupIds;

	function range(start, end) {
		for (let i = start; i < end; i++) {
			const base = i * Rec.size;
			const key = dv.getUint32(base + Rec.roughness, true) + ":" +
				dv.getUint32(base + Rec.metallic, true) + ":" +
				dv.getUint32(base + Rec.emission, true) + ":" +
				dv.getUint32(base + Rec.color, true) + ":" +
				dv.getUint32(base + Rec.color + 4, true) + ":" +
				dv.getUint32(base + Rec.color + 8, true);
			let group = groups.get(key);
			if (!group) {
				group = {
					index: list.length,
					name: "mat" + list.length,
					count: 0,
					first: i,
					roughness: dv.getFloat32(base + Rec.roughness, true),
					metallic: dv.getFloat32(base + Rec.metallic, true),
					emission: dv.getFloat32(base + Rec.emission, true),
					color: [dv.getFloat32(base + Rec.color, true), dv.getFloat32(base + Rec.color + 4, true), dv.getFloat32(base + Rec.color + 8, true)],
					uvMinU: 65535, uvMaxU: 0, uvMinV: 65535, uvMaxV: 0,
				};
				groups.set(key, group);
				list.push(group);
			}
			group.count++;
			groupIds[i] = group.index;
			for (let k = 0; k < 6; k += 2) {
				const u = dv.getUint16(base + Rec.uv + k, true);
				const v = dv.getUint16(base + Rec.uv + k + 1, true);
				if (u < group.uvMinU) group.uvMinU = u;
				if (u > group.uvMaxU) group.uvMaxU = u;
				if (v < group.uvMinV) group.uvMinV = v;
				if (v > group.uvMaxV) group.uvMaxV = v;
			}
		}
	}

	return runChunked(model.triCount, 32768, range, onProgress).then(function () {
		model.groups = list;
		return list;
	});
}

function setTriangleEmission(model, triangleIndex, value) {
	const dv = new DataView(model.triView.buffer, model.triView.byteOffset, model.triView.byteLength);
	dv.setFloat32(triangleIndex * Rec.size + Rec.emission, value, true);
}

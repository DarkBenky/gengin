// Atlas brush: a canvas at atlas resolution that paints the emission side buffer, the color map,
// or the roughness/metallic and normal planes inside the model atlas. Emission is exported as a
// 24bpp BMP sidecar; the other planes are written by the .bin writer on save.
//
// The file and type names keep "emission" because that is where this started; the surface now
// covers every atlas plane.
//
// Only one full size buffer is added on top of the model: the emission bytes. The dimmed backdrop
// is regenerated from the atlas the model already owns, and only for the rectangle a stroke
// touched, so a 4096 atlas stays cheap to paint on.

"use strict";

const EmissionEditor = {
	dimFactor: 0.35,
	minBrushSize: 1,
	maxBrushSize: 4096,
};

function dimChannel(value, factor) {
	return Math.round(value * factor);
}

function EmissionSurface(model) {
	if (!model.atlasView) throw FormatError("this model has no texture block, only the per-triangle emission scalar exists");
	this.textureSize = model.textureSize;
	this.atlasView = model.atlasView;
	this.emission = new Uint8Array(this.textureSize * this.textureSize * 3);
	this.canvas = document.createElement("canvas");
	this.canvas.width = this.textureSize;
	this.canvas.height = this.textureSize;
	this.context = this.canvas.getContext("2d");
	this.imageData = new ImageData(this.textureSize, this.textureSize);
	this.pixels = this.imageData.data;
	this.layer = "emission";
	this.compositeAll();
}

// setLayer switches which atlas plane the canvas shows and the brush writes to
EmissionSurface.prototype.setLayer = function (layer) {
	if (layer === this.layer) return;
	this.layer = layer;
	this.compositeAll();
};

// compositeInto renders the active layer into an RGBA buffer. Emission shows the dimmed color
// atlas so painted light reads clearly; the other layers show their own plane.
EmissionSurface.prototype.compositeInto = function (out, outWidth, left, top, width, height) {
	const size = this.textureSize;
	const count = size * size;
	const atlas = this.atlasView;
	const layer = this.layer;
	const factor = layer === "emission" ? EmissionEditor.dimFactor : 1;
	const materialChannel = layer === "metallic" ? 1 : 0;
	for (let y = 0; y < height; y++) {
		let texel = (top + y) * size + left;
		let o = y * outWidth * 4;
		for (let x = 0; x < width; x++, texel++, o += 4) {
			if (layer === "normal") {
				const s = count * AtlasPlane.normal + texel * 3;
				out[o] = atlas[s];
				out[o + 1] = atlas[s + 1];
				out[o + 2] = atlas[s + 2];
			} else if (layer === "roughness" || layer === "metallic") {
				const value = atlas[count * AtlasPlane.material + texel * 2 + materialChannel];
				out[o] = value;
				out[o + 1] = value;
				out[o + 2] = value;
			} else {
				const e = texel * 3;
				out[o] = dimChannel(atlas[texel * 4], factor) + this.emission[e];
				out[o + 1] = dimChannel(atlas[texel * 4 + 1], factor) + this.emission[e + 1];
				out[o + 2] = dimChannel(atlas[texel * 4 + 2], factor) + this.emission[e + 2];
			}
			out[o + 3] = 255;
		}
	}
};

// compositeRect repaints one rectangle after a stroke, so a 4096 atlas stays cheap to paint on
EmissionSurface.prototype.compositeRect = function (x0, y0, x1, y1) {
	const size = this.textureSize;
	const left = clamp(x0, 0, size - 1);
	const top = clamp(y0, 0, size - 1);
	const right = clamp(x1, 0, size - 1);
	const bottom = clamp(y1, 0, size - 1);
	if (right < left || bottom < top) return;

	const width = right - left + 1;
	const height = bottom - top + 1;
	const tile = new ImageData(width, height);
	this.compositeInto(tile.data, width, left, top, width, height);
	this.context.putImageData(tile, left, top);
};

EmissionSurface.prototype.compositeAll = function () {
	this.compositeInto(this.pixels, this.textureSize, 0, 0, this.textureSize, this.textureSize);
	this.context.putImageData(this.imageData, 0, 0);
};

EmissionSurface.prototype.loadRgb = function (rgb) {
	this.emission.set(rgb);
	this.compositeAll();
};

EmissionSurface.prototype.clear = function () {
	this.emission.fill(0);
	this.compositeAll();
};

// fillCell floods a rectangle with the active layer's value: the brush color for the color and
// emission planes, the strength modifier applied at full weight for the value planes
EmissionSurface.prototype.fillCell = function (x0, y0, x1, y1, color, strength) {
	const size = this.textureSize;
	const count = size * size;
	const layer = this.layer;
	const materialChannel = layer === "metallic" ? 1 : 0;
	for (let y = y0; y <= y1; y++) {
		let texel = y * size + x0;
		for (let x = x0; x <= x1; x++, texel++) {
			if (layer === "emission") {
				const e = texel * 3;
				this.emission[e] = color[0];
				this.emission[e + 1] = color[1];
				this.emission[e + 2] = color[2];
			} else if (layer === "color") {
				const c = texel * 4;
				this.atlasView[c] = color[0];
				this.atlasView[c + 1] = color[1];
				this.atlasView[c + 2] = color[2];
				this.atlasView[c + 3] = 255;
			} else if (layer === "normal") {
				const n = count * AtlasPlane.normal + texel * 3;
				for (let i = 0; i < 3; i++) {
					this.atlasView[n + i] = clamp(FlatNormal[i] + (this.atlasView[n + i] - FlatNormal[i]) * (1 + strength), 0, 255);
				}
			} else {
				const m = count * AtlasPlane.material + texel * 2 + materialChannel;
				this.atlasView[m] = clamp(this.atlasView[m] + strength * 255, 0, 255);
			}
		}
	}
	this.compositeRect(x0, y0, x1, y1);
	return { x0: x0, y0: y0, x1: x1, y1: y1 };
};

EmissionSurface.prototype.eraseCell = function (x0, y0, x1, y1) {
	this.fillCell(x0, y0, x1, y1, [0, 0, 0]);
};

// stamp paints a soft round brush: a smoothstep falloff from the centre out to the radius.
// Name and normal maps take a strength modifier instead of a colour, because typing RGB values
// for a tangent space normal is unusable. strength > 0 exaggerates the stored value, < 0 damps it
// (a normal pushed to -1 becomes the flat 128,128,255). deferComposite lets a stroke paint several
// steps into the same rectangle and repaint the canvas once.
EmissionSurface.prototype.stamp = function (cx, cy, radius, color, opacity, strength, deferComposite) {
	const size = this.textureSize;
	const count = size * size;
	const x0 = Math.max(0, Math.floor(cx - radius));
	const x1 = Math.min(size - 1, Math.ceil(cx + radius));
	const y0 = Math.max(0, Math.floor(cy - radius));
	const y1 = Math.min(size - 1, Math.ceil(cy + radius));
	if (x1 < x0 || y1 < y0) return null;
	const invRadius = radius > 0 ? 1 / radius : 1;
	const layer = this.layer;
	const materialChannel = layer === "metallic" ? 1 : 0;
	for (let y = y0; y <= y1; y++) {
		const dy = y - cy;
		let texel = y * size + x0;
		for (let x = x0; x <= x1; x++, texel++) {
			const dx = x - cx;
			const distance = Math.sqrt(dx * dx + dy * dy);
			if (distance > radius) continue;
			let falloff = 1 - distance * invRadius;
			falloff = falloff * falloff * (3 - 2 * falloff);
			const alpha = clamp(falloff * opacity, 0, 1);
			if (layer === "emission") {
				const e = texel * 3;
				this.emission[e] += (color[0] - this.emission[e]) * alpha;
				this.emission[e + 1] += (color[1] - this.emission[e + 1]) * alpha;
				this.emission[e + 2] += (color[2] - this.emission[e + 2]) * alpha;
			} else if (layer === "color") {
				const c = texel * 4;
				this.atlasView[c] += (color[0] - this.atlasView[c]) * alpha;
				this.atlasView[c + 1] += (color[1] - this.atlasView[c + 1]) * alpha;
				this.atlasView[c + 2] += (color[2] - this.atlasView[c + 2]) * alpha;
				this.atlasView[c + 3] = 255;
			} else if (layer === "normal") {
				const n = count * AtlasPlane.normal + texel * 3;
				for (let i = 0; i < 3; i++) {
					const current = this.atlasView[n + i];
					const target = clamp(FlatNormal[i] + (current - FlatNormal[i]) * (1 + strength), 0, 255);
					this.atlasView[n + i] += (target - current) * alpha;
				}
			} else {
				const m = count * AtlasPlane.material + texel * 2 + materialChannel;
				const current = this.atlasView[m];
				const target = clamp(current + strength * 255, 0, 255);
				this.atlasView[m] += (target - current) * alpha;
			}
		}
	}
	if (!deferComposite) this.compositeRect(x0, y0, x1, y1);
	return { x0: x0, y0: y0, x1: x1, y1: y1 };
};

// topDownRgb hands out a copy so the caller can keep it while painting continues
EmissionSurface.prototype.topDownRgb = function () {
	return this.emission.slice();
};

// EmissionView is the zoomable/pannable canvas the user paints on.
function EmissionView(canvas, surface, onChange) {
	this.canvas = canvas;
	this.context = canvas.getContext("2d");
	this.surface = surface;
	this.onChange = onChange;
	this.scale = 1;
	this.offsetX = 0;
	this.offsetY = 0;
	this.brushRadius = 16;
	this.brushColor = [255, 200, 90];
	this.brushStrength = 0.5;
	this.opacity = 0.6;
	this.eraser = false;
	this.painting = false;
	this.dirty = null;
	this.onHover = null;
	this.lastX = 0;
	this.lastY = 0;
	this.hover = null;
	this.bindInput();
}

EmissionView.prototype.setSurface = function (surface) {
	this.surface = surface;
	this.draw();
};

EmissionView.prototype.fit = function () {
	if (!this.surface) return;
	const size = this.surface.textureSize;
	this.scale = Math.min(this.canvas.clientWidth, this.canvas.clientHeight) / size;
	this.offsetX = (this.canvas.clientWidth - size * this.scale) / 2;
	this.offsetY = (this.canvas.clientHeight - size * this.scale) / 2;
	this.draw();
};

EmissionView.prototype.zoomAt = function (px, py, factor) {
	const before = this.toAtlas(px, py);
	this.scale = clamp(this.scale * factor, 0.02, 64);
	const after = this.toAtlas(px, py);
	this.offsetX += (after.x - before.x) * this.scale;
	this.offsetY += (after.y - before.y) * this.scale;
	this.draw();
};

// zoomToBox frames a material's UV bounding box with a margin
EmissionView.prototype.zoomToBox = function (uvMinU, uvMaxU, uvMinV, uvMaxV) {
	if (!this.surface) return;
	const size = this.surface.textureSize;
	const x0 = uvToTexel(uvMinU, size);
	const x1 = uvToTexel(uvMaxU, size);
	const y0 = uvToTexel(uvMinV, size);
	const y1 = uvToTexel(uvMaxV, size);
	const width = Math.max(1, x1 - x0);
	const height = Math.max(1, y1 - y0);
	const margin = 8;
	this.scale = Math.min((this.canvas.clientWidth - margin) / width, (this.canvas.clientHeight - margin) / height);
	this.offsetX = this.canvas.clientWidth / 2 - (x0 + width / 2) * this.scale;
	this.offsetY = this.canvas.clientHeight / 2 - (y0 + height / 2) * this.scale;
	this.draw();
};

EmissionView.prototype.toAtlas = function (px, py) {
	return { x: (px - this.offsetX) / this.scale, y: (py - this.offsetY) / this.scale };
};

// draw blits only the visible part of the atlas canvas so panning a 4096 atlas stays cheap
EmissionView.prototype.draw = function () {
	const ctx = this.context;
	const width = this.canvas.clientWidth;
	const height = this.canvas.clientHeight;
	const dpr = Math.min(2, window.devicePixelRatio || 1);
	const pixelWidth = Math.floor(width * dpr);
	const pixelHeight = Math.floor(height * dpr);
	if (this.canvas.width !== pixelWidth || this.canvas.height !== pixelHeight) {
		this.canvas.width = pixelWidth;
		this.canvas.height = pixelHeight;
	}
	ctx.setTransform(dpr, 0, 0, dpr, 0, 0);
	ctx.clearRect(0, 0, width, height);
	ctx.fillStyle = "#1a1c20";
	ctx.fillRect(0, 0, width, height);
	if (!this.surface) {
		ctx.fillStyle = "#8b9199";
		ctx.font = "13px system-ui, sans-serif";
		ctx.fillText("no texture block: this model has no atlas to paint", 14, 24);
		return;
	}
	const size = this.surface.textureSize;
	ctx.imageSmoothingEnabled = this.scale < 1;
	const srcX0 = clamp(Math.floor(-this.offsetX / this.scale), 0, size);
	const srcY0 = clamp(Math.floor(-this.offsetY / this.scale), 0, size);
	const srcX1 = clamp(Math.ceil((width - this.offsetX) / this.scale), 0, size);
	const srcY1 = clamp(Math.ceil((height - this.offsetY) / this.scale), 0, size);
	if (srcX1 > srcX0 && srcY1 > srcY0) {
		ctx.drawImage(this.surface.canvas, srcX0, srcY0, srcX1 - srcX0, srcY1 - srcY0,
			this.offsetX + srcX0 * this.scale, this.offsetY + srcY0 * this.scale,
			(srcX1 - srcX0) * this.scale, (srcY1 - srcY0) * this.scale);
	}
	this.drawCursor();
};

EmissionView.prototype.pointerAtlas = function (event) {
	const rect = this.canvas.getBoundingClientRect();
	return this.toAtlas(event.clientX - rect.left, event.clientY - rect.top);
};

EmissionView.prototype.bindInput = function () {
	const self = this;
	const canvas = this.canvas;
	canvas.addEventListener("pointerdown", function (event) {
		if (!self.surface) return;
		canvas.setPointerCapture(event.pointerId);
		self.painting = true;
		const at = self.pointerAtlas(event);
		self.lastX = at.x;
		self.lastY = at.y;
		self.paintAt(at.x, at.y);
	});
	canvas.addEventListener("pointermove", function (event) {
		if (!self.surface) return;
		const at = self.pointerAtlas(event);
		self.hover = at;
		if (self.onHover) self.onHover(at);
		if (!self.painting) { self.draw(); return; }
		self.strokeTo(at.x, at.y);
	});
	canvas.addEventListener("pointerup", function () {
		if (!self.painting) return;
		self.painting = false;
		const dirty = self.dirty;
		self.dirty = null;
		if (self.onChange) self.onChange(self.surface.layer, dirty);
	});
	canvas.addEventListener("pointerleave", function () {
		self.hover = null;
		if (self.onHover) self.onHover(null);
		self.draw();
	});
	canvas.addEventListener("wheel", function (event) {
		event.preventDefault();
		const rect = canvas.getBoundingClientRect();
		self.zoomAt(event.clientX - rect.left, event.clientY - rect.top, event.deltaY < 0 ? 1.15 : 1 / 1.15);
	}, { passive: false });
};

EmissionView.prototype.strokeTo = function (x, y) {
	const step = Math.max(1, this.brushRadius * 0.35);
	const steps = Math.max(1, Math.ceil(Math.hypot(x - this.lastX, y - this.lastY) / step));
	let union = null;
	for (let i = 1; i <= steps; i++) {
		const t = i / steps;
		const rect = this.surface.stamp(this.lastX + (x - this.lastX) * t, this.lastY + (y - this.lastY) * t,
			this.brushRadius, this.paintColor(), this.paintOpacity(), this.brushStrength, true);
		if (rect) union = mergeRect(union, rect);
	}
	if (union) this.surface.compositeRect(union.x0, union.y0, union.x1, union.y1);
	this.dirty = mergeRect(this.dirty, union);
	this.lastX = x;
	this.lastY = y;
	this.draw();
};

EmissionView.prototype.paintAt = function (x, y) {
	const rect = this.surface.stamp(x, y, this.brushRadius, this.paintColor(), this.paintOpacity(), this.brushStrength);
	if (rect) this.dirty = mergeRect(this.dirty, rect);
	this.draw();
};

function mergeRect(a, b) {
	if (!a) return b;
	return {
		x0: Math.min(a.x0, b.x0), y0: Math.min(a.y0, b.y0),
		x1: Math.max(a.x1, b.x1), y1: Math.max(a.y1, b.y1),
	};
}

EmissionView.prototype.paintColor = function () {
	return this.eraser ? [0, 0, 0] : this.brushColor;
};

EmissionView.prototype.paintOpacity = function () {
	return this.eraser ? 1 : this.opacity;
};

// drawCursor is called at the end of draw, it must not call draw again
EmissionView.prototype.drawCursor = function () {
	if (!this.hover || !this.surface) return;
	const ctx = this.context;
	ctx.strokeStyle = "rgba(255,255,255,0.7)";
	ctx.lineWidth = 1;
	ctx.beginPath();
	ctx.arc(this.offsetX + this.hover.x * this.scale, this.offsetY + this.hover.y * this.scale,
		this.brushRadius * this.scale, 0, Math.PI * 2);
	ctx.stroke();
};

// bakeGroupEmission averages the emission sampled at a group's triangle UVs and writes the
// luminance into the per-triangle emission scalar. Only called from an explicit button.
function bakeGroupEmission(model, group, surface) {
	const size = model.textureSize;
	const dv = new DataView(model.triView.buffer, model.triView.byteOffset, model.triView.byteLength);
	let total = 0;
	let samples = 0;
	for (let i = 0; i < model.triCount; i++) {
		if (model.groupIds[i] !== group.index) continue;
		const base = i * Rec.size;
		for (let corner = 0; corner < 3; corner++) {
			const u = dv.getUint16(base + Rec.uv + corner * 4, true);
			const v = dv.getUint16(base + Rec.uv + corner * 4 + 2, true);
			const texel = Math.round(uvToTexel(v, size)) * size + Math.round(uvToTexel(u, size));
			const e = texel * 3;
			total += (0.299 * surface.emission[e] + 0.587 * surface.emission[e + 1] + 0.114 * surface.emission[e + 2]) / 255;
			samples++;
		}
	}
	if (samples === 0) return 0;
	const average = total / samples;
	let touched = 0;
	for (let i = 0; i < model.triCount; i++) {
		if (model.groupIds[i] !== group.index) continue;
		setTriangleEmission(model, i, average);
		touched++;
	}
	return touched;
}

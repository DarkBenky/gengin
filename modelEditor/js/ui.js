// UI wiring: file entry points, stats, material list, brush controls, status line.

"use strict";

const Ui = {
	atlasChoices: [256, 512, 1024, 2048, 2560, 3072, 4096, 8192],
};

function AppElements() {
	return {
		status: document.getElementById("status"),
		stats: document.getElementById("stats"),
		binInput: document.getElementById("binInput"),
		objInput: document.getElementById("objInput"),
		imageInput: document.getElementById("imageInput"),
		atlasSize: document.getElementById("atlasSize"),
		previewCanvas: document.getElementById("preview"),
		emissionCanvas: document.getElementById("emissionCanvas"),
		materialList: document.getElementById("materialList"),
		materialNote: document.getElementById("materialNote"),
		brushSize: document.getElementById("brushSize"),
		brushSizeValue: document.getElementById("brushSizeValue"),
		brushOpacity: document.getElementById("brushOpacity"),
		brushOpacityValue: document.getElementById("brushOpacityValue"),
		brushColor: document.getElementById("brushColor"),
		eraser: document.getElementById("eraser"),
		paintLayer: document.getElementById("paintLayer"),
		brushStrength: document.getElementById("brushStrength"),
		brushStrengthValue: document.getElementById("brushStrengthValue"),
		emissionScalar: document.getElementById("emissionScalar"),
		faceResolution: document.getElementById("faceResolution"),
		strengthRow: document.getElementById("strengthRow"),
		colorRow: document.getElementById("colorRow"),
		wireframe: document.getElementById("wireframe"),
		cull: document.getElementById("cull"),
		textures: document.getElementById("textures"),
		emissionView: document.getElementById("emissionView"),
		paintOnModel: document.getElementById("paintOnModel"),
		selectedMaterial: document.getElementById("selectedMaterial"),
	};
}

function setStatus(message, kind) {
	const element = document.getElementById("status");
	element.textContent = message;
	element.className = "status" + (kind ? " " + kind : "");
}

function hexToRgb(hex) {
	const value = parseInt(hex.slice(1), 16);
	return [(value >> 16) & 255, (value >> 8) & 255, value & 255];
}

function rgbToHex(rgb) {
	return "#" + ((1 << 24) + (rgb[0] << 16) + (rgb[1] << 8) + rgb[2]).toString(16).slice(1);
}

function App() {
	this.elements = AppElements();
	this.model = null;
	this.renderer = null;
	this.emissionView = null;
	this.surface = null;
	this.selectedGroup = null;
	this.directoryHandle = null;
	this.lastObjFile = null;
	this.companionFiles = [];
	this.lastMarkerTexel = -1;
	this.busy = false;
}

App.prototype.init = function () {
	const self = this;
	const el = this.elements;

	this.renderer = new PreviewRenderer(el.previewCanvas);
	this.emissionView = new EmissionView(el.emissionCanvas, null, function (layer, dirty) {
		self.syncSurfaceToPreview(layer, dirty);
	});
	this.emissionView.onHover = function (at) { self.updateUvMarker(at); };
	this.emissionView.fit();

	el.binInput.addEventListener("change", function () {
		if (el.binInput.files.length > 0) self.loadBinFile(el.binInput.files[0]);
		el.binInput.value = "";
	});
	el.objInput.addEventListener("change", function () {
		self.importFromInput(el.objInput.files);
		el.objInput.value = "";
	});
	el.imageInput.addEventListener("change", function () {
		self.importFromInput(el.imageInput.files);
		el.imageInput.value = "";
	});

	document.getElementById("loadBinButton").addEventListener("click", function () { el.binInput.click(); });
	document.getElementById("importObjButton").addEventListener("click", function () { el.objInput.click(); });
	document.getElementById("addFilesButton").addEventListener("click", function () { el.imageInput.click(); });
	document.getElementById("saveBinButton").addEventListener("click", function () { self.saveBin(); });
	document.getElementById("exportEmissionButton").addEventListener("click", function () { self.exportEmission(); });
	document.getElementById("exportFacesButton").addEventListener("click", function () { self.exportFaceMaps(); });
	document.getElementById("loadEmissionButton").addEventListener("click", function () { self.pickEmissionFile(); });
	document.getElementById("linkFolderButton").addEventListener("click", function () { self.linkFolder(); });
	document.getElementById("frameButton").addEventListener("click", function () { self.renderer.frameCamera(); });
	document.getElementById("fillCellButton").addEventListener("click", function () { self.fillSelectedCell(); });
	document.getElementById("clearEmissionButton").addEventListener("click", function () { self.clearEmission(); });
	document.getElementById("bakeButton").addEventListener("click", function () { self.bakeSelected(); });
	document.getElementById("applyEmissionButton").addEventListener("click", function () { self.applyEmissionScalar(); });

	el.brushSize.addEventListener("input", function () {
		self.emissionView.brushRadius = Number(el.brushSize.value) / 2;
		el.brushSizeValue.textContent = el.brushSize.value + " px";
	});
	el.brushOpacity.addEventListener("input", function () {
		self.emissionView.opacity = Number(el.brushOpacity.value) / 100;
		el.brushOpacityValue.textContent = el.brushOpacity.value + "%";
	});
	el.brushColor.addEventListener("input", function () {
		self.emissionView.brushColor = hexToRgb(el.brushColor.value);
		self.emissionView.eraser = false;
		el.eraser.checked = false;
	});
	el.eraser.addEventListener("change", function () { self.emissionView.eraser = el.eraser.checked; });
	el.brushStrength.addEventListener("input", function () {
		self.emissionView.brushStrength = Number(el.brushStrength.value) / 100;
		el.brushStrengthValue.textContent = el.brushStrength.value + "%";
	});
	el.paintLayer.addEventListener("change", function () {
		if (self.surface) self.surface.setLayer(el.paintLayer.value);
		self.updateLayerUi();
		self.emissionView.draw();
	});
	el.paintOnModel.addEventListener("change", function () {
		el.previewCanvas.style.cursor = el.paintOnModel.checked ? "crosshair" : "default";
		if (!el.paintOnModel.checked) { setStatus("Direct paint off"); return; }
		setStatus("Direct paint: left drag paints the model, shift, middle or right drag pans, wheel zooms; turn it off to orbit");
		self.ensureAtlas();
	});

	el.wireframe.addEventListener("change", function () { self.renderer.wireframe = el.wireframe.checked; self.renderer.render(); });
	el.cull.addEventListener("change", function () { self.renderer.cullBackfaces = el.cull.checked; self.renderer.render(); });
	el.textures.addEventListener("change", function () { self.renderer.showTextures = el.textures.checked; self.renderer.render(); });
	el.emissionView.addEventListener("change", function () {
		self.renderer.showEmissionView = el.emissionView.checked;
		self.renderer.render();
		setStatus(el.emissionView.checked ? "Emission view: showing the painted emission unlit" : "Shaded view");
	});

	this.bindCamera();
	this.bindPreviewPaint();
	this.bindDrop();
	window.addEventListener("resize", function () { self.renderer.render(); self.emissionView.draw(); });

	this.updateLayerUi();
	setStatus("Load a .bin model or import an .obj with its .mtl and textures.");
};

App.prototype.bindCamera = function () {
	const self = this;
	const canvas = this.elements.previewCanvas;
	let dragging = false;
	let panning = false;
	let lastX = 0;
	let lastY = 0;

	canvas.addEventListener("pointerdown", function (event) {
		dragging = event.button === 0 && !event.shiftKey && !self.paintOnModel();
		panning = event.button === 1 || event.shiftKey || event.button === 2;
		lastX = event.clientX;
		lastY = event.clientY;
		canvas.setPointerCapture(event.pointerId);
	});
	canvas.addEventListener("pointermove", function (event) {
		if (!dragging && !panning) return;
		const dx = event.clientX - lastX;
		const dy = event.clientY - lastY;
		lastX = event.clientX;
		lastY = event.clientY;
		if (panning) self.renderer.pan(dx, dy);
		else self.renderer.orbit(dx * 0.008, dy * 0.008);
	});
	canvas.addEventListener("pointerup", function () { dragging = false; panning = false; });
	canvas.addEventListener("contextmenu", function (event) { event.preventDefault(); });
	canvas.addEventListener("wheel", function (event) {
		event.preventDefault();
		self.renderer.zoom(event.deltaY < 0 ? 1 / 1.12 : 1.12);
	}, { passive: false });
};

App.prototype.paintOnModel = function () {
	return this.elements.paintOnModel.checked;
};

// paintAtScreen raycasts the model under a canvas pixel and stamps the atlas at the hit UV with
// the brush settings from the atlas panel, so both views share one brush and one layer.
App.prototype.paintAtScreen = function (px, py) {
	if (!this.model || !this.surface) return null;
	const ray = this.renderer.cameraRay(px, py);
	const hit = pickTriangle(this.model, ray.origin, ray.direction);
	if (!hit) return null;
	const size = this.model.textureSize;
	const texelX = Math.round(uvToTexel(hit.u, size));
	const texelY = Math.round(uvToTexel(hit.v, size));
	const painted = this.surface.stamp(texelX, texelY, this.emissionView.brushRadius,
		this.emissionView.paintColor(), this.emissionView.paintOpacity(), this.emissionView.brushStrength);
	if (!painted) return null;
	this.syncSurfaceToPreview(this.surface.layer, painted);
	this.emissionView.draw();
	return { x: texelX, y: texelY };
};

// bindPreviewPaint paints on the model itself: left drag on the preview when the toggle is on,
// while shift, middle and right drag still orbit and pan through bindCamera
App.prototype.bindPreviewPaint = function () {
	const self = this;
	const canvas = this.elements.previewCanvas;
	let painting = false;
	let frame = 0;
	let pending = null;

	// one raycast and one stamp per animation frame: pointermove can fire far more often than the
	// preview can show, and each stamp re-uploads a rectangle of the emission texture
	function flush() {
		frame = 0;
		if (!pending) return;
		const point = pending;
		pending = null;
		self.paintAtScreen(point.x, point.y);
	}

	canvas.addEventListener("pointerdown", function (event) {
		if (event.button !== 0 || !self.paintOnModel()) return;
		if (!self.surface) {
			self.ensureAtlas().then(function (ok) {
				if (!ok) setStatus("direct paint needs an atlas: this model has no usable UVs to paint into", "error");
			});
			return;
		}
		event.preventDefault();
		painting = true;
		canvas.setPointerCapture(event.pointerId);
		self.paintAtScreen(event.clientX - canvas.getBoundingClientRect().left,
			event.clientY - canvas.getBoundingClientRect().top);
	});
	canvas.addEventListener("pointermove", function (event) {
		if (!painting) return;
		const rect = canvas.getBoundingClientRect();
		pending = { x: event.clientX - rect.left, y: event.clientY - rect.top };
		if (!frame) frame = requestAnimationFrame(flush);
	});
	canvas.addEventListener("pointerup", function (event) {
		if (!painting) return;
		painting = false;
		if (canvas.hasPointerCapture(event.pointerId)) canvas.releasePointerCapture(event.pointerId);
		if (frame) { cancelAnimationFrame(frame); frame = 0; }
		if (pending) { const point = pending; pending = null; self.paintAtScreen(point.x, point.y); }
	});
};

App.prototype.bindDrop = function () {	const self = this;
	window.addEventListener("dragover", function (event) {
		event.preventDefault();
		document.body.classList.add("dropping");
	});
	window.addEventListener("dragleave", function () { document.body.classList.remove("dropping"); });
	window.addEventListener("drop", function (event) {
		event.preventDefault();
		document.body.classList.remove("dropping");
		const files = Array.prototype.slice.call(event.dataTransfer.files);
		if (files.length === 0) return;
		self.handleFiles(files);
	});
};

App.prototype.importFromInput = function (fileList) {
	const files = Array.prototype.slice.call(fileList);
	if (files.length === 0) return;
	this.handleFiles(files);
};

App.prototype.handleFiles = function (files) {
	const bin = files.filter(function (file) { return /\.bin$/i.test(file.name); });
	const obj = files.filter(function (file) { return /\.obj$/i.test(file.name); });
	const bmp = files.filter(function (file) { return /\.bmp$/i.test(file.name); });

	if (obj.length > 0) {
		this.lastObjFile = obj[0];
		this.companionFiles = files.filter(function (file) { return file !== obj[0]; });
		this.importObj(this.lastObjFile, this.companionFiles);
		return;
	}
	if (bin.length > 0) {
		this.lastObjFile = null;
		this.companionFiles = [];
		this.loadBinFile(bin[0]);
		if (bmp.length > 0) this.loadEmissionFile(bmp[0]);
		return;
	}
	if (bmp.length > 0) {
		this.loadEmissionFile(bmp[0]);
		return;
	}
	// materials and images on their own are added to the OBJ that was imported last, so a model
	// imported before its maps were picked up can still get its atlas
	if (this.lastObjFile) {
		this.companionFiles = mergeFilesByName(this.companionFiles, files);
		this.importObj(this.lastObjFile, this.companionFiles);
		return;
	}
	setStatus("unsupported file: " + files[0].name, "error");
};

function mergeFilesByName(existing, incoming) {
	const byName = new Map();
	existing.forEach(function (file) { byName.set(file.name, file); });
	incoming.forEach(function (file) { byName.set(file.name, file); });
	return Array.from(byName.values());
}

App.prototype.beginBusy = function (message) {
	this.busy = true;
	setStatus(message, "busy");
};

App.prototype.endBusy = function (message, kind) {
	this.busy = false;
	setStatus(message || "ready", kind);
};

App.prototype.loadBinFile = function (file) {
	const self = this;
	this.beginBusy("Reading " + file.name + " (" + formatBytes(file.size) + ")...");
	readFileAsArrayBuffer(file).then(function (buffer) {
		const model = readBinModel(buffer, file.name);
		self.setStats(model, file.size);
		setStatus("Scanning materials...", "busy");
		return scanMaterialGroups(model, function (done, total) {
			setStatus("Scanning materials: " + formatCount(done) + " / " + formatCount(total), "busy");
		}).then(function () {
			return self.setModel(model);
		});
	}).then(function () {
		self.endBusy("Loaded " + self.model.fileName + (self.model.legacy ? " (legacy 16 byte header)" : ""));
		return self.maybeAutoLoadEmission();
	}).catch(function (err) {
		self.endBusy(err.name === "FormatError" ? err.message : "could not load " + file.name + ": " + err.message, "error");
	});
};

App.prototype.importObj = function (objFile, otherFiles) {
	const self = this;
	const atlasSize = Number(this.elements.atlasSize.value);
	this.beginBusy("Importing " + objFile.name + "...");
	importObjFile(objFile, otherFiles, atlasSize, function (message) { setStatus(message, "busy"); })
		.then(function (model) {
			self.setStats(model, null);
			return scanMaterialGroups(model).then(function () { return self.setModel(model); });
		})
		.then(function () {
			const warnings = self.model.warnings || [];
			self.endBusy("Imported " + self.model.fileName + (warnings.length ? " | " + warnings.join(" | ") : ""));
		})
		.catch(function (err) {
			self.endBusy("import failed: " + err.message, "error");
		});
};

App.prototype.setModel = function (model) {
	const self = this;
	this.model = model;
	this.surface = null;
	this.updateUvMarker(null);
	this.emissionView.setSurface(null);
	this.renderer.releaseEmission();
	return this.renderer.loadModel(model, function (done, total) {
		setStatus("Uploading geometry: " + formatCount(done) + " / " + formatCount(total) + " triangles", "busy");
	}).then(function () {
		self.elements.cull.checked = model.cullBackfaces;
		self.elements.textures.checked = self.renderer.showTextures;
		self.elements.wireframe.checked = self.renderer.wireframe;
		self.renderMaterialList();
		if (model.atlasView) {
			self.surface = new EmissionSurface(model);
			self.surface.setLayer(self.elements.paintLayer.value);
			self.emissionView.setSurface(self.surface);
			self.emissionView.fit();
			self.emissionView.brushRadius = Math.max(4, Math.round(model.textureSize / 128));
			self.elements.brushSize.value = String(self.emissionView.brushRadius * 2);
			self.elements.brushSizeValue.textContent = (self.emissionView.brushRadius * 2) + " px";
			document.getElementById("exportEmissionButton").disabled = false;
			document.getElementById("loadEmissionButton").disabled = false;
		} else {
			document.getElementById("exportEmissionButton").disabled = true;
			document.getElementById("loadEmissionButton").disabled = true;
		}
		self.updateLayerUi();
	});
};

App.prototype.setStats = function (model, fileSize) {
	const parts = [
		formatCount(model.triCount) + " triangles",
		model.atlasView ? "atlas " + model.textureSize + "x" + model.textureSize : "no texture block",
		"flags " + (model.flags & (BinFormat.flagTextured | BinFormat.flagCullBackfaces)),
		"file " + formatBytes(fileSize !== null ? fileSize : expectedCurrentSize(model.triCount,
			model.atlasView ? BinFormat.flagTextured : 0, model.textureSize)),
	];
	this.elements.stats.textContent = parts.join("  |  ");
};

App.prototype.renderMaterialList = function () {
	const self = this;
	const list = this.elements.materialList;
	list.textContent = "";
	const groups = this.model.groups || [];
	groups.forEach(function (group) {
		const row = document.createElement("button");
		row.type = "button";
		row.className = "materialRow";
		const swatch = "rgb(" + Math.round(clamp(group.color[0], 0, 1) * 255) + "," +
			Math.round(clamp(group.color[1], 0, 1) * 255) + "," +
			Math.round(clamp(group.color[2], 0, 1) * 255) + ")";
		row.innerHTML = '<span class="swatch" style="background:' + swatch + '"></span>' +
			"<span>" + group.name + "</span><span class=\"count\">" + formatCount(group.count) + "</span>";
		row.addEventListener("click", function () { self.selectGroup(group, row); });
		list.appendChild(row);
	});
	this.elements.selectedMaterial.textContent = "none selected";
	this.selectedGroup = null;
	this.updateLayerUi();
};

App.prototype.selectGroup = function (group, row) {
	this.selectedGroup = group;
	Array.prototype.forEach.call(this.elements.materialList.children, function (child) {
		child.classList.remove("selected");
	});
	row.classList.add("selected");
	this.elements.selectedMaterial.textContent = group.name + " (" + formatCount(group.count) +
		" tris, rough " + group.roughness.toFixed(3) + ", metal " + group.metallic.toFixed(3) +
		", emit " + group.emission.toFixed(3) + ")";
	this.elements.emissionScalar.value = String(group.emission);
	if (this.surface && this.model.atlasView) {
		this.emissionView.zoomToBox(group.uvMinU, group.uvMaxU, group.uvMinV, group.uvMaxV);
	}
	this.updateLayerUi();
};

// cellBoundsOfGroup maps a group's UV box onto atlas texels; imported models sit inside one cell,
// loaded models may span cells so the box is used directly.
function cellBoundsOfGroup(model, group) {
	const size = model.textureSize;
	return {
		x0: clamp(Math.floor(uvToTexel(group.uvMinU, size)), 0, size - 1),
		x1: clamp(Math.ceil(uvToTexel(group.uvMaxU, size)), 0, size - 1),
		y0: clamp(Math.floor(uvToTexel(group.uvMinV, size)), 0, size - 1),
		y1: clamp(Math.ceil(uvToTexel(group.uvMaxV, size)), 0, size - 1),
	};
}

// updateLayerUi keeps the brush panel matching the active layer: colour and eraser only make
// sense for the emission and color planes, value planes use the strength modifier instead
App.prototype.updateLayerUi = function () {
	const el = this.elements;
	const layer = el.paintLayer.value;
	const valueLayer = layer === "normal" || layer === "roughness" || layer === "metallic";
	el.colorRow.classList.toggle("hidden", valueLayer);
	el.strengthRow.classList.toggle("hidden", !valueLayer);
	el.eraser.disabled = valueLayer;

	const hasSurface = this.surface !== null;
	document.getElementById("fillCellButton").disabled = !hasSurface;
	document.getElementById("clearEmissionButton").disabled = !hasSurface || layer !== "emission";
	document.getElementById("bakeButton").disabled = !hasSurface || layer !== "emission";
	document.getElementById("exportEmissionButton").disabled = !hasSurface;
	document.getElementById("exportFacesButton").disabled = !hasSurface;
	document.getElementById("applyEmissionButton").disabled = this.selectedGroup === null || !this.model;

	if (!this.model) {
		el.materialNote.textContent = "";
		return;
	}
	if (!hasSurface) {
		el.materialNote.textContent = this.model
			? "No texture block yet: painting will build an atlas from the per-triangle colors and roughness/metallic."
			: "";
		return;
	}
	const notes = {
		emission: "Emission is painted into a sidecar BMP; it is never written into the .bin.",
		color: "Color edits change the atlas inside the .bin; Save .bin writes them.",
		roughness: "Strength raises or lowers the stored roughness; the material map inside the .bin changes.",
		metallic: "Strength raises or lowers the stored metallic value; the material map inside the .bin changes.",
		normal: "Strength exaggerates the stored normals, negative flattens them toward 128,128,255; the normal map inside the .bin changes.",
	};
	el.materialNote.textContent = notes[layer];
};

App.prototype.fillSelectedCell = function () {
	if (!this.surface || !this.selectedGroup) return;
	const box = cellBoundsOfGroup(this.model, this.selectedGroup);
	const rect = this.surface.fillCell(box.x0, box.y0, box.x1, box.y1,
		this.emissionView.brushColor, this.emissionView.brushStrength);
	this.syncSurfaceToPreview(this.surface.layer, rect);
	setStatus("Filled " + (box.x1 - box.x0 + 1) + "x" + (box.y1 - box.y0 + 1) + " texels of " +
		this.surface.layer + " for " + this.selectedGroup.name);
};

App.prototype.clearEmission = function () {
	if (!this.surface) return;
	this.surface.clear();
	this.syncEmissionToPreview();
	setStatus("Emission map cleared");
};

App.prototype.bakeSelected = function () {
	if (!this.surface || !this.selectedGroup) return;
	const touched = bakeGroupEmission(this.model, this.selectedGroup, this.surface);
	setStatus("Baked average emission " + (touched > 0 ? "" : "(no samples) ") + "into " + formatCount(touched) +
		" triangles of " + this.selectedGroup.name + ". Save the .bin to keep it.");
	this.refreshGeometry();
};

// applyEmissionScalar writes an arbitrary per triangle emission value; the file stores a float, so
// values above 1 (100, 1000) are legal and the engine scales them by 0.01 when shading
App.prototype.applyEmissionScalar = function () {
	if (!this.model || !this.selectedGroup) { setStatus("select a material first", "error"); return; }
	const value = Number(this.elements.emissionScalar.value);
	if (!isFinite(value) || value < 0) { setStatus("emission scalar must be a number >= 0", "error"); return; }
	const self = this;
	const group = this.selectedGroup;
	let touched = 0;
	for (let i = 0; i < this.model.triCount; i++) {
		if (this.model.groupIds[i] !== group.index) continue;
		setTriangleEmission(this.model, i, value);
		touched++;
	}
	setStatus("Emission " + value + " written to " + formatCount(touched) + " triangles of " + group.name, "busy");
	scanMaterialGroups(this.model).then(function () {
		self.renderMaterialList();
		return self.refreshGeometry();
	});
};

// refreshGeometry re-uploads the vertex stream after scalar edits so the preview shows them
App.prototype.refreshGeometry = function () {
	const self = this;
	this.beginBusy("Rebuilding the vertex stream...");
	return this.renderer.updateVertexStream(this.model, function (done, total) {
		setStatus("Rebuilding the vertex stream: " + formatCount(done) + " / " + formatCount(total), "busy");
	}).then(function () {
		self.endBusy("Material updated; save the .bin to keep it");
	});
};

// updateUvMarker stands the atlas cursor up on the model: atlas texel to u16 UV, then the first
// triangle covering that UV, then the world point projected back to a preview pixel
App.prototype.updateUvMarker = function (at) {
	const marker = document.getElementById("uvMarker");
	if (!at || !this.model || !this.model.atlasView) {
		marker.classList.add("hidden");
		this.lastMarkerTexel = -1;
		return;
	}
	const size = this.model.textureSize;
	const texelX = clamp(Math.floor(at.x), 0, size - 1);
	const texelY = clamp(Math.floor(at.y), 0, size - 1);
	const key = texelY * size + texelX;
	if (key === this.lastMarkerTexel) return;
	this.lastMarkerTexel = key;

	const point = findSurfaceAtUv(this.model,
		Math.round(texelX * 65535 / (size - 1)), Math.round(texelY * 65535 / (size - 1)));
	const screen = point ? this.renderer.projectToScreen(point) : null;
	if (!screen) { marker.classList.add("hidden"); return; }
	marker.style.left = screen.x + "px";
	marker.style.top = screen.y + "px";
	marker.classList.remove("hidden");
};

// ensureAtlas gives a model that arrived without a texture block one to paint into: the bake
// copies each triangle's scalars into the texels its UVs cover, so the look is unchanged
App.prototype.ensureAtlas = function () {
	const self = this;
	if (!this.model) return Promise.resolve(false);
	if (this.model.atlasView) return Promise.resolve(true);
	const size = Number(this.elements.atlasSize.value);
	if (!modelHasUvSpread(this.model, size)) {
		setStatus("this model has no UV coordinates spread over the atlas, so there is nothing to paint into", "error");
		return Promise.resolve(false);
	}
	this.beginBusy("Building a " + size + "x" + size + " atlas from the model materials...");
	return bakeAtlasFromTriangles(this.model, size, function (done, total) {
		setStatus("Baking atlas: " + formatCount(done) + " / " + formatCount(total) + " triangles", "busy");
	}).then(function (atlas) {
		const model = self.model;
		model.atlasView = atlas;
		model.textureSize = size;
		model.flags |= BinFormat.flagTextured;
		self.renderer.uploadAtlas(model);
		self.renderer.showTextures = true;
		self.elements.textures.checked = true;
		self.renderer.render();
		self.surface = new EmissionSurface(model);
		self.surface.setLayer(self.elements.paintLayer.value);
		self.emissionView.setSurface(self.surface);
		self.emissionView.fit();
		self.setStats(model, null);
		document.getElementById("exportEmissionButton").disabled = false;
		document.getElementById("loadEmissionButton").disabled = false;
		self.updateLayerUi();
		self.endBusy("Added a " + size + "x" + size + " atlas to " + model.fileName + " (" + formatBytes(atlas.length) + ")");
		return true;
	}).catch(function (err) {
		self.endBusy("could not build an atlas: " + err.message, "error");
		return false;
	});
};

App.prototype.syncEmissionToPreview = function () {
	if (!this.surface) return;
	// the live emission bytes are uploaded directly; copying a 4096 map on every stroke is too costly
	this.renderer.updateEmission(this.surface.emission, this.surface.textureSize);
};

// syncSurfaceToPreview pushes the region a stroke touched to the preview: emission lives in its
// own texture, the color, normal and material planes are sub-uploads into the atlas textures
App.prototype.syncSurfaceToPreview = function (layer, dirty) {
	if (!this.surface) return;
	if (layer === "emission") {
		if (dirty) {
			this.renderer.updateEmissionRegion(this.surface.emission, this.surface.textureSize,
				dirty.x0, dirty.y0, dirty.x1, dirty.y1);
		} else {
			this.syncEmissionToPreview();
		}
		return;
	}
	if (dirty) this.renderer.updateAtlasRegion(this.model, dirty.x0, dirty.y0, dirty.x1, dirty.y1);
};

App.prototype.saveBin = function () {
	if (!this.model) { setStatus("nothing to save", "error"); return; }
	const self = this;
	const blob = buildBinBlob(this.model);
	const name = baseNameOf(this.model.fileName) + ".bin";
	saveBlob(blob, name, this.directoryHandle || undefined).then(function (savedName) {
		if (!savedName) { setStatus("save cancelled", "error"); return; }
		setStatus("Saved " + savedName + " (" + formatBytes(blob.size) + ")");
	});
};

App.prototype.exportEmission = function () {
	if (!this.surface) { setStatus("no emission map to export", "error"); return; }
	const rgb = this.surface.topDownRgb();
	const blob = writeBmp24(this.surface.textureSize, this.surface.textureSize, rgb);
	const name = baseNameOf(this.model.fileName) + "_emission.bmp";
	saveBlob(blob, name, this.directoryHandle || undefined).then(function (savedName) {
		if (!savedName) { setStatus("export cancelled", "error"); return; }
		setStatus("Exported " + savedName + " (" + formatBytes(blob.size) + ")");
	});
};

// exportFaceMaps bakes the painted emission into the six EmissionMap style grids. A linked folder
// gets one file per face; without folder access a single bundled file is downloaded instead, so
// the feature still works in browsers that have no File System Access API
App.prototype.exportFaceMaps = function () {
	if (!this.surface) { setStatus("paint some emission first: there is no emission map to export", "error"); return; }
	const resolution = Number(this.elements.faceResolution.value);
	if (!isFinite(resolution) || resolution < FaceMaps.minResolution || resolution > FaceMaps.maxResolution) {
		setStatus("face map resolution must be between " + FaceMaps.minResolution + " and " + FaceMaps.maxResolution, "error");
		return;
	}
	const self = this;
	const folder = this.directoryHandle;
	this.beginBusy("Building " + resolution + "x" + resolution + " face emission maps...");
	buildFaceGrids(this.model, this.surface.emission, resolution, function (done, total) {
		setStatus("Baking face maps: " + formatCount(done) + " / " + formatCount(total) + " triangles", "busy");
	}).then(function (grids) {
		if (folder) {
			const writes = grids.faces.map(function (face) {
				return saveBlob(writeFaceBin(face, resolution, grids.bounds), face.name + ".bin", folder);
			});
			return Promise.all(writes).then(function () {
				self.endBusy("Exported 6 face maps (" + resolution + "x" + resolution + ") into the linked folder");
			});
		}
		const name = baseNameOf(self.model.fileName) + "_faces.bin";
		return saveBlob(writeFaceBundle(grids, grids.bounds), name).then(function () {
			self.endBusy("Exported " + name + ": six " + resolution + "x" + resolution +
				" face maps in one file (magic FEM2)");
		});
	}).catch(function (err) {
		self.endBusy("face export failed: " + err.message, "error");
	});
};

App.prototype.pickEmissionFile = function () {	const self = this;
	const input = document.createElement("input");
	input.type = "file";
	input.accept = ".bmp";
	input.addEventListener("change", function () {
		if (input.files.length > 0) self.loadEmissionFile(input.files[0]);
	});
	input.click();
};

App.prototype.loadEmissionFile = function (file) {
	if (!this.model || !this.model.atlasView) {
		setStatus("this model has no texture block, so there is no emission map to load", "error");
		return;
	}
	const self = this;
	readFileAsArrayBuffer(file).then(function (buffer) {
		const bmp = readBmp24(buffer, file.name);
		if (bmp.width !== self.model.textureSize || bmp.height !== self.model.textureSize) {
			throw FormatError("emission map is " + bmp.width + "x" + bmp.height +
				" but the atlas is " + self.model.textureSize + "x" + self.model.textureSize);
		}
		self.surface.loadRgb(bmp.rgb);
		self.syncEmissionToPreview();
		setStatus("Loaded " + file.name + " (" + bmp.width + "x" + bmp.height + ")");
	}).catch(function (err) {
		setStatus(err.name === "FormatError" ? err.message : "could not read emission map: " + err.message, "error");
	});
};

// linkFolder lets the editor find <base>_emission.bmp next to a .bin without the user picking it
App.prototype.linkFolder = function () {
	const self = this;
	if (typeof window.showDirectoryPicker !== "function") {
		setStatus("this browser has no folder access, drop the _emission.bmp file instead", "error");
		return;
	}
	window.showDirectoryPicker().then(function (handle) {
		self.directoryHandle = handle;
		setStatus("Folder linked: " + handle.name);
		return self.maybeAutoLoadEmission();
	}).catch(function (err) {
		if (err && err.name === "AbortError") return;
		setStatus("could not link a folder: " + err.message, "error");
	});
};

App.prototype.maybeAutoLoadEmission = function () {
	if (!this.directoryHandle || !this.model || !this.model.atlasView) return Promise.resolve();
	const self = this;
	const name = baseNameOf(this.model.fileName) + "_emission.bmp";
	return this.directoryHandle.getFileHandle(name).then(function (handle) {
		return handle.getFile();
	}).then(function (file) {
		return self.loadEmissionFile(file);
	}).catch(function () {
		return null;
	});
};

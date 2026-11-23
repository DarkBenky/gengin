// WebGL2 preview: triangle soup in one interleaved buffer, one draw call, flat shading.

"use strict";

const Preview = {
	floatsPerVertex: 15,
};

// interleaved layout in floats: pos, normal, color, rough/metal/emission, then two u16 uv
const VertexLayout = {
	pos: 0,
	normal: 3,
	color: 6,
	material: 9,
	uv: 12,
	strideBytes: 15 * 4,
};

// A cheap studio environment shared by the background and the reflection term, so metals have
// something to mirror and roughness reads as a duller reflection.
const EnvironmentSource = `
vec3 environment(vec3 d) {
	float up = clamp(d.y, -1.0, 1.0);
	vec3 sky = mix(vec3(0.10, 0.13, 0.18), vec3(0.30, 0.42, 0.60), smoothstep(0.0, 0.75, up));
	vec3 ground = mix(vec3(0.16, 0.14, 0.12), vec3(0.06, 0.06, 0.07), smoothstep(0.0, -0.7, up));
	vec3 color = up >= 0.0 ? sky : ground;
	float sun = pow(max(dot(d, normalize(uLightDir)), 0.0), 220.0);
	float band = exp(-abs(up) * 9.0) * 0.06;
	return color + vec3(1.0, 0.93, 0.80) * sun * 1.4 + vec3(band);
}
`;

const VertexShaderSource = `#version 300 es
layout(location=0) in vec3 aPos;
layout(location=1) in vec3 aNormal;
layout(location=2) in uvec2 aUv;
layout(location=3) in vec3 aColor;
layout(location=4) in vec3 aMaterial;
uniform mat4 uViewProj;
out vec3 vNormal;
out vec3 vColor;
out vec3 vWorld;
out vec2 vUv;
out float vEmission;
flat out float vRoughness;
flat out float vMetallic;
void main() {
	gl_Position = uViewProj * vec4(aPos, 1.0);
	vNormal = aNormal;
	vColor = aColor;
	vWorld = aPos;
	// interpolated in u16 units, matching how the file stores them
	vUv = vec2(aUv);
	vEmission = aMaterial.z;
	vRoughness = aMaterial.x;
	vMetallic = aMaterial.y;
}`;

const FragmentShaderSource = `#version 300 es
precision highp float;
in vec3 vNormal;
in vec3 vColor;
in vec3 vWorld;
in vec2 vUv;
in float vEmission;
flat in float vRoughness;
flat in float vMetallic;
uniform bool uHasTexture;
uniform bool uHasEmission;
uniform bool uEmissionView;
uniform int uTexSize;
uniform vec3 uEye;
uniform sampler2D uColorMap;
uniform sampler2D uNormalMap;
uniform sampler2D uMaterialMap;
uniform sampler2D uEmissionMap;
uniform vec3 uLightDir;
out vec4 outColor;
${EnvironmentSource}

// the engine samples with texX = uv * (size - 1) / 65535, so the same integer lookup is
// reproduced here instead of relying on filtered texture coordinates
ivec2 atlasTexel() {
	return ivec2((vUv * float(uTexSize - 1)) / 65535.0);
}

// emission above 1 is legal (the per triangle scalar can be 100 or 1000), so the excess is
// compressed instead of clipping to white; values in 0..1 are left exactly as they were
vec3 highlightRolloff(vec3 c) {
	float over = max(max(c.r, c.g), c.b) - 1.0;
	return over > 0.0 ? c / (1.0 + over * 0.5) : c;
}

// cotangent frame from screen space derivatives, so a painted normal map lines up with the UVs
mat3 cotangentFrame(vec3 n, vec3 p, vec2 uv) {
	vec3 dp1 = dFdx(p);
	vec3 dp2 = dFdy(p);
	vec2 duv1 = dFdx(uv);
	vec2 duv2 = dFdy(uv);
	vec3 dp2perp = cross(dp2, n);
	vec3 dp1perp = cross(n, dp1);
	vec3 t = dp2perp * duv1.x + dp1perp * duv2.x;
	vec3 b = dp2perp * duv1.y + dp1perp * duv2.y;
	float invmax = inversesqrt(max(dot(t, t), dot(b, b)) + 1e-8);
	return mat3(t * invmax, b * invmax, n);
}

void main() {
	vec3 base = vColor;
	float roughness = clamp(vRoughness, 0.0, 1.0);
	float metallic = clamp(vMetallic, 0.0, 1.0);
	vec3 n = normalize(vNormal);
	if (uHasTexture) {
		ivec2 texel = atlasTexel();
		base = texelFetch(uColorMap, texel, 0).rgb;
		vec2 rm = texelFetch(uMaterialMap, texel, 0).rg;
		roughness = clamp(rm.x, 0.0, 1.0);
		metallic = clamp(rm.y, 0.0, 1.0);
		vec3 rawNormal = texelFetch(uNormalMap, texel, 0).rgb;
		// texels outside every material cell stay zero, those keep the geometric normal
		if (any(greaterThan(rawNormal, vec3(0.0)))) {
			n = normalize(cotangentFrame(n, vWorld, vUv) * (rawNormal * 2.0 - 1.0));
		}
	}
	vec3 v = normalize(uEye - vWorld);
	vec3 l = normalize(uLightDir);
	vec3 h = normalize(l + v);
	float ndl = max(dot(n, l), 0.0);
	float ndh = max(dot(n, h), 0.0);
	float ndv = max(dot(n, v), 0.0);

	// unlit view for checking a painted emission map: the glow is shown as stored, plus a flat
	// base tint so the silhouette is still readable where nothing emits
	if (uEmissionView) {
		vec3 emissionColor = base * vEmission * 0.01;
		if (uHasEmission) emissionColor += texelFetch(uEmissionMap, atlasTexel(), 0).rgb;
		outColor = vec4(highlightRolloff(emissionColor) + base * 0.04, 1.0);
		return;
	}

	// rough surfaces fade the mirror toward a flat term; not a real GGX, just enough to answer
	// visibly to the material map
	vec3 envColor = mix(environment(reflect(-v, n)), environment(vec3(0.0, 1.0, 0.0)) * 0.5, roughness * 0.65);
	vec3 f0 = mix(vec3(0.04), base, metallic);
	float fresnel = pow(1.0 - ndv, 5.0);
	vec3 reflected = envColor * (f0 + (1.0 - f0) * fresnel);

	float shininess = mix(8.0, 260.0, 1.0 - roughness);
	float gloss = pow(ndh, shininess) * mix(0.04, 0.8, metallic) * (1.0 - roughness * 0.6);
	vec3 specular = mix(vec3(1.0), base, metallic) * gloss;

	vec3 diffuse = base * ndl * (1.0 - metallic);
	vec3 ambient = base * (0.10 + 0.18 * (1.0 - roughness)) * (1.0 - 0.7 * metallic);
	vec3 lit = ambient + diffuse + reflected * mix(0.35, 1.0, metallic) + specular;
	vec3 glow = base * vEmission * 0.01;
	if (uHasEmission) glow += texelFetch(uEmissionMap, atlasTexel(), 0).rgb * 0.9;
	outColor = vec4(lit + highlightRolloff(glow), 1.0);
}`;

const BackgroundVertexSource = `#version 300 es
out vec2 vNdc;
void main() {
	vec2 corners[3] = vec2[3](vec2(-1.0, -1.0), vec2(3.0, -1.0), vec2(-1.0, 3.0));
	vNdc = corners[gl_VertexID];
	gl_Position = vec4(vNdc, 0.0, 1.0);
}`;

const BackgroundFragmentSource = `#version 300 es
precision highp float;
in vec2 vNdc;
uniform vec3 uForward;
uniform vec3 uRight;
uniform vec3 uUp;
uniform float uTanHalf;
uniform float uAspect;
uniform vec3 uLightDir;
out vec4 outColor;
${EnvironmentSource}
void main() {
	vec3 dir = normalize(uForward + uRight * (vNdc.x * uTanHalf * uAspect) + uUp * (vNdc.y * uTanHalf));
	outColor = vec4(environment(dir), 1.0);
}`;

const WireframeShaderSource = `#version 300 es
layout(location=0) in vec3 aPos;
uniform mat4 uViewProj;
void main() {
	gl_Position = uViewProj * vec4(aPos, 1.0);
}`;

const WireframeFragmentSource = `#version 300 es
precision highp float;
uniform vec3 uColor;
out vec4 outColor;
void main() { outColor = vec4(uColor, 1.0); }`;

function compileShader(gl, type, source) {
	const shader = gl.createShader(type);
	gl.shaderSource(shader, source);
	gl.compileShader(shader);
	if (!gl.getShaderParameter(shader, gl.COMPILE_STATUS)) {
		const log = gl.getShaderInfoLog(shader);
		gl.deleteShader(shader);
		throw new Error("shader compile failed: " + log);
	}
	return shader;
}

function linkProgram(gl, vertexSource, fragmentSource) {
	const program = gl.createProgram();
	const vs = compileShader(gl, gl.VERTEX_SHADER, vertexSource);
	const fs = compileShader(gl, gl.FRAGMENT_SHADER, fragmentSource);
	gl.attachShader(program, vs);
	gl.attachShader(program, fs);
	gl.linkProgram(program);
	gl.deleteShader(vs);
	gl.deleteShader(fs);
	if (!gl.getProgramParameter(program, gl.LINK_STATUS)) {
		const log = gl.getProgramInfoLog(program);
		gl.deleteProgram(program);
		throw new Error("shader link failed: " + log);
	}
	return program;
}

function perspective(fovY, aspect, near, far) {
	const f = 1.0 / Math.tan(fovY / 2);
	const out = new Float32Array(16);
	out[0] = f / aspect;
	out[5] = f;
	out[10] = (far + near) / (near - far);
	out[11] = -1;
	out[14] = (2 * far * near) / (near - far);
	return out;
}

function lookAt(eye, center, up) {
	const z = normalize3([eye[0] - center[0], eye[1] - center[1], eye[2] - center[2]]);
	const x = normalize3(cross3(up, z));
	const y = cross3(z, x);
	const out = new Float32Array(16);
	out[0] = x[0]; out[1] = y[0]; out[2] = z[0]; out[3] = 0;
	out[4] = x[1]; out[5] = y[1]; out[6] = z[1]; out[7] = 0;
	out[8] = x[2]; out[9] = y[2]; out[10] = z[2]; out[11] = 0;
	out[12] = -dot3(x, eye); out[13] = -dot3(y, eye); out[14] = -dot3(z, eye); out[15] = 1;
	return out;
}

function multiplyMatrices(a, b) {
	const out = new Float32Array(16);
	for (let c = 0; c < 4; c++) {
		for (let r = 0; r < 4; r++) {
			out[c * 4 + r] = a[r] * b[c * 4] + a[4 + r] * b[c * 4 + 1] + a[8 + r] * b[c * 4 + 2] + a[12 + r] * b[c * 4 + 3];
		}
	}
	return out;
}

function cross3(a, b) {
	return [a[1] * b[2] - a[2] * b[1], a[2] * b[0] - a[0] * b[2], a[0] * b[1] - a[1] * b[0]];
}

function dot3(a, b) { return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]; }

// buildGpuRecords expands the 108 byte records into the interleaved vertex stream. The engine
// stores one normal per triangle, so each of the three vertices repeats it.
function buildGpuRecords(model, onProgress) {
	const vertexCount = model.triCount * 3;
	const data = new Float32Array(vertexCount * Preview.floatsPerVertex);
	// the uv slots are raw u16 pairs, so they are written through an integer view of the same
	// buffer instead of as floats
	const u32 = new Uint32Array(data.buffer);
	const dv = new DataView(model.triView.buffer, model.triView.byteOffset, model.triView.byteLength);

	function range(start, end) {
		for (let i = start; i < end; i++) {
			const base = i * Rec.size;
			const n0 = dv.getFloat32(base + Rec.normal, true);
			const n1 = dv.getFloat32(base + Rec.normal + 4, true);
			const n2 = dv.getFloat32(base + Rec.normal + 8, true);
			const rough = dv.getFloat32(base + Rec.roughness, true);
			const metal = dv.getFloat32(base + Rec.metallic, true);
			const emission = dv.getFloat32(base + Rec.emission, true);
			const c0 = dv.getFloat32(base + Rec.color, true);
			const c1 = dv.getFloat32(base + Rec.color + 4, true);
			const c2 = dv.getFloat32(base + Rec.color + 8, true);

			for (let corner = 0; corner < 3; corner++) {
				const recordOffset = base + corner * 16;
				const dst = (i * 3 + corner) * Preview.floatsPerVertex;
				data[dst + VertexLayout.pos] = dv.getFloat32(recordOffset, true);
				data[dst + VertexLayout.pos + 1] = dv.getFloat32(recordOffset + 4, true);
				data[dst + VertexLayout.pos + 2] = dv.getFloat32(recordOffset + 8, true);
				data[dst + VertexLayout.normal] = n0;
				data[dst + VertexLayout.normal + 1] = n1;
				data[dst + VertexLayout.normal + 2] = n2;
				data[dst + VertexLayout.color] = c0;
				data[dst + VertexLayout.color + 1] = c1;
				data[dst + VertexLayout.color + 2] = c2;
				data[dst + VertexLayout.material] = rough;
				data[dst + VertexLayout.material + 1] = metal;
				data[dst + VertexLayout.material + 2] = emission;
				u32[dst + VertexLayout.uv] = dv.getUint32(base + Rec.uv + corner * 4, true);
			}
		}
	}

	return runChunked(model.triCount, 24576, range, onProgress).then(function () {
		return { data: data, vertexCount: vertexCount };
	});
}

function computeBounds(model) {
	const dv = new DataView(model.triView.buffer, model.triView.byteOffset, model.triView.byteLength);
	const min = [Infinity, Infinity, Infinity];
	const max = [-Infinity, -Infinity, -Infinity];
	for (let i = 0; i < model.triCount; i++) {
		const base = i * Rec.size;
		for (let corner = 0; corner < 3; corner++) {
			for (let axis = 0; axis < 3; axis++) {
				const value = dv.getFloat32(base + corner * 16 + axis * 4, true);
				if (value < min[axis]) min[axis] = value;
				if (value > max[axis]) max[axis] = value;
			}
		}
	}
	if (!isFinite(min[0])) return { min: [0, 0, 0], max: [1, 1, 1], center: [0, 0, 0], radius: 1 };
	const center = [(min[0] + max[0]) / 2, (min[1] + max[1]) / 2, (min[2] + max[2]) / 2];
	const radius = Math.max(max[0] - min[0], max[1] - min[1], max[2] - min[2]) / 2 || 1;
	return { min: min, max: max, center: center, radius: radius };
}

function PreviewRenderer(canvas) {
	this.gl = canvas.getContext("webgl2", { antialias: true, preserveDrawingBuffer: false });
	if (!this.gl) throw new Error("WebGL2 is not available in this browser");
	const gl = this.gl;
	this.canvas = canvas;
	this.program = linkProgram(gl, VertexShaderSource, FragmentShaderSource);
	this.wireProgram = linkProgram(gl, WireframeShaderSource, WireframeFragmentSource);
	this.backgroundProgram = linkProgram(gl, BackgroundVertexSource, BackgroundFragmentSource);
	this.uniforms = {
		viewProj: gl.getUniformLocation(this.program, "uViewProj"),
		hasTexture: gl.getUniformLocation(this.program, "uHasTexture"),
		hasEmission: gl.getUniformLocation(this.program, "uHasEmission"),
		texSize: gl.getUniformLocation(this.program, "uTexSize"),
		colorMap: gl.getUniformLocation(this.program, "uColorMap"),
		normalMap: gl.getUniformLocation(this.program, "uNormalMap"),
		materialMap: gl.getUniformLocation(this.program, "uMaterialMap"),
		emissionMap: gl.getUniformLocation(this.program, "uEmissionMap"),
		lightDir: gl.getUniformLocation(this.program, "uLightDir"),
		eye: gl.getUniformLocation(this.program, "uEye"),
		emissionView: gl.getUniformLocation(this.program, "uEmissionView"),
	};
	this.wireUniforms = {
		viewProj: gl.getUniformLocation(this.wireProgram, "uViewProj"),
		color: gl.getUniformLocation(this.wireProgram, "uColor"),
	};
	this.backgroundUniforms = {
		forward: gl.getUniformLocation(this.backgroundProgram, "uForward"),
		right: gl.getUniformLocation(this.backgroundProgram, "uRight"),
		up: gl.getUniformLocation(this.backgroundProgram, "uUp"),
		tanHalf: gl.getUniformLocation(this.backgroundProgram, "uTanHalf"),
		aspect: gl.getUniformLocation(this.backgroundProgram, "uAspect"),
		lightDir: gl.getUniformLocation(this.backgroundProgram, "uLightDir"),
	};
	this.vbo = gl.createBuffer();
	this.wireIbo = gl.createBuffer();
	this.emptyBuffer = gl.createBuffer();
	gl.bindBuffer(gl.ARRAY_BUFFER, this.emptyBuffer);
	this.textures = { color: null, normal: null, material: null, emission: null };
	this.atlasSize = 0;
	this.emissionSize = 0;
	this.vertexCount = 0;
	this.wireIndexCount = 0;
	this.bounds = { center: [0, 0, 0], radius: 1 };
	this.camera = { yaw: 0.7, pitch: 0.35, distance: 3, target: [0, 0, 0] };
	this.showTextures = true;
	this.wireframe = false;
	this.cullBackfaces = false;
	this.showEmissionView = false;
	this.lightDir = normalize3([-0.4, 0.8, 0.5]);
	gl.enable(gl.DEPTH_TEST);
	gl.clearColor(0.11, 0.12, 0.14, 1);
}

PreviewRenderer.prototype.loadModel = function (model, onProgress) {
	const self = this;
	return buildGpuRecords(model, onProgress).then(function (built) {
		const gl = self.gl;
		gl.bindBuffer(gl.ARRAY_BUFFER, self.vbo);
		gl.bufferData(gl.ARRAY_BUFFER, built.data, gl.STATIC_DRAW);
		self.vertexCount = built.vertexCount;
		built.data = null;

		const wireIndices = new Uint32Array(model.triCount * 6);
		for (let i = 0, w = 0; i < model.triCount; i++) {
			const b = i * 3;
			wireIndices[w++] = b; wireIndices[w++] = b + 1;
			wireIndices[w++] = b + 1; wireIndices[w++] = b + 2;
			wireIndices[w++] = b + 2; wireIndices[w++] = b;
		}
		gl.bindBuffer(gl.ELEMENT_ARRAY_BUFFER, self.wireIbo);
		gl.bufferData(gl.ELEMENT_ARRAY_BUFFER, wireIndices, gl.STATIC_DRAW);
		self.wireIndexCount = wireIndices.length;

		self.bounds = computeBounds(model);
		self.camera.target = self.bounds.center.slice();
		self.camera.distance = self.bounds.radius * 2.6;
		self.showTextures = model.atlasView !== null;
		self.cullBackfaces = model.cullBackfaces;
		self.uploadAtlas(model);
		self.releaseEmission();
		self.render();
	});
};

// updateVertexStream rebuilds and re-uploads the interleaved stream after a per triangle material
// edit, leaving the camera, the atlas and the painted emission alone
PreviewRenderer.prototype.updateVertexStream = function (model, onProgress) {
	const self = this;
	return buildGpuRecords(model, onProgress).then(function (built) {
		const gl = self.gl;
		gl.bindBuffer(gl.ARRAY_BUFFER, self.vbo);
		gl.bufferData(gl.ARRAY_BUFFER, built.data, gl.STATIC_DRAW);
		self.vertexCount = built.vertexCount;
		built.data = null;
		self.render();
	});
};

PreviewRenderer.prototype.uploadAtlas = function (model) {
	const gl = this.gl;
	this.releaseAtlas();
	if (!model.atlasView) { this.atlasSize = 0; return; }
	const size = model.textureSize;
	const texelCount = size * size;
	this.atlasSize = size;
	this.textures.color = this.uploadPlane(model.atlasView.subarray(0, texelCount * 4), size, gl.RGBA8, gl.RGBA);
	this.textures.normal = this.uploadPlane(model.atlasView.subarray(texelCount * 4, texelCount * 7), size, gl.RGB8, gl.RGB);
	this.textures.material = this.uploadPlane(model.atlasView.subarray(texelCount * 7, texelCount * 9), size, gl.RG8, gl.RG);
};

PreviewRenderer.prototype.uploadPlane = function (bytes, size, internalFormat, format) {
	const gl = this.gl;
	const texture = gl.createTexture();
	gl.bindTexture(gl.TEXTURE_2D, texture);
	gl.pixelStorei(gl.UNPACK_ALIGNMENT, 1);
	gl.texStorage2D(gl.TEXTURE_2D, 1, internalFormat, size, size);
	gl.texSubImage2D(gl.TEXTURE_2D, 0, 0, 0, size, size, format, gl.UNSIGNED_BYTE, bytes);
	gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_MIN_FILTER, gl.NEAREST);
	gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_MAG_FILTER, gl.NEAREST);
	gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_WRAP_S, gl.CLAMP_TO_EDGE);
	gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_WRAP_T, gl.CLAMP_TO_EDGE);
	return texture;
};

// updateEmission takes top-down RGB bytes so the painted map lines up with the color atlas rows
PreviewRenderer.prototype.updateEmission = function (rgb, size) {
	const gl = this.gl;
	if (!rgb) { this.releaseEmission(); this.render(); return; }
	if (!this.textures.emission || this.emissionSize !== size) {
		if (this.textures.emission) gl.deleteTexture(this.textures.emission);
		this.textures.emission = this.uploadPlane(rgb, size, gl.RGB8, gl.RGB);
		this.emissionSize = size;
	} else {
		gl.bindTexture(gl.TEXTURE_2D, this.textures.emission);
		gl.pixelStorei(gl.UNPACK_ALIGNMENT, 1);
		gl.texSubImage2D(gl.TEXTURE_2D, 0, 0, 0, size, size, gl.RGB, gl.UNSIGNED_BYTE, rgb);
	}
	this.render();
};

PreviewRenderer.prototype.uploadSubPlane = function (texture, bytes, format, x, y, width, height) {
	const gl = this.gl;
	gl.bindTexture(gl.TEXTURE_2D, texture);
	gl.pixelStorei(gl.UNPACK_ALIGNMENT, 1);
	gl.texSubImage2D(gl.TEXTURE_2D, 0, x, y, width, height, format, gl.UNSIGNED_BYTE, bytes);
};

// updateAtlasRegion re-uploads only the rectangle a stroke touched; the three planes live at
// different bytes per texel inside atlasView, so UNPACK_ROW_LENGTH addresses them in place.
PreviewRenderer.prototype.updateAtlasRegion = function (model, x0, y0, x1, y1) {
	if (!model.atlasView || !this.textures.color) return;
	const gl = this.gl;
	const size = model.textureSize;
	const count = size * size;
	const left = clamp(x0, 0, size - 1);
	const top = clamp(y0, 0, size - 1);
	const right = clamp(x1, 0, size - 1);
	const bottom = clamp(y1, 0, size - 1);
	if (right < left || bottom < top) return;
	const width = right - left + 1;
	const height = bottom - top + 1;
	gl.pixelStorei(gl.UNPACK_ROW_LENGTH, size);
	gl.pixelStorei(gl.UNPACK_SKIP_PIXELS, left);
	gl.pixelStorei(gl.UNPACK_SKIP_ROWS, top);
	this.uploadSubPlane(this.textures.color, model.atlasView.subarray(0, count * 4), gl.RGBA, left, top, width, height);
	this.uploadSubPlane(this.textures.normal, model.atlasView.subarray(count * AtlasPlane.normal, count * AtlasPlane.material), gl.RGB, left, top, width, height);
	this.uploadSubPlane(this.textures.material, model.atlasView.subarray(count * AtlasPlane.material), gl.RG, left, top, width, height);
	gl.pixelStorei(gl.UNPACK_ROW_LENGTH, 0);
	gl.pixelStorei(gl.UNPACK_SKIP_PIXELS, 0);
	gl.pixelStorei(gl.UNPACK_SKIP_ROWS, 0);
	this.render();
};

// updateEmissionRegion uploads only the rectangle a stroke touched; rgb rows are size wide
PreviewRenderer.prototype.updateEmissionRegion = function (rgb, size, x0, y0, x1, y1) {
	if (!this.textures.emission || this.emissionSize !== size) { this.updateEmission(rgb, size); return; }
	const gl = this.gl;
	const left = clamp(x0, 0, size - 1);
	const top = clamp(y0, 0, size - 1);
	const right = clamp(x1, 0, size - 1);
	const bottom = clamp(y1, 0, size - 1);
	if (right < left || bottom < top) return;
	const width = right - left + 1;
	const height = bottom - top + 1;
	gl.bindTexture(gl.TEXTURE_2D, this.textures.emission);
	gl.pixelStorei(gl.UNPACK_ALIGNMENT, 1);
	gl.pixelStorei(gl.UNPACK_ROW_LENGTH, size);
	gl.pixelStorei(gl.UNPACK_SKIP_PIXELS, left);
	gl.pixelStorei(gl.UNPACK_SKIP_ROWS, top);
	gl.texSubImage2D(gl.TEXTURE_2D, 0, left, top, width, height, gl.RGB, gl.UNSIGNED_BYTE, rgb);
	gl.pixelStorei(gl.UNPACK_ROW_LENGTH, 0);
	gl.pixelStorei(gl.UNPACK_SKIP_PIXELS, 0);
	gl.pixelStorei(gl.UNPACK_SKIP_ROWS, 0);
	this.render();
};

PreviewRenderer.prototype.releaseAtlas = function () {
	const gl = this.gl;
	["color", "normal", "material"].forEach(function (key) {
		if (this.textures[key]) { gl.deleteTexture(this.textures[key]); this.textures[key] = null; }
	}, this);
	this.atlasSize = 0;
};

PreviewRenderer.prototype.releaseEmission = function () {
	if (this.textures.emission) {
		this.gl.deleteTexture(this.textures.emission);
		this.textures.emission = null;
	}
	this.emissionSize = 0;
};

PreviewRenderer.prototype.release = function () {
	this.releaseAtlas();
	this.releaseEmission();
	const gl = this.gl;
	gl.deleteBuffer(this.vbo);
	gl.deleteBuffer(this.wireIbo);
	gl.deleteBuffer(this.emptyBuffer);
	gl.deleteProgram(this.program);
	gl.deleteProgram(this.wireProgram);
	gl.deleteProgram(this.backgroundProgram);
};

PreviewRenderer.prototype.frameCamera = function () {
	this.camera.yaw = 0.7;
	this.camera.pitch = 0.35;
	this.camera.distance = this.bounds.radius * 2.6;
	this.camera.target = this.bounds.center.slice();
	this.render();
};

PreviewRenderer.prototype.orbit = function (dx, dy) {
	this.camera.yaw += dx;
	this.camera.pitch = clamp(this.camera.pitch + dy, -1.5, 1.5);
	this.render();
};

PreviewRenderer.prototype.zoom = function (factor) {
	this.camera.distance = clamp(this.camera.distance * factor, this.bounds.radius * 0.02, this.bounds.radius * 60);
	this.render();
};

PreviewRenderer.prototype.pan = function (dx, dy) {
	const scale = this.camera.distance * 0.0016;
	const forward = normalize3([
		Math.cos(this.camera.pitch) * Math.sin(this.camera.yaw),
		Math.sin(this.camera.pitch),
		Math.cos(this.camera.pitch) * Math.cos(this.camera.yaw),
	]);
	const right = normalize3(cross3(forward, [0, 1, 0]));
	const up = cross3(right, forward);
	for (let axis = 0; axis < 3; axis++) {
		this.camera.target[axis] += (-dx * right[axis] + dy * up[axis]) * scale;
	}
	this.render();
};

// cameraEye is where viewProjection and cameraRay put the viewer, kept in one place
PreviewRenderer.prototype.cameraEye = function () {
	const distance = this.camera.distance;
	return [
		this.camera.target[0] + distance * Math.cos(this.camera.pitch) * Math.sin(this.camera.yaw),
		this.camera.target[1] + distance * Math.sin(this.camera.pitch),
		this.camera.target[2] + distance * Math.cos(this.camera.pitch) * Math.cos(this.camera.yaw),
	];
};

PreviewRenderer.prototype.viewProjection = function () {
	const aspect = this.canvas.clientWidth / Math.max(1, this.canvas.clientHeight);
	const near = this.bounds.radius * 0.01;
	const far = this.bounds.radius * 200;
	return multiplyMatrices(perspective(Math.PI / 4, aspect, near, far), lookAt(this.cameraEye(), this.camera.target, [0, 1, 0]));
};

// cameraBasis is the frame the background pass and cameraRay both need
PreviewRenderer.prototype.cameraBasis = function () {
	const eye = this.cameraEye();
	const forward = normalize3([
		this.camera.target[0] - eye[0],
		this.camera.target[1] - eye[1],
		this.camera.target[2] - eye[2],
	]);
	const right = normalize3(cross3(forward, [0, 1, 0]));
	return { eye: eye, forward: forward, right: right, up: cross3(right, forward), tanHalf: Math.tan(Math.PI / 8) };
};

// cameraRay turns a canvas pixel (CSS coordinates, origin top left) into a world space ray whose
// basis matches viewProjection, so a hit lines up with what is on screen
PreviewRenderer.prototype.cameraRay = function (px, py) {
	const width = Math.max(1, this.canvas.clientWidth);
	const height = Math.max(1, this.canvas.clientHeight);
	const ndcX = (px / width) * 2 - 1;
	const ndcY = 1 - (py / height) * 2;
	const basis = this.cameraBasis();
	const aspect = width / height;
	const scaleX = ndcX * basis.tanHalf * aspect;
	const scaleY = ndcY * basis.tanHalf;
	const direction = normalize3([
		basis.forward[0] + basis.right[0] * scaleX + basis.up[0] * scaleY,
		basis.forward[1] + basis.right[1] * scaleX + basis.up[1] * scaleY,
		basis.forward[2] + basis.right[2] * scaleX + basis.up[2] * scaleY,
	]);
	return { origin: basis.eye, direction: direction };
};

// pickTriangle is a Moller Trumbore pass over the raw 108 byte records: no mesh copy, and the UVs
// come out barycentrically interpolated in u16 units. A brush stroke costs one pass per sample.
function pickTriangle(model, origin, direction) {
	const triView = model.triView;
	if ((triView.byteOffset & 3) !== 0) return null;
	const floats = new Float32Array(triView.buffer, triView.byteOffset, triView.byteLength >> 2);
	const uvs = new Uint16Array(triView.buffer, triView.byteOffset, triView.byteLength >> 1);
	const ox = origin[0], oy = origin[1], oz = origin[2];
	const dx = direction[0], dy = direction[1], dz = direction[2];
	let best = Infinity;
	let hitU = 0, hitV = 0;

	for (let i = 0; i < model.triCount; i++) {
		const f = i * 27;
		const ax = floats[f], ay = floats[f + 1], az = floats[f + 2];
		const e1x = floats[f + 4] - ax, e1y = floats[f + 5] - ay, e1z = floats[f + 6] - az;
		const e2x = floats[f + 8] - ax, e2y = floats[f + 9] - ay, e2z = floats[f + 10] - az;

		const px = dy * e2z - dz * e2y;
		const py = dz * e2x - dx * e2z;
		const pz = dx * e2y - dy * e2x;
		const det = e1x * px + e1y * py + e1z * pz;
		if (det > -1e-9 && det < 1e-9) continue;
		const inv = 1 / det;

		const tx = ox - ax, ty = oy - ay, tz = oz - az;
		const u = (tx * px + ty * py + tz * pz) * inv;
		if (u < 0 || u > 1) continue;

		const qx = ty * e1z - tz * e1y;
		const qy = tz * e1x - tx * e1z;
		const qz = tx * e1y - ty * e1x;
		const v = (dx * qx + dy * qy + dz * qz) * inv;
		if (v < 0 || u + v > 1) continue;

		const t = (e2x * qx + e2y * qy + e2z * qz) * inv;
		if (t <= 1e-6 || t >= best) continue;
		best = t;

		const base = i * 54;
		const w = 1 - u - v;
		hitU = w * uvs[base + 46] + u * uvs[base + 48] + v * uvs[base + 50];
		hitV = w * uvs[base + 47] + u * uvs[base + 49] + v * uvs[base + 51];
	}
	if (best === Infinity) return null;
	return { u: hitU, v: hitV, distance: best };
}

// findSurfaceAtUv walks the records for a triangle whose UV triangle contains the point (u16
// units) and returns the barycentrically interpolated world position. A UV shared by several
// triangles only happens at a seam, so the first match is good enough for a cursor marker.
function findSurfaceAtUv(model, u, v) {
	const triView = model.triView;
	if ((triView.byteOffset & 3) !== 0) return null;
	const floats = new Float32Array(triView.buffer, triView.byteOffset, triView.byteLength >> 2);
	const uvs = new Uint16Array(triView.buffer, triView.byteOffset, triView.byteLength >> 1);
	for (let i = 0; i < model.triCount; i++) {
		const base = i * 54;
		const u1 = uvs[base + 46], v1 = uvs[base + 47];
		const u2 = uvs[base + 48], v2 = uvs[base + 49];
		const u3 = uvs[base + 50], v3 = uvs[base + 51];

		// bounding box reject first: most triangles never reach the divisions
		if (u < Math.min(u1, u2, u3) || u > Math.max(u1, u2, u3)) continue;
		if (v < Math.min(v1, v2, v3) || v > Math.max(v1, v2, v3)) continue;

		const det = (v2 - v3) * (u1 - u3) + (u3 - u2) * (v1 - v3);
		if (det === 0) continue;
		const w1 = ((v2 - v3) * (u - u3) + (u3 - u2) * (v - v3)) / det;
		if (w1 < 0 || w1 > 1) continue;
		const w2 = ((v3 - v1) * (u - u3) + (u1 - u3) * (v - v3)) / det;
		if (w2 < 0 || w1 + w2 > 1) continue;
		const w3 = 1 - w1 - w2;

		const f = i * 27;
		return [
			w1 * floats[f] + w2 * floats[f + 4] + w3 * floats[f + 8],
			w1 * floats[f + 1] + w2 * floats[f + 5] + w3 * floats[f + 9],
			w1 * floats[f + 2] + w2 * floats[f + 6] + w3 * floats[f + 10],
		];
	}
	return null;
}

// projectToScreen maps a world point to canvas CSS pixels; null when it is behind the eye
PreviewRenderer.prototype.projectToScreen = function (point) {
	const m = this.viewProjection();
	const x = m[0] * point[0] + m[4] * point[1] + m[8] * point[2] + m[12];
	const y = m[1] * point[0] + m[5] * point[1] + m[9] * point[2] + m[13];
	const w = m[3] * point[0] + m[7] * point[1] + m[11] * point[2] + m[15];
	if (w <= 1e-6) return null;
	return {
		x: (x / w * 0.5 + 0.5) * this.canvas.clientWidth,
		y: (0.5 - y / w * 0.5) * this.canvas.clientHeight,
	};
};

PreviewRenderer.prototype.resize = function () {
	const canvas = this.canvas;
	const dpr = Math.min(2, window.devicePixelRatio || 1);
	const width = Math.max(1, Math.floor(canvas.clientWidth * dpr));
	const height = Math.max(1, Math.floor(canvas.clientHeight * dpr));
	if (canvas.width !== width || canvas.height !== height) {
		canvas.width = width;
		canvas.height = height;
	}
	this.gl.viewport(0, 0, width, height);
};

PreviewRenderer.prototype.drawBackground = function () {
	const gl = this.gl;
	const basis = this.cameraBasis();
	const aspect = this.canvas.clientWidth / Math.max(1, this.canvas.clientHeight);
	gl.useProgram(this.backgroundProgram);
	gl.uniform3fv(this.backgroundUniforms.forward, basis.forward);
	gl.uniform3fv(this.backgroundUniforms.right, basis.right);
	gl.uniform3fv(this.backgroundUniforms.up, basis.up);
	gl.uniform1f(this.backgroundUniforms.tanHalf, basis.tanHalf);
	gl.uniform1f(this.backgroundUniforms.aspect, aspect);
	gl.uniform3fv(this.backgroundUniforms.lightDir, this.lightDir);
	// the fullscreen triangle owns no attributes, and attributes stay enabled between frames
	// otherwise, which would read the mesh buffer here
	for (let location = 0; location < 5; location++) gl.disableVertexAttribArray(location);
	gl.depthMask(false);
	gl.disable(gl.DEPTH_TEST);
	gl.drawArrays(gl.TRIANGLES, 0, 3);
	gl.enable(gl.DEPTH_TEST);
	gl.depthMask(true);
};

PreviewRenderer.prototype.render = function () {
	const gl = this.gl;
	this.resize();
	gl.clear(gl.COLOR_BUFFER_BIT | gl.DEPTH_BUFFER_BIT);
	this.drawBackground();
	if (this.vertexCount === 0) return;

	const useTexture = this.showTextures && this.atlasSize > 0;
	const useEmission = this.textures.emission !== null;
	const viewProj = this.viewProjection();

	gl.enableVertexAttribArray(0);
	gl.enableVertexAttribArray(1);
	gl.enableVertexAttribArray(2);
	gl.enableVertexAttribArray(3);
	gl.enableVertexAttribArray(4);
	gl.bindBuffer(gl.ARRAY_BUFFER, this.vbo);
	const stride = VertexLayout.strideBytes;
	gl.vertexAttribPointer(0, 3, gl.FLOAT, false, stride, VertexLayout.pos * 4);
	gl.vertexAttribPointer(1, 3, gl.FLOAT, false, stride, VertexLayout.normal * 4);
	gl.vertexAttribIPointer(2, 2, gl.UNSIGNED_SHORT, stride, VertexLayout.uv * 4);
	gl.vertexAttribPointer(3, 3, gl.FLOAT, false, stride, VertexLayout.color * 4);
	gl.vertexAttribPointer(4, 3, gl.FLOAT, false, stride, VertexLayout.material * 4);

	gl.useProgram(this.program);
	gl.uniformMatrix4fv(this.uniforms.viewProj, false, viewProj);
	gl.uniform1i(this.uniforms.hasTexture, useTexture ? 1 : 0);
	gl.uniform1i(this.uniforms.hasEmission, useEmission ? 1 : 0);
	gl.uniform1i(this.uniforms.emissionView, this.showEmissionView ? 1 : 0);
	gl.uniform1i(this.uniforms.texSize, this.atlasSize);
	gl.uniform3fv(this.uniforms.lightDir, this.lightDir);
	gl.uniform3fv(this.uniforms.eye, this.cameraEye());

	if (useTexture) {
		gl.activeTexture(gl.TEXTURE0); gl.bindTexture(gl.TEXTURE_2D, this.textures.color);
		gl.uniform1i(this.uniforms.colorMap, 0);
		gl.activeTexture(gl.TEXTURE1); gl.bindTexture(gl.TEXTURE_2D, this.textures.normal);
		gl.uniform1i(this.uniforms.normalMap, 1);
		gl.activeTexture(gl.TEXTURE2); gl.bindTexture(gl.TEXTURE_2D, this.textures.material);
		gl.uniform1i(this.uniforms.materialMap, 2);
	}
	if (useEmission) {
		gl.activeTexture(gl.TEXTURE3); gl.bindTexture(gl.TEXTURE_2D, this.textures.emission);
		gl.uniform1i(this.uniforms.emissionMap, 3);
	}

	if (this.cullBackfaces) gl.enable(gl.CULL_FACE); else gl.disable(gl.CULL_FACE);
	gl.drawArrays(gl.TRIANGLES, 0, this.vertexCount);

	if (this.wireframe && this.wireIndexCount > 0) {
		gl.useProgram(this.wireProgram);
		gl.uniformMatrix4fv(this.wireUniforms.viewProj, false, viewProj);
		gl.uniform3f(this.wireUniforms.color, 0.75, 0.85, 1.0);
		gl.disableVertexAttribArray(1);
		gl.disableVertexAttribArray(2);
		gl.disableVertexAttribArray(3);
		gl.disableVertexAttribArray(4);
		gl.enableVertexAttribArray(0);
		gl.bindBuffer(gl.ELEMENT_ARRAY_BUFFER, this.wireIbo);
		gl.drawElements(gl.LINES, this.wireIndexCount, gl.UNSIGNED_INT, 0);
	}
};

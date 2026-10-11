// WebGL2 replay of the graphics slots recorded by src/platform/glide.c. Every
// animation frame executes the published slots in publication order, hands
// them back to the game and presents finished frames through PostFx.
import {
	BLEND_ONE, BLEND_ZERO, CTL_CFG_KEEP_ASPECT, CTL_CFG_LINEAR_FILTER, GAME_HEIGHT, GAME_WIDTH, GFX_CMD_OFFSET,
	GFX_CMD_WORDS, GFX_DEFAULT_BILINEAR, GFX_DEFAULT_BLEND_DST, GFX_DEFAULT_BLEND_SRC, GFX_DEFAULT_CLAMP,
	GFX_DEFAULT_DEPTH_FUNC, GFX_DEFAULT_DEPTH_MASK, GFX_FOG_ENTRIES, GFX_FRAME, GFX_FREE, GFX_NO_TEXTURE,
	GFX_OP_BLEND, GFX_OP_CLEAR, GFX_OP_CLIP, GFX_OP_DEPTHFUNC, GFX_OP_DEPTHMASK, GFX_OP_FOGCOLOR,
	GFX_OP_FOGENABLE, GFX_OP_FOGTABLE, GFX_OP_GAMMA, GFX_OP_MASK, GFX_OP_SAMPLER, GFX_OP_SIZE_SHIFT,
	GFX_OP_SWAP, GFX_OP_TEXBIND, GFX_OP_TEXENABLE, GFX_OP_TEXIMAGE, GFX_OP_TRIS, GFX_PARTIAL, GFX_SLOT_BYTES,
	GFX_STATE, GFX_TEX_MAX_SIZE, GFX_VTX_BYTES, GFX_VTX_CAPACITY, GFX_VTX_COLOR_OFFSET, GFX_VTX_COUNT,
	GFX_VTX_OFFSET, GFX_VTX_OOW_OFFSET, GFX_VTX_ST_OFFSET,
} from './layout.js';
import { createProgram, createTarget, deleteTarget, uniformLocations } from './glutil.js';
import { PostFx } from './postfx.js';

const FOG_TEXTURE_WIDTH = 256; // the fog table as an R8 texture: w % width across, w / width down

// Vertices are in game units and already projected; varyings are
// interpolated linearly in screen space like on the original hardware.
const SCENE_VS = `#version 300 es
layout(location = 0) in vec2 a_position;
layout(location = 1) in float a_oow;
layout(location = 2) in vec2 a_st;
layout(location = 3) in vec4 a_color;
out vec4 v_color;
out vec3 v_stq;
void main() {
	v_color = a_color;
	v_stq = vec3(a_st, a_oow);
	gl_Position = vec4(a_position.x / ${(GAME_WIDTH / 2).toFixed(1)} - 1.0, 1.0 - a_position.y / ${(GAME_HEIGHT / 2).toFixed(1)}, 0.0, 1.0);
}`;

// Depth is 1/w in a float buffer (nearer is larger), which keeps precision
// over the game's whole W range; depth functions are mirrored to match.
const SCENE_FS = `#version 300 es
precision highp float;
uniform sampler2D u_texture;
uniform sampler2D u_fogTable;
uniform bool u_textured;
uniform bool u_fog;
uniform vec3 u_fogColor;
in vec4 v_color;
in vec3 v_stq;
out vec4 o_color;
void main() {
	vec4 color = v_color;
	if (u_textured) {
		// Premultiplied texels filter without fringes; Glide combines straight colour.
		vec4 texel = texture(u_texture, v_stq.xy / v_stq.z);
		color *= vec4(texel.a > 0.0 ? texel.rgb / texel.a : vec3(0.0), texel.a);
	}
	if (color.a <= 16.0 / 255.0) // the game's alpha test, see the header of glide.c
		discard;
	if (u_fog) {
		const float maxW = ${(GFX_FOG_ENTRIES - 1).toFixed(1)};
		int w = int(clamp(1.0 / max(v_stq.z, 1.0 / maxW), 0.0, maxW));
		color.rgb = mix(color.rgb, u_fogColor, texelFetch(u_fogTable, ivec2(w % ${FOG_TEXTURE_WIDTH}, w / ${FOG_TEXTURE_WIDTH}), 0).r);
	}
	o_color = color;
	gl_FragDepth = clamp(v_stq.z, 0.0, 1.0);
}`;

const MAX_SCENE_WIDTH = 3840;
const MSAA_SAMPLES = 4;

const DIRTY_TARGET = 1;   // framebuffer, viewport, program, fixed enables, fog table unit
const DIRTY_VAO = 2;
const DIRTY_SCISSOR = 4;
const DIRTY_BLEND = 8;
const DIRTY_DEPTH = 16;
const DIRTY_TEXTURE = 32;
const DIRTY_SAMPLER = 64;
const DIRTY_UNIFORMS = 128;
const DIRTY_ALL = 255;

// Placement of the 4:3 picture inside a width x height area, or the whole area.
export function fitViewport(width, height, keepAspect) {
	if (!keepAspect)
		return { x: 0, y: 0, width, height };
	const w = Math.min(width, Math.round(height * 4 / 3)), h = Math.min(height, Math.round(width * 3 / 4));
	return { x: Math.floor((width - w) / 2), y: Math.floor((height - h) / 2), width: w, height: h };
}

// Glide's 16-bit W-buffer clear value (4-bit exponent, 12-bit mantissa) as 1/w.
export function clearDepth(value) {
	if (value >= 0xFFFF)
		return 0;
	return Math.min(1, 1 / ((1 + (value & 0xFFF) / 4096) * 2 ** (value >>> 12)));
}

export function slotViews(buffer, address) {
	return {
		u8: new Uint8Array(buffer, address, GFX_SLOT_BYTES),
		i32: new Int32Array(buffer, address, GFX_SLOT_BYTES >> 2),
		f32: new Float32Array(buffer, address, GFX_SLOT_BYTES >> 2),
	};
}

// A Glide 0xRRGGBB colour as 0..1 components.
const rgb = (color) => [((color >> 16) & 255) / 255, ((color >> 8) & 255) / 255, (color & 255) / 255];

// Index into Renderer.samplers for the GFX_OP_SAMPLER words.
const samplerIndex = (bilinear, clampS, clampT) => (bilinear ? 1 : 0) | (clampS ? 2 : 0) | (clampT ? 4 : 0);

function defaultState() {
	return {
		clip: [0, 0, GAME_WIDTH, GAME_HEIGHT], blendSrc: GFX_DEFAULT_BLEND_SRC, blendDst: GFX_DEFAULT_BLEND_DST,
		depthFunc: GFX_DEFAULT_DEPTH_FUNC, depthMask: GFX_DEFAULT_DEPTH_MASK !== 0, textured: false, texture: GFX_NO_TEXTURE,
		sampler: samplerIndex(GFX_DEFAULT_BILINEAR, GFX_DEFAULT_CLAMP, GFX_DEFAULT_CLAMP), fog: false, fogColor: 0,
	};
}

export class Renderer {
	// slots: slotViews() of both graphics slots; ctl: Int32Array control block;
	// settings: the live settings object (read every frame).
	constructor(canvas, slots, ctl, settings) {
		this.canvas = canvas;
		this.slots = slots;
		this.ctl = ctl;
		this.settings = settings;
		this.next = 0;
		this.state = defaultState();
		this.textures = new Map(); // id -> {size, data, texture}; data restores a lost context
		this.fogTable = new Uint8Array(GFX_FOG_ENTRIES);
		this.gamma = 1;
		this.frameOpen = false;
		this.needsPresent = false;
		this.lost = false;
		this.restored = false; // the context came back and is rebuilt by the next pump()
		this.canvasWidth = canvas.width;
		this.canvasHeight = canvas.height;
		this.gl = canvas.getContext('webgl2', {
			alpha: false, antialias: false, depth: false, stencil: false, premultipliedAlpha: false,
			preserveDrawingBuffer: false, powerPreference: 'high-performance',
		});
		if (!this.gl)
			return;
		canvas.addEventListener('webglcontextlost', (event) => {
			event.preventDefault();
			this.lost = true;
			this.restored = false;
		});
		canvas.addEventListener('webglcontextrestored', () => { this.restored = true; });
		this.initGL();
	}

	get available() {
		return !!this.gl;
	}

	keepAspect() {
		return Atomics.load(this.ctl, CTL_CFG_KEEP_ASPECT) !== 0 && this.settings.keepAspect;
	}

	// Canvas backing size in device pixels; applied when the next picture is drawn.
	resize(width, height) {
		this.canvasWidth = Math.max(1, Math.round(width));
		this.canvasHeight = Math.max(1, Math.round(height));
		this.invalidate();
	}

	// Redraws the last picture, e.g. after a settings change while the game waits.
	invalidate() {
		this.needsPresent = true;
	}

	// Executes published slots up to and including one finished frame; returns
	// whether a frame was presented. Throws when WebGL fails, also when a
	// restored context cannot be rebuilt.
	pump() {
		if (this.restored) {
			this.restored = false;
			this.initGL();
			this.lost = false;
		}
		let presented = false;
		for (;;) {
			const slot = this.slots[this.next];
			const state = Atomics.load(slot.i32, GFX_STATE);
			if (state !== GFX_PARTIAL && state !== GFX_FRAME)
				break;
			this.execute(slot, this.next);
			Atomics.store(slot.i32, GFX_STATE, GFX_FREE);
			Atomics.notify(slot.i32, GFX_STATE);
			this.next ^= 1;
			if (state === GFX_FRAME) {
				presented = !this.lost;
				break;
			}
		}
		if (!presented && this.needsPresent && this.scene && !this.frameOpen && !this.lost)
			this.present(false);
		return presented;
	}

	initGL() {
		const gl = this.gl;
		this.program = createProgram(gl, SCENE_VS, SCENE_FS);
		this.uniforms = uniformLocations(gl, this.program, ['u_texture', 'u_fogTable', 'u_textured', 'u_fog', 'u_fogColor']);
		gl.useProgram(this.program);
		gl.uniform1i(this.uniforms.u_texture, 0);
		gl.uniform1i(this.uniforms.u_fogTable, 1);

		this.buffers = [];
		this.vaos = [];
		for (let i = 0; i < 2; ++i) {
			const buffer = gl.createBuffer();
			gl.bindBuffer(gl.ARRAY_BUFFER, buffer);
			gl.bufferData(gl.ARRAY_BUFFER, GFX_VTX_CAPACITY * GFX_VTX_BYTES, gl.DYNAMIC_DRAW);
			const vao = gl.createVertexArray();
			gl.bindVertexArray(vao);
			for (const [location, size, type, normalized, offset] of [
				[0, 2, gl.FLOAT, false, 0], [1, 1, gl.FLOAT, false, GFX_VTX_OOW_OFFSET],
				[2, 2, gl.FLOAT, false, GFX_VTX_ST_OFFSET], [3, 4, gl.UNSIGNED_BYTE, true, GFX_VTX_COLOR_OFFSET],
			]) {
				gl.enableVertexAttribArray(location);
				gl.vertexAttribPointer(location, size, type, normalized, GFX_VTX_BYTES, offset);
			}
			this.buffers.push(buffer);
			this.vaos.push(vao);
		}
		gl.bindVertexArray(null);
		this.vaoIndex = 0;

		// Indexed by samplerIndex().
		const anisotropic = gl.getExtension('EXT_texture_filter_anisotropic');
		this.samplers = [];
		for (let i = 0; i < 8; ++i) {
			const sampler = gl.createSampler();
			gl.samplerParameteri(sampler, gl.TEXTURE_MIN_FILTER, i & 1 ? gl.LINEAR_MIPMAP_LINEAR : gl.NEAREST);
			gl.samplerParameteri(sampler, gl.TEXTURE_MAG_FILTER, i & 1 ? gl.LINEAR : gl.NEAREST);
			gl.samplerParameteri(sampler, gl.TEXTURE_WRAP_S, i & 2 ? gl.CLAMP_TO_EDGE : gl.REPEAT);
			gl.samplerParameteri(sampler, gl.TEXTURE_WRAP_T, i & 4 ? gl.CLAMP_TO_EDGE : gl.REPEAT);
			if (i & 1 && anisotropic) {
				const max = gl.getParameter(anisotropic.MAX_TEXTURE_MAX_ANISOTROPY_EXT);
				gl.samplerParameterf(sampler, anisotropic.TEXTURE_MAX_ANISOTROPY_EXT, Math.min(16, max));
			}
			this.samplers.push(sampler);
		}

		this.blackTexture = gl.createTexture();
		gl.bindTexture(gl.TEXTURE_2D, this.blackTexture);
		gl.texStorage2D(gl.TEXTURE_2D, 1, gl.RGBA8, 1, 1);
		gl.texSubImage2D(gl.TEXTURE_2D, 0, 0, 0, 1, 1, gl.RGBA, gl.UNSIGNED_BYTE, new Uint8Array([0, 0, 0, 255]));

		this.fogTexture = gl.createTexture();
		gl.bindTexture(gl.TEXTURE_2D, this.fogTexture);
		gl.texStorage2D(gl.TEXTURE_2D, 1, gl.R8, FOG_TEXTURE_WIDTH, GFX_FOG_ENTRIES / FOG_TEXTURE_WIDTH);
		gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_MIN_FILTER, gl.NEAREST);
		gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_MAG_FILTER, gl.NEAREST);
		this.uploadFogTable();
		for (const entry of this.textures.values()) {
			entry.texture = null;
			this.uploadTexture(entry);
		}

		this.depthFuncs = [gl.NEVER, gl.GREATER, gl.EQUAL, gl.GEQUAL, gl.LESS, gl.NOTEQUAL, gl.LEQUAL, gl.ALWAYS];
		this.blendFactors = [gl.ZERO, gl.ONE, gl.SRC_ALPHA, gl.ONE_MINUS_SRC_ALPHA];
		this.maxSamples = Math.min(gl.getParameter(gl.MAX_SAMPLES),
			...[gl.RGBA8, gl.DEPTH_COMPONENT32F].map((format) => Math.max(0, ...gl.getInternalformatParameter(gl.RENDERBUFFER, format, gl.SAMPLES))));
		this.maxSize = Math.min(gl.getParameter(gl.MAX_TEXTURE_SIZE), gl.getParameter(gl.MAX_RENDERBUFFER_SIZE));
		this.postfx = new PostFx(gl);
		this.scene = null;
		this.allowBilinear = true;
		this.dirty = DIRTY_ALL;
		this.needsPresent = true;
	}

	sceneConfig() {
		const viewport = fitViewport(this.canvasWidth, this.canvasHeight, this.keepAspect());
		let width = GAME_WIDTH, height = GAME_HEIGHT;
		if (this.settings.resolution !== 'original') {
			// Never below the original resolution unless asked: downscaling supersamples.
			let scale = Math.max(1, GAME_WIDTH / viewport.width, GAME_HEIGHT / viewport.height);
			if (this.settings.resolution === 'half')
				scale = 0.5;
			scale = Math.min(scale, MAX_SCENE_WIDTH / viewport.width, this.maxSize / viewport.width, this.maxSize / viewport.height);
			width = Math.max(1, Math.round(viewport.width * scale));
			height = Math.max(1, Math.round(viewport.height * scale));
		}
		const samples = this.settings.antialias && this.maxSamples >= MSAA_SAMPLES ? MSAA_SAMPLES : 0;
		return { width, height, samples };
	}

	// Called before the first slot of each frame: the scene target only changes
	// between frames so that partial frames land in the same target.
	beginFrame() {
		if (this.frameOpen && this.scene)
			return;
		this.frameOpen = true;
		const allowBilinear = Atomics.load(this.ctl, CTL_CFG_LINEAR_FILTER) !== 0 && this.settings.smoothTextures;
		if (allowBilinear !== this.allowBilinear) {
			this.allowBilinear = allowBilinear;
			this.dirty |= DIRTY_SAMPLER;
		}
		const want = this.sceneConfig(), scene = this.scene;
		if (scene && scene.width === want.width && scene.height === want.height && scene.samples === want.samples)
			return;
		this.deleteScene();
		this.scene = this.createScene(want) ?? this.createScene({ ...want, samples: 0 });
		this.dirty = DIRTY_ALL;
	}

	createScene({ width, height, samples }) {
		const gl = this.gl;
		const resolve = createTarget(gl, width, height);
		const scene = { width, height, samples, resolve, texture: resolve.texture, framebuffer: resolve.framebuffer, color: null };
		scene.depth = gl.createRenderbuffer();
		gl.bindRenderbuffer(gl.RENDERBUFFER, scene.depth);
		if (samples) {
			gl.renderbufferStorageMultisample(gl.RENDERBUFFER, samples, gl.DEPTH_COMPONENT32F, width, height);
			scene.color = gl.createRenderbuffer();
			gl.bindRenderbuffer(gl.RENDERBUFFER, scene.color);
			gl.renderbufferStorageMultisample(gl.RENDERBUFFER, samples, gl.RGBA8, width, height);
			scene.framebuffer = gl.createFramebuffer();
			gl.bindFramebuffer(gl.FRAMEBUFFER, scene.framebuffer);
			gl.framebufferRenderbuffer(gl.FRAMEBUFFER, gl.COLOR_ATTACHMENT0, gl.RENDERBUFFER, scene.color);
		} else {
			gl.renderbufferStorage(gl.RENDERBUFFER, gl.DEPTH_COMPONENT32F, width, height);
			gl.bindFramebuffer(gl.FRAMEBUFFER, scene.framebuffer);
		}
		gl.framebufferRenderbuffer(gl.FRAMEBUFFER, gl.DEPTH_ATTACHMENT, gl.RENDERBUFFER, scene.depth);
		if (gl.checkFramebufferStatus(gl.FRAMEBUFFER) !== gl.FRAMEBUFFER_COMPLETE && !gl.isContextLost()) {
			this.scene = scene;
			this.deleteScene();
			if (!samples)
				throw new Error('WebGL: the scene framebuffer is incomplete');
			return null;
		}
		gl.disable(gl.SCISSOR_TEST);
		gl.depthMask(true);
		gl.clearColor(0, 0, 0, 1);
		gl.clearDepth(0);
		gl.clear(gl.COLOR_BUFFER_BIT | gl.DEPTH_BUFFER_BIT);
		return scene;
	}

	deleteScene() {
		const gl = this.gl, scene = this.scene;
		if (!scene)
			return;
		if (scene.color) {
			gl.deleteRenderbuffer(scene.color);
			gl.deleteFramebuffer(scene.framebuffer);
		}
		gl.deleteRenderbuffer(scene.depth);
		deleteTarget(gl, scene.resolve);
		this.scene = null;
	}

	execute(slot, index) {
		const { i32, f32, u8 } = slot;
		const gl = this.gl, s = this.state, words = i32[GFX_CMD_WORDS], vertices = i32[GFX_VTX_COUNT];
		const live = !this.lost;
		if (live) {
			this.beginFrame();
			if (vertices > 0) {
				gl.bindBuffer(gl.ARRAY_BUFFER, this.buffers[index]);
				gl.bufferSubData(gl.ARRAY_BUFFER, 0, u8, GFX_VTX_OFFSET, vertices * GFX_VTX_BYTES);
			}
			this.vaoIndex = index;
			this.dirty |= DIRTY_VAO;
		}
		const base = GFX_CMD_OFFSET >> 2;
		for (let p = 0; p < words;) {
			const at = base + p, op = i32[at] & GFX_OP_MASK, size = i32[at] >>> GFX_OP_SIZE_SHIFT;
			if (!size || p + size > words)
				break;
			p += size;
			switch (op) {
			case GFX_OP_CLIP:
				s.clip = [i32[at + 1], i32[at + 2], i32[at + 3], i32[at + 4]];
				this.dirty |= DIRTY_SCISSOR;
				break;
			case GFX_OP_BLEND:
				s.blendSrc = i32[at + 1] & 3;
				s.blendDst = i32[at + 2] & 3;
				this.dirty |= DIRTY_BLEND;
				break;
			case GFX_OP_TEXENABLE:
				s.textured = i32[at + 1] !== 0;
				this.dirty |= DIRTY_UNIFORMS;
				break;
			case GFX_OP_FOGENABLE:
				s.fog = i32[at + 1] !== 0;
				this.dirty |= DIRTY_UNIFORMS;
				break;
			case GFX_OP_DEPTHMASK:
				s.depthMask = i32[at + 1] !== 0;
				this.dirty |= DIRTY_DEPTH;
				break;
			case GFX_OP_FOGCOLOR:
				s.fogColor = i32[at + 1];
				this.dirty |= DIRTY_UNIFORMS;
				break;
			case GFX_OP_FOGTABLE:
				this.fogTable.set(u8.subarray((at + 1) * 4, (at + 1) * 4 + GFX_FOG_ENTRIES));
				if (live)
					this.uploadFogTable();
				break;
			case GFX_OP_CLEAR:
				if (live)
					this.clear(i32[at + 1], i32[at + 2], i32[at + 3]);
				break;
			case GFX_OP_TEXIMAGE:
				this.textureImage(i32[at + 1], i32[at + 2], u8, (at + 3) * 4);
				break;
			case GFX_OP_TEXBIND:
				s.texture = i32[at + 1];
				this.dirty |= DIRTY_TEXTURE;
				break;
			case GFX_OP_GAMMA:
				this.gamma = f32[at + 1];
				break;
			case GFX_OP_TRIS:
				if (live)
					this.draw(i32[at + 1], i32[at + 2], vertices);
				break;
			case GFX_OP_SWAP:
				if (live)
					this.present(true);
				this.frameOpen = false;
				break;
			case GFX_OP_DEPTHFUNC:
				s.depthFunc = i32[at + 1] & 7;
				this.dirty |= DIRTY_DEPTH;
				break;
			case GFX_OP_SAMPLER:
				s.sampler = samplerIndex(i32[at + 1], i32[at + 2], i32[at + 3]);
				this.dirty |= DIRTY_SAMPLER;
				break;
			}
		}
	}

	textureImage(id, size, u8, offset) {
		if (!(size > 0 && size <= GFX_TEX_MAX_SIZE && !(size & (size - 1))))
			return;
		const old = this.textures.get(id);
		const entry = { size, data: u8.slice(offset, offset + size * size * 4), texture: null };
		if (old?.size === size)
			entry.texture = old.texture;
		else if (old?.texture && !this.lost)
			this.gl.deleteTexture(old.texture);
		this.textures.set(id, entry);
		if (!this.lost)
			this.uploadTexture(entry);
	}

	// Uploads on texture unit 0, so the game texture is rebound before drawing.
	uploadTexture(entry) {
		const gl = this.gl;
		if (!entry.texture) {
			entry.texture = gl.createTexture();
			gl.bindTexture(gl.TEXTURE_2D, entry.texture);
			gl.texStorage2D(gl.TEXTURE_2D, Math.log2(entry.size) + 1, gl.RGBA8, entry.size, entry.size);
		} else {
			gl.bindTexture(gl.TEXTURE_2D, entry.texture);
		}
		gl.texSubImage2D(gl.TEXTURE_2D, 0, 0, 0, entry.size, entry.size, gl.RGBA, gl.UNSIGNED_BYTE, entry.data);
		gl.generateMipmap(gl.TEXTURE_2D);
		this.dirty |= DIRTY_TEXTURE;
	}

	uploadFogTable() {
		const gl = this.gl;
		gl.bindTexture(gl.TEXTURE_2D, this.fogTexture);
		gl.texSubImage2D(gl.TEXTURE_2D, 0, 0, 0, FOG_TEXTURE_WIDTH, GFX_FOG_ENTRIES / FOG_TEXTURE_WIDTH, gl.RED, gl.UNSIGNED_BYTE, this.fogTable);
		this.dirty |= DIRTY_TEXTURE;
	}

	// Applies the state that changed since the last draw or clear.
	flush() {
		const gl = this.gl, s = this.state, scene = this.scene, dirty = this.dirty;
		if (dirty & DIRTY_TARGET) {
			gl.bindFramebuffer(gl.FRAMEBUFFER, scene.framebuffer);
			gl.viewport(0, 0, scene.width, scene.height);
			gl.useProgram(this.program);
			gl.enable(gl.SCISSOR_TEST);
			gl.enable(gl.DEPTH_TEST);
			gl.activeTexture(gl.TEXTURE1);
			gl.bindTexture(gl.TEXTURE_2D, this.fogTexture);
			gl.bindSampler(1, null);
			gl.activeTexture(gl.TEXTURE0);
		}
		if (dirty & DIRTY_VAO)
			gl.bindVertexArray(this.vaos[this.vaoIndex]);
		if (dirty & DIRTY_SCISSOR) {
			const sx = scene.width / GAME_WIDTH, sy = scene.height / GAME_HEIGHT, clamp = (v, lo, hi) => Math.min(hi, Math.max(lo, v));
			const x0 = clamp(Math.round(s.clip[0] * sx), 0, scene.width), x1 = clamp(Math.round(s.clip[2] * sx), x0, scene.width);
			const y0 = clamp(Math.round(s.clip[1] * sy), 0, scene.height), y1 = clamp(Math.round(s.clip[3] * sy), y0, scene.height);
			gl.scissor(x0, scene.height - y1, x1 - x0, y1 - y0);
		}
		if (dirty & DIRTY_BLEND) {
			if (s.blendSrc === BLEND_ONE && s.blendDst === BLEND_ZERO) {
				gl.disable(gl.BLEND);
			} else {
				gl.enable(gl.BLEND);
				gl.blendFunc(this.blendFactors[s.blendSrc], this.blendFactors[s.blendDst]);
			}
		}
		if (dirty & DIRTY_DEPTH) {
			gl.depthFunc(this.depthFuncs[s.depthFunc]);
			gl.depthMask(s.depthMask);
		}
		if (dirty & DIRTY_TEXTURE)
			gl.bindTexture(gl.TEXTURE_2D, this.textures.get(s.texture)?.texture ?? this.blackTexture);
		if (dirty & DIRTY_SAMPLER)
			gl.bindSampler(0, this.samplers[this.allowBilinear ? s.sampler : s.sampler & 6]);
		if (dirty & DIRTY_UNIFORMS) {
			gl.uniform1i(this.uniforms.u_textured, s.textured ? 1 : 0);
			gl.uniform1i(this.uniforms.u_fog, s.fog ? 1 : 0);
			gl.uniform3f(this.uniforms.u_fogColor, ...rgb(s.fogColor));
		}
		this.dirty = 0;
	}

	clear(color, alpha, depth) {
		const gl = this.gl;
		this.flush();
		gl.clearColor(...rgb(color), (alpha & 255) / 255);
		gl.clearDepth(clearDepth(depth));
		gl.clear(gl.COLOR_BUFFER_BIT | gl.DEPTH_BUFFER_BIT); // the depth mask still applies
	}

	draw(first, count, vertices) {
		if (count <= 0 || first < 0 || first + count > vertices)
			return;
		this.flush();
		this.gl.drawArrays(this.gl.TRIANGLES, first, count);
	}

	// Resolves the scene (unless redrawing the last picture) and draws it to the canvas.
	present(resolve) {
		const gl = this.gl, scene = this.scene, settings = this.settings;
		if (resolve && scene.samples) {
			gl.disable(gl.SCISSOR_TEST); // blits are scissored
			gl.bindFramebuffer(gl.READ_FRAMEBUFFER, scene.framebuffer);
			gl.bindFramebuffer(gl.DRAW_FRAMEBUFFER, scene.resolve.framebuffer);
			gl.blitFramebuffer(0, 0, scene.width, scene.height, 0, 0, scene.width, scene.height, gl.COLOR_BUFFER_BIT, gl.NEAREST);
		}
		if (this.canvas.width !== this.canvasWidth || this.canvas.height !== this.canvasHeight) {
			this.canvas.width = this.canvasWidth;
			this.canvas.height = this.canvasHeight;
		}
		const viewport = fitViewport(this.canvas.width, this.canvas.height, this.keepAspect());
		this.postfx.render(scene, viewport, this.canvas.width, this.canvas.height, {
			gamma: this.gamma,
			bloom: settings.bloom,
			sharpen: settings.sharpen,
			grade: settings.enhancedColors,
			antialias: settings.antialias && !scene.samples,
		});
		this.dirty = DIRTY_ALL;
		this.needsPresent = false;
	}
}

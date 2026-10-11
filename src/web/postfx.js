// Post-processing from the rendered scene to the canvas: optional bloom, then
// one pass that applies edge anti-aliasing or sharpening, the game's gamma,
// colour enhancement and the letterboxed placement.
import { createProgram, createTarget, deleteTarget, FULLSCREEN_VS, uniformLocations } from './glutil.js';

const BRIGHT_FS = `#version 300 es
precision highp float;
uniform sampler2D u_source;
uniform vec2 u_texel;
in vec2 v_uv;
out vec4 o_color;
void main() {
	// Four bilinear taps average a 4x4 block of the source.
	vec3 c = 0.25 * (texture(u_source, v_uv + vec2(-u_texel.x, -u_texel.y)).rgb +
		texture(u_source, v_uv + vec2(u_texel.x, -u_texel.y)).rgb +
		texture(u_source, v_uv + vec2(-u_texel.x, u_texel.y)).rgb +
		texture(u_source, v_uv + vec2(u_texel.x, u_texel.y)).rgb);
	float peak = max(c.r, max(c.g, c.b));
	o_color = vec4(c * max(peak - 0.6, 0.0) / max(peak, 1e-4), 1.0);
}`;

// Separable Gaussian: bilinear taps between texels halve the fetches.
const BLUR_FS = `#version 300 es
precision highp float;
uniform sampler2D u_source;
uniform vec2 u_step;
in vec2 v_uv;
out vec4 o_color;
void main() {
	vec3 c = texture(u_source, v_uv).rgb * 0.2270270;
	c += (texture(u_source, v_uv + u_step * 1.3846154).rgb + texture(u_source, v_uv - u_step * 1.3846154).rgb) * 0.3162162;
	c += (texture(u_source, v_uv + u_step * 3.2307692).rgb + texture(u_source, v_uv - u_step * 3.2307692).rgb) * 0.0702703;
	o_color = vec4(c, 1.0);
}`;

const FINAL_FS = `#version 300 es
precision highp float;
uniform sampler2D u_scene;
uniform sampler2D u_bloom;
uniform vec2 u_texel;
uniform float u_gamma;
uniform float u_bloomStrength;
uniform float u_sharpen;
uniform float u_grade;
uniform bool u_antialias;
in vec2 v_uv;
out vec4 o_color;

float luma(vec3 c) {
	return dot(c, vec3(0.299, 0.587, 0.114));
}

vec3 tap(vec2 uv, float dx, float dy) {
	return texture(u_scene, uv + vec2(dx, dy) * u_texel).rgb;
}

// Blurs along the local edge, which runs perpendicular to the luma gradient
// of the diagonal neighbours; used when multisampling is unavailable.
vec3 antialias(vec2 uv) {
	vec3 centre = tap(uv, 0.0, 0.0);
	float a = luma(tap(uv, -1.0, 1.0)), b = luma(tap(uv, 1.0, 1.0));
	float c = luma(tap(uv, -1.0, -1.0)), d = luma(tap(uv, 1.0, -1.0));
	float m = luma(centre);
	float lo = min(m, min(min(a, b), min(c, d))), hi = max(m, max(max(a, b), max(c, d)));
	if (hi - lo < max(1.0 / 24.0, hi / 8.0))
		return centre;
	vec2 edge = vec2(c + d - a - b, a + c - b - d);
	float bias = max((a + b + c + d) / 32.0, 1.0 / 128.0);
	edge = clamp(edge / (min(abs(edge.x), abs(edge.y)) + bias), -8.0, 8.0) * u_texel;
	vec3 near = 0.5 * (texture(u_scene, uv - edge / 6.0).rgb + texture(u_scene, uv + edge / 6.0).rgb);
	vec3 far = 0.5 * near + 0.25 * (texture(u_scene, uv - edge * 0.5).rgb + texture(u_scene, uv + edge * 0.5).rgb);
	float l = luma(far);
	return l < lo || l > hi ? near : far;
}

// Contrast-adaptive sharpening: strongest where the neighbourhood is flat.
vec3 sharpen(vec2 uv) {
	vec3 c = tap(uv, 0.0, 0.0);
	vec3 n = tap(uv, 0.0, 1.0), s = tap(uv, 0.0, -1.0), e = tap(uv, 1.0, 0.0), w = tap(uv, -1.0, 0.0);
	vec3 lo = min(c, min(min(n, s), min(e, w))), hi = max(c, max(max(n, s), max(e, w)));
	vec3 amount = sqrt(clamp(min(lo, 1.0 - hi) / max(hi, vec3(1e-4)), 0.0, 1.0));
	vec3 weight = -0.2 * u_sharpen * amount;
	return clamp((c + (n + s + e + w) * weight) / (1.0 + 4.0 * weight), 0.0, 1.0);
}

vec3 enhance(vec3 c) {
	vec3 saturated = clamp(mix(vec3(luma(c)), c, 1.15), 0.0, 1.0);
	vec3 curved = saturated * saturated * (3.0 - 2.0 * saturated);
	return mix(c, mix(saturated, curved, 0.25), u_grade);
}

void main() {
	vec3 c = u_antialias ? antialias(v_uv) : u_sharpen > 0.0 ? sharpen(v_uv) : tap(v_uv, 0.0, 0.0);
	if (u_bloomStrength > 0.0)
		c += texture(u_bloom, v_uv).rgb * u_bloomStrength;
	c = pow(clamp(c, 0.0, 1.0), vec3(1.0 / u_gamma));
	o_color = vec4(enhance(c), 1.0);
}`;

const BLOOM_DIVISOR = 4;
const BLOOM_STRENGTH = 0.3; // share of the blurred highlights added to the picture

export class PostFx {
	constructor(gl) {
		this.gl = gl;
		this.vao = gl.createVertexArray();
		this.bright = createProgram(gl, FULLSCREEN_VS, BRIGHT_FS);
		this.brightUniforms = uniformLocations(gl, this.bright, ['u_source', 'u_texel']);
		this.blur = createProgram(gl, FULLSCREEN_VS, BLUR_FS);
		this.blurUniforms = uniformLocations(gl, this.blur, ['u_source', 'u_step']);
		this.final = createProgram(gl, FULLSCREEN_VS, FINAL_FS);
		this.finalUniforms = uniformLocations(gl, this.final, [
			'u_scene', 'u_bloom', 'u_texel', 'u_gamma', 'u_bloomStrength', 'u_sharpen', 'u_grade', 'u_antialias',
		]);
		this.bloom = [null, null];
	}

	ensureBloomTargets(width, height) {
		const w = Math.max(1, Math.floor(width / BLOOM_DIVISOR)), h = Math.max(1, Math.floor(height / BLOOM_DIVISOR));
		if (this.bloom[0]?.width === w && this.bloom[0]?.height === h)
			return;
		for (const target of this.bloom)
			deleteTarget(this.gl, target);
		this.bloom = [createTarget(this.gl, w, h), createTarget(this.gl, w, h)];
	}

	pass(program, target, width, height) {
		const gl = this.gl;
		gl.bindFramebuffer(gl.FRAMEBUFFER, target ? target.framebuffer : null);
		gl.viewport(0, 0, width, height);
		gl.useProgram(program);
		gl.drawArrays(gl.TRIANGLES, 0, 3);
	}

	// scene: {texture, width, height}; viewport: placement on the canvas.
	// options: gamma, and the switches bloom, sharpen, grade (colour
	// enhancement) and antialias.
	render(scene, viewport, canvasWidth, canvasHeight, options) {
		const gl = this.gl;
		gl.disable(gl.SCISSOR_TEST);
		gl.disable(gl.DEPTH_TEST);
		gl.disable(gl.BLEND);
		gl.bindVertexArray(this.vao);
		gl.bindSampler(0, null);
		gl.bindSampler(1, null);

		if (options.bloom) {
			this.ensureBloomTargets(scene.width, scene.height);
			const [a, b] = this.bloom;
			gl.activeTexture(gl.TEXTURE0);
			gl.bindTexture(gl.TEXTURE_2D, scene.texture);
			gl.useProgram(this.bright);
			gl.uniform1i(this.brightUniforms.u_source, 0);
			gl.uniform2f(this.brightUniforms.u_texel, 1 / scene.width, 1 / scene.height);
			this.pass(this.bright, a, a.width, a.height);
			gl.useProgram(this.blur);
			gl.uniform1i(this.blurUniforms.u_source, 0);
			gl.bindTexture(gl.TEXTURE_2D, a.texture);
			gl.uniform2f(this.blurUniforms.u_step, 1 / a.width, 0);
			this.pass(this.blur, b, b.width, b.height);
			gl.bindTexture(gl.TEXTURE_2D, b.texture);
			gl.uniform2f(this.blurUniforms.u_step, 0, 1 / b.height);
			this.pass(this.blur, a, a.width, a.height);
		}

		gl.bindFramebuffer(gl.FRAMEBUFFER, null);
		gl.viewport(0, 0, canvasWidth, canvasHeight);
		gl.clearColor(0, 0, 0, 1);
		gl.clear(gl.COLOR_BUFFER_BIT);

		gl.activeTexture(gl.TEXTURE0);
		gl.bindTexture(gl.TEXTURE_2D, scene.texture);
		gl.activeTexture(gl.TEXTURE1);
		gl.bindTexture(gl.TEXTURE_2D, options.bloom ? this.bloom[0].texture : scene.texture);
		gl.activeTexture(gl.TEXTURE0);
		gl.useProgram(this.final);
		const u = this.finalUniforms;
		gl.uniform1i(u.u_scene, 0);
		gl.uniform1i(u.u_bloom, 1);
		gl.uniform2f(u.u_texel, 1 / scene.width, 1 / scene.height);
		gl.uniform1f(u.u_gamma, options.gamma > 0 ? options.gamma : 1);
		gl.uniform1f(u.u_bloomStrength, options.bloom ? BLOOM_STRENGTH : 0);
		gl.uniform1f(u.u_sharpen, options.sharpen ? 1 : 0);
		gl.uniform1f(u.u_grade, options.grade ? 1 : 0);
		gl.uniform1i(u.u_antialias, options.antialias ? 1 : 0);
		gl.viewport(viewport.x, viewport.y, viewport.width, viewport.height);
		gl.drawArrays(gl.TRIANGLES, 0, 3);
	}
}

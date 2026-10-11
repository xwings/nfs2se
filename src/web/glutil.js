// Small WebGL2 helpers shared by renderer.js and postfx.js.

export function createProgram(gl, vertexSource, fragmentSource) {
	const program = gl.createProgram();
	const shaders = [[gl.VERTEX_SHADER, vertexSource], [gl.FRAGMENT_SHADER, fragmentSource]].map(([type, source]) => {
		const shader = gl.createShader(type);
		gl.shaderSource(shader, source);
		gl.compileShader(shader);
		gl.attachShader(program, shader);
		return shader;
	});
	gl.linkProgram(program);
	// Queried only after linking so that compilation can run in parallel.
	if (!gl.getProgramParameter(program, gl.LINK_STATUS) && !gl.isContextLost()) {
		const logs = shaders.map((shader) => gl.getShaderInfoLog(shader)).filter(Boolean);
		throw new Error(`WebGL program failed: ${[...logs, gl.getProgramInfoLog(program)].join('\n')}`);
	}
	for (const shader of shaders)
		gl.deleteShader(shader);
	return program;
}

export function uniformLocations(gl, program, names) {
	const locations = {};
	for (const name of names)
		locations[name] = gl.getUniformLocation(program, name);
	return locations;
}

// Vertex shader for a triangle that covers the viewport; draw 3 vertices.
export const FULLSCREEN_VS = `#version 300 es
out vec2 v_uv;
void main() {
	vec2 p = vec2(float((gl_VertexID << 1) & 2), float(gl_VertexID & 2));
	v_uv = p;
	gl_Position = vec4(p * 2.0 - 1.0, 0.0, 1.0);
}`;

export function createTarget(gl, width, height) {
	const texture = gl.createTexture();
	gl.bindTexture(gl.TEXTURE_2D, texture);
	gl.texStorage2D(gl.TEXTURE_2D, 1, gl.RGBA8, width, height);
	gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_MIN_FILTER, gl.LINEAR);
	gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_MAG_FILTER, gl.LINEAR);
	gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_WRAP_S, gl.CLAMP_TO_EDGE);
	gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_WRAP_T, gl.CLAMP_TO_EDGE);
	const framebuffer = gl.createFramebuffer();
	gl.bindFramebuffer(gl.FRAMEBUFFER, framebuffer);
	gl.framebufferTexture2D(gl.FRAMEBUFFER, gl.COLOR_ATTACHMENT0, gl.TEXTURE_2D, texture, 0);
	return { texture, framebuffer, width, height };
}

export function deleteTarget(gl, target) {
	if (!target)
		return;
	gl.deleteTexture(target.texture);
	gl.deleteFramebuffer(target.framebuffer);
}

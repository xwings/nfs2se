// The WebGL2 replay of recorded graphics slots (src/web/renderer.js), fed
// hand-built command streams on a blank isolated page. Pixels are read back
// right after each pump, before the browser composites the canvas.
import { expect } from '@playwright/test';
import { BLEND_INV_SRC_ALPHA, BLEND_ONE, BLEND_SRC_ALPHA, BLEND_ZERO, GFX_FREE } from '../../src/web/layout.js';
import { DEFAULT_SETTINGS } from '../../src/web/settings.js';
import { openBlankPage, test } from './helpers.mjs';

// The final pass is then a copy: canvas pixel (x, y) is game pixel (x, y).
const PLAIN = { ...DEFAULT_SETTINGS, resolution: 'original', antialias: false, bloom: false, sharpen: false, enhancedColors: false, keepAspect: false };

const RED = [255, 0, 0, 255], GREEN = [0, 255, 0, 255], BLUE = [0, 0, 255, 255], WHITE = [255, 255, 255, 255];
const YELLOW = [255, 255, 0, 255], MAGENTA = [255, 0, 255, 255], BLACK = [0, 0, 0, 255];
// 2x2 premultiplied texture: red, green / blue, transparent.
const TEXELS = [...RED, ...GREEN, ...BLUE, 0, 0, 0, 0];
const LEQUAL = 3, ALWAYS = 7; // Glide depth functions
const FAR = 0xFFFF; // W-buffer clear value: 1/w = 0

// Two triangles over x0..x1, y0..y1 (game units) in one colour, with 1/w
// going from oow at the left edge to oowRight at the right edge.
function quad(x0, y0, x1, y1, color, { oow = 1, oowRight = oow, st = [0, 0, 1, 1] } = {}) {
	const [s0, t0, s1, t1] = st;
	const a = [x0, y0, oow, s0, t0, ...color], b = [x1, y0, oowRight, s1, t0, ...color];
	const c = [x1, y1, oowRight, s1, t1, ...color], d = [x0, y1, oow, s0, t1, ...color];
	return ['tris', [a, b, c, a, c, d]];
}

// Runs in the page. Every frame publishes its slots (the last one as a
// finished frame), pumps once and reads the probed canvas pixels (from the
// top left). Commands are [name, ...words] after GFX_OP_<NAME>, except:
// tris [[x, y, oow, s, t, r, g, b, a], ...] appends vertices (s and t are
// stored divided by w, as glide.c does), teximage id, size, [RGBA bytes],
// fogtable [[from, to, value], ...] over a zero table, and gamma (a float).
// loseContext loses the context before the frame and restores it after;
// brokenRestore then makes rebuilding fail. A pump that throws gives pumpError.
async function replay({ settings, canvas: [width, height] = [640, 480], ctl: ctlValues = {}, frames }) {
	const L = await import('/layout.js');
	const { Renderer, slotViews } = await import('/renderer.js');
	const buffer = new SharedArrayBuffer(L.CTL_WORDS * 4 + 2 * L.GFX_SLOT_BYTES);
	const ctl = new Int32Array(buffer, 0, L.CTL_WORDS);
	const slots = [0, 1].map((i) => slotViews(buffer, L.CTL_WORDS * 4 + i * L.GFX_SLOT_BYTES));
	const setCtl = (values) => Object.entries(values).forEach(([name, value]) => { ctl[L[name]] = value; });
	setCtl({ CTL_CFG_KEEP_ASPECT: 1, CTL_CFG_LINEAR_FILTER: 1, ...ctlValues });
	const canvas = document.body.appendChild(document.createElement('canvas'));
	canvas.width = width;
	canvas.height = height;
	const renderer = new Renderer(canvas, slots, ctl, { ...settings });
	const gl = renderer.gl, loseContext = gl.getExtension('WEBGL_lose_context');
	// Resolves in a later task: the browser only allows restoring a lost
	// context once the dispatch of webglcontextlost has finished.
	const event = (type) => new Promise((resolve) => canvas.addEventListener(type, () => setTimeout(resolve), { once: true }));

	function record(slot, commands) {
		const { i32, f32, u8 } = slot, start = L.GFX_CMD_OFFSET >> 2;
		let p = start, vertices = 0;
		for (const [name, ...args] of commands) {
			let words = 1 + args.length;
			if (name === 'tris') {
				i32.set([vertices, args[0].length], p + 1);
				for (const [x, y, oow, s, t, ...rgba] of args[0]) {
					const at = L.GFX_VTX_OFFSET + vertices++ * L.GFX_VTX_BYTES;
					f32.set([x, y], at >> 2);
					f32[(at + L.GFX_VTX_OOW_OFFSET) >> 2] = oow;
					f32.set([s * oow, t * oow], (at + L.GFX_VTX_ST_OFFSET) >> 2);
					u8.set(rgba, at + L.GFX_VTX_COLOR_OFFSET);
				}
				words = 3;
			} else if (name === 'teximage') {
				const [id, size, texels] = args;
				i32.set([id, size], p + 1);
				u8.set(texels, (p + 3) * 4);
				words = 3 + size * size;
			} else if (name === 'fogtable') {
				const at = (p + 1) * 4;
				u8.fill(0, at, at + L.GFX_FOG_ENTRIES);
				for (const [from, to, value] of args[0])
					u8.fill(value, at + from, at + to);
				words = 1 + L.GFX_FOG_ENTRIES / 4;
			} else if (name === 'gamma') {
				f32[p + 1] = args[0];
			} else {
				i32.set(args, p + 1);
			}
			i32[p] = L[`GFX_OP_${name.toUpperCase()}`] | words << L.GFX_OP_SIZE_SHIFT;
			p += words;
		}
		i32[L.GFX_CMD_WORDS] = p - start;
		i32[L.GFX_VTX_COUNT] = vertices;
	}

	const results = [];
	let next = 0;
	for (const frame of frames) {
		Object.assign(renderer.settings, frame.settings);
		setCtl(frame.ctl ?? {});
		if (frame.resize)
			renderer.resize(...frame.resize);
		if (frame.loseContext) {
			const lost = event('webglcontextlost');
			loseContext.loseContext();
			await lost;
		}
		frame.slots.forEach((commands, i) => {
			const slot = slots[next];
			next ^= 1;
			record(slot, commands);
			Atomics.store(slot.i32, L.GFX_STATE, i === frame.slots.length - 1 ? L.GFX_FRAME : L.GFX_PARTIAL);
		});
		let presented = false, pumpError = null;
		try {
			presented = renderer.pump();
		} catch (error) {
			pumpError = error.message;
		}
		const pixel = new Uint8Array(4);
		gl.bindFramebuffer(gl.READ_FRAMEBUFFER, null);
		const colors = (frame.probes ?? []).map(([x, y]) => {
			gl.readPixels(x, canvas.height - 1 - y, 1, 1, gl.RGBA, gl.UNSIGNED_BYTE, pixel);
			return [...pixel];
		});
		const scene = renderer.scene;
		results.push({
			presented, pumpError, colors, error: gl.getError(), states: slots.map((slot) => slot.i32[L.GFX_STATE]),
			canvas: [canvas.width, canvas.height], scene: scene && { width: scene.width, height: scene.height, samples: scene.samples },
		});
		if (frame.loseContext) {
			if (frame.brokenRestore)
				gl.createProgram = () => { throw new Error('createProgram failed'); };
			const restored = event('webglcontextrestored');
			loseContext.restoreContext();
			await restored;
		}
	}
	return results;
}

async function run(page, options) {
	await openBlankPage(page);
	const results = await page.evaluate(replay, { settings: PLAIN, ...options });
	results.forEach((result, i) => {
		if (!options.frames[i].loseContext)
			expect(result.error, `WebGL error after frame ${i}`).toBe(0);
	});
	return results;
}

// The frame was presented and its probes match the RGB values within tolerance.
function expectColors(result, expected, tolerance = 2) {
	expect(result.presented, 'frame presented').toBe(true);
	expect(result.colors).toHaveLength(expected.length);
	expected.forEach((color, i) => {
		const actual = result.colors[i].slice(0, 3);
		const off = Math.max(...actual.map((value, c) => Math.abs(value - color[c])));
		expect(off, `probe ${i}: got [${actual}], want [${color.slice(0, 3)}]`).toBeLessThanOrEqual(tolerance);
	});
}

test('clears and flat triangles land inside the clip rectangle', async ({ page }) => {
	const background = [0x20, 0x40, 0x80];
	const [first, second] = await run(page, {
		frames: [{
			slots: [[['clear', 0x204080, 255, FAR], quad(100, 100, 300, 200, RED), ['swap']]],
			probes: [[50, 50], [110, 110], [290, 190], [310, 150], [200, 210]],
		}, {
			slots: [[
				['clip', 0, 0, 320, 240], ['clear', 0x00FF00, 255, FAR],
				['clip', 320, 240, 640, 480], quad(0, 0, 640, 480, BLUE),
				['clip', 0, 0, 640, 480], ['swap'],
			]],
			probes: [[100, 100], [500, 100], [100, 400], [500, 400]],
		}],
	});
	expectColors(first, [background, RED, RED, background, background]);
	expectColors(second, [GREEN, background, background, BLUE]);
});

test('the depth buffer holds 1/w: nearer wins, the depth function and mask apply', async ({ page }) => {
	const [layered, nearClear] = await run(page, {
		frames: [{
			slots: [[
				['clear', 0x000000, 255, FAR],
				quad(0, 0, 320, 480, GREEN, { oow: 0.8 }),
				quad(0, 0, 640, 480, BLUE, { oow: 0.2 }),
				['depthfunc', ALWAYS], quad(0, 0, 160, 480, WHITE, { oow: 0.1 }),
				['depthfunc', LEQUAL], ['depthmask', 0], quad(160, 0, 320, 480, YELLOW, { oow: 0.9 }),
				// Passes only because the yellow quad left the depth at 0.8.
				['depthmask', 1], quad(160, 0, 320, 480, MAGENTA, { oow: 0.85 }),
				['swap'],
			]],
			probes: [[80, 240], [240, 240], [480, 240]],
		}, {
			// Cleared to the nearest depth (1/w = 1), nothing farther is drawn.
			slots: [[['clear', 0x000000, 255, 0], quad(0, 0, 640, 480, RED, { oow: 0.5 }), ['swap']]],
			probes: [[320, 240]],
		}],
	});
	expectColors(layered, [WHITE, MAGENTA, BLUE]);
	expectColors(nearClear, [BLACK]);
});

test('blending uses the game factors and the alpha test drops faint fragments', async ({ page }) => {
	const [frame] = await run(page, {
		frames: [{
			slots: [[
				['clear', 0x000000, 255, FAR],
				['blend', BLEND_SRC_ALPHA, BLEND_INV_SRC_ALPHA], quad(0, 0, 320, 480, [255, 255, 255, 128]),
				['blend', BLEND_ONE, BLEND_ONE], quad(0, 0, 160, 480, [64, 0, 0, 255]),
				['blend', BLEND_ONE, BLEND_ZERO], quad(320, 0, 480, 480, [255, 0, 0, 8]), quad(480, 0, 640, 480, [255, 0, 0, 24]),
				['swap'],
			]],
			probes: [[80, 240], [240, 240], [400, 240], [560, 240]],
		}],
	});
	expectColors(frame, [[192, 128, 128], [128, 128, 128], BLACK, RED]);
});

test('textures are sampled perspective-correct, modulate the colour and cut out', async ({ page }) => {
	const grey = [0x40, 0x40, 0x40];
	const [plain, perspective] = await run(page, {
		frames: [{
			slots: [[
				['clear', 0x404040, 255, FAR],
				['teximage', 5, 2, TEXELS], ['texbind', 5], ['texenable', 1], ['sampler', 0, 1, 1],
				quad(0, 0, 640, 480, WHITE),
				quad(0, 0, 80, 60, [128, 128, 128, 255], { st: [0, 0, 0.25, 0.25] }),
				['swap'],
			]],
			probes: [[160, 120], [480, 120], [160, 360], [480, 360], [40, 30]],
		}, {
			// 1/w falls to a quarter across the screen, which moves the texel
			// edge at s = 0.5 to 80 % of the width (an affine mapping: 50 %).
			slots: [[['clear', 0x404040, 255, FAR], quad(0, 0, 640, 480, WHITE, { oow: 1, oowRight: 0.25 }), ['swap']]],
			probes: [[448, 120], [576, 120]],
		}],
	});
	expectColors(plain, [RED, GREEN, BLUE, grey, [128, 0, 0]]);
	expectColors(perspective, [RED, GREEN]);
});

test('filtering and wrapping follow the game, the setting and the game config', async ({ page }) => {
	const draw = (sampler) => [['clear', 0x000000, 255, FAR], ['sampler', ...sampler], quad(0, 0, 640, 480, WHITE), ['swap']];
	const [clamped, wrapped, unsmoothed, configured] = await run(page, {
		frames: [
			{ slots: [[['teximage', 5, 2, TEXELS], ['texbind', 5], ['texenable', 1], ...draw([1, 1, 1])]], probes: [[240, 120], [32, 120]] },
			{ slots: [draw([1, 0, 1])], probes: [[32, 120]] },
			{ slots: [draw([1, 1, 1])], probes: [[240, 120]], settings: { smoothTextures: false } },
			{ slots: [draw([1, 1, 1])], probes: [[240, 120]], settings: { smoothTextures: true }, ctl: { CTL_CFG_LINEAR_FILTER: 0 } },
		],
	});
	// A quarter of the way from the red to the green texel; red at the clamped edge.
	expectColors(clamped, [[191, 64, 0], RED], 3);
	// Repeating, the left edge blends with the green texel.
	expectColors(wrapped, [[153, 102, 0]], 3);
	expectColors(unsmoothed, [RED]);
	expectColors(configured, [RED]);
});

test('fog mixes in the fog colour by the table entry at w = 1/oow', async ({ page }) => {
	const [fogged, plain] = await run(page, {
		frames: [{
			slots: [[
				['clear', 0x000000, 255, FAR],
				['fogtable', [[300, 3000, 128], [3000, 65536, 255]]], ['fogcolor', 0x00FF00], ['fogenable', 1],
				quad(0, 0, 213, 480, RED, { oow: 1 / 100 }),
				quad(213, 0, 426, 480, RED, { oow: 1 / 1000 }),
				quad(426, 0, 640, 480, RED, { oow: 1 / 10000 }),
				['swap'],
			]],
			probes: [[100, 240], [320, 240], [540, 240]],
		}, {
			slots: [[['clear', 0x000000, 255, FAR], ['fogenable', 0], quad(0, 0, 640, 480, RED, { oow: 1 / 10000 }), ['swap']]],
			probes: [[540, 240]],
		}],
	});
	expectColors(fogged, [RED, [127, 128, 0], GREEN]);
	expectColors(plain, [RED]);
});

test('a frame recorded in two slots is presented once and gamma applies to it', async ({ page }) => {
	const [split, idle, corrected] = await run(page, {
		frames: [{
			slots: [
				[['clear', 0x404040, 255, FAR], quad(0, 0, 320, 480, WHITE)],
				[quad(320, 0, 640, 480, RED), ['swap']],
			],
			probes: [[160, 240], [480, 240]],
		}, {
			slots: [],
		}, {
			slots: [[['gamma', 2], ['clear', 0x404040, 255, FAR], ['swap']]],
			probes: [[320, 240]],
		}],
	});
	expectColors(split, [WHITE, RED]);
	expect(split.states).toEqual([GFX_FREE, GFX_FREE]);
	expect(idle.presented).toBe(false);
	expectColors(corrected, [[128, 128, 128]]); // (64/255)^(1/2)
});

test('the picture keeps 4:3 when the game and the setting ask and follows resizes', async ({ page }) => {
	const white = { slots: [[['clear', 0xFFFFFF, 255, FAR], ['swap']]] };
	const [boxed, stretched, gameStretched, resized] = await run(page, {
		canvas: [800, 480],
		settings: { ...PLAIN, keepAspect: true },
		frames: [
			{ ...white, probes: [[40, 240], [400, 240], [760, 240]] },
			{ ...white, probes: [[40, 240]], settings: { keepAspect: false } },
			{ ...white, probes: [[40, 240]], settings: { keepAspect: true }, ctl: { CTL_CFG_KEEP_ASPECT: 0 } },
			{ ...white, probes: [[200, 50], [200, 300], [200, 550]], ctl: { CTL_CFG_KEEP_ASPECT: 1 }, resize: [400, 600] },
		],
	});
	expectColors(boxed, [BLACK, WHITE, BLACK]);
	expectColors(stretched, [WHITE]);
	expectColors(gameStretched, [WHITE]);
	expect(resized.canvas).toEqual([400, 600]);
	expectColors(resized, [BLACK, WHITE, BLACK]);
});

test('all picture improvements keep flat mid-grey and size the scene', async ({ page }) => {
	const grey = [0x80, 0x80, 0x80];
	const flat = { slots: [[['clear', 0x808080, 255, FAR], ['swap']]], probes: [[640, 480], [100, 900]] };
	const [native, half] = await run(page, {
		canvas: [1280, 960],
		settings: DEFAULT_SETTINGS,
		frames: [flat, { ...flat, settings: { resolution: 'half' } }],
	});
	expect(native.scene).toMatchObject({ width: 1280, height: 960 });
	expect([0, 4]).toContain(native.scene.samples);
	expectColors(native, [grey, grey]);
	expect(half.scene).toMatchObject({ width: 640, height: 480 });
	expectColors(half, [grey, grey]);
});

test('textures and the fog table recorded while the context is lost survive it', async ({ page }) => {
	const solid = (color) => [...color, ...color, ...color, ...color];
	const [before, lost, after] = await run(page, {
		frames: [{
			slots: [[['clear', 0, 255, FAR], ['teximage', 7, 2, solid(BLUE)], ['texbind', 7], ['texenable', 1], quad(0, 0, 640, 480, WHITE), ['swap']]],
			probes: [[320, 240]],
		}, {
			loseContext: true,
			slots: [[['teximage', 8, 2, solid(YELLOW)], ['fogtable', [[0, 65536, 255]]], ['fogcolor', 0xFF00FF], ['swap']]],
		}, {
			slots: [[
				['clear', 0, 255, FAR],
				['texbind', 7], quad(0, 0, 320, 480, WHITE),
				['texbind', 8], quad(320, 0, 640, 480, WHITE),
				['fogenable', 1], quad(0, 400, 640, 480, WHITE),
				['swap'],
			]],
			probes: [[160, 200], [480, 200], [320, 440]],
		}],
	});
	expectColors(before, [BLUE]);
	expect(lost.presented).toBe(false);
	expect(lost.states).toEqual([GFX_FREE, GFX_FREE]);
	expectColors(after, [BLUE, YELLOW, MAGENTA]);
});

test('a context that cannot be rebuilt after a loss fails the next pump', async ({ page }) => {
	const [lost, failed] = await run(page, {
		frames: [
			{ loseContext: true, brokenRestore: true, slots: [[['swap']]] },
			{ slots: [[['clear', 0, 255, FAR], ['swap']]] },
		],
	});
	expect(lost.pumpError).toBeNull();
	expect(failed.pumpError).toBe('createProgram failed');
	expect(failed.presented).toBe(false);
});

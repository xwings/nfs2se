// Pure helpers of the renderer: picture placement, W-buffer clear depth and
// the graphics slot views. The WebGL replay itself runs in tests/e2e.
import assert from 'node:assert/strict';
import { test } from 'node:test';
import { GFX_SLOT_BYTES } from '../../src/web/layout.js';
import { clearDepth, fitViewport, slotViews } from '../../src/web/renderer.js';

test('fitViewport centres the largest 4:3 picture', () => {
	assert.deepEqual(fitViewport(1920, 1080, true), { x: 240, y: 0, width: 1440, height: 1080 });
	assert.deepEqual(fitViewport(800, 800, true), { x: 0, y: 100, width: 800, height: 600 });
	assert.deepEqual(fitViewport(1001, 500, true), { x: 167, y: 0, width: 667, height: 500 });
	assert.deepEqual(fitViewport(640, 480, true), { x: 0, y: 0, width: 640, height: 480 });
	assert.deepEqual(fitViewport(1, 1, true), { x: 0, y: 0, width: 1, height: 1 });
});

test('fitViewport fills the area when the aspect ratio is not kept', () => {
	assert.deepEqual(fitViewport(1920, 1080, false), { x: 0, y: 0, width: 1920, height: 1080 });
});

test('clearDepth turns the 4.12 floating-point W value into 1/w', () => {
	assert.equal(clearDepth(0xFFFF), 0);
	assert.equal(clearDepth(0), 1);
	assert.equal(clearDepth(0x1000), 0.5);
	assert.equal(clearDepth(0x0800), 1 / 1.5);
	assert.equal(clearDepth(0xF000), 1 / 32768);
	assert.equal(clearDepth(0xFFFE), 1 / ((1 + 4094 / 4096) * 32768));
	for (let value = 1; value < 0xFFFF; value += 97)
		assert.ok(clearDepth(value) < clearDepth(value - 1), `not decreasing at ${value}`);
});

test('slotViews alias one graphics slot as bytes, int32 and float32', () => {
	const buffer = new ArrayBuffer(64 + GFX_SLOT_BYTES);
	const { u8, i32, f32 } = slotViews(buffer, 64);
	assert.equal(u8.byteOffset, 64);
	assert.equal(u8.length, GFX_SLOT_BYTES);
	assert.equal(i32.length, GFX_SLOT_BYTES / 4);
	assert.equal(f32.length, GFX_SLOT_BYTES / 4);
	i32[i32.length - 1] = 0x3F800000;
	assert.equal(f32[f32.length - 1], 1);
	assert.deepEqual([...u8.subarray(GFX_SLOT_BYTES - 4)], [0x00, 0x00, 0x80, 0x3F]);
	assert.equal(new Uint8Array(buffer)[63 + GFX_SLOT_BYTES], 0x3F);
});

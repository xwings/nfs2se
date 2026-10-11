// The AudioWorklet processor that plays the shared audio ring, run with
// stand-ins for the worklet globals.
import assert from 'node:assert/strict';
import { test } from 'node:test';
import {
	AUD_ACTIVE, AUD_CAPACITY_FRAMES, AUD_HEADER_WORDS, AUD_RATE, AUD_READ, AUD_SAMPLE_RATE, AUD_WRITE,
} from '../../src/web/layout.js';

const processors = new Map();
globalThis.AudioWorkletProcessor = class {};
globalThis.registerProcessor = (name, processor) => processors.set(name, processor);
globalThis.sampleRate = 2 * AUD_SAMPLE_RATE;
await import('../../src/web/audio-worklet.js');
const RingPlayer = processors.get('nfs2se-ring');

const ADDRESS = 64;

// A ring whose frame k holds (100 k, -100 k), wrapping like int16.
function ring({ read = 0, write = 0, active = 1 } = {}) {
	const buffer = new SharedArrayBuffer(ADDRESS + AUD_HEADER_WORDS * 4 + AUD_CAPACITY_FRAMES * 4);
	const header = new Int32Array(buffer, ADDRESS, AUD_HEADER_WORDS);
	const frames = new Int16Array(buffer, ADDRESS + AUD_HEADER_WORDS * 4, AUD_CAPACITY_FRAMES * 2);
	for (let k = 0; k < AUD_CAPACITY_FRAMES; ++k) {
		frames[2 * k] = 100 * k;
		frames[2 * k + 1] = -100 * k;
	}
	header[AUD_RATE] = AUD_SAMPLE_RATE;
	header[AUD_READ] = read;
	header[AUD_WRITE] = write;
	header[AUD_ACTIVE] = active;
	return { buffer, header, frames };
}

function render(player, length) {
	const left = new Float32Array(length).fill(NaN), right = new Float32Array(length).fill(NaN);
	assert.equal(player.process([], [[left, right]]), true);
	return { left: [...left], right: [...right] };
}

const sample = (value) => Math.fround(value / 32768);

test('the processor is registered as nfs2se-ring', () => {
	assert.equal(typeof RingPlayer, 'function');
});

test('frames are interpolated to the context rate and consumed', () => {
	const { buffer, header } = ring({ write: 10 });
	const player = new RingPlayer({ processorOptions: { buffer, address: ADDRESS } });
	const { left, right } = render(player, 8);
	assert.deepEqual(left, [0, 50, 100, 150, 200, 250, 300, 350].map(sample));
	assert.deepEqual(right, [0, -50, -100, -150, -200, -250, -300, -350].map(sample));
	assert.equal(header[AUD_READ], 4);
	assert.deepEqual(render(player, 2).left, [400, 450].map(sample));
	assert.equal(header[AUD_READ], 5);
});

test('an underrun plays silence and keeps the unplayed frame', () => {
	const { buffer, header } = ring({ write: 2 });
	const player = new RingPlayer({ processorOptions: { buffer, address: ADDRESS } });
	const { left, right } = render(player, 6);
	assert.deepEqual(left, [0, 50, 0, 0, 0, 0].map(sample));
	assert.deepEqual(right, [0, -50, 0, 0, 0, 0].map(sample));
	assert.equal(header[AUD_READ], 1);
	header[AUD_WRITE] = 4;
	assert.deepEqual(render(player, 2).left, [100, 150].map(sample));
});

test('a fractional read position carries over between calls', () => {
	globalThis.sampleRate = 48000;
	try {
		const { buffer, header } = ring({ write: 4096 });
		const player = new RingPlayer({ processorOptions: { buffer, address: ADDRESS } });
		render(player, 128);
		assert.equal(header[AUD_READ], Math.floor(128 * AUD_SAMPLE_RATE / 48000));
		render(player, 128);
		assert.equal(header[AUD_READ], Math.floor(256 * AUD_SAMPLE_RATE / 48000));
	} finally {
		globalThis.sampleRate = 2 * AUD_SAMPLE_RATE;
	}
});

test('reading wraps around the ring and the int32 counters', () => {
	const capacity = AUD_CAPACITY_FRAMES;
	const { buffer, header } = ring({ read: capacity - 2, write: capacity + 4 });
	const player = new RingPlayer({ processorOptions: { buffer, address: ADDRESS } });
	const value = (k) => (100 * k << 16) >> 16; // the int16 stored for frame k
	const expected = [value(capacity - 2), (value(capacity - 2) + value(capacity - 1)) / 2, value(capacity - 1), (value(capacity - 1) + value(0)) / 2];
	assert.deepEqual(render(player, 4).left, expected.map(sample));
	assert.equal(header[AUD_READ], capacity);

	const wrapped = ring({ read: 0x7FFFFFFF, write: (0x7FFFFFFF + 6) | 0 });
	const second = new RingPlayer({ processorOptions: { buffer: wrapped.buffer, address: ADDRESS } });
	render(second, 8);
	assert.equal(wrapped.header[AUD_READ], (0x7FFFFFFF + 4) | 0);
});

test('a stopped ring drops what is queued and plays silence', () => {
	const { buffer, header } = ring({ read: 3, write: 40, active: 0 });
	const player = new RingPlayer({ processorOptions: { buffer, address: ADDRESS } });
	const { left, right } = render(player, 4);
	assert.deepEqual(left, [0, 0, 0, 0]);
	assert.deepEqual(right, [0, 0, 0, 0]);
	assert.equal(header[AUD_READ], 40);
	render(player, 4);
	assert.equal(header[AUD_READ], 40);
	header[AUD_ACTIVE] = 1;
	header[AUD_WRITE] = 44;
	assert.deepEqual(render(player, 2).left, [4000, 4050].map(sample));
});

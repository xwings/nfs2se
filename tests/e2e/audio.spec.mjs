// The audio path in a real browser: the AudioWorklet module, the shared ring
// handed to it and the volume gain, rendered offline at the game's rate.
import { expect } from '@playwright/test';
import { openBlankPage, test } from './helpers.mjs';

test('the worklet plays the shared ring at the game rate with the volume applied', async ({ page }) => {
	await openBlankPage(page);
	const { left, right, frames, read, consumer } = await page.evaluate(async () => {
		const L = await import('/layout.js');
		const { startAudio } = await import('/audio.js');
		const address = 64, count = 1000;
		const buffer = new SharedArrayBuffer(address + (L.AUD_HEADER_WORDS + L.AUD_CAPACITY_FRAMES) * 4);
		const header = new Int32Array(buffer, address, L.AUD_HEADER_WORDS);
		const frames = new Int16Array(buffer, address + L.AUD_HEADER_WORDS * 4, L.AUD_CAPACITY_FRAMES * 2);
		for (let i = 0; i < count; ++i) {
			frames[2 * i] = (i * 397) % 20000 - 10000;
			frames[2 * i + 1] = -frames[2 * i];
		}
		header[L.AUD_RATE] = L.AUD_SAMPLE_RATE;
		header[L.AUD_WRITE] = count;
		header[L.AUD_ACTIVE] = 1;
		const context = new OfflineAudioContext(2, 2048, L.AUD_SAMPLE_RATE);
		await startAudio(context, buffer, address, 0.5);
		const rendered = await context.startRendering();
		return {
			left: [...rendered.getChannelData(0)], right: [...rendered.getChannelData(1)],
			frames: [...frames.subarray(0, 2 * count)], read: header[L.AUD_READ], consumer: header[L.AUD_CONSUMER],
		};
	});
	expect(consumer).toBe(1);
	expect(read).toBe(999); // the last frame waits for the next one to interpolate towards
	const expected = (channel) => left.map((_, i) => (i < 999 ? 0.5 * frames[2 * i + channel] / 32768 : 0));
	const off = (actual, want) => Math.max(...actual.map((value, i) => Math.abs(value - want[i])));
	expect(off(left, expected(0))).toBeLessThan(1e-6);
	expect(off(right, expected(1))).toBeLessThan(1e-6);
});

test('pausing suspends the context and never rejects, even once it is closed', async ({ page }) => {
	await openBlankPage(page);
	const warnings = [];
	page.on('console', (message) => {
		if (message.text().startsWith('Audio could not pause or resume'))
			warnings.push(message.type());
	});
	const states = await page.evaluate(async () => {
		const L = await import('/layout.js');
		const { startAudio } = await import('/audio.js');
		const buffer = new SharedArrayBuffer((L.AUD_HEADER_WORDS + L.AUD_CAPACITY_FRAMES) * 4);
		const context = new AudioContext({ sampleRate: L.AUD_SAMPLE_RATE });
		const audio = await startAudio(context, buffer, 0, 1);
		await audio.setPaused(false);
		const running = context.state;
		await audio.setPaused(true);
		const paused = context.state;
		await context.close();
		await audio.setPaused(false);
		await audio.setPaused(true);
		return [running, paused, context.state];
	});
	expect(states).toEqual(['running', 'suspended', 'closed']);
	expect(warnings).toEqual(['warning', 'warning']);
});

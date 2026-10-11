// Real keyboard and mouse events through src/web/input.js into the control
// block, on a blank isolated page with an 800x600 canvas at the top left.
import { expect } from '@playwright/test';
import { MOUSE_LEFT, MOUSE_LEFT_LATCH, WM_CHAR, WM_KEYDOWN, WM_KEYUP } from '../../src/web/layout.js';
import { openBlankPage, test } from './helpers.mjs';

const lParam = (scancode) => 1 | scancode << 16;

async function setUp(page) {
	await openBlankPage(page);
	await page.evaluate(async () => {
		const L = await import('/layout.js');
		const { Input } = await import('/input.js');
		const canvas = document.body.appendChild(document.createElement('canvas'));
		canvas.style.cssText = 'position: fixed; left: 0; top: 0; width: 800px; height: 600px';
		const ctl = new Int32Array(new SharedArrayBuffer(L.CTL_WORDS * 4));
		const harness = { L, ctl, fullscreens: 0 };
		harness.input = new Input(canvas, ctl, { keepAspect: () => true, onFullscreen: () => { ++harness.fullscreens; } });
		harness.input.setEnabled(true);
		window.harness = harness;
	});
}

// Takes the queued [message, wParam, lParam] entries like src/platform/user32.c.
function takeMessages(page) {
	return page.evaluate(() => {
		const { L, ctl } = window.harness;
		const messages = [];
		for (let tail = ctl[L.CTL_RING_TAIL]; tail !== ctl[L.CTL_RING_HEAD]; tail = (tail + 1) | 0) {
			const entry = L.CTL_RING_BASE + (tail & (L.CTL_RING_ENTRIES - 1)) * L.CTL_RING_ENTRY_WORDS;
			messages.push([ctl[entry + L.RING_MSG], ctl[entry + L.RING_WPARAM], ctl[entry + L.RING_LPARAM]]);
		}
		ctl[L.CTL_RING_TAIL] = ctl[L.CTL_RING_HEAD];
		return messages;
	});
}

test('key presses become key and character messages with Windows codes', async ({ page }) => {
	await setUp(page);
	await page.keyboard.press('KeyA');
	expect(await takeMessages(page)).toEqual([
		[WM_KEYDOWN, 0x41, lParam(0x1E)], [WM_CHAR, 0x61, lParam(0x1E)], [WM_KEYUP, 0x41, lParam(0x1E)],
	]);
	await page.keyboard.press('Shift+KeyA');
	expect(await takeMessages(page)).toEqual([
		[WM_KEYDOWN, 0x10, lParam(0x2A)], [WM_KEYDOWN, 0x41, lParam(0x1E)], [WM_CHAR, 0x41, lParam(0x1E)],
		[WM_KEYUP, 0x41, lParam(0x1E)], [WM_KEYUP, 0x10, lParam(0x2A)],
	]);
	await page.keyboard.press('ArrowUp');
	await page.keyboard.press('Enter');
	expect(await takeMessages(page)).toEqual([
		[WM_KEYDOWN, 0x26, lParam(0x48)], [WM_KEYUP, 0x26, lParam(0x48)],
		[WM_KEYDOWN, 0x0D, lParam(0x1C)], [WM_CHAR, 0x0D, lParam(0x1C)], [WM_KEYUP, 0x0D, lParam(0x1C)],
	]);
});

test('Alt+Enter asks for fullscreen and the game only sees Alt', async ({ page }) => {
	await setUp(page);
	await page.keyboard.press('Alt+Enter');
	expect(await page.evaluate(() => window.harness.fullscreens)).toBe(1);
	expect(await takeMessages(page)).toEqual([[WM_KEYDOWN, 0x12, lParam(0x38)], [WM_KEYUP, 0x12, lParam(0x38)]]);
});

test('the mouse maps into the 640x480 picture and the left button latches', async ({ page }) => {
	await setUp(page);
	const mouse = () => page.evaluate(() => {
		const { L, ctl } = window.harness;
		return [ctl[L.CTL_MOUSE_X], ctl[L.CTL_MOUSE_Y], ctl[L.CTL_MOUSE_BUTTONS]];
	});
	await page.mouse.move(400, 300);
	expect(await mouse()).toEqual([320, 240, 0]);
	await page.mouse.down();
	expect(await mouse()).toEqual([320, 240, MOUSE_LEFT | MOUSE_LEFT_LATCH]);
	await page.mouse.up();
	expect(await mouse()).toEqual([320, 240, MOUSE_LEFT_LATCH]);
	await page.mouse.move(799, 599);
	expect((await mouse()).slice(0, 2)).toEqual([639, 479]);
});

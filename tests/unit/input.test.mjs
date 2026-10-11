// Keyboard, mouse and gamepad translation into the shared control block.
import assert from 'node:assert/strict';
import { beforeEach, test } from 'node:test';
import {
	CTL_MOUSE_BUTTONS, CTL_MOUSE_MOVES, CTL_MOUSE_X, CTL_MOUSE_Y, CTL_PAD_BASE, CTL_PAD_STRIDE, CTL_RING_BASE,
	CTL_RING_ENTRIES, CTL_RING_ENTRY_WORDS, CTL_RING_HEAD, CTL_RING_TAIL, CTL_SEQ, CTL_WORDS, HAT_DOWN, HAT_LEFT,
	HAT_RIGHT, HAT_UP, MOUSE_LEFT, MOUSE_LEFT_LATCH, PAD_AXES, PAD_BUTTONS, PAD_CONNECTED, PAD_HAT, PAD_NUM_AXES,
	PAD_NUM_BUTTONS, PAD_NUM_HATS, RING_LPARAM, RING_MSG, RING_WPARAM, WM_CHAR, WM_KEYDOWN, WM_KEYUP,
} from '../../src/web/layout.js';
import { charCode, Input, KEYMAP, padState, pushMessage } from '../../src/web/input.js';

const EXTENDED_KEYS = ['NumpadEnter', 'ControlRight', 'NumpadDivide', 'AltRight', 'Home', 'ArrowUp', 'PageUp',
	'ArrowLeft', 'ArrowRight', 'End', 'ArrowDown', 'PageDown', 'Insert', 'Delete'];

function newCtl() {
	return new Int32Array(new SharedArrayBuffer(CTL_WORDS * 4));
}

// Consumes the queued messages like src/platform/user32.c.
function takeMessages(ctl) {
	const messages = [];
	for (; ctl[CTL_RING_TAIL] !== ctl[CTL_RING_HEAD]; ctl[CTL_RING_TAIL] = (ctl[CTL_RING_TAIL] + 1) | 0) {
		const entry = CTL_RING_BASE + (ctl[CTL_RING_TAIL] & (CTL_RING_ENTRIES - 1)) * CTL_RING_ENTRY_WORDS;
		messages.push([ctl[entry + RING_MSG], ctl[entry + RING_WPARAM], ctl[entry + RING_LPARAM]]);
	}
	return messages;
}

function listeners() {
	const registry = new Map();
	return {
		addEventListener(type, listener) {
			registry.set(type, [...(registry.get(type) ?? []), listener]);
		},
		dispatch(type, event) {
			for (const listener of registry.get(type) ?? [])
				listener(event);
			return event;
		},
	};
}

function keyEvent(code, key, modifiers = {}) {
	return {
		code, key, altKey: false, ctrlKey: false, metaKey: false, repeat: false, getModifierState: () => false, ...modifiers,
		prevented: false, preventDefault() { this.prevented = true; },
	};
}

function pointerEvent(clientX, clientY, button = 0) {
	return { clientX, clientY, button };
}

function button(pressed, value = pressed ? 1 : 0) {
	return { pressed, value };
}

test('KEYMAP holds Windows virtual-key codes and set 1 scancodes', () => {
	for (const [code, key] of Object.entries(KEYMAP)) {
		assert.equal(key.length, 2, code);
		const [vk, scancode] = key;
		assert.ok(Number.isInteger(vk) && vk > 0 && vk < 0x100, `${code} vk`);
		assert.ok(Number.isInteger(scancode) && scancode > 0 && scancode < 0x80, `${code} scancode`);
	}
	assert.deepEqual(KEYMAP.KeyA, [0x41, 0x1E]);
	assert.deepEqual(KEYMAP.KeyQ, [0x51, 0x10]);
	assert.deepEqual(KEYMAP.KeyP, [0x50, 0x19]);
	assert.deepEqual(KEYMAP.KeyL, [0x4C, 0x26]);
	assert.deepEqual(KEYMAP.KeyZ, [0x5A, 0x2C]);
	assert.deepEqual(KEYMAP.KeyM, [0x4D, 0x32]);
	assert.deepEqual(KEYMAP.Digit1, [0x31, 0x02]);
	assert.deepEqual(KEYMAP.Digit0, [0x30, 0x0B]);
	assert.deepEqual(KEYMAP.F1, [0x70, 0x3B]);
	assert.deepEqual(KEYMAP.F10, [0x79, 0x44]);
	assert.deepEqual(KEYMAP.F11, [0x7A, 0x57]);
	assert.deepEqual(KEYMAP.F12, [0x7B, 0x58]);
	assert.deepEqual(KEYMAP.Escape, [0x1B, 0x01]);
	assert.deepEqual(KEYMAP.Enter, [0x0D, 0x1C]);
	assert.deepEqual(KEYMAP.Space, [0x20, 0x39]);
	assert.deepEqual(KEYMAP.ArrowUp, [0x26, 0x48]);
	assert.deepEqual(KEYMAP.PageUp, [0x21, 0x49]);
	assert.deepEqual(KEYMAP.PageDown, [0x22, 0x51]);
});

test('extended keys share the scancode of their base key; base scancodes are unique', () => {
	const base = new Map();
	for (const [code, [, scancode]] of Object.entries(KEYMAP)) {
		if (EXTENDED_KEYS.includes(code))
			continue;
		assert.ok(!base.has(scancode), `${code} and ${base.get(scancode)} share scancode ${scancode}`);
		base.set(scancode, code);
	}
	for (const code of EXTENDED_KEYS)
		assert.ok(base.has(KEYMAP[code][1]), code);
	assert.equal(KEYMAP.ArrowUp[1], KEYMAP.Numpad8[1]);
	assert.equal(KEYMAP.NumpadEnter[1], KEYMAP.Enter[1]);
});

test('charCode: printable Latin-1 characters and the control keys that type one', () => {
	assert.equal(charCode(keyEvent('KeyA', 'a'), 0x41), 0x61);
	assert.equal(charCode(keyEvent('KeyA', 'A', { shiftKey: true }), 0x41), 0x41);
	assert.equal(charCode(keyEvent('Digit2', '@', { shiftKey: true }), 0x32), 0x40);
	assert.equal(charCode(keyEvent('Quote', 'é'), 0xDE), 0xE9);
	assert.equal(charCode(keyEvent('KeyE', '€'), 0x45), 0);
	assert.equal(charCode(keyEvent('KeyA', 'a', { ctrlKey: true }), 0x41), 0);
	const altGr = { ctrlKey: true, altKey: true, getModifierState: (key) => key === 'AltGraph' };
	assert.equal(charCode(keyEvent('KeyQ', '@', altGr), 0x51), 0x40);
	assert.equal(charCode(keyEvent('KeyA', 'a', { metaKey: true }), 0x41), 0);
	assert.equal(charCode(keyEvent('Backspace', 'Backspace'), 0x08), 0x08);
	assert.equal(charCode(keyEvent('Tab', 'Tab'), 0x09), 0x09);
	assert.equal(charCode(keyEvent('Enter', 'Enter'), 0x0D), 0x0D);
	assert.equal(charCode(keyEvent('Escape', 'Escape'), 0x1B), 0x1B);
	assert.equal(charCode(keyEvent('ArrowUp', 'ArrowUp'), 0x26), 0);
	assert.equal(charCode(keyEvent('F1', 'F1'), 0x70), 0);
});

test('charCode: modifier and lock keys type nothing', () => {
	assert.equal(charCode(keyEvent('ShiftLeft', 'Shift', { shiftKey: true }), 0x10), 0);
	assert.equal(charCode(keyEvent('ControlLeft', 'Control', { ctrlKey: true }), 0x11), 0);
	assert.equal(charCode(keyEvent('AltLeft', 'Alt', { altKey: true }), 0x12), 0);
	assert.equal(charCode(keyEvent('Pause', 'Pause'), 0x13), 0);
	assert.equal(charCode(keyEvent('CapsLock', 'CapsLock'), 0x14), 0);
});

test('pushMessage writes an entry, advances the head and bumps the sequence', () => {
	const ctl = newCtl();
	assert.equal(pushMessage(ctl, WM_KEYDOWN, 0x41, 0x1E0001), true);
	assert.deepEqual([...ctl.subarray(CTL_RING_BASE, CTL_RING_BASE + 3)], [WM_KEYDOWN, 0x41, 0x1E0001]);
	assert.equal(ctl[CTL_RING_HEAD], 1);
	assert.equal(ctl[CTL_SEQ], 1);
	assert.deepEqual(takeMessages(ctl), [[WM_KEYDOWN, 0x41, 0x1E0001]]);
});

test('pushMessage refuses a full ring and reuses consumed entries', () => {
	const ctl = newCtl();
	for (let i = 0; i < CTL_RING_ENTRIES; ++i)
		assert.equal(pushMessage(ctl, WM_CHAR, i, 0), true);
	assert.equal(pushMessage(ctl, WM_CHAR, 999, 0), false);
	assert.equal(ctl[CTL_RING_HEAD], CTL_RING_ENTRIES);
	assert.equal(ctl[CTL_SEQ], CTL_RING_ENTRIES);
	ctl[CTL_RING_TAIL] = 1;
	assert.equal(pushMessage(ctl, WM_CHAR, 1000, 0), true);
	assert.equal(ctl[CTL_RING_BASE + RING_WPARAM], 1000);
	assert.equal(pushMessage(ctl, WM_CHAR, 1001, 0), false);
});

test('pushMessage keeps working when the indices wrap around int32', () => {
	const ctl = newCtl();
	ctl[CTL_RING_HEAD] = ctl[CTL_RING_TAIL] = 0x7FFFFFFF;
	assert.equal(pushMessage(ctl, WM_KEYUP, 7, 0), true);
	assert.equal(ctl[CTL_RING_HEAD], -0x80000000);
	assert.equal(ctl[CTL_RING_BASE + (CTL_RING_ENTRIES - 1) * CTL_RING_ENTRY_WORDS + RING_WPARAM], 7);
	for (let i = 1; i < CTL_RING_ENTRIES; ++i)
		assert.equal(pushMessage(ctl, WM_KEYUP, i, 0), true);
	assert.equal(pushMessage(ctl, WM_KEYUP, 0, 0), false);
	assert.equal(takeMessages(ctl).length, CTL_RING_ENTRIES);
	assert.equal(pushMessage(ctl, WM_KEYUP, 0, 0), true);
});

test('padState maps a standard gamepad to SDL axes, buttons and a hat', () => {
	const buttons = Array.from({ length: 17 }, () => button(false));
	buttons[0] = button(true);
	buttons[3] = button(true);
	buttons[7] = button(true, 1);
	buttons[6] = button(false, 0);
	buttons[12] = button(true);
	buttons[15] = button(true);
	const state = padState({ mapping: 'standard', axes: [-1, 1, 0.5, 0], buttons });
	assert.deepEqual(state.axes, [-32767, 32767, 16384, 0, -32768, 32767]);
	assert.equal(state.buttons, 1 << 0 | 1 << 3 | 1 << 7);
	assert.equal(state.hat, HAT_UP | HAT_RIGHT);
	assert.equal(state.numHats, 1);
	assert.equal(state.numButtons, 12);

	buttons[12] = buttons[15] = button(false);
	buttons[13] = buttons[14] = button(true);
	buttons[6] = button(false, 0.5);
	const second = padState({ mapping: 'standard', axes: [], buttons });
	assert.equal(second.hat, HAT_DOWN | HAT_LEFT);
	assert.deepEqual(second.axes, [0, 0, 0, 0, 0, 32767]);
});

test('padState passes other gamepads through, clamped to six axes and twelve buttons', () => {
	const state = padState({
		mapping: '', axes: [0.25, -0.25, 2, -2, 0, 0, 0.9], buttons: [button(false), button(true), button(true)],
	});
	assert.deepEqual(state.axes, [8192, -8192, 32767, -32768, 0, 0]);
	assert.equal(state.buttons, 0b110);
	assert.equal(state.numButtons, 3);
	assert.equal(state.hat, 0);
	assert.equal(state.numHats, 0);

	const many = padState({ mapping: '', axes: [], buttons: Array.from({ length: 20 }, () => button(true)) });
	assert.equal(many.numButtons, 12);
	assert.equal(many.buttons, 0xFFF);
	assert.deepEqual(many.axes, []);
});

let page, canvas, ctl, fullscreenRequests, keepAspect, input;

function setNavigator(value) {
	Object.defineProperty(globalThis, 'navigator', { value, configurable: true, writable: true });
}

beforeEach(() => {
	page = listeners();
	globalThis.addEventListener = page.addEventListener;
	canvas = { ...listeners(), getBoundingClientRect: () => ({ left: 10, top: 20, width: 1920, height: 1080 }) };
	ctl = newCtl();
	fullscreenRequests = 0;
	keepAspect = true;
	input = new Input(canvas, ctl, { keepAspect: () => keepAspect, onFullscreen: () => ++fullscreenRequests });
	setNavigator({});
});

test('Input ignores the keyboard until it is enabled', () => {
	const event = page.dispatch('keydown', keyEvent('KeyA', 'a'));
	assert.equal(event.prevented, false);
	assert.deepEqual(takeMessages(ctl), []);
});

test('Input turns key presses into WM_KEYDOWN, WM_CHAR and WM_KEYUP', () => {
	input.setEnabled(true);
	const lParam = 1 | 0x1E << 16;
	assert.equal(page.dispatch('keydown', keyEvent('KeyA', 'a')).prevented, true);
	assert.equal(page.dispatch('keyup', keyEvent('KeyA', 'a')).prevented, true);
	assert.deepEqual(takeMessages(ctl), [[WM_KEYDOWN, 0x41, lParam], [WM_CHAR, 0x61, lParam], [WM_KEYUP, 0x41, lParam]]);

	page.dispatch('keydown', keyEvent('ShiftLeft', 'Shift', { shiftKey: true }));
	page.dispatch('keydown', keyEvent('ArrowLeft', 'ArrowLeft', { shiftKey: true }));
	assert.deepEqual(takeMessages(ctl), [[WM_KEYDOWN, 0x10, 1 | 0x2A << 16], [WM_KEYDOWN, 0x25, 1 | 0x4B << 16]]);
});

test('Input drops releases of keys it did not see pressed and unknown keys', () => {
	input.setEnabled(true);
	page.dispatch('keyup', keyEvent('KeyB', 'b'));
	assert.equal(page.dispatch('keydown', keyEvent('MetaLeft', 'Meta')).prevented, false);
	assert.deepEqual(takeMessages(ctl), []);
});

test('Input leaves the browser keys to the browser but still reports them', () => {
	input.setEnabled(true);
	for (const code of ['F5', 'F11', 'F12'])
		assert.equal(page.dispatch('keydown', keyEvent(code, code)).prevented, false, code);
	assert.equal(page.dispatch('keydown', keyEvent('F1', 'F1')).prevented, true);
	assert.deepEqual(takeMessages(ctl).map(([message, vk]) => [message, vk]),
		[[WM_KEYDOWN, 0x74], [WM_KEYDOWN, 0x7A], [WM_KEYDOWN, 0x7B], [WM_KEYDOWN, 0x70]]);
});

test('Alt+Enter toggles fullscreen once per press and reaches no game', () => {
	input.setEnabled(true);
	assert.equal(page.dispatch('keydown', keyEvent('Enter', 'Enter', { altKey: true })).prevented, true);
	page.dispatch('keydown', keyEvent('Enter', 'Enter', { altKey: true, repeat: true }));
	assert.equal(fullscreenRequests, 1);
	assert.deepEqual(takeMessages(ctl), []);
});

test('disabling or blurring releases held keys and the mouse button', () => {
	input.setEnabled(true);
	page.dispatch('keydown', keyEvent('ArrowUp', 'ArrowUp'));
	page.dispatch('keydown', keyEvent('Space', ' '));
	canvas.dispatch('pointerdown', pointerEvent(100, 100));
	takeMessages(ctl);
	input.setEnabled(false);
	assert.deepEqual(takeMessages(ctl), [[WM_KEYUP, 0x26, 1 | 0x48 << 16], [WM_KEYUP, 0x20, 1 | 0x39 << 16]]);
	assert.equal(ctl[CTL_MOUSE_BUTTONS] & MOUSE_LEFT, 0);

	input.setEnabled(true);
	page.dispatch('keydown', keyEvent('KeyW', 'w'));
	takeMessages(ctl);
	page.dispatch('blur', {});
	assert.deepEqual(takeMessages(ctl), [[WM_KEYUP, 0x57, 1 | 0x11 << 16]]);
	page.dispatch('blur', {});
	assert.deepEqual(takeMessages(ctl), []);
});

// A pending release is dropped when its key is pressed again; the others
// follow in the order the keys were released.
test('key releases the full ring refuses reach the game through later polls', () => {
	input.setEnabled(true);
	for (const [code, key] of [['ArrowUp', 'ArrowUp'], ['KeyW', 'w'], ['KeyS', 's']])
		page.dispatch('keydown', keyEvent(code, key));
	takeMessages(ctl);
	for (let i = 0; i < CTL_RING_ENTRIES; ++i)
		pushMessage(ctl, WM_CHAR, i, 0);
	page.dispatch('keyup', keyEvent('ArrowUp', 'ArrowUp'));
	page.dispatch('keydown', keyEvent('ArrowUp', 'ArrowUp'));
	page.dispatch('keyup', keyEvent('KeyS', 's'));
	input.setEnabled(false);
	input.poll();
	assert.equal(takeMessages(ctl).length, CTL_RING_ENTRIES);
	input.poll();
	assert.deepEqual(takeMessages(ctl),
		[[WM_KEYUP, 0x53, 1 | 0x1F << 16], [WM_KEYUP, 0x57, 1 | 0x11 << 16], [WM_KEYUP, 0x26, 1 | 0x48 << 16]]);
	input.poll();
	assert.deepEqual(takeMessages(ctl), []);
});

test('pointer positions map into the letterboxed 640x480 picture once enabled', () => {
	canvas.dispatch('pointermove', pointerEvent(500, 500));
	assert.equal(ctl[CTL_MOUSE_MOVES], 0);
	input.setEnabled(true);
	canvas.dispatch('pointermove', pointerEvent(10 + 240 + 720, 20 + 540));
	assert.deepEqual([ctl[CTL_MOUSE_X], ctl[CTL_MOUSE_Y], ctl[CTL_MOUSE_MOVES]], [320, 240, 1]);
	canvas.dispatch('pointermove', pointerEvent(0, 0));
	assert.deepEqual([ctl[CTL_MOUSE_X], ctl[CTL_MOUSE_Y]], [0, 0]);
	canvas.dispatch('pointermove', pointerEvent(5000, 5000));
	assert.deepEqual([ctl[CTL_MOUSE_X], ctl[CTL_MOUSE_Y], ctl[CTL_MOUSE_MOVES]], [639, 479, 3]);
	keepAspect = false;
	canvas.dispatch('pointermove', pointerEvent(10 + 960, 20 + 1079));
	assert.deepEqual([ctl[CTL_MOUSE_X], ctl[CTL_MOUSE_Y]], [320, 479]);
});

test('the left button latches until the game reads it; other buttons are ignored', () => {
	canvas.dispatch('pointerdown', pointerEvent(500, 500));
	assert.equal(ctl[CTL_MOUSE_BUTTONS], 0);
	input.setEnabled(true);
	canvas.dispatch('pointerdown', pointerEvent(500, 500, 2));
	assert.equal(ctl[CTL_MOUSE_BUTTONS], 0);
	canvas.dispatch('pointerdown', pointerEvent(500, 500));
	assert.equal(ctl[CTL_MOUSE_BUTTONS], MOUSE_LEFT | MOUSE_LEFT_LATCH);
	page.dispatch('pointerup', pointerEvent(500, 500));
	assert.equal(ctl[CTL_MOUSE_BUTTONS], MOUSE_LEFT_LATCH);
	let prevented = false;
	canvas.dispatch('contextmenu', { preventDefault: () => { prevented = true; } });
	assert.equal(prevented, true);
});

test('poll publishes the first two connected gamepads while enabled', () => {
	const standard = {
		connected: true, mapping: 'standard', axes: [1, 0, 0, 0],
		buttons: [button(true), ...Array.from({ length: 16 }, () => button(false))],
	};
	const other = { connected: true, mapping: '', axes: [0, -1], buttons: [button(false), button(true)] };
	setNavigator({ getGamepads: () => [null, { connected: false }, standard, other] });
	input.poll();
	assert.equal(ctl[CTL_PAD_BASE + PAD_CONNECTED], 0);

	input.setEnabled(true);
	input.poll();
	const first = CTL_PAD_BASE, second = CTL_PAD_BASE + CTL_PAD_STRIDE;
	assert.equal(ctl[first + PAD_CONNECTED], 1);
	assert.equal(ctl[first + PAD_NUM_AXES], 6);
	assert.deepEqual([...ctl.subarray(first + PAD_AXES, first + PAD_AXES + 6)], [32767, 0, 0, 0, -32768, -32768]);
	assert.equal(ctl[first + PAD_BUTTONS], 1);
	assert.equal(ctl[first + PAD_NUM_BUTTONS], 12);
	assert.equal(ctl[first + PAD_NUM_HATS], 1);
	assert.equal(ctl[second + PAD_CONNECTED], 1);
	assert.equal(ctl[second + PAD_NUM_AXES], 2);
	assert.deepEqual([...ctl.subarray(second + PAD_AXES, second + PAD_AXES + 2)], [0, -32767]);
	assert.equal(ctl[second + PAD_BUTTONS], 2);
	assert.equal(ctl[second + PAD_HAT], 0);
	assert.equal(ctl[second + PAD_NUM_HATS], 0);

	setNavigator({ getGamepads: () => [standard] });
	input.poll();
	assert.equal(ctl[first + PAD_CONNECTED], 1);
	assert.equal(ctl[second + PAD_CONNECTED], 0);
	input.setEnabled(false);
	input.poll();
	assert.equal(ctl[first + PAD_CONNECTED], 0);
	setNavigator({});
	input.setEnabled(true);
	input.poll();
	assert.equal(ctl[first + PAD_CONNECTED], 0);
});

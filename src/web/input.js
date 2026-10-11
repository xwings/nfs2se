// Keyboard, mouse and gamepad input. Keys become Windows messages in the
// control ring (consumed by src/platform/user32.c); the mouse and gamepads are
// published as shared words that src/platform/dinput.c reads.
import {
	CTL_MOUSE_BUTTONS, CTL_MOUSE_MOVES, CTL_MOUSE_X, CTL_MOUSE_Y, CTL_PAD_BASE, CTL_PAD_COUNT, CTL_PAD_STRIDE,
	CTL_RING_BASE, CTL_RING_ENTRIES, CTL_RING_ENTRY_WORDS, CTL_RING_HEAD, CTL_RING_TAIL, CTL_SEQ, GAME_HEIGHT, GAME_WIDTH,
	HAT_DOWN, HAT_LEFT, HAT_RIGHT, HAT_UP, KEY_REPEAT, KEY_SCANCODE_SHIFT, MOUSE_LEFT, MOUSE_LEFT_LATCH, PAD_AXES,
	PAD_BUTTONS, PAD_CONNECTED, PAD_HAT, PAD_MAX_AXES, PAD_NUM_AXES, PAD_NUM_BUTTONS, PAD_NUM_HATS, RING_LPARAM,
	RING_MSG, RING_WPARAM, SC_DOWN, SC_ESCAPE, SC_F11, SC_F12, SC_LEFT, SC_RIGHT, SC_UP, VK_BACK, VK_DOWN, VK_ESCAPE,
	VK_F11, VK_F12, VK_LEFT, VK_RETURN, VK_RIGHT, VK_TAB, VK_UP, WM_CHAR, WM_KEYDOWN, WM_KEYUP,
} from './layout.js';
import { fitViewport } from './renderer.js';

const PAD_BUTTON_COUNT = 12;
// Keys the browser keeps for itself (reload, fullscreen, developer tools).
const BROWSER_KEYS = new Set(['F5', 'F11', 'F12']);

// KeyboardEvent.code -> [Windows virtual-key code, set 1 scancode]. Extended
// keys share the scancode of their base key, as the game sees no E0 prefix.
export const KEYMAP = (() => {
	const keys = {
		Escape: [VK_ESCAPE, SC_ESCAPE], Minus: [0xBD, 0x0C], Equal: [0xBB, 0x0D], Backspace: [VK_BACK, 0x0E], Tab: [VK_TAB, 0x0F],
		BracketLeft: [0x5B, 0x1A], BracketRight: [0x5D, 0x1B], Enter: [VK_RETURN, 0x1C], ControlLeft: [0x11, 0x1D],
		Semicolon: [0x3B, 0x27], Quote: [0xDE, 0x28], Backquote: [0xC0, 0x29], ShiftLeft: [0x10, 0x2A],
		Backslash: [0x5C, 0x2B], Comma: [0x2C, 0x33], Period: [0xBE, 0x34], Slash: [0x2F, 0x35],
		ShiftRight: [0x10, 0x36], NumpadMultiply: [0x6A, 0x37], AltLeft: [0x12, 0x38], Space: [0x20, 0x39],
		CapsLock: [0x14, 0x3A], NumLock: [0x90, 0x45], ScrollLock: [0x91, 0x46],
		Numpad7: [0x67, 0x47], Numpad8: [0x68, 0x48], Numpad9: [0x69, 0x49], NumpadSubtract: [0x6D, 0x4A],
		Numpad4: [0x64, 0x4B], Numpad5: [0x65, 0x4C], Numpad6: [0x66, 0x4D], NumpadAdd: [0x6B, 0x4E],
		Numpad1: [0x61, 0x4F], Numpad2: [0x62, 0x50], Numpad3: [0x63, 0x51], Numpad0: [0x60, 0x52],
		NumpadDecimal: [0x6E, 0x53], IntlBackslash: [0xE2, 0x56], F11: [VK_F11, SC_F11], F12: [VK_F12, SC_F12],
		NumpadEnter: [VK_RETURN, 0x1C], ControlRight: [0x11, 0x1D], NumpadDivide: [0x6F, 0x35], AltRight: [0x12, 0x38],
		Home: [0x24, 0x47], ArrowUp: [VK_UP, SC_UP], PageUp: [0x21, 0x49], ArrowLeft: [VK_LEFT, SC_LEFT],
		ArrowRight: [VK_RIGHT, SC_RIGHT], End: [0x23, 0x4F], ArrowDown: [VK_DOWN, SC_DOWN], PageDown: [0x22, 0x51],
		Insert: [0x2D, 0x52], Delete: [0x2E, 0x53],
	};
	const rows = [['1234567890', 0x02, 'Digit'], ['QWERTYUIOP', 0x10, 'Key'], ['ASDFGHJKL', 0x1E, 'Key'], ['ZXCVBNM', 0x2C, 'Key']];
	for (const [chars, firstScancode, prefix] of rows)
		[...chars].forEach((c, i) => { keys[prefix + c] = [c.charCodeAt(0), firstScancode + i]; });
	for (let i = 0; i < 10; ++i)
		keys[`F${i + 1}`] = [0x70 + i, 0x3B + i];
	return keys;
})();

// The character a key press types, as WM_CHAR wParam, or 0. AltGr also sets
// ctrlKey on Windows but still types.
export function charCode(event, vk) {
	const typing = !event.metaKey && (!event.ctrlKey || event.getModifierState('AltGraph'));
	if (event.key.length === 1 && typing) {
		const code = event.key.charCodeAt(0);
		return code < 256 ? code : 0;
	}
	return vk === VK_BACK || vk === VK_TAB || vk === VK_RETURN || vk === VK_ESCAPE ? vk : 0; // as user32.c does
}

const keyLParam = (scancode) => KEY_REPEAT | scancode << KEY_SCANCODE_SHIFT;

// Writes one message to the control ring; returns false when it is full.
export function pushMessage(ctl, message, wParam, lParam) {
	const head = Atomics.load(ctl, CTL_RING_HEAD);
	if (((head - Atomics.load(ctl, CTL_RING_TAIL)) | 0) >= CTL_RING_ENTRIES)
		return false;
	const entry = CTL_RING_BASE + (head & (CTL_RING_ENTRIES - 1)) * CTL_RING_ENTRY_WORDS;
	ctl[entry + RING_MSG] = message;
	ctl[entry + RING_WPARAM] = wParam;
	ctl[entry + RING_LPARAM] = lParam;
	Atomics.store(ctl, CTL_RING_HEAD, (head + 1) | 0);
	Atomics.add(ctl, CTL_SEQ, 1);
	Atomics.notify(ctl, CTL_SEQ);
	return true;
}

// Gamepad API state as SDL-style axes (-32768..32767), buttons and hat.
export function padState(pad) {
	const axis = (v) => Math.max(-32768, Math.min(32767, Math.round(v * 32767)));
	const pressed = (i) => !!pad.buttons[i]?.pressed;
	let axes, buttons = 0, hat = 0, numHats = 0;
	if (pad.mapping === 'standard') {
		// Triggers become the fifth and sixth axis, released at -32768.
		const trigger = (i) => Math.round((pad.buttons[i]?.value ?? 0) * 65535) - 32768;
		axes = [axis(pad.axes[0] ?? 0), axis(pad.axes[1] ?? 0), axis(pad.axes[2] ?? 0), axis(pad.axes[3] ?? 0), trigger(6), trigger(7)];
		hat = (pressed(12) ? HAT_UP : 0) | (pressed(13) ? HAT_DOWN : 0) | (pressed(14) ? HAT_LEFT : 0) | (pressed(15) ? HAT_RIGHT : 0);
		numHats = 1;
	} else {
		axes = pad.axes.slice(0, PAD_MAX_AXES).map(axis);
	}
	const numButtons = Math.min(pad.buttons.length, PAD_BUTTON_COUNT);
	for (let i = 0; i < numButtons; ++i)
		buttons |= pressed(i) ? 1 << i : 0;
	return { axes, buttons, hat, numHats, numButtons };
}

export class Input {
	// keepAspect(): whether the picture is letterboxed, for mapping the cursor;
	// onFullscreen(): Alt+Enter.
	constructor(canvas, ctl, { keepAspect, onFullscreen }) {
		this.canvas = canvas;
		this.ctl = ctl;
		this.keepAspect = keepAspect;
		this.onFullscreen = onFullscreen;
		this.enabled = false;
		this.down = new Map(); // code -> [vk, scancode] of keys held
		this.unsent = new Map(); // code -> [vk, scancode] of releases not in the ring yet
		addEventListener('keydown', (event) => this.key(event, true));
		addEventListener('keyup', (event) => this.key(event, false));
		addEventListener('blur', () => this.releaseKeys());
		canvas.addEventListener('pointermove', (event) => this.pointer(event));
		canvas.addEventListener('pointerdown', (event) => {
			this.pointer(event);
			if (event.button === 0 && this.enabled)
				Atomics.or(ctl, CTL_MOUSE_BUTTONS, MOUSE_LEFT | MOUSE_LEFT_LATCH);
		});
		addEventListener('pointerup', (event) => {
			if (event.button === 0)
				Atomics.and(ctl, CTL_MOUSE_BUTTONS, ~MOUSE_LEFT);
		});
		canvas.addEventListener('contextmenu', (event) => event.preventDefault());
	}

	// Input only reaches the game while it runs and no page dialog is open.
	setEnabled(enabled) {
		if (!enabled)
			this.releaseKeys();
		this.enabled = enabled;
	}

	key(event, down) {
		if (!this.enabled)
			return;
		if (down && event.code === 'Enter' && event.altKey) {
			event.preventDefault();
			if (!event.repeat)
				this.onFullscreen();
			return;
		}
		const key = KEYMAP[event.code];
		if (!key)
			return;
		if (!BROWSER_KEYS.has(event.code))
			event.preventDefault();
		const [vk, scancode] = key, lParam = keyLParam(scancode);
		if (!down) {
			if (this.down.delete(event.code))
				this.release(event.code, key);
			return;
		}
		this.down.set(event.code, key);
		this.unsent.delete(event.code); // pressed again before the game saw the release
		pushMessage(this.ctl, WM_KEYDOWN, vk, lParam);
		const char = charCode(event, vk);
		if (char)
			pushMessage(this.ctl, WM_CHAR, char, lParam);
	}

	// Sends WM_KEYUP; a release the full ring refuses waits for poll().
	release(code, key) {
		this.unsent.set(code, key);
		this.sendReleases();
	}

	sendReleases() {
		for (const [code, [vk, scancode]] of this.unsent) {
			if (!pushMessage(this.ctl, WM_KEYUP, vk, keyLParam(scancode)))
				return;
			this.unsent.delete(code);
		}
	}

	releaseKeys() {
		for (const [code, key] of this.down)
			this.release(code, key);
		this.down.clear();
		Atomics.and(this.ctl, CTL_MOUSE_BUTTONS, ~MOUSE_LEFT);
	}

	pointer(event) {
		if (!this.enabled)
			return;
		const rect = this.canvas.getBoundingClientRect();
		const viewport = fitViewport(rect.width, rect.height, this.keepAspect());
		const x = Math.floor((event.clientX - rect.left - viewport.x) * GAME_WIDTH / viewport.width);
		const y = Math.floor((event.clientY - rect.top - viewport.y) * GAME_HEIGHT / viewport.height);
		Atomics.store(this.ctl, CTL_MOUSE_X, Math.max(0, Math.min(GAME_WIDTH - 1, x)));
		Atomics.store(this.ctl, CTL_MOUSE_Y, Math.max(0, Math.min(GAME_HEIGHT - 1, y)));
		Atomics.add(this.ctl, CTL_MOUSE_MOVES, 1);
	}

	// Sends the releases the ring refused and publishes the first CTL_PAD_COUNT
	// gamepads; call once per animation frame.
	poll() {
		this.sendReleases();
		const pads = navigator.getGamepads ? [...navigator.getGamepads()].filter((pad) => pad?.connected) : [];
		for (let i = 0; i < CTL_PAD_COUNT; ++i) {
			const base = CTL_PAD_BASE + i * CTL_PAD_STRIDE, pad = this.enabled ? pads[i] : undefined;
			if (!pad) {
				Atomics.store(this.ctl, base + PAD_CONNECTED, 0);
				continue;
			}
			const state = padState(pad);
			state.axes.forEach((value, axis) => Atomics.store(this.ctl, base + PAD_AXES + axis, value));
			Atomics.store(this.ctl, base + PAD_NUM_AXES, state.axes.length);
			Atomics.store(this.ctl, base + PAD_BUTTONS, state.buttons);
			Atomics.store(this.ctl, base + PAD_NUM_BUTTONS, state.numButtons);
			Atomics.store(this.ctl, base + PAD_HAT, state.hat);
			Atomics.store(this.ctl, base + PAD_NUM_HATS, state.numHats);
			Atomics.store(this.ctl, base + PAD_CONNECTED, 1);
		}
	}
}

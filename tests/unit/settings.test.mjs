// Page settings: parsing stored JSON, localStorage access and the form binding.
import assert from 'node:assert/strict';
import { afterEach, test } from 'node:test';
import { bindSettingsForm, DEFAULT_SETTINGS, loadSettings, parseSettings, saveSettings } from '../../src/web/settings.js';

const STORAGE_KEY = 'nfs2se-settings';

function setStorage(storage) {
	Object.defineProperty(globalThis, 'localStorage', { value: storage, configurable: true, writable: true });
}

function memoryStorage(initial = {}) {
	const items = new Map(Object.entries(initial));
	return {
		items,
		getItem: (key) => items.get(key) ?? null,
		setItem: (key, value) => items.set(key, String(value)),
	};
}

afterEach(() => {
	delete globalThis.localStorage;
});

test('the defaults are frozen and parsing returns fresh copies', () => {
	assert.ok(Object.isFrozen(DEFAULT_SETTINGS));
	const settings = parseSettings(null);
	assert.deepEqual(settings, DEFAULT_SETTINGS);
	settings.bloom = false;
	assert.equal(DEFAULT_SETTINGS.bloom, true);
	assert.notEqual(parseSettings(null), parseSettings(null));
});

test('parseSettings falls back to the defaults for unusable text', () => {
	for (const text of [null, '', '{bad', '"text"', '42', 'null', '[1, 2]'])
		assert.deepEqual(parseSettings(text), DEFAULT_SETTINGS, String(text));
});

test('parseSettings takes stored values of the right type and range', () => {
	const settings = parseSettings(JSON.stringify({
		resolution: 'half', antialias: false, bloom: 'no', sharpen: 0, volume: 0.25, showFps: true, extra: 1,
	}));
	assert.deepEqual(settings, { ...DEFAULT_SETTINGS, resolution: 'half', antialias: false, volume: 0.25, showFps: true });
	assert.equal(parseSettings('{"resolution": "4k"}').resolution, 'native');
	assert.equal(parseSettings('{"resolution": "original"}').resolution, 'original');
	assert.equal(parseSettings('{"volume": 1.5}').volume, 1);
	assert.equal(parseSettings('{"volume": -0.1}').volume, 1);
	assert.equal(parseSettings('{"volume": 0}').volume, 0);
	assert.equal(parseSettings('{"volume": "0.5"}').volume, 1);
});

test('loadSettings and saveSettings use localStorage when it works', () => {
	const storage = memoryStorage({ [STORAGE_KEY]: '{"bloom": false}' });
	setStorage(storage);
	const settings = loadSettings();
	assert.equal(settings.bloom, false);
	settings.volume = 0.5;
	saveSettings(settings);
	assert.deepEqual(JSON.parse(storage.items.get(STORAGE_KEY)), settings);
});

test('settings still work without localStorage or when it throws', () => {
	assert.deepEqual(loadSettings(), DEFAULT_SETTINGS);
	assert.doesNotThrow(() => saveSettings({ ...DEFAULT_SETTINGS }));
	const denied = () => { throw new Error('SecurityError'); };
	setStorage({ getItem: denied, setItem: denied });
	assert.deepEqual(loadSettings(), DEFAULT_SETTINGS);
	assert.ok(!Object.isFrozen(loadSettings()));
	assert.doesNotThrow(() => saveSettings({ ...DEFAULT_SETTINGS }));
});

test('bindSettingsForm shows the settings and stores every change', () => {
	const storage = memoryStorage();
	setStorage(storage);
	const controls = {
		bloom: { name: 'bloom', type: 'checkbox', checked: false },
		resolution: { name: 'resolution', type: 'select-one', value: '' },
		volume: { name: 'volume', type: 'range', value: '' },
		other: { name: 'other', type: 'text', value: '' },
	};
	let onInput = null;
	const form = {
		elements: { namedItem: (name) => controls[name] ?? null },
		addEventListener: (type, listener) => { assert.equal(type, 'input'); onInput = listener; },
	};
	const settings = { ...DEFAULT_SETTINGS }, changes = [];
	bindSettingsForm(form, settings, (key) => changes.push(key));
	assert.equal(controls.bloom.checked, true);
	assert.equal(controls.resolution.value, 'native');
	assert.equal(controls.volume.value, '1');

	controls.bloom.checked = false;
	onInput({ target: controls.bloom });
	controls.volume.value = '0.25';
	onInput({ target: controls.volume });
	controls.resolution.value = 'half';
	onInput({ target: controls.resolution });
	onInput({ target: controls.other });
	assert.deepEqual(changes, ['bloom', 'volume', 'resolution']);
	assert.equal(settings.bloom, false);
	assert.equal(settings.volume, 0.25);
	assert.equal(settings.resolution, 'half');
	assert.ok(!Object.hasOwn(settings, 'other'));
	assert.deepEqual(JSON.parse(storage.items.get(STORAGE_KEY)), settings);
});

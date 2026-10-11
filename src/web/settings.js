// Page settings, kept in localStorage. The renderer and audio read the live
// object, so changes apply immediately.

const STORAGE_KEY = 'nfs2se-settings';
const RESOLUTIONS = ['native', 'original', 'half'];

export const DEFAULT_SETTINGS = Object.freeze({
	resolution: 'native', // scene size: 'native' display pixels, 'original' 640x480, 'half' of native
	antialias: true,
	bloom: true,
	sharpen: true,
	enhancedColors: true,
	smoothTextures: true,
	keepAspect: true,
	showFps: false,
	volume: 1,
});

// Defaults, overridden by stored values of the right type and range.
export function parseSettings(text) {
	const settings = { ...DEFAULT_SETTINGS };
	let stored;
	try {
		stored = JSON.parse(text);
	} catch {
		return settings;
	}
	if (!stored || typeof stored !== 'object')
		return settings;
	for (const key of Object.keys(settings)) {
		if (typeof stored[key] === typeof settings[key])
			settings[key] = stored[key];
	}
	if (!RESOLUTIONS.includes(settings.resolution))
		settings.resolution = DEFAULT_SETTINGS.resolution;
	if (!(settings.volume >= 0 && settings.volume <= 1))
		settings.volume = DEFAULT_SETTINGS.volume;
	return settings;
}

// localStorage throws when storage is disabled; settings then last one visit.
export function loadSettings() {
	try {
		return parseSettings(localStorage.getItem(STORAGE_KEY));
	} catch {
		return { ...DEFAULT_SETTINGS };
	}
}

export function saveSettings(settings) {
	try {
		localStorage.setItem(STORAGE_KEY, JSON.stringify(settings));
	} catch {
		// Not persisted.
	}
}

// Shows settings in a form whose controls are named after the keys, and
// stores every change; onChange(key) runs after it.
export function bindSettingsForm(form, settings, onChange) {
	for (const [key, value] of Object.entries(settings)) {
		const control = form.elements.namedItem(key);
		if (control?.type === 'checkbox')
			control.checked = value;
		else if (control)
			control.value = String(value);
	}
	form.addEventListener('input', (event) => {
		const control = event.target, key = control.name;
		if (!Object.hasOwn(settings, key))
			return;
		if (control.type === 'checkbox')
			settings[key] = control.checked;
		else
			settings[key] = typeof settings[key] === 'number' ? Number(control.value) : control.value;
		saveSettings(settings);
		onChange(key);
	});
}

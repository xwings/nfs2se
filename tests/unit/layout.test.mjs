// src/web/layout.js must mirror every enum constant of src/platform/web_layout.h.
import assert from 'node:assert/strict';
import { readFileSync } from 'node:fs';
import { test } from 'node:test';
import * as layout from '../../src/web/layout.js';

// Evaluates the header's enum entries in order; values may use decimal or hex
// integers, earlier names, + - * << and parentheses.
function headerConstants() {
	const source = readFileSync(new URL('../../src/platform/web_layout.h', import.meta.url), 'utf8')
		.replace(/\/\*[\s\S]*?\*\//g, '');
	const constants = new Map();
	for (const [, body] of source.matchAll(/enum\s*\{([^}]*)\}/g)) {
		for (const item of body.split(',').map((text) => text.trim()).filter(Boolean)) {
			const [, name, expression] = item.match(/^([A-Z][A-Z0-9_]*)\s*=\s*(.+)$/) ?? [];
			assert.ok(name, `cannot parse enum entry "${item}"`);
			const arithmetic = expression.replace(/0x[0-9A-F]+|[A-Z][A-Z0-9_]*/g, (token) => {
				if (token.startsWith('0x'))
					return String(Number(token));
				assert.ok(constants.has(token), `${name} uses ${token} before it is defined`);
				return `(${constants.get(token)})`;
			});
			assert.match(arithmetic, /^[\d\s()+\-*<]+$/, `unexpected expression for ${name}`);
			constants.set(name, Function(`"use strict"; return (${arithmetic});`)());
		}
	}
	return constants;
}

test('every header constant has the same value in layout.js', () => {
	const constants = headerConstants();
	assert.equal(constants.size, Object.keys(layout).length);
	for (const [name, value] of constants)
		assert.equal(layout[name], value, name);
});

test('layout.js exports nothing the header lacks', () => {
	const constants = headerConstants();
	for (const name of Object.keys(layout))
		assert.ok(constants.has(name), `${name} is not in web_layout.h`);
});

test('derived sizes keep the slot and ring arithmetic aligned', () => {
	assert.equal(layout.GFX_SLOT_BYTES % 4, 0);
	assert.equal(layout.GFX_VTX_OFFSET % 4, 0);
	assert.equal(layout.CTL_RING_ENTRIES & (layout.CTL_RING_ENTRIES - 1), 0);
	assert.equal(layout.AUD_CAPACITY_FRAMES & (layout.AUD_CAPACITY_FRAMES - 1), 0);
	assert.ok(layout.CTL_PAD_BASE + layout.CTL_PAD_COUNT * layout.CTL_PAD_STRIDE <= layout.CTL_RING_BASE);
	assert.ok(layout.PAD_AXES + layout.PAD_MAX_AXES <= layout.CTL_PAD_STRIDE);
});

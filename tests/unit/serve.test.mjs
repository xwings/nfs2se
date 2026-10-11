// tools/serve.mjs: files below the root with the isolation headers, and
// failures that answer the request instead of stopping the server.
import assert from 'node:assert/strict';
import { spawn } from 'node:child_process';
import { once } from 'node:events';
import { chmod, mkdir, mkdtemp, rm, symlink, writeFile } from 'node:fs/promises';
import { tmpdir } from 'node:os';
import { join } from 'node:path';
import { after, before, test } from 'node:test';
import { fileURLToPath } from 'node:url';

let root, server, base;

before(async () => {
	root = await mkdtemp(join(tmpdir(), 'nfs2se-serve-'));
	await mkdir(join(root, 'files'));
	await writeFile(join(root, 'index.html'), '<!doctype html>\n');
	await writeFile(join(root, 'nfs2se.wasm'), new Uint8Array([0, 0x61, 0x73, 0x6D]));
	await writeFile(join(root, 'files', 'text.eng'), 'dummy text\n');
	await writeFile(join(root, 'locked.js'), '\n');
	await chmod(join(root, 'locked.js'), 0);
	await symlink('loop.js', join(root, 'loop.js'));
	server = spawn(process.execPath, [fileURLToPath(new URL('../../tools/serve.mjs', import.meta.url)), root, '0'],
		{ stdio: ['ignore', 'pipe', 'ignore'] });
	const [line] = await once(server.stdout, 'data');
	base = String(line).match(/http:\/\/localhost:\d+\//)[0];
});

after(async () => {
	server.kill();
	await rm(root, { recursive: true, force: true });
});

test('files are served with their type and the isolation headers', async () => {
	for (const [path, type, body] of [
		['', 'text/html; charset=utf-8', '<!doctype html>\n'],
		['files/text.eng', 'application/octet-stream', 'dummy text\n'],
	]) {
		const response = await fetch(base + path);
		assert.equal(response.status, 200, path);
		assert.equal(response.headers.get('content-type'), type, path);
		assert.equal(response.headers.get('cross-origin-opener-policy'), 'same-origin');
		assert.equal(response.headers.get('cross-origin-embedder-policy'), 'require-corp');
		assert.equal(await response.text(), body);
	}
	const wasm = await fetch(`${base}nfs2se.wasm`, { method: 'HEAD' });
	assert.equal(wasm.headers.get('content-type'), 'application/wasm');
	assert.equal(wasm.headers.get('content-length'), '4');
	assert.equal((await fetch(base, { method: 'POST' })).status, 405);
});

test('missing files and paths outside the root are not found', async () => {
	for (const path of ['missing.js', 'index.html/x', 'files/', '%2e%2e/package.json', '%zz'])
		assert.equal((await fetch(base + path)).status, 404, path);
});

test('files that cannot be read are a server error and the server keeps running', async () => {
	assert.equal((await fetch(`${base}loop.js`)).status, 500);
	if (process.getuid?.() !== 0) // root reads any file
		assert.equal((await fetch(`${base}locked.js`)).status, 500);
	assert.equal((await fetch(base)).status, 200);
});

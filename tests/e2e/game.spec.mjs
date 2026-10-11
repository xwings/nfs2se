// A whole session of the WebAssembly game with generated dummy game files: it
// starts, stops at the missing intro movie with its OK/Cancel abort box (shown
// by the page), waits for the answer, exits, and its first-run settings file
// is kept in IndexedDB. Also how starting, saving and the display fail.
import { expect } from '@playwright/test';
import { importGameFolder, test } from './helpers.mjs';

const CONF = '/home/web_user/.nfs2se/nfs2se.conf'; // created from files/nfs2se.conf.template

// Whether IDBFS (database = mount point) holds CONF with the template's bytes.
function confSaved(page) {
	return page.evaluate(async (path) => {
		const result = (request) => new Promise((resolve, reject) => {
			request.onsuccess = () => resolve(request.result);
			request.onerror = () => reject(request.error);
		});
		const db = await result(indexedDB.open('/home/web_user/.nfs2se'));
		let entry;
		try {
			entry = db.objectStoreNames.contains('FILE_DATA') && await result(db.transaction('FILE_DATA').objectStore('FILE_DATA').get(path));
		} finally {
			db.close();
		}
		if (!entry?.contents)
			return false;
		const template = new Uint8Array(await (await fetch('/files/nfs2se.conf.template')).arrayBuffer());
		return entry.contents.length === template.length && entry.contents.every((byte, i) => byte === template[i]);
	}, CONF);
}

// Plays until the game shows its message box about the missing movie.
async function playToMessageBox(page, testInfo) {
	await importGameFolder(page, testInfo);
	await page.locator('#play').click();
	await expect(page.locator('body')).toHaveClass(/\bplaying\b/);
	const box = page.locator('#message-box');
	await expect(box).toBeVisible({ timeout: 60_000 });
	return box;
}

test('the game runs, waits for its message box and ends', async ({ page }, testInfo) => {
	test.setTimeout(120_000);
	const box = await playToMessageBox(page, testInfo);
	await expect(page.locator('#message-box-text')).toHaveText('MOVIE FILE NOT FOUND');
	await expect(page.locator('#message-box-cancel')).toBeVisible();
	await expect(page.locator('#ended')).toBeHidden();
	await box.locator('button[value=ok]').click();

	await expect(page.locator('#ended-text')).toHaveText('The game has quit.');
	await expect(page.locator('#ended')).toBeVisible();
	await expect(page.locator('body')).not.toHaveClass(/\bplaying\b/);
	await expect.poll(() => confSaved(page)).toBe(true);
	await expect(page.locator('#ended-warning')).toBeHidden();
});

// The module and the canvas are set up once per page, so only a reload can
// try again.
test('a support file that cannot be loaded stops the start with the reason', async ({ page }, testInfo) => {
	await page.route('**/files/nfs2se.conf.template', (route) => route.fulfill({ status: 404 }));
	await importGameFolder(page, testInfo);
	await page.locator('#play').click();
	await expect(page.locator('#ended-text')).toHaveText('The game could not start: Could not load files/nfs2se.conf.template (HTTP 404).');
	await expect(page.locator('#ended')).toBeVisible();
	await expect(page.locator('#play')).toBeDisabled();
	await expect(page.locator('body')).not.toHaveClass(/\bplaying\b/);
});

test('a failed save is shown when the game ends and retried until it works', async ({ page }, testInfo) => {
	test.setTimeout(120_000);
	await importGameFolder(page, testInfo);
	await page.evaluate(() => {
		const put = IDBObjectStore.prototype.put;
		IDBObjectStore.prototype.put = () => { throw new Error('put failed'); };
		window.restorePut = () => { IDBObjectStore.prototype.put = put; };
	});
	await page.locator('#play').click();
	const box = page.locator('#message-box');
	await expect(box).toBeVisible({ timeout: 60_000 });
	await box.locator('button[value=ok]').click();

	await expect(page.locator('#ended-text')).toHaveText('The game has quit.');
	await expect(page.locator('#ended-warning')).toHaveText('Saved games and settings could not be stored: put failed');
	await expect(page.locator('#ended-warning')).toBeVisible();
	expect(await confSaved(page)).toBe(false);
	await page.evaluate(() => window.restorePut());
	await expect(page.locator('#ended-warning')).toBeHidden();
	await expect.poll(() => confSaved(page)).toBe(true);
});

test('a display that cannot be rebuilt after a context loss ends the game', async ({ page }, testInfo) => {
	test.setTimeout(120_000);
	await playToMessageBox(page, testInfo);
	await page.evaluate(async () => {
		const canvas = document.getElementById('screen');
		const gl = canvas.getContext('webgl2'); // the renderer's context
		const extension = gl.getExtension('WEBGL_lose_context');
		gl.createProgram = () => { throw new Error('createProgram failed'); };
		// Restoring is only allowed once the webglcontextlost dispatch has finished.
		await new Promise((resolve) => {
			canvas.addEventListener('webglcontextlost', () => setTimeout(resolve), { once: true });
			extension.loseContext();
		});
		extension.restoreContext();
	});
	await expect(page.locator('#ended-text')).toHaveText('The game display failed: createProgram failed');
	await expect(page.locator('body')).not.toHaveClass(/\bplaying\b/);
});

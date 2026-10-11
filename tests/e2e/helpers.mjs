// Shared pieces of the browser tests. Game files are generated dummy bytes in
// the layout of an installed game, never real game data.
import { mkdir, writeFile } from 'node:fs/promises';
import { dirname, join } from 'node:path';
import { expect, test as base } from '@playwright/test';

// path -> text; selectGameFiles stores them in lower case below the root.
export const GAME_FILES = {
	'NFS2SE/GAMEDATA/CONFIG/DUMMY.DAT': 'dummy config\n',
	'NFS2SE/FEDATA/PC/ART/DUMMY.QFS': 'dummy art\n',
};

export const STORED_GAME_FILES = {
	'fedata/pc/art/dummy.qfs': 'dummy art\n',
	'gamedata/config/dummy.dat': 'dummy config\n',
};

const ISOLATION_HEADERS = {
	'Cross-Origin-Opener-Policy': 'same-origin',
	'Cross-Origin-Embedder-Policy': 'require-corp',
};

// Every test fails on uncaught page errors.
export const test = base.extend({
	page: async ({ page }, use) => {
		const errors = [];
		page.on('pageerror', (error) => errors.push(error.message));
		await use(page);
		expect(errors, 'uncaught page errors').toEqual([]);
	},
});

// Writes files ({path: text}) below root and returns root.
export async function writeTree(root, files) {
	for (const [path, text] of Object.entries(files)) {
		const file = join(root, path);
		await mkdir(dirname(file), { recursive: true });
		await writeFile(file, text);
	}
	return root;
}

// Opens the start page of a browser profile without game files.
export async function openLauncher(page) {
	await page.goto('/');
	await expect(page.locator('#files-status')).toHaveText('No game files imported yet.');
}

// Opens the start page and imports GAME_FILES through the folder picker.
export async function importGameFolder(page, testInfo) {
	await openLauncher(page);
	await page.setInputFiles('#pick-folder', await writeTree(testInfo.outputPath('game'), GAME_FILES));
	await expect(page.locator('#message')).toHaveText('Imported 2 files. Press Play to start.');
}

// {path: text} of the game files stored in IndexedDB (src/web/storage.js).
export function storedFiles(page) {
	return page.evaluate(async () => {
		const result = (request) => new Promise((resolve, reject) => {
			request.onsuccess = () => resolve(request.result);
			request.onerror = () => reject(request.error);
		});
		const db = await result(indexedDB.open('nfs2se'));
		let paths, blobs;
		try {
			const store = db.transaction('files').objectStore('files');
			[paths, blobs] = await Promise.all([result(store.getAllKeys()), result(store.getAll())]);
		} finally {
			db.close();
		}
		const texts = await Promise.all(blobs.map((blob) => blob.text()));
		return Object.fromEntries(paths.map((path, i) => [path, texts[i]]));
	});
}

// An empty cross-origin isolated page of the test server's origin, for tests
// that import the page modules directly instead of running main.js.
export async function openBlankPage(page) {
	await page.route('**/blank.html', (route) => route.fulfill({
		contentType: 'text/html; charset=utf-8',
		headers: ISOLATION_HEADERS,
		body: '<!doctype html><html><head><title>test</title></head><body></body></html>',
	}));
	await page.goto('/blank.html');
	expect(await page.evaluate(() => crossOriginIsolated)).toBe(true);
}

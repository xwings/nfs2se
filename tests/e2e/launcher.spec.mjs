// The start page: isolation, importing the game files from a folder, a ZIP
// archive or a drop, keeping and removing them, and the settings dialog.
import { expect } from '@playwright/test';
import { ascii, buildZip } from '../support/zip.mjs';
import { GAME_FILES, importGameFolder, openLauncher, STORED_GAME_FILES, storedFiles, test, writeTree } from './helpers.mjs';

// GAME_FILES as a ZIP archive whose second entry is deflated, with the
// directory values in declared listed for that entry instead of the true ones.
const gameZip = (declared) => buildZip(Object.entries(GAME_FILES).map(([name, text], i) => ({ name, data: ascii(text), method: i ? 8 : 0, declared: i ? declared : undefined })));

test('the page is cross-origin isolated and waits for game files', async ({ page }) => {
	await openLauncher(page);
	expect(await page.evaluate(() => crossOriginIsolated)).toBe(true);
	await expect(page.locator('#play')).toBeDisabled();
	await expect(page.locator('#remove-files')).toBeHidden();
	await expect(page.locator('#message')).toBeEmpty();
});

test('a folder without the game data is rejected', async ({ page }, testInfo) => {
	await openLauncher(page);
	await page.setInputFiles('#pick-folder', await writeTree(testInfo.outputPath('other'), { 'Documents/readme.txt': 'not a game\n' }));
	await expect(page.locator('#message')).toHaveText(/^Could not find the GAMEDATA and FEDATA folders\./);
	await expect(page.locator('#message')).toHaveClass(/\berror\b/);
	await expect(page.locator('#files-status')).toHaveText('No game files imported yet.');
	await expect(page.locator('#play')).toBeDisabled();
});

test('imported game files are kept across visits until removed', async ({ page }, testInfo) => {
	await importGameFolder(page, testInfo);
	await expect(page.locator('#files-status')).toHaveText('Imported: 2 files (0 MB).');
	await expect(page.locator('#play')).toBeEnabled();
	expect(await storedFiles(page)).toEqual(STORED_GAME_FILES);

	await page.reload();
	await expect(page.locator('#files-status')).toHaveText('Imported: 2 files (0 MB).');
	await expect(page.locator('#play')).toBeEnabled();

	page.once('dialog', (dialog) => dialog.dismiss());
	await page.locator('#remove-files').click();
	await expect(page.locator('#files-status')).toHaveText('Imported: 2 files (0 MB).');

	page.once('dialog', (dialog) => dialog.accept());
	await page.locator('#remove-files').click();
	await expect(page.locator('#message')).toHaveText('The game files were removed.');
	await expect(page.locator('#files-status')).toHaveText('No game files imported yet.');
	await expect(page.locator('#play')).toBeDisabled();
	await expect(page.locator('#remove-files')).toBeHidden();
	expect(await storedFiles(page)).toEqual({});
});

test('a ZIP archive is imported through the picker', async ({ page }) => {
	await openLauncher(page);
	await page.setInputFiles('#pick-archive', { name: 'game.zip', mimeType: 'application/zip', buffer: Buffer.from(gameZip()) });
	await expect(page.locator('#message')).toHaveText('Imported 2 files. Press Play to start.');
	expect(await storedFiles(page)).toEqual(STORED_GAME_FILES);
});

test('a ZIP archive that fails partway leaves no game files stored', async ({ page }) => {
	await openLauncher(page);
	// The second entry inflates to one byte less than listed, which shows only
	// after the first file is stored.
	const zip = gameZip({ size: Object.values(GAME_FILES)[1].length + 1 });
	await page.setInputFiles('#pick-archive', { name: 'game.zip', mimeType: 'application/zip', buffer: Buffer.from(zip) });
	await expect(page.locator('#message')).toHaveText('game.zip: damaged ZIP entry.');
	await expect(page.locator('#files-status')).toHaveText('No game files imported yet.');
	await expect(page.locator('#play')).toBeDisabled();
	expect(await storedFiles(page)).toEqual({});
});

test('a full disk is reported even when removing the partial import fails', async ({ page }) => {
	await openLauncher(page);
	const warnings = [];
	page.on('console', (message) => {
		if (message.text().startsWith('Could not remove the files of a failed import'))
			warnings.push(message.type());
	});
	await page.evaluate(() => {
		const clear = IDBObjectStore.prototype.clear;
		IDBObjectStore.prototype.put = () => { throw new DOMException('No room', 'QuotaExceededError'); };
		// An import clears the store before it starts and again after it fails.
		let clears = 0;
		IDBObjectStore.prototype.clear = function () {
			if (++clears === 2)
				throw new Error('clear failed');
			return clear.call(this);
		};
	});
	await page.setInputFiles('#pick-archive', { name: 'game.zip', mimeType: 'application/zip', buffer: Buffer.from(gameZip()) });
	await expect(page.locator('#message')).toHaveText(/^The browser has no room left for the game files\./);
	await expect(page.locator('#files-status')).toHaveText('No game files imported yet.');
	expect(warnings).toEqual(['warning']);
});

test('a ZIP archive dropped on the page is imported', async ({ page }) => {
	await openLauncher(page);
	const dataTransfer = await page.evaluateHandle((bytes) => {
		const transfer = new DataTransfer();
		transfer.items.add(new File([new Uint8Array(bytes)], 'game.zip'));
		return transfer;
	}, [...gameZip()]);
	await page.dispatchEvent('#drop-zone', 'drop', { dataTransfer });
	await expect(page.locator('#message')).toHaveText('Imported 2 files. Press Play to start.');
	expect(await storedFiles(page)).toEqual(STORED_GAME_FILES);
});

test('settings are saved and restored on the next visit', async ({ page }) => {
	await page.goto('/');
	const form = page.locator('#settings-form');
	await page.locator('#settings-open').click();
	await expect(page.locator('#settings-dialog')).toBeVisible();
	await expect(form.locator('[name=bloom]')).toBeChecked();
	await expect(form.locator('[name=showFps]')).not.toBeChecked();
	await expect(form.locator('[name=resolution]')).toHaveValue('native');
	await expect(form.locator('[name=volume]')).toHaveValue('1');

	await form.locator('[name=bloom]').uncheck();
	await form.locator('[name=showFps]').check();
	await form.locator('[name=resolution]').selectOption('original');
	await form.locator('[name=volume]').fill('0.25');
	await expect(page.locator('#fps')).not.toHaveAttribute('hidden');
	await page.locator('#settings-dialog button[value=close]').click();
	await expect(page.locator('#settings-dialog')).toBeHidden();

	await page.reload();
	await page.locator('#settings-open').click();
	await expect(form.locator('[name=bloom]')).not.toBeChecked();
	await expect(form.locator('[name=showFps]')).toBeChecked();
	await expect(form.locator('[name=resolution]')).toHaveValue('original');
	await expect(form.locator('[name=volume]')).toHaveValue('0.25');
	await expect(form.locator('[name=sharpen]')).toBeChecked();
	expect(JSON.parse(await page.evaluate(() => localStorage.getItem('nfs2se-settings'))))
		.toMatchObject({ bloom: false, showFps: true, resolution: 'original', volume: 0.25, sharpen: true });
});

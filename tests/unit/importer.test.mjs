// Game-file import from folders, ZIP archives and CD images. Every archive
// and image here is generated from dummy bytes.
import assert from 'node:assert/strict';
import { test } from 'node:test';
import {
	archiveEntries, cdImage, droppedEntries, folderEntries, isoEntries, selectGameFiles, zipEntries,
} from '../../src/web/importer.js';
import { ascii, buildZip, concat } from '../support/zip.mjs';

const SECTOR = 2048;
const CD_SYNC = [0x00, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0x00];

function pattern(length, seed) {
	return Uint8Array.from({ length }, (_, i) => (i * 31 + seed) & 0xFF);
}

async function bytesOf(entry) {
	return new Uint8Array(await (await entry.blob()).arrayBuffer());
}

function summary(entries) {
	return entries.map(({ path, size }) => [path, size]);
}

function zipFile(entries, options) {
	return new File([buildZip(entries, options)], 'game.zip');
}

// Builds an archive, then lets change(view, end) damage it; end is the
// offset of the end record, which has no comment here.
function patchedZip(entries, change) {
	const bytes = buildZip(entries);
	change(new DataView(bytes.buffer), bytes.length - 22);
	return new File([bytes], 'game.zip');
}

function bothEndian32(view, at, value) {
	view.setUint32(at, value, true);
	view.setUint32(at + 4, value, false);
}

function directoryRecord(name, extent, size, isDirectory) {
	const nameBytes = typeof name === 'string' ? ascii(name) : Uint8Array.of(name);
	const record = new Uint8Array(33 + nameBytes.length + (nameBytes.length % 2 ? 0 : 1)), view = new DataView(record.buffer);
	record[0] = record.length;
	bothEndian32(view, 2, extent);
	bothEndian32(view, 10, size);
	record[25] = isDirectory ? 2 : 0;
	view.setUint16(28, 1, true);
	view.setUint16(30, 1, false);
	record[32] = nameBytes.length;
	record.set(nameBytes, 33);
	return record;
}

const TRACK_DATA = pattern(5000, 7);
const SMALL_DATA = ascii('abc');

// A cooked image of the given number of sectors whose volume descriptors
// name the root directory.
function isoVolume(sectors, root, rootSize) {
	const image = new Uint8Array(sectors * SECTOR);
	image.set(concat([Uint8Array.of(1), ascii('CD001'), Uint8Array.of(1)]), 16 * SECTOR);
	image.set(directoryRecord(0, root, rootSize, true), 16 * SECTOR + 156);
	image.set(concat([Uint8Array.of(255), ascii('CD001'), Uint8Array.of(1)]), 17 * SECTOR);
	return image;
}

// A cooked ISO 9660 image: the root directory spans two sectors (the second
// starts after padding) and holds a directory record that points back at
// the root; TRACK.DAT spans three sectors.
function buildIso() {
	const ROOT = 18, GAMEDATA = 20, TRACK = 21, FEDATA = 24, PC = 25, SMALL = 26;
	const image = isoVolume(27, ROOT, 2 * SECTOR);
	const directory = (lba, parent, records) => image.set(concat([
		directoryRecord(0, lba, SECTOR, true), directoryRecord(1, parent, SECTOR, true), ...records,
	]), lba * SECTOR);
	directory(ROOT, ROOT, [directoryRecord('GAMEDATA', GAMEDATA, SECTOR, true), directoryRecord('LOOP', ROOT, 2 * SECTOR, true)]);
	image.set(concat([directoryRecord('FEDATA', FEDATA, SECTOR, true), directoryRecord('README.TXT;1', SMALL, 3, false)]), (ROOT + 1) * SECTOR);
	directory(GAMEDATA, ROOT, [directoryRecord('TRACK.DAT;1', TRACK, TRACK_DATA.length, false)]);
	directory(FEDATA, ROOT, [directoryRecord('PC', PC, SECTOR, true)]);
	directory(PC, FEDATA, [directoryRecord('FILE.TXT;1', SMALL, 3, false), directoryRecord('NOEXT.;1', SMALL, 2, false)]);
	image.set(TRACK_DATA, TRACK * SECTOR);
	image.set(SMALL_DATA, SMALL * SECTOR);
	return image;
}

// Raw sectors: sync, header, (mode 2 form 1 subheader,) data, then EDC/ECC
// and optional subchannel bytes, filled with noise the reader must skip.
function toRaw(cooked, mode, subchannel) {
	const sectorBytes = 2352 + (subchannel ? 96 : 0), dataOffset = mode === 2 ? 24 : 16;
	const raw = new Uint8Array(cooked.length / SECTOR * sectorBytes).fill(0xA5);
	for (let sector = 0; sector < cooked.length / SECTOR; ++sector) {
		const at = sector * sectorBytes;
		raw.set(CD_SYNC, at);
		raw.set([0, 2, sector], at + 12);
		raw[at + 15] = mode;
		if (mode === 2)
			raw.set([0, 0, 8, 0, 0, 0, 8, 0], at + 16);
		raw.set(cooked.subarray(sector * SECTOR, (sector + 1) * SECTOR), at + dataOffset);
	}
	return raw;
}

const ISO_FILES = [['GAMEDATA/TRACK.DAT', 5000], ['FEDATA/PC/FILE.TXT', 3], ['FEDATA/PC/NOEXT', 2], ['README.TXT', 3]];

async function checkIsoEntries(entries) {
	assert.deepEqual(summary(entries), ISO_FILES);
	assert.deepEqual(await bytesOf(entries[0]), TRACK_DATA);
	assert.deepEqual(await bytesOf(entries[1]), SMALL_DATA);
	assert.deepEqual(await bytesOf(entries[2]), SMALL_DATA.subarray(0, 2));
}

async function openIso(bytes) {
	const file = new File([bytes], 'game.iso');
	return isoEntries(file, await cdImage(file));
}

// buildIso() after change(view, at) edits the directory record with the given name.
function patchedIso(name, change) {
	const image = buildIso();
	change(new DataView(image.buffer), Buffer.from(image.buffer).indexOf(name) - 33);
	return image;
}

// A root directory that lists count empty files, 60 records per sector.
function crowdedIso(count) {
	const sectors = Math.ceil(count / 60), image = isoVolume(18 + sectors, 18, sectors * SECTOR);
	for (let i = 0; i < count; ++i)
		image.set(directoryRecord('F', 0, 0, false), (18 + Math.floor(i / 60)) * SECTOR + (i % 60) * 34);
	return image;
}

// A root directory with levels folders nested below it, one per sector.
function nestedIso(levels) {
	const image = isoVolume(19 + levels, 18, SECTOR);
	for (let level = 0; level < levels; ++level)
		image.set(directoryRecord('D', 19 + level, SECTOR, true), (18 + level) * SECTOR);
	return image;
}

test('zipEntries lists stored and deflated files and skips directories', async () => {
	const deflated = Uint8Array.from({ length: 20000 }, (_, i) => i % 7);
	const file = zipFile([
		{ name: 'NFS2SE/' },
		{ name: 'NFS2SE/GameData/X.DAT', data: pattern(3000, 1) },
		{ name: 'NFS2SE\\FeData\\PC\\Y.DAT', data: deflated, method: 8, localExtra: pattern(9, 3) },
		{ name: 'NFS2SE/empty.txt' },
	], { comment: ascii('archive comment') });
	const entries = await zipEntries(file);
	assert.deepEqual(summary(entries), [['NFS2SE/GameData/X.DAT', 3000], ['NFS2SE/FeData/PC/Y.DAT', 20000], ['NFS2SE/empty.txt', 0]]);
	assert.deepEqual(await bytesOf(entries[0]), pattern(3000, 1));
	assert.deepEqual(await bytesOf(entries[1]), deflated);
	assert.deepEqual(await bytesOf(entries[2]), new Uint8Array());
});

test('zipEntries decodes UTF-8 names when flagged and code page 1252 otherwise', async () => {
	const entries = await zipEntries(zipFile([
		{ name: 'fedata/pc/Grüße.txt', flags: 0x800, data: ascii('1') },
		{ name: concat([ascii('fedata/pc/caf'), Uint8Array.of(0xE9), ascii('.txt')]), data: ascii('2') },
	]));
	assert.deepEqual(entries.map((entry) => entry.path), ['fedata/pc/Grüße.txt', 'fedata/pc/café.txt']);
});

test('zipEntries reads ZIP64 directories and extra fields', async () => {
	const entries = await zipEntries(zipFile([
		{ name: 'gamedata/a.dat', data: pattern(700, 2), method: 8, zip64: ['size', 'compressed', 'offset'] },
		{ name: 'gamedata/b.dat', data: pattern(300, 4), zip64: ['offset'] },
	], { zip64: true }));
	assert.deepEqual(summary(entries), [['gamedata/a.dat', 700], ['gamedata/b.dat', 300]]);
	assert.deepEqual(await bytesOf(entries[0]), pattern(700, 2));
	assert.deepEqual(await bytesOf(entries[1]), pattern(300, 4));
});

test('zipEntries explains damaged, oversized, encrypted and unsupported archives', async () => {
	const data = pattern(10, 0);
	const damagedDirectory = { message: 'game.zip: damaged ZIP directory.' }, damagedEntry = { message: 'game.zip: damaged ZIP entry.' };
	await assert.rejects(zipEntries(new File([data], 'game.zip')), { message: 'game.zip: the ZIP directory is missing.' });
	await assert.rejects(zipEntries(zipFile([{ name: 'a', data }], { zip64: true, locator: false })),
		{ message: 'game.zip: damaged ZIP64 directory.' });
	const misplaced = buildZip([{ name: 'a', data }]);
	new DataView(misplaced.buffer).setUint32(misplaced.length - 6, 0, true); // directory offset -> local header
	await assert.rejects(zipEntries(new File([misplaced], 'game.zip')), damagedDirectory);
	await assert.rejects(zipEntries(zipFile([{ name: 'secret.dat', data, flags: 1 }])),
		{ message: 'game.zip: secret.dat is encrypted.' });
	await assert.rejects(zipEntries(zipFile([{ name: 'packed.dat', data, method: 12 }])),
		{ message: 'game.zip: packed.dat uses an unsupported compression method (12).' });
	const broken = buildZip([{ name: 'a', data }]);
	broken[0] = 0;
	const [entry] = await zipEntries(new File([broken], 'game.zip'));
	await assert.rejects(entry.blob(), damagedEntry);

	// The end record holds the entry count at +10, the directory size at +12
	// and its offset at +16. A ZIP64 field ends the directory record here.
	const single = [{ name: 'a', data }];
	const record = (view, end) => view.getUint32(end + 16, true);
	// A name or extra field past the end of the directory, and a ZIP64 field
	// too short for the size it replaces.
	await assert.rejects(zipEntries(patchedZip(single, (view, end) => view.setUint16(record(view, end) + 28, 0xFFFF, true))), damagedDirectory);
	await assert.rejects(zipEntries(patchedZip(single, (view, end) => view.setUint16(record(view, end) + 30, 0xFFFF, true))), damagedDirectory);
	await assert.rejects(zipEntries(patchedZip([{ name: 'a', data, zip64: ['size'] }], (view, end) => view.setUint16(end - 10, 4, true))),
		damagedDirectory);
	// A directory that runs into the end record; data that two entries share,
	// that runs into the directory, or whose stored length differs.
	await assert.rejects(zipEntries(patchedZip(single, (view, end) => view.setUint32(end + 12, view.getUint32(end + 12, true) + 1, true))),
		damagedDirectory);
	await assert.rejects(zipEntries(zipFile([{ name: 'a', data }, { name: 'b', data, declared: { offset: 0 } }])), damagedDirectory);
	await assert.rejects(zipEntries(zipFile([{ name: 'a', data, method: 8, declared: { compressed: 1000 } }])), damagedDirectory);
	await assert.rejects(zipEntries(zipFile([{ name: 'a', data, declared: { size: 11 } }])), damagedDirectory);
	// A local header that pushes the data into the directory, and inflated
	// data longer or shorter than listed, or cut off.
	const [shifted] = await zipEntries(patchedZip(single, (view) => view.setUint16(28, 1, true)));
	await assert.rejects(shifted.blob(), damagedEntry);
	for (const declared of [{ size: 9 }, { size: 11 }, { compressed: 2 }]) {
		const [inflated] = await zipEntries(zipFile([{ name: 'a', data, method: 8, declared }]));
		await assert.rejects(inflated.blob(), damagedEntry);
	}
	// More entries or directory bytes than the limits allow.
	const tooLarge = { message: 'game.zip holds too many files and folders for a copy of the game.' };
	await assert.rejects(zipEntries(patchedZip(single, (view, end) => view.setUint16(end + 10, 20001, true))), tooLarge);
	await assert.rejects(zipEntries(patchedZip(single, (view, end) => view.setUint32(end + 12, 0x7FFFFFFF, true))), tooLarge);
});

test('cdImage recognises cooked and raw images and rejects other files', async () => {
	const cooked = buildIso();
	assert.deepEqual(await cdImage(new File([cooked], 'a.iso')), { sectorBytes: 2048, dataOffset: 0 });
	assert.deepEqual(await cdImage(new File([toRaw(cooked, 1, false)], 'a.bin')), { sectorBytes: 2352, dataOffset: 16 });
	assert.deepEqual(await cdImage(new File([toRaw(cooked, 2, false)], 'a.bin')), { sectorBytes: 2352, dataOffset: 24 });
	assert.deepEqual(await cdImage(new File([toRaw(cooked, 1, true)], 'a.img')), { sectorBytes: 2448, dataOffset: 16 });
	assert.equal(await cdImage(new File([buildZip([{ name: 'a' }])], 'a.zip')), null);
	assert.equal(await cdImage(new File([pattern(100000, 9)], 'noise.bin')), null);
	assert.equal(await cdImage(new File([], 'empty.iso')), null);
});

test('isoEntries walks the directory tree of cooked and raw images', async () => {
	const cooked = buildIso();
	for (const bytes of [cooked, toRaw(cooked, 1, false), toRaw(cooked, 2, false), toRaw(cooked, 1, true)]) {
		const file = new File([bytes], 'game.bin');
		await checkIsoEntries(await isoEntries(file, await cdImage(file)));
	}
});

test('isoEntries explains damaged and oversized images', async () => {
	const damaged = { message: 'game.iso: damaged CD image.' };
	const tooLarge = { message: 'game.iso holds too many files and folders for a copy of the game.' };
	const extent = (lba) => (view, at) => bothEndian32(view, at + 2, lba);
	const size = (bytes) => (view, at) => bothEndian32(view, at + 10, bytes);
	// An image cut off inside its volume descriptor.
	await assert.rejects(openIso(buildIso().subarray(0, 16 * SECTOR + 6)), damaged);
	// Files and folders outside the image, also in raw sectors.
	await assert.rejects(openIso(patchedIso('TRACK.DAT;1', extent(1000))), damaged);
	await assert.rejects(openIso(toRaw(patchedIso('README.TXT;1', size(0xFFFFFFFF)), 1, false)), damaged);
	await assert.rejects(openIso(patchedIso('FEDATA', extent(1000))), damaged);
	// Records shorter than their fixed part, past the end of their folder, or
	// with a name past their end.
	await assert.rejects(openIso(patchedIso('TRACK.DAT;1', (view, at) => view.setUint8(at, 20))), damaged);
	await assert.rejects(openIso(patchedIso('GAMEDATA', size(70))), damaged);
	await assert.rejects(openIso(patchedIso('README.TXT;1', (view, at) => view.setUint8(at + 32, 100))), damaged);
	// The limits on folder bytes, entries and nesting.
	await assert.rejects(openIso(patchedIso('GAMEDATA', size(0x7FFFFFFF))), tooLarge);
	assert.equal((await openIso(crowdedIso(20000))).length, 20000);
	await assert.rejects(openIso(crowdedIso(20001)), tooLarge);
	assert.deepEqual(await openIso(nestedIso(32)), []);
	await assert.rejects(openIso(nestedIso(33)), tooLarge);
});

test('archiveEntries opens CD images and ZIP archives and names other files', async () => {
	await checkIsoEntries(await archiveEntries(new File([buildIso()], 'game.iso')));
	assert.deepEqual(summary(await archiveEntries(zipFile([{ name: 'a.txt', data: ascii('hi') }]))), [['a.txt', 2]]);
	await assert.rejects(archiveEntries(new File([pattern(5000, 1)], 'notes.txt')),
		{ message: 'notes.txt is not a ZIP archive or a CD image (ISO/BIN).' });
	await assert.rejects(archiveEntries(new File([], 'empty.zip')), { message: /empty\.zip is not a ZIP archive/ });
});

test('folderEntries uses the path inside the chosen folder', async () => {
	const nested = new File([ascii('abcd')], 'X.DAT');
	Object.defineProperty(nested, 'webkitRelativePath', { value: 'NFS2SE/GameData/X.DAT' });
	const loose = new File([ascii('z')], 'loose.txt');
	const entries = folderEntries([nested, loose]);
	assert.deepEqual(summary(entries), [['NFS2SE/GameData/X.DAT', 4], ['loose.txt', 1]]);
	assert.equal(await entries[0].blob(), nested);
});

function fileEntry(name, bytes) {
	const file = new File([bytes], name);
	return { isFile: true, name, file: (resolve) => setTimeout(() => resolve(file)) };
}

// Returns its children two at a time, like browsers return large folders in batches.
function directoryEntry(name, children) {
	return {
		isFile: false, name,
		createReader() {
			let next = 0;
			return {
				readEntries(resolve) {
					const batch = children.slice(next, next + 2);
					next += batch.length;
					setTimeout(() => resolve(batch));
				},
			};
		},
	};
}

test('droppedEntries walks dropped folders and opens a single dropped archive', async () => {
	const tree = directoryEntry('NFS2SE', [
		directoryEntry('GameData', [fileEntry('A.DAT', pattern(5, 0))]),
		directoryEntry('FeData', [directoryEntry('PC', [fileEntry('B', ascii('b')), fileEntry('C', ascii('cc')), fileEntry('D', ascii('ddd'))])]),
		fileEntry('readme.txt', ascii('r')),
	]);
	const entries = await droppedEntries([tree]);
	assert.deepEqual(summary(entries), [
		['NFS2SE/GameData/A.DAT', 5], ['NFS2SE/FeData/PC/B', 1], ['NFS2SE/FeData/PC/C', 2], ['NFS2SE/FeData/PC/D', 3],
		['NFS2SE/readme.txt', 1],
	]);
	assert.deepEqual(await bytesOf(entries[3]), ascii('ddd'));

	const two = await droppedEntries([directoryEntry('gamedata', [fileEntry('x', ascii('x'))]), fileEntry('install.win', ascii('w'))]);
	assert.deepEqual(summary(two), [['gamedata/x', 1], ['install.win', 1]]);

	const archive = await droppedEntries([fileEntry('game.zip', buildZip([{ name: 'gamedata/y', data: ascii('yy') }]))]);
	assert.deepEqual(summary(archive), [['gamedata/y', 2]]);
});

const entry = (path, size = 1) => ({ path, size, blob: async () => new Blob([pattern(size, 0)]) });

test('selectGameFiles keeps the game trees under the shallowest root, in lower case', () => {
	const files = selectGameFiles([
		entry('Old/Backup/GameData/old.dat'), entry('Old/Backup/FeData/PC/old.dat'),
		entry('NFS2SE/GameData/Config/X.DAT', 3), entry('NFS2SE/readme.txt'), entry('NFS2SE/FeData/PC/Art/Y.QFS', 4),
		entry('\\NFS2SE\\GameData\\Track\\Z.TRK', 2),
	]);
	assert.deepEqual(summary(files), [['gamedata/config/x.dat', 3], ['fedata/pc/art/y.qfs', 4], ['gamedata/track/z.trk', 2]]);
	assert.equal(typeof files[0].blob, 'function');

	const top = selectGameFiles([entry('deep/gamedata/a'), entry('deep/fedata/pc/b'), entry('gamedata/c'), entry('fedata/pc/d')]);
	assert.deepEqual(top.map((file) => file.path), ['gamedata/c', 'fedata/pc/d']);

	// Depth decides, not the length of the folder names.
	const shallow = selectGameFiles([
		entry('x/y/gamedata/a', 1), entry('x/y/fedata/pc/b', 1), entry('needforspeed/gamedata/c', 2), entry('needforspeed/fedata/pc/d', 2),
	]);
	assert.deepEqual(summary(shallow), [['gamedata/c', 2], ['fedata/pc/d', 2]]);
});

test('selectGameFiles drops paths with empty, "." or ".." parts', () => {
	const files = selectGameFiles([
		entry('gamedata/ok'), entry('fedata/pc/ok'), entry('gamedata/../../etc/passwd'), entry('gamedata//x'),
		entry('./gamedata/y'), entry('fedata/pc/./z'), entry('fedata/pc/'),
	]);
	assert.deepEqual(files.map((file) => file.path), ['gamedata/ok', 'fedata/pc/ok']);
});

test('selectGameFiles explains missing, incomplete or oversized game folders', () => {
	const notFound = /^Could not find the GAMEDATA and FEDATA folders/;
	assert.throws(() => selectGameFiles([]), { message: notFound });
	assert.throws(() => selectGameFiles([entry('gamedata/a'), entry('readme.txt')]), { message: notFound });
	assert.throws(() => selectGameFiles([entry('a/gamedata/x'), entry('b/fedata/pc/y')]), { message: notFound });
	assert.throws(() => selectGameFiles([entry('gamedata'), entry('fedata/pc/x')]), { message: notFound });
	assert.throws(() => selectGameFiles([entry('gamedata/x'), entry('fedata/y')]), { message: /^The FEDATA folder has no PC subfolder/ });

	const tooLarge = /^The GAMEDATA and FEDATA folders are too large for this game \(over 20000 files or 1 GB\)\./;
	const many = Array.from({ length: 19999 }, (_, i) => entry(`gamedata/${i}`));
	assert.equal(selectGameFiles([...many, entry('fedata/pc/x')]).length, 20000);
	assert.throws(() => selectGameFiles([...many, entry('fedata/pc/x'), entry('fedata/pc/y')]), { message: tooLarge });
	assert.equal(selectGameFiles([entry('gamedata/a', 2 ** 30 - 1), entry('fedata/pc/b')]).length, 2);
	assert.throws(() => selectGameFiles([entry('gamedata/a', 2 ** 30), entry('fedata/pc/b')]), { message: tooLarge });
});

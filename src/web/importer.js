// Reads the player's own copy of the game from a folder, a ZIP archive or a
// CD image (ISO 9660 in cooked 2048-byte or raw 2352/2448-byte sectors) and
// picks its gamedata/ and fedata/ trees. Everything is read locally with
// Blob.slice, so archives and images are never loaded whole.
//
// An entry is {path, size, blob(): Promise<Blob>}.

const ZIP_END = 0x06054b50;
const ZIP64_LOCATOR = 0x07064b50;
const ZIP64_END = 0x06064b50;
const ZIP_CENTRAL = 0x02014b50;
const ZIP_LOCAL = 0x04034b50;
const ZIP_END_SEARCH = 22 + 0xFFFF; // the end record and its longest comment
const SECTOR = 2048;
const RAW_SECTOR = 2352;
const CD_SYNC = [0x00, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0x00];
const PVD_SECTOR = 16; // the primary volume descriptor
const PVD_ROOT = 156; // the root directory record inside it

// Limits for untrusted archives and images. The game's two trees hold a few
// thousand files and fit on its CD (under 700 MB), so each limit leaves
// several times that room.
const MAX_FILES = 20000; // entries an archive or image lists, and files kept
const MAX_GAME_BYTES = 2 ** 30; // the size of the files kept
const MAX_DIRECTORY_BYTES = 16 << 20; // a ZIP directory, or all directories of an image
const MAX_DEPTH = 32; // folder nesting in an image; ISO 9660 itself allows 8 levels

function damaged(file, part) {
	return new Error(`${file.name}: damaged ${part}.`);
}

function tooLarge(file) {
	return new Error(`${file.name} holds too many files and folders for a copy of the game.`);
}

async function readBytes(blob, start, end) {
	return new Uint8Array(await blob.slice(start, end).arrayBuffer());
}

function view(bytes) {
	return new DataView(bytes.buffer, bytes.byteOffset, bytes.byteLength);
}

function ascii(bytes) {
	return String.fromCharCode(...bytes);
}

function fileEntry(path, file) {
	return { path, size: file.size, blob: async () => file };
}

export function folderEntries(files) {
	return [...files].map((file) => fileEntry(file.webkitRelativePath || file.name, file));
}

// roots: FileSystemEntry objects, taken with webkitGetAsEntry() while the
// drop event is dispatched. A single dropped file is opened as an archive.
export async function droppedEntries(roots) {
	const fileOf = (entry) => new Promise((resolve, reject) => entry.file(resolve, reject));
	if (roots.length === 1 && roots[0].isFile)
		return archiveEntries(await fileOf(roots[0]));
	const entries = [];
	const walk = async (entry, prefix) => {
		if (entry.isFile) {
			entries.push(fileEntry(prefix + entry.name, await fileOf(entry)));
			return;
		}
		const reader = entry.createReader();
		for (;;) {
			const batch = await new Promise((resolve, reject) => reader.readEntries(resolve, reject));
			if (!batch.length)
				break;
			for (const child of batch)
				await walk(child, `${prefix}${entry.name}/`);
		}
	};
	for (const root of roots)
		await walk(root, '');
	return entries;
}

export async function archiveEntries(file) {
	const image = await cdImage(file);
	if (image)
		return isoEntries(file, image);
	const head = await readBytes(file, 0, 4);
	if (head.length === 4 && view(head).getUint32(0, true) === ZIP_LOCAL)
		return zipEntries(file);
	throw new Error(`${file.name} is not a ZIP archive or a CD image (ISO/BIN).`);
}

// ZIP: the central directory lists every entry; ZIP64 fields are honoured.
export async function zipEntries(file) {
	const tailStart = Math.max(0, file.size - ZIP_END_SEARCH);
	const tail = await readBytes(file, tailStart, file.size), tv = view(tail);
	let end = -1;
	for (let i = tail.length - 22; i >= 0 && end < 0; --i)
		end = tv.getUint32(i, true) === ZIP_END ? i : -1;
	if (end < 0)
		throw new Error(`${file.name}: the ZIP directory is missing.`);
	let count = tv.getUint16(end + 10, true), size = tv.getUint32(end + 12, true), offset = tv.getUint32(end + 16, true);
	let directoryEnd = tailStart + end; // the directory ends before the end records
	if (count === 0xFFFF || size === 0xFFFFFFFF || offset === 0xFFFFFFFF) {
		if (end < 20 || tv.getUint32(end - 20, true) !== ZIP64_LOCATOR)
			throw damaged(file, 'ZIP64 directory');
		directoryEnd = Number(tv.getBigUint64(end - 12, true));
		const record = view(await readBytes(file, directoryEnd, directoryEnd + 56));
		if (record.byteLength < 56 || record.getUint32(0, true) !== ZIP64_END)
			throw damaged(file, 'ZIP64 directory');
		count = Number(record.getBigUint64(32, true));
		size = Number(record.getBigUint64(40, true));
		offset = Number(record.getBigUint64(48, true));
	}
	if (count > MAX_FILES || size > MAX_DIRECTORY_BYTES)
		throw tooLarge(file);
	if (offset + size > directoryEnd)
		throw damaged(file, 'ZIP directory');
	const directory = await readBytes(file, offset, offset + size), dv = view(directory);
	const found = [];
	for (let p = 0, i = 0; i < count; ++i) {
		if (p + 46 > directory.length || dv.getUint32(p, true) !== ZIP_CENTRAL)
			throw damaged(file, 'ZIP directory');
		const flags = dv.getUint16(p + 8, true), method = dv.getUint16(p + 10, true);
		let compressed = dv.getUint32(p + 20, true), length = dv.getUint32(p + 24, true), local = dv.getUint32(p + 42, true);
		const nameLength = dv.getUint16(p + 28, true), extraLength = dv.getUint16(p + 30, true);
		const next = p + 46 + nameLength + extraLength + dv.getUint16(p + 32, true);
		if (next > directory.length)
			throw damaged(file, 'ZIP directory');
		const nameBytes = directory.subarray(p + 46, p + 46 + nameLength);
		const name = new TextDecoder(flags & 0x800 ? 'utf-8' : 'windows-1252').decode(nameBytes).replace(/\\/g, '/');
		// The ZIP64 extra field holds the saturated fields, in this order.
		for (let e = p + 46 + nameLength, extraEnd = e + extraLength; e + 4 <= extraEnd; e += 4 + dv.getUint16(e + 2, true)) {
			if (dv.getUint16(e, true) !== 1)
				continue;
			let q = e + 4;
			const fieldEnd = Math.min(q + dv.getUint16(e + 2, true), extraEnd);
			const wide = () => {
				if (q + 8 > fieldEnd)
					throw damaged(file, 'ZIP directory');
				const v = Number(dv.getBigUint64(q, true));
				q += 8;
				return v;
			};
			if (length === 0xFFFFFFFF)
				length = wide();
			if (compressed === 0xFFFFFFFF)
				compressed = wide();
			if (local === 0xFFFFFFFF)
				local = wide();
		}
		p = next;
		if (name.endsWith('/'))
			continue;
		if (flags & 1)
			throw new Error(`${file.name}: ${name} is encrypted.`);
		if (method !== 0 && method !== 8)
			throw new Error(`${file.name}: ${name} uses an unsupported compression method (${method}).`);
		if (method === 0 && compressed !== length)
			throw damaged(file, 'ZIP directory');
		found.push({ path: name, local, compressed, length, method });
	}
	// In file order, each entry's local header (30 bytes or more) and data must
	// end before the next entry or the directory starts, so that no two entries
	// share bytes.
	const byPosition = [...found].sort((a, b) => a.local - b.local);
	for (const [i, entry] of byPosition.entries()) {
		entry.limit = byPosition[i + 1]?.local ?? offset;
		if (entry.local + 30 + entry.compressed > entry.limit)
			throw damaged(file, 'ZIP directory');
	}
	return found.map((entry) => ({ path: entry.path, size: entry.length, blob: () => zipData(file, entry) }));
}

// entry: a listed entry, whose data ends by entry.limit.
async function zipData(file, { local, compressed, length, method, limit }) {
	const header = view(await readBytes(file, local, local + 30));
	if (header.getUint32(0, true) !== ZIP_LOCAL)
		throw damaged(file, 'ZIP entry');
	const start = local + 30 + header.getUint16(26, true) + header.getUint16(28, true);
	if (start + compressed > limit)
		throw damaged(file, 'ZIP entry');
	const data = file.slice(start, start + compressed);
	if (method === 0)
		return data;
	// Inflating stops once the output passes the listed length. A stream that
	// fails or gives any other length means the entry is damaged.
	let left = length;
	const cut = new TransformStream({
		transform(chunk, controller) {
			left -= chunk.length;
			if (left < 0)
				throw damaged(file, 'ZIP entry');
			controller.enqueue(chunk);
		},
	});
	const blob = await new Response(data.stream().pipeThrough(new DecompressionStream('deflate-raw')).pipeThrough(cut)).blob()
		.catch(() => null);
	if (blob?.size !== length)
		throw damaged(file, 'ZIP entry');
	return blob;
}

// Finds the primary volume descriptor (sector 16) of a cooked image or of a
// raw one, with or without the 96 subchannel bytes per sector.
export async function cdImage(file) {
	const isPvd = (bytes, at) => bytes[at] === 1 && ascii(bytes.subarray(at + 1, at + 6)) === 'CD001';
	const cooked = await readBytes(file, PVD_SECTOR * SECTOR, PVD_SECTOR * SECTOR + 6);
	if (cooked.length === 6 && isPvd(cooked, 0))
		return { sectorBytes: SECTOR, dataOffset: 0 };
	for (const sectorBytes of [RAW_SECTOR, RAW_SECTOR + 96]) {
		const raw = await readBytes(file, PVD_SECTOR * sectorBytes, PVD_SECTOR * sectorBytes + 30);
		if (raw.length === 30 && CD_SYNC.every((byte, i) => raw[i] === byte)) {
			const dataOffset = raw[15] === 2 ? 24 : 16; // mode 2 form 1 has an 8-byte subheader
			if (isPvd(raw, dataOffset))
				return { sectorBytes, dataOffset };
		}
	}
	return null;
}

function sectorBlob(file, image, lba, length) {
	if (image.sectorBytes === SECTOR)
		return file.slice(lba * SECTOR, lba * SECTOR + length);
	const parts = [];
	for (let sector = lba, left = length; left > 0; ++sector, left -= SECTOR) {
		const start = sector * image.sectorBytes + image.dataOffset;
		parts.push(file.slice(start, start + Math.min(SECTOR, left)));
	}
	return new Blob(parts);
}

export async function isoEntries(file, image) {
	// Extents must lie inside the image, so that raw slices end there too.
	const sectors = Math.floor(file.size / image.sectorBytes);
	const inside = (lba, length) => lba + Math.ceil(length / SECTOR) <= sectors;
	const read = async (lba, length) => {
		if (!inside(lba, length))
			throw damaged(file, 'CD image');
		return new Uint8Array(await sectorBlob(file, image, lba, length).arrayBuffer());
	};
	const pvd = await read(PVD_SECTOR, SECTOR), pv = view(pvd);
	const entries = [], visited = new Set();
	let listed = 0, directoryBytes = 0;
	const walk = async (lba, length, prefix, depth) => {
		if (visited.has(lba))
			return;
		visited.add(lba);
		directoryBytes += length;
		if (depth > MAX_DEPTH || directoryBytes > MAX_DIRECTORY_BYTES)
			throw tooLarge(file);
		const records = await read(lba, length), rv = view(records);
		for (let p = 0; p < records.length;) {
			const recordLength = records[p];
			if (!recordLength) { // records never cross sectors; the rest is padding
				p = (Math.floor(p / SECTOR) + 1) * SECTOR;
				continue;
			}
			// A record has 33 fixed bytes, then the name.
			if (recordLength < 33 || p + recordLength > records.length || 33 + records[p + 32] > recordLength)
				throw damaged(file, 'CD image');
			const extent = rv.getUint32(p + 2, true), size = rv.getUint32(p + 10, true), flags = records[p + 25];
			const nameBytes = records.subarray(p + 33, p + 33 + records[p + 32]);
			p += recordLength;
			if (nameBytes.length === 1 && nameBytes[0] <= 1)
				continue; // "." and ".."
			if (++listed > MAX_FILES)
				throw tooLarge(file);
			const name = ascii(nameBytes).replace(/;\d*$/, '').replace(/\.$/, '');
			if (flags & 2)
				await walk(extent, size, `${prefix}${name}/`, depth + 1);
			else if (inside(extent, size))
				entries.push({ path: prefix + name, size, blob: async () => sectorBlob(file, image, extent, size) });
			else
				throw damaged(file, 'CD image');
		}
	};
	await walk(pv.getUint32(PVD_ROOT + 2, true), pv.getUint32(PVD_ROOT + 10, true), '', 0);
	return entries;
}

// Keeps the gamedata/ and fedata/ trees under the shallowest directory that
// has both, with lower-case paths relative to it (the game opens lower-case
// paths). Throws with a message for the player when they are missing or
// larger than the limits above. Archive paths with empty, "." or ".." parts
// are dropped.
export function selectGameFiles(entries) {
	const normalized = entries
		.map((entry) => ({ ...entry, path: entry.path.replace(/\\/g, '/').replace(/^\/+/, '').toLowerCase() }))
		.filter(({ path }) => path.split('/').every((part) => part && part !== '.' && part !== '..'));
	const found = new Map(); // root prefix -> 1 gamedata | 2 fedata
	for (const { path } of normalized) {
		const parts = path.split('/');
		const at = parts.findIndex((part, i) => i < parts.length - 1 && (part === 'gamedata' || part === 'fedata'));
		if (at >= 0) {
			const root = parts.slice(0, at).map((part) => `${part}/`).join('');
			found.set(root, (found.get(root) ?? 0) | (parts[at] === 'gamedata' ? 1 : 2));
		}
	}
	const depth = (root) => root.split('/').length;
	const roots = [...found].filter(([, mask]) => mask === 3).map(([root]) => root).sort((a, b) => depth(a) - depth(b));
	if (!roots.length)
		throw new Error('Could not find the GAMEDATA and FEDATA folders. Choose the folder where the game is installed, or its CD image.');
	const root = roots[0];
	const files = normalized
		.filter(({ path }) => path.startsWith(`${root}gamedata/`) || path.startsWith(`${root}fedata/`))
		.map(({ path, size, blob }) => ({ path: path.slice(root.length), size, blob }));
	if (!files.some(({ path }) => path.startsWith('fedata/pc/')))
		throw new Error('The FEDATA folder has no PC subfolder. Use the files of the installed Windows version.');
	if (files.length > MAX_FILES || files.reduce((sum, { size }) => sum + size, 0) > MAX_GAME_BYTES)
		throw new Error(`The GAMEDATA and FEDATA folders are too large for this game (over ${MAX_FILES} files or ${MAX_GAME_BYTES / 2 ** 30} GB). Choose the folder where the game is installed, or its CD image.`);
	return files;
}

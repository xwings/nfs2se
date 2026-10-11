// ZIP archives for the importer tests (unit and browser), built from the
// given bytes. CRC fields stay zero: the importer does not check them.
import { deflateRawSync } from 'node:zlib';

export const ascii = (text) => new TextEncoder().encode(text);

export function concat(parts) {
	const out = new Uint8Array(parts.reduce((sum, part) => sum + part.length, 0));
	let at = 0;
	for (const part of parts) {
		out.set(part, at);
		at += part.length;
	}
	return out;
}

// entries: {name (string or bytes), data, method, flags, localExtra, zip64: fields stored in the ZIP64 extra,
// declared: {size, compressed, offset} that the directory states instead of the true values}
export function buildZip(entries, { zip64 = false, locator = true, comment = new Uint8Array() } = {}) {
	const parts = [], records = [];
	let offset = 0;
	for (const entry of entries) {
		const name = typeof entry.name === 'string' ? ascii(entry.name) : entry.name;
		const data = entry.data ?? new Uint8Array(), method = entry.method ?? 0, flags = entry.flags ?? 0;
		const stored = method === 8 ? new Uint8Array(deflateRawSync(data)) : data;
		const localExtra = entry.localExtra ?? new Uint8Array();
		const local = new Uint8Array(30 + name.length + localExtra.length), lv = new DataView(local.buffer);
		lv.setUint32(0, 0x04034b50, true);
		lv.setUint16(4, 20, true);
		lv.setUint16(6, flags, true);
		lv.setUint16(8, method, true);
		lv.setUint32(18, stored.length, true);
		lv.setUint32(22, data.length, true);
		lv.setUint16(26, name.length, true);
		lv.setUint16(28, localExtra.length, true);
		local.set(name, 30);
		local.set(localExtra, 30 + name.length);
		parts.push(local, stored);

		const wide = entry.zip64 ?? [];
		const values = { size: data.length, compressed: stored.length, offset, ...entry.declared };
		const extra64 = new DataView(new ArrayBuffer(4 + 8 * wide.length));
		extra64.setUint16(0, 1, true);
		extra64.setUint16(2, 8 * wide.length, true);
		['size', 'compressed', 'offset'].filter((field) => wide.includes(field))
			.forEach((field, i) => extra64.setBigUint64(4 + 8 * i, BigInt(values[field]), true));
		// An unrelated extra field first, which the reader must skip.
		const extra = wide.length ? concat([Uint8Array.of(0x55, 0x54, 5, 0, 1, 2, 3, 4, 5), new Uint8Array(extra64.buffer)]) : new Uint8Array();
		const saturate = (field) => (wide.includes(field) ? 0xFFFFFFFF : values[field]);
		const record = new Uint8Array(46 + name.length + extra.length), rv = new DataView(record.buffer);
		rv.setUint32(0, 0x02014b50, true);
		rv.setUint16(4, 20, true);
		rv.setUint16(6, 20, true);
		rv.setUint16(8, flags, true);
		rv.setUint16(10, method, true);
		rv.setUint32(20, saturate('compressed'), true);
		rv.setUint32(24, saturate('size'), true);
		rv.setUint16(28, name.length, true);
		rv.setUint16(30, extra.length, true);
		rv.setUint32(42, saturate('offset'), true);
		record.set(name, 46);
		record.set(extra, 46 + name.length);
		records.push(record);
		offset += local.length + stored.length;
	}
	const directory = concat(records), directoryOffset = offset;
	parts.push(directory);
	const end = new Uint8Array(22), ev = new DataView(end.buffer);
	ev.setUint32(0, 0x06054b50, true);
	if (zip64) {
		const record = new DataView(new ArrayBuffer(56));
		record.setUint32(0, 0x06064b50, true);
		record.setBigUint64(4, 44n, true);
		record.setBigUint64(24, BigInt(entries.length), true);
		record.setBigUint64(32, BigInt(entries.length), true);
		record.setBigUint64(40, BigInt(directory.length), true);
		record.setBigUint64(48, BigInt(directoryOffset), true);
		parts.push(new Uint8Array(record.buffer));
		if (locator) {
			const locatorView = new DataView(new ArrayBuffer(20));
			locatorView.setUint32(0, 0x07064b50, true);
			locatorView.setBigUint64(8, BigInt(directoryOffset + directory.length), true);
			locatorView.setUint32(16, 1, true);
			parts.push(new Uint8Array(locatorView.buffer));
		}
		ev.setUint16(8, 0xFFFF, true);
		ev.setUint16(10, 0xFFFF, true);
		ev.setUint32(12, 0xFFFFFFFF, true);
		ev.setUint32(16, 0xFFFFFFFF, true);
	} else {
		ev.setUint16(8, entries.length, true);
		ev.setUint16(10, entries.length, true);
		ev.setUint32(12, directory.length, true);
		ev.setUint32(16, directoryOffset, true);
	}
	ev.setUint16(20, comment.length, true);
	parts.push(end, comment);
	return concat(parts);
}

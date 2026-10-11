// Keeps the imported game files in IndexedDB so that they are imported once.
// The "meta" record is written last and deleted first, so a partial import
// never counts as installed.

const DB_NAME = 'nfs2se';
const DB_VERSION = 1;
const FILES = 'files'; // path -> Blob
const META = 'meta';
const GAME_KEY = 'game'; // {count, bytes, imported}

function done(request) {
	return new Promise((resolve, reject) => {
		request.onsuccess = () => resolve(request.result);
		request.onerror = () => reject(request.error);
	});
}

function committed(transaction) {
	return new Promise((resolve, reject) => {
		transaction.oncomplete = () => resolve();
		transaction.onerror = transaction.onabort = () => reject(transaction.error ?? new Error('IndexedDB transaction aborted'));
	});
}

export async function openStorage() {
	const request = indexedDB.open(DB_NAME, DB_VERSION);
	request.onupgradeneeded = () => {
		request.result.createObjectStore(FILES);
		request.result.createObjectStore(META);
	};
	return done(request);
}

// {count, bytes, imported} of a complete import, or null.
export async function gameFilesInfo(db) {
	return (await done(db.transaction(META).objectStore(META).get(GAME_KEY))) ?? null;
}

export async function clearGameFiles(db) {
	const transaction = db.transaction([META, FILES], 'readwrite');
	transaction.objectStore(META).delete(GAME_KEY);
	transaction.objectStore(FILES).clear();
	await committed(transaction);
}

// files: importer entries. onProgress(index, count, path) runs before each file.
// A failed import removes the files it stored, then rethrows its own error.
export async function storeGameFiles(db, files, onProgress) {
	await clearGameFiles(db);
	try {
		let bytes = 0;
		for (const [index, file] of files.entries()) {
			onProgress(index, files.length, file.path);
			const blob = await file.blob();
			const transaction = db.transaction(FILES, 'readwrite');
			transaction.objectStore(FILES).put(blob, file.path);
			await committed(transaction);
			bytes += blob.size;
		}
		const info = { count: files.length, bytes, imported: Date.now() };
		const transaction = db.transaction(META, 'readwrite');
		transaction.objectStore(META).put(info, GAME_KEY);
		await committed(transaction);
		return info;
	} catch (error) {
		await clearGameFiles(db).catch((clearError) => console.warn('Could not remove the files of a failed import', clearError));
		throw error;
	}
}

// Calls onFile(path, bytes, index, count) for every stored file.
export async function loadGameFiles(db, onFile) {
	const transaction = db.transaction(FILES);
	const store = transaction.objectStore(FILES);
	const [paths, blobs] = await Promise.all([done(store.getAllKeys()), done(store.getAll())]);
	for (const [index, path] of paths.entries())
		onFile(path, new Uint8Array(await blobs[index].arrayBuffer()), index, paths.length);
}

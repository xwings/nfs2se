// Page controller: cross-origin isolation, game-file import, settings and the
// game session (module, renderer, input, audio, save persistence).
import createNfs2se from './nfs2se.js';
import { createAudioContext, startAudio } from './audio.js';
import { archiveEntries, droppedEntries, folderEntries, selectGameFiles } from './importer.js';
import { Input } from './input.js';
import {
	CTL_DIALOG, CTL_FS_DIRTY, CTL_WORDS, DIALOG_CANCEL, DIALOG_OK, SHARED_AUDIO, SHARED_CTL, SHARED_GFX0, SHARED_GFX1,
} from './layout.js';
import { Renderer, slotViews } from './renderer.js';
import { bindSettingsForm, loadSettings } from './settings.js';
import { clearGameFiles, gameFilesInfo, loadGameFiles, openStorage, storeGameFiles } from './storage.js';

const GAME_DIR = '/game'; // the game's working directory, passed to main()
const SAVE_DIR = '/home/web_user/.nfs2se'; // settings and saves, persisted with IDBFS and passed to main()
const COI_RELOAD_KEY = 'nfs2se-coi-reload';
const SYNC_INTERVAL_MS = 1000;
const TOOLBAR_HIDE_MS = 2500;
const FPS_WINDOW_MS = 500; // the frame rate shown is averaged over this long

const $ = (id) => document.getElementById(id);
const canvas = $('screen');
const settings = loadSettings();
let db = null;
let modulePromise = null;
let session = null; // {module, ctl, renderer, input, audio, running, syncing} once the game starts
let busy = false;
let toolbarTimer = 0;

function showMessage(text, isError = false) {
	$('message').textContent = text;
	$('message').classList.toggle('error', isError);
}

// Emscripten's FS.ErrnoError has no message, only an errno.
function errorText(error) {
	return error?.name === 'ErrnoError' ? `file system error ${error.errno}` : error?.message ?? String(error);
}

// For failures in event handlers, which have no caller to report to.
const reportError = (error) => showMessage(errorText(error), true);

function showProgress(label, fraction) {
	$('progress').hidden = label === null;
	$('progress-label').textContent = label ?? '';
	const bar = $('progress-bar');
	if (fraction === undefined)
		bar.removeAttribute('value');
	else
		bar.value = fraction;
}

function formatBytes(bytes) {
	return bytes >= 1 << 30 ? `${(bytes / (1 << 30)).toFixed(1)} GB` : `${Math.round(bytes / (1 << 20))} MB`;
}

function setBusy(value) {
	busy = value;
	for (const id of ['play', 'remove-files', 'pick-folder', 'pick-archive'])
		$(id).disabled = value;
}

async function refreshFiles() {
	const info = await gameFilesInfo(db);
	$('files-status').textContent = info
		? `Imported: ${info.count} files (${formatBytes(info.bytes)}).`
		: 'No game files imported yet.';
	$('files').classList.toggle('ready', !!info);
	$('play').disabled = busy || !info;
	$('remove-files').hidden = !info;
}

// Static hosts cannot send COOP/COEP, so coi-sw.js adds them; one reload
// puts the page under its control.
async function isolate() {
	if (!('serviceWorker' in navigator) || sessionStorage.getItem(COI_RELOAD_KEY))
		return false;
	try {
		await navigator.serviceWorker.register(new URL('./coi-sw.js', import.meta.url));
		await navigator.serviceWorker.ready;
	} catch (error) {
		console.warn('Service worker registration failed', error);
		return false;
	}
	sessionStorage.setItem(COI_RELOAD_KEY, '1');
	location.reload();
	return true;
}

// Getting entries happens while the progress is shown; errors explain what to fix.
async function importFrom(getEntries) {
	if (busy || session)
		return;
	setBusy(true);
	showMessage('');
	showProgress('Reading the game files…');
	try {
		const files = selectGameFiles(await getEntries());
		// Best effort: unpersisted files may be evicted under storage pressure,
		// and a later import restores them.
		navigator.storage?.persist?.().catch(() => {});
		const info = await storeGameFiles(db, files, (index, count, path) => showProgress(`Importing ${path}`, index / count));
		showMessage(`Imported ${info.count} files. Press Play to start.`);
	} catch (error) {
		showMessage(error?.name === 'QuotaExceededError'
			? 'The browser has no room left for the game files. Free some disk space or allow this site more storage.'
			: errorText(error), true);
	} finally {
		showProgress(null);
		setBusy(false);
		await refreshFiles().catch(reportError);
	}
}

function syncfs(FS, populate) {
	return new Promise((resolve, reject) => FS.syncfs(populate, (error) => (error ? reject(error) : resolve())));
}

// A file shipped in files/ next to this page.
async function fetchFile(name) {
	const response = await fetch(new URL(`files/${name}`, import.meta.url));
	if (!response.ok)
		throw new Error(`Could not load files/${name} (HTTP ${response.status}).`);
	return new Uint8Array(await response.arrayBuffer());
}

// Writes the support files (listed in files/manifest.txt by the Makefile) and
// the imported data into /game and mounts the persisted settings and saves
// directory.
async function populateFiles(module) {
	const FS = module.FS;
	FS.mkdir(GAME_DIR);
	const names = new TextDecoder().decode(await fetchFile('manifest.txt')).split('\n').filter(Boolean);
	await Promise.all(names.map(async (name) => FS.writeFile(`${GAME_DIR}/${name}`, await fetchFile(name))));
	await loadGameFiles(db, (path, bytes, index, count) => {
		const target = `${GAME_DIR}/${path}`;
		FS.mkdirTree(target.slice(0, target.lastIndexOf('/')));
		FS.writeFile(target, bytes, { canOwn: true });
		showProgress('Loading the game files…', (index + 1) / count);
	});
	FS.mkdirTree(SAVE_DIR);
	FS.mount(FS.filesystems.IDBFS, {}, SAVE_DIR);
	await syncfs(FS, true);
}

// Stores files the game wrote (C sets CTL_FS_DIRTY) in IndexedDB. A failed
// save leaves them dirty for the next call and is reported once the game ends.
function persist() {
	const s = session;
	if (!s || s.syncing || !Atomics.load(s.ctl, CTL_FS_DIRTY))
		return;
	Atomics.store(s.ctl, CTL_FS_DIRTY, 0);
	s.syncing = true;
	syncfs(s.module.FS, false)
		.then(() => showSaveError(null), (error) => {
			console.warn('Saving the game settings failed', error);
			Atomics.store(s.ctl, CTL_FS_DIRTY, 1);
			showSaveError(error);
		})
		.finally(() => { s.syncing = false; });
}

// The warning shown with the end-of-game text; null hides it.
function showSaveError(error) {
	$('ended-warning').hidden = !error;
	$('ended-warning').textContent = error ? `Saved games and settings could not be stored: ${errorText(error)}` : '';
}

function updateInputEnabled() {
	session?.input.setEnabled(session.running && !$('settings-dialog').open && !$('message-box').open);
}

function focusGame() {
	if (session?.running)
		canvas.focus({ preventScroll: true });
}

// Module.nfsMessageBox: called on this thread by web_message_box(), whose game
// thread waits until CTL_DIALOG holds the answer.
function messageBox(text, caption, okCancel) {
	const dialog = $('message-box');
	$('message-box-caption').textContent = caption;
	$('message-box-text').textContent = text;
	$('message-box-cancel').hidden = !okCancel;
	dialog.returnValue = '';
	dialog.addEventListener('close', () => {
		const answer = dialog.returnValue === 'ok' || (!okCancel && dialog.returnValue === '') ? DIALOG_OK : DIALOG_CANCEL;
		if (session) {
			Atomics.store(session.ctl, CTL_DIALOG, answer);
			Atomics.notify(session.ctl, CTL_DIALOG);
		}
		updateInputEnabled();
		focusGame();
	}, { once: true });
	dialog.showModal();
	updateInputEnabled();
}

function endSession(text) {
	if (session?.running) {
		session.running = false;
		updateInputEnabled();
		persist();
		session.audio?.setPaused(true);
	}
	if (document.fullscreenElement)
		document.exitFullscreen().catch(() => {}); // rejects only when fullscreen has ended already
	document.body.classList.remove('playing');
	$('ended-text').textContent = text;
	$('ended').hidden = false;
}

async function toggleFullscreen() {
	try {
		if (document.fullscreenElement) {
			await document.exitFullscreen();
			return;
		}
		await document.documentElement.requestFullscreen({ navigationUI: 'hide' });
		// Lets Escape reach the game; holding it still leaves fullscreen.
		await navigator.keyboard?.lock?.(['Escape']);
	} catch (error) {
		console.warn('Fullscreen failed', error);
	}
}

function showToolbar() {
	$('toolbar').classList.add('visible');
	clearTimeout(toolbarTimer);
	toolbarTimer = setTimeout(() => $('toolbar').classList.remove('visible'), TOOLBAR_HIDE_MS);
}

function observeCanvasSize(renderer) {
	const observer = new ResizeObserver(([entry]) => {
		const box = entry.devicePixelContentBoxSize?.[0];
		if (box)
			renderer.resize(box.inlineSize, box.blockSize);
		else
			renderer.resize(entry.contentRect.width * devicePixelRatio, entry.contentRect.height * devicePixelRatio);
	});
	try {
		observer.observe(canvas, { box: 'device-pixel-content-box' });
	} catch {
		observer.observe(canvas);
	}
}

function runFrames() {
	let frames = 0, since = performance.now();
	const frame = (now) => {
		if (!session.running)
			return;
		session.input.poll();
		try {
			if (session.renderer.pump())
				++frames;
		} catch (error) {
			console.error(error);
			endSession(`The game display failed: ${errorText(error)}`);
			return;
		}
		if (now - since >= FPS_WINDOW_MS) {
			$('fps').textContent = `${Math.round(frames * 1000 / (now - since))} fps`;
			frames = 0;
			since = now;
		}
		requestAnimationFrame(frame);
	};
	requestAnimationFrame(frame);
}

async function play() {
	if (busy || session)
		return;
	setBusy(true);
	showMessage('');
	let context = null;
	try {
		context = createAudioContext(); // inside the click, before any await
	} catch (error) {
		console.warn('Audio is unavailable', error);
	}
	try {
		showProgress('Starting the game…');
		const module = await modulePromise;
		const buffer = module.HEAPU8.buffer, address = (which) => module._web_shared(which);
		const ctl = new Int32Array(buffer, address(SHARED_CTL), CTL_WORDS);
		const renderer = new Renderer(canvas, [slotViews(buffer, address(SHARED_GFX0)), slotViews(buffer, address(SHARED_GFX1))], ctl, settings);
		if (!renderer.available)
			throw new Error('This browser could not create a WebGL2 context, which the game needs. Enable hardware acceleration or try another browser.');
		await populateFiles(module);
		let audio = null;
		if (context) {
			try {
				audio = await startAudio(context, buffer, address(SHARED_AUDIO), settings.volume);
			} catch (error) {
				console.warn('Audio is unavailable', error);
				context.close();
				context = null;
			}
		}
		const input = new Input(canvas, ctl, { keepAspect: () => renderer.keepAspect(), onFullscreen: toggleFullscreen });
		session = { module, ctl, renderer, input, audio, running: true, syncing: false };
		document.body.classList.add('playing');
		observeCanvasSize(renderer);
		updateInputEnabled();
		focusGame();
		runFrames();
		setInterval(persist, SYNC_INTERVAL_MS); // also after the game ends, to retry a failed save
		module.callMain([GAME_DIR, SAVE_DIR]);
		setBusy(false);
	} catch (error) {
		// The module, the canvas's WebGL context and /game are set up once per
		// page, so only a reload tries again; the buttons stay disabled.
		context?.close();
		endSession(`The game could not start: ${errorText(error)}`);
	} finally {
		showProgress(null);
	}
}

function bindPage() {
	bindSettingsForm($('settings-form'), settings, (key) => {
		if (key === 'volume')
			session?.audio?.setVolume(settings.volume);
		$('fps').hidden = !settings.showFps;
		session?.renderer.invalidate();
	});
	$('fps').hidden = !settings.showFps;
	const openSettings = () => {
		$('settings-dialog').showModal();
		updateInputEnabled();
	};
	$('settings-open').addEventListener('click', openSettings);
	$('toolbar-settings').addEventListener('click', openSettings);
	$('toolbar-fullscreen').addEventListener('click', toggleFullscreen);
	$('settings-dialog').addEventListener('close', () => {
		updateInputEnabled();
		focusGame();
	});
	$('play').addEventListener('click', play);
	$('reload').addEventListener('click', () => location.reload());
	$('remove-files').addEventListener('click', async () => {
		if (busy || session || !confirm('Remove the imported game files from this browser? Saved games and settings are kept.'))
			return;
		try {
			await clearGameFiles(db);
			showMessage('The game files were removed.');
			await refreshFiles();
		} catch (error) {
			reportError(error);
		}
	});
	for (const id of ['pick-folder', 'pick-archive']) {
		$(id).addEventListener('change', (event) => {
			const files = [...event.target.files];
			event.target.value = '';
			if (files.length)
				importFrom(async () => (id === 'pick-folder' ? folderEntries(files) : archiveEntries(files[0])));
		});
	}
	addEventListener('dragover', (event) => {
		event.preventDefault();
		$('drop-zone').classList.toggle('dragging', !session);
	});
	addEventListener('dragleave', (event) => {
		if (!event.relatedTarget)
			$('drop-zone').classList.remove('dragging');
	});
	addEventListener('drop', (event) => {
		event.preventDefault();
		$('drop-zone').classList.remove('dragging');
		if (session)
			return;
		// Entries are only readable while the event is dispatched.
		const roots = [...event.dataTransfer.items].map((item) => item.webkitGetAsEntry?.()).filter(Boolean);
		const [file] = event.dataTransfer.files;
		if (roots.length)
			importFrom(() => droppedEntries(roots));
		else if (file)
			importFrom(() => archiveEntries(file));
	});
	addEventListener('pointermove', () => {
		if (session?.running)
			showToolbar();
	});
	document.addEventListener('visibilitychange', () => {
		if (!session?.running)
			return;
		session.audio?.setPaused(document.hidden);
		if (document.hidden)
			persist();
	});
	addEventListener('pagehide', persist);
	addEventListener('beforeunload', (event) => {
		if (session?.running)
			event.preventDefault();
	});
}

async function boot() {
	bindPage();
	if (!crossOriginIsolated) {
		if (await isolate())
			return;
		setBusy(true);
		showMessage('This page needs cross-origin isolation (COOP and COEP headers) for SharedArrayBuffer. Serve it with tools/serve.mjs or a host that sends these headers, over HTTPS or on localhost.', true);
		$('files-status').textContent = 'The game cannot run on this page.';
		return;
	}
	sessionStorage.removeItem(COI_RELOAD_KEY);
	if (!document.createElement('canvas').getContext('webgl2'))
		showMessage('This browser has no WebGL2, which the game needs. Enable hardware acceleration or try another browser.', true);
	modulePromise = createNfs2se({
		nfsMessageBox: messageBox,
		onExit: (status) => endSession(status === 0 ? 'The game has quit.' : `The game stopped (exit status ${status}).`),
		onAbort: (what) => endSession(`The game crashed: ${what}`),
	});
	modulePromise.catch((error) => showMessage(`The game could not be loaded: ${errorText(error)}`, true));
	try {
		db = await openStorage();
	} catch (error) {
		setBusy(true);
		showMessage(`This browser does not allow storing the game files (IndexedDB: ${errorText(error)}).`, true);
		return;
	}
	await refreshFiles();
}

boot().catch(reportError);

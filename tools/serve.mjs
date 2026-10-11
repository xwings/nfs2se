// Static file server with the cross-origin isolation headers that
// SharedArrayBuffer (and so the wasm threads) needs. It listens on the
// loopback interface only: over plain HTTP only localhost is the secure
// context that isolation requires.
//   node tools/serve.mjs [root=dist] [port=8080, 0 picks a free one]
import { once } from 'node:events';
import { createReadStream } from 'node:fs';
import { stat } from 'node:fs/promises';
import { createServer } from 'node:http';
import { extname, join, resolve, sep } from 'node:path';
import { pipeline } from 'node:stream/promises';

const root = resolve(process.argv[2] ?? 'dist');
const port = Number(process.argv[3] ?? 8080);
// The types of the files in dist; others are sent as application/octet-stream.
const TYPES = {
	'.html': 'text/html; charset=utf-8',
	'.js': 'text/javascript; charset=utf-8',
	'.css': 'text/css; charset=utf-8',
	'.wasm': 'application/wasm',
};
const HEADERS = {
	'Cross-Origin-Opener-Policy': 'same-origin',
	'Cross-Origin-Embedder-Policy': 'require-corp',
	'Cache-Control': 'no-cache',
};

// The file for a URL path inside root, or null for paths that escape it.
function fileFor(url) {
	let path;
	try {
		path = decodeURIComponent(new URL(url, 'http://localhost').pathname);
	} catch {
		return null;
	}
	const file = join(root, path);
	return file === root || file.startsWith(root + sep) ? file : null;
}

function sendText(response, status, text) {
	response.writeHead(status, { ...HEADERS, 'Content-Type': 'text/plain; charset=utf-8' }).end(`${text}\n`);
}

const server = createServer(async (request, response) => {
	if (request.method !== 'GET' && request.method !== 'HEAD') {
		response.writeHead(405, { ...HEADERS, Allow: 'GET, HEAD' }).end();
		return;
	}
	let file = fileFor(request.url), stream = null, info;
	try {
		info = file && await stat(file);
		if (info?.isDirectory()) {
			file = join(file, 'index.html');
			info = await stat(file);
		}
		if (info?.isFile()) {
			stream = createReadStream(file);
			await once(stream, 'open');
		}
	} catch (error) {
		stream?.destroy();
		if (error.code === 'ENOENT' || error.code === 'ENOTDIR') {
			stream = null;
		} else {
			console.error(`${file}: ${error.message}`);
			sendText(response, 500, 'Internal server error');
			return;
		}
	}
	if (!stream) {
		sendText(response, 404, 'Not found');
		return;
	}
	response.writeHead(200, { ...HEADERS, 'Content-Type': TYPES[extname(file)] ?? 'application/octet-stream', 'Content-Length': info.size });
	if (request.method === 'HEAD') {
		stream.destroy();
		response.end();
		return;
	}
	// A read error after the headers went out can only cut the response short.
	pipeline(stream, response).catch((error) => {
		if (error.code !== 'ERR_STREAM_PREMATURE_CLOSE') // not for downloads the browser stopped
			console.error(`${file}: ${error.message}`);
	});
});
server.listen(port, '127.0.0.1', () => console.log(`Serving ${root} at http://localhost:${server.address().port}/`));

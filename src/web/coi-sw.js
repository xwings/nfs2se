// Service worker that adds the cross-origin isolation headers to every
// same-origin response, for hosts that cannot send them (SharedArrayBuffer
// needs them). main.js registers it and reloads once.

self.addEventListener('install', () => self.skipWaiting());
self.addEventListener('activate', (event) => event.waitUntil(self.clients.claim()));

self.addEventListener('fetch', (event) => {
	const request = event.request;
	if (new URL(request.url).origin !== self.location.origin)
		return;
	event.respondWith(fetch(request).then((response) => {
		// Opaque and redirect responses cannot be rebuilt.
		if (response.type !== 'basic' && response.type !== 'default')
			return response;
		const headers = new Headers(response.headers);
		headers.set('Cross-Origin-Opener-Policy', 'same-origin');
		headers.set('Cross-Origin-Embedder-Policy', 'require-corp');
		return new Response(response.body, { status: response.status, statusText: response.statusText, headers });
	}));
});

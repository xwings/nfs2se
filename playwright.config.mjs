// Browser tests against dist/, served with the isolation headers (make test-e2e).
import { defineConfig, devices } from '@playwright/test';

const PORT = 8123;

export default defineConfig({
	testDir: 'tests/e2e',
	timeout: 60_000,
	fullyParallel: true,
	reporter: 'list',
	use: {
		baseURL: `http://localhost:${PORT}/`,
	},
	projects: [{
		name: 'chromium',
		use: {
			...devices['Desktop Chrome'],
			launchOptions: { args: ['--autoplay-policy=no-user-gesture-required', '--enable-unsafe-swiftshader'] },
		},
	}],
	// Outside CI a server already running on PORT (node tools/serve.mjs dist
	// 8123) is reused, which skips the start-up check for a free port.
	webServer: {
		command: `node tools/serve.mjs dist ${PORT}`,
		url: `http://localhost:${PORT}/`,
		reuseExistingServer: !process.env.CI,
	},
});

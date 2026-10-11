# nfs2se-web: Need for Speed II SE in the browser

nfs2se-web is a port of Need for Speed II SE, the 1997 racing game, to the web browser. The game is compiled to WebAssembly and runs natively, with no x86 emulator: the build compiles the C++ translation of the game from [NFSIISE](https://github.com/zaps166/NFSIISE) by zaps166 with Emscripten. A platform layer written in C provides what the game expects from Win32, DirectX, the EA sound library and 3dfx Glide. The page draws the game with WebGL2 and plays its sound through an AudioWorklet.

No game data is included. You import the files from your own copy of the game, and they stay in your browser's storage on your computer.

## Status

Early: play with real game data has not been verified yet. The build and the test suite pass, but the tests use generated dummy files, so menus, races, saves and the frame rate are unverified. Known gaps:

- The game's movies are not shown.
- Several 3dfx Glide features have no effect yet ([graphics](ARCHITECTURE/graphics.md)).
- No network or serial-link play, and no force feedback.
- Only Chromium is tested.

## Features

- Imports your copy of the game from its installed folder, a CD image (ISO or BIN) or a ZIP archive, chosen or dropped on the page.
- Renders the game's 640 × 480 picture at the size it is shown on screen, up to 3840 pixels wide, with optional anti-aliasing, smooth textures, bloom, sharpening and enhanced colours. Settings apply while you play.
- Keyboard, mouse and gamepad input.
- Saves the game's settings, saved games and records in the browser.
- Shows the game's message boxes as page dialogs.

## Requirements

- **To play:** a browser with WebAssembly threads, WebGL2 and AudioWorklet. The page must be cross-origin isolated; the included server takes care of that ([Hosting](#hosting) covers other servers).
- **To build:** git, GNU make, Python 3 (Emscripten runs on it), the [Emscripten SDK](https://emscripten.org/docs/getting_started/downloads.html) and Node.js 20 or later. The build was last checked on Linux (WSL2) with Emscripten 6.0.12, Python 3.13.5 and Node.js 20.19.2.

## Install

1. Install the Emscripten SDK and activate 6.0.12, the version the build was last checked with. If you already have the SDK, use its directory instead of `~/emsdk`, and run `git pull` in it instead of the `git clone` so that it knows this version:

   ```sh
   git clone https://github.com/emscripten-core/emsdk.git ~/emsdk
   ~/emsdk/emsdk install 6.0.12
   ~/emsdk/emsdk activate 6.0.12
   source ~/emsdk/emsdk_env.sh
   ```

   `emsdk_env.sh` puts `emcc`, `em++` and `em-config` on the `PATH` of the current shell only; source it again in each new shell before you build.

2. Clone this repository with its submodules. The translated game is not part of this repository: it is a nested submodule of NFSIISE, compiled at build time.

   ```sh
   git clone --recursive https://github.com/xwings/nfs2se.git
   cd nfs2se
   ```

   In a clone made without `--recursive`, run `git submodule update --init --recursive`.

3. Build:

   ```sh
   make
   ```

   This compiles the game and the platform layer into `dist/nfs2se.js` and `dist/nfs2se.wasm`, and copies the page, the upstream support files and the licence notices into `dist/`.

4. Start the local server and open <http://localhost:8080/>:

   ```sh
   make serve
   ```

   `make serve PORT=9000` uses another port. The server sends the headers the game needs and accepts connections only from this computer.

To update later, run `git pull`, `git submodule update --init --recursive`, `make clean` and `make`. The `make clean` is needed because the build does not notice changed compiler flags. If the updated README names a newer Emscripten version, install and activate it first, as in step 1.

## Play

1. On the start page, choose or drop your copy of the game: the folder where it is installed, a CD image (ISO or BIN), or a ZIP archive with its `FEDATA` and `GAMEDATA` folders. Only those two folders are kept, and `FEDATA` must contain the `PC` folder of the Windows version. You import the files once; they are kept for later visits.
2. Press **Play**.

**Controls:**

- The keyboard and gamepads reach the game as they would on Windows; set up the controls in the game's options.
- Alt+Enter or the **Fullscreen** button turns fullscreen on and off. In fullscreen in Chromium-based browsers, Escape goes to the game; hold it down to leave fullscreen.
- While you play, moving the mouse shows the **Settings** and **Fullscreen** buttons.
- F5, F11 and F12 keep their browser functions (reload, fullscreen and developer tools) and also reach the game, which uses F5 for the HUD and F11 and F12 to reset a player's car. While the game runs, the browser asks before it reloads or leaves the page.

**Settings**, on the start page or in the toolbar while you play, control the picture and the sound and apply at once: the rendering resolution (the display's, half of it, or the original 640 × 480), anti-aliasing, smooth textures, bloom, sharpening, enhanced colours, keeping the 4:3 picture shape, a frame-rate counter and the volume. The browser remembers them.

**Your data:** the imported game files, saved games, records and the game's own settings stay in this browser's storage. The storage belongs to the page's address, so another port, or 127.0.0.1 instead of localhost, starts empty. The page asks the browser to keep this storage, but a browser that declines may clear it when disk space runs low. **Remove game files** deletes only the imported files; clearing the site's data in the browser deletes everything. After the game quits, **Back to the start screen** reloads the page so you can play again.

## Hosting

`dist/` is a static site, and `make serve` is meant for your own computer only. Any other web server must serve `dist/` over HTTPS (or on localhost), and should send these two headers, which browsers require before they allow the shared memory the game's threads use:

```
Cross-Origin-Opener-Policy: same-origin
Cross-Origin-Embedder-Policy: require-corp
```

If the server cannot send them, the page registers its `coi-sw.js` service worker, which adds them, and reloads once. That fallback is untested.

Before you publish `dist/`, read [NOTICE](NOTICE): `dist/` contains the compiled translated game and the upstream support files, which neither this project's licence nor the upstream MIT licence covers.

## Development

The tests also need Playwright and its Chromium:

```sh
npm ci
npx playwright install chromium
make test
```

`make test` runs the C tests (compiled to WebAssembly and run under Node), the JavaScript unit tests and the Playwright browser tests against a freshly built `dist/`. `make test-c`, `make test-unit` and `make test-e2e` run each part on its own. The tests use generated dummy files, never game data.

[ARCHITECTURE.md](ARCHITECTURE.md) describes the design, the subsystems and which tests to run for each change.

## Licence and credits

- This project is MIT-licensed ([LICENSE](LICENSE)).
- The translated game comes from [NFSIISE](https://github.com/zaps166/NFSIISE) by zaps166 (Błażej Szczygieł). Its MIT licence covers only its wrapper, not the translated game, so this repository does not include the game code: it comes in as a git submodule and is compiled at build time. The platform layer in `src/platform` is original code that re-implements the wrapper's behaviour for the browser.
- Need for Speed II SE is a trademark of Electronic Arts. Neither this repository nor `dist/` contains game data.
- [NOTICE](NOTICE) has the full credits and says which licence, if any, covers each part of `dist/`, including the Emscripten runtime and libraries.

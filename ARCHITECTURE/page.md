---
eatmycode_version: "1.2.0"
---
# Page

## Goal

- **Owns:** the browser main thread around the subsystems it wires together.
  - `src/web/main.js` is the composition root: boot and isolation, launcher state, imports, Play, the frame loop, message boxes, save persistence, fullscreen, error reporting and session end.
  - `src/web/settings.js` holds the page settings. `src/web/coi-sw.js` supplies the isolation headers on hosts that cannot send them.
  - `src/web/index.html` and `src/web/style.css` are the launcher and the game view.
- **Non-goals:** import parsing and storage ([game files](game-files.md)), drawing ([graphics](graphics.md)), key mapping ([input](input.md)), the audio ring ([audio](audio.md)) and the shared-word layout ([shared memory](shared-memory.md)). No framework, bundler or router.
- **Kind:** product code, not infrastructure.

## Status

`done`.

- **Evidence:** on 2026-10-11, `make test` exited 0. Run on their own, `node --test tests/unit/settings.test.mjs` passed 6 of 6 and the Playwright tests of `tests/e2e/launcher.spec.mjs` and `tests/e2e/game.spec.mjs` printed `12 passed`.
- **Unverified:** a session with real game data, browsers other than Chromium, and the coverage gaps in [How to Test](#how-to-test).

## Code Structure

| File | Role |
| ---- | ---- |
| `src/web/main.js` | Composition root and session controller. |
| `src/web/settings.js` | Frozen defaults, validation of stored JSON, `localStorage`, form binding. |
| `src/web/coi-sw.js` | Service worker that adds COOP/COEP to same-origin responses. |
| `src/web/index.html` | Canvas, toolbar, fps overlay, launcher, settings and message-box dialogs, the ended panel with its save warning. |
| `src/web/style.css` | Launcher layout; the full-window game view under `body.playing`. |
| `tests/unit/settings.test.mjs` | `settings.js` with a fake storage and form. |
| `tests/e2e/launcher.spec.mjs` | Start page: isolation, imports, removal, settings. Uses `openLauncher` and `importGameFolder` (`tests/e2e/helpers.mjs:44`, `:50`). |
| `tests/e2e/game.spec.mjs` | A wasm session with dummy files: Play, message box, exit, IDBFS; a failed support file, a failed save and a failed display. |

## Language and Conventions

- **Language:** JavaScript ES modules loaded as-is, with no bundler (`index.html:9`, `main.js:3`–`:12`). `main.js:3` imports the Emscripten output `./nfs2se.js`, so the page only runs from a built `dist/` ([build and test](build-and-test.md)).
- **Root rules:** [Coding Style and Code Design](../ARCHITECTURE.md#coding-style-and-code-design). No linter runs; only tests enforce behaviour.
- **Local patterns to reuse (observed):**
  - **Element ids** are the contract between `index.html`, `main.js` and the tests, looked up with `$(id)` (`main.js:21`).
  - **Messages:** launcher failures reach the player through `showMessage(text, true)` (`main.js:30`), with text that says what to do (`:106`, `:407`). Failures once Play has started end the session with the reason instead (`main.js:259`, `:316`, `:417`). Optional features that fail only `console.warn` (`main.js:223`, `:281`, `:297`).
  - **Error text:** `errorText` gives an Emscripten `FS.ErrnoError`, which carries only an errno, the text `file system error <errno>`, and other errors their `message` (`main.js:35`–`:37`). `reportError` shows that text for event handlers that have no caller to report to, and catches `boot` (`main.js:40`–`:41`, `:430`).
  - **Ignored rejections** carry a comment with the reason (`main.js:99`–`:101`, `:207`).
  - **Guards:** user actions return while `busy` or a `session` exists (`main.js:92`, `:273`, `:344`). Imports wrap their work in `setBusy(true)` … `finally` (`main.js:94`–`:111`); Play clears `busy` only after a successful start (`main.js:311`).
  - **Progress:** `showProgress(label, fraction)` hides on a `null` label and is indeterminate without a fraction (`main.js:43`).
  - **Dialogs:** `<dialog>` with `<form method="dialog">`; the pressed button's `value` becomes `returnValue` (`index.html:60`, `:90`, `main.js:187`).
  - **Settings controls** are named after the setting keys (`settings.js:58`, `index.html:65`–`:81`).
  - **CSS state:** classes that `main.js` toggles (`body.playing`, `#files.ready`, `#drop-zone.dragging`, `#toolbar.visible`, `#message.error`); `:root` colour properties (`style.css:1`); `[hidden]` always wins (`style.css:19`).
  - **Text** from the game or status goes in through `textContent`, never HTML (`main.js:31`, `:166`, `:182`–`:183`, `:209`).

## Design and Invariants

### Decomposition and state

- **Wiring:** `main.js` imports the partner modules (`main.js:3`–`:12`) and nothing imports it. Partners get the canvas, shared views, live `settings` or `db` as arguments (`renderer.js:117`, `input.js:96`, `audio.js:20`).
- **Module state:** the live `settings` (`main.js:23`), plus `db`, `modulePromise`, `session` and `busy` (`main.js:24`–`:27`). `session` is `{module, ctl, renderer, input, audio, running, syncing}` (`main.js:303`).
- **One session per page load:** `session` is never cleared, and import, Play, Remove and drops refuse while it exists (`main.js:92`, `:273`, `:344`, `:373`); `#reload` reloads the page (`main.js:342`). The module, the canvas's WebGL context and `/game` are set up once per page (`main.js:313`–`:314`): `modulePromise` is created once (`main.js:414`), and the module links with `-sEXIT_RUNTIME=1` (`Makefile:43`).

### Boot (`main.js:401`)

1. `bindPage` attaches every listener first (`main.js:402`).
2. **Isolation:** without `crossOriginIsolated`, `isolate` registers `coi-sw.js`, sets the `sessionStorage` key `nfs2se-coi-reload` and reloads once (`main.js:75`–`:88`). The key prevents a reload loop. A page that is still not isolated disables the buttons and shows the isolation error (`main.js:406`–`:408`); once isolated, it removes the key (`main.js:411`).
3. **WebGL2:** a missing context only produces a message (`main.js:412`–`:413`); Play fails later with its own error (`main.js:290`).
4. **Module:** `createNfs2se` starts loading at once (`main.js:414`). Its hooks are `nfsMessageBox`, plus `onExit` and `onAbort`, which end the session with a message (`main.js:416`–`:417`). A load failure shows a message (`main.js:419`).
5. **Storage:** `openStorage`, then `refreshFiles` (`main.js:421`, `:427`). Without IndexedDB, the page disables the buttons, shows a message and stops (`main.js:423`–`:425`).
6. **Other failures** in `boot`, such as a failing `refreshFiles`, become a launcher message through `reportError` (`main.js:430`).

### Launcher and import

- **State:** `refreshFiles` derives the launcher from `gameFilesInfo` (`main.js:63`). Play is enabled only with stored files and when not busy; Remove is shown only with files (`main.js:69`–`:70`). `setBusy` disables Play, Remove and both pickers (`main.js:57`–`:60`).
- **Sources:** the pickers (`main.js:354`) and drops (`main.js:370`) give `importFrom` a function that produces entries (`main.js:91`); [game files](game-files.md) selects and stores them. It asks for persistent storage as a best effort (`main.js:99`–`:101`) and gives `QuotaExceededError` its own message (`main.js:105`).
- **Errors:** `importFrom` shows every failure as a message and never rejects, so callers do not await it; a failing refresh afterwards goes to `reportError` (`main.js:104`–`:111`).
- **Drops:** entries are only readable during the dispatch, so they are taken synchronously (`main.js:375`–`:377`). `droppedEntries` opens a single dropped file as an archive (`importer.js:60`); without entries, the first file goes to `archiveEntries` (`main.js:380`–`:381`). Drops during a session are ignored (`main.js:373`).
- **Remove** asks `confirm()`, keeps saves and settings, and reports a failure through `reportError` (`main.js:343`–`:353`).

### Play (`main.js:272`)

The order is required:

1. **Audio context** in the click, before any `await` (`main.js:279`): some browsers only start a context inside a gesture (`audio.js:4`–`:5`). A failure only warns (`main.js:281`).
2. **Views:** `ctl` and both graphics slots on `module.HEAPU8.buffer` (`main.js:286`–`:288`). Without WebGL2, Play throws an actionable error (`main.js:290`).
3. **Files:** `populateFiles` (`main.js:130`) creates `/game`, fetches `files/manifest.txt`, then every file it lists, in parallel (`main.js:132`–`:134`). `make` writes the manifest from `SUPPORT_FILES` (`Makefile:69`–`:71`, `:23`). An HTTP error throws `Could not load files/<name> (HTTP <status>).` (`main.js:123`). Then it writes every stored game file, mounts IDBFS at `SAVE_DIR` and loads it (`main.js:135`–`:143`).
4. **Audio:** `startAudio` runs before `callMain`, as its comment requires: the game feeds the ring through `AUD_CONSUMER` (`audio.js:18`–`:19`, [audio](audio.md)). If it rejects, Play warns, closes the context and continues without sound (`main.js:296`–`:299`).
5. **Input:** `Input` uses the renderer's `keepAspect`; Alt+Enter calls `toggleFullscreen` (`main.js:302`, `input.js:130`).
6. **Start:** set `session` and `body.playing`, observe the canvas size, enable input, focus, start `runFrames` and the 1 s `persist` interval, then `callMain([GAME_DIR, SAVE_DIR])` (`main.js:303`–`:310`). Only `main.js` names the two paths (`main.js:14`–`:15`); C takes them from `argv[1]` and `argv[2]` (`web.c:117`, `:122`).

**Failure:** any other error closes the audio context and ends the session with `The game could not start: <reason>` (`main.js:315`–`:316`). `busy` stays set, so Play, Remove and both pickers stay disabled until `#reload` (`main.js:313`–`:314`).

### Frames, canvas, fullscreen

- **Frame loop:** `runFrames` (`main.js:248`) is one `requestAnimationFrame` loop: `input.poll()` for gamepads and refused key releases (`input.js:188`–`:190`), then `renderer.pump()` (`renderer.js:171`). It stops when `session.running` is false; the page never waits for the game. When `pump` throws, the loop logs the error and ends the session with `The game display failed: <message>` (`main.js:257`–`:259`).
- **Frame rate:** the `#fps` text counts the frames `pump` presented, averaged over at least 500 ms (`main.js:19`, `:255`–`:256`, `:262`–`:263`).
- **Canvas size:** a `ResizeObserver` on `device-pixel-content-box`, falling back to `contentRect` × `devicePixelRatio`, feeds `renderer.resize` (`main.js:233`–`:246`).
- **Fullscreen:** `toggleFullscreen` locks Escape with `navigator.keyboard.lock` so the key reaches the game (`main.js:220`–`:221`). Errors only warn (`main.js:223`).
- **Toolbar:** shown on `pointermove` during play and hidden after 2500 ms (`main.js:18`, `:230`, `:383`).

### Message boxes (`main.js:180`)

- **Protocol:** `web_message_box` calls `Module.nfsMessageBox` on the main thread (`web.c:76`), then the game thread waits on `CTL_DIALOG` and re-checks every second in case a wake is missed (`web.c:17`, `:78`–`:79`). On close, `messageBox` stores `DIALOG_OK` or `DIALOG_CANCEL` and notifies (`main.js:187`–`:190`). Escape (an empty `returnValue`) means OK in an OK-only box and Cancel otherwise.
- **Input:** while any page dialog is open, input is disabled and held keys are released (`main.js:170`, `input.js:121`–`:123`).
- **One box at a time:** a mutex in `web_message_box` makes boxes from several game threads wait their turn, so the dialog and `CTL_DIALOG` serve one box at a time (`web.c:68`–`:70`, `:73`, `:80`).

### Persistence (`main.js:148`)

- **Dirty word:** C calls `web_mark_fs_dirty` (`web.c:89`) after a writable file closes (`kernel32.c:520`, `wrapper.c:201`), a file is deleted (`kernel32.c:502`), the configuration template is copied (`wrapper.c:100`) or a records table is saved (`records.c:99`).
- **Sync:** `persist` runs only when the word is set and no sync is running. It clears the word before `syncfs(false)`, so a write during the sync sets it again (`main.js:150`–`:154`).
- **Failure:** `persist` warns, sets the word again so the next call retries, and puts `Saved games and settings could not be stored: <reason>` into `#ended-warning` (`main.js:155`–`:158`, `:164`–`:166`). The warning shows with the ended panel; the next successful sync hides it (`main.js:155`).
- **Triggers:** the 1 s interval (`main.js:309`), a hidden tab while the game runs (`main.js:391`–`:392`), `pagehide` (`main.js:394`) and `endSession` (`main.js:203`). The interval is never cleared, so writes after the end are still stored and a failed save is retried.
- **Scope and compatibility:** only `SAVE_DIR` persists; `/game` is a MEMFS copy written by each Play. The IDBFS database is named after the mount point (`main.js:15`), which `game.spec.mjs:17` opens. Renaming it orphans saves ([Mission and Constraints](../ARCHITECTURE.md#mission-and-constraints)).

### Session end

- **`endSession(text)`** (`main.js:199`) runs from `onExit`, `onAbort`, a display failure or a failed Play. The first call with a running game stops input and frames, persists and pauses audio (`main.js:200`–`:204`). Every call leaves fullscreen, removes `body.playing` and shows `#ended` with the text (`main.js:206`–`:210`).
- **Exit texts:** `The game has quit.` for status 0, otherwise `The game stopped (exit status <n>).`; an abort shows `The game crashed: <what>` (`main.js:416`–`:417`).
- **`visibilitychange`**, while the game runs, pauses or resumes audio and persists when the tab is hidden (`main.js:387`–`:393`). **`beforeunload`** asks for confirmation while the game runs (`main.js:395`–`:398`).

### Settings (`settings.js`)

- **Defaults:** `DEFAULT_SETTINGS` is frozen (`settings.js:7`). `resolution` is `native`, `original` or `half`; `volume` is 0 to 1; the rest are booleans.
- **Parsing:** `parseSettings` (`settings.js:20`) ignores invalid JSON and non-objects. It takes a stored key only if its `typeof` matches the default, drops unknown keys, and rejects resolutions outside `RESOLUTIONS` and volumes outside [0, 1]. New settings therefore start at their defaults for existing users.
- **Storage:** `loadSettings` and `saveSettings` survive a missing or throwing `localStorage`; settings then last one visit (`settings.js:41`–`:56`).
- **Form:** `bindSettingsForm` (`settings.js:60`) fills controls named after keys. On input it stores a boolean, a `Number` or a string, saves, then calls `onChange(key)`. It ignores unknown control names (`settings.js:70`) and relies on the controls to bound values (`index.html:65`, `:81`).
- **Live apply:** `main.js` sets the volume, toggles `#fps` and invalidates the renderer (`main.js:323`–`:328`), which reads `settings` every frame (`renderer.js:116`).

### Service worker and markup

- **`coi-sw.js`:** `skipWaiting` and `clients.claim` let the single reload already run under the worker (`coi-sw.js:5`–`:6`). Cross-origin requests are not intercepted (`coi-sw.js:10`). Same-origin `basic` and `default` responses are rebuilt with `Cross-Origin-Opener-Policy: same-origin` and `Cross-Origin-Embedder-Policy: require-corp`; other responses pass through (`coi-sw.js:14`, `:17`–`:19`).
- **`index.html` ids:** `#screen` (`index.html:12`), `#toolbar` (`#toolbar-settings`, `#toolbar-fullscreen`) and `#fps`. The `#launcher` holds `#files` with `#files-status`, `#drop-zone`, `#pick-folder`, `#pick-archive` and `#progress` (`#progress-bar`, `#progress-label`), then `#message`, `#play`, `#settings-open` and `#remove-files`. Then `#settings-dialog` with `#settings-form`, `#message-box` (`#message-box-caption`, `#message-box-text`, `#message-box-cancel`), and `#ended` with `#ended-text`, `#ended-warning` and `#reload` (`index.html:100`–`:103`).
- **`style.css`:** the game view stays hidden until `body.playing` (`style.css:162`–`:175`). The canvas then fills the window with no cursor, since the game draws its own (`style.css:160`, `:182`), and sets `touch-action: none` (`style.css:183`). `#ended-warning` uses the error colour (`style.css:151`–`:153`).

## Key Types and Entry Points

1. **`boot()`** (`main.js:401`): runs once at load as `boot().catch(reportError)` (`main.js:430`). It calls `bindPage` (`main.js:322`), which attaches every DOM listener, then ensures isolation with at most one reload, starts the module load and opens storage. Failures become launcher messages; where the page cannot work, the buttons stay disabled.
2. **`play()`** (`main.js:272`): the Play click; returns at once while busy or in a session. Builds the views, renderer, files, audio and input, sets `session`, starts the frame loop and the persist interval, then calls `callMain([GAME_DIR, SAVE_DIR])`. Any failure ends the session with `The game could not start: <reason>`; the buttons stay disabled until a reload.
3. **`populateFiles(module)`** (`main.js:130`): writes `/game` from `files/manifest.txt`, the files it lists and IndexedDB, then mounts and loads IDBFS at `SAVE_DIR`. Rejects on an HTTP error from `fetchFile` (`main.js:120`) or a file-system error.
4. **`runFrames()`** (`main.js:248`): the frame loop. Each animation frame polls input, pumps the renderer and updates the fps text; the loop stops when the session stops running. A throwing `pump` ends the session with `The game display failed: <message>`.
5. **`messageBox(text, caption, okCancel)`** (`main.js:180`): the `Module.nfsMessageBox` hook. Opens the modal and disables input; on close it stores `DIALOG_OK` or `DIALOG_CANCEL` in `CTL_DIALOG`, notifies and re-enables input. Returns nothing; the game thread waits for the word.
6. **`persist()`** (`main.js:148`): `syncfs(false)` when `CTL_FS_DIRTY` is set and no sync runs. On failure it warns, sets the word again and shows the reason through `showSaveError` (`main.js:164`), which a later success clears. Reports failures instead of throwing.
7. **`endSession(text)`** (`main.js:199`): stops a running session once (input, frames, a last persist, audio pause), leaves fullscreen and shows `#ended` with `text`. Safe to call twice and without a session.
8. **`importFrom(getEntries)`** (`main.js:91`): a picker or drop import with progress; returns at once while busy or in a session. Shows the result or the error as a message, then refreshes the launcher. Never rejects, so callers do not await it.
9. **`parseSettings(text)`, `bindSettingsForm(form, settings, onChange)`** (`settings.js:20`, `:60`): validated settings from stored JSON, and the live form that saves every change, then calls `onChange(key)`. Unusable stored values fall back to the defaults; unknown controls are ignored.
10. **The `fetch` handler** (`coi-sw.js:8`): adds COOP and COEP to same-origin `basic` and `default` responses; other requests and responses are left as they are.

## Interactions

| Partner | Boundary and direction | Duties |
| ------- | ---------------------- | ------ |
| [Shared memory](shared-memory.md) | Views through `_web_shared` (`main.js:286`–`:288`). Writes `CTL_DIALOG` (`main.js:189`); clears `CTL_FS_DIRTY` before a sync and sets it again after a failed one (`main.js:152`, `:157`); C sets it after writes (`web.c:91`). | `Atomics` and `layout.js` constants only. |
| [Platform](platform.md) | C → page: `Module.nfsMessageBox` (`web.c:76`), `onExit`, `onAbort`. Page → C: `callMain([GAME_DIR, SAVE_DIR])` runs `main` (`web.c:114`), which enters `/game` and keeps the save directory for `web_save_dir` (`web.c:117`–`:122`); without `/game` it shows a message box and exits with status 1 (`web.c:119`–`:120`). `WrapperInit` seeds `nfs2se.conf` from the template that Play copies into `/game` (`wrapper.c:127`–`:128`). | Keep the single C → page hook ([System Design](../ARCHITECTURE.md#system-design)). Only `main.js` names the paths. |
| [Graphics](graphics.md) | `Renderer`: `available`, `keepAspect`, `resize`, `invalidate`, `pump` (`renderer.js:148`–`:171`); reads `settings` live. `pump` returns whether it presented a frame and throws when WebGL fails, also when a restored context cannot be rebuilt (`renderer.js:168`–`:170`). | `pump` once per animation frame; never block. A throw ends the session. |
| [Input](input.md) | Constructs `Input`; `setEnabled`, `poll` each frame; `onFullscreen` calls back. | Input is off while a page dialog is open (`main.js:170`). |
| [Audio](audio.md) | `createAudioContext`, `startAudio` (`audio.js:6`, `:20`), `setVolume`, `setPaused` (`audio.js:29`, `:33`). | Keep the Play ordering; close the context when `startAudio` rejects. |
| [Game files](game-files.md) | Entry readers and `selectGameFiles`; `openStorage`, `gameFilesInfo`, `storeGameFiles`, `loadGameFiles`, `clearGameFiles` (`main.js:5`, `:12`). | Show their error messages verbatim through `errorText`. |
| [Build and test](build-and-test.md) | `make` copies `src/web` and the support files into `dist/` and writes `files/manifest.txt` (`Makefile:86`–`:88`, `:65`–`:71`). `main.js` fetches `files/<name>` (`main.js:121`). `tools/serve.mjs` sends COOP/COEP (`tools/serve.mjs:22`–`:24`). | `SUPPORT_FILES` (`Makefile:23`) is the only list of support files. |

Integration tests: `game.spec.mjs` (page, platform, renderer, storage, IDBFS) and `launcher.spec.mjs` (page, importer, storage).

## How to Test

**Prerequisites:** as in the root's [Verification and Review Map](../ARCHITECTURE.md#verification-and-review-map).

| Command | Expected evidence |
| ------- | ----------------- |
| `node --test tests/unit/settings.test.mjs` | `# pass 6`, `# fail 0` |
| `make test-unit` | `# fail 0` |
| `make`, then `npx --no playwright test tests/e2e/launcher.spec.mjs tests/e2e/game.spec.mjs` | `12 passed`; the server on port 8123 is started or, outside CI, reused (`playwright.config.mjs:21`–`:27`) |
| `make test` | Exit 0 ([Verification and Review Map](../ARCHITECTURE.md#verification-and-review-map)) |

| Behaviour or invariant | Test |
| ---------------------- | ---- |
| Isolation; an empty launcher with Play disabled and Remove hidden | "the page is cross-origin isolated and waits for game files" (`launcher.spec.mjs:11`) |
| A folder without game data is rejected with an error message | "a folder without the game data is rejected" (`launcher.spec.mjs:19`) |
| Files are kept across reloads; Remove honours `confirm()` | "imported game files are kept across visits until removed" (`launcher.spec.mjs:28`) |
| A ZIP through the picker | "a ZIP archive is imported through the picker" (`launcher.spec.mjs:51`) |
| A ZIP that fails partway leaves nothing stored ([game files](game-files.md)) | "a ZIP archive that fails partway leaves no game files stored" (`launcher.spec.mjs:58`) |
| A full disk shows the disk-space message, even when removing the partial import fails, which only warns | "a full disk is reported even when removing the partial import fails" (`launcher.spec.mjs:70`) |
| A dropped ZIP (synthetic `DataTransfer`, the `archiveEntries` branch) | "a ZIP archive dropped on the page is imported" (`launcher.spec.mjs:94`) |
| Settings are saved and restored; `#fps` toggles live | "settings are saved and restored on the next visit" (`launcher.spec.mjs:106`) |
| Play, an OK/Cancel box answered OK, `onExit` and the ended panel; IDBFS keeps `nfs2se.conf`; no save warning | "the game runs, waits for its message box and ends" (`game.spec.mjs:41`) |
| A failed support-file fetch ends the start with its reason; Play stays disabled | "a support file that cannot be loaded stops the start with the reason" (`game.spec.mjs:58`) |
| A failed save shows `#ended-warning`; the interval retries it and success hides the warning | "a failed save is shown when the game ends and retried until it works" (`game.spec.mjs:68`) |
| A renderer that cannot rebuild after a context loss ends the session with the reason | "a display that cannot be rebuilt after a context loss ends the game" (`game.spec.mjs:90`) |
| Frozen defaults; unusable text; type and range checks | "the defaults are frozen and parsing returns fresh copies", "parseSettings falls back to the defaults for unusable text", "parseSettings takes stored values of the right type and range" (`settings.test.mjs:25`, `:34`, `:39`) |
| `localStorage` present, missing or throwing | "loadSettings and saveSettings use localStorage when it works", "settings still work without localStorage or when it throws" (`settings.test.mjs:52`, `:62`) |
| Form binding; unknown controls are ignored | "bindSettingsForm shows the settings and stores every change" (`settings.test.mjs:72`) |

The fixture fails a test on any uncaught page error (`helpers.mjs:24`). Game files are generated dummies (`helpers.mjs:8`).

**Coverage gaps:**
- **Service worker:** `coi-sw.js` and `isolate` never run, because the test server always sends COOP/COEP.
- **Untested messages:** missing WebGL2 (the boot message and Play's error), a module load failure, no IndexedDB, a failing `boot` or Remove, the `ErrnoError` text, `onAbort`, a non-zero exit, and a Cancel or Escape answer.
- **Untested features:** fullscreen and the Escape lock (`input.spec.mjs:57` only checks that Alt+Enter calls back), the toolbar, `observeCanvasSize`, the fps text, the visibility pause, `pagehide`, `beforeunload`, a failed `startAudio`, Remove and the pickers staying disabled after a failed Play, and real directory drops (`droppedEntries`).
- **Saves after a reload:** no test plays again after a reload, so loading the save directory back (`main.js:143`) is untested; `game.spec.mjs:52` only checks that the settings file reaches IndexedDB.

## Review and Refactor Guide

| Change | Touch | Check |
| ------ | ----- | ----- |
| New page setting | `DEFAULT_SETTINGS` (its type is the validation), a `parseSettings` range check unless boolean, a named control in `#settings-form`, the consumer | `settings.test.mjs`, `launcher.spec.mjs:106`; keep `STORAGE_KEY` |
| New upstream support file | `SUPPORT_FILES` (`Makefile:23`); the page reads the manifest | `game.spec.mjs:41` |
| New hook from C | `createNfs2se` options (`main.js:414`), plus [platform](platform.md) | Justify it against [System Design](../ARCHITECTURE.md#system-design) |
| Launcher ids or text | `index.html`, `main.js` | `launcher.spec.mjs`, `game.spec.mjs`; `helpers.mjs:46`, `:53` expect launcher text |
| Launcher buttons, settings, the toolbar or kept data, as players see them | Also `README.md` | No test reads it; compare it with `index.html` |
| Player-facing error text | `main.js` | `game.spec.mjs:58`, `:68`, `:90` compare it exactly |
| Persistence | `persist`, `showSaveError`, `SAVE_DIR` | `game.spec.mjs:41`, `:68`; never rename `SAVE_DIR` |
| Play ordering or failure | `play` | `game.spec.mjs:41`, `:58`; the audio context stays in the click |
| Isolation headers | `tools/serve.mjs:22`, `coi-sw.js:17`, `ISOLATION_HEADERS` (`helpers.mjs:18`), `README.md` | `serve.test.mjs:34`, `launcher.spec.mjs:11` |

- **Reuse:** the local patterns in [Language and Conventions](#language-and-conventions), `fetchFile` for files next to the page (`main.js:120`) and the `syncfs` wrapper (`main.js:115`).
- **Duplicated values:** the COOP/COEP values are written in `tools/serve.mjs:23`–`:24` (a Node tool outside `dist/`), `coi-sw.js:17`–`:18` (a service worker registered as a classic script, `main.js:79`) and `ISOLATION_HEADERS` (`tests/e2e/helpers.mjs:18`–`:21`, the headers of `openBlankPage`); `serve.test.mjs:42`–`:43` asserts them, and `README.md` repeats them for other servers. Change them together.
- **Forbidden coupling:** game memory is read only through the shared regions; only `main.js` imports `nfs2se.js`; partners never touch `session`.

**Proposals (not accepted):**
- **Retry-safe Play:** a failed Play needs a reload, because the module, the canvas's WebGL context and `/game` are set up once per page (`main.js:313`–`:314`). Making that set-up repeatable would let Play run again. Benefit: no reload after a transient failure such as a support-file fetch. Check: `game.spec.mjs:58` then expects Play enabled, and a second click after the route is removed starts the game.
- **Isolation test:** serve once without COOP/COEP, and assert one reload, then `crossOriginIsolated`. Benefit: covers `coi-sw.js` and `isolate`. Check: green in `make test-e2e`.
- **Save export and import** ([Roadmap](../ARCHITECTURE.md#roadmap)): download and restore `SAVE_DIR` as a ZIP. Benefit: saves survive clearing site data. Check: an e2e round trip with a generated save.

## Open Gaps / Roadmap

- **Real game data:** a full session is unverified; this is the accepted work in [Roadmap](../ARCHITECTURE.md#roadmap).
- **Reloads:** a retry after a failed Play and a new game after exiting both need a page reload (`main.js:313`–`:314`; one module instance per page, `main.js:414`).
- **`pagehide` persistence** is best effort: `syncfs` is asynchronous, and nothing tests that IndexedDB commits before the page goes.

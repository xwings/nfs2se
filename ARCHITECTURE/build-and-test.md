---
eatmycode_version: "1.2.0"
---
# Build and test

## Goal

- **Owns:** the `Makefile` build of `dist/`, the development server, the npm, Playwright and git configuration, and the shared test harnesses.
- **Infrastructure:** no runtime code, but the link flags fix the page's runtime contract. Each test file belongs to the subsystem it tests ([inventory](#test-inventory)); `tests/unit/serve.test.mjs` belongs here.
- **Non-goals:** hosting, CI, fetching game data ([Mission and Constraints](../ARCHITECTURE.md#mission-and-constraints)).

## Status

`done`: implemented. Latest evidence, `make test` on 2026-10-11 (exit 0):
- C: `config` 117, `dinput` 549, `fmt` 23, `glide` 306, `kernel32` 87, `records` 352, `timer` 1, `user32` 2164 and `wrapper` 40 checks, all `0 failed`.
- Unit: `# tests 54`, `# fail 0`. Playwright: `28 passed`.
- `dist/` held the files listed under How to Test, and the curl checks there answered as listed.

No CI runs these. Only Chromium is tested (`playwright.config.mjs:14`–`:15`), and only with dummy game files; play with real data is unverified.

## Code Structure

| File | Role |
| ---- | ---- |
| `Makefile` | Build, copies into `dist/`, test targets |
| `tools/serve.mjs` | Static server with the isolation headers |
| `package.json` | ES modules (`package.json:4`); `engines` Node `>=20` (`:7`–`:9`); `npm test` runs `make test` (`:11`); dev dependency `@playwright/test` (`:14`) |
| `package-lock.json` | Lockfile v3 (`package-lock.json:3`) with the same `engines` (`:9`–`:11`); Playwright 1.64.0 (`:17`) |
| `playwright.config.mjs` | Browser test runner |
| `.gitmodules` | Submodule `third_party/NFSIISE` from `https://github.com/zaps166/NFSIISE.git` (`.gitmodules:2`–`:3`). The game source is its nested submodule `src/Cpp` (`third_party/NFSIISE/.gitmodules:4`); the other, `src/Asm`, is unused |
| `.gitignore` | `/build/`, `/dist/`, `/node_modules/`, `/test-results/`, `/playwright-report/` (`.gitignore:1`–`:5`) |
| `tests/c/check.h` | C check macros |
| `tests/e2e/helpers.mjs` | Playwright fixture, dummy game files, page helpers |
| `tests/support/zip.mjs` | ZIP fixture builder |
| `tests/unit/serve.test.mjs` | Tests of `tools/serve.mjs` on a temporary root |

## Language and Conventions

- **Languages:** GNU make, JavaScript ES modules (`package.json:4`) and GNU11 C (`Makefile:33`). [Coding Style and Code Design](../ARCHITECTURE.md#coding-style-and-code-design) applies.
- **Overrides:** only `EMCC`, `EMXX`, `EMCONFIG`, `NODE`, `NPX` and `PORT` use `?=` (`Makefile:9`–`:14`).
- **Toolchain floor:** `engines` asks for Node `>=20` (`package.json:7`–`:9`), the floor of Playwright 1.64 (`package-lock.json:27`–`:29`). No `.npmrc` makes npm enforce it, and no Emscripten version is pinned.
- **Test conventions** (observed):
  - C tests end `main` with `return check_exit();` (`tests/c/config_test.c:166`).
  - Unit tests use `node:test` and import `src/web` directly, so they need no build (`tests/unit/renderer.test.mjs:4`–`:6`).
  - All five specs take `test` from `./helpers.mjs`, not from `@playwright/test` (`tests/e2e/game.spec.mjs:6`).

## Design and Invariants

### Build

| Variable | Meaning |
| -------- | ------- |
| `BUILD`, `DIST` | `build`, `dist` (`Makefile:16`–`:17`) |
| `UPSTREAM`, `GAME_CPP` | The submodule and its `src/Cpp/NFS2SE.cpp` (`Makefile:18`–`:19`) |
| `UPSTREAM_FILES`, `SUPPORT_FILES` | Its `Need For Speed II SE` directory and the 8 files the game expects next to its data (`Makefile:22`–`:23`); the page reads their names from `files/manifest.txt` (`:20`–`:21`) |
| `NOTICE_FILES` | `dist/LICENSE`, `dist/NOTICE`, `dist/LICENSE.emscripten`, `dist/LICENSE.musl` and `dist/LICENSE.compiler-rt`, the licences of what `dist/` contains (`Makefile:24`–`:25`) |
| `PLATFORM_HDR`, `PLATFORM_OBJ`, `WEB_FILES` | Wildcards over `src/platform/*.h`, `src/platform/*.c` and `src/web/*` (`Makefile:27`–`:29`) |
| `GAME_CXXFLAGS`, `CFLAGS`, `LDFLAGS` | Game, platform and link flags (`Makefile:31`, `:33`, `:42`) |
| `C_TESTS`, `TEST_SRC_<name>`, `TEST_CFLAGS_<name>` | One name per `tests/c/*_test.c`, the sources each links and its extra flags (`Makefile:96`–`:108`) |

- **Targets:** `all` needs the link output, the support files and `manifest.txt` in `dist/files/`, the page files and the notices (`Makefile:51`). Rules build the game object (`Makefile:53`), the platform objects (`:57`), the link (`:61`), the support files (`:65`), the manifest (`:69`), the notices (`:73`, `:77`, `:80`, `:83`), the page files (`:86`) and the C tests (`:111`). The phony targets are `all`, `clean`, `serve`, `test`, `test-c`, `test-unit` and `test-e2e` (`Makefile:49`).
- **Compile flags** (no recorded reasons): the game uses C++14, `-O2 -DNDEBUG`, no RTTI or exceptions, `-D__WORDSIZE=32` and `-include endian.h` (`Makefile:31`–`:32`). Platform code fails on any warning (`Makefile:33`).

**Link flags** (`Makefile:42`–`:47`):

| Flag | Purpose and evidence |
| ---- | -------------------- |
| `-Wl,--fatal-warnings` | wasm-ld only warns about an import whose signature differs from its definition and links a trapping stub; this makes it a link error (`Makefile:34`–`:35`) |
| `-sPROXY_TO_PTHREAD` | `main()` and the game threads run in workers, so blocking never stalls the page (`src/platform/web.c:2`–`:3`) |
| `-sINVOKE_RUN=0` | The page calls `callMain` itself, after `populateFiles` (`src/web/main.js:291`, `:310`) |
| `-sEXIT_RUNTIME=1` | `exit` reaches `onExit`, which ends the session (`main.js:416`) |
| `-sMODULARIZE -sEXPORT_ES6 -sEXPORT_NAME=createNfs2se` | The ES module factory the page imports (`main.js:3`, `:414`) |
| `-sEXPORTED_RUNTIME_METHODS=callMain,FS,HEAPU8,UTF8ToString` | Used at `main.js:310`, `:131`, `:286` and `src/platform/web.c:76` |
| `-lidbfs.js` | The IndexedDB file system for saves (`src/web/main.js:142`) |
| `-sSTACK_SIZE=8MB`, `-sDEFAULT_PTHREAD_STACK_SIZE=2MB` | Main and default pthread stacks; game threads set 4 MiB themselves (`src/platform/kernel32.c:28`, `:143`). No recorded reasons |
| `-sPTHREAD_POOL_SIZE=8` | Workers loaded with the page: the platform's four threads (game, timer, audio producer, exit watchdog) and four of the game's; a further thread starts once a worker is created for it (`Makefile:36`–`:38`) |
| `-sALLOW_MEMORY_GROWTH`, `-sINITIAL_MEMORY=256MB`, `-sMAXIMUM_MEMORY=2GB`, `-Wno-pthreads-mem-growth` | 256 MiB, growable to 2 GiB. The warning is off: growth only slows Emscripten's own JS heap accesses, and the page's views of the shared regions stay valid, as growth never detaches a shared buffer (`Makefile:39`–`:41`) |
| `-sENVIRONMENT=web,worker` | Web and worker only. No recorded reason |

- **dist:**
  - The link writes `dist/nfs2se.js` and `dist/nfs2se.wasm` (`Makefile:61`).
  - The 14 `src/web` files are copied unchanged (`Makefile:86`).
  - **Required:** upstream support files go only to the gitignored `dist/files/` (`Makefile:65`, `.gitignore:2`). `manifest.txt` lists them one per line (`Makefile:71`), and the page writes each listed file into `/game` (`src/web/main.js:133`–`:134`).
  - `LICENSE` and `NOTICE` are copied from the root (`Makefile:73`). After the link, three notices are copied from `$(EMCONFIG) EMSCRIPTEN_ROOT`: Emscripten's `LICENSE` to `dist/LICENSE.emscripten`, musl's `COPYRIGHT` to `dist/LICENSE.musl` and compiler-rt's `LICENSE.TXT` to `dist/LICENSE.compiler-rt` (`:77`–`:84`).
  - With emcc 6.0.12 the link takes code from `libc-mt` (musl and Emscripten), `libclang_rt.builtins-mt` (compiler-rt and Emscripten), `libdlmalloc-mt` (dlmalloc and Emscripten's `sbrk`) and `libstubs` (Emscripten), and none from libc++ (observed). Linking with `-Wl,--why-extract=<file>` lists them; a library new to that list needs its licence in `NOTICE`.
- **Rebuilds** (observed):
  - Platform objects depend on every platform header (`Makefile:57`). Objects deliberately do not depend on the flags or the `Makefile`, as that would also recompile the 5.7 MB `NFS2SE.cpp`, so a flag edit needs `make clean`. Only `manifest.txt` depends on the `Makefile` (`:69`).
  - A C test depends on its sources, the platform headers and `check.h` (`Makefile:111`), so an edit of `TEST_SRC_<name>` or `TEST_CFLAGS_<name>` alone does not rebuild it.
  - The `.wasm` is an undeclared side output (`Makefile:61`).
  - The `WEB_FILES` wildcard never deletes stale `dist/` files (`Makefile:29`).

### C tests

- Each `tests/c/<name>_test.c` is linked with only the sources in `TEST_SRC_<name>` and built with the extra flags in `TEST_CFLAGS_<name>` into `build/tests/<name>.cjs` (`Makefile:94`–`:95`, `:111`). With no `TEST_SRC_<name>`, it links alone.
- They build for Node at `-O1` with `-Werror` (`Makefile:113`) and run as wasm32, so structures have the game's 32-bit layout (`Makefile:93`).
- **Threads:** without `-pthread` a test is single-threaded. `dinput`, `kernel32` and `wrapper` add `-pthread -sPTHREAD_POOL_SIZE=2` (`Makefile:99`, `:103`, `:108`), so a second thread can read a joystick, be woken by an event or watch an exit procedure (`tests/c/dinput_test.c:2`–`:3`, `tests/c/kernel32_test.c:1`–`:2`, `tests/c/wrapper_test.c:2`–`:3`). The timer test needs the build without `-pthread`, where `pthread_create` always fails (`tests/c/timer_test.c:1`–`:2`).
- **`.cjs`** (no recorded reason): emcc's Node output is CommonJS, and under `"type": "module"` (`package.json:4`) Node would load a `.js` file as an ES module.
- **Stubs:** a test defines the functions of the platform files it does not link. The glide test stubs `web.c`; its `web_futex_wait` plays the renderer and drains the published slots (`tests/c/glide_test.c:1`–`:3`, `:154`). The user32 test's stands in for JS while `GetMessageA` waits (`tests/c/user32_test.c:52`). The dinput test's `wrap_stdcall2_ret` plays the game's device callback (`tests/c/dinput_test.c:109`).

### Development server

- **Usage:** `node tools/serve.mjs [root=dist] [port=8080, 0 picks a free one]` (`tools/serve.mjs:5`). `make serve` builds first and passes `PORT` (`Makefile:90`–`:91`).
- **Loopback only:** it listens on `127.0.0.1`, because over plain HTTP only localhost is the secure context isolation requires (`tools/serve.mjs:2`–`:4`, `:86`). It logs `Serving <root> at http://localhost:<port>/` with the bound port (`:86`), which the unit test parses (`tests/unit/serve.test.mjs:26`).
- **Headers:** every response, errors included, has COOP `same-origin`, COEP `require-corp` and `Cache-Control: no-cache` (`tools/serve.mjs:22`, `:41`, `:46`, `:74`). SharedArrayBuffer needs the first two (`:1`–`:2`).
- **Methods:** only `GET` and `HEAD`; others get 405 with `Allow: GET, HEAD` (`tools/serve.mjs:45`–`:46`). `HEAD` gets the `GET` headers, `Content-Length` included, without a body (`:74`–`:78`).
- **Trust boundary:** `fileFor` decodes the path and returns null for a bad escape or a path outside the root (`tools/serve.mjs:29`, `:34`, `:37`). Null, `ENOENT`, `ENOTDIR` or anything but a regular file gives 404 (`:62`, `:70`–`:71`). Directories serve their `index.html` (`tools/serve.mjs:52`–`:53`). The root check is lexical: symlinks inside the root are followed.
- **Errors:** headers go out only after the file has opened (`tools/serve.mjs:58`, `:74`). Any other error, such as a symlink loop or an unreadable file, is logged and answered 500, and the server keeps running (`:65`–`:66`). A read error after the headers can only cut the response short (`:80`–`:84`).
- **MIME types:** `.html`, `.js`, `.css` and `.wasm`, the types in `dist/`; others get `application/octet-stream` (`tools/serve.mjs:15`–`:21`, `:74`).
- **Three header copies, kept apart:** COOP and COEP are written out in `tools/serve.mjs:22`, `src/web/coi-sw.js:17`–`:18` and `tests/e2e/helpers.mjs:18`. Nothing shares them, deliberately: the service worker ships in `dist/` (`Makefile:86`), the server is a development tool, and the test copy is an independent oracle. `tests/unit/serve.test.mjs:42`–`:43` also spells out the server's values.

### Browser test runner

- **Settings:** `tests/e2e`, 60 s per test, fully parallel, base URL on port 8123 (`playwright.config.mjs:4`–`:12`).
- **Project:** one `chromium` project (`playwright.config.mjs:14`–`:15`). Its flags let audio start without a gesture and allow SwiftShader's software WebGL (`:18`). No comment gives reasons.
- **Server:** `node tools/serve.mjs dist 8123` (`playwright.config.mjs:24`). Outside CI, any server already on 8123 is reused, which skips the free-port check (`:21`–`:22`, `:26`).
- **Freshness:** `make test-e2e` builds `all` first and runs `npx --no playwright test`, so npx never downloads an unpinned Playwright (`Makefile:124`–`:125`). A direct `npx --no playwright test` uses whatever `dist/` holds. Specs import `src/web/layout.js` and `src/web/settings.js` in Node while the page loads them from `dist/` (`tests/e2e/renderer.spec.mjs:5`–`:6`, `:37`), so rebuild `dist/` after editing either.

**Required:** test fixtures are generated bytes, never game data (`tests/e2e/helpers.mjs:1`–`:2`, `tests/unit/importer.test.mjs:1`–`:2`; [Project-Specific Deviations](../ARCHITECTURE.md#project-specific-deviations)).

## Key Types and Entry Points

1. **`all` and the link rule** (`Makefile:51`, `:61`): builds `dist/` from the submodule and `src/`, linking with `LDFLAGS` (`Makefile:42`). Needs `emcc`, `em++` and `em-config`. Fails on a platform warning, a mismatched import or a missing submodule.
2. **`SUPPORT_FILES` and `manifest.txt`** (`Makefile:23`, `:69`): each support file is copied from the submodule into `dist/files/` (`:65`), and their names, one per line, go to `dist/files/manifest.txt` (`:71`). The page fetches the manifest, then each listed file, into `/game` (`src/web/main.js:133`–`:134`). A failed fetch stops the start with `Could not load files/<name> (HTTP <status>).` (`:123`).
3. **C test rule** (`Makefile:111`): builds `build/tests/<name>.cjs` from `tests/c/<name>_test.c`, `TEST_SRC_<name>` and `TEST_CFLAGS_<name>` (`:113`–`:114`). Fails on any warning or an undefined symbol, such as a missing stub. The program exits 1 after any failed check (`tests/c/check.h:52`).
4. **`test`** (`Makefile:116`): runs `test-c` (`:118`), `test-unit` (`node --test`, `:121`–`:122`) and `test-e2e`, which builds `all` and runs Playwright (`:124`–`:125`). `test-c` echoes each `build/tests/<name>.cjs`, runs it with `node` and stops at the first non-zero exit (`:119`). `npm test` runs `make test` (`package.json:11`).
5. **`tools/serve.mjs`** (`tools/serve.mjs:44`, `:86`): serves a root on `127.0.0.1` with the isolation headers and logs `Serving <root> at http://localhost:<port>/`. Failures become 404, 405 or logged 500 responses; the server keeps running.
6. **`test` fixture** (`tests/e2e/helpers.mjs:24`): Playwright's `test` whose `page` collects `pageerror` events; any of them fails the test afterwards (`:29`).
7. **`openLauncher(page)`, `importGameFolder(page, testInfo)`** (`tests/e2e/helpers.mjs:44`, `:50`): `openLauncher` opens `/` and waits for `No game files imported yet.` (`:46`). `importGameFolder` then picks a folder that `writeTree` (`:34`) fills with the two dummy `GAME_FILES` (`:8`) and waits for `Imported 2 files. Press Play to start.` (`:53`).
8. **`storedFiles(page)`, `openBlankPage(page)`** (`tests/e2e/helpers.mjs:57`, `:78`): `storedFiles` returns `{path: text}` from IndexedDB `nfs2se`, store `files`, to compare with `STORED_GAME_FILES` (`:13`). It rejects on an IndexedDB error and always closes the database (`:61`, `:68`–`:69`). `openBlankPage` serves an empty page with its own `ISOLATION_HEADERS` (`:18`, `:81`) for specs that import page modules, and asserts `crossOriginIsolated` (`:85`).
9. **`buildZip(entries, {zip64, locator, comment})`** (`tests/support/zip.mjs:19`): returns ZIP bytes with stored or deflated entries (`:17`, `:24`). An entry's `declared` sizes or offset replace the true values in the central directory, to build damaged archives (`:18`, `:41`). ZIP64 fields come after an unrelated extra field (`:47`–`:48`). CRCs stay zero (`:2`). Pure; it does not validate its input.
10. **`check.h`** (`tests/c/check.h:12`): `CHECK`, `CHECK_INT` (int64, `:23`) and `CHECK_STR` (null-safe, `:36`) count checks in `s_checkCount` and failures in `s_checkFailures` (`:10`), printing `file:line` and the values on failure. `check_exit()` prints `N checks, M failed` and returns 1 if any failed (`:49`).

## Interactions

- **[Platform](platform.md):** every `src/platform/*.c` is linked (`Makefile:28`). Imports keep the game's `int32_t` signatures (`src/platform/platform.h:3`–`:6`), which `--fatal-warnings` enforces.
- **[Page](page.md):** `main.js` imports the factory (`src/web/main.js:3`) and reads the support file names from `files/manifest.txt` (`:133`). It registers the service worker only without isolation (`src/web/main.js:403`–`:404`), which `tools/serve.mjs` always provides, so tests never reach it. If that fails too, its error names `tools/serve.mjs` (`src/web/main.js:407`).
- **[Game files](game-files.md):** `buildZip` and `GAME_FILES` feed the importer tests (`tests/unit/importer.test.mjs:8`, `tests/e2e/launcher.spec.mjs:9`). `storedFiles` reads the store named in `src/web/storage.js:5`–`:7`.
- **[Graphics](graphics.md), [input](input.md), [audio](audio.md):** their specs start from `openBlankPage` (`tests/e2e/renderer.spec.mjs:138`, `tests/e2e/input.spec.mjs:10`, `tests/e2e/audio.spec.mjs:7`).
- **[Shared memory](shared-memory.md):** tests import `src/web/layout.js` directly (`tests/unit/renderer.test.mjs:5`, `tests/e2e/input.spec.mjs:4`).
- **Upstream:** `game.spec.mjs` checks that the saved `nfs2se.conf` has the template's bytes (`tests/e2e/game.spec.mjs:10`–`:11`).

## How to Test

**Prerequisites** (run everything from the repository root):
- `git submodule update --init --recursive`: the game source is the nested submodule `src/Cpp` (`third_party/NFSIISE/.gitmodules:4`).
- Every target except `test-unit` and `clean` compiles: first `source <emsdk>/emsdk_env.sh`, where `<emsdk>` is the emsdk checkout; it puts `emcc`, `em++` and `em-config` on `PATH` (observed emcc 6.0.12). `emsdk` and these tools run on Python 3 (observed 3.13.5).
- Node 20 or later (`package.json:8`; observed v20.19.2).
- Browser tests: `npm ci`, then `npx playwright install chromium`.

| Command | Expected evidence |
| ------- | ----------------- |
| `make` | Exit 0; `dist/` has `nfs2se.js`, `nfs2se.wasm`, the 14 page files, `LICENSE`, `NOTICE`, `LICENSE.emscripten`, `LICENSE.musl` and `LICENSE.compiler-rt`; `dist/files/` has the 8 support files and `manifest.txt` |
| `make test-c` | Each `build/tests/<name>.cjs` path, then `N checks, 0 failed`: `config` 117, `dinput` 549, `fmt` 23, `glide` 306, `kernel32` 87, `records` 352, `timer` 1, `user32` 2164, `wrapper` 40 |
| `make build/tests/<name>.cjs && node build/tests/<name>.cjs` | One C test through the pattern rule (`Makefile:111`); there are no per-test phony targets |
| `make test-unit`; one file: `node --test tests/unit/<file>.test.mjs` | `# tests 54`, `# fail 0` for the suite |
| `make test-e2e`; one spec, after `make`: `npx --no playwright test tests/e2e/<file>.spec.mjs` | `28 passed` for the suite |
| `make test` | All of the above; exit 0 |
| `node tools/serve.mjs dist 8123`, then `curl -sI http://localhost:8123/` | `200 OK` with the COOP, COEP and `Cache-Control` headers [above](#development-server) |
| `curl -s -o /dev/null -w '%{http_code}' -X POST http://localhost:8123/` | `405` |
| `curl -s -o /dev/null -w '%{http_code}' --path-as-is 'http://localhost:8123/..%2f..%2fMakefile'` | `404`; the only check that reaches the root test (`tools/serve.mjs:37`) |
| `make clean` | `build/` and `dist/` removed (`Makefile:128`) |

**Evidence map:**
- Every response has the isolation headers and its type; other methods get 405: `tests/unit/serve.test.mjs:34`.
- Missing files and bad escapes get 404: `tests/unit/serve.test.mjs:52`.
- File errors become 500 and the server keeps running: `tests/unit/serve.test.mjs:57`; the unreadable-file check is skipped as root (`:59`).
- The served page is cross-origin isolated: `tests/e2e/launcher.spec.mjs:11`; `openBlankPage` asserts it for blank pages (`tests/e2e/helpers.mjs:85`).
- Page errors fail every spec: `tests/e2e/helpers.mjs:24`.
- `manifest.txt` and the support files reach `/game`, and the template reaches the save directory: `tests/e2e/game.spec.mjs:41`. A support file that cannot be loaded stops the start: `:58`.
- The header and `layout.js` constants agree: `tests/unit/layout.test.mjs:30`.

**Coverage gaps:**
- `make`, the link flags, `--fatal-warnings` and the notices in `dist/` are checked only by building; a missing toolchain notice fails `make` (`Makefile:51`).
- The service worker and the reload in `isolate` never run: the test server always sends COOP/COEP (`src/web/main.js:403`).
- URL parsing normalises the `%2e%2e/package.json` case of `tests/unit/serve.test.mjs:53` before `fileFor` sees it, so no test reaches the root check (`tools/serve.mjs:37`); only the encoded-slash curl check does.
- Browsers other than Chromium, and runs with `CI` set.

### Test inventory

| Test file | Owner | Cases |
| --------- | ----- | ----- |
| `tests/c/config_test.c` | [platform](platform.md) | `:23` test_defaults · `:44` test_parse · `:87` test_paths · `:113` test_fopen · `:141` test_game_dirs |
| `tests/c/dinput_test.c` | [input](input.md) | `:206` test_objects · `:258` test_enum · `:277` test_unplugged · `:298` test_buttons · `:321` test_keys · `:382` test_key_queue_full · `:401` test_hat · `:442` test_assign_delay · `:480` test_axes · `:551` test_menu_axes · `:593` test_mouse · `:680` test_process_axes |
| `tests/c/fmt_test.c` | [platform](platform.md) | `:48` test_integers · `:76` test_doubles · `:95` test_text · `:108` test_fields · `:129` test_special |
| `tests/c/glide_test.c` | [graphics](graphics.md) | `:218` test_triangles · `:246` test_lines · `:265` test_state · `:322` test_textures · `:379` test_palette_textures · `:426` test_fog · `:479` test_fog_generate · `:493` test_overflow · `:526` test_back_pressure · `:540` test_lfb |
| `tests/c/kernel32_test.c` | [platform](platform.md) | `:81` test_events · `:127` test_event_wakes_waiter (infinite and 10 s timeouts) · `:147` test_files · `:207` test_mapping · `:240` test_delete · `:249` test_directories |
| `tests/c/records_test.c` | [platform](platform.md) | `:112` test_defaults (LF and CRLF) · `:138` test_missing_defaults · `:147` test_clear · `:158` test_long_name · `:178` test_write_failure |
| `tests/c/timer_test.c` | [platform](platform.md) | `:39` test_start_failure |
| `tests/c/user32_test.c` | [input](input.md) | `:123` test_window · `:139` test_post_key · `:159` test_full_queue · `:176` test_post_message · `:189` test_wait · `:204` test_destroy · `:213` test_message_box |
| `tests/c/wrapper_test.c` | [platform](platform.md) | `:120` test_init_seeds_settings · `:136` test_init_keeps_settings · `:148` test_files · `:167` test_init_failed_copy · `:199` test_exit_procs |
| `tests/unit/layout.test.mjs` | [shared memory](shared-memory.md) | `:30` every header constant has the same value in layout.js · `:37` layout.js exports nothing the header lacks · `:43` derived sizes keep the slot and ring arithmetic aligned |
| `tests/unit/renderer.test.mjs` | [graphics](graphics.md) | `:8` fitViewport centres the largest 4:3 picture · `:16` fitViewport fills the area when the aspect ratio is not kept · `:20` clearDepth turns the 4.12 floating-point W value into 1/w · `:31` slotViews alias one graphics slot as bytes, int32 and float32 |
| `tests/unit/input.test.mjs` | [input](input.md) | `:58` KEYMAP holds Windows virtual-key codes and set 1 scancodes · `:85` extended keys share the scancode of their base key; base scancodes are unique · `:99` charCode: printable Latin-1 characters and the control keys that type one · `:117` charCode: modifier and lock keys type nothing · `:125` pushMessage writes an entry, advances the head and bumps the sequence · `:134` pushMessage refuses a full ring and reuses consumed entries · `:147` pushMessage keeps working when the indices wrap around int32 · `:160` padState maps a standard gamepad to SDL axes, buttons and a hat · `:183` padState passes other gamepads through, clamped to six axes and twelve buttons · `:216` Input ignores the keyboard until it is enabled · `:222` Input turns key presses into WM_KEYDOWN, WM_CHAR and WM_KEYUP · `:234` Input drops releases of keys it did not see pressed and unknown keys · `:241` Input leaves the browser keys to the browser but still reports them · `:250` Alt+Enter toggles fullscreen once per press and reaches no game · `:258` disabling or blurring releases held keys and the mouse button · `:279` key releases the full ring refuses reach the game through later polls · `:299` pointer positions map into the letterboxed 640x480 picture once enabled · `:314` the left button latches until the game reads it; other buttons are ignored · `:329` poll publishes the first two connected gamepads while enabled |
| `tests/unit/audio-worklet.test.mjs` | [audio](audio.md) | `:42` the processor is registered as nfs2se-ring · `:46` frames are interpolated to the context rate and consumed · `:57` an underrun plays silence and keeps the unplayed frame · `:68` a fractional read position carries over between calls · `:82` reading wraps around the ring and the int32 counters · `:97` a stopped ring drops what is queued and plays silence |
| `tests/unit/settings.test.mjs` | [page](page.md) | `:25` the defaults are frozen and parsing returns fresh copies · `:34` parseSettings falls back to the defaults for unusable text · `:39` parseSettings takes stored values of the right type and range · `:52` loadSettings and saveSettings use localStorage when it works · `:62` settings still work without localStorage or when it throws · `:72` bindSettingsForm shows the settings and stores every change |
| `tests/unit/importer.test.mjs` | [game files](game-files.md) | `:142` zipEntries lists stored and deflated files and skips directories · `:157` zipEntries decodes UTF-8 names when flagged and code page 1252 otherwise · `:165` zipEntries reads ZIP64 directories and extra fields · `:175` zipEntries explains damaged, oversized, encrypted and unsupported archives · `:224` cdImage recognises cooked and raw images and rejects other files · `:235` isoEntries walks the directory tree of cooked and raw images · `:243` isoEntries explains damaged and oversized images · `:267` archiveEntries opens CD images and ZIP archives and names other files · `:275` folderEntries uses the path inside the chosen folder · `:306` droppedEntries walks dropped folders and opens a single dropped archive · `:328` selectGameFiles keeps the game trees under the shallowest root, in lower case · `:347` selectGameFiles drops paths with empty, "." or ".." parts · `:355` selectGameFiles explains missing, incomplete or oversized game folders |
| `tests/unit/serve.test.mjs` | [build and test](#development-server) | `:34` files are served with their type and the isolation headers · `:52` missing files and paths outside the root are not found · `:57` files that cannot be read are a server error and the server keeps running |
| `tests/e2e/launcher.spec.mjs` | [page](page.md), [game files](game-files.md) | `:11` the page is cross-origin isolated and waits for game files · `:19` a folder without the game data is rejected · `:28` imported game files are kept across visits until removed · `:51` a ZIP archive is imported through the picker · `:58` a ZIP archive that fails partway leaves no game files stored · `:70` a full disk is reported even when removing the partial import fails · `:94` a ZIP archive dropped on the page is imported · `:106` settings are saved and restored on the next visit |
| `tests/e2e/renderer.spec.mjs` | [graphics](graphics.md) | `:158` clears and flat triangles land inside the clip rectangle · `:177` the depth buffer holds 1/w: nearer wins, the depth function and mask apply · `:201` blending uses the game factors and the alpha test drops faint fragments · `:217` textures are sampled perspective-correct, modulate the colour and cut out · `:240` filtering and wrapping follow the game, the setting and the game config · `:258` fog mixes in the fog colour by the table entry at w = 1/oow · `:279` a frame recorded in two slots is presented once and gamma applies to it · `:300` the picture keeps 4:3 when the game and the setting ask and follows resizes · `:319` all picture improvements keep flat mid-grey and size the scene · `:334` textures and the fog table recorded while the context is lost survive it · `:360` a context that cannot be rebuilt after a loss fails the next pump |
| `tests/e2e/input.spec.mjs` | [input](input.md) | `:38` key presses become key and character messages with Windows codes · `:57` Alt+Enter asks for fullscreen and the game only sees Alt · `:64` the mouse maps into the 640x480 picture and the left button latches |
| `tests/e2e/audio.spec.mjs` | [audio](audio.md) | `:6` the worklet plays the shared ring at the game rate with the volume applied · `:38` pausing suspends the context and never rejects, even once it is closed |
| `tests/e2e/game.spec.mjs` | [page](page.md), [platform](platform.md) | `:41` the game runs, waits for its message box and ends · `:58` a support file that cannot be loaded stops the start with the reason · `:68` a failed save is shown when the game ends and retried until it works · `:90` a display that cannot be rebuilt after a context loss ends the game |

## Review and Refactor Guide

| Change | Check |
| ------ | ----- |
| Platform `.c` file added | `PLATFORM_OBJ` picks it up (`Makefile:28`); it must pass `-Werror` |
| `src/web` file added or removed | `WEB_FILES` copies it (`Makefile:29`); run `make clean` after a removal |
| Upstream support file added or removed | Edit `SUPPORT_FILES` (`Makefile:23`); `manifest.txt` is rewritten because it depends on the `Makefile` (`:69`) |
| C test added | Add `tests/c/<name>_test.c`, `TEST_SRC_<name>` and, if it needs threads, `TEST_CFLAGS_<name>` (`Makefile:94`–`:95`); stub the platform files it does not link |
| Unit test or spec added | The glob (`Makefile:122`) or `testDir` (`playwright.config.mjs:7`) finds it |
| Flags changed | Run `make clean`. Keep `--fatal-warnings` and the runtime methods `main.js` uses |
| `tools/serve.mjs` changed | Keep the headers on every response, the loopback binding and the root check; run `node --test tests/unit/serve.test.mjs` and the curl checks |
| COOP or COEP value changed | Change the server, the service worker and the test copy together ([above](#development-server)), and the values in `README.md` |
| Component added to `dist/`, or a library new to the link | List it and its licence in `NOTICE`; a licence file it requires gets a copy rule like `Makefile:80` and a place in `NOTICE_FILES` (`:25`) |
| Port changed | Edit only `playwright.config.mjs:4`; specs use relative URLs |
| Prerequisites, `make` targets, the `PORT` default (`Makefile:14`), the observed toolchain versions or the tested browsers changed | Update the matching text in `README.md` |

**Forbidden:** writing into `third_party/NFSIISE`; upstream files outside `dist/files/`; game data in tests; `test` straight from `@playwright/test`; one shared definition of the isolation headers.

**Proposals** (not accepted):

| Proposal | Benefit | Success check |
| -------- | ------- | ------------- |
| CI on a pinned emsdk and Node 20+, with an emcc version check | A gate; early failure | Green from a fresh clone; a wrong emcc stops clearly |
| A `serve.test.mjs` case with an encoded slash (`..%2f`) | A guarded root check | Removing the check at `tools/serve.mjs:37` fails a test |
| Firefox and WebKit ([Roadmap](../ARCHITECTURE.md#roadmap)) | Wider support | The suite passes in each |

## Open Gaps / Roadmap

- No CI, linter or formatter, and no pinned emsdk.
- A flag edit needs `make clean`; this and the other rebuild gaps are under [Build](#build).
- Outside CI, Playwright reuses whatever listens on 8123 (`playwright.config.mjs:26`).
- No test reaches the server's root check ([coverage gaps](#how-to-test)).
- A NUL byte in the path (`%00`) gets 500, not 404: Node rejects it with an error other than `ENOENT` (`tools/serve.mjs:62`, `:65`–`:66`).
- **Untested:**
  - Browsers other than Chromium.
  - `coi-sw.js`.
  - 3 of the 12 platform C files have no C test: `sound.c`, `web.c` and `wsock32.c`.

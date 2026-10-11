---
eatmycode_version: "1.2.0"
---
# nfs2se-web: Need for Speed II SE in the browser

A browser port of the NFSIISE C++ translation of Need for Speed II SE. The translated game compiles to WebAssembly. A C platform layer replaces Win32, DirectX, EA sound and 3dfx Glide. JavaScript runs the page, the WebGL2 renderer, input, audio and storage.

## Mission and Constraints

- **Purpose (required):** play Need for Speed II SE in a browser as native WebAssembly, with no x86 emulator. Graphics improve on the 640×480 original ([graphics](ARCHITECTURE/graphics.md)):
  - The scene renders at the picture's size on the canvas, at least 640×480 and at most 3840 pixels wide, unless the `resolution` setting asks for 640×480 or half size (`src/web/renderer.js:270`).
  - `antialias` means 4× MSAA where supported, else an FXAA-like post pass (`renderer.js:282`, `:553`). Sharpening, bloom and a colour grade are optional (`src/web/postfx.js:93`, `:101`, `:86`).
- **Observable behaviour (observed in `tests/e2e/`):**
  - The launcher imports the player's own copy of the game into IndexedDB: a folder, drag and drop, a ZIP, or an ISO 9660 CD image.
  - Play runs the game in a canvas. Settings change rendering and volume live.
  - The game's message boxes become page dialogs.
  - Imports and page settings survive reloads; the game's settings file is stored in IndexedDB.
- **Supported platforms:** browsers with WebAssembly threads (SharedArrayBuffer under cross-origin isolation), WebGL2 and AudioWorklet. Only Chromium is tested (`playwright.config.mjs:15`). Isolation comes from COOP/COEP headers (`tools/serve.mjs:22`), or from the `src/web/coi-sw.js` service worker on hosts that cannot send them (`src/web/main.js:75`).
- **Code-scope non-goals:**
  - Shipping, downloading or synthesising real game data.
  - Network play: Winsock reports the network as unavailable and every socket call fails (`src/platform/wsock32.c:1`).
  - Serial-link play.
  - x86 emulation.
- **Legal constraints (required):**
  - The translated game `third_party/NFSIISE/src/Cpp/NFS2SE.cpp` (a git submodule) is not MIT. It is compiled at build time and never copied into this repository.
  - `make` copies the upstream support files `install.win`, `text.*` and `nfs2se.conf.template` only into the gitignored `dist/files/` (`Makefile:23`, `:65`).
  - `src/platform` re-implements the behaviour of NFSIISE's MIT wrapper (zaps166) in original code. Never paste upstream source.
  - Tables with a mathematical definition are computed (fog, `src/platform/glide.c:341`, `:607`).
  - Credits live in `NOTICE`. `make` copies it, `LICENSE` and the toolchain licences into `dist/` (`Makefile:25`). The project licence is MIT (`LICENSE`).
- **Compatibility:**
  - The game draws in 640×480 game units (`src/platform/web_layout.h:16`).
  - `nfs2se.conf` keeps the upstream key format. Keys without a browser equivalent are ignored (`src/platform/config.c:98`).
  - Storage names must not change, or existing imports, saves and settings are orphaned: IndexedDB `nfs2se` (`src/web/storage.js:5`), the IDBFS database named after `/home/web_user/.nfs2se` (`src/web/main.js:15`), and localStorage `nfs2se-settings` (`src/web/settings.js:4`).
- **Implementation limits (observed):**
  - wasm32 memory: 256 MiB initial, 2 GiB maximum (`Makefile:44`). Eight pre-started pthread workers (`Makefile:43`, reason at `:36`).
  - Square textures up to 256×256 (`web_layout.h:184`); only the largest mip level is used (`src/platform/glide.c:673`).
  - Linear-frame-buffer writes are dropped, so the FMV movies are not shown (`glide.c:638`).
- **Verification status:** no game data exists here. Play with real data is unverified (see [Roadmap](#roadmap)).

## Languages and Toolchain

| Area | Language | Flags and evidence |
| ---- | -------- | ------------------ |
| `third_party/NFSIISE/src/Cpp/NFS2SE.cpp` (submodule) | C++14 | `-O2 -fno-rtti -fno-exceptions -pthread` and more (`Makefile:31`) |
| `src/platform/*.c`, `*.h` | C, GNU11 | `-O2 -pthread -Wall -Wextra -Werror` (`Makefile:33`) |
| `tests/c/*_test.c` | C, GNU11, wasm32 under Node | `-O1 -Wall -Wextra -Werror -sENVIRONMENT=node` (`Makefile:113`) |
| `src/web/*.js`, `tools/*.mjs`, `tests/**/*.mjs`, `playwright.config.mjs` | JavaScript ES modules, no bundler | `"type": "module"` (`package.json`); `src/web` is copied to `dist/` unchanged (`Makefile:86`) |
| Shader strings in `src/web/renderer.js`, `postfx.js`, `glutil.js` | GLSL ES 3.00 | `#version 300 es` (`src/web/renderer.js:21`) |
| `src/web/index.html`, `style.css` | HTML, CSS | none |

- **Declared support:** `package.json` requires Node `>=20`. Its only dependency is the dev-only `@playwright/test` `^1.64.0`; the lockfile resolves 1.64.0. No Emscripten or make version is pinned, and there is no CI configuration.
- **Observed locally on 2026-10-11:** emcc 6.0.12, Node v20.19.2, Playwright 1.64.0, GNU make. Python 3.13.5 runs `emsdk`, `emcc` and the doc size check.
- **Link settings:** `Makefile:42`, with reasons in the comments above it and in [build and test](ARCHITECTURE/build-and-test.md). They include `PROXY_TO_PTHREAD`, `INVOKE_RUN=0`, `EXIT_RUNTIME=1`, `MODULARIZE`/`EXPORT_ES6` (`createNfs2se`) and `-lidbfs.js`. `-Wl,--fatal-warnings` turns an import with a mismatched signature into a link error instead of a trapping stub (`Makefile:34`).

## System Design

```
browser main thread (src/web)                 wasm workers (PROXY_TO_PTHREAD)
  main.js      session, dialogs, persistence    NFS2SE.cpp   translated game (submodule)
  importer.js, storage.js  IndexedDB              | int32_t imports (Win32, DirectX, Glide, EA sound)
  settings.js  localStorage                       v
  renderer.js -> postfx.js, glutil.js  <-- GFX -- glide.c                 src/platform/*.c
  input.js                             -- CTL --> user32.c, dinput.c
  audio.js -> audio-worklet.js         <- AUDIO - sound.c
        layout.js  ==  shared regions (static arrays in web.c)  ==  web_layout.h
```

- **Game to platform:**
  - The game declares every import `extern "C" int32_t f(int32_t...)`. Each platform definition therefore returns `int32_t` and takes 32-bit arguments (`src/platform/platform.h:3`).
  - Game code is entered with its `void *game` thread context: a `GameFn` directly (`kernel32.c:122`, `wrapper.c:61`, `sound.c:114`), functions with arguments through `wrap_regparm2`, `wrap_stdcall2_ret`, `wrap_stdcall4` and `sub_41B250` (`platform.h:22`).
- **Platform layer:** `src/platform` owns the Win32, DirectX, Glide and EA-sound emulation state, and every C-side write to the shared regions.
- **Shared regions** ([shared memory](ARCHITECTURE/shared-memory.md)):
  - The control block, two graphics slots and the audio ring are static arrays (`src/platform/web.c:19`).
  - JS gets their addresses from `web_shared` and views them on the wasm SharedArrayBuffer (`src/web/main.js:286`).
  - `web_layout.h` and `src/web/layout.js` must agree, enforced by `tests/unit/layout.test.mjs:30`.
- **Page:** `src/web` owns the DOM, the WebGL2 context, the AudioContext, IndexedDB and the session lifecycle.
- **Calls across the wasm boundary (observed; keep):**
  - JS calls only `module._web_shared`, `module.callMain` and Emscripten's `FS` (`main.js:286`, `:310`).
  - C reaches the page only through `Module.nfsMessageBox` (`src/platform/web.c:76`).
  - Everything else is shared memory with Atomics. The page never blocks on the game; game threads may block on futexes (`web.c:1`).
- **Forbidden coupling (required):**
  - `src/platform` includes only its own and system headers, never upstream files.
  - JS never reads game memory outside the shared regions.
  - A layout constant changes in both mirrors in the same change.
  - Renderer and `glide.c` start from the defaults in `web_layout.h:196`.
- **Trust boundaries:**
  - Imported archives and CD images are untrusted. `src/web/importer.js` checks every entry against the archive or image, caps entries, sizes and folder depth (`importer.js:20`) and stops inflating past an entry's listed size (`:185`). A failed import is cleared (`src/web/storage.js:65`). Tests: `tests/unit/importer.test.mjs`.
  - Game data is parsed by the translated game inside the wasm sandbox.
  - `nfs2se.conf` is parsed by `src/platform/config.c:118`.
  - `tools/serve.mjs` listens on the loopback interface only, answers only GET and HEAD, and rejects paths outside its root (`tools/serve.mjs:86`, `:45`, `:29`).
  - `src/web/coi-sw.js:10` only rewrites same-origin responses.
- **Recorded decisions:**
  - Glide calls are recorded into command slots instead of calling WebGL from workers (`src/platform/glide.c:1`).
  - Deliberate differences from upstream's OpenGL renderer are listed at `glide.c:12`.
  - Keys travel through a futex-backed Windows message ring (`web_layout.h:27`, `:84`).

## Runtime and Data Flow

1. **Boot** (`src/web/main.js:401`):
   - Requires `crossOriginIsolated`. Without it, registers `coi-sw.js` and reloads once (`main.js:75`), then explains what is missing (`:407`).
   - Warns without WebGL2 (`:412`), starts `createNfs2se` with the `nfsMessageBox`, `onExit` and `onAbort` hooks (`:414`) and opens IndexedDB (`:421`).
2. **Import** (`main.js:91`): `selectGameFiles` keeps `gamedata/` and `fedata/` (`src/web/importer.js:286`). `storeGameFiles` writes the blobs, then the meta record last, and clears the store if any step fails (`src/web/storage.js:48`).
3. **Play** (`main.js:272`):
   - Creates the AudioContext inside the click (`:279`), views the shared regions and creates the `Renderer`.
   - `populateFiles` (`:130`) writes the support files listed in `files/manifest.txt` (`Makefile:69`) and the stored game files into MEMFS `/game`, then mounts and loads IDBFS at `/home/web_user/.nfs2se`.
   - Starts audio, `Input`, the frame loop (`main.js:248`) and the 1 s persistence timer (`:309`), then calls `callMain` with both directories (`main.js:310`).
4. **Game start:**
   - `main` (`src/platform/web.c:114`) enters `argv[1]` and keeps `argv[2]` as the save directory. Without them it shows a message box and exits with status 1.
   - `WrapperInit` (`src/platform/wrapper.c:111`) creates the save tree, seeds and parses `nfs2se.conf`, and publishes the `CTL_CFG_*` words.
   - `WrapperCreateWindow` (`wrapper.c:150`) validates the game directories (on error: message box, exit status −1).
5. **Frames:**
   - Glide calls fill the open slot. `grBufferSwap` publishes it as `GFX_FRAME` and waits for the other slot, so the game is at most two frames ahead (`src/platform/glide.c:428`).
   - `Renderer.pump` (`src/web/renderer.js:171`) runs once per animation frame. It executes published slots in order and frees each with `Atomics.notify`.
6. **Input:**
   - DOM events become Windows messages in the control-block ring, plus mouse and pad words (`src/web/input.js:59`).
   - `GetMessageA_wrap` waits on `CTL_SEQ` (`src/platform/user32.c:108`). DirectInput reads the pad words for joysticks (`src/platform/dinput.c:405`) and the mouse words (`dinput.c:467`).
7. **Audio:**
   - With `AUD_CONSUMER` set by the page, `iSNDdirectstart_` starts a producer thread that fills the ring through the game's mixer (`src/platform/sound.c:83`).
   - The page asks for an AudioContext at the ring's 22050 Hz (`src/web/audio.js:10`). The AudioWorklet resamples linearly only at other rates (`src/web/audio-worklet.js:13`).
8. **Persistence:**
   - Closing a writable file or deleting a file sets `CTL_FS_DIRTY` (`src/platform/kernel32.c:520`, `:502`; `web.c:91`).
   - `persist` (`main.js:148`) runs `FS.syncfs` only while the flag is set and no sync runs. It is called every second, when the tab is hidden, on `pagehide` and at session end (`:309`, `:387`, `:394`, `:199`).
   - A failed save sets the flag again and shows a warning in the ended panel (`main.js:157`, `:164`).
   - Game files are rewritten from IndexedDB into MEMFS on every Play.
9. **Configuration:** game settings live in `~/.nfs2se/nfs2se.conf`, parsed by `WrapperInit` (`wrapper.c:137`). Page settings live in localStorage and apply live (`src/web/settings.js:1`).
10. **Failure and recovery:**
    - A message box blocks its game thread until the page dialog answers. Concurrent boxes wait their turn (`web.c:71`).
    - After WebGL context loss, the next `pump` rebuilds the GL objects and re-uploads kept textures (`renderer.js:172`, `:253`).
    - `abort` and renderer errors end the session with a message (`main.js:417`, `:257`). So does a failed Play, after which the buttons stay disabled until a reload (`:312`).
    - A load failure (`main.js:419`) and a full disk during import (`:105`) show messages.
11. **Shutdown:**
    - `ExitProcess_wrap` runs the game's exit procedures, each under a 2.5 s watchdog, waits up to 2.5 s for the window to close, then exits (`src/platform/kernel32.c:631`, `wrapper.c:69`).
    - `onExit` calls `endSession` (`main.js:416`, `:199`), which disables input, persists, pauses audio and shows the ended panel. Playing again needs a reload.

**Concurrency invariants:**
- Control words use sequentially consistent atomics on both sides (`src/platform/platform.h:112`, JS `Atomics`). Ring entries are plain stores, published by the atomic store of the head (`input.js:64`).
- Each ring has one producer and one consumer: JS owns the input head and C its tail; C owns the audio write count and the worklet its read count.
- A graphics slot belongs to C while `GFX_FREE` or `GFX_WRITING`, and to the renderer while `GFX_PARTIAL` or `GFX_FRAME`.

## Workspace Map

| Path | Contents | Rules |
| ---- | -------- | ----- |
| `src/platform/` | C platform layer, `platform.h`, `web_layout.h` | Original code; keep `web_layout.h` and `src/web/layout.js` in step |
| `src/web/` | Page modules, `index.html`, `style.css`, `coi-sw.js` | Copied to `dist/` as-is; relative `./x.js` imports |
| `third_party/NFSIISE/` | Submodule pinned at `66cbaf4`: `src/Cpp/NFS2SE.cpp` (nested submodule), `Need For Speed II SE/` | Never edit or vendor; only move the pin (`git submodule update --init --recursive`) |
| `tests/c/` | C tests and `check.h` | Sources per test in `TEST_SRC_<name>` (`Makefile:94`) |
| `tests/unit/`, `tests/e2e/`, `tests/support/` | `node --test` tests of `src/web` and `tools/serve.mjs`; Playwright specs against `dist/`; ZIP fixture builder | Fixtures are generated bytes, never game data |
| `tools/serve.mjs` | Static server with COOP/COEP | Development and tests |
| `Makefile`, `package.json`, `package-lock.json`, `playwright.config.mjs` | Build and test configuration | [build and test](ARCHITECTURE/build-and-test.md) |
| `build/`, `dist/`, `node_modules/`, `test-results/`, `playwright-report/` | Generated, gitignored | `make`, `npm ci`; `make clean` |
| `ARCHITECTURE.md`, `ARCHITECTURE/` | These docs | `CLAUDE.md`, `AGENTS.md` are symlinks to `ARCHITECTURE.md` |
| `LICENSE`, `NOTICE`, `.gitmodules`, `.gitignore` | Licence, credits, submodule URL, ignores | List every new third-party component in `NOTICE` |

## Coding Style and Code Design

No formatter, linter or type checker is configured. Enforced: C warnings as errors (`Makefile:33`), the import-signature link check (`Makefile:42`) and the layout test. The rest is observed convention:

- **All files:**
  - Tab indentation in C, JS, HTML, CSS and the Makefile.
  - Comments are full sentences stating behaviour, contracts or differences from upstream, placed above the code (`src/platform/glide.c:139`, `src/web/main.js:127`).
- **C:**
  - Braces on their own lines (`src/platform/web.c:28`); `static` helpers in `snake_case` (`glide.c:140`), file statics with an `s_` prefix (`web.c:19`), constants in `UPPER_SNAKE` (`glide.c:23`).
  - Imports keep the names the game links against: a `_wrap` suffix (`src/platform/kernel32.c:128`), or exact names such as `grBufferSwap`.
  - Unused parameters are cast to `(void)` (`glide.c:384`).
  - Shared words use `ctl_load`/`ctl_store` or `__atomic` builtins (`platform.h:112`, `glide.c:144`).
- **JS:** named ES exports; `camelCase` functions, `PascalCase` classes, `UPPER_SNAKE` constants (`src/web/renderer.js:64`); single quotes and semicolons; `Atomics` for every shared word (`main.js:150`).
- **Errors:**
  - Imports fail the Windows way: zero or false, `INVALID_HANDLE_VALUE`, or DirectInput codes (`src/platform/dinput.c:23`).
  - Fatal conditions call `wrapper_fatal`, which shows a message box, then exits (`wrapper.c:24`).
  - The page shows actionable text with `showMessage(text, true)` and `errorText` (`main.js:30`, `:36`). A missing optional feature only logs `console.warn` (`main.js:281`).
  - C prints only the banner, message-box text and a missing-config notice (`wrapper.c:115`, `web.c:74`, `wrapper.c:142`).
- **Tests:**
  - C: `CHECK`, `CHECK_INT`, `CHECK_STR` and `check_exit` (`tests/c/check.h:12`). Unit: `node:test` with `node:assert/strict` (`tests/unit/layout.test.mjs:2`).
  - Browser: the `test` fixture fails on uncaught page errors (`tests/e2e/helpers.mjs:24`). Game files are generated dummies (`helpers.mjs:8`).
- **Reuse:** `put_command` and `sync_word` for graphics ops (`glide.c:175`, `:186`); `web_futex_wait` and `web_futex_wake` (`web.c:58`, `:63`); `createProgram` and `createTarget` (`src/web/glutil.js:3`, `:39`); `buildZip` (`tests/support/zip.mjs:19`).

## Verification and Review Map

**Prerequisites:** Activated emsdk (`emcc`, `em++`, `em-config` on `PATH`); `npm ci`, then `npx playwright install chromium`; `git submodule update --init --recursive`. Run all commands from the repository root.

| Command | Does | Expected evidence |
| ------- | ---- | ----------------- |
| `make` | Builds `dist/` | `dist/nfs2se.js`, `dist/nfs2se.wasm`, `dist/files/*`, `dist/LICENSE*`, `dist/NOTICE` |
| `make serve` | Builds, then serves `dist/` on `PORT` (8080) with COOP/COEP | `Serving … at http://localhost:8080/` |
| `make test-c` | Builds and runs each `tests/c/*_test.c` as wasm32 under Node | One `N checks, 0 failed` line per test |
| `make test-unit` | `node --test tests/unit/*.test.mjs` | `# fail 0` |
| `make test-e2e` | Builds, then runs `npx --no playwright test` against `node tools/serve.mjs dist 8123` (started or reused) | `N passed` |
| `make test` | All three | Exit 0 |
| `python3 -c 'from pathlib import Path; [print(len(p.read_bytes().decode()), p) for p in [Path("ARCHITECTURE.md"), *sorted(Path("ARCHITECTURE").rglob("*.md"))]]'` | Doc size check | Every count ≤ 35000 |

| Change | Run |
| ------ | --- |
| `web_layout.h`, `layout.js`, `Makefile` link flags | `make test` |
| `glide.c`, `renderer.js`, `postfx.js`, `glutil.js` | `glide_test`, `renderer.test.mjs`, `renderer.spec.mjs` |
| `input.js`, `user32.c`, `dinput.c` | `user32_test`, `dinput_test`, `input.test.mjs`, `input.spec.mjs`, `game.spec.mjs` |
| `sound.c`, `audio.js`, `audio-worklet.js` | `audio-worklet.test.mjs`, `audio.spec.mjs`, `game.spec.mjs` |
| `config.c`, `fmt_x86.c`, `kernel32.c`, `records.c`, `timer.c`, `wrapper.c` | The matching `tests/c/*_test.c` (`make test-c`), then `game.spec.mjs` |
| `web.c`, `wsock32.c` | `game.spec.mjs` (the only test that runs them) |
| `main.js`, `index.html`, `style.css`, `settings.js`, `coi-sw.js` | `settings.test.mjs`, `launcher.spec.mjs`, `game.spec.mjs` |
| `importer.js`, `storage.js` | `importer.test.mjs`, `launcher.spec.mjs` |
| `tools/serve.mjs` | `serve.test.mjs`, then `make test-e2e` |

**Review constraints:** the [Project-Specific Deviations](#project-specific-deviations), and C stays clean under `-Werror`.

**Coverage gaps:**
- Real game data: menus, races, saves and frame rate.
- Browsers other than Chromium.
- Directory drops: a synthetic `DataTransfer` cannot carry entries.
- No C tests for `sound.c`, `web.c` and `wsock32.c`. The slot handshake is tested single-threaded only (`tests/c/glide_test.c:3`).
- `coi-sw.js` and the reload in `isolate` never run: the test server always sends COOP/COEP.
- Performance benchmarks.

## Roadmap

No milestone IDs are established. Status by owner:
- [Graphics](ARCHITECTURE/graphics.md) is in progress: movies and several Glide features are missing.
- [Platform](ARCHITECTURE/platform.md) is done; its behaviour with real data is unverified.
- All other owners are done.

**Accepted work.** Verify a full play session with real game data, which the goal of a playable game requires.
- Needs: a player-supplied copy of the game.
- Owners: all subsystems.
- Success check: import, menus, a race, and settings and records kept after a reload, with no page errors.

**Known coding gaps** (details in the owner docs):
- LFB writes are dropped, so movies are not shown (`src/platform/glide.c:638`).
- Glide state without effect: colour combine, alpha test, chroma key, culling, depth bias, depth-buffer mode, dithering, mipmap mode and texture combine (`glide.c:512`–`:576`, `:745`–`:765`).
- Network and serial play are absent, and force feedback is accepted but not rendered.
- Platform shortcuts: `WrapperAtExit` keeps 10 procedures (`src/platform/wrapper.c:32`), writes through file views never reach the file (`src/platform/kernel32.c:418`), and `CreateFileA` ignores the creation disposition (`kernel32.c:328`). More in the [platform doc](ARCHITECTURE/platform.md).
- Gamepad words are stored one by one, so the game can read a mix of two polls ([input](ARCHITECTURE/input.md)).

**Proposals (not accepted):**
- **Movies** (graphics): upload LFB writes as a texture, keeping the slot protocol. Success: the intro movie is visible, with `glide_test` coverage.
- **File system** (platform, page): WasmFS instead of MEMFS file calls proxied to the main thread. Success: IDBFS persistence still passes `game.spec.mjs`.
- **Saves** (page): export and import `~/.nfs2se`. Success: an e2e round trip with a generated save.
- **Browsers** (build and test): Firefox and WebKit Playwright projects. Success: green per project.
- **C tests** (platform, audio, shared memory): cover `sound.c`, the `web.c` message box and a two-thread slot handshake. Success: new `tests/c/*_test.c` pass in `make test-c`.

## Development Loop

Frame → Write → Prove → Review → Gate. Findings return to Write;
uncertainty that changes the plan returns to Frame.

Use one subagent per role when available, otherwise distinct labeled
passes. Tester and Verifier report findings and never edit; Coder repairs.

| Role | Stages | Handoff |
| ---- | ------ | ------- |
| Planner | Frame | Goal, observable checks, assumptions, affected files/owners, and plan. |
| Coder | Write | Planned changes or repairs to named findings. |
| Tester | Prove | Commands, results, and behavioral/structural evidence. |
| Verifier | Review + Gate | Evidence-backed findings or verified completion. |

### The loop

1. **Frame:** Inspect the request, code, docs, and conventions before
   planning. Give the goal and each plan step an observable check. When
   using eatmycode, run its Version and Freshness Gate before trusting
   architecture; include versions, migration scope, Index/agent-file
   changes, and verification commands in architecture plans. Resolve
   uncertainty from evidence and record the narrowest supported assumptions.
   Only Planner may ask one focused question, when a required decision
   cannot be discovered or safely inferred and guessing changes the result.
2. **Write:** Apply Coding Discipline. Make the planned change; for a
   repair, address only named findings. Update affected architecture with
   changes to its documented contracts.
3. **Prove:** Run relevant tests and structural checks, retaining observable
   evidence. For architecture work under eatmycode, apply its Architecture
   Verification. Failures and missing, duplicate, or obsolete coverage
   become Coder findings. Re-run affected checks after repairs; never send
   a red result to Review.
4. **Review:** Apply every Review Check as a separate pass over full affected
   files. Use an independent agent or isolated pass for Fit, Dependencies,
   and Security when available. Return findings to Coder, then re-prove
   and re-review the repairs.
5. **Gate:** Confirm completion only when the Definition of Done passes.
   Return unmet criteria to the responsible stage; continue until resolved.
   If an external constraint prevents verification, state the missing
   evidence and remaining work without claiming completion or readiness.

Handoffs are automatic. Continue without pauses for plan approval,
permission to continue, or review/reporting ceremonies. Finish with the
harness's normal concise completion handoff.

### Definition of Done

- **Correctness:** The goal and named checks pass. Tests cover claimed
  behavior; bug fixes have a reproducing regression test. The project
  builds and tests from a fresh clone without local-only dependencies.
  Owning modules' **How to Test** commands pass with evidence.
- **Review:** Every Review Check ran and its completion threshold passes.
- **Contract:** Docs reflect source and let an agent locate owners,
  constraints, and verification commands. When using eatmycode, architecture
  satisfies its Output Contract, verification, and version rules. Public
  names, signatures, errors, and recovery are intelligible. Breaking
  changes, deprecations, dependencies, licenses, and attribution are handled;
  commit or PR text, when present, explains why.
- **Scope:** Changed lines serve the goal and follow Coding Discipline;
  no debugging remnants, commented-out code, secrets, tokens, or local paths
  remain. Test edits follow the inventory and coverage rules below.

### Iterating without thrashing

- Each repair pass targets a named finding; nits alone do not trigger one.
- Two no-change passes force Gate re-evaluation. If Done still fails,
  return the surviving evidence to Frame.
- Three passes against the same finding return to Frame for a new approach.
- Never widen scope to satisfy a finding. Record coding follow-ups under
  **Open Gaps / Roadmap** and keep non-coding work outside architecture.

### Project-Specific Deviations

These add to the Definition of Done:

- **No game data or translated code:** no change adds Need for Speed II SE data, files from `third_party/NFSIISE` (only the submodule pin may move), or source copied from the upstream wrapper. Tests generate dummy bytes (`tests/e2e/helpers.mjs:8`, `tests/support/zip.mjs:1`).
- **Licences:** every new third-party component or dependency is listed in `NOTICE` with its licence.
- **Full suite:** `make test` passes; it includes the Playwright suite against a fresh `dist/`.
- **Layout mirrors:** `src/platform/web_layout.h` and `src/web/layout.js` change together, and `tests/unit/layout.test.mjs` passes.
- **Real-data claims:** behaviour that needs game data is described as unverified until a session with player-supplied data confirms it.

## Coding Discipline

- Implement only the goal. Prefer the simplest approach that passes its
  checks; simplify code materially larger than the problem.
- Match local style. Avoid speculative features, flexibility, single-use
  abstractions, and checks for impossible conditions.
- Keep edits surgical: no unrelated refactoring, reformatting, or cleanup.
  Remove imports, variables, and functions made unused by this change;
  leave pre-existing dead code alone unless requested.
- Make success concrete: validation rejects invalid input in a named test;
  a regression test fails before a bug fix and passes after; behavior tests
  pass before and after a refactor.

### Before editing tests

Before any test edit, including during Write, inventory the whole suite:
enumerate every test file and case name, then read in full tests whose
subject, fixtures, or assertions touch the change. Use a subagent for broad
inventory when supported. Plan all additions, changes, merges, and removals
from that evidence, citing `file:line`, before executing the test edits.

- **Reuse first:** Extend the test owning the behavior or sharing its
  setup, fixtures, and subject. Add a function/file only if no existing
  owner fits or merging would obscure which case failed.
- **Add only required coverage:** A bug fix needs its regression test;
  a capability needs a test of its claimed behavior. Avoid duplicates.
- **Retire only what changed:** Remove tests of deleted behavior and merge
  new duplicates, citing surviving coverage. Record unrelated suspected
  redundancy under **Open Gaps / Roadmap**.
- **Preserve coverage:** Never delete or weaken tests to turn red green.
  Removal needs evidence that behavior is gone or covered elsewhere;
  coverage of claimed behavior must not decrease.

## Review Checks

Run every check against every change before confirming a code edit is
complete, even when no commit or merge is requested. Keep checks separate.

- **Evidence or no finding:** Cite `file:line` for every finding.
- **Repository authority:** Demand only conventions supported by the tree.
- **Full context:** Read affected files, not only hunks; context can expose
  unreachable code, unused parameters, or hidden duplication.
- **Code and impact:** Review the change, never the author or how it was made.

### 1. Style and Naming

Check indentation and local conventions; leave machine-checkable formatting
to existing formatters/linters and never demand unrelated reformatting.
Mixed indentation is `major`; a consistent new file with the wrong local
indent is `nit`. Compare names with nearby precedents. If the repository
is inconsistent, demand nothing. A local naming mismatch is `nit`; an
inconsistent public name is `major`.

### 2. Duplication

Search distinctive constants, errors, fields, and call sequences, beyond
symbol names, for the same job. Cite both sites and a remedy. Cross-layer
duplication is `major`; small local repetition is `nit`. Similar code with
meaningfully different branches is not duplication.

### 3. Quality

Require followable control flow, errors handled where they occur, and
proportionate abstractions. Swallowed errors, inappropriate prints,
unexplained magic values, and dead branches are `major`. Remove unrequested
configurability, one-caller wrappers, filler comments, debugging remnants,
and unrelated formatting. Missing tests belong to Prove.

### 4. Fit

Read the root architecture and owning module before the diff. Check
language/toolchain constraints, conventions, scope, layering, ownership,
invariants, public-API growth, compatibility, and performance claims against
source. A layering violation or unjustified public API is `major`.
Architectural/public-behavior changes need matching docs in the same change.

### 5. Dependencies

Check manifests/imports, maintenance, supply-chain risk, advisories,
install-time behavior, license, transitive cost, and standard-library
alternatives. An unjustified top-level dependency is `major`; a live
advisory or abandoned upstream is `blocker`. Incomplete evidence does not pass.

### 6. Security

Check defects and widened exposure: unsafe memory access, unchecked sizes
or offsets, integer overflow, traversal, unsafe deserialization, command
construction, committed secrets, and unbounded untrusted input. Trace input
to impact; without a reachable path there is no finding. A real defect is
`major`; a trust-boundary break is `blocker`. Describe fixes without exploit
steps.

### Severity and the completion threshold

| Severity | Effect |
| -------- | ------ |
| `blocker` | Must not confirm completion or merge. |
| `major` | Must be resolved before confirming completion or merging. |
| `nit` | Apply or consciously decline. |
| `info` | Context or a question; no action implied. |

Confirm completion or merge only with no `blocker` or unresolved `major`.
A check that did not run does not pass; explain evidence-backed
inapplicability. Findings feed Write and Gate directly.

## Index

Each subsystem has one owner doc. Read the owner first, then its partners.

- **[Shared memory](ARCHITECTURE/shared-memory.md)**
  - Sources: `src/platform/web_layout.h`, `src/web/layout.js`, the regions and `web_shared` in `src/platform/web.c`, `tests/unit/layout.test.mjs`.
  - Owns: word offsets, slot states, graphics ops and the writer of each shared word.
  - Change for: new control words, ops, states, capacities or audio header fields.
  - Partners: platform, graphics, input, audio, page.
- **[Platform](ARCHITECTURE/platform.md)**
  - Sources: `src/platform/platform.h`, `web.c`, `wrapper.c`, `kernel32.c`, `timer.c`, `records.c`, `config.c`, `fmt_x86.c`, `wsock32.c`, and the game compile rule in `Makefile`.
  - Owns: the Win32 emulation the game links against (files, threads, synchronisation, timers, configuration, records, message boxes, exit).
  - Change for: missing or mismatched imports, file or thread behaviour, `nfs2se.conf` keys, start-up and shutdown.
  - Partners: shared memory, graphics, input, audio, build and test.
- **[Graphics](ARCHITECTURE/graphics.md)**
  - Sources: `src/platform/glide.c`, `src/web/renderer.js`, `src/web/postfx.js`, `src/web/glutil.js`.
  - Owns: recording Glide calls into slots, and the WebGL2 renderer and post-processing that play them.
  - Change for: drawing errors, textures, fog, depth, resolution, effects, context loss, movies.
  - Partners: shared memory, page.
- **[Input](ARCHITECTURE/input.md)**
  - Sources: `src/web/input.js`, `src/platform/user32.c`, `src/platform/dinput.c`.
  - Owns: keyboard, mouse and gamepad, from DOM events to window messages and DirectInput devices.
  - Change for: keys, focus, pointer, gamepads, force feedback, the message loop.
  - Partners: shared memory, page.
- **[Audio](ARCHITECTURE/audio.md)**
  - Sources: `src/platform/sound.c`, `src/web/audio.js`, `src/web/audio-worklet.js`.
  - Owns: the producer thread around the game's mixer, the PCM ring and the worklet player.
  - Change for: crackles, latency, sample rate, volume, pausing.
  - Partners: shared memory, page.
- **[Page](ARCHITECTURE/page.md)**
  - Sources: `src/web/main.js`, `src/web/index.html`, `src/web/style.css`, `src/web/settings.js`, `src/web/coi-sw.js`.
  - Owns: the launcher, cross-origin isolation, the session lifecycle, dialogs, persistence and settings.
  - Change for: UI, boot, Play, session end, saves, settings.
  - Partners: every subsystem.
- **[Game files](ARCHITECTURE/game-files.md)**
  - Sources: `src/web/importer.js`, `src/web/storage.js`.
  - Owns: reading the player's folder, ZIP or CD image, and keeping the selected files in IndexedDB.
  - Change for: archive or image formats, file selection, the storage schema.
  - Partners: page.
- **[Build and test](ARCHITECTURE/build-and-test.md)**
  - Sources: `Makefile`, `tools/serve.mjs`, `package.json`, `package-lock.json`, `playwright.config.mjs`, `.gitmodules`, `.gitignore`, `tests/c/check.h`, `tests/e2e/helpers.mjs`, `tests/support/zip.mjs`, `tests/unit/serve.test.mjs`.
  - Owns: the wasm and `dist/` build, the development server and the test harnesses.
  - Change for: compiler or link flags, new sources or tests, server headers, CI.
  - Partners: every subsystem.

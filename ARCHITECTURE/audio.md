---
eatmycode_version: "1.2.0"
---
# Audio

## Goal

This module owns the game's sound path:
- the EA sound DLL imports that the translated game calls (`src/platform/sound.c`);
- the producer thread that runs the game's own software mixer;
- the page side that plays the shared PCM ring through an AudioWorklet (`src/web/audio.js`, `src/web/audio-worklet.js`).

**Non-goals:**
- Mixing, decoding or effects. The game's mixer produces every sample (`sound.c:1`).
- Sound in browsers without AudioWorklet. The game then runs silent (`main.js:296`).
- Honouring `LinearSoundInterpolation` (`sound.c:6`). The config parser ignores the key too (`config.c:98`–`:99`).

The ring's word layout belongs to [shared memory](shared-memory.md). The AudioContext's lifetime and the volume setting belong to [page](page.md). This module is neither infrastructure nor scaffolding.

## Status

`done`. The import surface, producer thread, ring player and volume control are implemented.

**Evidence:**
- On 2026-10-11, `make test` exited 0. Run on their own, `node --test tests/unit/audio-worklet.test.mjs` passed 6 of 6, `tests/e2e/audio.spec.mjs` printed `2 passed` and `tests/e2e/game.spec.mjs` `4 passed`.

**Unverified:**
- Sound with real game data. There is none on this machine.
- The C side. No automated test checks it: `tests/e2e/game.spec.mjs` connects audio when starting the game but asserts nothing about sound.

## Code Structure

| File | Role |
| ---- | ---- |
| `src/platform/sound.c` | EA sound imports, producer thread, writes to the ring |
| `src/web/audio.js` | AudioContext creation, worklet node, gain, pause |
| `src/web/audio-worklet.js` | `RingPlayer` processor that reads the ring |
| `tests/unit/audio-worklet.test.mjs` | `RingPlayer` under Node with stand-in worklet globals |
| `tests/e2e/audio.spec.mjs` | Worklet, ring and gain in Chromium with an `OfflineAudioContext` |

## Language and Conventions

- C GNU11 and JS ES modules follow the root rules ([Coding Style and Code Design](../ARCHITECTURE.md#coding-style-and-code-design)). C warnings are errors (`Makefile:33`).
- Import names are exactly what `NFS2SE.cpp` links against: a trailing underscore (`sound.c:62`, `sound.c:83`) or the `_wrap` suffix (`sound.c:67`). Each one returns `int32_t` (`platform.h:3`).
- **Atomics:**
  - C accesses ring header words only through `ring_load` and `ring_store` (`sound.c:27`, `sound.c:32`). These are sequentially consistent `__atomic` helpers, the audio counterpart of `ctl_load` (`platform.h:113`).
  - The producer flag `s_producing` is a C11 `atomic_bool` (`sound.c:23`).
  - The worklet reads every header word with `Atomics.load` (`audio-worklet.js:13`, `:20`, `:22`) and writes `AUD_READ` with `Atomics.store` (`:37`, `:42`). `startAudio` stores `AUD_CONSUMER` with `Atomics.store` (`audio.js:27`).
- `getSamples` runs only through `wrap_regparm2` with the `audio_game_thread` context (`sound.c:39`, `platform.h:25`, `platform.h:30`). `fadeInOut` is called directly as a `GameFn` with the serving thread's context (`sound.c:114`, `platform.h:20`).
- Waits use `web_futex_wait` and `web_futex_wake` (`web.c:58`, `web.c:63`).
- **Worklet naming and location:**
  - The processor name `nfs2se-ring` appears in `audio.js:22` and `audio-worklet.js:52`.
  - `audio.js:21` loads the worklet by a URL relative to `audio.js`, so the two files stay siblings in `dist/`.
- The unit test stubs `AudioWorkletProcessor`, `registerProcessor` and `sampleRate` before it imports the worklet (`audio-worklet.test.mjs:10`–`:13`).
- Enforced: compile flags, the static asserts (`sound.c:17`, `sound.c:18`) and the layout test. The rest is observed convention.

## Design and Invariants

**Pipeline:**
1. The game mixer (`getSamples`) runs on the producer thread.
2. The producer writes into the shared ring.
3. `RingPlayer` reads the ring on the audio rendering thread.
4. The output goes through a `GainNode` to the destination (`audio.js:26`).

Samples never wait for the browser main thread (`sound.c:4`).

**Imports** (`sound.c:62`–`sound.c:133`):
- `iSNDdllversion_` and `iSNDdirectcaps_` return the constants at `sound.c:13`–`:14`.
- `iSNDdirectsetfunctions_wrap` keeps the `getSamples` and `fadeInOut` callbacks and ignores the other three (`sound.c:67`).
- `iSNDdirectstart_` acts once until stopped (`sound.c:87`). It reads `AUD_CONSUMER` once and starts the producer only if it is set (`sound.c:91`).
- `iSNDdirectserve_` is called periodically by the game (`sound.c:104`).
  - It runs `fadeInOut` on the calling game thread (`sound.c:114`).
  - The first call after a start sets `AUD_ACTIVE`, but only when there is a consumer (`sound.c:111`).
  - Without a consumer it renders one chunk into scratch, so the mixer still advances (`sound.c:115`).
- `iSNDdirectstop_` clears `AUD_ACTIVE`, then wakes and joins the producer (`sound.c:126`, `sound.c:129`).

**Ring ownership** (the layout is in [shared memory](shared-memory.md)):
- The 16-word header holds `AUD_WRITE` 0, `AUD_READ` 1, `AUD_ACTIVE` 2, `AUD_RATE` 3 and `AUD_CONSUMER` 4 (`web_layout.h:212`–`:217`, mirrored at `layout.js:131`–`:136`). Interleaved int16 stereo frames follow it.
- Capacity, target and chunk size are compiled constants, not header words: `AUD_CAPACITY_FRAMES` 8192, `AUD_TARGET_FRAMES` 1536 and `AUD_CHUNK_FRAMES` 256, with `AUD_SAMPLE_RATE` 22050 (`web_layout.h:219`–`:222`).
- One producer and one consumer: only C writes `AUD_WRITE` (`sound.c:57`), and only the worklet writes `AUD_READ` (`audio-worklet.js:37`, `audio-worklet.js:42`).
- C writes `AUD_ACTIVE` (`sound.c:112`, `sound.c:126`). It is 1 while the worklet plays: from the first `iSNDdirectserve_` after `iSNDdirectstart_` to `iSNDdirectstop_` (`web_layout.h:214`).
- JS writes `AUD_CONSUMER` before `main()` (`audio.js:27`). `play` awaits it at `main.js:295`, before `callMain` at `main.js:310`.
- `AUD_RATE` only holds its static initial value, `AUD_SAMPLE_RATE` (`web.c:20`). The worklet reads it once (`audio-worklet.js:13`).

**Fill and indexing:**
- The counters are monotonic frame totals that wrap as int32. C uses uint32 arithmetic (`sound.c:49`, `sound.c:51`). The worklet uses `| 0` (`audio-worklet.js:23`, `audio-worklet.js:37`).
- The ring position is `counter & (AUD_CAPACITY_FRAMES - 1)` (`sound.c:56`, `audio-worklet.js:19`), so the capacity must be a power of two (`tests/unit/layout.test.mjs:47`).
- The producer renders a 256-frame chunk only while the ring is active and the chunk keeps at most `AUD_TARGET_FRAMES` (1536) queued (`sound.c:51`).
  - The target is at most the capacity (`sound.c:18`), so unread frames are never overwritten.
  - At most about 70 ms is queued at 22050 Hz.
- Chunks never wrap around the ring:
  - the capacity is a multiple of the chunk size (`sound.c:17`);
  - `AUD_WRITE` grows from 0 in chunk steps (`sound.c:57`).

**Wake-ups:**
- The worklet notifies `AUD_READ` after consuming or dropping frames (`audio-worklet.js:38`, `audio-worklet.js:43`).
- The producer futex-waits on that word for at most `PRODUCER_WAIT_MS`, 10 ms (`sound.c:15`, `sound.c:53`).
  - Activation sends no wake, so the timeout bounds how late the producer notices it.
  - `iSNDdirectstop_` wakes the producer (`sound.c:129`). The timeout covers a wake that arrives before the wait starts.

**Playback** (`audio-worklet.js:17`):
- `step = AUD_RATE / sampleRate` is fixed at construction (`audio-worklet.js:13`).
  - `createAudioContext` asks for a 22050 Hz context (`audio.js:10`). Then `step` is 1, the fraction `t` stays 0 and frames are copied unchanged; the browser resamples (`audio.js:9`).
  - If that constructor throws, the context runs at the default rate (`audio.js:12`). The worklet interpolates linearly only when the context rate differs from `AUD_SAMPLE_RATE` (`audio-worklet.js:30`).
- Frame `index` plays only while frame `index + 1` is also queued, so the newest frame waits for its successor. On underrun the rest of the quantum is silent (`audio-worklet.js:27`, `audio-worklet.js:46`).
- The fractional source position carries over to the next quantum (`audio-worklet.js:35`).
- While `AUD_ACTIVE` is 0, the worklet plays silence and drops whatever is queued by setting `AUD_READ` to `AUD_WRITE` (`audio-worklet.js:42`).

**Threads:**
- `getSamples` runs on the producer pthread with the `audio_game_thread` context. At the same time game threads call `fadeInOut` from `iSNDdirectserve_`, with no lock between them (`sound.c:39`, `sound.c:114`).
- Without a consumer, `getSamples` runs on the serving game thread, still with the `audio_game_thread` context (`sound.c:116`).
- The upstream wrapper that `sound.c:5` follows also runs the mixer off the game thread, on its audio callback.
- The producer occupies one pthread worker; the pool of 8 counts it (`Makefile:37`, `Makefile:43`).

**Errors and recovery:**
- If `createAudioContext` throws, `play` logs a warning and the game runs silent (`main.js:281`).
- If `startAudio` rejects, `play` logs a warning and closes the context, and the game runs silent (`main.js:296`–`:299`).
- In both cases `AUD_CONSUMER` stays 0, so no producer starts.
- If a later step of `play` fails, it closes the context (`main.js:315`).
- A failed `pthread_create` falls back the same way (`sound.c:95`), and `AUD_ACTIVE` stays 0 (`sound.c:111`).
- `createAudioContext` and `setPaused` catch a rejected `resume()` or `suspend()`, as on a closed context, and only warn (`audio.js:14`, `:34`).
- The callbacks are called without null checks (`sound.c:39`, `sound.c:114`). The game must register them before it serves.

**Memory growth:**
- The worklet keeps the `SharedArrayBuffer` it was given at start (`audio.js:23`).
- The ring is static data inside the initial memory (`web.c:20`, `Makefile:44`). Growing a shared memory never detaches its old buffer, so the views stay valid (`Makefile:40`–`:41`).
- No test covers growth.

## Key Types and Entry Points

1. **`iSNDdirectstart_`** (`sound.c:83`), called by the game.
   - Starts audio once and always returns 0; its arguments are ignored. Calls repeated before a stop do nothing.
   - Side effects: reads `AUD_CONSUMER` and may start the producer. A failed thread start leaves it without a consumer.
2. **`iSNDdirectserve_`** (`sound.c:107`), called by the game with its thread context.
   - Returns 0, and does nothing before a start.
   - Activates the ring on the first call when there is a consumer, runs `fadeInOut`, and renders to scratch when there is no consumer.
3. **`iSNDdirectstop_`** (`sound.c:120`), called by the game.
   - Deactivates the ring, then wakes and joins the producer, which may finish one chunk first. Returns 0.
   - Does nothing when audio is not started.
4. **`iSNDdirectsetfunctions_wrap`** (`sound.c:67`): stores the mixer callbacks that every later call uses, and returns 0.
5. **`producer_main`** (`sound.c:42`): the thread body.
   - Renders a chunk and publishes `AUD_WRITE` while the ring is active and below the target; otherwise waits on `AUD_READ`.
   - It exits once `s_producing` clears.
6. **`createAudioContext`** (`audio.js:6`):
   - Must be called synchronously in the Play click (`main.js:279`).
   - Returns a context at 22050 Hz, or at the default rate when that constructor throws (`audio.js:11`).
   - Requests `resume()`; a rejection only logs a warning (`audio.js:14`).
   - Throws if `AudioContext` is unavailable.
7. **`startAudio(context, buffer, address, volume)`** (`audio.js:20`):
   - Loads the worklet, connects node → gain → destination and sets `AUD_CONSUMER`.
   - Resolves to `{ setVolume, setPaused }` (`audio.js:28`).
   - Rejects if the module or node cannot be created, leaving `AUD_CONSUMER` 0.
8. **`setVolume(value)` and `setPaused(paused)`** (`audio.js:29`, `audio.js:33`):
   - `setVolume` sets the gain without checking it. The settings keep it in 0..1 (`settings.js:36`, `index.html:81`).
   - `setPaused` suspends or resumes the context. Its promise never rejects; a failure only logs a warning (`audio.js:34`).
9. **`RingPlayer`** (`audio-worklet.js:7`), the processor `nfs2se-ring`.
   - It takes `processorOptions` `{ buffer, address }` and views the header and frames there (`audio-worklet.js:11`–`:12`).
   - `process` (`audio-worklet.js:17`) fills both output channels, advances `AUD_READ` and always returns `true`.

## Interactions

- **[Shared memory](shared-memory.md):**
  - The `AUD_*` header and frame area (`web_layout.h:209`) are mirrored in `layout.js:131`.
  - The ring is `s_audio` (`web.c:20`), which `web_shared` exposes as `SHARED_AUDIO` (`web.c:37`).
  - Constants change in both mirrors together. `tests/unit/layout.test.mjs:30` checks that they are equal.
- **[Platform](platform.md):**
  - `NFS2SE.cpp` calls the imports.
  - `getSamples` runs through `wrap_regparm2` (`platform.h:25`).
  - The futex helpers live in `web.c:58`.
  - The pthread pool size and stack size come from the link flags (`Makefile:43`, `Makefile:45`).
- **[Page](page.md):**
  - `play` creates the context inside the click (`main.js:279`) and awaits `startAudio` before `callMain` (`main.js:295`).
  - The volume setting calls `setVolume` (`main.js:325`).
  - Hiding the tab calls `setPaused(document.hidden)` (`main.js:390`), and `endSession` pauses audio (`main.js:204`).
  - Pausing only suspends the context; the producer idles once the ring holds the target.
- **[Build and test](build-and-test.md):**
  - `dist/` must contain `audio.js`, `audio-worklet.js` and `layout.js`, copied from `src/web` (`Makefile:29`, `Makefile:86`).
  - Chromium runs with `--autoplay-policy=no-user-gesture-required` (`playwright.config.mjs:18`).
- **Integration tests:** `tests/e2e/audio.spec.mjs:6`, `:38`. `tests/e2e/game.spec.mjs:41` runs the game with audio connected but asserts nothing about sound.

## How to Test

Run the commands from the repository root.

**Prerequisites:**
- Node for the unit tests.
- For `make` targets and browser tests: the root's [prerequisites](../ARCHITECTURE.md#verification-and-review-map), with emsdk activated.
- Browser tests need a `dist/` built by `make`. Playwright starts `node tools/serve.mjs dist 8123` or, outside CI, reuses one already running (`playwright.config.mjs:21`–`:27`).

| Command | Expected evidence |
| ------- | ----------------- |
| `node --test tests/unit/audio-worklet.test.mjs` | `# pass 6`, `# fail 0` |
| `node --test tests/unit/layout.test.mjs` | `# pass 3`, `# fail 0` |
| `make` | Exit 0 and `dist/nfs2se.wasm`; the static asserts in `sound.c:17`–`:18` hold |
| `make && npx --no playwright test tests/e2e/audio.spec.mjs` | `2 passed` |
| `npx --no playwright test tests/e2e/game.spec.mjs`, after `make` | `4 passed` |
| `make test` | Exit 0: `N checks, 0 failed` per C test (none covers `sound.c`), `# fail 0`, `N passed` |

| Behaviour or invariant | Test |
| ---------------------- | ---- |
| Processor registered as `nfs2se-ring` | "the processor is registered as nfs2se-ring" (`audio-worklet.test.mjs:42`) |
| Linear interpolation at half the context rate, and `AUD_READ` advance | "frames are interpolated to the context rate and consumed" (`audio-worklet.test.mjs:46`) |
| Underrun plays silence and keeps the unplayed frame | "an underrun plays silence and keeps the unplayed frame" (`audio-worklet.test.mjs:57`) |
| Fractional position carries over at 48 kHz | "a fractional read position carries over between calls" (`audio-worklet.test.mjs:68`) |
| Wrap around the ring and the int32 counters | "reading wraps around the ring and the int32 counters" (`audio-worklet.test.mjs:82`) |
| An inactive ring drops the queue, then plays again | "a stopped ring drops what is queued and plays silence" (`audio-worklet.test.mjs:97`) |
| Real worklet at 22050 Hz: module load, shared buffer, frames copied unchanged with the gain applied, `AUD_CONSUMER`, last frame held | "the worklet plays the shared ring at the game rate with the volume applied" (`audio.spec.mjs:6`; assertions from `audio.spec.mjs:30`) |
| `setPaused(false)` runs the context and `setPaused(true)` suspends it; on a closed context both directions only warn | "pausing suspends the context and never rejects, even once it is closed" (`audio.spec.mjs:38`) |
| `AUD_*` constants equal in both mirrors | "every header constant has the same value in layout.js" (`layout.test.mjs:30`) |
| Capacity is a power of two | "derived sizes keep the slot and ring arithmetic aligned" (`layout.test.mjs:43`; assertion at `layout.test.mjs:47`) |
| Capacity a chunk multiple; chunk ≤ target ≤ capacity | The static asserts `sound.c:17`–`:18`, checked by `make` |
| Stored volume kept in 0..1 | "parseSettings takes stored values of the right type and range" (`settings.test.mjs:39`) |
| The game starts with audio connected; sound is not asserted | "the game runs, waits for its message box and ends" (`game.spec.mjs:41`) |

**Coverage gaps:**
- `sound.c` has no C test: `tests/c/` holds no sound test, and the Makefile defines no `TEST_SRC_sound` (`Makefile:97`–`:108`). Untested there: start, serve and stop; `AUD_ACTIVE` only with a consumer; the fill bound; the fallbacks for no consumer and for a failed thread.
- `game.spec.mjs` does not show whether the dummy-data session reaches `iSNDdirectstart_`.
- Untested on the page side: `setVolume` after start, `setPaused(false)` on an open context, the default-rate fallback in `createAudioContext`, and the context close when `startAudio` rejects.
- Real game audio is unverified: effects, music and timing.
- Browsers other than Chromium and memory growth are untested.

## Review and Refactor Guide

| Change | Inspect | Re-run |
| ------ | ------- | ------ |
| Ring size, target or rate | `web_layout.h:219`–`:222`, `layout.js:137`–`:140`, `sound.c:17`–`:18`, `sound.c:51`, `web.c:20`, `audio.js:10` | `make test`: layout and audio unit tests, the static asserts, `audio.spec.mjs` |
| Header words | `web_layout.h:212`–`:217`, `layout.js:131`–`:136`, every `ring_load`, `ring_store` and `Atomics` access | `make test` |
| Resampling or underrun policy | `audio-worklet.js:25` | `audio-worklet.test.mjs`, `audio.spec.mjs` (it expects the frames unchanged at step 1) |
| Start, serve or stop | `sound.c:83`–`sound.c:133` | `make`, `game.spec.mjs`; check restart by hand, because `AUD_ACTIVE` returns only at the first serve after a start (`sound.c:90`, `sound.c:111`) |
| Pause and volume wiring | `audio.js:28`, `main.js:204`, `main.js:325`, `main.js:390` | `audio.spec.mjs`, `settings.test.mjs`, `launcher.spec.mjs`, `game.spec.mjs` |

**Keep:**
- One writer per ring word.
- `AUD_CONSUMER` set before `callMain`.
- The capacity a power of two and a multiple of the chunk size, and chunk ≤ target ≤ capacity. The static asserts fail the build otherwise (`sound.c:17`–`:18`).
- No samples on the browser main thread.

**Forbidden:**
- The worklet viewing memory outside the ring, even though it holds the whole wasm buffer.
- C calling JS for audio.

**Proposals (not accepted):**
- **C test for `sound.c`.** Stub `web_audio`, the futex helpers and `wrap_regparm2` the way `tests/c/user32_test.c:52` and `tests/c/user32_test.c:86` stub theirs. Wire it through `TEST_SRC_sound` with pthread flags, like `Makefile:102`–`:103`.
  - Benefit: covers the import state machine, `AUD_ACTIVE` gating and the fill bound.
  - Success check: `make test-c` prints `N checks, 0 failed` for it.
- **Browser checks for `setVolume` and resuming.** Success check: a spec observes the gain value after `setVolume` and the running state after `setPaused(false)`.

## Open Gaps / Roadmap

- Sound with real game data is unverified (root [Roadmap](../ARCHITECTURE.md#roadmap)).
- No automated test reaches `sound.c`.
- `LinearSoundInterpolation` has no effect (`sound.c:6`).
- No milestones are established.

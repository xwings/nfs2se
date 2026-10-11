---
eatmycode_version: "1.2.0"
---
# Shared memory

## Goal

This module owns the contract between the wasm game threads and the page: the three shared regions (`src/platform/web.c:19`–`:21`), every shared word and every constant both sides must agree on (`src/platform/web_layout.h:1`, mirrored by `src/web/layout.js`), and the slot, ring and futex protocols.

**Non-goals:** what the data means. Drawing belongs to [graphics](graphics.md), messages and devices to [input](input.md), PCM to [audio](audio.md), the session to [page](page.md) and Win32 emulation to [platform](platform.md).

It is infrastructure: apart from `callMain`, Emscripten's `FS`, the exit and abort hooks (`main.js:416`–`:417`) and the message-box call (`web.c:76`), every path between the game and the browser crosses it ([System Design](../ARCHITECTURE.md#system-design)).

## Status

`done`: the layout, regions, protocols, mirror test and capacity asserts are implemented.
- **Evidence (2026-10-11):** `node --test` passes `tests/unit/layout.test.mjs` (3 of 3), `input.test.mjs` (19), `renderer.test.mjs` (4) and `audio-worklet.test.mjs` (6), each with `# fail 0`. `make test` exited 0: the `glide`, `user32` and `dinput` C tests printed `306 checks, 0 failed`, `2164 checks, 0 failed` and `549 checks, 0 failed`, and Playwright `28 passed`.
- **Unverified:** real game data, of which there is none on this machine. Game threads meet the page only in `tests/e2e/game.spec.mjs` (`:41`, `:68`, `:90`), which checks the message box, the exit, saving and a display failure, not frames, input or sound.

## Code Structure

| File | Role |
| ---- | ---- |
| `src/platform/web_layout.h` | Region ids and every shared constant, in `enum`s |
| `src/web/layout.js` | JS mirror: same names, same order |
| `src/platform/web.c` | Region storage and accessors, `web_shared`, futex helpers, the serialised message box, FS dirty flag |
| `src/platform/platform.h` | `web_*` declarations, `ctl_load`, `ctl_store` |
| `tests/unit/layout.test.mjs` | Mirror and arithmetic checks |

The protocol ends live in the modules that use them (see Interactions). The capacity asserts sit next to their users in `glide.c` and `sound.c`.

## Language and Conventions

- C GNU11 and JS ES modules follow the root rules ([Coding Style and Code Design](../ARCHITECTURE.md#coding-style-and-code-design)).
- **Mirror:** each header enum constant has a JS twin with the same name and value, and JS has no extras (`layout.test.mjs:30`, `:37`). Derived values stay expressions (`web_layout.h:47`, `layout.js:29`).
- **Enum only:** the test strips only `/* */` comments (`layout.test.mjs:11`) and reads `enum` bodies in order (`:13`). Values are decimal or upper-case hex integers and earlier names (`:17`, `:20`), joined by `+ - * <<` and parentheses (`:23`). A `#define` is invisible to it.
- **Comments** name each word's writer, unit or range (`web_layout.h:27`–`:35`).
- **Atomics, control block:** every word except the ring entries is atomic on both sides: JS `Atomics` (`input.js:60`–`:61`, `main.js:150`), C seq_cst `ctl_load`/`ctl_store` (`platform.h:113`, `:118`) and `__atomic` read-modify-writes (`user32.c:53`, `dinput.c:455`). The page writes ring entries plainly (`input.js:64`–`:66`); C reads them with `ctl_load` (`user32.c:76`–`:78`).
- **Atomics, slots:** only `GFX_STATE` is atomic (`glide.c:144`, `:146`, `:160`; `renderer.js:180`, `:184`). Fill counts, commands and vertices are plain on both sides (`glide.c:158`–`:159`, `renderer.js:354`).
- **Atomics, audio ring:** the header words are atomic (`sound.c:27`, `:32`; `audio-worklet.js:13`, `:20`, `:22`; `audio.js:27`), the frames plain (`sound.c:56`, `audio-worklet.js:29`–`:31`). `AUD_RATE` comes from a static initialiser (`web.c:20`).
- **Futexes:** C waits only through `web_futex_wait`, which returns nothing (`platform.h:104`), so each caller re-checks its word in a loop (`glide.c:144`–`:145`, `user32.c:114`–`:119`, `web.c:78`–`:79`, `sound.c:47`–`:53`). The page only notifies; nothing in `src/web` calls `Atomics.wait`.
- **Enforced:** the mirror test and the static asserts on capacities (`glide.c:100`–`:101`, `sound.c:17`–`:19`); `web.c` has none. The rest is observed convention.

## Design and Invariants

### Regions

| Region | C storage | Bytes | JS view |
| ------ | --------- | ----- | ------- |
| `SHARED_CTL` 0 | `s_ctl` (`web.c:19`) | 4,352 | `Int32Array` (`main.js:287`) |
| `SHARED_GFX0`, `SHARED_GFX1` 1, 2 | `s_gfx`, 16-byte aligned (`web.c:21`) | 16,777,264 each | `slotViews` (`renderer.js:92`), made at `main.js:288` |
| `SHARED_AUDIO` 3 | `s_audio` (`web.c:20`) | 32,832 | int32 header, int16 frames (`audio-worklet.js:11`–`:12`) |

- `web_shared(id)` returns a byte address, or 0 for an unknown id (`web.c:27`, `:40`). C code reaches the regions through `web_ctl`, `web_audio` and `web_gfx_slot` (`web.c:43`–`:56`).
- `play` views the control block and both slots at once (`main.js:286`–`:288`), and the audio ring only when an AudioContext exists (`:295`), all before `callMain` (`:310`).
- The static arrays, about 32 MiB, lie inside the 256 MB initial memory (`Makefile:44`). The page's views are made once and stay valid, because growth never detaches a shared buffer (`Makefile:39`–`:41`).

### Control block

All words are int32. Words 11–15 and 48–63 are free.

| Word | Writer | Reader | Notes |
| ---- | ------ | ------ | ----- |
| `CTL_SEQ` 0 | `input.js:68`, `user32.c:53` | `user32.c:116` | Futex; atomic add on both sides |
| `CTL_RING_HEAD` 1 | `input.js:67` | `user32.c:73` | Free-running |
| `CTL_RING_TAIL` 2 | `user32.c:79` | `input.js:61` | Free-running |
| `CTL_MOUSE_X`, `_Y` 3, 4 | `input.js:183`–`:184` | `dinput.c:489`–`:490` | `0..GAME_WIDTH − 1`, `0..GAME_HEIGHT − 1` |
| `CTL_MOUSE_BUTTONS` 5 | `input.js:111`, `:115`, `:173`; C clears the latch (`dinput.c:455`) | `dinput.c:455` | Atomic read-modify-write on both sides |
| `CTL_MOUSE_MOVES` 6 | `input.js:185` | `dinput.c:216`, `:485` | Change counter |
| `CTL_DIALOG` 7 | C 0 (`web.c:75`); page answer (`main.js:189`) | `web.c:78` | Futex; `DIALOG_*`; one dialog at a time (`web.c:73`) |
| `CTL_FS_DIRTY` 8 | C 1 (`web.c:91`); page 0 before a sync, 1 after a failure (`main.js:152`, `:157`) | `main.js:150` | |
| `CTL_CFG_*` 9, 10 | `WrapperInit` (`wrapper.c:145`–`:146`) | `renderer.js:153`, `:292` | |
| Pads 16–47 | `input.js:196`–`:206` | `dinput.c:415`–`:443` | 2 × 16 words |
| Ring 64–1087 | `input.js:64`–`:66` | `user32.c:76`–`:78` | 256 × 4 words |

- **Pads** (`web_layout.h:50`–`:60`): connected, SDL-order button bits, `HAT_*` bits, the hat, button and axis counts, and six SDL-range axes. The page writes `PAD_CONNECTED` last (`input.js:206`), or only 0 for a missing pad (`:196`). C reads it first (`dinput.c:415`).
- **Mouse latch:** a press sets `MOUSE_LEFT | MOUSE_LEFT_LATCH` (`input.js:111`); a release or blur clears `MOUSE_LEFT` (`:115`, `:173`). C clears the latch with `fetch_and` and counts either bit as down (`dinput.c:455`–`:456`), so a click shorter than one poll survives. It reports a press when a read finds the button down after one that did not (`:457`–`:458`).
- **Message ring:** entry *i* is message, wParam, lParam and a spare word, at `CTL_RING_BASE + (i & 255) × 4` (`web_layout.h:43`–`:45`, `:84`–`:89`).
  1. `pushMessage` refuses when `(head − tail) | 0 ≥ 256`. Otherwise it writes the entry, stores head + 1, adds 1 to `CTL_SEQ` and notifies (`input.js:59`–`:69`).
  2. `GetMessageA_wrap` loads `CTL_SEQ`, then pops. If nothing is queued, it waits up to 100 ms on that value, so a later push is never missed (`user32.c:116`–`:119`).
  3. `ring_pop` stores tail + 1 after reading the entry (`user32.c:79`). Only the message-loop thread pops (`:69`).
- **Local queue:** C's own messages (posted messages, `WM_DESTROY`, joystick keys) go to a mutex-guarded queue that also bumps `CTL_SEQ` and wakes the loop. `GetMessageA_wrap` takes them before the ring's (`user32.c:30`–`:32`, `:43`–`:54`, `:117`).

### Graphics slots

A slot is 16 header words, command words from byte 64, then vertices from byte 12,582,976: 3 << 20 command words and 174,762 vertices (`web_layout.h:130`–`:149`).
- **Header:** `GFX_STATE` (`FREE`, `WRITING`, `PARTIAL`, `FRAME`; `web_layout.h:152`–`:158`), `GFX_CMD_WORDS`, `GFX_VTX_COUNT`. Words 3–15 are free.
- **Vertex** (24 bytes, `web_layout.h:142`–`:145`): float x, y (game units), oow at byte 8, s, t at 12 (texture coordinates are s/oow, t/oow), RGBA8 at 20. `put_vertex` removes the snap offset and scales sow and tow by 1/256 (`glide.c:327`–`:338`).

**Protocol.** C owns a slot while it is `FREE` or `WRITING`; the renderer owns it while it is `PARTIAL` or `FRAME`.
1. `open_slot` waits for `FREE` in 100 ms futex waits, stores `WRITING` and resets the fill counts (`glide.c:140`–`:152`).
2. Commands and vertices are appended with plain stores (`glide.c:175`, `:296`).
3. `publish` writes the fill counts, stores the state last and switches slots (`glide.c:155`–`:162`).
4. `grBufferSwap` appends `SWAP`, publishes `FRAME` and opens the next slot at once, which keeps the game at most two frames ahead (`glide.c:428`–`:434`).
5. If a command or vertex batch does not fit, `reserve` publishes `PARTIAL` and opens the other slot (`glide.c:167`–`:173`).
6. On each animation frame, `pump` runs the expected slot while it is `PARTIAL` or `FRAME`, stores `FREE`, notifies and switches. It stops after one `FRAME` (`renderer.js:171`–`:191`).

- **Order:** both sides alternate strictly, starting with `SHARED_GFX0` (`web_layout.h:130`–`:132`, `glide.c:161`, `renderer.js:186`). There is no sequence number: the alternation and the state word are the whole handshake.
- **State** carries across slots. `sync_state` emits only what differs from `s_emitted` (`glide.c:263`, `:111`). Both sides start from the `GFX_DEFAULT_*` values and the defaults listed beside them (`web_layout.h:196`–`:206`): `DEFAULT_STATE` (`glide.c:91`–`:98`) and `defaultState` (`renderer.js:106`–`:112`).
- **TRIS** grows with each batch (`glide.c:307`). Any other command or a new slot closes it (`glide.c:182`, `:151`), so none spans two slots.
- **Errors:** a zero or overlong size ends parsing of the slot (`renderer.js:368`–`:369`), and the `switch` skips unknown ops (`:371`). Texture sizes and draw ranges are validated (`:438`, `:528`); payload lengths are not, but the views keep reads inside the slot. While the context is lost, `execute` keeps state, textures and the fog table without drawing, and `pump` still frees the slot (`:355`, `:184`). Play refuses to start without WebGL2 (`main.js:289`–`:290`). A WebGL failure throws out of `pump` and ends the session (`renderer.js:168`–`:170`, `main.js:257`–`:259`).

### Commands

- **Header word:** `op | words << GFX_OP_SIZE_SHIFT`, counting the header (`web_layout.h:160`, `glide.c:180`, `renderer.js:367`). Sizes fit the 24 bits above the op (`glide.c:101`). Payloads are documented at `web_layout.h:165`–`:179`.
- **Recording and execution:** `sync_state` records the state ops (`glide.c:263`–`:293`), except `GAMMA` (`glide.c:624`). `execute` has one case per op, in id order (`renderer.js:371`–`:433`).

| Op | Payload | Default |
| -- | ------- | ------- |
| `CLIP` 1 | minX, minY, maxX, maxY | 0, 0, `GAME_WIDTH`, `GAME_HEIGHT` |
| `BLEND` 2 | src, dst | `ONE`, `ZERO` |
| `TEXENABLE` 3 | 0 or 1 | 0 |
| `FOGENABLE` 4 | 0 or 1 | 0 |
| `DEPTHMASK` 5 | 0 or 1 | 1 |
| `FOGCOLOR` 6 | `0x00RRGGBB` | 0 |
| `FOGTABLE` 7 | `GFX_FOG_ENTRIES` (65,536) bytes indexed by integer w | All zeros |
| `CLEAR` 8 | colour, alpha, depth; honours the clip window and depth mask | Action (`glide.c:446`) |
| `TEXIMAGE` 9 | id, size, size² premultiplied RGBA8 texels; binding unchanged | Action (`:202`) |
| `TEXBIND` 10 | id, or `GFX_NO_TEXTURE` (−1) | `GFX_NO_TEXTURE` |
| `GAMMA` 11 | float bits | 1.0 |
| `TRIS` 12 | first vertex, count | Action (`:302`) |
| `SWAP` 13 | none | Action (`:431`) |
| `DEPTHFUNC` 14 | `GR_CMP_*` 0..7 | 3 (`LEQUAL`) |
| `SAMPLER` 15 | bilinear, clamp s, clamp t | 1, 1, 1 |

- The largest command is a `GFX_TEX_MAX_SIZE`² `TEXIMAGE` of 65,539 words, asserted to fit an empty slot (`glide.c:100`).
- **Blend factors:** `BLEND_ZERO` 0, `BLEND_ONE` 1, `BLEND_SRC_ALPHA` 2, `BLEND_INV_SRC_ALPHA` 3 (`web_layout.h:188`–`:194`). `glide.c:497`–`:498` maps the game's factors onto them. The renderer masks them with 3 and looks them up (`renderer.js:377`–`:378`, `:259`).

### Audio ring

The ring is 16 int32 header words, then 8,192 interleaved int16 stereo frames (`web_layout.h:209`–`:222`).

| Word | Writer | Reader |
| ---- | ------ | ------ |
| `AUD_WRITE` 0 | Producer (`sound.c:57`) | `audio-worklet.js:20` |
| `AUD_READ` 1 | Worklet, then notify (`audio-worklet.js:37`–`:38`, `:42`–`:43`) | Producer (`sound.c:50`), futex wait (`:53`) |
| `AUD_ACTIVE` 2 | 1 at the first serve with a consumer, 0 at stop (`sound.c:111`–`:112`, `:126`) | `sound.c:51`, `audio-worklet.js:22` |
| `AUD_RATE` 3 | Static initialiser (`web.c:20`) | `audio-worklet.js:13`, once |
| `AUD_CONSUMER` 4 | Page, before `callMain` (`audio.js:27`) | `sound.c:91`, at each start |

Words 5–15 are free. `AUD_ACTIVE` is 1 from the first serve with a consumer after a start until the stop (`sound.c:111`; `web_layout.h:214`).
- **Producer:** while the ring is active and one more chunk keeps the fill at or below `AUD_TARGET_FRAMES`, it renders 256 frames at `write & 8191` and then stores `write + 256`. Otherwise it waits up to 10 ms on `AUD_READ` (`sound.c:47`–`:57`). Chunks never wrap, and chunk ≤ target ≤ capacity (`sound.c:17`–`:19`).
- **Consumer:** the worklet treats `(write − read) | 0` as available and plays silence on underrun. It then stores the read count and notifies (`audio-worklet.js:23`–`:38`). While the ring is inactive, it drops the queue by storing read = write (`:40`–`:43`).
- **No consumer:** without `AUD_CONSUMER`, or when the thread cannot start, no producer runs, and the mixer renders into scratch memory (`sound.c:95`–`:98`, `:115`–`:116`).
- **Stop:** `iSNDdirectstop_` clears `AUD_ACTIVE`, then wakes and joins the producer (`sound.c:126`–`:130`).

### Other mirrored constants

| Constants | Header | Used by |
| --------- | ------ | ------- |
| `GAME_WIDTH`, `GAME_HEIGHT` | `web_layout.h:20`–`:21` | `input.js:183`–`:184`, `glide.c:93`, `renderer.js:31` |
| `DIALOG_OK`, `DIALOG_CANCEL` (Win32 `IDOK`, `IDCANCEL`) | `web_layout.h:66`–`:67` | `main.js:187`; returned unchanged by `MessageBoxA_wrap` (`user32.c:168`–`:172`) |
| `WM_KEYDOWN`, `WM_KEYUP`, `WM_CHAR`, `KEY_REPEAT`, `KEY_SCANCODE_SHIFT` | `web_layout.h:99`–`:103` | `input.js:56`, `:149`, `:152`, `:163`; `user32.c:99`–`:103` |
| `VK_*`, `SC_*` | `web_layout.h:111`–`:127` | `input.js:53`, `user32.c:102`, `dinput.c:156`–`:160` |
| `GFX_VTX_OOW_OFFSET`, `GFX_VTX_ST_OFFSET`, `GFX_VTX_COLOR_OFFSET` | `web_layout.h:143`–`:145` | `glide.c:332`–`:334`, `renderer.js:214`–`:215` |
| `GFX_OP_SIZE_SHIFT`, `GFX_OP_MASK` | `web_layout.h:181`–`:182` | `glide.c:180`, `renderer.js:367` |
| `GFX_FOG_ENTRIES` | `web_layout.h:183` | `glide.c:120`, `:289`; `renderer.js:125`, `:398` |
| `GFX_TEX_MAX_SIZE` | `web_layout.h:184` | `glide.c:100`, `:213`; `renderer.js:438` |
| `GFX_NO_TEXTURE` | `web_layout.h:185` | `glide.c:96`, `:725`; `renderer.js:109` |
| `GFX_DEFAULT_*` | `web_layout.h:201`–`:206` | `glide.c:91`–`:98`, `renderer.js:106`–`:112` |

### Invariants

- **Publication:** data is written plainly, then one seq_cst store of the index or state word publishes it. The reader loads that word first: `input.js:67` → `user32.c:73`, `glide.c:160` → `renderer.js:180`, `sound.c:57` → `audio-worklet.js:20`.
- **Hand-back:** the consumer finishes reading before the store that returns the space: `user32.c:79` → `input.js:61`, `renderer.js:184` → `glide.c:144`, `audio-worklet.js:37` → `sound.c:50`.
- **One producer, one consumer** per ring and per slot direction ([Runtime and Data Flow](../ARCHITECTURE.md#runtime-and-data-flow)).
- **Several writers:** `CTL_SEQ` and `CTL_MOUSE_BUTTONS` use atomic read-modify-write on both sides (`input.js:68`, `user32.c:53`; `input.js:111`, `dinput.c:455`). `CTL_DIALOG` and `CTL_FS_DIRTY` rely on store order: C clears the dialog word before it calls the page (`web.c:75`–`:76`), and the page clears the dirty flag before it syncs and sets it again after a failure (`main.js:152`–`:154`, `:157`).
- **Bounded waits:** slots 100 ms (`glide.c:26`), dialog 1 s (`web.c:17`), messages 100 ms (`user32.c:17`), audio 10 ms (`sound.c:15`). A missed wake costs at most one timeout; every caller waits again until its condition holds.
- **The page never blocks:** it only loads, stores and notifies. This is observed; no test enforces it.
- **Free-running int32 counters:** the ring head and tail, `AUD_WRITE` and `AUD_READ`. JS compares them with `| 0`, and C as uint32 (`input.js:61`, `user32.c:72`–`:73`, `sound.c:49`–`:51`). Masked capacities are powers of two (`layout.test.mjs:46`–`:47`).
- **Alignment:** the slot size and vertex offset are multiples of 4 (`layout.test.mjs:44`–`:45`), for the typed views (`renderer.js:95`–`:96`). `s_gfx` is 16-byte aligned (`web.c:21`).
- **Word map:** the pads end before the ring, and the axes fit the pad stride (`layout.test.mjs:48`–`:49`).

## Key Types and Entry Points

| Entry | Responsibility | Inputs → outputs | Side effects; failure |
| ----- | -------------- | ---------------- | --------------------- |
| Layout constants (`web_layout.h:7`, `layout.js:4`) | Name every shared value | Compile-time values | Drift fails `layout.test.mjs:30`, `:37` |
| `web_shared` (`web.c:27`) | Region addresses | Id → byte address | None; an unknown id gives 0 (`:40`) |
| `ctl_load`, `ctl_store` (`platform.h:113`, `:118`) | seq_cst control words | Index, value → value | No bounds check |
| `web_futex_wait`, `web_futex_wake` (`web.c:58`, `:63`) | Block and wake game threads | Word, expected, ms → none | A wake wakes every waiter (`:65`); no result, so callers re-check |
| `web_message_box` (`web.c:71`) | Page dialog | Text, caption, OK/Cancel → `DIALOG_OK` or `DIALOG_CANCEL` | One at a time (`:73`); clears `CTL_DIALOG`, calls the page (`:76`), waits with no total limit |
| `open_slot`, `publish`, `reserve` (`glide.c:140`, `:155`, `:167`) | Hand slots over | State or room needed → none | Block until the slot is `FREE`, with no total limit; overflow publishes `PARTIAL` |
| `put_command`, `sync_state` (`glide.c:175`, `:263`) | Append commands; emit changed state | Op, payload words → payload pointer | Close the open `TRIS`; update `s_emitted` |
| `slotViews`, `Renderer.pump`, `execute` (`renderer.js:92`, `:171`, `:352`) | View and replay slots | Buffer, address → views; none → frame presented or not | GL calls, `FREE` and notify; a bad size ends the slot; WebGL failures throw (`:168`–`:170`) |
| `pushMessage`, `ring_pop` (`input.js:59`, `user32.c:70`) | Carry Windows messages | Message → message | Bump and notify `CTL_SEQ`; store the tail; false when full or empty |
| `producer_main`, `RingPlayer.process` (`sound.c:42`, `audio-worklet.js:17`) | Fill and drain PCM | Chunks → samples | Store the counters, and the worklet notifies `AUD_READ`; ends when `s_producing` clears; underrun plays silence |

## Interactions

| Partner | Boundary and direction | Tests |
| ------- | ---------------------- | ----- |
| [platform](platform.md) | C → regions: `web_*`, `ctl_*`, config words (`wrapper.c:145`–`:146`), FS dirty after file writes and deletes (`kernel32.c:502`, `:520`, `records.c:99`, `wrapper.c:100`, `:201`) | `game.spec.mjs:41`, `:68` |
| [graphics](graphics.md) | `glide.c` → `renderer.js`: slots, ops, vertices, blend factors | `glide_test.c:493`, `renderer.spec.mjs:279` |
| [input](input.md) | `input.js` → `user32.c`, `dinput.c`: ring, `CTL_SEQ`, mouse and pad words | `input.spec.mjs:38`, `:64`; `user32_test.c:189` |
| [audio](audio.md) | `sound.c` → `audio-worklet.js`: the audio ring | `audio.spec.mjs:6` |
| [page](page.md) | Views (`main.js:286`–`:288`), dialog answers (`:189`), FS dirty (`:150`), input poll and slot pump each frame (`:253`, `:255`), `AUD_CONSUMER` (`audio.js:27`) | `game.spec.mjs:41` |
| [build and test](build-and-test.md) | `HEAPU8` export (`Makefile:47`), views under memory growth (`:39`–`:41`); `make test-unit` runs the layout test (`:121`–`:122`) | — |

**Compatibility:** both sides ship together in one `dist/`, and nothing persists the layout. A change only needs both mirrors updated together.

## How to Test

**Prerequisites:** as in the root's [Verification and Review Map](../ARCHITECTURE.md#verification-and-review-map); `node --test` needs only Node.

| Command | Expected evidence |
| ------- | ----------------- |
| `node --test tests/unit/layout.test.mjs` | `# pass 3`, `# fail 0` |
| `make build/tests/glide.cjs && node build/tests/glide.cjs`, likewise `user32` and `dinput` (pattern rule `Makefile:111`) | `N checks, 0 failed` |
| `make test-c` | One `N checks, 0 failed` line per test |
| `make test-unit` | `# fail 0` |
| `make`, then `npx --no playwright test tests/e2e/renderer.spec.mjs`, likewise `input`, `audio` and `game` | `N passed` |
| `make test-e2e` | `N passed` |
| `make test` | All three pass, with exit status 0 |

Playwright starts `node tools/serve.mjs dist 8123`, or, outside CI, reuses one already running (`playwright.config.mjs:21`–`:27`).

| Behaviour | Test |
| --------- | ---- |
| Mirror, no extras | `layout.test.mjs:30`, `:37` |
| Alignment, powers of two, word map | `layout.test.mjs:43` |
| Slot views | `renderer.test.mjs:31` |
| Emitted state and defaults | `glide_test.c:265` `test_state` |
| `PARTIAL` on vertex and command overflow, slot alternation | `glide_test.c:493` `test_overflow` |
| Back-pressure | `glide_test.c:526` `test_back_pressure` |
| A frame in two slots; both slots freed | `renderer.spec.mjs:279` |
| Slots freed and data kept while the context is lost | `renderer.spec.mjs:334` |
| Ring push, full ring, int32 wrap | `input.test.mjs:125`, `:134`, `:147` |
| Refused releases sent by later polls | `input.test.mjs:279` |
| Mouse latch, pads | `input.test.mjs:314`, `:329`; `input.spec.mjs:64` |
| `CTL_SEQ` bump and wake, C's queue before the ring, waiting on `CTL_SEQ` | `user32_test.c:139`, `:176`, `:189` |
| Latch read in C; a held button counted once | `dinput_test.c:593` |
| Audio consumption, underrun, wrap, stop | `audio-worklet.test.mjs:46`, `:57`, `:82`, `:97` |
| Worklet on a ring in a browser; `AUD_CONSUMER` set | `audio.spec.mjs:6` |
| Dialog, exit and FS dirty in a real session | `game.spec.mjs:41`, `:68` |

**Gaps:**
- No C test runs two threads against the regions: `glide_test.c` plays the renderer inside its futex stub (`glide_test.c:1`–`:4`), `user32_test.c` pushes from its futex stub (`user32_test.c:52`), and `dinput_test.c` joins its reader thread before checking (`dinput_test.c:565`–`:566`).
- No C test links `web.c` or `sound.c` (`Makefile:97`–`:108`). `web_message_box` runs only in the browser sessions of `game.spec.mjs`, and no test checks `producer_main`.
- Nothing replays `DEFAULT_STATE` against `defaultState`.
- No test enforces that the page never blocks.

## Review and Refactor Guide

**Add a control word:** take a free index (11–15 or 48–63). Add it to `web_layout.h` with a writer comment, and to `layout.js` in the same place. Give it one writer, use read-modify-write, or order the stores as `CTL_DIALOG` and `CTL_FS_DIRTY` do. Run the layout test.

**Add a shared constant:** put it in an `enum` of the header, in decimal or upper-case hex, and mirror it in the same order. A `#define` escapes the mirror test.

**Add a graphics op** (the next id is 16):
1. Document its payload (`web_layout.h:165`–`:179`) and default (`:196`–`:206`) in the header, and mirror them.
2. A state op needs a `State` field (`glide.c:80`–`:88`), a `DEFAULT_STATE` value (`:91`), a `sync_state` line (`:263`) and a `defaultState` field (`renderer.js:106`). An action calls `put_command` (`glide.c:175`).
3. Add an `execute` case (`renderer.js:371`). Keep in JS whatever must survive a lost context, as textures and the fog table do (`:124`–`:125`).
4. Extend `glide_test.c` and the replay harness (`renderer.spec.mjs:36`), including the lost-context test (`:334`).

**Change a capacity:** slots are static (`web.c:21`), and each has a GL buffer of the full vertex capacity (`renderer.js:210`). The asserts keep the largest command within an empty slot and the 24-bit size field (`glide.c:100`–`:101`), and the audio chunk within the target and capacity without wrapping (`sound.c:17`–`:19`). The layout test checks alignment and powers of two (`layout.test.mjs:43`).

**Forbidden coupling:**
- JS reading game memory outside these regions ([System Design](../ARCHITECTURE.md#system-design)).
- Numeric literals for shared values.
- Blocking the page thread.
- A second ring consumer or recording thread: the recorder's statics have no lock (`glide.c:126`–`:132`).

**Proposals:**

| # | Proposal | Benefit | Success check |
| - | -------- | ------- | ------------- |
| 1 | Replay a slot recorded from `DEFAULT_STATE` on a fresh renderer | The defaults cannot drift | The test fails when one side changes a default |
| 2 | A C test that runs a producer and a consumer thread on the real regions (`web.c`, `sound.c`) | Covers the futex loops and store order under real concurrency | It passes under `make test-c` and fails when a wake or a publishing store is removed |
| 3 | `_Static_assert(GFX_SLOT_BYTES % 4 == 0)` next to `s_gfx` (`web.c:21`) | C's atomics on the second slot's header stay aligned without the JS test | A misaligned slot size stops `make` |
| 4 | Report a press whenever a read clears a set latch (`dinput.c:455`–`:458`) | A release and a new press between two reads are not lost | A `dinput_test.c` case: a read with the button held, then `MOUSE_LEFT \| MOUSE_LEFT_LATCH` again, reports a press |

## Open Gaps / Roadmap

- **Defaults:** only `GFX_DEFAULT_*`, `GAME_*` and `GFX_NO_TEXTURE` are shared. Texturing and fog off, fog colour and table 0 and gamma 1.0 are written separately on each side (`glide.c:91`–`:98`, `:120`, `:123`; `renderer.js:106`–`:112`, `:125`–`:126`), and no test replays one against the other (proposal 1).
- **Threads:** no C test runs two threads on the shared regions, and none links `web.c` or `sound.c` (see How to Test; proposal 2).
- **Not asserted in C:** `GFX_SLOT_BYTES % 4 == 0`, which C's atomics on the second slot's header need; only `layout.test.mjs:44` checks it (proposal 3).
- **Silent loss:** the page ignores a key press or `WM_CHAR` the full ring refuses (`input.js:149`, `:152`); refused releases are sent again by later polls (`:155`–`:167`, `:191`). Joystick keys the full local queue refuses are posted again (`dinput.c:332`–`:333`), but a `WM_CHAR` that no longer fits is dropped (`user32.c:95`–`:96`).
- **Mouse latch:** after a read that saw the button down, a release and a new press before the next read are not reported, because a press needs the previous read to see the button up (`dinput.c:457`; proposal 4).
- **Races:** one pad read may mix two polls, because C loads the words one by one while the page rewrites them (`input.js:200`–`:206`, `dinput.c:415`–`:443`).
- **Unbounded waits:** a message box waits for the player's answer with no total limit (`web.c:78`–`:79`). After the page ends a session while the game still runs, as on a display failure (`main.js:257`–`:259`), nothing frees the slots, and the recording thread keeps waiting in `open_slot` (`glide.c:144`–`:145`).
- **Unverified:** threaded C behaviour and real game data ([Roadmap](../ARCHITECTURE.md#roadmap)).

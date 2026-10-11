---
eatmycode_version: "1.2.0"
---
# Input

## Goal

- **Owns** input from the browser to the game:
  - `src/web/input.js` turns keyboard, pointer and Gamepad API state into window messages and shared words.
  - `src/platform/user32.c` is the window and the message queue the game's loop reads, plus `MessageBoxA_wrap` (`user32.c:169`).
  - `src/platform/dinput.c` emulates DirectInput for the mouse and two joysticks, and replaces the game's axis routine (`dinput.c:575`).
- **Non-goals:** rendering force feedback (`dinput.c:5`); warping the browser pointer, which browsers cannot do (`dinput.c:464`); on-screen touch controls, since touch arrives only as pointer events (`src/web/input.js:107`).
- **Owned elsewhere:** the control-block layout and the key-message constants ([shared memory](shared-memory.md), `src/platform/web_layout.h:24`, `:92`), `nfs2se.conf` parsing ([platform](platform.md), `src/platform/config.c:70`) and when input is enabled ([page](page.md)).
- A runtime subsystem, not infrastructure.

## Status

`done`.
- **Evidence (2026-10-11):** `make test` exited 0. In it, `user32` printed `2164 checks, 0 failed` and `dinput` `549 checks, 0 failed`. Run on their own, `node --test tests/unit/input.test.mjs` passed 19/19 and the browser tests of `tests/e2e/input.spec.mjs` and `tests/e2e/game.spec.mjs` printed `7 passed`.
- **Unverified:** gameplay with real game data; real gamepads (tests use synthetic `Gamepad` objects); DirectInput against the running game, since the C tests stub the game's variables (`tests/c/dinput_test.c:69`).
- **Not implemented:** force-feedback output.

## Code Structure

| File | Role |
| ---- | ---- |
| `src/web/input.js` | `KEYMAP`, `charCode`, `pushMessage`, `padState`, `Input`: DOM events → ring messages, mouse words and pad words. |
| `src/platform/user32.c` | Window, local queue and ring consumer; the Win32 message imports and `MessageBoxA`. |
| `src/platform/dinput.c` | DirectInput COM objects, joystick and mouse state, button-to-key emulation, `processAxes`. |
| `tests/unit/input.test.mjs` | `input.js` against a fake DOM and a `SharedArrayBuffer` control block. |
| `tests/e2e/input.spec.mjs` | Real Chromium key and mouse events through `Input` on a blank page. |
| `tests/c/user32_test.c` | `user32.c` with stub web helpers: dispatch, queue order, waiting, full queue, window lifetime, message box. |
| `tests/c/dinput_test.c` | `dinput.c` and `config.c` with stub game variables, thunks and `user32_post_key`. |

## Language and Conventions

- JavaScript (ES module) and C (`-std=gnu11`, `Makefile:33`). The root rules apply: [Coding Style and Code Design](../ARCHITECTURE.md#coding-style-and-code-design).
- **Names are fixed by the link.** Win32 imports keep the game's `_wrap` names (`src/platform/user32.c:108`). DirectInput methods are plain C functions; the vtables hold the `<method>_wrap` thunks that NFS2SE.cpp generates for them (`src/platform/dinput.c:9`, `:119`; `src/platform/platform.h:35`).
- **Windows struct sizes are asserted:** `MSG` 28 bytes (`user32.c:23`); `DIDEVICEINSTANCEA` 580, `DIDEVCAPS` 44, `DIJOYSTATE` 80 (`dinput.c:56`, `:63`, `:76`).
- **Shared constants:** the key message ids, the key `lParam` encoding and the virtual-key and scancode values come from the layout mirrors (`src/platform/web_layout.h:92`–`:127`, `src/web/layout.js:55`–`:77`). Only `WM_DESTROY` and the `WM_USER` range are local to `user32.c` (`user32.c:12`–`:14`); the DirectInput constants are named in `dinput.c:18`–`:40`.
- **Shared words:** `input.js` uses `Atomics`, except that ring payloads are plain stores published by an `Atomics.store` of the head (`src/web/input.js:64`–`:67`). C uses `ctl_load`/`ctl_store` (`src/platform/platform.h:113`, `:118`) and `__atomic` builtins (`user32.c:53`, `dinput.c:455`).
- **Reuse** `pushMessage` for messages from JS (`input.js:59`), `user32_post_key` for keys made in C (`user32.c:97`), `set_key` for button-to-key changes (`dinput.c:334`) and `fitViewport` for screen-to-game coordinates (`src/web/renderer.js:78`).
- **Enforced by tests:** the `KEYMAP` shape and unique base scancodes (`tests/unit/input.test.mjs:58`, `:85`), and the layout constants (`tests/unit/layout.test.mjs:30`). Key codes beyond the spot checks at `input.test.mjs:65`–`:82` are only observed.

## Design and Invariants

**Paths.** Keys travel through the control ring to `GetMessageA_wrap` (`src/platform/user32.c:108`). The mouse and pads travel through shared words to `GetDeviceData` and `GetDeviceState` (`src/platform/dinput.c:467`, `:405`). The overview is in [Runtime and Data Flow](../ARCHITECTURE.md#runtime-and-data-flow).

**Message queue (`user32.c`)**
- **One producer, one consumer.** JS alone writes `CTL_RING_HEAD` (`src/web/input.js:67`). C alone writes `CTL_RING_TAIL`, from the message-loop thread (`src/platform/user32.c:69`, `:79`). Indices are free-running int32 values masked by the power-of-two entry count (`input.js:61`, `:63`; `user32.c:75`).
- **A full ring refuses the message:** `pushMessage` returns `false` and writes nothing (`input.js:61`–`:62`). A refused `WM_KEYDOWN` or `WM_CHAR` is lost (`input.js:149`, `:152`); a refused `WM_KEYUP` waits and is sent later (see Keyboard).
- **The local queue comes first.** Messages made in C go into a mutex-protected 256-entry queue, served before the ring (`user32.c:16`, `:30`, `:117`): `PostMessageA` (`:143`), `WM_DESTROY` from `DestroyWindow` (`:159`) and keys from pad buttons (`:97`–`:104`). A full queue refuses the message (`:47`). `user32_post_key` returns `false` only when the key message does not fit; a control-key `WM_CHAR` that no longer fits is dropped (`:95`–`:96`, `:103`).
- **No lost wake-ups.** `GetMessageA_wrap` loads `CTL_SEQ` before checking both queues, then waits up to 100 ms for it to change (`user32.c:116`–`:119`, `:17`). Every push bumps and notifies `CTL_SEQ` (`user32.c:53`–`:54`, `input.js:68`–`:69`).
- **Fields:** `time` is milliseconds since `main()` (`src/platform/web.c:94`, `user32.c:127`), and `pt` stays zero (`user32.c:122`). The window handle is the address of a static (`user32.c:35`).
- **No `WM_QUIT`:** `GetMessageA_wrap` always returns 1, and the game ends through `ExitProcess` (`user32.c:107`, `:128`). Nothing queues `WM_QUIT`: JS pushes only key messages, and `PostMessageA_wrap` accepts only `WM_USER`..`0xBFFF` and drops `wParam` (`user32.c:140`–`:142`).
- **Window lifetime:** `DefWindowProcA_wrap` marks the window dead on `WM_DESTROY` (`user32.c:151`–`:152`), and shutdown waits up to 2.5 s for that (`src/platform/wrapper.c:16`, `:76`–`:77`).
- **`MessageBoxA_wrap`** shows OK/Cancel when `MB_OKCANCEL` (bit 0) is set, shows a `NULL` text or caption as empty, and returns 1 or 2 (`user32.c:168`–`:172`). It blocks the game thread until the page answers through `CTL_DIALOG` (`web.c:78`–`:79`).

**Keyboard (`input.js`)**
- **`KEYMAP`** maps `KeyboardEvent.code` to a virtual key and a set-1 scancode, whatever the keyboard layout (`input.js:18`–`:20`). Extended keys reuse their base scancode because the game sees no E0 prefix (`tests/unit/input.test.mjs:85`). Unmapped codes such as Meta and Pause go to the browser (`src/web/input.js:136`–`:138`).
- **`lParam`** is `KEY_REPEAT | scancode << KEY_SCANCODE_SHIFT`, that is `1 | scancode << 16`, with no extended, previous-state or transition bit (`input.js:56`; `src/platform/web_layout.h:102`–`:103`). Auto-repeat sends more `WM_KEYDOWN`s, and `WM_CHAR`s, with the same `lParam` (`src/web/input.js:147`–`:152`).
- **`WM_CHAR`** follows a `WM_KEYDOWN` whose `event.key` is one character below 256 while typing is allowed, or which is Backspace, Tab, Enter or Escape (`input.js:47`–`:53`, `:150`–`:152`). Meta always blocks typing; Ctrl blocks it unless AltGr is down, since AltGr also sets `ctrlKey` on Windows (`input.js:45`–`:48`; `tests/unit/input.test.mjs:105`–`:108`). Only Latin-1 types: `€` gives nothing (`input.test.mjs:104`).
- **One rule in two places:** the control keys that type their own virtual key are listed in `src/web/input.js:53` and again in `src/platform/user32.c:102`; the contract is written once, at `src/platform/web_layout.h:92`–`:96`.
- **Releases pair with presses.** `WM_KEYUP` goes only for keys `Input` saw pressed (`src/web/input.js:142`–`:145`). A release the full ring refuses waits in `unsent`; `release` and every `poll`, enabled or not, send the waiting releases in the order the keys were released, stopping at the first refusal (`input.js:103`, `:155`–`:165`, `:191`). The page polls once per animation frame (`src/web/main.js:253`). A new press of the key drops its waiting release (`src/web/input.js:148`).
- **`releaseKeys`** releases every held key and the left button on window `blur` and on `setEnabled(false)` (`input.js:106`, `:121`–`:123`, `:169`–`:173`). The page disables input when the session stops or a dialog opens (`src/web/main.js:169`–`:170`).
- **Browser keys:** F5, F11 and F12 reach the game without `preventDefault`; every other mapped key is prevented (`src/web/input.js:16`, `:139`–`:140`).
- **Alt+Enter** requests fullscreen once per press and is not forwarded, so the game sees only Alt (`input.js:130`–`:134`; `tests/e2e/input.spec.mjs:57`).

**Mouse**
- **Mapping:** `fitViewport` gives the picture rectangle, letterboxed when `Renderer.keepAspect()` is true (`src/web/input.js:180`; `src/web/renderer.js:78`, `:152`). The pointer is scaled to 640×480, floored and clamped, and each move bumps `CTL_MOUSE_MOVES` (`input.js:181`–`:185`). Nothing is published while input is disabled (`input.js:177`–`:178`).
- **Left button only.** `pointerdown` on the canvas sets `MOUSE_LEFT | MOUSE_LEFT_LATCH` while enabled, `pointerup` anywhere clears `MOUSE_LEFT`, and the context menu is suppressed (`input.js:108`–`:117`). C clears the latch as it reads the button, so a click shorter than one poll is still seen (`src/platform/dinput.c:450`–`:456`).
- **Relative motion.** `GetDeviceData` reports the delta from the game's cursor (`src/platform/platform.h:33`) to the pointer, only after `CTL_MOUSE_MOVES` changes (`dinput.c:485`–`:492`). A call that finds the game moved its own cursor adopts it and reports neither motion nor the button; the latch keeps a click for the next call (`dinput.c:478`–`:482`).
- **Records:** X, Y and the left button come first, and any further records report the wheel, which never moves; a joystick or fewer than three records get nothing (`dinput.c:169`–`:172`, `:471`–`:474`). The button reads as pressed only when the previous call saw it released (`dinput.c:457`).

**Gamepads**
- **`padState`** (`src/web/input.js:74`): a `standard` pad gives axes 0–3 in the SDL range, the triggers as the fifth and sixth axes (−32768 when released), and buttons 12–15 as the hat (`input.js:78`–`:83`). Other pads pass through up to six axes (`input.js:85`). At most 12 buttons are reported (`input.js:14`, `:87`).
- **`Input.poll`** publishes the first two connected pads, and none while disabled (`input.js:192`–`:197`).
- **Devices:** `CreateDevice` accepts only the mouse and the two joystick GUIDs (`src/platform/dinput.c:210`–`:211`). `EnumDevices` lists both joysticks, connected or not, because browsers reveal a pad only after its first button press; an absent one reads centred (`dinput.c:221`–`:232`, `:415`–`:418`). Joysticks report force feedback, 32 buttons and 6 axes (`dinput.c:260`–`:262`).
- **`GetDeviceState`** fills `DIJOYSTATE` only for a joystick and an exact 80-byte size; otherwise it returns `DI_OK` with the buffer untouched (`dinput.c:407`–`:408`). It reads each pad word separately (`dinput.c:415`–`:443`).
  - **Keys from buttons:** the configured Escape and reset buttons post Escape, or F11/F12 for pad 0/1 (`dinput.c:156`–`:158`, `:345`–`:346`). An emulated key changes state only when `user32_post_key` accepts the message, so a refused change is posted again on the next poll (`dinput.c:332`–`:337`). A pad that reads disconnected, as every pad does while input is disabled, releases its emulated keys (`dinput.c:417`).
  - **D-pad:** a hat drives the arrow keys and diagonals press nothing; without a hat, the configured d-pad buttons drive them (`dinput.c:161`–`:162`, `:349`). No buttons are reported while the hat is held (`dinput.c:427`), nor buttons that press keys (`dinput.c:354`–`:359`, `:431`).
  - **Buttons** report `0x80`. While the controls screen waits for an assignment, a button must be held for 50 polls, so that axes which also act as buttons are detected as axes; a button held for part of that time stays hidden until released as long (`dinput.c:38`, `:40`, `:362`–`:372`).
  - **Axes** are hidden from the thread that created DirectInput (the menu thread) when the pad has a hat or `JoystickDisableAxesInMenu` is set, except during assignment (`dinput.c:387`–`:393`, `:175`, `:246`). Each output up to the pad's axis count takes its configured source axis (none for −1; a source the pad does not publish reads centred), converted to 0..65535 and optionally halved or inverted-halved (`dinput.c:375`–`:384`, `:437`–`:444`); outputs 3–5 fill Rz and the sliders, so Rx and Ry stay centred (`dinput.c:164`–`:166`).
- **`processAxes`** replaces the game's axis routine, which assumes Windows ranges, and maps one encoded axis zone to 0..255 (`dinput.c:555`–`:558`, `:610`). `JoystickApplyDeadzone` measures each zone from its own boundary instead of the centre or the axis end, leaving a dead band (`dinput.c:586`, `:593`, `:607`). Zone bytes are exchanged in pairs as upstream does (`dinput.c:560`–`:563`).

**COM objects**
- Each object starts with its vtable pointer, so the interface pointer is the object pointer (`dinput.c:7`–`:9`). Slots the game does not call are `NULL` (`dinput.c:103`–`:148`).
- `QueryInterface` ignores the IID and returns the same object with one more reference (`dinput.c:189`–`:194`); `Release` frees at zero references (`dinput.c:197`–`:200`). `object_new` returns `NULL` when `calloc` fails, and its callers then return `DIERR_OUTOFMEMORY` (`dinput.c:181`–`:182`, `:213`–`:214`).

**Trust, bounds and concurrency**
- **DOM input is bounded before it is shared:** `KEYMAP` keys only, a clamped pointer, clamped axes, at most 12 buttons (`src/web/input.js:136`, `:183`–`:184`, `:75`, `:87`). C re-checks pad indices and counts (`src/platform/dinput.c:210`, `:329`, `:396`–`:399`, `:443`); a static assert keeps the axis cap within the pad's axis words (`:167`). And `GetObjectInfo` clears the structure after its size word, never past 316 bytes (`dinput.c:315`–`:317`).
- **Pad words are not a snapshot:** each is a separate store with no sequence lock, so C can mix two frames (`src/web/input.js:200`–`:206`). `PAD_CONNECTED` is written last (`input.js:206`).
- **Device state, the hold counters and the last left-button state are unlocked** (`src/platform/dinput.c:96`–`:100`, `:176`–`:177`). This assumes one polling thread per device; nothing confirms it.

## Key Types and Entry Points

1. **`Input`** (`src/web/input.js:93`): built from the canvas, the control `Int32Array`, `keepAspect()` and `onFullscreen()` (`input.js:96`). Adds window and canvas listeners, starts disabled (`input.js:101`), has no teardown.
2. **`Input.key`, `setEnabled`, `releaseKeys`** (`input.js:127`, `:121`, `:169`): key events → ring messages or the fullscreen callback, ignored while disabled. Disabling sends `WM_KEYUP` for held keys and clears `MOUSE_LEFT`. A press the full ring refuses is lost; a refused release waits for `poll`.
3. **`Input.pointer`, `Input.poll`** (`input.js:176`, `:190`): store the mouse and pad words; `pointer` does nothing while disabled. `poll` must run every frame: it sends waiting releases and, while disabled, reports no pads.
4. **`pushMessage(ctl, message, wParam, lParam)`** (`input.js:59`): appends an entry, publishes the head, notifies `CTL_SEQ`. Returns `false` and writes nothing when 256 entries are pending (`src/platform/web_layout.h:44`).
5. **`KEYMAP`, `charCode(event, vk)`, `padState(pad)`** (`src/web/input.js:20`, `:47`, `:74`): pure. Code → `[vk, scancode]`; the `WM_CHAR` value (0 for none); Gamepad → `{axes, buttons, hat, numHats, numButtons}`.
6. **`GetMessageA_wrap`, `DispatchMessageA_wrap`** (`src/platform/user32.c:108`, `:131`): block until a message arrives, fill the `MSG` and return 1, never 0. Dispatch calls the window procedure stored by `user32_open_window` (`user32.c:83`) and returns 0.
7. **`user32_post_key(down, vk, scancode)`** (`user32.c:97`): queues a key message from C, plus `WM_CHAR` for a pressed control key. Returns `false` and queues nothing when the key message does not fit.
8. **`MessageBoxA_wrap`** (`user32.c:169`): blocks the calling thread on the page dialog; returns `IDOK` 1 or `IDCANCEL` 2.
9. **`GetDeviceState`, `EnumDevices`** (`src/platform/dinput.c:405`, `:224`): joystick state from the pad words and `g_config`; may post emulated keys. A wrong size or a mouse device returns `DI_OK` with the buffer untouched; a missing pad reads centred and releases its emulated keys. `EnumDevices` lists both pads for the joystick type only, until the callback stops it.
10. **`GetDeviceData`** (`dinput.c:467`): mouse delta and left-button edge; clears `MOUSE_LEFT_LATCH`. Returns `DI_OK` and writes nothing for a joystick or fewer than 3 records (`dinput.c:471`).

## Interactions

- **[Shared memory](shared-memory.md):** `CTL_SEQ`, the ring, `CTL_MOUSE_*`, the pad words and the key-message constants (`src/platform/web_layout.h:24`–`:127`). JS produces the words; C writes only `CTL_RING_TAIL`, the latch clear and `CTL_SEQ` bumps for its local queue (`src/platform/user32.c:79`, `src/platform/dinput.c:455`, `user32.c:53`). Both mirrors change together (`tests/unit/layout.test.mjs:30`); the browser test takes its constants from `layout.js` (`tests/e2e/input.spec.mjs:4`).
- **[Page](page.md):** `play()` creates `Input` (`src/web/main.js:302`); `updateInputEnabled` enables it only while running with no dialog open (`main.js:169`–`:170`); the frame loop calls `poll` (`main.js:253`); `messageBox` answers `CTL_DIALOG` (`main.js:189`–`:190`); fullscreen locks Escape so the game gets it (`main.js:221`).
- **[Graphics](graphics.md):** the pointer mapping must match where the picture is drawn (`src/web/renderer.js:78`, `:152`). `keepAspect` needs both `CTL_CFG_KEEP_ASPECT` from `nfs2se.conf` and the page setting (`renderer.js:153`; `src/platform/wrapper.c:145`).
- **[Platform](platform.md):** this module uses the futex, dialog and tick helpers (`src/platform/web.c:58`, `:63`, `:71`, `:96`), the stdcall glue and game variables (`src/platform/platform.h:26`–`:33`) and the joystick `Config` fields (`platform.h:67`–`:73`) parsed at `src/platform/config.c:40`–`:41` and `:70`–`:95`. Platform calls `user32_open_window` (`src/platform/wrapper.c:155`), polls `user32_window_alive` at exit (`wrapper.c:77`) and reports two joysticks to the game (`wrapper.c:160`–`:162`).
- **Game (NFS2SE.cpp):** calls the `_wrap` imports and the DirectInput vtable slots, whose indices (`src/platform/dinput.c:103`–`:115`) no test checks.
- **Integration tests:** `tests/e2e/input.spec.mjs` sends real Chromium events into the control block. `tests/e2e/game.spec.mjs:41` runs the game to its OK/Cancel box from `MessageBoxA_wrap` (`src/platform/user32.c:169`), checks the Cancel button and clicks OK (`tests/e2e/game.spec.mjs:45`, `:47`).

## How to Test

Prerequisites: as in the root's [Verification and Review Map](../ARCHITECTURE.md#verification-and-review-map).

| Command | Evidence |
| ------- | -------- |
| `node --test tests/unit/input.test.mjs` | `# tests 19`, `# fail 0` |
| `make test-c` | For each C test, its `build/tests/<name>.cjs` path, then `N checks, 0 failed`; `user32` and `dinput` are among them (`Makefile:98`, `:106`, `:118`–`:119`). `make build/tests/dinput.cjs` builds one test; run it with `node build/tests/dinput.cjs` (`Makefile:111`). |
| `make && npx --no playwright test tests/e2e/input.spec.mjs tests/e2e/game.spec.mjs` | `7 passed`. Playwright serves `dist` on port 8123, reusing a running server outside CI (`playwright.config.mjs:4`, `:21`–`:26`). |
| `make test` | Exit 0. Runs `test-c`, `test-unit` and `test-e2e` (`Makefile:116`). |

| Behaviour | Test |
| --------- | ---- |
| `KEYMAP` codes; shared extended scancodes | `tests/unit/input.test.mjs:58`, `:85` |
| `WM_CHAR`: Latin-1, control keys, Ctrl, AltGr, Meta; modifier keys | `input.test.mjs:99`, `:117` |
| Ring entry, full ring, int32 wrap-around | `input.test.mjs:125`, `:134`, `:147` |
| Standard and other pads | `input.test.mjs:160`, `:183` |
| Disabled input; key messages; unpaired releases and unknown keys; browser keys | `input.test.mjs:216`, `:222`, `:234`, `:241` |
| Alt+Enter; release on disable and blur | `input.test.mjs:250`, `:258` |
| Releases the full ring refused, sent by later polls in release order | `input.test.mjs:279` |
| Letterboxed pointer, nothing while disabled; left-button latch | `input.test.mjs:299`, `:314` |
| Pad publishing while enabled | `input.test.mjs:329` |
| Real key, Alt+Enter and mouse events | `tests/e2e/input.spec.mjs:38`, `:57`, `:64` |
| `test_window`: dispatch to the window procedure | `tests/c/user32_test.c:123` |
| `test_post_key`, `test_full_queue`: pad keys, Escape's `WM_CHAR`, a refused key accepted once there is room | `user32_test.c:139`, `:159` |
| `test_post_message`, `test_wait`: the `WM_USER` range and dropped `wParam`, local queue before ring, waiting on `CTL_SEQ` | `user32_test.c:176`, `:189` |
| `test_destroy`, `test_message_box` | `user32_test.c:204`, `:213` |
| `test_objects`, `test_enum`: COM objects, capabilities, object info, enumeration | `tests/c/dinput_test.c:206`, `:258` |
| `test_unplugged`, `test_buttons`: absent pad, wrong size, mouse device, button counts | `dinput_test.c:277`, `:298` |
| `test_keys`, `test_key_queue_full`, `test_hat`: keys from buttons and their release when the pad reads disconnected, retry after a full queue, hat as d-pad | `dinput_test.c:321`, `:382`, `:401` |
| `test_assign_delay`, `test_axes`, `test_menu_axes`: the assignment delay; axis mapping, modes and unpublished sources; menu axes | `dinput_test.c:442`, `:480`, `:551` |
| `test_mouse`: deltas, latch, edges, cursor adoption | `dinput_test.c:593` |
| `test_process_axes`: zones and deadzone | `dinput_test.c:680` |
| `MessageBoxA_wrap` answered by the page | `tests/e2e/game.spec.mjs:41` |
| Joystick config keys | `tests/c/config_test.c:44`, keys at `:51`–`:60` |

**Coverage gaps:**
- The vtable slot indices are unchecked: the C test only checks that the vtable pointer is not `NULL` (`tests/c/dinput_test.c:704`).
- The C tests stub the game and the web helpers (`dinput_test.c:69`–`:77`, `tests/c/user32_test.c:52`). No test sends keys or pad input to the running game: `game.spec.mjs` checks only the message box and the exit.
- A press or `WM_CHAR` the full ring refuses, a C `WM_CHAR` the full local queue drops, and torn pad reads are untested.
- Real gamepads, non-US layouts (AltGr on a real keyboard, dead keys, IME) and browsers other than Chromium (`playwright.config.mjs:15`) are untested.

## Review and Refactor Guide

| Change | Inspect | Tests |
| ------ | ------- | ----- |
| Add or fix a key | `KEYMAP`, `charCode` (`src/web/input.js:20`, `:47`) | `input.test.mjs:58`, `:85`, `:99`, `:117`; `input.spec.mjs:38` |
| Control keys that type | Both `src/web/input.js:53` and `src/platform/user32.c:102`, and the contract at `src/platform/web_layout.h:92`–`:96` | `input.test.mjs:99`; `tests/c/user32_test.c:139` |
| New page-to-game message | `pushMessage`; `GetMessageA_wrap` forwards any message (`src/platform/user32.c:117`); entries have a spare fourth word (`web_layout.h:45`, `:84`–`:89`) | `input.test.mjs:125`; `user32_test.c:176` |
| Control-word layout | Both mirrors ([shared memory](shared-memory.md)) | `tests/unit/layout.test.mjs`; `make test` |
| Pointer or letterbox | `Input.pointer`, `fitViewport`, `Renderer.keepAspect` and `GetDeviceData` together | `input.test.mjs:299`; `input.spec.mjs:64`; `tests/unit/renderer.test.mjs:8`, `:16`; `tests/c/dinput_test.c:593` |
| Joystick mapping or keys | `GetDeviceState`, `processAxes`, `Config` (`src/platform/platform.h:63`), `src/platform/config.c:70` | `make test-c` (`dinput`, `config`) |
| When input is enabled | `updateInputEnabled` (`src/web/main.js:169`), `setEnabled` | `input.test.mjs:216`, `:258`, `:279` |

- **Forbidden coupling:** `input.js` imports only `layout.js` and `fitViewport` (`src/web/input.js:4`–`:12`). Page logic stays in `main.js`, behind the constructor callbacks. C reads input only from shared words.
- **Compatibility:** keep the Windows struct sizes, the vtable slot indices and the upstream `Joystick*` key names (`src/platform/config.c:70`–`:95`).

**Proposals (not adopted):**
- **Rumble** through the Gamepad `vibrationActuator`, fed by a new pad word. Benefit: force feedback. Check: unit tests for the word and a manual test with a real pad.
- **Pad sequence lock:** a new per-pad word, in both layout mirrors, that `poll` makes odd before writing a pad and even after; `GetDeviceState` rereads until it sees the same even value on both sides (`src/web/input.js:200`–`:206`, `src/platform/dinput.c:415`–`:443`). Benefit: no state mixed from two frames. Check: a unit test of the store order and a C test that reads a pad mid-update.

## Open Gaps / Roadmap

- Force feedback is accepted and ignored (`src/platform/dinput.c:5`, `:519`). The root lists it under [Roadmap](../ARCHITECTURE.md#roadmap).
- Gameplay feel, real gamepads and real keyboards are unverified with game data.
- The game cursor and the browser pointer stay apart after the game moves its cursor, until the pointer moves (`dinput.c:464`–`:465`).
- A key press the full ring refuses is lost with its `WM_CHAR`, while its release still follows (`src/web/input.js:149`, `:152`). In C, a control-key `WM_CHAR` that no longer fits the local queue is dropped (`src/platform/user32.c:103`).
- Pad reads can mix two frames (`src/web/input.js:200`–`:206`).
- `Release` frees a joystick device without releasing its emulated keys, so the game gets no key-up for a key held at that moment (`src/platform/dinput.c:197`–`:202`).
- `WM_CHAR` covers Latin-1 only (`input.js:51`). AltGr typing relies on `getModifierState('AltGraph')` and is tested only with a synthetic event (`input.js:48`; `tests/unit/input.test.mjs:106`–`:107`).
- `DestroyWindow_wrap` fails when the local queue is full, and shutdown then waits out the 2.5 s grace (`src/platform/user32.c:159`; `src/platform/wrapper.c:76`–`:77`).

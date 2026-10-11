---
eatmycode_version: "1.2.0"
---
# Platform

## Goal

- **Owns:** the Win32 emulation that the game (`third_party/NFSIISE/src/Cpp/NFS2SE.cpp`) links against: process start, files and paths, threads and synchronisation, the timer, `nfs2se.conf`, track records, `vsprintf`, Winsock, message boxes and exit.
- **Non-goals:**
  - Changing the game, which is compiled as-is (`Makefile:53`).
  - Network play (`src/platform/wsock32.c:1`) and serial-link play (`src/platform/kernel32.c:3`).
  - Window messages and DirectInput ([input](input.md)), Glide ([graphics](graphics.md)), sound ([audio](audio.md)) and the region layout ([shared memory](shared-memory.md)).
- **Kind:** infrastructure for every other C subsystem.

## Status

`done` for everything the tests reach:
- `make test` exited 0 on 2026-10-11. The six C tests of this module passed in it: `config_test` (117 checks), `fmt_test` (23), `kernel32_test` (87), `records_test` (352), `timer_test` (1) and `wrapper_test` (40). `tests/e2e/game.spec.mjs` alone printed `4 passed`.
- The link uses `-Wl,--fatal-warnings`, so `make` fails on any import whose signature differs from the game's declaration (`Makefile:42`).
- `tests/e2e/game.spec.mjs:41` runs start-up on dummy files up to the missing-movie box. It answers the box, sees exit status 0 and finds the seeded `nfs2se.conf` in IDBFS.

**Unverified:**
- Everything after that point with real game data: the game's threads, events, the timer, mappings, records, exit procedures and the worker count. There is no game data on this machine.
- `web.c` and `wsock32.c`, which have no C tests.

## Code Structure

| File | Role |
| ---- | ---- |
| `src/platform/platform.h` | ABI note, glue, game variables, `Config`, internal API, `ctl_load`/`ctl_store` |
| `src/platform/web.c` | `main`, `web_shared`, futexes, message box, save directory, FS-dirty flag, ticks, sleep |
| `src/platform/wrapper.c` | `WrapperInit`, `WrapperCreateWindow`, `wrapper_fatal`, exit procedures and their watchdog, SDL and libc shims |
| `src/platform/kernel32.c` | Threads, critical sections, events, files, mappings, directories, search, system info, `ExitProcess` |
| `src/platform/timer.c` | Game timer thread, WinMM stubs |
| `src/platform/records.c` | Track records: a track's defaults text to the 620-byte table |
| `src/platform/config.c` | `nfs2se.conf`, path mapping, game-directory check |
| `src/platform/fmt_x86.c` | `vsprintf` over x86 argument slots |
| `src/platform/wsock32.c` | Winsock that reports no network |
| `tests/c/` | `config`, `fmt`, `kernel32`, `records`, `timer` and `wrapper` tests of this module |
| `Makefile` | Game flags (`Makefile:31`), platform objects (`:57`), link (`:61`), C test sources (`:97`) |

## Language and Conventions

- **Languages:** C (GNU11) with warnings as errors (`Makefile:33`). The game is C++14 (`Makefile:31`). Root rules: [Coding Style and Code Design](../ARCHITECTURE.md#coding-style-and-code-design).
- **Import signatures (required):** `int32_t` results and 32-bit arguments; pointers are 32-bit in wasm32 (`src/platform/platform.h:3`). A mismatch fails the link (`Makefile:42`).
- **Names:** imports keep the names the game links against. Win32, SDL and libc imports end in `_wrap` (`kernel32.c:128`, `wrapper.c:192`); wrapper entry points have no suffix (`wrapper.c:111`, `timer.c:59`, `records.c:106`). Internal API carries its file's prefix (`platform.h:78`, `:92`, `:104`).
- **Calling game code (required):** always with a thread context. A `GameFn` is called directly (`kernel32.c:122`, `wrapper.c:61`); functions with arguments go through the glue (`platform.h:22`, `records.c:114`).
- **Win32 structures** carry `_Static_assert` sizes (`kernel32.c:69`, `:82`, `:93`, `records.c:25`).
- **Shared words** use `ctl_load`/`ctl_store` (`platform.h:112`) and futexes (`web.c:58`). Game variables written by other threads are read with relaxed atomics (`timer.c:17`).
- **Upstream semantics:** headers name the part of NFSIISE's MIT wrapper each file follows (`kernel32.c:2`, `timer.c:3`, `records.c:4`, `config.c:2`, `wrapper.c:3`), and kept quirks are commented (`kernel32.c:377`). Write original code; never paste upstream source ([Mission and Constraints](../ARCHITECTURE.md#mission-and-constraints)).
- **Reuse:** the handle header (`kernel32.c:31`), `config_convert_path` and `config_fopen` (`config.c:201`, `:211`), `wrapper_fatal` (`wrapper.c:24`) and `web_mark_fs_dirty` (`web.c:89`).
- **Constants** are named, with their unit or reason in a comment (`platform.h:17`, `web.c:17`, `wrapper.c:16`, `kernel32.c:29`).
- **Enforced:** warnings, signatures, struct sizes. **Observed only:** naming, comments, error values.

## Design and Invariants

### Start-up

1. The page writes the support files (`Makefile:23`), listed in `files/manifest.txt` (`:69`), and the imported data to MEMFS `/game`. It then mounts and loads IDBFS at the save directory (`src/web/main.js:130`, `:141`–`:143`, [page](page.md)).
2. It calls `callMain([GAME_DIR, SAVE_DIR])`, that is `/game` and `/home/web_user/.nfs2se` (`main.js:310`, `:14`, `:15`).
3. `main` (`src/platform/web.c:114`) sets the tick base (`web.c:116`), enters `argv[1]` (`:117`) and keeps `argv[2]` as the save directory (`:122`). Then it runs the game (`:123`).
4. `WrapperInit` (`src/platform/wrapper.c:111`):
   - creates the save directory with `config`, `save`, `stats`, `stats/prh` and `tmptrk` (`wrapper.c:113`, `:120`–`:125`);
   - seeds a missing `nfs2se.conf` from `/game/nfs2se.conf.template` (`:126`–`:128`). The copy leaves no partial file and marks the FS dirty on success (`:83`, `:99`–`:102`);
   - enables the redirects only if that file opens; otherwise it reads `./nfs2se.conf` or keeps the defaults (`:130`–`:143`);
   - publishes `CTL_CFG_KEEP_ASPECT` and `CTL_CFG_LINEAR_FILTER`, then returns 0 (`:145`–`:147`).
5. `WrapperCreateWindow` (`wrapper.c:150`) needs the `gamedata` and `fedata/pc` directories (`src/platform/config.c:231`). If one is missing, it calls `wrapper_fatal` with the reason (`wrapper.c:154`). Otherwise it opens the window (`src/platform/user32.c:83`).

**Save directory (observed):** `web_save_dir` returns `argv[2]` (`web.c:84`), which the page sets to `SAVE_DIR`, its IDBFS mount point (`main.js:15`, `:142`). C hard-codes no save path. `game.spec.mjs:52` fails if the seeded conf does not reach IDBFS.

### Files and paths

- **Mapping** (`config.c:201`):
  - With a settings directory, user data goes to `<save dir>/<subdir>/<rest of the name>` (`config.c:208`). User data is everything under `.\fedata\pc\config\`, `.\fedata\pc\save\` and `.\gamedata\tmptrk\`, `.stf` names under `.\fedata\pc\stats\` and its `prh\`, and `replay.rpy`, which goes to `tmptrk` (`:160`–`:169`). Prefixes match case-insensitively (`:141`).
  - The rest of a redirected name is kept verbatim, case and backslashes included, so backslash-separated parts stay one file name (`:150`–`:152`; `tests/c/config_test.c:107`). A `/` in it still separates directories (`config.c:208`).
  - Other paths stay relative to `/game`: `\` becomes `/`, and letters are lowered on request (`:182`–`:194`).
  - Out of memory gives NULL (`platform.h:81`).
  - The redirects exist only once `WrapperInit` opened the settings file (`wrapper.c:132`). Otherwise user data stays in `/game`, which is not persisted; only the save directory is (`main.js:142`).
- **Case (required):** game files are opened or deleted through lower-cased paths (`kernel32.c:341`, `:500`, `config.c:213`, `records.c:91`), and MEMFS is case-sensitive. The importer therefore stores lower-case paths (`src/web/importer.js:288`). Directory names keep their case (`kernel32.c:544`).
- **Files:** `CreateFileA_wrap` (`kernel32.c:328`) creates and truncates for `GENERIC_WRITE`, or opens read-only for `GENERIC_READ` (`:342`–`:345`). Any other access and `\\.\COMx` fail (`:347`, `:336`); share mode, disposition and flags are ignored (`:331`–`:335`). Short reads and writes return FALSE with the count done (`:377`, `:385`). Move methods equal `SEEK_*` (`:391`).
- **Mappings:** a view is a private heap copy of the whole file plus four zeroed bytes, so writes never reach the file (`kernel32.c:416`, `:29`). An empty or oversized file, or one that cannot be read up to its size, gives no view (`:417`, `:425`, `:435`). The mapping reuses the file's descriptor (`:412`), so the file handle must stay open until the view is made.
- **Search** (`kernel32.c:589`): only the current directory (`:594`). Names match the game's pattern ignoring case; directories, and dot names the pattern does not spell out, are skipped (`:558`). Only `fileName` is filled; the rest is zeroed (`:560`–`:561`).
- **Persistence:** `web_mark_fs_dirty` (`web.c:89`) makes the page run `FS.syncfs` (`main.js:148`). It is called when a file is finished or deleted: by `fclose_wrap` for files not opened read-only (`wrapper.c:200`), `CloseHandle_wrap` for writable files (`kernel32.c:519`), `DeleteFileA_wrap` (`:502`), the conf seeding (`wrapper.c:99`) and records (`records.c:98`). `WriteFile_wrap`, `SetEndOfFile_wrap` and a truncating open do not call it (`kernel32.c:380`, `:395`, `:343`), so writes to a file that is never closed start no sync. New writers must call it when they finish.
- **Records:** unless clearing, `fetchTrackRecords` (`records.c:106`) reads the track's defaults from the game's path at index track + 20 (`:112`–`:115`, `:15`); without the file the table is empty. Lines are cut to 127 characters, and names end at a tab and keep 9 (`:36`, `:66`–`:67`). The 620-byte table goes to the path at index track (`:123`–`:124`), written whole or not at all (`:88`). Its location depends on the game's path; `.stf` stats names are redirected (`config.c:163`).

### Threads and synchronisation

- **Threads:**
  - `CreateThread_wrap` (`kernel32.c:128`) starts a detached pthread with a 4 MiB stack (`:28`, `:143`–`:144`). It runs the game's start routine with its context (`:122`).
  - The thread first waits on a resume semaphore. It is posted at creation unless `CREATE_SUSPENDED` is set (`:139`), or later by `ResumeThread_wrap` (`:159`).
  - The handle and the unstarted thread share the object by reference count (`:39`, `:106`).
  - `stackSize` is ignored because the game allocates its own emulated stack (`:131`). Priority and termination calls do nothing and return 0 (`:165`, `:182`); `DuplicateHandle_wrap` fails (`:189`).
  - It returns 0 when allocation or `pthread_create` fails (`:133`, `:148`).
- **Handles** are heap objects that start with a `HandleType` (`kernel32.c:31`). `CloseHandle_wrap` dispatches on it and rejects NULL and `INVALID_HANDLE` (`:506`–`:508`).
- **Critical sections** are recursive pthread mutexes (`kernel32.c:206`, `:212`), stored by pointer in the game's 24-byte struct (`:66`, `:69`). Running out of memory there is fatal, as the call cannot fail (`:214`).
- **Events:** all events share one mutex and condition variable, and `SetEvent_wrap` broadcasts (`kernel32.c:101`, `:263`). `WaitForMultipleObjects_wrap` (`:307`) ignores `waitAll` (`:309`):
  - the lowest signalled event wins and only it is acquired; an auto-reset event is reset by that (`:304`, `:276`);
  - a timeout of 0 polls, `INFINITE` waits forever, and any other value waits until a `CLOCK_REALTIME` deadline (`:313`–`:321`, `:286`).
- **Timer** (`src/platform/timer.c:30`): one thread, started by `startTimer` and joined by `stopTimer` (`timer.c:59`, `:66`). A thread that cannot start is fatal, as the game never retries (`:58`).
  - The period is a game variable in 16.16 fixed-point ms (`:15`). Deadlines accumulate, and a tick more than a second late resynchronises (`:45`–`:51`).
  - At `FAST_PERIOD` it ticks as fast as possible, sleeping 1 ms between ticks (`:10`, `:39`). Each tick signals the game's timer events (`:20`).
- **Workers:** with `PROXY_TO_PTHREAD`, `main` and every thread run in workers, so blocking never stalls the page (`web.c:1`). The pool pre-starts 8 workers, sized for the platform's four threads and four of the game's; more threads start once a worker is created for them (`Makefile:36`–`:38`, `:43`).
- **Time:**
  - `web_ticks` counts milliseconds since `main`, with one origin across workers (`web.c:94`), and `web_sleep_ms(0)` yields (`:103`).
  - `SDL_GetTicks_wrap` and `SDL_Delay_wrap` wrap them (`wrapper.c:165`, `:170`). `SDL_NumJoysticks_wrap` reports `CTL_PAD_COUNT`, which is 2 (`wrapper.c:160`, `web_layout.h:41`).
  - `time_wrap` truncates `time_t` to 32 bits (`wrapper.c:221`).

### Message boxes

- **Flow** (`web.c:71`): one box at a time across threads (`web.c:73`).
  - `web_message_box` logs to stderr, stores 0 in `CTL_DIALOG` and calls `Module.nfsMessageBox` on the browser main thread (`:74`–`:76`, `main.js:180`).
  - It then waits on `CTL_DIALOG` in one-second futex slices (`web.c:17`, `:78`) until the page stores `DIALOG_OK` (1) or `DIALOG_CANCEL` (2) (`main.js:189`, `web_layout.h:66`).
  - Only the calling thread blocks, with no timeout. A box closed without a button answers OK when it has no Cancel button, and Cancel otherwise (`main.js:187`).
- **Callers:** `MessageBoxA_wrap` (`user32.c:169`), `wrapper_fatal` (`wrapper.c:24`) and `main` (`web.c:119`). The latter two use the caption `GAME_TITLE` (`platform.h:16`).

### Exit

- `ExitProcess_wrap` runs `wrapper_exit_procs`, then calls `exit(code)` (`kernel32.c:631`, `:634`).
- `wrapper_exit_procs` (`wrapper.c:69`) runs the procedures registered by `WrapperAtExit` (`:32`), at most 10 (`:18`). They run in order and each once; a nested call finds none left (`:67`, `:71`–`:72`).
  - Each runs with `main_game_thread` (`:61`) under a watchdog thread that calls `exit(0)` after 2500 ms (`:16`, `:45`–`:52`). If the watchdog cannot start, the procedure runs unwatched (`:60`).
  - Then it waits up to 2500 ms while the window is alive (`:76`–`:78`). `DefWindowProcA_wrap` ends that on `WM_DESTROY` (`user32.c:151`).
- `GetMessageA_wrap` never returns 0, as nothing posts `WM_QUIT` (`user32.c:107`).
- `wrapper_fatal` shows a box, then calls `exit(-1)` (`wrapper.c:27`).
- With `-sEXIT_RUNTIME=1` (`Makefile:43`), `exit` and a return from `main` reach the page's `onExit`. Status 0 shows "The game has quit."; other statuses show their number (`main.js:416`).

### Other services

- **`vsprintf_wrap`** forwards to `fmt_x86_vsprintf` (`wrapper.c:178`). Arguments are 4-byte x86 slots with 4-byte-aligned doubles, which a wasm `va_list` cannot express, so each conversion is formatted separately (`src/platform/fmt_x86.c:1`).
  - It supports flags, `*` widths and precisions, and the `hh`, `h`, `ll`, `l`, `I64` and `L` modifiers (`fmt_x86.c:80`–`:82`). The rebuilt spec has a fixed size, because width and precision travel as `*` arguments and repeated flags are merged (`:17`–`:21`, `:28`, `:86`).
  - `%p` prints eight upper-case hex digits (`:164`), `%s` prints `(null)` for NULL (`:160`), and `%n` stores the count so far through its argument (`:180`).
  - Unknown conversions and out-of-range widths or precisions are copied verbatim without consuming an argument (`:71`, `:220`). A format that ends inside a conversion drops it (`:212`).
  - Output is unbounded, as the caller's buffer size is unknown (`:7`).
- **Winsock** (`src/platform/wsock32.c`):
  - `WSAStartup_wrap` returns `WSASYSNOTREADY` (`wsock32.c:12`), and `WSAGetLastError_wrap` gives `WSAENETDOWN` (`:24`).
  - `socket_wrap` and `accept_wrap` return `INVALID_SOCKET` (`:36`, `:50`). `closesocket_wrap`, `bind_wrap` to `recvfrom_wrap`, and `gethostname_wrap` return `SOCKET_ERROR` (`:44`, `:58`–`:146`, `:163`).
  - `gethostbyname_wrap`, `WSACleanup_wrap` and `WSAFDIsSet_wrap` return 0 (`:157`, `:19`, `:29`). `inet_addr_wrap` gives `INADDR_NONE` and `inet_ntoa_wrap` gives `0.0.0.0` (`:175`, `:181`). Only `htons_wrap` computes a value (`:170`).
  - The header says the game then offers only single-player and split-screen (`:1`); this is unverified.
- **libc:** `fopen_wrap` maps paths (`wrapper.c:192`), and `fclose_wrap` marks the FS dirty (`:197`). `fscanf_wrap` forwards to `vfscanf` (`:183`). `malloc_wrap`, `calloc_wrap` and `free_wrap` forward to libc (`:205`–`:215`).
- **System information:** 2 GiB − 1 byte of total physical, page-file and virtual memory, with 0 bytes of free physical memory, and 4096-byte pages (`kernel32.c:608`–`:614`, `:621`). `GetCurrentDirectoryA_wrap` returns the buffer length, not the path length (`:541`). `GetLastError_wrap` maps only `ENOENT` (`:534`).

### Configuration

- **Defaults** (`config.c:17`): keep aspect and linear filtering on (`config.c:20`–`:21`), and the joystick dead zone and in-menu axis disabling off (`:19`). Each pad has identity axes in full mode and no Esc, reset or D-pad buttons (`:25`–`:29`).
- **Parsing** (`:118`): one `Key=value` per line, with CR and LF stripped (`:125`). Empty lines, lines starting with a space or `#`, and lines without `=` are skipped (`:127`, `:102`). Keys match case-insensitively and in full (`:44`). Flags are integers; non-zero is true (`:111`).
- **Keys:**
  - `KeepAspectRatio`, `LinearTextureFiltering`, `JoystickApplyDeadzone` and `JoystickDisableAxesInMenu` (`:37`).
  - `Joystick<pad>Axes2` (six source axes, a colon, six modes), `EscButton`, `ResetButton` and `DPadButtons` (`:70`, `:83`–`:95`). `<pad>` is 0 or 1 (`:78`).
  - A malformed list keeps the old values (`:49`). Button numbers are read with `strtol`, unchecked (`:91`).
- **Ignored:** window, CPU, MSAA, vsync, sound interpolation, network and COM-port keys, like unknown keys (`:98`).
- **Use:** `KeepAspectRatio` and `LinearTextureFiltering` feed `CTL_CFG_*` (`wrapper.c:145`). The renderer combines them with the page settings (`renderer.js:153`, `:292`). The joystick keys feed DirectInput (`dinput.c:345`, `:440`).
- **Compatibility:** the key format is upstream's, so existing files keep working (`config.c:2`).

### Errors and resources

- Failures follow Windows conventions: 0 or FALSE, `INVALID_HANDLE` (−1, `kernel32.c:21`), `WAIT_TIMEOUT` (`:22`) and Winsock codes.
- Allocation failures return the call's failure value (`kernel32.c:133`, `:248`, `:350`, `:408`, `:428`, `:591`, `config.c:208`). A critical section cannot report failure, so there it is fatal (`kernel32.c:214`).
- Fatal conditions show a box, then end the process:
  - a missing `/game` returns 1 from `main` (`web.c:117`);
  - missing game directories (`wrapper.c:154`), a timer that cannot start (`timer.c:61`) and a critical section without memory call `wrapper_fatal`, which exits with −1 (`wrapper.c:27`).
- The conf seeding (`wrapper.c:83`) and the records table (`records.c:88`) are written whole or not at all.
- Memory starts at 256 MiB and may grow to 2 GiB (`Makefile:44`).

## Key Types and Entry Points

1. **`main`** (`src/platform/web.c:114`): called by `callMain` with the game directory and the save directory. Enters the first, keeps the second for `web_save_dir`, runs the game and returns 0. With fewer arguments or no game directory, it shows a box and returns 1.
2. **`WrapperInit`** (`src/platform/wrapper.c:111`): called at game start. Creates the settings tree, seeds and parses `nfs2se.conf`, enables the redirects, publishes `CTL_CFG_*` and may mark the FS dirty. Always returns 0; without a usable conf the defaults apply, and without the settings file user data stays in `/game`.
3. **`WrapperCreateWindow`** (`wrapper.c:150`): takes the game's window procedure and returns the handle from `user32_open_window`. Without `gamedata` or `fedata/pc` it shows a box and exits with −1.
4. **`config_convert_path`** and **`config_fopen`** (`src/platform/config.c:201`, `:211`): map a game path to a host path, malloc'd for the caller to free, or NULL when out of memory. User data is redirected while a settings directory is set and keeps the case of its name; game-data paths are lowered on request. `config_fopen` requests lowering and returns the `FILE *` or NULL.
5. **`config_reset`** and **`config_parse`** (`config.c:17`, `:118`): fill `g_config` (`src/platform/platform.h:76`) with defaults, or from an open stream that the caller closes. Unknown keys are ignored and a malformed list keeps its old values; flags and button numbers use unchecked `strtol`, so a non-numeric value reads as 0.
6. **`CreateThread_wrap`** (`src/platform/kernel32.c:128`): starts a detached pthread that runs a `GameFn` with its context, optionally suspended, and stores the thread id. Returns the handle, or 0 when allocation or `pthread_create` fails.
7. **`WaitForMultipleObjects_wrap`** (`kernel32.c:307`): waits for any of `count` events, for 0 ms, a finite time or `INFINITE`. Returns the lowest signalled index, resetting that event if it is auto-reset, or `WAIT_TIMEOUT`.
8. **`web_message_box`** (`web.c:71`): shows `text` with `caption` and OK, or OK and Cancel, on the page, and logs it to stderr. Blocks the caller without timeout until the page answers, then returns `DIALOG_OK` (1) or `DIALOG_CANCEL` (2).
9. **`ExitProcess_wrap`** and **`wrapper_exit_procs`** (`kernel32.c:631`, `wrapper.c:69`): run the exit procedures under watchdogs, wait for the window to close, then `exit(code)`; `ExitProcess_wrap` never returns. A procedure that hangs for 2.5 s ends the process with status 0.
10. **`fmt_x86_vsprintf`** (`src/platform/fmt_x86.c:191`): formats `fmt` with x86 argument slots into `out`, NUL-terminated, and returns the length. Output is unbounded, and `%n` writes through its argument.

## Interactions

- **Game:** calls the imports. This module calls back only with a thread context, directly or through the glue (`platform.h:22`). `make` checks the names and `int32_t` signatures (`Makefile:42`).
- **[Shared memory](shared-memory.md):** writes `CTL_DIALOG`, `CTL_FS_DIRTY` and `CTL_CFG_*` (`web.c:75`, `:91`, `wrapper.c:145`), and waits on `CTL_DIALOG` (`web.c:79`). `web.c` defines the regions (`web.c:19`) and exports their addresses (`:27`). Layout changes follow the [deviations](../ARCHITECTURE.md#project-specific-deviations).
- **[Page](page.md):**
  - `Module.nfsMessageBox` (`web.c:76`, `main.js:180`).
  - `/game`, IDBFS and the arguments before `main` (`main.js:130`, `:310`).
  - `CTL_FS_DIRTY` drives `persist` (`:148`), and `exit` reaches `onExit` (`:416`).
  - Integration test: `game.spec.mjs:41`.
- **[Game files](game-files.md):** lower-case paths relative to `/game` (`importer.js:288`) match the lower-casing lookups. The support files come from the submodule (`Makefile:20`–`:23`, `:65`).
- **[Input](input.md):** this module opens the window and polls whether it is alive (`wrapper.c:155`, `:77`). `MessageBoxA_wrap` uses `web_message_box` (`user32.c:172`). DirectInput reads the joystick keys (`dinput.c:345`).
- **[Graphics](graphics.md):** the renderer reads `CTL_CFG_*` (`renderer.js:153`, `:292`). `glide.c` uses `web_futex_wait` (`glide.c:145`).
- **[Audio](audio.md):** `sound.c` uses `web_futex_wait` (`sound.c:53`) and calls the mixer with `audio_game_thread` (`:39`).
- **[Build and test](build-and-test.md):** the sources of each C test (`Makefile:97`).

## How to Test

**Prerequisites:** as in the root's [Verification and Review Map](../ARCHITECTURE.md#verification-and-review-map).

| Command | Proves | Expected evidence |
| ------- | ------ | ----------------- |
| `make` | Import signatures match (`Makefile:42`) | Exit 0; `dist/nfs2se.js` and `dist/nfs2se.wasm` |
| `make test-c` | This module's six tests, plus `dinput`, `glide` and `user32` | Per test (`tests/c/check.h:51`): `117 checks, 0 failed` (config), 23 (fmt), 87 (kernel32), 352 (records), 1 (timer), 40 (wrapper) |
| `make build/tests/kernel32.cjs && node build/tests/kernel32.cjs` | One test, through the pattern rule (`Makefile:111`); `config`, `fmt`, `records`, `timer` and `wrapper` work alike | `87 checks, 0 failed` |
| `npx --no playwright test tests/e2e/game.spec.mjs` | Start-up, message box, exit, conf persistence | `4 passed`. Run `make` first; Playwright starts `node tools/serve.mjs dist 8123` or, outside CI, reuses one already running (`playwright.config.mjs:21`–`:27`) |
| `make test` | Everything (`Makefile:116`) | Exit 0 |

| Behaviour | Test |
| --------- | ---- |
| Config defaults | `test_defaults` (`tests/c/config_test.c:23`) |
| Comments, leading space, key case and length, CRLF, malformed lists, unknown pads and keys, no `=` | `test_parse` (`config_test.c:44`) |
| Redirects, `.stf`-only stats, lower-casing, verbatim rest | `test_paths` (`config_test.c:87`) |
| `config_fopen` | `test_fopen` (`config_test.c:113`) |
| Game-directory check | `test_game_dirs` (`config_test.c:141`) |
| `vsprintf`: integers, doubles, text and `%p`, fields, `%%`, `%n`, unknown and cut conversions | `tests/c/fmt_test.c:48`, `:76`, `:95`, `:108`, `:129` |
| Events: auto and manual reset, lowest wins, NULL, 40 ms timeout | `test_events` (`tests/c/kernel32_test.c:81`) |
| A wait woken by another thread, with `INFINITE` and 10 s | `test_event_wakes_waiter` (`kernel32_test.c:127`, `:291`–`:292`) |
| File modes, lower-casing, short read, seeks, truncation, `ENOENT`, COM, dirty marks | `test_files` (`kernel32_test.c:147`) |
| Views: copy, empty file, short read | `test_mapping` (`kernel32_test.c:207`) |
| `DeleteFileA_wrap` | `test_delete` (`kernel32_test.c:240`) |
| Directory case, search patterns, dot names, directories | `test_directories` (`kernel32_test.c:249`) |
| Records from LF and CRLF defaults, path indices, tab-ended names | `test_defaults` (`tests/c/records_test.c:112`, `:199`–`:200`) |
| Missing defaults, clearing, long names, failed writes | `records_test.c:138`, `:147`, `:158`, `:178` |
| Timer start failure is fatal | `test_start_failure` (`tests/c/timer_test.c:39`) |
| Conf seeding and reuse, `CTL_CFG_*`, settings tree | `tests/c/wrapper_test.c:120`, `:136` |
| `fopen_wrap` and `fclose_wrap` dirty marks | `test_files` (`wrapper_test.c:148`) |
| Unreadable template: no conf, no redirects | `test_init_failed_copy` (`wrapper_test.c:167`) |
| Exit procedures: limit, order, once, nesting, window grace | `test_exit_procs` (`wrapper_test.c:199`) |
| `main` → `WrapperInit` → `WrapperCreateWindow` → box → exit 0 → conf in IDBFS | `tests/e2e/game.spec.mjs:41` (box `:44`–`:47`, exit `:49`, conf `:52`) |

**Gaps:**
- No C tests for `web.c` and `wsock32.c`.
- Untested: a watchdog that fires, thread creation and resumption, critical sections, timer scheduling, and the SDL, `time` and allocation shims.
- `game.spec.mjs` uses dummy files and stops at the missing movie.
- The browser never shows the missing-`/game` box (`web.c:119`) or the game-directory box (`wrapper.c:154`). The page always creates `/game` and passes both arguments (`main.js:132`, `:310`), and the importer requires files under `gamedata/` and `fedata/pc/` (`importer.js:300`, `:307`). Only `test_game_dirs` covers the directory check (`config_test.c:141`).

## Review and Refactor Guide

| Change | Inspect | Tests |
| ------ | ------- | ----- |
| Import | `platform.h:3` rules, the game's declaration | `make`, `game.spec.mjs` |
| User-data location | `settings_subdir` (`config.c:153`), subdirectories (`wrapper.c:113`), `SAVE_DIR` (`main.js:15`), lower-case import (`importer.js:288`) | `test_paths`, `wrapper_test`, `game.spec.mjs` |
| Import that writes files | call `web_mark_fs_dirty` (`web.c:89`) | `kernel32_test`, `wrapper_test`, `records_test` |
| `nfs2se.conf` key | `parse_line` (`config.c:100`), `Config` (`platform.h:63`), its consumer | `test_parse`, `test_defaults` |
| Thread, event or timer semantics | `CreateThread_wrap`, `WaitForMultipleObjects_wrap`, `timer_main` (`timer.c:30`) | `kernel32_test`, `timer_test`; scheduling has none |
| Exit | `wrapper_exit_procs`, `ExitProcess_wrap`, `onExit` (`main.js:416`) | `test_exit_procs`, `game.spec.mjs` |
| `vsprintf` | `parse_spec` (`fmt_x86.c:74`), `format_conversion` (`:138`) | `fmt_test` |

- **Coupling (observed):** platform files include only `platform.h` and `web_layout.h` from this repository. The only JS call is `Module.nfsMessageBox` (`web.c:76`).
- **Compatibility:** renaming the save subdirectories or the redirect prefixes orphans existing saves and records (`config.c:160`, `wrapper.c:113`).

## Open Gaps / Roadmap

- Verification with real game data ([Roadmap](../ARCHITECTURE.md#roadmap)).
- No network (`wsock32.c:1`) or serial link (`kernel32.c:336`).
- Ignored `nfs2se.conf` keys (`config.c:98`).
- **Kernel32 limits:** view writes never reach the file (`kernel32.c:416`), and the creation disposition is ignored (`:333`). Priority and termination do nothing (`:165`, `:182`). `GetLastError_wrap` maps only `ENOENT` (`:534`), and `GetCurrentDirectoryA_wrap` returns the buffer length (`:541`).
- More than 10 exit procedures are dropped silently (`wrapper.c:34`), and a hanging one ends the process with status 0 (`:52`).
- Whether 8 workers cover the game's threads is unverified (`Makefile:36`).
- The C test gaps listed in [How to Test](#how-to-test).

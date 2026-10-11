---
eatmycode_version: "1.2.0"
---
# Game files

## Goal

- **Owns:** getting the player's own copy of the game into the browser.
  - Reads a picked folder, a dropped tree, a ZIP archive or an ISO 9660 CD image (`src/web/importer.js:1`).
  - Keeps only the `gamedata/` and `fedata/` trees (`importer.js:286`).
  - Stores them in IndexedDB until the player removes them (`src/web/storage.js:1`).
- **Non-goals:**
  - Shipping or generating real game data ([Mission and Constraints](../ARCHITECTURE.md#mission-and-constraints)).
  - Writing the files into the wasm file system on Play, and the game's saves and settings: both belong to [page](page.md).
  - Checking file contents. The platform only checks that the directories exist (`src/platform/config.c:229`).
- **Kind:** player-facing feature, not infrastructure.

## Status

`done`.
- **Evidence (2026-10-11):** `node --test tests/unit/importer.test.mjs` passes 13 of 13 cases on generated ZIPs, cooked and raw ISO images and fake drop entries. `make test` exited 0 with all 28 Playwright tests passing, including the 6 import cases of `tests/e2e/launcher.spec.mjs` on generated folders and ZIPs; that spec alone printed `8 passed`.
- **Unverified:** real game installs, ZIPs made by other tools and real CD images (no game data exists here); import time and quota use; directory drops in a real browser.

## Code Structure

| File | Role |
| ---- | ---- |
| `src/web/importer.js` | Lists entries from folders, drops, ZIP archives and ISO 9660 images, within fixed limits. `selectGameFiles` picks and normalises the game trees. |
| `src/web/storage.js` | IndexedDB database `nfs2se`: store, describe, remove and load the imported files. |
| `tests/unit/importer.test.mjs` | Node tests of every importer export on generated archives, images and fake `FileSystemEntry` trees, including damaged and oversized input. |
| `tests/e2e/launcher.spec.mjs` | Browser tests of folder, picker-ZIP and dropped-ZIP imports, rejection, a failed import, a full disk, keeping files across reloads, and removal. The isolation and settings cases belong to [page](page.md). |

Owned elsewhere: `importFrom` and `populateFiles` in `src/web/main.js` ([page](page.md)); `buildZip` in `tests/support/zip.mjs` and `storedFiles` in `tests/e2e/helpers.mjs` ([build and test](build-and-test.md)).

## Language and Conventions

- **Language:** JavaScript ES modules, served unchanged ([Languages and Toolchain](../ARCHITECTURE.md#languages-and-toolchain)). The root [style rules](../ARCHITECTURE.md#coding-style-and-code-design) apply; only the tests enforce anything here.
- **Node:** `importer.js` uses only web APIs that Node 20 also has, so the unit tests import it directly (`tests/unit/importer.test.mjs:5`). `storage.js` needs `indexedDB`, so only browser tests cover it.
- **Entry contract (observed; every caller relies on it):** every reader returns `{path, size, blob(): Promise<Blob>}` (`src/web/importer.js:6`) and reads no file data until `blob()` is called. New readers must return the same shape.
- **Reads:** through `readBytes`, i.e. `Blob.slice` (`importer.js:36`); never a whole archive or image (`importer.js:4`). Fields are little-endian through `view` (`importer.js:40`); ISO both-endian fields use their little-endian half (`importer.js:261`).
- **Errors:** an `Error` whose message is for the player and names the file. Damage uses `damaged` (`<name>: damaged ZIP directory.`, `importer.js:28`) and the limits use `tooLarge` (`importer.js:32`). `importFrom` shows the message verbatim (`src/web/main.js:107`), and the tests match exact texts (`tests/unit/importer.test.mjs:175`).
- **IndexedDB helpers:** reuse `done` for requests and `committed` for transactions (`src/web/storage.js:11`, `storage.js:18`).

## Design and Invariants

### Readers (`src/web/importer.js`)

- **Folder picker:** `folderEntries` (`importer.js:52`) uses `webkitRelativePath`, falling back to the file name. The input is `webkitdirectory` (`src/web/index.html:29`).
- **Drops:** `droppedEntries` (`src/web/importer.js:58`) walks `FileSystemEntry` trees, calling `readEntries` until it returns an empty batch, because browsers return large folders in batches (`importer.js:69`). A single dropped file is opened as an archive (`importer.js:60`). The page must take the roots while the drop event is dispatched (`src/web/main.js:375`).
- **Archive dispatch:** `archiveEntries` (`src/web/importer.js:82`) tries a CD image first, then the ZIP local-header signature at offset 0, and otherwise throws `<name> is not a ZIP archive or a CD image (ISO/BIN).` (`importer.js:89`). Detection is by content; the picker merely suggests `.iso,.bin,.img,.mdf,.zip` (`src/web/index.html:30`).
- **ZIP** (`zipEntries`, `src/web/importer.js:93`):
  - **Directory:** the end record is searched for in the last 65,557 bytes (`importer.js:13`, `:94`). A saturated count, size or offset switches to ZIP64 through the locator and the ZIP64 end record (`importer.js:103`). Per entry, the ZIP64 extra field (id 1) supplies the saturated length, compressed size and local offset, in that order (`importer.js:131`).
  - **Names:** UTF-8 when flag bit 11 is set, otherwise windows-1252; backslashes become slashes (`importer.js:130`).
  - **Entries:** directories are skipped (`importer.js:152`), encrypted entries rejected (`importer.js:154`), and only methods 0 (stored) and 8 (deflate) accepted (`importer.js:156`).
  - **Data:** `zipData` (`importer.js:175`) re-reads the local header for its name and extra lengths, then inflates with `DecompressionStream('deflate-raw')` (`importer.js:196`). Sizes come from the central directory, so entries with a data descriptor (flag bit 3) need no special case; no test builds one. CRCs are not checked (`tests/support/zip.mjs:2`).
- **CD images** (`cdImage`, `src/web/importer.js:205`): the primary volume descriptor is looked for in sector 16, first with cooked 2048-byte sectors (`importer.js:207`), then with raw 2352-byte sectors and 2448-byte sectors with subchannel data (`importer.js:210`), recognised by the 12-byte sync pattern (`importer.js:212`). In mode 2 form 1, user data starts at byte 24 instead of 16 (`importer.js:213`). `sectorBlob` (`importer.js:221`) joins the 2048-byte user-data parts of raw sectors into one Blob.
- **ISO 9660 tree** (`isoEntries`, `importer.js:232`): walks from the root record of the primary volume descriptor (`importer.js:277`). Records never cross sectors, so a zero length byte skips to the next sector (`importer.js:254`). Version suffixes such as `;1` and a trailing `.` are removed (`importer.js:268`). A `visited` set stops directory loops (`importer.js:245`). Only the primary volume descriptor is read (`importer.js:241`), so Joliet and Rock Ridge names are ignored.

### Selection (`selectGameFiles`, `src/web/importer.js:286`)

- **Normalisation** (`importer.js:288`): backslashes become slashes, leading slashes are removed, and paths are lower-cased.
- **Path guard:** paths with an empty, `.` or `..` part are dropped (`importer.js:289`). Stored paths become MEMFS paths under `/game` (`src/web/main.js:136`), so this blocks path traversal.
- **Root choice:** a root is the path prefix before the first `gamedata` or `fedata` directory component (`src/web/importer.js:293`); a file of that name does not count. Only roots that have both qualify, and the shallowest wins, counted in path components (`importer.js:300`). Between equally deep roots the first in entry order wins; this is untested.
- **Kept files:** only files under `<root>gamedata/` and `<root>fedata/`, with the root removed (`importer.js:304`); at least one must be under `fedata/pc/` (`importer.js:307`). At most 20,000 files and 1 GiB in total are kept (`importer.js:309`).
- **Why lower case (required):** the platform lower-cases the Windows paths the game opens (`src/platform/config.c:184`), e.g. in `CreateFileA_wrap` (`src/platform/kernel32.c:341`), and at start-up requires the `gamedata` and `fedata/pc` directories (`src/platform/config.c:231`).

### Storage (`src/web/storage.js`)

- **Schema** (`storage.js:5`–`storage.js:9`): database `nfs2se` version 1; store `files` maps a path to a Blob; store `meta` maps key `'game'` to `{count, bytes, imported}`. `onupgradeneeded` creates the stores (`storage.js:27`). The names must not change ([Mission and Constraints](../ARCHITECTURE.md#mission-and-constraints)). There is no migration code: raising `DB_VERSION` needs an upgrade handler that keeps or rebuilds both stores.
- **Commit record (required): `meta` exists only for a complete import.**
  - `storeGameFiles` clears both stores (`storage.js:49`), puts each file in its own transaction (`storage.js:55`), and writes `meta` last (`storage.js:61`).
  - A failure after that first clear clears both stores again before the error is rethrown (`storage.js:65`), so a failed import leaves nothing behind. If that clean-up fails too, it only logs a warning and the import's own error is rethrown (`storage.js:66`); without `meta` the leftovers never count, and the next import clears them (`storage.js:49`).
  - `clearGameFiles` deletes `meta` and clears `files` in one transaction (`storage.js:40`).
  - If the tab closes mid-import, `gameFilesInfo` (`storage.js:35`) returns `null` and Play stays disabled (`src/web/main.js:69`). The next import clears the orphaned blobs (`storage.js:49`); Remove stays hidden without `meta` (`src/web/main.js:70`).
- **One transaction per file:** an IndexedDB transaction commits once it has no pending requests, so each blob (possibly a ZIP entry being inflated) is produced (`src/web/storage.js:54`) before its transaction opens.
- **Progress:** `storeGameFiles` calls `onProgress(index, count, path)` before each file (`storage.js:53`); `loadGameFiles` calls `onFile(path, bytes, index, count)` for each file (`storage.js:77`). The page shows both (`src/web/main.js:102`, `main.js:139`).
- **Loading:** `loadGameFiles` gets all keys and Blobs in one read-only transaction (`src/web/storage.js:73`), then reads the bytes one file at a time. The page writes each file under `/game` with `canOwn` (`src/web/main.js:138`), so each Play holds the selected trees in MEMFS ([Runtime and Data Flow](../ARCHITECTURE.md#runtime-and-data-flow) step 3).
- **Game writes never reach this database:** user data maps to the IDBFS settings directory (`src/platform/config.c:201`) and other writes under `/game` last only for the session, so removing the files keeps saves and settings (`src/web/main.js:344`).

### Errors, trust and resources

- **Trust boundary:** archives, images and dropped trees are untrusted ([System Design](../ARCHITECTURE.md#system-design)). Parsing is plain JavaScript, and every read goes through `Blob.slice`, which clamps to the file.
- **Limits (required)** (`src/web/importer.js:20`): an archive or image may list at most 20,000 entries (`MAX_FILES`); a ZIP directory, or all directories of an image together, at most 16 MiB (`MAX_DIRECTORY_BYTES`); image folders nest at most 32 deep (`MAX_DEPTH`). Exceeding one throws `<name> holds too many files and folders for a copy of the game.` (`importer.js:114`, `:249`, `:266`). Selection then caps the kept files (`importer.js:309`).
- **ZIP bounds:** the directory must end before the end records (`importer.js:116`); each record, its name, extra and comment fields, and each ZIP64 field must fit (`importer.js:121`, `:127`, `:138`); a stored entry's two sizes must match (`importer.js:158`); in file order, each entry's local header and data must end before the next entry or the directory, so no two entries share bytes (`importer.js:162`).
- **ZIP data:** `zipData` checks the local signature and that the data ends by that limit (`importer.js:177`, `:180`). Inflating stops as soon as the output passes the listed size, and any other final size or a failed stream is `damaged ZIP entry` (`importer.js:185`).
- **ISO bounds:** every directory and file extent must lie inside the image (`importer.js:233`, `:271`); each record must hold its 33 fixed bytes and its name and must fit its folder (`importer.js:259`). Damage throws `<name>: damaged CD image.`.
- **Quota and persistence:** `importFrom` requests persistent storage (`src/web/main.js:101`) and turns `QuotaExceededError` into a disk-space message (`main.js:105`). If `openStorage` fails, boot shows a message and stops (`main.js:424`).
- **Memory:** a ZIP directory is read whole, at most 16 MiB; image directories one at a time. Files are stored and loaded one at a time (`src/web/storage.js:52`, `storage.js:76`), but MEMFS then holds all kept files, up to the 1 GiB cap.

## Key Types and Entry Points

1. **Entry** `{path, size, blob()}` (`src/web/importer.js:6`): what every reader returns and `storeGameFiles` consumes. `blob()` can reject late, e.g. on a damaged ZIP entry.
2. **`folderEntries(files)`** (`importer.js:52`): turns a picker `FileList` into entries with relative paths. Synchronous; never fails.
3. **`droppedEntries(roots)`** (`importer.js:58`): turns drop roots into entries, opening a single file with `archiveEntries`. Rejects when the browser cannot read an entry.
4. **`archiveEntries(file)`** (`importer.js:82`): returns entries from a CD image or a ZIP, or throws the not-an-archive error.
5. **`zipEntries(file)`** (`importer.js:93`): returns the file entries; throws the missing, damaged and oversized directory errors and the encryption and method errors. An entry's `blob()` inflates its data and can reject with `damaged ZIP entry`.
6. **`cdImage(file)` and `isoEntries(file, image)`** (`importer.js:205`, `importer.js:232`): `cdImage` returns `{sectorBytes, dataOffset}`, or `null` for anything else, including short files; `isoEntries` returns every file in the image or throws the damaged or oversized image error.
7. **`selectGameFiles(entries)`** (`importer.js:286`): synchronous and reads no data. Returns the kept entries with lower-case paths relative to the root; throws the not-found, no-PC-subfolder or too-large error (`importer.js:302`, `:308`, `:310`).
8. **`storeGameFiles(db, files, onProgress)`** (`src/web/storage.js:48`): replaces the stored set and resolves to `{count, bytes, imported}`. Rejects with the import's own IndexedDB or read error, including `QuotaExceededError`, after removing everything it stored; a failed removal only warns.
9. **`openStorage()`, `gameFilesInfo(db)`, `clearGameFiles(db)`** (`storage.js:25`, `storage.js:35`, `storage.js:39`): open the database, return the `meta` record or `null`, and remove everything atomically.
10. **`loadGameFiles(db, onFile)`** (`storage.js:72`): calls `onFile` for every stored file in key order, with its bytes as a `Uint8Array`.

## Interactions

- **[Page](page.md) calls in.** `src/web/main.js` is the only production caller (`main.js:5`, `main.js:12`):
  - **Pickers:** `#pick-folder` calls `folderEntries`; `#pick-archive` calls `archiveEntries(files[0])` (`main.js:359`).
  - **Drops:** `droppedEntries` with the entries captured during the event, otherwise `archiveEntries` on the first file (`main.js:379`, `:381`).
  - **Import:** `importFrom` (`main.js:91`) adds the busy state, progress and error display.
  - **Status:** `refreshFiles` (`main.js:63`) uses `gameFilesInfo` for the status line and the Play button.
  - **Remove:** after a confirmation, the button calls `clearGameFiles` (`main.js:347`).
  - **Play:** `populateFiles` calls `loadGameFiles` to fill `/game` (`main.js:135`).
- **File layout contract with [platform](platform.md):** stored paths are lower-case `gamedata/…` and `fedata/…`, relative to `/game`. `config_convert_path` (`src/platform/config.c:201`) maps the game's paths onto them, and `config_check_game_dirs` (`config.c:229`) requires the directories. `install.win` and `text.*` come from `dist/files`, listed in `files/manifest.txt`, not from the import (`src/web/main.js:133`).
- **Schema readers:** `storedFiles` (`tests/e2e/helpers.mjs:57`) opens `nfs2se`/`files` directly and must change with the schema ([build and test](build-and-test.md)). The IDBFS saves database for `SAVE_DIR` (`src/web/main.js:15`) is separate and belongs to [page](page.md).
- **Integration tests:** the launcher cases in [How to Test](#how-to-test); `tests/e2e/game.spec.mjs:33` imports through `importGameFolder` before Play.

## How to Test

**Prerequisites:** Node 20 for the unit tests; the browser tests need the root's [prerequisites](../ARCHITECTURE.md#verification-and-review-map) and a built `dist/` (`make`).

Run from the repository root:

| Command | Expected evidence |
| ------- | ----------------- |
| `node --test tests/unit/importer.test.mjs` | `# tests 13`, `# pass 13`, `# fail 0` |
| `npx --no playwright test tests/e2e/launcher.spec.mjs` | `8 passed`; Playwright starts `node tools/serve.mjs dist 8123` or, outside CI, reuses one already running (`playwright.config.mjs:21`–`:27`) |
| `make test` | Exit 0; includes both |

| Behaviour or invariant | Test |
| ---------------------- | ---- |
| Stored and deflated entries, skipped directories, backslash names | `tests/unit/importer.test.mjs:142` |
| UTF-8 flag versus windows-1252 names | `importer.test.mjs:157` |
| ZIP64 directory and extra fields | `importer.test.mjs:165` |
| ZIP messages; directory and record bounds; shared, misplaced and mis-sized data; inflate cap; entry and directory limits | `importer.test.mjs:175` |
| CD detection: cooked, raw mode 1 and 2, subchannel; other files rejected | `importer.test.mjs:224` |
| ISO walk: padding, loop, version suffixes, raw images | `importer.test.mjs:235` |
| ISO bounds of extents and records; entry, folder-byte and depth limits | `importer.test.mjs:243` |
| `archiveEntries` dispatch and its error | `importer.test.mjs:267` |
| Folder-picker paths | `importer.test.mjs:275` |
| Drops read in batches; one dropped archive | `importer.test.mjs:306` |
| Shallowest root by depth, lower case | `importer.test.mjs:328` |
| Empty, `.` and `..` parts dropped | `importer.test.mjs:347` |
| Selection errors and the kept-file limits | `importer.test.mjs:355` |
| Rejected folder in the page | `tests/e2e/launcher.spec.mjs:19` |
| Stored set equals the selection; kept across reload; removal cancelled and confirmed | `launcher.spec.mjs:28` |
| ZIP through the picker and by drop | `launcher.spec.mjs:51`, `launcher.spec.mjs:94` |
| A ZIP entry that fails after another file was stored leaves no files and no `meta` | `launcher.spec.mjs:58` |
| A full disk is reported with the disk-space message even when the clean-up fails too, which only warns | `launcher.spec.mjs:70` |

**Coverage gaps:**
- **`storage.js`:** no unit tests, because Node has no IndexedDB. A tab closed mid-import is untested, and `QuotaExceededError` is tested only as a `put` that throws it (`tests/e2e/launcher.spec.mjs:79`).
- **Untested inputs:** ZIPs with data descriptors, ties between equally deep roots, and images whose data track does not start the file.
- **Directory drops:** a synthetic `DataTransfer` holds only files, so only fake entries cover them (`tests/unit/importer.test.mjs:306`).
- **Real data:** see [Status](#status).

## Review and Refactor Guide

| Change | Inspect | Tests |
| ------ | ------- | ----- |
| New archive or image format | Dispatch in `src/web/importer.js:82`; keep the entry shape and apply the limits | Beside `tests/unit/importer.test.mjs:224` and `importer.test.mjs:267`, on generated dummy bytes |
| ZIP features (methods, CRCs) | `zipEntries`, `zipData` (`src/web/importer.js:93`, `importer.js:175`) | `tests/unit/importer.test.mjs:142` to `importer.test.mjs:175`; extend `buildZip` (`tests/support/zip.mjs:19`) |
| Limits | `src/web/importer.js:20`; the game's real size | `tests/unit/importer.test.mjs:175`, `importer.test.mjs:243`, `importer.test.mjs:355` |
| Selection rules | `selectGameFiles` (`src/web/importer.js:286`); platform mapping (`src/platform/config.c:201`, `config.c:229`) | `tests/unit/importer.test.mjs:328` to `importer.test.mjs:355`, `tests/e2e/launcher.spec.mjs:19`, `tests/e2e/game.spec.mjs:41` |
| Storage schema or commit order | `src/web/storage.js:5`, `storage.js:27`, `storage.js:48`, `tests/e2e/helpers.mjs:57` | `tests/e2e/launcher.spec.mjs:28`, `launcher.spec.mjs:58`; existing imports must survive, or a re-import must be intended |
| Messages or progress | `importFrom` (`src/web/main.js:91`) and the messages in `src/web/importer.js` | Exact texts in `tests/unit/importer.test.mjs:175` and `tests/e2e/launcher.spec.mjs:22`, `launcher.spec.mjs:64`, `launcher.spec.mjs:89` |

- **Keep:** readers lazy; every offset and length from a file checked before use; `meta` written last and deleted first; a failed import cleared; `selectGameFiles` as the only gate to `/game`; the database, store and key names.
- **Forbidden:** real game data as fixtures ([Project-Specific Deviations](../ARCHITECTURE.md#project-specific-deviations)).

**Proposals (not accepted):** none. The gaps below have no evidence-backed fix planned.

## Open Gaps / Roadmap

- **Real data:** verify folder, ZIP and CD-image imports with player-supplied data ([Roadmap](../ARCHITECTURE.md#roadmap), accepted work).
- **Data track position:** detection assumes sector 16 of the file is in the data track (`src/web/importer.js:207`, `importer.js:211`). Images with a leading audio track, or a layout described by a CUE or MDS file, are not handled.
- **Joliet and Rock Ridge:** their names are ignored (`importer.js:241`).
- **No partial update:** a changed game copy needs a full re-import, because `storeGameFiles` clears everything first (`src/web/storage.js:49`).
- **Untested behaviour:** see the coverage gaps in [How to Test](#how-to-test).

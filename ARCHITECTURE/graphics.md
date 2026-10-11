---
eatmycode_version: "1.2.0"
---
# Graphics

## Goal

This module owns the picture:
- `src/platform/glide.c` records the game's Glide 2.x calls into the two graphics slots on the game thread (`glide.c:1`).
- `src/web/renderer.js` replays them with WebGL2 into an off-screen scene on the main thread (`renderer.js:1`).
- `src/web/postfx.js` draws the scene onto the canvas with the picture improvements (`postfx.js:1`). `src/web/glutil.js` holds shared helpers (`glutil.js:1`).

**Non-goals:** bit-exact 3dfx emulation (deliberate differences at `glide.c:12`); the slot layout ([shared memory](shared-memory.md)); the settings form and frame loop ([page](page.md)). This is not infrastructure.

## Status

`in progress`: LFB (linear frame buffer) writes are dropped, so movies are invisible (`glide.c:638`), and several Glide calls have no effect. The rest is implemented and has the tests listed in How to Test.

- **Evidence (2026-10-11):** `make test` exited 0, with `glide` at `306 checks, 0 failed`. Run on their own, `renderer.test.mjs` printed `# pass 4` and `layout.test.mjs` `# pass 3`, both with `# fail 0`; `renderer.spec.mjs` printed `11 passed` and `game.spec.mjs` `4 passed`.
- **Unverified:** real game data (none on this machine), frame rate, hardware GPUs, the MSAA path, and browsers other than Chromium with SwiftShader (`playwright.config.mjs:18`).

## Code Structure

| File | Role |
| ---- | ---- |
| `src/platform/glide.c` | Glide imports: state, texture memory, palette, fog, slot recorder |
| `src/web/renderer.js` | `Renderer` (pump, op interpreter, scene, shaders, context loss); `fitViewport`, `clearDepth`, `slotViews` |
| `src/web/postfx.js` | `PostFx`: bloom, edge antialiasing or sharpening, gamma, colour enhancement |
| `src/web/glutil.js` | `createProgram`, `uniformLocations`, `FULLSCREEN_VS`, `createTarget`, `deleteTarget` |
| `tests/c/glide_test.c` | Recorder tests with stubbed slots and futex (`tests/c/glide_test.c:1`) |
| `tests/unit/renderer.test.mjs` | Pure renderer helpers |
| `tests/e2e/renderer.spec.mjs` | Hand-built slots replayed in Chromium and checked by pixel probes |

## Language and Conventions

- **Languages:** C, `-std=gnu11` with warnings as errors (`Makefile:33`); JavaScript ES modules with GLSL ES 3.00 strings (`renderer.js:21`). Root rules: [toolchain](../ARCHITECTURE.md#languages-and-toolchain), [style](../ARCHITECTURE.md#coding-style-and-code-design).
- **Canonical helpers (observed):** `put_command` for ops (`glide.c:175`), `sync_word` and `sync_words` for state (`:186`, `:194`), `put_teximage` for images (`:202`), `createProgram` for shaders (`glutil.js:3`), `FULLSCREEN_VS` for full-screen passes (`:31`), `createTarget` for colour targets (`:39`), `rgb` for Glide 0xRRGGBB colours (`renderer.js:101`).
- **Wire constants come from the layout mirrors** (equal values enforced by `layout.test.mjs:30`): the game size (`web_layout.h:20`–`:21`), vertex offsets (`:143`–`:145`), the op header shift and mask (`:181`–`:182`), fog entries and the largest texture (`:183`–`:184`) and the `GFX_DEFAULT_*` state (`:201`–`:206`), mirrored in `layout.js:9`–`:10`, `:86`–`:88`, `:113`–`:116` and `:124`–`:129`. The scene shaders interpolate them (`renderer.js:31`, `:56`).
- **Initial state:** `DEFAULT_STATE` (`glide.c:91`) and `defaultState` (`renderer.js:106`) both start from `GFX_DEFAULT_*`. The remaining defaults (full-screen clip, texturing and fog off, fog colour and table 0, no texture, gamma 1.0) agree only by the comment at `web_layout.h:196`–`:198`.
- The header comment lists what follows NFSIISE and what differs (`glide.c:7`–`:16`). Keep it current.

## Design and Invariants

### Recording (glide.c)

- **State diff.** `sync_state` (`glide.c:263`) emits the differences between `s_state` and `s_emitted` (`:111`) right before a clear or draw, so redundant calls are free (`:4`–`:5`). Clears sync only the clip and depth mask (`:265`–`:268`). Draws sync binding and sampler only while texturing (`:272`–`:282`), fog colour and table only while fogging (`:284`–`:292`).
- **Slots** (the writer side of [shared memory](shared-memory.md)). `open_slot` waits for `GFX_FREE` in 100 ms futex waits (`glide.c:26`, `:144`–`:145`), then sets `GFX_WRITING` (`:146`). `publish` writes the fill levels, then the state, and switches slots (`:158`–`:161`). `reserve` publishes a full slot as `GFX_PARTIAL` (`:169`–`:170`). `grBufferSwap` publishes `GFX_FRAME` and opens the other slot, waiting while the renderer holds it, so the game runs at most two frames ahead (`:431`–`:433`).
- **Commands and vertices.** The header word is `op | words << GFX_OP_SIZE_SHIFT` (`glide.c:180`); the shift is 8, so sizes have 24 bits. Static asserts keep the largest command, a 256×256 texture image, within an empty slot and every command size within the header's size field (`:100`–`:101`). Any command ends the triangle run (`:182`); consecutive draws extend one `GFX_OP_TRIS` (`:300`–`:307`). A 24-byte vertex (`web_layout.h:142`–`:145`) holds x and y minus the 786432 snap bias (`glide.c:24`, `:329`), oow, sow/256, tow/256 (`:330`) and RGBA bytes, rounded and clamped with NaN as 0 (`:318`–`:324`). Lines are two triangles one game unit wide (`:475`–`:489`).
- **Textures.** 2 MiB of addresses hold one record per address / 4 (`glide.c:23`, `:113`). `grTexDownloadMipMap` (`:669`) keeps only the largest level, as a square `GFX_TEX_MAX_SIZE >> largeLod` wide (`:213`, `:675`), and ignores addresses from 2 MiB and levels past 1×1 (`:673`–`:674`). RGB565, ARGB1555 and ARGB4444 go out at once as `GFX_OP_TEXIMAGE` (`:690`–`:699`); P8 indices wait in `s_textureMem` (`:684`–`:689`). Other formats only invalidate the record (`:700`–`:702`), so the old image or black is sampled.
- **Texels** are premultiplied with rounding into RGBA8 (`glide.c:216`–`:224`): 565 by bit replication (`:230`), 1555 without its alpha bit transparent (`:235`–`:236`), 4444 times 17 (`:243`).
- **Palette.** `grTexDownloadTable` skips non-palette and unchanged tables (`glide.c:710`), premultiplies and bumps a nonzero generation (`:713`–`:716`). At a draw, a bound P8 texture of another generation is expanded by `upload_p8` (`:277`–`:278`); a download resets it (`:679`). So P8 expands once per palette or texel change, with the palette current at draw time.
- **Fog.** `fog_index_w` places entry i at w = 8 / (8 − (i mod 4)) · 2^⌊i/4⌋ (`glide.c:345`), so entry 4k is at w = 2^k. `build_fog_lut` (`:350`) expands the 64 entries to 65,536 bytes by integer w, linear in 1/w between entries (`:363`–`:366`) and constant past the last (`:358`–`:361`), only on change (`:599`–`:603`). `guFogGenerateExp` (`:607`) uses f(w) = 1 − e^(−density·w): entry i = ⌊255 · f(wᵢ) / f(w₆₃)⌋, clamped; a density ≤ 0 gives zeros (`:609`–`:614`).
- **Other state.** `grSstWinOpen` resets the clip to 640×480 (`glide.c:403`–`:404`). The depth function keeps 3 bits (`:566`), the mask 0 or 1 (`:572`). Blending keeps colour factors only: sources other than ZERO and ONE become SRC_ALPHA, destinations other than ONE and ONE_MINUS_SRC_ALPHA become ZERO (`:497`–`:498`). `grAlphaCombine` only enables texturing, when "other" is the texture (`:508`). Clamping is per axis (`:732`–`:733`); bilinear follows the magnification filter only (`:741`). Gamma goes out on change (`:621`–`:624`).
- **No effect** (return 0): `grColorCombine`, the alpha-test and chromakey calls, `grCullMode`, `grDepthBiasLevel`, `grDepthBufferMode` (`glide.c:512`–`:562`), `grDitherMode` (`:576`), `grTexMipMapMode`, `grTexCombine`, `grTexCombineFunction` (`:745`–`:770`). Fog modes other than off and table, such as iterated fog, change nothing (`:584`–`:587`). A fixed discard of alpha ≤ 16/255 replaces the alpha test (`renderer.js:53`), as the header states (`glide.c:10`).
- **LFB.** `grLfbLock` (`glide.c:628`) fails unless write-only (`:635`–`:636`). Writers get a 600 KiB scratch buffer with a 2-byte stride that nothing reads (`:29`, `:124`, `:637`–`:638`), so movies are invisible.
- **Threading (unenforced):** the recorder statics have no lock, so one game thread is assumed, unverified (`glide.c:126`–`:132`).

### Replay (renderer.js)

- **Context:** WebGL2 without default alpha, depth, stencil or antialiasing (`renderer.js:133`–`:136`).
- **Pump** (`:171`) runs `GFX_PARTIAL` and `GFX_FRAME` slots in order, frees each with `Atomics.store` and `Atomics.notify` (`:184`–`:185`) and stops after a frame (`:187`–`:190`). Otherwise it redraws if invalidated (`:192`–`:193`). It returns whether a game frame was presented; a redraw does not count (`:194`).
- **Execute** (`:352`) uploads the slot's vertices (`:358`–`:361`) and interprets commands. A zero-size or overrunning command ends the slot (`:368`–`:369`); unknown ops are skipped (`:371`). State ops set dirty bits that `flush` (`:474`) applies before clears and draws. `draw` ignores out-of-range runs (`:528`–`:529`).
- **Scene**, chosen once per game frame (`:286`–`:290`). `sceneConfig` (`:270`): 640×480 for `original` (`:272`–`:273`); otherwise the picture's area on the canvas, 4:3 when letterboxed (`:271`), scaled to at least 640×480 (`:275`) or by 0.5 for `half`, which may fall below 640×480 (`:276`–`:277`), capped at 3840 wide and the GL size limits (`:64`, `:262`, `:278`). 4× MSAA with `antialias` when RGBA8 and DEPTH_COMPONENT32F renderbuffers allow 4 samples (`:260`–`:261`, `:282`). An incomplete MSAA target falls back to single-sample (`:301`); an incomplete single-sample one throws (`:327`–`:328`).
- **Depth** is 1/w (`:61`) in DEPTH_COMPONENT32F (`:312`, `:320`). Nearer is larger, so the functions are mirrored (`:258`) and new scenes clear to 0 (`:334`). `clearDepth` converts Glide's 4.12 W to 1/w, with 0xFFFF as 0 (`:86`–`:89`). Clears honour the scissor and depth mask (`:524`).
- **Shading.** Clip w is 1 (`:31`), so dividing s and t by oow per pixel is perspective-correct (`:50`). The texel, its premultiplied alpha divided out, modulates the vertex colour (`:49`–`:51`); alpha ≤ 16/255 is always discarded (`:53`–`:54`). Fog reads the 256×256 R8 LUT at w = 1/oow, clamped to 65535, and mixes in the fog colour (`:56`–`:58`, `:247`–`:251`).
- **Textures** keep an RGBA8 copy per id (`:124`, `:441`), accept power-of-two sizes up to 256 (`:438`) and get full mip chains (`:457`, `:462`). Unknown ids sample opaque black (`:245`, `:508`). The eight **samplers** are indexed by bilinear | clamp s << 1 | clamp t << 2 (`:104`); bilinear ones are trilinear with up to 16× anisotropy if available (`:227`–`:238`). Bilinear needs the game's config word and `smoothTextures` (`:292`, `:510`).
- **Blending** is off for ONE/ZERO, else the mapped factors (`:495`–`:501`). The **scissor** scales the clip to the scene with y flipped (`:489`–`:493`).
- **Present** (`:535`) resolves MSAA (`:537`–`:542`), applies a pending canvas size (`:543`–`:546`) and runs `PostFx` into the `fitViewport` rectangle (`:547`–`:548`). 4:3 needs both the `KeepAspectRatio` word and `keepAspect` (`:152`–`:153`).
- **Context loss.** The lost handler calls `preventDefault`, marks the renderer lost and cancels a pending rebuild (`:139`–`:143`); the restored handler only flags a rebuild (`:144`). While lost, `pump` still frees slots, presents nothing and returns false (`:184`–`:188`, `:192`); `execute` keeps state, texture images and the fog table but skips clears, draws and presents (`:355`, `:397`–`:424`, `:447`). The next `pump` after a restore runs `initGL` (`:172`–`:176`), which re-uploads the fog table and textures (`:252`–`:256`) and drops the scene (`:264`). If the rebuild throws, `pump` throws (`:168`–`:170`).
- **Resources and performance (unmeasured).** Texture copies stay in JS memory; a GL texture is freed only on a size change (`:442`–`:445`). State is filtered in C and again by dirty bits (`:67`–`:75`). Each slot is one buffer upload (`:360`), each triangle run one draw (`:531`). `discard` and `gl_FragDepth` disable early depth tests (`:54`, `:61`).

### Post-processing (postfx.js)

- **Bloom** (`postfx.js:147`–`:164`): a quarter-size bright pass averages 4×4 pixels (`:13`, `:100`, `:119`) and keeps colour above a 0.6 peak (`:19`). Two 9-tap Gaussians take 5 fetches each (`:30`–`:32`). `BLOOM_STRENGTH` adds the blur at 0.3 (`:101`, `:182`).
- **Final pass** (`:92`–`:97`): the FXAA-like edge filter if `antialias`, else contrast-adaptive sharpening if `sharpen` (`:93`). Then bloom (`:94`–`:95`), gamma as c^(1/gamma) with gamma ≤ 0 taken as 1 (`:96`, `:181`), and enhancement: 1.15 saturation with 25 % of a smoothstep curve (`:87`–`:89`).
- **Options are switches:** gamma plus the booleans `bloom`, `sharpen`, `grade` (colour enhancement) and `antialias` (`:135`–`:137`) become fixed uniforms (`:182`–`:185`); no strength is configurable.
- The renderer asks for the edge filter only without MSAA (`renderer.js:553`); with MSAA, sharpening, if on, takes its place (`postfx.js:93`). Letterbox bars are cleared black (`:166`–`:169`).
- `createProgram` checks the link status after linking, for parallel compilation, and throws unless the context is lost (`glutil.js:13`–`:16`). `createTarget(gl, width, height)` makes a linear, edge-clamped RGBA8 target (`:39`–`:50`) for the scene resolve and both bloom targets (`renderer.js:307`, `postfx.js:124`).

### Settings consumed

[Page](page.md) owns the form and the defaults (`settings.js:7`): `resolution` is one of three names (`:5`), the six renderer switches are booleans (`:9`–`:14`). The renderer reads the live object every frame (`renderer.js:115`–`:116`):
- `resolution` picks the scene size (`:273`).
- `antialias` asks for 4× MSAA (`:282`), otherwise for the edge filter (`:553`).
- `bloom`, `sharpen` and `enhancedColors` switch the post-processing (`:550`–`:552`).
- `smoothTextures` allows bilinear only together with the `LinearTextureFiltering` word (`:292`); `keepAspect` letterboxes only together with the `KeepAspectRatio` word (`:153`).

### Errors and recovery

- Rejected downloads return 0 like accepted ones (`glide.c:673`–`:674`, `:704`); the game never sees an error.
- Without WebGL2, `available` is false (`renderer.js:137`–`:138`, `:148`–`:149`) and the page refuses to start (`main.js:289`–`:290`).
- GL failures throw: `createScene` (`renderer.js:327`–`:328`), `createProgram` (`glutil.js:14`–`:16`), and so a failed rebuild after a restore (`renderer.js:172`–`:176`). They escape `pump`.
- The frame loop catches them, logs them and ends the session with "The game display failed: …" (`main.js:254`–`:260`). The game thread then waits in `open_slot` (`glide.c:144`–`:145`), as nothing frees the slots any more.
- A failed rebuild leaves the renderer half-built and still lost (`renderer.js:173`–`:175`); the page never pumps it again.

## Key Types and Entry Points

1. **`open_slot`, `publish`, `reserve`, `grBufferSwap`** (`glide.c:140`, `:155`, `:167`, `:428`): the slot writer. In: words and vertices to reserve, a state to publish. Out: room in the open slot; `grBufferSwap` returns 0. Effects: futex waits, fill counts, state, slot switch; `grBufferSwap` appends `GFX_OP_SWAP` and publishes a frame. Failure: blocks while the renderer holds the slot.
2. **`sync_state`** (`glide.c:263`): In: whether a draw (else a clear) follows. Effects: emits the state that differs from `s_emitted`; may expand a P8 texture. Failure: none.
3. **`put_vertices`, `put_vertex`** (`glide.c:296`, `:327`): In: a vertex count; one Glide vertex and a line offset. Out: room for the run; a packed 24-byte vertex. Effects: syncs state, extends or starts a `GFX_OP_TRIS`. Failure: none; a full slot is published as partial.
4. **`grTexDownloadMipMap`, `grTexDownloadTable`** (`glide.c:669`, `:707`): In: a start address and `GrTexInfo`, or a table type and palette. Out: 0. Effects: `GFX_OP_TEXIMAGE` now, or at a draw for P8; a new palette generation. Failure: an invalid record and nothing sent; other tables ignored.
5. **`grFogTable`, `guFogGenerateExp`** (`glide.c:597`, `:607`): In: a 64-byte table; a density and a table to fill. Out: 0. Effects: a new LUT, sent at the next fogged draw; the filled table. Failure: none.
6. **`Renderer`, `pump`** (`renderer.js:117`, `:171`): In: canvas, slot views, control block, live settings. Out: `available` (`:148`); `pump` returns whether a frame was presented. Effects: GL work, slot release, the rebuild after a restore. Failure: throws on GL failure, including a failed rebuild; while lost it frees slots and returns false.
7. **`execute`, `flush`** (`renderer.js:352`, `:474`): In: one slot. Effects: interprets the ops; applies dirty state before clears and draws. Failure: stops at a malformed command; ignores unknown ops, bad texture sizes and out-of-range runs.
8. **`beginFrame`, `sceneConfig`, `createScene`** (`renderer.js:288`, `:270`, `:305`): In: settings, canvas size, `CTL_CFG_*` words. Out: `{ width, height, samples }` and the scene target. Effects: replaces the target between frames only. Failure: an incomplete MSAA target falls back; single-sample throws.
9. **`PostFx.render`** (`postfx.js:138`): In: scene, viewport, canvas size, gamma and four switches (`:135`–`:137`). Out: the canvas picture. Effects: sizes bloom targets lazily (`:118`), clears the letterbox. Failure: construction throws via `createProgram`.
10. **`createProgram`, `createTarget`** (`glutil.js:3`, `:39`): In: a context and shader sources; a size. Out: a linked program; `{ texture, framebuffer, width, height }` (`:50`). Effects: GL objects; shaders are deleted after linking. Failure: `createProgram` throws with the logs unless the context is lost (`:14`–`:16`); `createTarget` does not check completeness.

## Interactions

| Boundary | Direction | Contract | Tests |
| -------- | --------- | -------- | ----- |
| Glide imports | game → `glide.c` | Glide names returning `int32_t`; struct sizes asserted (`glide.c:54`, `:63`) | `glide_test.c:550` |
| Graphics slots | `glide.c` → `renderer.js` | [Shared memory](shared-memory.md) layout, states, ops, defaults (`web_layout.h:130`–`:207`) | `glide_test.c`, `renderer.spec.mjs`, `layout.test.mjs:30` |
| Slot memory, futex | `glide.c` → `web.c` | `web_gfx_slot` (`web.c:53`), `web_futex_wait` (`web.c:58`); see [platform](platform.md) | stubs at `glide_test.c:92`, `:154` |
| Config words | `wrapper.c` → `renderer.js` | `CTL_CFG_KEEP_ASPECT`, `CTL_CFG_LINEAR_FILTER` (`wrapper.c:145`–`:146`) from `KeepAspectRatio`, `LinearTextureFiltering` (`config.c:38`–`:39`), both true by default (`:20`–`:21`) | `renderer.spec.mjs:240`, `:300` |
| Page | `main.js` → `Renderer` | Construction (`main.js:288`), no-WebGL2 error (`:289`–`:290`), `pump` per animation frame, a throw ending the session (`:254`–`:260`), presented frames counted for the FPS display over 500 ms (`:19`, `:255`–`:256`, `:262`–`:263`), `resize` in device pixels (`:237`), `invalidate` on setting changes (`:327`) | `game.spec.mjs:41`, `:90` |
| Pointer | `input.js` → `renderer.js` | `fitViewport` and `keepAspect` (`input.js:12`, `:180`; `main.js:302`); see [input](input.md) | `input.test.mjs:299` |

## How to Test

Prerequisites: as in the root's [Verification and Review Map](../ARCHITECTURE.md#verification-and-review-map).

| Command | Expected evidence |
| ------- | ----------------- |
| `make build/tests/glide.cjs && node build/tests/glide.cjs` | `306 checks, 0 failed` |
| `make test-c` | `build/tests/glide.cjs`, then `306 checks, 0 failed`, among the other C tests |
| `node --test tests/unit/renderer.test.mjs` | `# pass 4`, `# fail 0` |
| `node --test tests/unit/layout.test.mjs` | `# pass 3`, `# fail 0` |
| `make && npx --no playwright test tests/e2e/renderer.spec.mjs` | `11 passed`; the config starts or reuses the server (`playwright.config.mjs:23`) |
| `make && npx --no playwright test tests/e2e/game.spec.mjs` | `4 passed` |
| `make test` | Exit 0 before finishing a change |

| Behaviour or invariant | Case |
| ---------------------- | ---- |
| Vertex packing, shared run | `test_triangles` (`glide_test.c:218`) |
| Lines | `test_lines` (`:246`) |
| Lazy state, blend mapping, gamma | `test_state` (`:265`) |
| 16-bit formats, rejections, lazy binding | `test_textures` (`:322`) |
| P8 expansion | `test_palette_textures` (`:379`) |
| Fog LUT, deferred table, iterated fog ignored | `test_fog` (`:426`) |
| Exponential fog table | `test_fog_generate` (`:479`) |
| Partial slots for vertices and for the largest texture images | `test_overflow` (`:493`, `:511`) |
| Back-pressure, LFB | `test_back_pressure`, `test_lfb` (`:526`, `:540`) |
| Largest command fits a slot; sizes fit the header | compile-time asserts (`glide.c:100`–`:101`) |
| Mirrored constants | `layout.test.mjs:30` |
| `fitViewport`, `clearDepth`, `slotViews` | `renderer.test.mjs:8`, `:16`, `:20`, `:31` |
| Clip and clears | `renderer.spec.mjs:158` |
| Depth as 1/w, functions, mask | `:177` |
| Blend factors, alpha discard | `:201` |
| Perspective-correct texturing, modulation, cut-out | `:217` |
| Filtering, wrapping, setting, config word | `:240` |
| Fog at w = 1/oow | `:258` |
| Two-slot frame presented once, idle pump not presented, gamma | `:279` |
| 4:3 placement, resizes | `:300` |
| Improvements keep flat grey; `native` and `half` scene sizes | `:319` |
| Textures and fog table survive context loss; lost pumps free slots and present nothing | `:334` |
| A failed rebuild makes the next pump throw | `:360` |
| The page ends the session on a display failure | `game.spec.mjs:90` |

**Coverage gaps:** no real data, so no real picture or frame rate is checked. SwiftShader only; the MSAA path may never run (`renderer.spec.mjs:328` accepts 0 samples), so sharpening in place of the edge filter is unchecked. Improvements are checked only on flat grey (`:319`). Untested: lines on screen, anisotropy, mipmapped minification, an incomplete scene framebuffer (`renderer.js:324`–`:328`) and the frame-rate value (`main.js:262`–`:263`).

## Review and Refactor Guide

| Likely change | Touch | Tests |
| ------------- | ----- | ----- |
| New Glide state | `State`, `DEFAULT_STATE` (`glide.c:80`, `:91`), setter, `sync_state`; an op in both layout mirrors ([shared memory](shared-memory.md)); `execute` (`renderer.js:371`), `defaultState` (`:106`), a dirty bit (`:67`) | `glide_test.c:265`, a new e2e case, `layout.test.mjs:30` |
| New texture format | A converter beside `texel_565` (`glide.c:227`), a case at `:690` | `glide_test.c:322` |
| Scene or MSAA policy | `sceneConfig` (`renderer.js:270`) | `renderer.spec.mjs:319` |
| New effect | `FINAL_FS` or a pass (`postfx.js:36`, `:138`), options in `present` (`renderer.js:548`), a setting ([page](page.md)) | `renderer.spec.mjs:319` |

- **Reusable patterns:** the canonical helpers above; dirty bits in `flush`.
- **Forbidden coupling:** the game thread never calls WebGL or JS. The renderer reads wasm memory only through the slots and the `CTL_CFG_*` words (`renderer.js:153`, `:292`). Each op replaces one piece of state (`web_layout.h:160`–`:162`), so never infer state from command order. `PostFx` knows nothing of Glide (`postfx.js:135`–`:137`).
- **Compatibility checks:** the vertex offsets are named in both mirrors (`web_layout.h:142`–`:145`), but `put_vertex` (`glide.c:331`–`:338`) and the attributes (`renderer.js:213`–`:219`) must keep float x, y, oow, s, t and RGBA8. The fog LUT has `GFX_FOG_ENTRIES` bytes (`web_layout.h:183`, `glide.c:120`), stored as a texture `FOG_TEXTURE_WIDTH` wide (`renderer.js:17`, `:249`). The depth mirrors (`:258`) follow Glide's function order (`web_layout.h:178`). Texture ids are start addresses, shared with the P8 store (`glide.c:113`–`:114`, `:725`).

**Proposals (not accepted):**
- **Movies.** Record LFB writes and draw them as a texture over the slot protocol: a real stride (`glide.c:638`) and an op at unlock (`:642`). Benefit: movies appear. Check: `glide_test.c` and a pixel e2e case cover it; real data shows the intro.
- **Measure performance.** Record the page's frame rate (`main.js:262`–`:263`) with real data on a hardware GPU, with and without the improvements and `gl_FragDepth` (`renderer.js:61`). Benefit and check: recorded frame-rate numbers.

## Open Gaps / Roadmap

- **Movies:** LFB writes are dropped (`glide.c:628`–`:640`); the upload is a proposal in the root [roadmap](../ARCHITECTURE.md#roadmap).
- **Glide features:** the calls without effect; alpha blend factors and lossy colour factors (`glide.c:497`–`:498`); the minification filter (`:740`); iterated fog (`:584`–`:587`); square textures up to 256×256 with only the largest mip level (`:652`, `:673`–`:675`); formats beyond P8 and the three 16-bit ones (`:700`–`:702`). Which the game needs is unknown without real data.
- **Unmeasured:** performance and peak texture memory. Real-data behaviour is unverified.

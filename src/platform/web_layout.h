/* Layout of the memory regions shared between the wasm game threads and the
 * browser main thread / AudioWorklet. Mirrored by src/web/layout.js; the unit
 * test tests/unit/layout.test.mjs fails when the two drift apart. */
#ifndef NFS2SE_WEB_LAYOUT_H
#define NFS2SE_WEB_LAYOUT_H

/* web_shared(which) region ids */
enum
{
	SHARED_CTL = 0,
	SHARED_GFX0 = 1,
	SHARED_GFX1 = 2,
	SHARED_AUDIO = 3,
};

/* The game's screen in game units, the unit of cursor positions, the clip
 * window and vertex positions. */
enum
{
	GAME_WIDTH = 640,
	GAME_HEIGHT = 480,
};

/* Control block: int32 words. JS writes input, C consumes it. */
enum
{
	CTL_SEQ = 0,            /* futex word, bumped on every queued message */
	CTL_RING_HEAD = 1,      /* ring write index, owned by JS */
	CTL_RING_TAIL = 2,      /* ring read index, owned by C */
	CTL_MOUSE_X = 3,        /* cursor in game units (0..GAME_WIDTH - 1) */
	CTL_MOUSE_Y = 4,        /* cursor in game units (0..GAME_HEIGHT - 1) */
	CTL_MOUSE_BUTTONS = 5,  /* MOUSE_* bits */
	CTL_MOUSE_MOVES = 6,    /* bumped on every mouse move */
	CTL_DIALOG = 7,         /* futex word: 0 while a message box is open, then its DIALOG_* answer */
	CTL_FS_DIRTY = 8,       /* C sets 1 after writing files, JS clears it after persisting */
	CTL_CFG_KEEP_ASPECT = 9,
	CTL_CFG_LINEAR_FILTER = 10,

	CTL_PAD_BASE = 16,
	CTL_PAD_STRIDE = 16,
	CTL_PAD_COUNT = 2,

	CTL_RING_BASE = 64,
	CTL_RING_ENTRIES = 256, /* power of two */
	CTL_RING_ENTRY_WORDS = 4,

	CTL_WORDS = CTL_RING_BASE + CTL_RING_ENTRIES * CTL_RING_ENTRY_WORDS,
};

/* Per-gamepad words, relative to CTL_PAD_BASE + pad * CTL_PAD_STRIDE */
enum
{
	PAD_CONNECTED = 0,
	PAD_BUTTONS = 1,        /* bitmask, SDL joystick button order */
	PAD_HAT = 2,            /* HAT_* bits */
	PAD_NUM_HATS = 3,
	PAD_NUM_BUTTONS = 4,
	PAD_NUM_AXES = 5,
	PAD_AXES = 6,           /* PAD_MAX_AXES words, SDL range -32768..32767 */
	PAD_MAX_AXES = 6,
};

/* CTL_DIALOG answers, the Win32 IDOK and IDCANCEL values MessageBoxA returns */
enum
{
	DIALOG_OK = 1,
	DIALOG_CANCEL = 2,
};

enum
{
	MOUSE_LEFT = 1,         /* left button is down */
	MOUSE_LEFT_LATCH = 2,   /* set by JS on press, cleared by C when it reads the button */
};

enum
{
	HAT_UP = 1,
	HAT_RIGHT = 2,
	HAT_DOWN = 4,
	HAT_LEFT = 8,
};

/* Ring entry: Windows message delivered to GetMessageA. */
enum
{
	RING_MSG = 0,
	RING_WPARAM = 1,
	RING_LPARAM = 2,
};

/* Key messages from the keyboard (input.js) and from joystick buttons
 * (dinput.c via user32.c). wParam is the virtual key, lParam is KEY_REPEAT
 * plus the scancode shifted by KEY_SCANCODE_SHIFT. A press of a key that types
 * a character is followed by WM_CHAR with that character; of the control keys,
 * VK_BACK, VK_TAB, VK_RETURN and VK_ESCAPE type their own virtual key. */
enum
{
	WM_KEYDOWN = 0x100,
	WM_KEYUP = 0x101,
	WM_CHAR = 0x102,
	KEY_REPEAT = 1,         /* repeat count 1 in bits 0..15 */
	KEY_SCANCODE_SHIFT = 16,
};

/* Windows virtual keys and set 1 scancodes: the control keys that type a
 * character, then the keys a joystick can press (dinput.c), for which the
 * keyboard sends the same codes. */
enum
{
	VK_BACK = 0x08,
	VK_TAB = 0x09,
	VK_RETURN = 0x0D,
	VK_ESCAPE = 0x1B,
	VK_LEFT = 0x25,
	VK_UP = 0x26,
	VK_RIGHT = 0x27,
	VK_DOWN = 0x28,
	VK_F11 = 0x7A,
	VK_F12 = 0x7B,
	SC_ESCAPE = 0x01,
	SC_UP = 0x48,
	SC_LEFT = 0x4B,
	SC_RIGHT = 0x4D,
	SC_DOWN = 0x50,
	SC_F11 = 0x57,
	SC_F12 = 0x58,
};

/* Graphics slot: header words, then command words, then vertices. glide.c
 * publishes the two slots alternately, starting with SHARED_GFX0, and the
 * renderer replays them in that order. */
enum
{
	GFX_STATE = 0,          /* GFX_FREE/GFX_WRITING/GFX_PARTIAL/GFX_FRAME */
	GFX_CMD_WORDS = 1,      /* used command words */
	GFX_VTX_COUNT = 2,      /* used vertices */
	GFX_HEADER_WORDS = 16,

	GFX_CMD_CAPACITY = 3 << 20,     /* words (12 MiB) */
	GFX_VTX_CAPACITY = 174762,      /* vertices (4 MiB / 24 bytes) */
	GFX_VTX_BYTES = 24,             /* float x, y (game units), oow, s, t (texture coords are s/oow, t/oow); RGBA8 */
	GFX_VTX_OOW_OFFSET = 8,         /* bytes from vertex start, after x and y */
	GFX_VTX_ST_OFFSET = 12,
	GFX_VTX_COLOR_OFFSET = 20,

	GFX_CMD_OFFSET = GFX_HEADER_WORDS * 4,                    /* bytes from slot start */
	GFX_VTX_OFFSET = GFX_CMD_OFFSET + GFX_CMD_CAPACITY * 4,  /* bytes from slot start */
	GFX_SLOT_BYTES = GFX_VTX_OFFSET + GFX_VTX_CAPACITY * GFX_VTX_BYTES,
};

enum
{
	GFX_FREE = 0,
	GFX_WRITING = 1,
	GFX_PARTIAL = 2,        /* mid-frame overflow: execute now, no present */
	GFX_FRAME = 3,          /* ends with GFX_OP_SWAP: execute and present */
};

/* Command header word: op | (total words including header) << GFX_OP_SIZE_SHIFT.
 * Ops replace one piece of renderer state each, starting from the
 * GFX_DEFAULT_* state below. */
enum
{
	GFX_OP_CLIP = 1,        /* minX, minY, maxX, maxY in game units */
	GFX_OP_BLEND = 2,       /* src, dst (BLEND_*) */
	GFX_OP_TEXENABLE = 3,   /* 0/1 */
	GFX_OP_FOGENABLE = 4,   /* 0/1 */
	GFX_OP_DEPTHMASK = 5,   /* 0/1 */
	GFX_OP_FOGCOLOR = 6,    /* 0x00RRGGBB */
	GFX_OP_FOGTABLE = 7,    /* GFX_FOG_ENTRIES fog bytes indexed by integer w */
	GFX_OP_CLEAR = 8,       /* color 0x00RRGGBB, alpha 0..255, depth 0..65535; honours clip and depth mask */
	GFX_OP_TEXIMAGE = 9,    /* id, size, size*size premultiplied RGBA8 texels; binding unchanged */
	GFX_OP_TEXBIND = 10,    /* id, or GFX_NO_TEXTURE; ids without an image sample black */
	GFX_OP_GAMMA = 11,      /* float bits applied when presenting */
	GFX_OP_TRIS = 12,       /* first vertex, vertex count */
	GFX_OP_SWAP = 13,       /* present the frame */
	GFX_OP_DEPTHFUNC = 14,  /* Glide GR_CMP_* 0..7 (same order as GL) */
	GFX_OP_SAMPLER = 15,    /* bilinear 0/1, clamp s 0/1, clamp t 0/1 (0 wraps) */

	GFX_OP_SIZE_SHIFT = 8,
	GFX_OP_MASK = (1 << GFX_OP_SIZE_SHIFT) - 1,
	GFX_FOG_ENTRIES = 65536,
	GFX_TEX_MAX_SIZE = 256, /* texture sizes are powers of two up to Glide's largest */
	GFX_NO_TEXTURE = -1,
};

enum
{
	BLEND_ZERO = 0,
	BLEND_ONE = 1,
	BLEND_SRC_ALPHA = 2,
	BLEND_INV_SRC_ALPHA = 3,
};

/* Renderer state before the first command, which glide.c starts from too.
 * Besides these, the clip window covers the screen, texturing and fog are off,
 * the fog colour and table are 0, GFX_NO_TEXTURE is bound and gamma is 1.0. */
enum
{
	GFX_DEFAULT_BLEND_SRC = BLEND_ONE,
	GFX_DEFAULT_BLEND_DST = BLEND_ZERO,
	GFX_DEFAULT_DEPTH_FUNC = 3,     /* GR_CMP_LEQUAL */
	GFX_DEFAULT_DEPTH_MASK = 1,
	GFX_DEFAULT_BILINEAR = 1,
	GFX_DEFAULT_CLAMP = 1,          /* s and t */
};

/* Audio ring: int32 header words, then interleaved int16 stereo frames. */
enum
{
	AUD_WRITE = 0,          /* total frames written (C) */
	AUD_READ = 1,           /* total frames consumed (worklet), futex word */
	AUD_ACTIVE = 2,         /* 1 while the worklet plays: from the first iSNDdirectserve_ after iSNDdirectstart_ to iSNDdirectstop_ */
	AUD_RATE = 3,           /* sample rate of the frames */
	AUD_CONSUMER = 4,       /* set by JS before main() when the AudioWorklet plays the ring */
	AUD_HEADER_WORDS = 16,

	AUD_CAPACITY_FRAMES = 8192,
	AUD_TARGET_FRAMES = 1536,
	AUD_SAMPLE_RATE = 22050,
	AUD_CHUNK_FRAMES = 256, /* the game's mixer always produces 256 frames */
};

#endif /* NFS2SE_WEB_LAYOUT_H */

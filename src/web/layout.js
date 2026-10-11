// Shared memory layout, mirrored from src/platform/web_layout.h (see the
// comments there). tests/unit/layout.test.mjs fails when the two drift apart.

export const SHARED_CTL = 0;
export const SHARED_GFX0 = 1;
export const SHARED_GFX1 = 2;
export const SHARED_AUDIO = 3;

export const GAME_WIDTH = 640;
export const GAME_HEIGHT = 480;

export const CTL_SEQ = 0;
export const CTL_RING_HEAD = 1;
export const CTL_RING_TAIL = 2;
export const CTL_MOUSE_X = 3;
export const CTL_MOUSE_Y = 4;
export const CTL_MOUSE_BUTTONS = 5;
export const CTL_MOUSE_MOVES = 6;
export const CTL_DIALOG = 7;
export const CTL_FS_DIRTY = 8;
export const CTL_CFG_KEEP_ASPECT = 9;
export const CTL_CFG_LINEAR_FILTER = 10;
export const CTL_PAD_BASE = 16;
export const CTL_PAD_STRIDE = 16;
export const CTL_PAD_COUNT = 2;
export const CTL_RING_BASE = 64;
export const CTL_RING_ENTRIES = 256;
export const CTL_RING_ENTRY_WORDS = 4;
export const CTL_WORDS = CTL_RING_BASE + CTL_RING_ENTRIES * CTL_RING_ENTRY_WORDS;

export const PAD_CONNECTED = 0;
export const PAD_BUTTONS = 1;
export const PAD_HAT = 2;
export const PAD_NUM_HATS = 3;
export const PAD_NUM_BUTTONS = 4;
export const PAD_NUM_AXES = 5;
export const PAD_AXES = 6;
export const PAD_MAX_AXES = 6;

export const DIALOG_OK = 1;
export const DIALOG_CANCEL = 2;

export const MOUSE_LEFT = 1;
export const MOUSE_LEFT_LATCH = 2;

export const HAT_UP = 1;
export const HAT_RIGHT = 2;
export const HAT_DOWN = 4;
export const HAT_LEFT = 8;

export const RING_MSG = 0;
export const RING_WPARAM = 1;
export const RING_LPARAM = 2;

export const WM_KEYDOWN = 0x100;
export const WM_KEYUP = 0x101;
export const WM_CHAR = 0x102;
export const KEY_REPEAT = 1;
export const KEY_SCANCODE_SHIFT = 16;

export const VK_BACK = 0x08;
export const VK_TAB = 0x09;
export const VK_RETURN = 0x0D;
export const VK_ESCAPE = 0x1B;
export const VK_LEFT = 0x25;
export const VK_UP = 0x26;
export const VK_RIGHT = 0x27;
export const VK_DOWN = 0x28;
export const VK_F11 = 0x7A;
export const VK_F12 = 0x7B;
export const SC_ESCAPE = 0x01;
export const SC_UP = 0x48;
export const SC_LEFT = 0x4B;
export const SC_RIGHT = 0x4D;
export const SC_DOWN = 0x50;
export const SC_F11 = 0x57;
export const SC_F12 = 0x58;

export const GFX_STATE = 0;
export const GFX_CMD_WORDS = 1;
export const GFX_VTX_COUNT = 2;
export const GFX_HEADER_WORDS = 16;
export const GFX_CMD_CAPACITY = 3 << 20;
export const GFX_VTX_CAPACITY = 174762;
export const GFX_VTX_BYTES = 24;
export const GFX_VTX_OOW_OFFSET = 8;
export const GFX_VTX_ST_OFFSET = 12;
export const GFX_VTX_COLOR_OFFSET = 20;
export const GFX_CMD_OFFSET = GFX_HEADER_WORDS * 4;
export const GFX_VTX_OFFSET = GFX_CMD_OFFSET + GFX_CMD_CAPACITY * 4;
export const GFX_SLOT_BYTES = GFX_VTX_OFFSET + GFX_VTX_CAPACITY * GFX_VTX_BYTES;

export const GFX_FREE = 0;
export const GFX_WRITING = 1;
export const GFX_PARTIAL = 2;
export const GFX_FRAME = 3;

export const GFX_OP_CLIP = 1;
export const GFX_OP_BLEND = 2;
export const GFX_OP_TEXENABLE = 3;
export const GFX_OP_FOGENABLE = 4;
export const GFX_OP_DEPTHMASK = 5;
export const GFX_OP_FOGCOLOR = 6;
export const GFX_OP_FOGTABLE = 7;
export const GFX_OP_CLEAR = 8;
export const GFX_OP_TEXIMAGE = 9;
export const GFX_OP_TEXBIND = 10;
export const GFX_OP_GAMMA = 11;
export const GFX_OP_TRIS = 12;
export const GFX_OP_SWAP = 13;
export const GFX_OP_DEPTHFUNC = 14;
export const GFX_OP_SAMPLER = 15;
export const GFX_OP_SIZE_SHIFT = 8;
export const GFX_OP_MASK = (1 << GFX_OP_SIZE_SHIFT) - 1;
export const GFX_FOG_ENTRIES = 65536;
export const GFX_TEX_MAX_SIZE = 256;
export const GFX_NO_TEXTURE = -1;

export const BLEND_ZERO = 0;
export const BLEND_ONE = 1;
export const BLEND_SRC_ALPHA = 2;
export const BLEND_INV_SRC_ALPHA = 3;

export const GFX_DEFAULT_BLEND_SRC = BLEND_ONE;
export const GFX_DEFAULT_BLEND_DST = BLEND_ZERO;
export const GFX_DEFAULT_DEPTH_FUNC = 3;
export const GFX_DEFAULT_DEPTH_MASK = 1;
export const GFX_DEFAULT_BILINEAR = 1;
export const GFX_DEFAULT_CLAMP = 1;

export const AUD_WRITE = 0;
export const AUD_READ = 1;
export const AUD_ACTIVE = 2;
export const AUD_RATE = 3;
export const AUD_CONSUMER = 4;
export const AUD_HEADER_WORDS = 16;
export const AUD_CAPACITY_FRAMES = 8192;
export const AUD_TARGET_FRAMES = 1536;
export const AUD_SAMPLE_RATE = 22050;
export const AUD_CHUNK_FRAMES = 256;

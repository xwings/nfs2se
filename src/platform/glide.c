/* Glide 2.x imports. The game thread records every call into one of the two
 * shared graphics slots (web_layout.h) and src/web/renderer.js replays the
 * slots in publication order with WebGL2, so the game never touches WebGL.
 * State calls only update the desired state; the differences are emitted
 * right before the next draw or clear, which makes redundant calls free.
 *
 * Rendering semantics follow NFSIISE's Glide2x/OpenGL2.c (MIT, zaps166):
 * x and y lose the vertex snap bias, texture coordinates are sow/256 and
 * tow/256 divided by oow per pixel, colour combine is vertex colour times
 * texture, fragments with alpha <= 16/255 are discarded, chroma key, culling
 * and depth bias are ignored, and the LFB is a write-only scratch buffer.
 * Differences: fog is looked up per pixel from the hardware fog table, the
 * game's depth function, blend source factor and texture clamp/filter modes
 * are honoured, P8 textures are expanded with the palette current at draw
 * time, textures are premultiplied for artefact-free filtering, and lines are
 * quads one game unit (one original pixel) wide. */
#include "platform.h"
#include "web_layout.h"

#include <math.h>
#include <string.h>

#define TEXTURE_MEM 0x200000
#define VERTEX_SNAP 786432.0f /* 0xC0000: the game adds it to x and y to snap them to 1/16 */
#define LINE_HALF_WIDTH 0.5f
#define SLOT_WAIT_MS 100
#define NO_COMMAND UINT32_MAX
#define FOG_TABLE_SIZE 64
#define LFB_BYTES (GAME_WIDTH * GAME_HEIGHT * 2)

#define GR_BLEND_ZERO 0x0
#define GR_BLEND_ONE 0x4
#define GR_BLEND_ONE_MINUS_SRC_ALPHA 0x5
#define GR_COMBINE_OTHER_TEXTURE 0x1
#define GR_FOG_DISABLE 0x0
#define GR_FOG_WITH_TABLE 0x2
#define GR_LFB_WRITE_ONLY 0x1
#define GR_LOD_1 0x8
#define GR_TEXTABLE_PALETTE 0x2
#define GR_TEXFMT_P_8 0x5
#define GR_TEXFMT_16BIT 0x8
#define GR_TEXFMT_RGB_565 0xa
#define GR_TEXFMT_ARGB_1555 0xb
#define GR_TEXFMT_ARGB_4444 0xc
#define GR_TEXTURECLAMP_CLAMP 0x1
#define GR_TEXTUREFILTER_BILINEAR 0x1

/* Glide structures as the 32-bit game lays them out. */
typedef struct
{
	int32_t smallLod, largeLod, aspectRatio, format;
	const uint8_t *data;
} GrTexInfo;
_Static_assert(sizeof(GrTexInfo) == 20, "GrTexInfo layout");

typedef struct
{
	int32_t size;
	void *lfbPtr;
	uint32_t strideInBytes;
	int32_t writeMode, origin;
} GrLfbInfo;
_Static_assert(sizeof(GrLfbInfo) == 20, "GrLfbInfo layout");

/* GrVertex float indices: x, y, z, r, g, b, ooz, a, oow, sow, tow. */
enum
{
	V_X = 0,
	V_Y = 1,
	V_R = 3,
	V_G = 4,
	V_B = 5,
	V_A = 7,
	V_OOW = 8,
	V_SOW = 9,
	V_TOW = 10,
	V_FLOATS = 11,
};

typedef struct
{
	int32_t clip[4];
	int32_t blend[2];
	int32_t depthFunc, depthMask;
	int32_t texEnabled, texture; /* bound start address or GFX_NO_TEXTURE */
	int32_t sampler[3];          /* bilinear, clamp s, clamp t */
	int32_t fogEnabled, fogColor;
} State;

/* The renderer's initial state, see GFX_DEFAULT_* in web_layout.h. */
#define DEFAULT_STATE                                                            \
	{                                                                            \
		.clip = {0, 0, GAME_WIDTH, GAME_HEIGHT},                                 \
		.blend = {GFX_DEFAULT_BLEND_SRC, GFX_DEFAULT_BLEND_DST},                 \
		.depthFunc = GFX_DEFAULT_DEPTH_FUNC, .depthMask = GFX_DEFAULT_DEPTH_MASK, \
		.texture = GFX_NO_TEXTURE,                                               \
		.sampler = {GFX_DEFAULT_BILINEAR, GFX_DEFAULT_CLAMP, GFX_DEFAULT_CLAMP}, \
	}

_Static_assert(3 + GFX_TEX_MAX_SIZE * GFX_TEX_MAX_SIZE <= GFX_CMD_CAPACITY, "the largest command, a texture image, fits an empty slot");
_Static_assert(GFX_CMD_CAPACITY < 1 << (32 - GFX_OP_SIZE_SHIFT), "command sizes fit the header word");

typedef struct
{
	uint8_t format;
	uint8_t lod; /* size is lod_size(lod) */
	bool valid;
	uint32_t paletteGen; /* P8: palette generation of the uploaded expansion, 0 for none */
} Texture;

static State s_state = DEFAULT_STATE, s_emitted = DEFAULT_STATE;

static Texture s_textures[TEXTURE_MEM >> 2]; /* by start address / 4 */
static uint8_t s_textureMem[TEXTURE_MEM];    /* P8 indices by start address */
static uint32_t s_palette[256];               /* as downloaded: 0xAARRGGBB */
static uint32_t s_palettePremul[256];         /* premultiplied RGBA8 */
static uint32_t s_paletteGen = 1;

static uint8_t s_fogTable[FOG_TABLE_SIZE];
static uint8_t s_fogLut[GFX_FOG_ENTRIES];
static bool s_fogLutChanged;

static float s_gamma = 1.0f;
static uint8_t s_lfb[LFB_BYTES];

/* Recorder: the slot being written and its fill level. */
static int s_slot;
static bool s_slotOpen;
static uint32_t *s_cmd;
static uint8_t *s_vtx;
static uint32_t s_cmdWords, s_vtxCount;
static uint32_t s_trisCmd = NO_COMMAND; /* index of the TRIS command still accepting vertices */

static int32_t *slot_header(int slot)
{
	return (int32_t *)web_gfx_slot(slot);
}

/* Waits until the renderer has released the slot. */
static void open_slot(void)
{
	int32_t *header = slot_header(s_slot);
	int32_t state;
	while ((state = __atomic_load_n(&header[GFX_STATE], __ATOMIC_SEQ_CST)) != GFX_FREE)
		web_futex_wait(&header[GFX_STATE], state, SLOT_WAIT_MS);
	__atomic_store_n(&header[GFX_STATE], GFX_WRITING, __ATOMIC_SEQ_CST);
	s_cmd = (uint32_t *)((uint8_t *)header + GFX_CMD_OFFSET);
	s_vtx = (uint8_t *)header + GFX_VTX_OFFSET;
	s_cmdWords = 0;
	s_vtxCount = 0;
	s_trisCmd = NO_COMMAND;
	s_slotOpen = true;
}

static void publish(int32_t state)
{
	int32_t *header = slot_header(s_slot);
	header[GFX_CMD_WORDS] = (int32_t)s_cmdWords;
	header[GFX_VTX_COUNT] = (int32_t)s_vtxCount;
	__atomic_store_n(&header[GFX_STATE], state, __ATOMIC_SEQ_CST);
	s_slot ^= 1;
	s_slotOpen = false;
}

/* Makes room for the given command words and vertices; a full slot is
 * published as a partial frame and recording continues in the other one. */
static void reserve(uint32_t words, uint32_t vertices)
{
	if (s_slotOpen && (s_cmdWords + words > GFX_CMD_CAPACITY || s_vtxCount + vertices > GFX_VTX_CAPACITY))
		publish(GFX_PARTIAL);
	if (!s_slotOpen)
		open_slot();
}

static uint32_t *put_command(int op, uint32_t payloadWords)
{
	const uint32_t words = payloadWords + 1;
	reserve(words, 0);
	uint32_t *command = s_cmd + s_cmdWords;
	command[0] = (uint32_t)op | words << GFX_OP_SIZE_SHIFT;
	s_cmdWords += words;
	s_trisCmd = NO_COMMAND;
	return command + 1;
}

static void sync_word(int op, int32_t want, int32_t *have)
{
	if (want == *have)
		return;
	*put_command(op, 1) = (uint32_t)want;
	*have = want;
}

static void sync_words(int op, const int32_t *want, int32_t *have, uint32_t count)
{
	if (!memcmp(want, have, count * sizeof *want))
		return;
	memcpy(put_command(op, count), want, count * sizeof *want);
	memcpy(have, want, count * sizeof *want);
}

static uint32_t *put_teximage(uint32_t address, uint32_t size)
{
	uint32_t *payload = put_command(GFX_OP_TEXIMAGE, 2 + size * size);
	payload[0] = address;
	payload[1] = size;
	return payload + 2;
}

/* Side of a Glide texture level in texels. */
static uint32_t lod_size(int32_t lod)
{
	return (uint32_t)GFX_TEX_MAX_SIZE >> lod;
}

static uint32_t premultiplied(uint32_t r, uint32_t g, uint32_t b, uint32_t a)
{
	if (a != 255)
	{
		r = (r * a + 127) / 255;
		g = (g * a + 127) / 255;
		b = (b * a + 127) / 255;
	}
	return r | g << 8 | b << 16 | a << 24;
}

static uint32_t texel_565(uint32_t v)
{
	const uint32_t r = v >> 11, g = (v >> 5) & 63, b = v & 31;
	return premultiplied(r << 3 | r >> 2, g << 2 | g >> 4, b << 3 | b >> 2, 255);
}

static uint32_t texel_1555(uint32_t v)
{
	if (!(v & 0x8000))
		return 0;
	const uint32_t r = (v >> 10) & 31, g = (v >> 5) & 31, b = v & 31;
	return premultiplied(r << 3 | r >> 2, g << 3 | g >> 2, b << 3 | b >> 2, 255);
}

static uint32_t texel_4444(uint32_t v)
{
	return premultiplied(((v >> 8) & 15) * 17, ((v >> 4) & 15) * 17, (v & 15) * 17, (v >> 12) * 17);
}

static uint32_t texel_argb(uint32_t v)
{
	return premultiplied((v >> 16) & 255, (v >> 8) & 255, v & 255, v >> 24);
}

static void upload_p8(uint32_t address, Texture *texture)
{
	const uint32_t size = lod_size(texture->lod);
	const uint8_t *in = s_textureMem + address;
	uint32_t *out = put_teximage(address, size);
	for (uint32_t i = 0; i < size * size; ++i)
		out[i] = s_palettePremul[in[i]];
	texture->paletteGen = s_paletteGen;
}

/* Emits the state that differs from what the renderer has. Clears only depend
 * on the clip window and the depth mask. */
static void sync_state(bool draw)
{
	sync_words(GFX_OP_CLIP, s_state.clip, s_emitted.clip, 4);
	sync_word(GFX_OP_DEPTHMASK, s_state.depthMask, &s_emitted.depthMask);
	if (!draw)
		return;
	sync_words(GFX_OP_BLEND, s_state.blend, s_emitted.blend, 2);
	sync_word(GFX_OP_DEPTHFUNC, s_state.depthFunc, &s_emitted.depthFunc);
	sync_word(GFX_OP_TEXENABLE, s_state.texEnabled, &s_emitted.texEnabled);
	if (s_state.texEnabled)
	{
		if (s_state.texture >= 0)
		{
			Texture *texture = &s_textures[s_state.texture >> 2];
			if (texture->valid && texture->format == GR_TEXFMT_P_8 && texture->paletteGen != s_paletteGen)
				upload_p8((uint32_t)s_state.texture, texture);
		}
		sync_word(GFX_OP_TEXBIND, s_state.texture, &s_emitted.texture);
		sync_words(GFX_OP_SAMPLER, s_state.sampler, s_emitted.sampler, 3);
	}
	sync_word(GFX_OP_FOGENABLE, s_state.fogEnabled, &s_emitted.fogEnabled);
	if (s_state.fogEnabled)
	{
		sync_word(GFX_OP_FOGCOLOR, s_state.fogColor, &s_emitted.fogColor);
		if (s_fogLutChanged)
		{
			memcpy(put_command(GFX_OP_FOGTABLE, GFX_FOG_ENTRIES / 4), s_fogLut, GFX_FOG_ENTRIES);
			s_fogLutChanged = false;
		}
	}
}

/* Returns room for count vertices drawn as triangles with the current state. */
static uint8_t *put_vertices(uint32_t count)
{
	sync_state(true);
	reserve(3, count);
	if (s_trisCmd == NO_COMMAND)
	{
		uint32_t *payload = put_command(GFX_OP_TRIS, 2);
		payload[0] = s_vtxCount;
		payload[1] = 0;
		s_trisCmd = s_cmdWords - 3;
	}
	s_cmd[s_trisCmd + 2] += count;
	uint8_t *out = s_vtx + s_vtxCount * GFX_VTX_BYTES;
	s_vtxCount += count;
	return out;
}

static void read_vertex(const void *grVertex, float v[V_FLOATS])
{
	memcpy(v, grVertex, V_FLOATS * sizeof *v);
}

static uint8_t color_byte(float value)
{
	if (!(value > 0.0f))
		return 0;
	if (value >= 255.0f)
		return 255;
	return (uint8_t)(value + 0.5f);
}

static void put_vertex(uint8_t *out, const float v[V_FLOATS], float dx, float dy)
{
	const float position[2] = {v[V_X] - VERTEX_SNAP + dx, v[V_Y] - VERTEX_SNAP + dy};
	const float st[2] = {v[V_SOW] / 256.0f, v[V_TOW] / 256.0f};
	memcpy(out, position, sizeof position);
	memcpy(out + GFX_VTX_OOW_OFFSET, &v[V_OOW], sizeof v[V_OOW]);
	memcpy(out + GFX_VTX_ST_OFFSET, st, sizeof st);
	uint8_t *color = out + GFX_VTX_COLOR_OFFSET;
	color[0] = color_byte(v[V_R]);
	color[1] = color_byte(v[V_G]);
	color[2] = color_byte(v[V_B]);
	color[3] = color_byte(v[V_A]);
}

/* w at fog table index i: exponent i / 4 with a two-bit mantissa, as in the
 * Glide SDK's guFogTableIndexToW. */
static float fog_index_w(int index)
{
	return ldexpf(8.0f / (float)(8 - (index & 3)), index >> 2);
}

/* Expands the 64-entry table to one entry per integer w. The hardware blends
 * neighbouring entries linearly in 1/w. */
static void build_fog_lut(void)
{
	int index = 0;
	s_fogLut[0] = s_fogTable[0];
	for (uint32_t w = 1; w < GFX_FOG_ENTRIES; ++w)
	{
		while (index < FOG_TABLE_SIZE - 1 && fog_index_w(index + 1) <= (float)w)
			++index;
		if (index == FOG_TABLE_SIZE - 1)
		{
			s_fogLut[w] = s_fogTable[index];
			continue;
		}
		const float nearOow = 1.0f / fog_index_w(index), farOow = 1.0f / fog_index_w(index + 1);
		const float t = (nearOow - 1.0f / (float)w) / (nearOow - farOow);
		const float fog = s_fogTable[index] + (float)(s_fogTable[index + 1] - s_fogTable[index]) * t;
		s_fogLut[w] = (uint8_t)(fog + 0.5f);
	}
}

/* Glide entry points */

int32_t grGlideInit(void)
{
	return 0;
}

int32_t grGlideShutdown(void)
{
	return 0;
}

int32_t grSstQueryHardware(void *hwConfig)
{
	(void)hwConfig; /* left untouched like NFSIISE: the game then skips grSstSelect */
	return 1;
}

int32_t grSstSelect(int32_t sst)
{
	(void)sst;
	return 0;
}

int32_t grSstWinOpen(int32_t hWnd, int32_t resolution, int32_t refresh, int32_t colorFormat, int32_t origin, int32_t colorBuffers, int32_t auxBuffers)
{
	(void)hWnd;
	(void)resolution;
	(void)refresh;
	(void)colorFormat;
	(void)origin;
	(void)colorBuffers;
	(void)auxBuffers;
	const int32_t full[4] = {0, 0, GAME_WIDTH, GAME_HEIGHT};
	memcpy(s_state.clip, full, sizeof full);
	return 1;
}

int32_t grSstWinClose(void)
{
	return 0;
}

int32_t grSstIdle(void)
{
	return 0;
}

int32_t grSstIsBusy(void)
{
	return 0;
}

int32_t grSstStatus(void)
{
	return 0x0FFFF03F; /* idle, no swaps pending */
}

int32_t grBufferSwap(int32_t swapInterval)
{
	(void)swapInterval;
	put_command(GFX_OP_SWAP, 0);
	publish(GFX_FRAME);
	open_slot(); /* back-pressure: at most two frames ahead of the display */
	return 0;
}

int32_t grRenderBuffer(int32_t buffer)
{
	(void)buffer;
	return 0;
}

int32_t grBufferClear(uint32_t color, uint32_t alpha, uint32_t depth)
{
	sync_state(false);
	uint32_t *payload = put_command(GFX_OP_CLEAR, 3);
	payload[0] = color & 0xFFFFFF;
	payload[1] = alpha & 0xFF;
	payload[2] = depth & 0xFFFF;
	return 0;
}

int32_t grClipWindow(int32_t minX, int32_t minY, int32_t maxX, int32_t maxY)
{
	s_state.clip[0] = minX;
	s_state.clip[1] = minY;
	s_state.clip[2] = maxX;
	s_state.clip[3] = maxY;
	return 0;
}

int32_t grDrawTriangle(const void *a, const void *b, const void *c)
{
	float va[V_FLOATS], vb[V_FLOATS], vc[V_FLOATS];
	read_vertex(a, va);
	read_vertex(b, vb);
	read_vertex(c, vc);
	uint8_t *out = put_vertices(3);
	put_vertex(out, va, 0.0f, 0.0f);
	put_vertex(out + GFX_VTX_BYTES, vb, 0.0f, 0.0f);
	put_vertex(out + 2 * GFX_VTX_BYTES, vc, 0.0f, 0.0f);
	return 0;
}

/* A quad one game unit wide across the line's minor axis. */
int32_t grDrawLine(const void *a, const void *b)
{
	float va[V_FLOATS], vb[V_FLOATS];
	read_vertex(a, va);
	read_vertex(b, vb);
	const bool xMajor = fabsf(vb[V_X] - va[V_X]) >= fabsf(vb[V_Y] - va[V_Y]);
	const float dx = xMajor ? 0.0f : LINE_HALF_WIDTH, dy = xMajor ? LINE_HALF_WIDTH : 0.0f;
	uint8_t *out = put_vertices(6);
	put_vertex(out, va, -dx, -dy);
	put_vertex(out + GFX_VTX_BYTES, va, dx, dy);
	put_vertex(out + 2 * GFX_VTX_BYTES, vb, dx, dy);
	put_vertex(out + 3 * GFX_VTX_BYTES, va, -dx, -dy);
	put_vertex(out + 4 * GFX_VTX_BYTES, vb, dx, dy);
	put_vertex(out + 5 * GFX_VTX_BYTES, vb, -dx, -dy);
	return 0;
}

int32_t grAlphaBlendFunction(int32_t rgbSrc, int32_t rgbDst, int32_t alphaSrc, int32_t alphaDst)
{
	(void)alphaSrc;
	(void)alphaDst;
	s_state.blend[0] = rgbSrc == GR_BLEND_ZERO ? BLEND_ZERO : rgbSrc == GR_BLEND_ONE ? BLEND_ONE : BLEND_SRC_ALPHA;
	s_state.blend[1] = rgbDst == GR_BLEND_ONE ? BLEND_ONE : rgbDst == GR_BLEND_ONE_MINUS_SRC_ALPHA ? BLEND_INV_SRC_ALPHA : BLEND_ZERO;
	return 0;
}

int32_t grAlphaCombine(int32_t function, int32_t factor, int32_t local, int32_t other, int32_t invert)
{
	(void)function;
	(void)factor;
	(void)local;
	(void)invert;
	s_state.texEnabled = other == GR_COMBINE_OTHER_TEXTURE;
	return 0;
}

int32_t grColorCombine(int32_t function, int32_t factor, int32_t local, int32_t other, int32_t invert)
{
	(void)function;
	(void)factor;
	(void)local;
	(void)other;
	(void)invert;
	return 0;
}

int32_t grAlphaTestFunction(int32_t function)
{
	(void)function;
	return 0;
}

int32_t grAlphaTestReferenceValue(int32_t value)
{
	(void)value;
	return 0;
}

int32_t grChromakeyMode(int32_t mode)
{
	(void)mode;
	return 0;
}

int32_t grChromakeyValue(int32_t value)
{
	(void)value;
	return 0;
}

int32_t grCullMode(int32_t mode)
{
	(void)mode;
	return 0;
}

int32_t grDepthBiasLevel(int32_t level)
{
	(void)level;
	return 0;
}

int32_t grDepthBufferMode(int32_t mode)
{
	(void)mode;
	return 0;
}

int32_t grDepthBufferFunction(int32_t function)
{
	s_state.depthFunc = function & 7;
	return 0;
}

int32_t grDepthMask(int32_t mask)
{
	s_state.depthMask = mask != 0;
	return 0;
}

int32_t grDitherMode(int32_t mode)
{
	(void)mode;
	return 0;
}

int32_t grFogMode(int32_t mode)
{
	if (mode == GR_FOG_DISABLE)
		s_state.fogEnabled = 0;
	else if (mode == GR_FOG_WITH_TABLE)
		s_state.fogEnabled = 1;
	return 0;
}

int32_t grFogColorValue(uint32_t color)
{
	s_state.fogColor = (int32_t)(color & 0xFFFFFF);
	return 0;
}

int32_t grFogTable(const uint8_t *table)
{
	if (!memcmp(s_fogTable, table, FOG_TABLE_SIZE))
		return 0;
	memcpy(s_fogTable, table, FOG_TABLE_SIZE);
	build_fog_lut();
	s_fogLutChanged = true;
	return 0;
}

int32_t guFogGenerateExp(uint8_t *table, float density)
{
	const float last = 1.0f - expf(-density * fog_index_w(FOG_TABLE_SIZE - 1));
	for (int i = 0; i < FOG_TABLE_SIZE; ++i)
	{
		float f = last > 0.0f ? (1.0f - expf(-density * fog_index_w(i))) / last : 0.0f;
		f = f < 0.0f ? 0.0f : f > 1.0f ? 1.0f : f;
		table[i] = (uint8_t)(f * 255.0f);
	}
	return 0;
}

int32_t grGammaCorrectionValue(float value)
{
	if (value == s_gamma)
		return 0;
	s_gamma = value;
	memcpy(put_command(GFX_OP_GAMMA, 1), &value, sizeof value);
	return 0;
}

int32_t grLfbLock(int32_t type, int32_t buffer, int32_t writeMode, int32_t origin, int32_t pixelPipeline, GrLfbInfo *info)
{
	(void)buffer;
	(void)writeMode;
	(void)origin;
	(void)pixelPipeline;
	memset(info, 0, sizeof *info);
	if (type != GR_LFB_WRITE_ONLY)
		return 0;
	info->lfbPtr = s_lfb;
	info->strideInBytes = 2; /* as in NFSIISE: rows overlap, the writes are dropped */
	return 1;
}

int32_t grLfbUnlock(int32_t type, int32_t buffer)
{
	(void)type;
	(void)buffer;
	return 1;
}

int32_t grTexCalcMemRequired(int32_t smallLod, int32_t largeLod, int32_t aspectRatio, int32_t format)
{
	(void)smallLod;
	(void)aspectRatio;
	const uint32_t size = lod_size(largeLod & 15);
	return (int32_t)(size * size * (format >= GR_TEXFMT_16BIT ? 2 : 1));
}

int32_t grTexMinAddress(int32_t tmu)
{
	(void)tmu;
	return 0;
}

int32_t grTexMaxAddress(int32_t tmu)
{
	(void)tmu;
	return TEXTURE_MEM;
}

int32_t grTexDownloadMipMap(int32_t tmu, uint32_t startAddress, int32_t evenOdd, const GrTexInfo *info)
{
	(void)tmu;
	(void)evenOdd;
	if (startAddress >= TEXTURE_MEM || (uint32_t)info->largeLod > GR_LOD_1)
		return 0;
	const uint32_t size = lod_size(info->largeLod), texels = size * size;
	Texture *texture = &s_textures[startAddress >> 2];
	texture->format = (uint8_t)info->format;
	texture->lod = (uint8_t)info->largeLod;
	texture->paletteGen = 0;
	texture->valid = true;
	const uint8_t *in = info->data;
	switch (info->format)
	{
	case GR_TEXFMT_P_8:
		if (startAddress + texels <= TEXTURE_MEM)
			memcpy(s_textureMem + startAddress, in, texels);
		else
			texture->valid = false;
		break;
	case GR_TEXFMT_RGB_565:
	case GR_TEXFMT_ARGB_1555:
	case GR_TEXFMT_ARGB_4444:
	{
		uint32_t (*convert)(uint32_t) = info->format == GR_TEXFMT_RGB_565 ? texel_565 : info->format == GR_TEXFMT_ARGB_1555 ? texel_1555 : texel_4444;
		uint32_t *out = put_teximage(startAddress, size);
		for (uint32_t i = 0; i < texels; ++i)
			out[i] = convert((uint32_t)in[2 * i] | (uint32_t)in[2 * i + 1] << 8);
		break;
	}
	default:
		texture->valid = false;
		break;
	}
	return 0;
}

int32_t grTexDownloadTable(int32_t tmu, int32_t type, const void *data)
{
	(void)tmu;
	if (type != GR_TEXTABLE_PALETTE || !memcmp(s_palette, data, sizeof s_palette))
		return 0;
	memcpy(s_palette, data, sizeof s_palette);
	for (int i = 0; i < 256; ++i)
		s_palettePremul[i] = texel_argb(s_palette[i]);
	if (++s_paletteGen == 0)
		s_paletteGen = 1;
	return 0;
}

int32_t grTexSource(int32_t tmu, uint32_t startAddress, int32_t evenOdd, const GrTexInfo *info)
{
	(void)tmu;
	(void)evenOdd;
	(void)info;
	s_state.texture = startAddress < TEXTURE_MEM ? (int32_t)startAddress : GFX_NO_TEXTURE;
	return 0;
}

int32_t grTexClampMode(int32_t tmu, int32_t clampS, int32_t clampT)
{
	(void)tmu;
	s_state.sampler[1] = clampS == GR_TEXTURECLAMP_CLAMP;
	s_state.sampler[2] = clampT == GR_TEXTURECLAMP_CLAMP;
	return 0;
}

int32_t grTexFilterMode(int32_t tmu, int32_t minFilter, int32_t magFilter)
{
	(void)tmu;
	(void)minFilter;
	s_state.sampler[0] = magFilter == GR_TEXTUREFILTER_BILINEAR;
	return 0;
}

int32_t grTexMipMapMode(int32_t tmu, int32_t mode, int32_t lodBlend)
{
	(void)tmu;
	(void)mode;
	(void)lodBlend;
	return 0;
}

int32_t grTexCombine(int32_t tmu, int32_t rgbFunction, int32_t rgbFactor, int32_t alphaFunction, int32_t alphaFactor, int32_t rgbInvert, int32_t alphaInvert)
{
	(void)tmu;
	(void)rgbFunction;
	(void)rgbFactor;
	(void)alphaFunction;
	(void)alphaFactor;
	(void)rgbInvert;
	(void)alphaInvert;
	return 0;
}

int32_t grTexCombineFunction(int32_t tmu, int32_t function)
{
	(void)tmu;
	(void)function;
	return 0;
}

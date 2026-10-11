/* glide.c: the Glide recorder. The web.c functions it needs are stubbed here;
 * the futex wait stub plays the renderer and drains the published slots, so
 * back-pressure and partial frames run single-threaded. The tests run in order;
 * each leaves blending, clipping, depth, texturing and fog at the defaults. */
#include "check.h"
#include "platform.h"
#include "web_layout.h"

#include <stdlib.h>

#define SNAP 786432.0f
#define MAX_OPS 64
#define LOG_SIZE 4

/* GrVertex float indices, as in glide.c. */
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

int32_t grSstWinOpen(int32_t, int32_t, int32_t, int32_t, int32_t, int32_t, int32_t);
int32_t grBufferSwap(int32_t);
int32_t grBufferClear(uint32_t, uint32_t, uint32_t);
int32_t grClipWindow(int32_t, int32_t, int32_t, int32_t);
int32_t grDrawTriangle(const void *, const void *, const void *);
int32_t grDrawLine(const void *, const void *);
int32_t grAlphaBlendFunction(int32_t, int32_t, int32_t, int32_t);
int32_t grAlphaCombine(int32_t, int32_t, int32_t, int32_t, int32_t);
int32_t grDepthBufferFunction(int32_t);
int32_t grDepthMask(int32_t);
int32_t grFogMode(int32_t);
int32_t grFogColorValue(uint32_t);
int32_t grFogTable(const uint8_t *);
int32_t guFogGenerateExp(uint8_t *, float);
int32_t grGammaCorrectionValue(float);
int32_t grLfbLock(int32_t, int32_t, int32_t, int32_t, int32_t, void *);
int32_t grTexCalcMemRequired(int32_t, int32_t, int32_t, int32_t);
int32_t grTexDownloadMipMap(int32_t, uint32_t, int32_t, const void *);
int32_t grTexDownloadTable(int32_t, int32_t, const void *);
int32_t grTexSource(int32_t, uint32_t, int32_t, const void *);
int32_t grTexClampMode(int32_t, int32_t, int32_t);
int32_t grTexFilterMode(int32_t, int32_t, int32_t);

/* Glide structures with the 32-bit layout glide.c expects. */
typedef struct
{
	int32_t smallLod, largeLod, aspectRatio, format;
	const void *data;
} TexInfo;

typedef struct
{
	int32_t size;
	void *lfbPtr;
	uint32_t strideInBytes;
	int32_t writeMode, origin;
} LfbInfo;

/* A drained slot: which one it was, its header, commands with up to 8 payload
 * words, the first 12 vertices (x, y, oow, s, t and colour) and the last fog
 * table. */
typedef struct
{
	int32_t op, words;
	uint32_t payload[8];
} Op;

typedef struct
{
	int32_t slot, state, vtxCount, opCount;
	Op ops[MAX_OPS];
	float vtx[12][5];
	uint8_t color[12][4];
	uint8_t fog[GFX_FOG_ENTRIES];
} Drained;

static uint8_t *s_slots[2];
static int s_next; /* the slot the renderer replays next */
static int s_waits;
static Drained s_log[LOG_SIZE];
static int s_logCount;

uint8_t *web_gfx_slot(int index)
{
	return s_slots[index];
}

static int32_t *header(int slot)
{
	return (int32_t *)s_slots[slot];
}

/* back 0 is the slot drained last. */
static const Drained *logged(int back)
{
	return &s_log[(s_logCount - 1 - back) % LOG_SIZE];
}

static void drain(int slot)
{
	const int32_t *h = header(slot);
	Drained *d = &s_log[s_logCount++ % LOG_SIZE];
	memset(d, 0, sizeof *d);
	d->slot = slot;
	d->state = h[GFX_STATE];
	d->vtxCount = h[GFX_VTX_COUNT];
	const uint32_t *cmd = (const uint32_t *)(s_slots[slot] + GFX_CMD_OFFSET);
	for (int32_t p = 0; p < h[GFX_CMD_WORDS];)
	{
		Op *op = &d->ops[d->opCount < MAX_OPS ? d->opCount++ : MAX_OPS - 1];
		op->op = (int32_t)(cmd[p] & GFX_OP_MASK);
		op->words = (int32_t)(cmd[p] >> GFX_OP_SIZE_SHIFT);
		if (op->words <= 0 || p + op->words > h[GFX_CMD_WORDS])
		{
			CHECK(!"malformed command");
			break;
		}
		memcpy(op->payload, cmd + p + 1, (size_t)(op->words - 1 < 8 ? op->words - 1 : 8) * 4);
		if (op->op == GFX_OP_FOGTABLE)
			memcpy(d->fog, cmd + p + 1, sizeof d->fog);
		p += op->words;
	}
	const uint8_t *vtx = s_slots[slot] + GFX_VTX_OFFSET;
	for (int32_t i = 0; i < d->vtxCount && i < 12; ++i)
	{
		const uint8_t *v = vtx + i * GFX_VTX_BYTES;
		memcpy(d->vtx[i], v, 2 * sizeof(float));
		memcpy(&d->vtx[i][2], v + GFX_VTX_OOW_OFFSET, sizeof(float));
		memcpy(&d->vtx[i][3], v + GFX_VTX_ST_OFFSET, 2 * sizeof(float));
		memcpy(d->color[i], v + GFX_VTX_COLOR_OFFSET, 4);
	}
	__atomic_store_n(&header(slot)[GFX_STATE], GFX_FREE, __ATOMIC_SEQ_CST);
}

/* Like the renderer: the published slots, alternately. */
static void drain_published(void)
{
	while (header(s_next)[GFX_STATE] >= GFX_PARTIAL)
	{
		drain(s_next);
		s_next ^= 1;
	}
}

void web_futex_wait(int32_t *addr, int32_t expected, uint32_t ms)
{
	(void)expected;
	(void)ms;
	++s_waits;
	drain_published();
	CHECK(*addr == GFX_FREE);
}

/* Ends the frame, drains it and returns it. */
static const Drained *swap(void)
{
	grBufferSwap(1);
	drain_published();
	const Drained *d = logged(0);
	CHECK_INT(d->state, GFX_FRAME);
	return d;
}

static void vertex(float v[V_FLOATS], float x, float y, float oow)
{
	memset(v, 0, V_FLOATS * sizeof *v);
	v[V_X] = SNAP + x;
	v[V_Y] = SNAP + y;
	v[V_R] = v[V_G] = v[V_B] = v[V_A] = 255.0f;
	v[V_OOW] = oow;
}

static void triangle(void)
{
	float a[V_FLOATS], b[V_FLOATS], c[V_FLOATS];
	vertex(a, 0, 0, 1);
	vertex(b, 10, 0, 1);
	vertex(c, 0, 10, 1);
	grDrawTriangle(a, b, c);
}

static int count_ops(const Drained *d, int op)
{
	int n = 0;
	for (int i = 0; i < d->opCount; ++i)
		n += d->ops[i].op == op;
	return n;
}

/* The first command with this op; the test fails when there is none. */
static const Op *find_op(const Drained *d, int op)
{
	static const Op none;
	for (int i = 0; i < d->opCount; ++i)
	{
		if (d->ops[i].op == op)
			return &d->ops[i];
	}
	CHECK(!"op missing");
	return &none;
}

static void check_vertex(const Drained *d, int i, float x, float y)
{
	CHECK(d->vtx[i][0] == x);
	CHECK(d->vtx[i][1] == y);
}

static void test_triangles(void)
{
	float a[V_FLOATS], b[V_FLOATS], c[V_FLOATS];
	vertex(a, 100.5f, 20.25f, 0.5f);
	a[V_R] = 255.4f;
	a[V_G] = -3.0f;
	a[V_B] = 127.5f;
	a[V_A] = 300.0f;
	a[V_SOW] = 128.0f;
	a[V_TOW] = 64.0f;
	vertex(b, 200, 20, 1);
	vertex(c, 150, 120, 1);
	grDrawTriangle(a, b, c);
	triangle();
	const Drained *d = swap();
	/* The default state needs no commands; both triangles share one TRIS. */
	CHECK_INT(d->opCount, 2);
	CHECK(d->ops[0].op == GFX_OP_TRIS && d->ops[0].words == 3);
	CHECK(d->ops[0].payload[0] == 0 && d->ops[0].payload[1] == 6);
	CHECK(d->ops[1].op == GFX_OP_SWAP && d->ops[1].words == 1);
	CHECK_INT(d->vtxCount, 6);
	check_vertex(d, 0, 100.5f, 20.25f);
	CHECK(d->vtx[0][2] == 0.5f && d->vtx[0][3] == 0.5f && d->vtx[0][4] == 0.25f);
	CHECK(d->color[0][0] == 255 && d->color[0][1] == 0 && d->color[0][2] == 128 && d->color[0][3] == 255);
	check_vertex(d, 2, 150.0f, 120.0f);
	check_vertex(d, 4, 10.0f, 0.0f);
}

static void test_lines(void)
{
	float a[V_FLOATS], b[V_FLOATS];
	vertex(a, 10, 10, 1);
	vertex(b, 20, 12, 1);
	grDrawLine(a, b); /* x-major: widened in y */
	vertex(a, 5, 5, 1);
	vertex(b, 6, 50, 1);
	grDrawLine(a, b); /* y-major: widened in x */
	const Drained *d = swap();
	CHECK(d->ops[0].op == GFX_OP_TRIS && d->ops[0].payload[1] == 12);
	const float expected[12][2] = {
		{10, 9.5f}, {10, 10.5f}, {20, 12.5f}, {10, 9.5f}, {20, 12.5f}, {20, 11.5f},
		{4.5f, 5}, {5.5f, 5}, {6.5f, 50}, {4.5f, 5}, {6.5f, 50}, {5.5f, 50},
	};
	for (int i = 0; i < 12; ++i)
		check_vertex(d, i, expected[i][0], expected[i][1]);
}

static void test_state(void)
{
	grDepthMask(1); /* unchanged: free */
	grAlphaBlendFunction(1 /* SRC_ALPHA */, 5 /* ONE_MINUS_SRC_ALPHA */, 4, 0);
	grClipWindow(10, 20, 300, 200);
	grDepthMask(0);
	grBufferClear(0x12345678, 0x1FF, 0x1FFFF);
	triangle();
	grDepthBufferFunction(0x17);
	triangle();
	const Drained *d = swap();
	/* Clears only sync the clip window and the depth mask. */
	const int expected[] = {GFX_OP_CLIP, GFX_OP_DEPTHMASK, GFX_OP_CLEAR, GFX_OP_BLEND, GFX_OP_TRIS, GFX_OP_DEPTHFUNC, GFX_OP_TRIS, GFX_OP_SWAP};
	const int n = (int)(sizeof expected / sizeof *expected);
	CHECK_INT(d->opCount, n);
	for (int i = 0; i < n && i < d->opCount; ++i)
		CHECK_INT(d->ops[i].op, expected[i]);
	CHECK(d->ops[0].payload[0] == 10 && d->ops[0].payload[1] == 20 && d->ops[0].payload[2] == 300 && d->ops[0].payload[3] == 200);
	CHECK_INT(d->ops[1].payload[0], 0);
	CHECK(d->ops[2].payload[0] == 0x345678 && d->ops[2].payload[1] == 0xFF && d->ops[2].payload[2] == 0xFFFF);
	CHECK(d->ops[3].payload[0] == BLEND_SRC_ALPHA && d->ops[3].payload[1] == BLEND_INV_SRC_ALPHA);
	CHECK(d->ops[4].payload[0] == 0 && d->ops[4].payload[1] == 3);
	CHECK_INT(d->ops[5].payload[0], 7);
	CHECK(d->ops[6].payload[0] == 3 && d->ops[6].payload[1] == 3);

	grAlphaBlendFunction(4 /* ONE */, 4, 0, 0);
	triangle();
	d = swap();
	CHECK(d->ops[0].op == GFX_OP_BLEND && d->ops[0].payload[0] == BLEND_ONE && d->ops[0].payload[1] == BLEND_ONE);
	grAlphaBlendFunction(0 /* ZERO */, 0, 0, 0);
	triangle();
	d = swap();
	CHECK(d->ops[0].op == GFX_OP_BLEND && d->ops[0].payload[0] == BLEND_ZERO && d->ops[0].payload[1] == BLEND_ZERO);

	grAlphaBlendFunction(4, 0, 4, 0);
	grClipWindow(0, 0, 640, 480);
	grDepthMask(1);
	grDepthBufferFunction(3);
	triangle();
	d = swap();
	CHECK(count_ops(d, GFX_OP_BLEND) == 1 && find_op(d, GFX_OP_BLEND)->payload[0] == BLEND_ONE && find_op(d, GFX_OP_BLEND)->payload[1] == BLEND_ZERO);
	CHECK(count_ops(d, GFX_OP_CLIP) == 1 && find_op(d, GFX_OP_CLIP)->payload[2] == 640);
	CHECK(count_ops(d, GFX_OP_DEPTHMASK) == 1 && find_op(d, GFX_OP_DEPTHMASK)->payload[0] == 1);
	CHECK(count_ops(d, GFX_OP_DEPTHFUNC) == 1 && find_op(d, GFX_OP_DEPTHFUNC)->payload[0] == 3);

	grGammaCorrectionValue(1.3f);
	grGammaCorrectionValue(1.3f);
	d = swap();
	CHECK_INT(count_ops(d, GFX_OP_GAMMA), 1);
	float gamma;
	memcpy(&gamma, &find_op(d, GFX_OP_GAMMA)->payload[0], sizeof gamma);
	CHECK(gamma == 1.3f);
	grGammaCorrectionValue(1.0f);
	d = swap();
	CHECK_INT(count_ops(d, GFX_OP_GAMMA), 1);
}

static void test_textures(void)
{
	/* 2x2 texels are payload words 2..5. */
	const uint16_t rgb565[4] = {0xF800, 0x07E0, 0x001F, 0xFFFF};
	const TexInfo info = {7, 7, 3, 0xa, rgb565};
	CHECK_INT(grTexCalcMemRequired(7, 7, 3, 0xa), 8);
	CHECK_INT(grTexCalcMemRequired(0, 0, 3, 0x5), 65536);
	grTexDownloadMipMap(0, 0x1000, 3, &info);
	grTexSource(0, 0x1000, 3, &info);
	grTexFilterMode(0, 0, 0);
	grTexClampMode(0, 0, 1);
	triangle(); /* texturing is off: nothing bound */
	grAlphaCombine(1, 0, 0, 1 /* OTHER_TEXTURE */, 0);
	triangle();
	const Drained *d = swap();
	const int expected[] = {GFX_OP_TEXIMAGE, GFX_OP_TRIS, GFX_OP_TEXENABLE, GFX_OP_TEXBIND, GFX_OP_SAMPLER, GFX_OP_TRIS, GFX_OP_SWAP};
	const int n = (int)(sizeof expected / sizeof *expected);
	CHECK_INT(d->opCount, n);
	for (int i = 0; i < n && i < d->opCount; ++i)
		CHECK_INT(d->ops[i].op, expected[i]);
	const Op *image = &d->ops[0];
	CHECK(image->words == 7 && image->payload[0] == 0x1000 && image->payload[1] == 2);
	CHECK(image->payload[2] == 0xFF0000FF && image->payload[3] == 0xFF00FF00 && image->payload[4] == 0xFFFF0000 && image->payload[5] == 0xFFFFFFFF);
	CHECK_INT(d->ops[2].payload[0], 1);
	CHECK_INT(d->ops[3].payload[0], 0x1000);
	CHECK(d->ops[4].payload[0] == 0 && d->ops[4].payload[1] == 0 && d->ops[4].payload[2] == 1);

	/* ARGB1555 without the alpha bit is transparent; ARGB4444 is premultiplied. */
	const uint16_t argb1555[4] = {0x7FFF, 0xFC00, 0x83E0, 0};
	const uint16_t argb4444[4] = {0x8F00, 0xF0F0, 0x0FFF, 0xFFFF};
	const TexInfo info1555 = {7, 7, 3, 0xb, argb1555}, info4444 = {7, 7, 3, 0xc, argb4444};
	grTexDownloadMipMap(0, 0x2000, 3, &info1555);
	grTexDownloadMipMap(0, 0x3000, 3, &info4444);
	d = swap();
	CHECK_INT(d->opCount, 3);
	CHECK(d->ops[0].payload[2] == 0 && d->ops[0].payload[3] == 0xFF0000FF && d->ops[0].payload[4] == 0xFF00FF00);
	CHECK(d->ops[1].payload[2] == 0x88000088 && d->ops[1].payload[3] == 0xFF00FF00 && d->ops[1].payload[4] == 0 && d->ops[1].payload[5] == 0xFFFFFFFF);

	/* Rejected downloads emit nothing; an unknown address binds no texture. */
	const TexInfo outside = {7, 7, 3, 0xa, rgb565}, tooLarge = {9, 9, 3, 0xa, rgb565}, rgb332 = {7, 7, 3, 0x0, rgb565};
	grTexDownloadMipMap(0, 0x200000, 3, &outside);
	grTexDownloadMipMap(0, 0x1000, 3, &tooLarge);
	grTexDownloadMipMap(0, 0x3000, 3, &rgb332);
	grTexSource(0, 0x200000, 3, &outside);
	triangle();
	d = swap();
	CHECK_INT(count_ops(d, GFX_OP_TEXIMAGE), 0);
	CHECK_INT(find_op(d, GFX_OP_TEXBIND)->payload[0], 0xFFFFFFFF);

	grTexFilterMode(0, 1, 1);
	grTexClampMode(0, 1, 1);
	grAlphaCombine(1, 0, 0, 0, 0);
	triangle();
	d = swap();
	CHECK(find_op(d, GFX_OP_TEXENABLE)->payload[0] == 0 && count_ops(d, GFX_OP_SAMPLER) == 0);
}

static void test_palette_textures(void)
{
	uint32_t palette[256] = {0};
	palette[1] = 0xFF00FF00; /* opaque green */
	palette[2] = 0x80FF0000; /* red at half alpha */
	uint8_t indices[4] = {1, 2, 2, 1};
	const TexInfo info = {7, 7, 3, 0x5, indices};
	grTexDownloadMipMap(0, 0x4000, 3, &info);
	grTexDownloadTable(0, 0x2, palette);
	grTexSource(0, 0x4000, 3, &info);
	grAlphaCombine(1, 0, 0, 1, 0);
	triangle();
	triangle();
	const Drained *d = swap();
	/* Expanded with the palette at the first draw, once. */
	CHECK_INT(count_ops(d, GFX_OP_TEXIMAGE), 1);
	const Op *image = find_op(d, GFX_OP_TEXIMAGE);
	CHECK(image->payload[0] == 0x4000 && image->payload[1] == 2);
	CHECK(image->payload[2] == 0xFF00FF00 && image->payload[3] == 0x80000080 && image->payload[4] == 0x80000080 && image->payload[5] == 0xFF00FF00);
	CHECK(find_op(d, GFX_OP_TEXIMAGE) < find_op(d, GFX_OP_TEXBIND));
	CHECK_INT(count_ops(d, GFX_OP_TRIS), 1);

	grTexDownloadTable(0, 0x2, palette); /* unchanged */
	grTexDownloadTable(0, 0x1, NULL);    /* not a palette: ignored */
	triangle();
	d = swap();
	CHECK_INT(count_ops(d, GFX_OP_TEXIMAGE), 0);

	palette[1] = 0xFFFFFFFF;
	grTexDownloadTable(0, 0x2, palette);
	triangle();
	d = swap();
	CHECK_INT(count_ops(d, GFX_OP_TEXIMAGE), 1);
	CHECK_INT(find_op(d, GFX_OP_TEXIMAGE)->payload[2], 0xFFFFFFFF);

	indices[0] = 2;
	grTexDownloadMipMap(0, 0x4000, 3, &info);
	triangle();
	d = swap();
	CHECK_INT(count_ops(d, GFX_OP_TEXIMAGE), 1);
	CHECK_INT(find_op(d, GFX_OP_TEXIMAGE)->payload[2], 0x80000080);

	grAlphaCombine(1, 0, 0, 0, 0);
	triangle();
	swap();
}

static void test_fog(void)
{
	uint8_t table[64];
	for (int i = 0; i < 64; ++i)
		table[i] = (uint8_t)(i * 4);
	grFogTable(table);
	grFogColorValue(0xFF336699);
	grFogMode(2 /* WITH_TABLE */);
	triangle();
	const Drained *d = swap();
	const int expected[] = {GFX_OP_FOGENABLE, GFX_OP_FOGCOLOR, GFX_OP_FOGTABLE, GFX_OP_TRIS, GFX_OP_SWAP};
	const int n = (int)(sizeof expected / sizeof *expected);
	CHECK_INT(d->opCount, n);
	for (int i = 0; i < n && i < d->opCount; ++i)
		CHECK_INT(d->ops[i].op, expected[i]);
	CHECK_INT(d->ops[0].payload[0], 1);
	CHECK_INT(d->ops[1].payload[0], 0x336699);
	CHECK_INT(d->ops[2].words, 1 + GFX_FOG_ENTRIES / 4);
	/* Table entry 4k sits at w = 2^k; the rest is interpolated in 1/w. */
	CHECK_INT(d->fog[0], 0);
	for (int k = 0; k < 16; ++k)
		CHECK_INT(d->fog[1 << k], table[4 * k]);
	CHECK_INT(d->fog[GFX_FOG_ENTRIES - 1], 252);
	bool monotonic = true;
	for (int w = 1; w < GFX_FOG_ENTRIES; ++w)
		monotonic &= d->fog[w] >= d->fog[w - 1];
	CHECK(monotonic);
	CHECK(d->fog[3] > d->fog[2] && d->fog[3] < d->fog[4]);

	grFogTable(table); /* unchanged: not sent again */
	triangle();
	d = swap();
	CHECK(count_ops(d, GFX_OP_FOGTABLE) == 0 && count_ops(d, GFX_OP_FOGENABLE) == 0 && count_ops(d, GFX_OP_FOGCOLOR) == 0);

	/* A table set while fog is off is sent when fog is used again. */
	grFogMode(0);
	table[63] = 255;
	grFogTable(table);
	triangle();
	d = swap();
	CHECK(find_op(d, GFX_OP_FOGENABLE)->payload[0] == 0 && count_ops(d, GFX_OP_FOGTABLE) == 0);
	grFogMode(2);
	grFogMode(1); /* iterated fog: unsupported, ignored */
	triangle();
	d = swap();
	CHECK(find_op(d, GFX_OP_FOGENABLE)->payload[0] == 1 && count_ops(d, GFX_OP_FOGTABLE) == 1);
	CHECK_INT(d->fog[GFX_FOG_ENTRIES - 1], 255);

	grFogMode(0);
	triangle();
	swap();
}

static void test_fog_generate(void)
{
	uint8_t table[64];
	guFogGenerateExp(table, 1.0f / 256.0f);
	CHECK_INT(table[63], 255);
	CHECK_INT(table[32], 161); /* w = 256: 255 * (1 - 1/e) */
	CHECK(table[0] <= 1);
	for (int i = 0; i < 63; ++i)
		CHECK(table[i] <= table[i + 1]);
	guFogGenerateExp(table, 0.0f);
	for (int i = 0; i < 64; ++i)
		CHECK_INT(table[i], 0);
}

static void test_overflow(void)
{
	/* Vertices: the triangle that does not fit starts a new slot. */
	int waits = s_waits, before = s_logCount;
	const int fit = GFX_VTX_CAPACITY / 3;
	for (int i = 0; i <= fit; ++i)
		triangle();
	const Drained *frame = swap();
	CHECK_INT(s_waits, waits + 1); /* the partial slot was still queued */
	CHECK_INT(s_logCount, before + 2);
	const Drained *partial = logged(1);
	CHECK_INT(partial->state, GFX_PARTIAL);
	CHECK_INT(partial->vtxCount, fit * 3);
	CHECK(partial->opCount == 1 && partial->ops[0].op == GFX_OP_TRIS && partial->ops[0].payload[1] == (uint32_t)fit * 3);
	CHECK_INT(partial->slot ^ 1, frame->slot);
	CHECK_INT(frame->vtxCount, 3);
	CHECK(frame->ops[0].op == GFX_OP_TRIS && frame->ops[0].payload[0] == 0 && frame->ops[0].payload[1] == 3);

	/* Commands: the largest texture images. */
	static const uint16_t texels[GFX_TEX_MAX_SIZE * GFX_TEX_MAX_SIZE];
	const TexInfo info = {0, 0, 3, 0xa, texels};
	const int images = GFX_CMD_CAPACITY / (3 + GFX_TEX_MAX_SIZE * GFX_TEX_MAX_SIZE);
	waits = s_waits;
	for (int i = 0; i <= images; ++i)
		grTexDownloadMipMap(0, 0, 3, &info);
	frame = swap();
	partial = logged(1);
	CHECK_INT(s_waits, waits + 1);
	CHECK_INT(partial->state, GFX_PARTIAL);
	CHECK_INT(count_ops(partial, GFX_OP_TEXIMAGE), images);
	CHECK_INT(count_ops(frame, GFX_OP_TEXIMAGE), 1);
}

static void test_back_pressure(void)
{
	const int waits = s_waits, before = s_logCount;
	triangle();
	grBufferSwap(1); /* not drained: the renderer is a frame behind */
	CHECK_INT(s_waits, waits);
	triangle();
	grBufferSwap(1); /* the next slot is still queued */
	CHECK_INT(s_waits, waits + 1);
	CHECK_INT(s_logCount, before + 2);
	CHECK(logged(1)->state == GFX_FRAME && logged(0)->state == GFX_FRAME);
	CHECK_INT(logged(1)->slot ^ 1, logged(0)->slot);
}

static void test_lfb(void)
{
	LfbInfo info;
	memset(&info, 0xAA, sizeof info);
	CHECK_INT(grLfbLock(0 /* READ_ONLY */, 1, 0, 0, 0, &info), 0);
	CHECK(info.lfbPtr == NULL && info.strideInBytes == 0);
	CHECK_INT(grLfbLock(1 /* WRITE_ONLY */, 1, 0, 0, 0, &info), 1);
	CHECK(info.lfbPtr != NULL && info.strideInBytes == 2);
}

int main(void)
{
	s_slots[0] = calloc(1, GFX_SLOT_BYTES);
	s_slots[1] = calloc(1, GFX_SLOT_BYTES);
	CHECK(s_slots[0] && s_slots[1]);
	if (!s_slots[0] || !s_slots[1])
		return check_exit();
	grSstWinOpen(0, 7, 0, 0, 0, 2, 1);
	test_triangles();
	test_lines();
	test_state();
	test_textures();
	test_palette_textures();
	test_fog();
	test_fog_generate();
	test_overflow();
	test_back_pressure();
	test_lfb();
	return check_exit();
}

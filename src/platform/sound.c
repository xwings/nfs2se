/* EAcsnd replacement. The game's software mixer renders 256-frame chunks of
 * 22050 Hz stereo int16 audio through the getSamples callback it registers.
 * A producer thread keeps the shared audio ring filled to a target level and
 * an AudioWorklet (src/web/audio-worklet.js) plays it, so audio never waits
 * for the browser main thread. Follows NFSIISE's EAcsnd.c (MIT, zaps166);
 * LinearSoundInterpolation is ignored because the browser resamples. */
#include "platform.h"
#include "web_layout.h"

#include <pthread.h>
#include <stdatomic.h>

#define EACSND_VERSION 0x60002
#define EACSND_CAPS 0x23E0F
#define PRODUCER_WAIT_MS 10

_Static_assert(AUD_CAPACITY_FRAMES % AUD_CHUNK_FRAMES == 0, "chunks never wrap around the ring");
_Static_assert(AUD_CHUNK_FRAMES <= AUD_TARGET_FRAMES && AUD_TARGET_FRAMES <= AUD_CAPACITY_FRAMES,
               "the producer reaches its target level without overwriting unplayed frames");

static GameFn s_getSamples, s_fadeInOut;
static bool s_started, s_serving, s_haveConsumer;
static atomic_bool s_producing;
static pthread_t s_producer;
static int16_t s_scratch[AUD_CHUNK_FRAMES * 2];

static int32_t ring_load(const int32_t *ring, int word)
{
	return __atomic_load_n(&ring[word], __ATOMIC_SEQ_CST);
}

static void ring_store(int32_t *ring, int word, int32_t value)
{
	__atomic_store_n(&ring[word], value, __ATOMIC_SEQ_CST);
}

static void render(int16_t *out)
{
	wrap_regparm2(audio_game_thread, s_getSamples, (int32_t)(uintptr_t)out, AUD_CHUNK_FRAMES);
}

static void *producer_main(void *param)
{
	(void)param;
	int32_t *ring = web_audio();
	int16_t *frames = (int16_t *)(ring + AUD_HEADER_WORDS);
	while (atomic_load(&s_producing))
	{
		const uint32_t write = (uint32_t)ring_load(ring, AUD_WRITE);
		const int32_t read = ring_load(ring, AUD_READ);
		if (!ring_load(ring, AUD_ACTIVE) || write + AUD_CHUNK_FRAMES - (uint32_t)read > AUD_TARGET_FRAMES)
		{
			web_futex_wait(&ring[AUD_READ], read, PRODUCER_WAIT_MS);
			continue;
		}
		render(frames + (write & (AUD_CAPACITY_FRAMES - 1)) * 2);
		ring_store(ring, AUD_WRITE, (int32_t)(write + AUD_CHUNK_FRAMES));
	}
	return NULL;
}

int32_t iSNDdllversion_(void)
{
	return EACSND_VERSION;
}

int32_t iSNDdirectsetfunctions_wrap(GameFn getSamples, GameFn unused1, GameFn unused2, GameFn fadeInOut, GameFn unused3)
{
	(void)unused1;
	(void)unused2;
	(void)unused3;
	s_getSamples = getSamples;
	s_fadeInOut = fadeInOut;
	return 0;
}

int32_t iSNDdirectcaps_(void *hWnd)
{
	(void)hWnd;
	return EACSND_CAPS;
}

int32_t iSNDdirectstart_(int32_t arg, void *hWnd)
{
	(void)arg;
	(void)hWnd;
	if (s_started)
		return 0;
	s_started = true;
	s_serving = false;
	s_haveConsumer = ring_load(web_audio(), AUD_CONSUMER) != 0;
	if (s_haveConsumer)
	{
		atomic_store(&s_producing, true);
		if (pthread_create(&s_producer, NULL, producer_main, NULL))
		{
			atomic_store(&s_producing, false);
			s_haveConsumer = false;
		}
	}
	return 0;
}

/* Called periodically by the game. Like an audio device that is unpaused on
 * the first call, the ring only starts playing once the game serves it.
 * Without a browser audio consumer the mixer is still advanced here. */
int32_t iSNDdirectserve_(void *game)
{
	if (!s_started)
		return 0;
	if (!s_serving && s_haveConsumer)
		ring_store(web_audio(), AUD_ACTIVE, 1);
	s_serving = true;
	s_fadeInOut(game);
	if (!s_haveConsumer)
		render(s_scratch);
	return 0;
}

int32_t iSNDdirectstop_(void)
{
	if (!s_started)
		return 0;
	s_started = false;
	int32_t *ring = web_audio();
	ring_store(ring, AUD_ACTIVE, 0); /* the worklet drops what is still queued */
	if (atomic_exchange(&s_producing, false))
	{
		web_futex_wake(&ring[AUD_READ]);
		pthread_join(s_producer, NULL);
	}
	return 0;
}

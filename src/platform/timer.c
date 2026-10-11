/* The game's periodic timer and the WinMM calls around it. The game sets its
 * period as 16.16 fixed-point milliseconds; each tick signals the game's timer
 * events. Same scheduling as NFSIISE's Timer.c (MIT, zaps166): deadlines
 * accumulate the period, and a tick more than a second late resynchronises. */
#include "platform.h"

#include <pthread.h>
#include <stdatomic.h>

#define FAST_PERIOD (10 << 16) /* ticks as fast as possible while set */

static pthread_t s_timer;
static atomic_bool s_running;

static uint32_t timer_period(void)
{
	return __atomic_load_n((uint32_t *)dword_5637A0, __ATOMIC_RELAXED);
}

static void signal_events(void)
{
	if (*dword_5637CC)
		SetEvent_wrap(*dword_5637CC);
	if (*dword_5637D8 && !__atomic_load_n((uint32_t *)dword_4DB1B0, __ATOMIC_RELAXED))
		SetEvent_wrap(*dword_5637D8);
	if (*dword_4DDA70)
		SetEvent_wrap(*dword_4DDA70);
}

static void *timer_main(void *param)
{
	(void)param;
	uint32_t deadline = 0, fraction = 0;
	uint32_t delay = 1;
	while (atomic_load(&s_running))
	{
		web_sleep_ms(delay);
		const uint32_t period = timer_period();
		if (period == FAST_PERIOD)
		{
			delay = 1;
		}
		else
		{
			fraction += period;
			deadline += fraction >> 16;
			fraction &= 0xFFFF;
			const int32_t remaining = (int32_t)(deadline - web_ticks()) - 1;
			delay = remaining < 1 ? 1 : (uint32_t)remaining;
			if (remaining < -1000)
				deadline = web_ticks() + 1;
		}
		signal_events();
	}
	return NULL;
}

/* The game never retries a timer that did not start, so that is fatal. */
int32_t startTimer(void)
{
	if (!atomic_exchange(&s_running, true) && pthread_create(&s_timer, NULL, timer_main, NULL))
		wrapper_fatal("Cannot start the game timer.");
	return 0;
}

int32_t stopTimer(void)
{
	if (atomic_exchange(&s_running, false))
		pthread_join(s_timer, NULL);
	return 0;
}

int32_t timeBeginPeriod_wrap(uint32_t period)
{
	(void)period;
	return 0;
}

int32_t timeEndPeriod_wrap(uint32_t period)
{
	(void)period;
	return 0;
}

int32_t timeGetDevCaps_wrap(void *caps, uint32_t size)
{
	(void)caps;
	(void)size;
	return 0;
}

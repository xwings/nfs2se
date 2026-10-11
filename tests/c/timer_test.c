/* timer.c: starting the game's timer thread. Built without -pthread, where
 * pthread_create always fails, so the failure path runs. */
#include "check.h"
#include "platform.h"

#include <setjmp.h>

int32_t startTimer(void);

void **dword_4DDA70, **dword_5637CC, **dword_5637D8;
void *dword_4DB1B0, *dword_5637A0;

static jmp_buf s_fatalExit;
static const char *s_fatalMessage;

_Noreturn void wrapper_fatal(const char *message)
{
	s_fatalMessage = message;
	longjmp(s_fatalExit, 1);
}

int32_t SetEvent_wrap(void *event)
{
	(void)event;
	return 1;
}

uint32_t web_ticks(void)
{
	return 0;
}

void web_sleep_ms(uint32_t ms)
{
	(void)ms;
}

/* The game never retries a timer that did not start, so the failure is fatal. */
static void test_start_failure(void)
{
	if (!setjmp(s_fatalExit))
	{
		startTimer();
		CHECK(!"startTimer returned after pthread_create failed");
	}
	CHECK(s_fatalMessage && *s_fatalMessage);
}

int main(void)
{
	test_start_failure();
	return check_exit();
}

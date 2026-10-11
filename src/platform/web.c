/* Browser integration: the memory regions shared with JS, futex waits, the
 * message box, timing and main(). With PROXY_TO_PTHREAD, main() and every game
 * thread run in workers, so blocking here never stalls the page; the browser
 * main thread (src/web/main.js) only exchanges data through the shared
 * regions and Atomics. */
#include "platform.h"
#include "web_layout.h"

#include <emscripten.h>
#include <emscripten/threading.h>
#include <limits.h>
#include <pthread.h>
#include <sched.h>
#include <time.h>
#include <unistd.h>

#define DIALOG_WAIT_MS 1000 /* re-checks CTL_DIALOG in case a wake is missed */

static int32_t s_ctl[CTL_WORDS];
static int32_t s_audio[AUD_HEADER_WORDS + AUD_CAPACITY_FRAMES] = {[AUD_RATE] = AUD_SAMPLE_RATE};
static _Alignas(16) uint8_t s_gfx[2][GFX_SLOT_BYTES];
static double s_tickBase;
static const char *s_saveDir;
static pthread_mutex_t s_dialogMutex = PTHREAD_MUTEX_INITIALIZER;

/* Byte address of a shared region inside the wasm memory, for JS views. */
EMSCRIPTEN_KEEPALIVE uintptr_t web_shared(int which)
{
	switch (which)
	{
	case SHARED_CTL:
		return (uintptr_t)s_ctl;
	case SHARED_GFX0:
		return (uintptr_t)s_gfx[0];
	case SHARED_GFX1:
		return (uintptr_t)s_gfx[1];
	case SHARED_AUDIO:
		return (uintptr_t)s_audio;
	}
	return 0;
}

int32_t *web_ctl(void)
{
	return s_ctl;
}

int32_t *web_audio(void)
{
	return s_audio;
}

uint8_t *web_gfx_slot(int index)
{
	return s_gfx[index & 1];
}

void web_futex_wait(int32_t *addr, int32_t expected, uint32_t ms)
{
	emscripten_futex_wait(addr, (uint32_t)expected, (double)ms);
}

void web_futex_wake(int32_t *addr)
{
	emscripten_futex_wake(addr, INT_MAX);
}

/* Shows the dialog on the browser main thread and blocks until it answers by
 * storing DIALOG_OK or DIALOG_CANCEL into CTL_DIALOG. Threads asking at the
 * same time get their dialogs one after another. */
int32_t web_message_box(const char *text, const char *caption, bool okCancel)
{
	pthread_mutex_lock(&s_dialogMutex);
	fprintf(stderr, "%s: %s\n", caption, text);
	ctl_store(CTL_DIALOG, 0);
	MAIN_THREAD_EM_ASM({ Module.nfsMessageBox(UTF8ToString($0), UTF8ToString($1), !!$2); }, text, caption, okCancel);
	int32_t answer;
	while (!(answer = ctl_load(CTL_DIALOG)))
		web_futex_wait(&s_ctl[CTL_DIALOG], 0, DIALOG_WAIT_MS);
	pthread_mutex_unlock(&s_dialogMutex);
	return answer;
}

const char *web_save_dir(void)
{
	return s_saveDir;
}

void web_mark_fs_dirty(void)
{
	ctl_store(CTL_FS_DIRTY, 1);
}

/* Milliseconds since main() started; emscripten_get_now() shares one time
 * origin across workers. */
uint32_t web_ticks(void)
{
	return (uint32_t)(int64_t)(emscripten_get_now() - s_tickBase);
}

void web_sleep_ms(uint32_t ms)
{
	if (!ms)
	{
		sched_yield();
		return;
	}
	const struct timespec duration = {ms / 1000, (long)(ms % 1000) * 1000000L};
	nanosleep(&duration, NULL);
}

/* main.js passes the game directory, which becomes the working directory,
 * and the persisted directory for settings and saves. */
int main(int argc, char **argv)
{
	s_tickBase = emscripten_get_now();
	if (argc < 3 || chdir(argv[1]))
	{
		web_message_box("The game files are missing. Reload the page and import them again.", GAME_TITLE, false);
		return 1;
	}
	s_saveDir = argv[2];
	nfs2seEntrypoint();
	return 0;
}

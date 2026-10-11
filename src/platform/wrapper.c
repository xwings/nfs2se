/* Wrapper entry points called by the game: initialisation, window creation,
 * exit procedures, SDL timing and libc shims. Covers the entry points of
 * NFSIISE's Wrapper.c (MIT, zaps166) on top of the browser platform. */
#include "platform.h"

#include <fcntl.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#define EXIT_GRACE_MS 2500 /* longest wait for each exit procedure, then for the window to close */
#define EXIT_POLL_MS 10
#define MAX_EXIT_PROCS 10

static GameFn s_exitProcs[MAX_EXIT_PROCS];
static uint32_t s_exitProcCount;
static atomic_bool s_exitProcDone;

_Noreturn void wrapper_fatal(const char *message)
{
	web_message_box(message, GAME_TITLE, false);
	exit(-1);
}

/* Exit procedures */

int32_t WrapperAtExit(GameFn proc)
{
	if (s_exitProcCount < MAX_EXIT_PROCS)
		s_exitProcs[s_exitProcCount++] = proc;
	return 0;
}

static bool before(uint32_t deadline)
{
	return (int32_t)(deadline - web_ticks()) > 0;
}

/* Ends the process when the running exit procedure hangs. */
static void *exit_watchdog(void *param)
{
	(void)param;
	const uint32_t deadline = web_ticks() + EXIT_GRACE_MS;
	while (!atomic_load(&s_exitProcDone) && before(deadline))
		web_sleep_ms(EXIT_POLL_MS);
	if (!atomic_load(&s_exitProcDone))
		exit(0);
	return NULL;
}

static void run_exit_proc(GameFn proc)
{
	atomic_store(&s_exitProcDone, false);
	pthread_t watchdog;
	const bool watched = pthread_create(&watchdog, NULL, exit_watchdog, NULL) == 0;
	proc(main_game_thread);
	atomic_store(&s_exitProcDone, true);
	if (watched)
		pthread_join(watchdog, NULL);
}

/* Runs each registered procedure once; one that exits again finds none left.
 * Then the message loop gets time to handle the game's WM_DESTROY. */
void wrapper_exit_procs(void)
{
	const uint32_t count = s_exitProcCount;
	s_exitProcCount = 0;
	for (uint32_t i = 0; i < count; ++i)
		run_exit_proc(s_exitProcs[i]);

	const uint32_t deadline = web_ticks() + EXIT_GRACE_MS;
	while (user32_window_alive() && before(deadline))
		web_sleep_ms(EXIT_POLL_MS);
}

/* Initialisation */

/* Copies src to dst; a failed copy leaves no partial dst behind. */
static void copy_file(const char *src, const char *dst)
{
	FILE *in = fopen(src, "rb");
	if (!in)
		return;
	FILE *out = fopen(dst, "wb");
	bool ok = out != NULL;
	char buffer[4096];
	size_t n;
	while (ok && (n = fread(buffer, 1, sizeof buffer, in)) > 0)
		ok = fwrite(buffer, 1, n, out) == n;
	ok = ok && !ferror(in);
	if (out)
	{
		ok = fclose(out) == 0 && ok;
		if (ok)
			web_mark_fs_dirty();
		else
			unlink(dst);
	}
	fclose(in);
}

/* Creates the settings tree in the persisted directory on first run, seeds
 * nfs2se.conf from the template shipped with the support files and parses it.
 * Without a usable settings file the game falls back to ./nfs2se.conf and
 * keeps user data in the game directory. */
int32_t WrapperInit(void)
{
	static const char *const subdirs[] = {"config", "save", "stats", "stats/prh", "tmptrk"};

	printf("Need for Speed II SE (game v%s) on WebAssembly\n", (const char *)binaryGameVersion);
	config_reset();

	const char *dir = web_save_dir();
	char path[MAX_PATH];
	mkdir(dir, 0755);
	for (size_t i = 0; i < sizeof subdirs / sizeof *subdirs; ++i)
	{
		snprintf(path, sizeof path, "%s/%s", dir, subdirs[i]);
		mkdir(path, 0755);
	}
	snprintf(path, sizeof path, "%s/nfs2se.conf", dir);
	if (access(path, F_OK) != 0)
		copy_file("nfs2se.conf.template", path);

	FILE *conf = fopen(path, "r");
	if (conf)
		config_set_settings_dir(dir);
	else
		conf = fopen("nfs2se.conf", "r");
	if (conf)
	{
		config_parse(conf);
		fclose(conf);
	}
	else
	{
		fprintf(stderr, "Cannot open configuration file \"nfs2se.conf\", using defaults\n");
	}

	ctl_store(CTL_CFG_KEEP_ASPECT, g_config.keepAspectRatio);
	ctl_store(CTL_CFG_LINEAR_FILTER, g_config.linearFiltering);
	return 0;
}

int32_t WrapperCreateWindow(GameFn windowProc)
{
	const char *error = config_check_game_dirs();
	if (error)
		wrapper_fatal(error);
	return user32_open_window(windowProc);
}

/* SDL replacements */

int32_t SDL_NumJoysticks_wrap(void)
{
	return CTL_PAD_COUNT;
}

int32_t SDL_GetTicks_wrap(void)
{
	return (int32_t)web_ticks();
}

int32_t SDL_Delay_wrap(uint32_t ms)
{
	web_sleep_ms(ms);
	return 0;
}

/* libc shims */

int32_t vsprintf_wrap(char *out, const char *fmt, const uint8_t *args)
{
	return fmt_x86_vsprintf(out, fmt, args);
}

int32_t fscanf_wrap(FILE *f, const char *fmt, ...)
{
	va_list args;
	va_start(args, fmt);
	const int ret = vfscanf(f, fmt, args);
	va_end(args);
	return ret;
}

int32_t fopen_wrap(const char *name, const char *mode)
{
	return (int32_t)(uintptr_t)config_fopen(name, mode);
}

int32_t fclose_wrap(FILE *f)
{
	const int flags = fcntl(fileno(f), F_GETFL);
	if (flags >= 0 && (flags & O_ACCMODE) != O_RDONLY)
		web_mark_fs_dirty();
	return fclose(f);
}

int32_t malloc_wrap(uint32_t size)
{
	return (int32_t)(uintptr_t)malloc(size);
}

int32_t calloc_wrap(uint32_t count, uint32_t size)
{
	return (int32_t)(uintptr_t)calloc(count, size);
}

int32_t free_wrap(void *ptr)
{
	free(ptr);
	return 0;
}

/* time_t is 64-bit in Emscripten; the game stores 32 bits. */
int32_t time_wrap(int32_t *out)
{
	const int32_t now = (int32_t)time(NULL);
	if (out)
		*out = now;
	return now;
}

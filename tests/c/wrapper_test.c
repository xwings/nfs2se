/* wrapper.c: seeding and reading the persisted settings, the game's file
 * shims and the exit procedures. Built with -pthread: each exit procedure
 * runs under a watchdog thread. */
#include "check.h"
#include "platform.h"

#include <emscripten.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <unistd.h>

#define SAVE_DIR "/home/web_user/.nfs2se"
#define SETTINGS_FILE SAVE_DIR "/nfs2se.conf"
#define TEMPLATE_FILE "nfs2se.conf.template"
#define GRACE_MS 2500

int32_t WrapperInit(void);
int32_t WrapperAtExit(GameFn proc);
int32_t fopen_wrap(const char *name, const char *mode);
int32_t fclose_wrap(FILE *f);

void *main_game_thread;
void *binaryGameVersion = (void *)"test";

static int32_t s_ctl[CTL_WORDS];
static int s_windowChecks; /* user32_window_alive answers true this many more times */

int32_t *web_ctl(void)
{
	return s_ctl;
}

const char *web_save_dir(void)
{
	return SAVE_DIR;
}

void web_mark_fs_dirty(void)
{
	ctl_store(CTL_FS_DIRTY, 1);
}

/* emscripten_get_now() counts from the epoch in pthread builds; the 64-bit
 * step wraps the count like web.c's tick counter instead of saturating. */
uint32_t web_ticks(void)
{
	return (uint32_t)(uint64_t)emscripten_get_now();
}

void web_sleep_ms(uint32_t ms)
{
	usleep(ms * 1000);
}

int32_t web_message_box(const char *text, const char *caption, bool okCancel)
{
	(void)text;
	(void)caption;
	(void)okCancel;
	return DIALOG_OK;
}

int32_t user32_open_window(GameFn wndProc)
{
	(void)wndProc;
	return 1;
}

bool user32_window_alive(void)
{
	return s_windowChecks-- > 0;
}

int32_t fmt_x86_vsprintf(char *out, const char *fmt, const uint8_t *args)
{
	(void)fmt;
	(void)args;
	*out = '\0';
	return 0;
}

static void write_file(const char *path, const char *text)
{
	FILE *f = fopen(path, "w");
	CHECK(f != NULL);
	if (f)
	{
		fputs(text, f);
		fclose(f);
	}
}

static const char *read_file(const char *path)
{
	static char text[256];
	text[0] = '\0';
	FILE *f = fopen(path, "r");
	if (f)
	{
		text[fread(text, 1, sizeof text - 1, f)] = '\0';
		fclose(f);
	}
	return text;
}

static bool is_directory(const char *path)
{
	struct stat st;
	return stat(path, &st) == 0 && S_ISDIR(st.st_mode);
}

static void check_path(const char *gamePath, const char *expected)
{
	char *path = config_convert_path(gamePath, true);
	CHECK_STR(path, expected);
	free(path);
}

/* First run: the template seeds the settings file, which is then used. */
static void test_init_seeds_settings(void)
{
	write_file(TEMPLATE_FILE, "KeepAspectRatio=0\n");
	WrapperInit();
	CHECK_STR(read_file(SETTINGS_FILE), "KeepAspectRatio=0\n");
	CHECK_INT(ctl_load(CTL_FS_DIRTY), 1);
	CHECK_INT(ctl_load(CTL_CFG_KEEP_ASPECT), 0);
	CHECK_INT(ctl_load(CTL_CFG_LINEAR_FILTER), 1);
	CHECK(is_directory(SAVE_DIR "/config"));
	CHECK(is_directory(SAVE_DIR "/save"));
	CHECK(is_directory(SAVE_DIR "/stats/prh"));
	CHECK(is_directory(SAVE_DIR "/tmptrk"));
	check_path(".\\fedata\\pc\\save\\slot0", SAVE_DIR "/save/slot0");
}

/* Later runs keep the user's settings file. */
static void test_init_keeps_settings(void)
{
	write_file(SETTINGS_FILE, "LinearTextureFiltering=0\n");
	ctl_store(CTL_FS_DIRTY, 0);
	WrapperInit();
	CHECK_STR(read_file(SETTINGS_FILE), "LinearTextureFiltering=0\n");
	CHECK_INT(ctl_load(CTL_FS_DIRTY), 0);
	CHECK_INT(ctl_load(CTL_CFG_KEEP_ASPECT), 1);
	CHECK_INT(ctl_load(CTL_CFG_LINEAR_FILTER), 0);
}

/* A file the game writes marks the file system dirty when closed. */
static void test_files(void)
{
	const char *name = ".\\fedata\\pc\\config\\Test.cfg";
	FILE *f = (FILE *)(uintptr_t)fopen_wrap(name, "w");
	CHECK(f != NULL);
	ctl_store(CTL_FS_DIRTY, 0);
	CHECK_INT(fclose_wrap(f), 0);
	CHECK_INT(ctl_load(CTL_FS_DIRTY), 1);
	CHECK(access(SAVE_DIR "/config/Test.cfg", F_OK) == 0);

	f = (FILE *)(uintptr_t)fopen_wrap(name, "r");
	CHECK(f != NULL);
	ctl_store(CTL_FS_DIRTY, 0);
	CHECK_INT(fclose_wrap(f), 0);
	CHECK_INT(ctl_load(CTL_FS_DIRTY), 0);
}

/* A template that cannot be read leaves no partial settings file, so the game
 * runs on defaults instead of an empty or cut configuration. */
static void test_init_failed_copy(void)
{
	unlink(SETTINGS_FILE);
	unlink(TEMPLATE_FILE);
	EM_ASM(FS.createDevice('/', 'nfs2se.conf.template', function() { throw new Error('read error'); }, null););
	config_set_settings_dir(NULL);
	ctl_store(CTL_FS_DIRTY, 0);
	WrapperInit();
	CHECK(access(SETTINGS_FILE, F_OK) != 0);
	CHECK_INT(ctl_load(CTL_FS_DIRTY), 0);
	CHECK_INT(ctl_load(CTL_CFG_LINEAR_FILTER), 1);
	check_path(".\\fedata\\pc\\save\\slot0", "./fedata/pc/save/slot0");
}

static char s_procLog[64];
static int s_reentries;

static void proc_a(void *game)
{
	CHECK(game == main_game_thread);
	strcat(s_procLog, "a");
}

/* Exits again from inside an exit procedure. */
static void proc_b(void *game)
{
	(void)game;
	strcat(s_procLog, "b");
	if (s_reentries++ == 0)
		wrapper_exit_procs();
}

static void test_exit_procs(void)
{
	main_game_thread = &s_reentries;
	WrapperAtExit(proc_a);
	WrapperAtExit(proc_b);
	for (int i = 0; i < 9; ++i)
		WrapperAtExit(proc_a); /* only 10 procedures are kept */

	/* Each runs once in order, also when one of them exits again. */
	s_windowChecks = 0;
	wrapper_exit_procs();
	CHECK_STR(s_procLog, "abaaaaaaaa");

	/* A later call runs none again but still waits for the window to close. */
	s_windowChecks = 3;
	wrapper_exit_procs();
	CHECK_STR(s_procLog, "abaaaaaaaa");
	CHECK_INT(s_windowChecks, -1);

	/* A window that stays open is given up on after the grace period. */
	s_windowChecks = 1000000;
	const double start = emscripten_get_now();
	wrapper_exit_procs();
	const double waited = emscripten_get_now() - start;
	CHECK(waited >= GRACE_MS - 50 && waited < 4 * GRACE_MS);
	CHECK(s_windowChecks < 1000000 - 1);
}

int main(void)
{
	test_init_seeds_settings();
	test_init_keeps_settings();
	test_files();
	test_init_failed_copy();
	test_exit_procs();
	return check_exit();
}

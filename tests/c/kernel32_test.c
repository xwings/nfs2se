/* kernel32.c: events, files, mapped views and directory searches. Built with
 * -pthread (Makefile TEST_CFLAGS_kernel32) so an event can wake another thread. */
#include "check.h"
#include "platform.h"

#include <emscripten.h>
#include <pthread.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

/* Imported by the game, which declares them with 32-bit integer arguments. */
int32_t CreateEventA_wrap(int32_t attributes, int32_t manualReset, int32_t initialState, int32_t name);
int32_t WaitForMultipleObjects_wrap(int32_t count, int32_t handles, int32_t waitAll, int32_t milliseconds);
int32_t CloseHandle_wrap(int32_t handle);
int32_t CreateFileA_wrap(int32_t name, int32_t access, int32_t share, int32_t attributes, int32_t disposition,
                         int32_t flags, int32_t templateFile);
int32_t GetFileSize_wrap(int32_t file, int32_t sizeHigh);
int32_t ReadFile_wrap(int32_t file, int32_t buffer, int32_t toRead, int32_t bytesRead, int32_t overlapped);
int32_t WriteFile_wrap(int32_t file, int32_t buffer, int32_t toWrite, int32_t bytesWritten, int32_t overlapped);
int32_t SetFilePointer_wrap(int32_t file, int32_t distance, int32_t distanceHigh, int32_t moveMethod);
int32_t SetEndOfFile_wrap(int32_t file);
int32_t GetLastError_wrap(void);
int32_t CreateFileMappingA_wrap(int32_t file, int32_t attributes, int32_t protect, int32_t sizeHigh, int32_t sizeLow,
                                int32_t name);
int32_t MapViewOfFile_wrap(int32_t mapping, int32_t access, int32_t offsetHigh, int32_t offsetLow, int32_t bytes);
int32_t UnmapViewOfFile_wrap(int32_t view);
int32_t DeleteFileA_wrap(int32_t name);
int32_t SetCurrentDirectoryA_wrap(int32_t name);
int32_t GetCurrentDirectoryA_wrap(int32_t length, int32_t buffer);
int32_t FindFirstFileA_wrap(int32_t pattern, int32_t data);
int32_t FindNextFileA_wrap(int32_t find, int32_t data);
int32_t FindClose_wrap(int32_t find);

#define ARG(p) ((int32_t)(uintptr_t)(p))

enum
{
	INVALID_HANDLE = -1,
	WAIT_TIMEOUT = 0x102,
	INFINITE = -1,
	GENERIC_READ = (int32_t)0x80000000u,
	GENERIC_WRITE = 0x40000000,
	FILE_BEGIN = 0,
	FILE_END = 2,
	ERROR_FILE_NOT_FOUND = 2,
	FIND_DATA_BYTES = 320,
	FIND_NAME_OFFSET = 44, /* WIN32_FIND_DATAA.cFileName */
};

static int s_dirtyMarks;

void web_mark_fs_dirty(void)
{
	++s_dirtyMarks;
}

void wrapper_exit_procs(void)
{
}

void wrapper_fatal(const char *message)
{
	fprintf(stderr, "fatal: %s\n", message);
	exit(1);
}

static uint32_t now_ms(void)
{
	struct timespec t;
	clock_gettime(CLOCK_MONOTONIC, &t);
	return (uint32_t)(t.tv_sec * 1000 + t.tv_nsec / 1000000);
}

static int32_t wait_events(const int32_t *handles, int count, int32_t milliseconds)
{
	return WaitForMultipleObjects_wrap(count, ARG(handles), 0, milliseconds);
}

static void test_events(void)
{
	const int32_t handles[2] = {CreateEventA_wrap(0, 0, 0, 0), CreateEventA_wrap(0, 0, 0, 0)};
	CHECK(handles[0] != 0 && handles[1] != 0);
	CHECK_INT(wait_events(handles, 2, 0), WAIT_TIMEOUT);

	CHECK_INT(SetEvent_wrap((void *)(uintptr_t)handles[1]), 1);
	CHECK_INT(wait_events(handles, 2, 0), 1);
	CHECK_INT(wait_events(handles, 2, 0), WAIT_TIMEOUT); /* auto-reset */

	/* Win32 acquires only the lowest signalled event; the other stays set. */
	SetEvent_wrap((void *)(uintptr_t)handles[0]);
	SetEvent_wrap((void *)(uintptr_t)handles[1]);
	CHECK_INT(wait_events(handles, 2, 0), 0);
	CHECK_INT(wait_events(handles, 2, 0), 1);
	CHECK_INT(wait_events(handles, 2, 0), WAIT_TIMEOUT);

	const int32_t manual = CreateEventA_wrap(0, 1, 1, 0);
	CHECK_INT(wait_events(&manual, 1, 0), 0);
	CHECK_INT(wait_events(&manual, 1, 0), 0); /* manual reset: stays set */
	CHECK_INT(SetEvent_wrap(NULL), 0);

	const uint32_t start = now_ms();
	CHECK_INT(wait_events(handles, 2, 40), WAIT_TIMEOUT);
	CHECK(now_ms() - start >= 40);

	CHECK_INT(CloseHandle_wrap(handles[0]), 1);
	CHECK_INT(CloseHandle_wrap(handles[1]), 1);
	CHECK_INT(CloseHandle_wrap(manual), 1);
}

typedef struct
{
	int32_t handles[2];
	int32_t milliseconds;
	int32_t result;
} Waiter;

static void *waiter_main(void *param)
{
	Waiter *waiter = param;
	waiter->result = wait_events(waiter->handles, 2, waiter->milliseconds);
	return NULL;
}

/* A wait blocks until another thread sets one of its events. */
static void test_event_wakes_waiter(int32_t milliseconds)
{
	Waiter waiter = {{CreateEventA_wrap(0, 0, 0, 0), CreateEventA_wrap(0, 0, 0, 0)}, milliseconds, -2};
	pthread_t thread;
	CHECK_INT(pthread_create(&thread, NULL, waiter_main, &waiter), 0);
	const uint32_t start = now_ms();
	usleep(30 * 1000);
	SetEvent_wrap((void *)(uintptr_t)waiter.handles[1]);
	pthread_join(thread, NULL);
	CHECK_INT(waiter.result, 1);
	CHECK(now_ms() - start < 5000);
	CloseHandle_wrap(waiter.handles[0]);
	CloseHandle_wrap(waiter.handles[1]);
}

static int32_t open_file(const char *name, int32_t access)
{
	return CreateFileA_wrap(ARG(name), access, 0, 0, 0, 0, 0);
}

static void test_files(void)
{
	static const uint8_t data[10] = {1, 2, 3, 4, 5, 6, 7, 8, 9, 10};
	uint8_t buffer[32];
	uint32_t done, high = 99;

	s_dirtyMarks = 0;
	int32_t file = open_file(".\\Data.BIN", GENERIC_WRITE);
	CHECK(file != INVALID_HANDLE);
	CHECK_INT(WriteFile_wrap(file, ARG(data), sizeof data, ARG(&done), 0), 1);
	CHECK_INT(done, sizeof data);
	CHECK_INT(CloseHandle_wrap(file), 1);
	CHECK_INT(s_dirtyMarks, 1);

	file = open_file(".\\DATA.bin", GENERIC_READ); /* names are lowered */
	CHECK(file != INVALID_HANDLE);
	CHECK_INT(GetFileSize_wrap(file, ARG(&high)), sizeof data);
	CHECK_INT(high, 0);
	CHECK_INT(ReadFile_wrap(file, ARG(buffer), 4, ARG(&done), 0), 1);
	CHECK_INT(done, 4);
	CHECK(memcmp(buffer, data, 4) == 0);
	CHECK_INT(SetFilePointer_wrap(file, 2, 0, FILE_BEGIN), 2);
	CHECK_INT(ReadFile_wrap(file, ARG(buffer), sizeof buffer, ARG(&done), 0), 0); /* short read fails */
	CHECK_INT(done, 8);
	CHECK(memcmp(buffer, data + 2, 8) == 0);
	CHECK_INT(SetFilePointer_wrap(file, -3, 0, FILE_END), 7);
	CHECK_INT(CloseHandle_wrap(file), 1);
	CHECK_INT(s_dirtyMarks, 1); /* reading changes nothing to persist */

	file = open_file(".\\data.bin", GENERIC_WRITE); /* truncates */
	WriteFile_wrap(file, ARG(data), sizeof data, ARG(&done), 0);
	CHECK_INT(SetFilePointer_wrap(file, 4, 0, FILE_BEGIN), 4);
	CHECK_INT(SetEndOfFile_wrap(file), 1);
	CloseHandle_wrap(file);
	struct stat st;
	CHECK_INT(stat("data.bin", &st), 0);
	CHECK_INT(st.st_size, 4);

	CHECK_INT(open_file(".\\data.bin", GENERIC_READ | GENERIC_WRITE), INVALID_HANDLE);
	CHECK_INT(open_file(".\\missing.bin", GENERIC_READ), INVALID_HANDLE);
	CHECK_INT(GetLastError_wrap(), ERROR_FILE_NOT_FOUND);
	CHECK_INT(open_file("\\\\.\\COM1", GENERIC_READ), INVALID_HANDLE);
	CHECK_INT(CloseHandle_wrap(INVALID_HANDLE), 0);
}

static int32_t map_file(const char *name, int32_t *file, int32_t *mapping)
{
	*file = open_file(name, GENERIC_READ);
	CHECK(*file != INVALID_HANDLE);
	*mapping = CreateFileMappingA_wrap(*file, 0, 0, 0, 0, 0);
	CHECK(*mapping != 0);
	return MapViewOfFile_wrap(*mapping, 0, 0, 0, 0);
}

static void unmap_file(int32_t file, int32_t mapping)
{
	CHECK_INT(CloseHandle_wrap(mapping), 1);
	CHECK_INT(CloseHandle_wrap(file), 1);
}

static void test_mapping(void)
{
	uint8_t data[1000];
	for (size_t i = 0; i < sizeof data; ++i)
		data[i] = (uint8_t)(i * 13);
	FILE *f = fopen("map.bin", "wb");
	fwrite(data, 1, sizeof data, f);
	fclose(f);

	int32_t file, mapping;
	const int32_t view = map_file(".\\MAP.BIN", &file, &mapping);
	CHECK(view != 0);
	if (view)
		CHECK(memcmp((const void *)(uintptr_t)view, data, sizeof data) == 0);
	CHECK_INT(UnmapViewOfFile_wrap(view), 1);
	unmap_file(file, mapping);

	fclose(fopen("empty.bin", "wb"));
	CHECK_INT(map_file(".\\empty.bin", &file, &mapping), 0);
	unmap_file(file, mapping);

	/* A file that delivers fewer bytes than its size has no view. */
	EM_ASM({
		var node = FS.lookupPath('/k32/map.bin').node;
		var getattr = node.node_ops.getattr;
		node.node_ops = Object.assign({}, node.node_ops, {
			getattr: function(n) { var attr = Object.assign({}, getattr(n)); attr.size += 16; return attr; }
		});
	});
	CHECK_INT(map_file(".\\map.bin", &file, &mapping), 0);
	unmap_file(file, mapping);
}

static void test_delete(void)
{
	s_dirtyMarks = 0;
	CHECK_INT(DeleteFileA_wrap(ARG(".\\DATA.BIN")), 1);
	CHECK_INT(s_dirtyMarks, 1);
	CHECK_INT(DeleteFileA_wrap(ARG(".\\data.bin")), 0);
	CHECK_INT(s_dirtyMarks, 1);
}

static void test_directories(void)
{
	char cwd[MAX_PATH];
	CHECK_INT(mkdir("Tracks", 0777), 0);
	CHECK_INT(SetCurrentDirectoryA_wrap(ARG(".\\Tracks")), 1); /* names keep their case */
	CHECK(GetCurrentDirectoryA_wrap(sizeof cwd, ARG(cwd)) != 0);
	CHECK_STR(cwd, "/k32/Tracks");
	CHECK_INT(SetCurrentDirectoryA_wrap(ARG(".\\Missing")), 0);

	static const char *const files[] = {"a.trk", "B.TRK", "c.txt", ".hidden.trk"};
	for (size_t i = 0; i < sizeof files / sizeof *files; ++i)
		fclose(fopen(files[i], "w"));
	CHECK_INT(mkdir("dir.trk", 0777), 0);

	uint8_t data[FIND_DATA_BYTES];
	memset(data, 0xAA, sizeof data);
	const int32_t find = FindFirstFileA_wrap(ARG("*.trk"), ARG(data));
	CHECK(find != INVALID_HANDLE);
	bool zeroed = true;
	for (size_t i = 0; i < FIND_NAME_OFFSET; ++i)
		zeroed &= data[i] == 0;
	CHECK(zeroed);
	char first[MAX_PATH];
	snprintf(first, sizeof first, "%s", (const char *)data + FIND_NAME_OFFSET);
	CHECK_INT(FindNextFileA_wrap(find, ARG(data)), 1);
	const char *second = (const char *)data + FIND_NAME_OFFSET;
	CHECK((!strcmp(first, "a.trk") && !strcmp(second, "B.TRK")) || (!strcmp(first, "B.TRK") && !strcmp(second, "a.trk")));
	CHECK_INT(FindNextFileA_wrap(find, ARG(data)), 0);
	CHECK_INT(FindClose_wrap(find), 1);

	CHECK_INT(FindFirstFileA_wrap(ARG("*.xyz"), ARG(data)), INVALID_HANDLE);
	CHECK_INT(FindClose_wrap(INVALID_HANDLE), 0);
	CHECK_INT(SetCurrentDirectoryA_wrap(ARG("..")), 1);
}

int main(void)
{
	CHECK_INT(mkdir("/k32", 0777), 0);
	CHECK_INT(chdir("/k32"), 0);
	config_set_settings_dir(NULL);

	test_events();
	test_event_wakes_waiter(INFINITE);
	test_event_wakes_waiter(10000);
	test_files();
	test_mapping();
	test_delete();
	test_directories();
	return check_exit();
}

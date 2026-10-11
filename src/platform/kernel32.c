/* Kernel32 subset used by the game: threads, critical sections, events and
 * file I/O over POSIX, covering the same calls as NFSIISE's Kernel32.c (MIT,
 * zaps166). Serial ports are not available in the browser, so COM handles
 * fail to open and the overlapped I/O paths are never reached. */
#include "platform.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <fnmatch.h>
#include <pthread.h>
#include <semaphore.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#define INVALID_HANDLE ((void *)-1)
#define WAIT_TIMEOUT 0x102
#define INFINITE 0xFFFFFFFFu
#define ERROR_FILE_NOT_FOUND 2
#define CREATE_SUSPENDED 0x4
#define GENERIC_WRITE 0x40000000u
#define GENERIC_READ 0x80000000u
#define THREAD_STACK_SIZE (4 << 20)
#define VIEW_SLACK 4 /* zeroed bytes after a mapped view, so a 32-bit read of its last byte stays inside */

typedef enum { HANDLE_THREAD = 1, HANDLE_FILE, HANDLE_MAPPING, HANDLE_EVENT } HandleType;

typedef struct
{
	HandleType type;
	GameFn func;
	void *arg;
	sem_t resume;
	atomic_int refs; /* the handle and the not yet started thread */
} Thread;

typedef struct
{
	HandleType type;
	int fd;
	bool writable;
} File;

typedef struct
{
	HandleType type;
	int fd;
} Mapping;

typedef struct
{
	HandleType type;
	bool manualReset;
	bool set;
} Event;

/* Win32 structure layouts as seen by the 32-bit game. */
typedef struct
{
	void *reserved0[3];
	pthread_mutex_t *mutex;
	void *reserved1[2];
} CriticalSection;
_Static_assert(sizeof(CriticalSection) == 24, "CRITICAL_SECTION is 24 bytes");

typedef struct
{
	uint32_t length, memoryLoad, totalPhys, availPhys, totalPageFile, availPageFile, totalVirtual, availVirtual;
} MemoryStatus;

typedef struct
{
	uint32_t oemId, pageSize, minAddress, maxAddress, activeProcessorMask, numberOfProcessors, processorType,
		allocationGranularity;
	uint16_t processorLevel, processorRevision;
} SystemInfo;
_Static_assert(sizeof(SystemInfo) == 36, "SYSTEM_INFO is 36 bytes");

typedef struct
{
	uint32_t fileAttributes;
	uint32_t times[6];
	uint32_t fileSizeHigh, fileSizeLow;
	uint32_t reserved[2];
	char fileName[MAX_PATH];
	char alternateFileName[14];
} FindData;
_Static_assert(sizeof(FindData) == 320, "WIN32_FIND_DATAA is 320 bytes");

typedef struct
{
	DIR *dir;
	char *pattern;
} FindFile;

static pthread_mutex_t s_eventMutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t s_eventCond = PTHREAD_COND_INITIALIZER;

/* Threads */

static void thread_release(Thread *thread)
{
	if (atomic_fetch_sub(&thread->refs, 1) == 1)
	{
		sem_destroy(&thread->resume);
		free(thread);
	}
}

static void *thread_main(void *param)
{
	Thread *thread = param;
	sem_wait(&thread->resume);
	GameFn func = thread->func;
	void *arg = thread->arg;
	thread_release(thread);
	func(arg);
	return NULL;
}

/* The game passes a freshly constructed Game context as the parameter, and the
 * start routine is a translated function taking that context. */
int32_t CreateThread_wrap(void *attributes, uint32_t stackSize, GameFn start, void *param, uint32_t flags, uint32_t *threadId)
{
	(void)attributes;
	(void)stackSize; /* sizes the game's emulated x86 stack, which it allocates itself */
	Thread *thread = malloc(sizeof *thread);
	if (!thread)
		return 0;
	thread->type = HANDLE_THREAD;
	thread->func = start;
	thread->arg = param;
	atomic_init(&thread->refs, 2);
	sem_init(&thread->resume, 0, (flags & CREATE_SUSPENDED) ? 0 : 1);

	pthread_attr_t attr;
	pthread_attr_init(&attr);
	pthread_attr_setstacksize(&attr, THREAD_STACK_SIZE);
	pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
	pthread_t pt;
	const int err = pthread_create(&pt, &attr, thread_main, thread);
	pthread_attr_destroy(&attr);
	if (err)
	{
		sem_destroy(&thread->resume);
		free(thread);
		return 0;
	}
	if (threadId)
		*threadId = (uint32_t)(uintptr_t)pt;
	return (int32_t)(uintptr_t)thread;
}

int32_t ResumeThread_wrap(Thread *thread)
{
	sem_post(&thread->resume);
	return 0;
}

int32_t SetThreadPriority_wrap(void *thread, int32_t priority)
{
	(void)thread;
	(void)priority;
	return 0;
}

int32_t GetCurrentThreadId_wrap(void)
{
	return (int32_t)(uintptr_t)pthread_self();
}

int32_t GetCurrentThread_wrap(void)
{
	return 0;
}

int32_t TerminateThread_wrap(void *thread, uint32_t exitCode)
{
	(void)thread;
	(void)exitCode;
	return 0;
}

int32_t DuplicateHandle_wrap(void *srcProcess, void *src, void *dstProcess, void **dst, uint32_t access, int32_t inherit, uint32_t options)
{
	(void)srcProcess;
	(void)src;
	(void)dstProcess;
	(void)access;
	(void)inherit;
	(void)options;
	*dst = NULL;
	return 0;
}

int32_t GetCurrentProcess_wrap(void)
{
	return 0;
}

/* Critical sections are recursive on Windows. */

int32_t InitializeCriticalSection_wrap(CriticalSection *cs)
{
	pthread_mutexattr_t attr;
	pthread_mutexattr_init(&attr);
	pthread_mutexattr_settype(&attr, PTHREAD_MUTEX_RECURSIVE);
	cs->mutex = malloc(sizeof *cs->mutex);
	if (!cs->mutex)
		wrapper_fatal("Out of memory.");
	pthread_mutex_init(cs->mutex, &attr);
	pthread_mutexattr_destroy(&attr);
	return 0;
}

int32_t EnterCriticalSection_wrap(CriticalSection *cs)
{
	pthread_mutex_lock(cs->mutex);
	return 0;
}

int32_t LeaveCriticalSection_wrap(CriticalSection *cs)
{
	pthread_mutex_unlock(cs->mutex);
	return 0;
}

int32_t DeleteCriticalSection_wrap(CriticalSection *cs)
{
	pthread_mutex_destroy(cs->mutex);
	free(cs->mutex);
	cs->mutex = NULL;
	return 0;
}

/* Events */

int32_t CreateEventA_wrap(void *attributes, int32_t manualReset, int32_t initialState, const char *name)
{
	(void)attributes;
	(void)name;
	Event *event = malloc(sizeof *event);
	if (!event)
		return 0;
	event->type = HANDLE_EVENT;
	event->manualReset = manualReset != 0;
	event->set = initialState != 0;
	return (int32_t)(uintptr_t)event;
}

int32_t SetEvent_wrap(void *handle)
{
	Event *event = handle;
	if (!event)
		return 0;
	pthread_mutex_lock(&s_eventMutex);
	event->set = true;
	pthread_cond_broadcast(&s_eventCond);
	pthread_mutex_unlock(&s_eventMutex);
	return 1;
}

/* Acquires the first signalled event: auto-reset events are reset by it.
 * Returns its index, or -1 when none is signalled. */
static int32_t acquire_signalled(uint32_t count, Event *const *events)
{
	for (uint32_t i = 0; i < count; ++i)
	{
		if (events[i]->set)
		{
			events[i]->set = events[i]->manualReset;
			return (int32_t)i;
		}
	}
	return -1;
}

static struct timespec deadline_after(uint32_t milliseconds)
{
	struct timespec t;
	clock_gettime(CLOCK_REALTIME, &t);
	t.tv_sec += milliseconds / 1000;
	t.tv_nsec += (long)(milliseconds % 1000) * 1000000;
	if (t.tv_nsec >= 1000000000)
	{
		t.tv_nsec -= 1000000000;
		++t.tv_sec;
	}
	return t;
}

static bool deadline_passed(const struct timespec *deadline)
{
	struct timespec now;
	clock_gettime(CLOCK_REALTIME, &now);
	return now.tv_sec != deadline->tv_sec ? now.tv_sec > deadline->tv_sec : now.tv_nsec >= deadline->tv_nsec;
}

/* Win32 semantics for the form the game uses (waitAll FALSE): the lowest
 * signalled event wins and only that one is acquired. 0 polls, INFINITE waits
 * forever. */
int32_t WaitForMultipleObjects_wrap(uint32_t count, Event *const *events, int32_t waitAll, uint32_t milliseconds)
{
	(void)waitAll;
	const struct timespec deadline = deadline_after(milliseconds == INFINITE ? 0 : milliseconds);
	pthread_mutex_lock(&s_eventMutex);
	int32_t index;
	while ((index = acquire_signalled(count, events)) < 0 && milliseconds != 0)
	{
		if (milliseconds == INFINITE)
			pthread_cond_wait(&s_eventCond, &s_eventMutex);
		else if (deadline_passed(&deadline))
			break;
		else
			pthread_cond_timedwait(&s_eventCond, &s_eventMutex, &deadline);
	}
	pthread_mutex_unlock(&s_eventMutex);
	return index < 0 ? WAIT_TIMEOUT : index;
}

/* Files */

int32_t CreateFileA_wrap(const char *fileName, uint32_t access, uint32_t shareMode, void *attributes,
	uint32_t disposition, uint32_t flags, void *templateFile)
{
	(void)shareMode;
	(void)attributes;
	(void)disposition;
	(void)flags;
	(void)templateFile;
	static const char comPrefix[] = "\\\\.\\com";
	if (!strncasecmp(fileName, comPrefix, sizeof comPrefix - 1))
		return (int32_t)(intptr_t)INVALID_HANDLE;

	int fd = -1;
	char *path = config_convert_path(fileName, true);
	if (path && access == GENERIC_WRITE)
		fd = open(path, O_CREAT | O_WRONLY | O_TRUNC, 0644);
	else if (path && access == GENERIC_READ)
		fd = open(path, O_RDONLY);
	free(path);
	if (fd < 0)
		return (int32_t)(intptr_t)INVALID_HANDLE;

	File *file = malloc(sizeof *file);
	if (!file)
	{
		close(fd);
		return (int32_t)(intptr_t)INVALID_HANDLE;
	}
	file->type = HANDLE_FILE;
	file->fd = fd;
	file->writable = access == GENERIC_WRITE;
	return (int32_t)(uintptr_t)file;
}

int32_t GetFileSize_wrap(File *file, uint32_t *sizeHigh)
{
	struct stat st;
	if (sizeHigh)
		*sizeHigh = 0;
	if (fstat(file->fd, &st))
		return -1;
	return (int32_t)st.st_size;
}

int32_t ReadFile_wrap(File *file, void *buffer, uint32_t toRead, uint32_t *bytesRead, void *overlapped)
{
	(void)overlapped;
	const ssize_t r = read(file->fd, buffer, toRead);
	*bytesRead = r > 0 ? (uint32_t)r : 0;
	return *bytesRead == toRead; /* short reads fail, as in the upstream wrapper */
}

int32_t WriteFile_wrap(File *file, const void *buffer, uint32_t toWrite, uint32_t *bytesWritten, void *overlapped)
{
	(void)overlapped;
	const ssize_t w = write(file->fd, buffer, toWrite);
	*bytesWritten = w > 0 ? (uint32_t)w : 0;
	return *bytesWritten == toWrite;
}

int32_t SetFilePointer_wrap(File *file, int32_t distance, int32_t *distanceHigh, uint32_t moveMethod)
{
	(void)distanceHigh;
	/* FILE_BEGIN, FILE_CURRENT and FILE_END equal SEEK_SET, SEEK_CUR and SEEK_END */
	return (int32_t)lseek(file->fd, distance, (int)moveMethod);
}

int32_t SetEndOfFile_wrap(File *file)
{
	const off_t pos = lseek(file->fd, 0, SEEK_CUR);
	return pos >= 0 && !ftruncate(file->fd, pos);
}

int32_t CreateFileMappingA_wrap(File *file, void *attributes, uint32_t protect, uint32_t sizeHigh, uint32_t sizeLow, const char *name)
{
	(void)attributes;
	(void)protect;
	(void)sizeHigh;
	(void)sizeLow;
	(void)name;
	Mapping *mapping = malloc(sizeof *mapping);
	if (!mapping)
		return 0;
	mapping->type = HANDLE_MAPPING;
	mapping->fd = file->fd;
	return (int32_t)(uintptr_t)mapping;
}

/* Views are private heap copies of the whole file; UnmapViewOfFile frees them.
 * A file that cannot be read completely has no view. */
int32_t MapViewOfFile_wrap(Mapping *mapping, uint32_t access, uint32_t offsetHigh, uint32_t offsetLow, uint32_t bytes)
{
	(void)access;
	(void)offsetHigh;
	(void)offsetLow;
	(void)bytes;
	struct stat st;
	if (fstat(mapping->fd, &st) || st.st_size <= 0 || (uint64_t)st.st_size > SIZE_MAX - VIEW_SLACK)
		return 0;
	const size_t size = (size_t)st.st_size;
	uint8_t *view = malloc(size + VIEW_SLACK);
	if (!view)
		return 0;
	size_t done = 0;
	while (done < size)
	{
		const ssize_t r = pread(mapping->fd, view + done, size - done, (off_t)done);
		if (r <= 0)
		{
			free(view);
			return 0;
		}
		done += (size_t)r;
	}
	memset(view + size, 0, VIEW_SLACK);
	return (int32_t)(uintptr_t)view;
}

int32_t UnmapViewOfFile_wrap(void *view)
{
	free(view);
	return 1;
}

int32_t GetOverlappedResult_wrap(void *file, void *overlapped, uint32_t *transferred, int32_t wait)
{
	(void)file;
	(void)overlapped;
	(void)wait;
	*transferred = 0;
	return 0;
}

int32_t GetCommState_wrap(void *file, void *dcb)
{
	(void)file;
	(void)dcb;
	return 0;
}

int32_t SetCommState_wrap(void *file, void *dcb)
{
	(void)file;
	(void)dcb;
	return 0;
}

int32_t SetCommTimeouts_wrap(void *file, void *timeouts)
{
	(void)file;
	(void)timeouts;
	return 0;
}

int32_t PurgeComm_wrap(void *file, uint32_t flags)
{
	(void)file;
	(void)flags;
	return 0;
}

/* Runs op on the host path of a game path; returns the Win32 BOOL result. */
static int32_t on_host_path(const char *gamePath, bool toLower, int (*op)(const char *))
{
	char *path = config_convert_path(gamePath, toLower);
	const bool ok = path && op(path) == 0;
	free(path);
	return ok;
}

int32_t DeleteFileA_wrap(const char *fileName)
{
	if (!on_host_path(fileName, true, unlink))
		return 0;
	web_mark_fs_dirty();
	return 1;
}

int32_t CloseHandle_wrap(void *handle)
{
	if (!handle || handle == INVALID_HANDLE)
		return 0;
	switch (*(HandleType *)handle)
	{
		case HANDLE_THREAD:
			thread_release(handle);
			return 1;
		case HANDLE_FILE:
		{
			File *file = handle;
			close(file->fd);
			if (file->writable)
				web_mark_fs_dirty();
			free(file);
			return 1;
		}
		case HANDLE_MAPPING:
		case HANDLE_EVENT:
			free(handle);
			return 1;
	}
	return 0;
}

int32_t GetLastError_wrap(void)
{
	return errno == ENOENT ? ERROR_FILE_NOT_FOUND : 0;
}

/* Directories */

int32_t GetCurrentDirectoryA_wrap(uint32_t length, char *buffer)
{
	return getcwd(buffer, length) ? (int32_t)length : 0;
}

/* Directory names keep their case. */
int32_t SetCurrentDirectoryA_wrap(const char *pathName)
{
	return on_host_path(pathName, false, chdir);
}

/* The next regular file in the searched directory whose name matches the
 * pattern, ignoring case like Windows. Names starting with a dot, including
 * "." and "..", are skipped. */
static bool find_next(FindFile *find, FindData *data)
{
	const struct dirent *entry;
	while ((entry = readdir(find->dir)))
	{
		if (entry->d_type == DT_DIR || fnmatch(find->pattern, entry->d_name, FNM_CASEFOLD | FNM_PERIOD) != 0)
			continue;
		memset(data, 0, sizeof *data);
		snprintf(data->fileName, sizeof data->fileName, "%s", entry->d_name);
		return true;
	}
	return false;
}

static void find_free(FindFile *find)
{
	if (find->dir)
		closedir(find->dir);
	free(find->pattern);
	free(find);
}

int32_t FindNextFileA_wrap(FindFile *find, FindData *data)
{
	return find_next(find, data);
}

int32_t FindClose_wrap(FindFile *find)
{
	if (!find || find == INVALID_HANDLE)
		return 0;
	find_free(find);
	return 1;
}

/* The game searches the current directory with patterns such as "*.trk". */
int32_t FindFirstFileA_wrap(const char *pattern, FindData *data)
{
	FindFile *find = calloc(1, sizeof *find);
	if (!find)
		return (int32_t)(intptr_t)INVALID_HANDLE;
	find->dir = opendir(".");
	find->pattern = strdup(pattern);
	if (!find->dir || !find->pattern || !find_next(find, data))
	{
		find_free(find);
		return (int32_t)(intptr_t)INVALID_HANDLE;
	}
	return (int32_t)(uintptr_t)find;
}

/* System information */

int32_t GlobalMemoryStatus_wrap(MemoryStatus *status)
{
	memset(status, 0, sizeof *status);
	status->length = sizeof *status;
	status->totalPhys = 0x7FFFFFFF;
	status->totalPageFile = 0x7FFFFFFF;
	status->availPageFile = 0x7FFFFFFF;
	status->totalVirtual = 0x7FFFFFFF;
	status->availVirtual = 0x7FFFFFFF;
	return 0;
}

int32_t GetSystemInfo_wrap(SystemInfo *info)
{
	memset(info, 0, sizeof *info);
	info->pageSize = 4096;
	return 0;
}

int32_t GetModuleHandleA_wrap(const char *moduleName)
{
	(void)moduleName;
	return 0;
}

int32_t ExitProcess_wrap(uint32_t exitCode)
{
	wrapper_exit_procs();
	exit((int)exitCode);
}

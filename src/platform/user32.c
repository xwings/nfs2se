/* User32 subset: the window message queue. Keyboard messages are built in JS
 * (src/web/input.js) and arrive through the control block ring; C queues its
 * own messages (PostMessage, DestroyWindow, keys simulated by joystick
 * buttons) in a local queue that is served first. Covers the entry points of
 * NFSIISE's User32.c (MIT, zaps166). */
#include "platform.h"

#include <pthread.h>
#include <stdatomic.h>
#include <string.h>

#define WM_DESTROY 0x0002
#define WM_USER 0x0400
#define WM_USER_END 0xC000

#define LOCAL_QUEUE_SIZE 256 /* power of two */
#define MESSAGE_WAIT_MS 100

typedef struct
{
	uint32_t hwnd, message, wParam, lParam, time, ptX, ptY;
} Msg;
_Static_assert(sizeof(Msg) == 28, "MSG is 28 bytes");

typedef struct
{
	uint32_t message, wParam, lParam;
} QueuedMsg;

static pthread_mutex_t s_queueMutex = PTHREAD_MUTEX_INITIALIZER;
static QueuedMsg s_queue[LOCAL_QUEUE_SIZE];
static uint32_t s_queueHead, s_queueTail;

static GameFn s_wndProc;
static int s_window; /* its address is the window handle */
static atomic_bool s_windowAlive;

static uint32_t window_handle(void)
{
	return (uint32_t)(uintptr_t)&s_window;
}

/* Bumps CTL_SEQ like JS does for the ring, which wakes GetMessageA. */
static bool queue_push(uint32_t message, uint32_t wParam, uint32_t lParam)
{
	pthread_mutex_lock(&s_queueMutex);
	const bool ok = s_queueHead - s_queueTail < LOCAL_QUEUE_SIZE;
	if (ok)
		s_queue[s_queueHead++ & (LOCAL_QUEUE_SIZE - 1)] = (QueuedMsg){message, wParam, lParam};
	pthread_mutex_unlock(&s_queueMutex);
	if (ok)
	{
		__atomic_fetch_add(&web_ctl()[CTL_SEQ], 1, __ATOMIC_SEQ_CST);
		web_futex_wake(&web_ctl()[CTL_SEQ]);
	}
	return ok;
}

static bool queue_pop(QueuedMsg *out)
{
	pthread_mutex_lock(&s_queueMutex);
	const bool ok = s_queueHead != s_queueTail;
	if (ok)
		*out = s_queue[s_queueTail++ & (LOCAL_QUEUE_SIZE - 1)];
	pthread_mutex_unlock(&s_queueMutex);
	return ok;
}

/* Single consumer: only the game's message loop thread reads the ring. */
static bool ring_pop(QueuedMsg *out)
{
	const uint32_t tail = (uint32_t)ctl_load(CTL_RING_TAIL);
	if ((uint32_t)ctl_load(CTL_RING_HEAD) == tail)
		return false;
	const int entry = CTL_RING_BASE + (int)(tail & (CTL_RING_ENTRIES - 1)) * CTL_RING_ENTRY_WORDS;
	out->message = (uint32_t)ctl_load(entry + RING_MSG);
	out->wParam = (uint32_t)ctl_load(entry + RING_WPARAM);
	out->lParam = (uint32_t)ctl_load(entry + RING_LPARAM);
	ctl_store(CTL_RING_TAIL, (int32_t)(tail + 1));
	return true;
}

int32_t user32_open_window(GameFn wndProc)
{
	s_wndProc = wndProc;
	atomic_store(&s_windowAlive, true);
	return (int32_t)window_handle();
}

bool user32_window_alive(void)
{
	return atomic_load(&s_windowAlive);
}

/* Keys from joystick buttons, encoded like keyboard input (web_layout.h). The
 * key message decides the result; a WM_CHAR that no longer fits is dropped. */
bool user32_post_key(bool down, int32_t vk, int32_t scancode)
{
	const uint32_t lParam = KEY_REPEAT | (uint32_t)scancode << KEY_SCANCODE_SHIFT;
	if (!queue_push(down ? WM_KEYDOWN : WM_KEYUP, (uint32_t)vk, lParam))
		return false;
	if (down && (vk == VK_BACK || vk == VK_TAB || vk == VK_RETURN || vk == VK_ESCAPE))
		queue_push(WM_CHAR, (uint32_t)vk, lParam);
	return true;
}

/* Never returns 0: nothing posts WM_QUIT, the game ends through ExitProcess. */
int32_t GetMessageA_wrap(Msg *msg, void *hWnd, uint32_t filterMin, uint32_t filterMax)
{
	(void)hWnd;
	(void)filterMin;
	(void)filterMax;
	QueuedMsg m;
	for (;;)
	{
		const int32_t seq = ctl_load(CTL_SEQ);
		if (queue_pop(&m) || ring_pop(&m))
			break;
		web_futex_wait(&web_ctl()[CTL_SEQ], seq, MESSAGE_WAIT_MS);
	}

	memset(msg, 0, sizeof *msg);
	msg->hwnd = window_handle();
	msg->message = m.message;
	msg->wParam = m.wParam;
	msg->lParam = m.lParam;
	msg->time = web_ticks();
	return 1;
}

int32_t DispatchMessageA_wrap(void *game, const Msg *msg)
{
	wrap_stdcall4(game, s_wndProc, (int32_t)msg->hwnd, (int32_t)msg->message, (int32_t)msg->wParam, (int32_t)msg->lParam);
	return 0;
}

int32_t PostMessageA_wrap(void *hWnd, uint32_t message, uint32_t wParam, uint32_t lParam)
{
	(void)hWnd;
	(void)wParam; /* dropped, as in the upstream wrapper */
	if (message < WM_USER || message >= WM_USER_END)
		return 0;
	return queue_push(message, 0, lParam);
}

int32_t DefWindowProcA_wrap(void *hWnd, uint32_t message, uint32_t wParam, uint32_t lParam)
{
	(void)hWnd;
	(void)wParam;
	(void)lParam;
	if (message == WM_DESTROY)
		atomic_store(&s_windowAlive, false);
	return 0;
}

int32_t DestroyWindow_wrap(void *hWnd)
{
	(void)hWnd;
	return queue_push(WM_DESTROY, 0, 0);
}

int32_t GetKeyboardType_wrap(int32_t typeFlag)
{
	(void)typeFlag;
	return 0;
}

/* MB_OKCANCEL is bit 0; returns IDOK (1) or IDCANCEL (2). */
int32_t MessageBoxA_wrap(void *hWnd, const char *text, const char *caption, uint32_t type)
{
	(void)hWnd;
	return web_message_box(text ? text : "", caption ? caption : "", type & 1);
}

/* user32.c: the window message queue. Messages queued by C come before those
 * JS writes to the control block ring; web_futex_wait stands in for JS while
 * GetMessageA waits. */
#include "check.h"
#include "platform.h"

#define WM_DESTROY 0x0002
#define WM_USER 0x0400
#define MB_OKCANCEL 0x01
#define MB_ICONWARNING 0x30
#define TICKS 1234u

typedef struct
{
	uint32_t hwnd, message, wParam, lParam, time, ptX, ptY;
} Msg; /* Win32 MSG */

int32_t GetMessageA_wrap(Msg *msg, void *hWnd, uint32_t filterMin, uint32_t filterMax);
int32_t DispatchMessageA_wrap(void *game, const Msg *msg);
int32_t PostMessageA_wrap(void *hWnd, uint32_t message, uint32_t wParam, uint32_t lParam);
int32_t DefWindowProcA_wrap(void *hWnd, uint32_t message, uint32_t wParam, uint32_t lParam);
int32_t DestroyWindow_wrap(void *hWnd);
int32_t MessageBoxA_wrap(void *hWnd, const char *text, const char *caption, uint32_t type);

static int32_t s_ctl[CTL_WORDS];
static int s_wakes;
static uint32_t s_onWait[3]; /* message JS sends while GetMessageA waits, 0 for none */
static int s_waits;

int32_t *web_ctl(void)
{
	return s_ctl;
}

uint32_t web_ticks(void)
{
	return TICKS;
}

/* Like pushMessage() in src/web/input.js. */
static void push_ring(uint32_t message, uint32_t wParam, uint32_t lParam)
{
	const int32_t head = s_ctl[CTL_RING_HEAD];
	const int entry = CTL_RING_BASE + (head & (CTL_RING_ENTRIES - 1)) * CTL_RING_ENTRY_WORDS;
	s_ctl[entry + RING_MSG] = (int32_t)message;
	s_ctl[entry + RING_WPARAM] = (int32_t)wParam;
	s_ctl[entry + RING_LPARAM] = (int32_t)lParam;
	s_ctl[CTL_RING_HEAD] = head + 1;
	++s_ctl[CTL_SEQ];
}

void web_futex_wait(int32_t *addr, int32_t expected, uint32_t ms)
{
	++s_waits;
	CHECK(addr == &s_ctl[CTL_SEQ]);
	CHECK_INT(*addr, expected);
	CHECK(ms > 0);
	if (!s_onWait[0])
	{
		CHECK(!"GetMessageA waited for a message that never comes");
		s_onWait[0] = WM_USER;
	}
	push_ring(s_onWait[0], s_onWait[1], s_onWait[2]);
	s_onWait[0] = 0;
}

void web_futex_wake(int32_t *addr)
{
	CHECK(addr == &s_ctl[CTL_SEQ]);
	++s_wakes;
}

static const char *s_boxText, *s_boxCaption;
static bool s_boxOkCancel;

int32_t web_message_box(const char *text, const char *caption, bool okCancel)
{
	s_boxText = text;
	s_boxCaption = caption;
	s_boxOkCancel = okCancel;
	return DIALOG_CANCEL;
}

static int32_t s_dispatched[4];

void wrap_stdcall4(void *game, GameFn func, int32_t arg0, int32_t arg1, int32_t arg2, int32_t arg3)
{
	(void)game;
	(void)func;
	s_dispatched[0] = arg0;
	s_dispatched[1] = arg1;
	s_dispatched[2] = arg2;
	s_dispatched[3] = arg3;
}

static void window_proc(void *game)
{
	(void)game;
}

static uint32_t s_window;

/* Takes the next message, which must be queued already. */
static Msg next_message(void)
{
	Msg msg;
	const int waits = s_waits;
	CHECK_INT(GetMessageA_wrap(&msg, NULL, 0, 0), 1);
	CHECK_INT(s_waits, waits);
	CHECK_INT(msg.hwnd, s_window);
	CHECK_INT(msg.time, TICKS);
	return msg;
}

static void check_message(uint32_t message, uint32_t wParam, uint32_t lParam)
{
	const Msg msg = next_message();
	CHECK_INT(msg.message, message);
	CHECK_INT(msg.wParam, wParam);
	CHECK_INT(msg.lParam, lParam);
}

static void test_window(void)
{
	s_window = (uint32_t)user32_open_window(window_proc);
	CHECK(s_window != 0);
	CHECK(user32_window_alive());

	const Msg msg = {s_window, WM_USER + 1, 2, 3, 0, 0, 0};
	DispatchMessageA_wrap(NULL, &msg);
	CHECK_INT(s_dispatched[0], s_window);
	CHECK_INT(s_dispatched[1], WM_USER + 1);
	CHECK_INT(s_dispatched[2], 2);
	CHECK_INT(s_dispatched[3], 3);
}

/* Joystick keys are encoded like the keyboard's: lParam holds the scancode,
 * and of these keys only Escape types a character. */
static void test_post_key(void)
{
	const int32_t seq = s_ctl[CTL_SEQ];
	const int wakes = s_wakes;
	const uint32_t escape = KEY_REPEAT | SC_ESCAPE << KEY_SCANCODE_SHIFT;
	const uint32_t f11 = KEY_REPEAT | SC_F11 << KEY_SCANCODE_SHIFT;
	CHECK(user32_post_key(true, VK_ESCAPE, SC_ESCAPE));
	CHECK(user32_post_key(false, VK_ESCAPE, SC_ESCAPE));
	CHECK(user32_post_key(true, VK_F11, SC_F11));
	CHECK(user32_post_key(false, VK_F11, SC_F11));
	CHECK_INT(s_ctl[CTL_SEQ], seq + 5);
	CHECK_INT(s_wakes, wakes + 5);
	check_message(WM_KEYDOWN, VK_ESCAPE, escape);
	check_message(WM_CHAR, VK_ESCAPE, escape);
	check_message(WM_KEYUP, VK_ESCAPE, escape);
	check_message(WM_KEYDOWN, VK_F11, f11);
	check_message(WM_KEYUP, VK_F11, f11);
}

/* A key the full queue cannot take is reported, so dinput.c can retry it. */
static void test_full_queue(void)
{
	const uint32_t up = KEY_REPEAT | SC_UP << KEY_SCANCODE_SHIFT;
	int queued = 0;
	while (queued < 1000 && user32_post_key(queued % 2 == 0, VK_UP, SC_UP))
		++queued;
	CHECK(queued >= 16 && queued < 1000);
	CHECK(!user32_post_key(false, VK_UP, SC_UP));
	check_message(WM_KEYDOWN, VK_UP, up);
	CHECK(user32_post_key(false, VK_UP, SC_UP)); /* the key that did not fit */
	for (int i = 1; i < queued; ++i)
		check_message(i % 2 ? WM_KEYUP : WM_KEYDOWN, VK_UP, up);
	check_message(WM_KEYUP, VK_UP, up);
}

/* PostMessage takes application messages only and drops wParam; C's messages
 * come before the ring's. */
static void test_post_message(void)
{
	CHECK_INT(PostMessageA_wrap(NULL, WM_KEYDOWN, 1, 2), 0);
	CHECK_INT(PostMessageA_wrap(NULL, 0xC000, 1, 2), 0);
	push_ring(WM_KEYDOWN, 0x41, 7);
	CHECK(PostMessageA_wrap(NULL, WM_USER, 1, 2) != 0);
	CHECK(PostMessageA_wrap(NULL, 0xBFFF, 3, 4) != 0);
	check_message(WM_USER, 0, 2);
	check_message(0xBFFF, 0, 4);
	check_message(WM_KEYDOWN, 0x41, 7);
}

/* With nothing queued GetMessageA waits on CTL_SEQ, which JS bumps. */
static void test_wait(void)
{
	s_onWait[0] = WM_CHAR;
	s_onWait[1] = 'a';
	s_onWait[2] = 9;
	Msg msg;
	CHECK_INT(GetMessageA_wrap(&msg, NULL, 0, 0), 1);
	CHECK_INT(s_waits, 1);
	CHECK_INT(msg.message, WM_CHAR);
	CHECK_INT(msg.wParam, 'a');
	CHECK_INT(msg.lParam, 9);
}

/* DestroyWindow queues WM_DESTROY; the window is gone once the game passes it
 * to DefWindowProc. */
static void test_destroy(void)
{
	CHECK(DestroyWindow_wrap(NULL) != 0);
	check_message(WM_DESTROY, 0, 0);
	CHECK(user32_window_alive());
	DefWindowProcA_wrap(NULL, WM_DESTROY, 0, 0);
	CHECK(!user32_window_alive());
}

static void test_message_box(void)
{
	CHECK_INT(MessageBoxA_wrap(NULL, NULL, "Title", MB_OKCANCEL), DIALOG_CANCEL);
	CHECK_STR(s_boxText, "");
	CHECK_STR(s_boxCaption, "Title");
	CHECK(s_boxOkCancel);
	MessageBoxA_wrap(NULL, "Text", NULL, MB_ICONWARNING);
	CHECK_STR(s_boxText, "Text");
	CHECK_STR(s_boxCaption, "");
	CHECK(!s_boxOkCancel);
}

int main(void)
{
	test_window();
	test_post_key();
	test_full_queue();
	test_post_message();
	test_wait();
	test_destroy();
	test_message_box();
	return check_exit();
}

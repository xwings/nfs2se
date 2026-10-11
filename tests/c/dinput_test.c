/* dinput.c: the DirectInput objects for the mouse and the two joysticks, and
 * processAxes. Built with -pthread (Makefile TEST_CFLAGS_dinput) so a second
 * thread can read a joystick the way the race thread does. */
#include "check.h"
#include "platform.h"

#include <pthread.h>
#include <stdlib.h>

/* Imported by the game, which declares them with 32-bit integer arguments. */
int32_t DirectInputCreateA_wrap(int32_t game, int32_t instance, int32_t version, int32_t out, int32_t outer);
int32_t EnumDevices(int32_t self, int32_t devType, int32_t callback, int32_t ref, int32_t flags);
int32_t CreateDevice(int32_t self, int32_t guid, int32_t out, int32_t outer);
int32_t QueryInterface(int32_t self, int32_t riid, int32_t out);
int32_t Release(int32_t self);
int32_t GetCapabilities(int32_t self, int32_t caps);
int32_t GetObjectInfo(int32_t self, int32_t instance, int32_t obj, int32_t how);
int32_t GetDeviceState(int32_t self, int32_t size, int32_t state);
int32_t GetDeviceData(int32_t self, int32_t objectSize, int32_t data, int32_t inOut, int32_t flags);
int32_t CreateEffect(int32_t self, int32_t guid, int32_t params, int32_t out, int32_t outer);
int32_t processAxes(int32_t inputValue);

#define ARG(p) ((int32_t)(uintptr_t)(p))

/* The game's views of DirectInput's structures (32-bit layout). */
typedef struct
{
	uint32_t size;
	uint32_t guidInstance[4];
} InstanceHead;

typedef struct
{
	uint32_t size, flags, devType, axes, buttons, rest[6];
} DevCaps;

typedef struct
{
	int32_t axes[8];
	uint32_t povs[4];
	uint8_t buttons[32];
} JoyState;
_Static_assert(sizeof(JoyState) == 80, "DIJOYSTATE is 80 bytes");

typedef struct
{
	uint32_t ofs, data, timeStamp, sequence;
} ObjectData;

enum
{
	GUID_MOUSE = 0x6F1D2B60,
	GUID_JOYSTICK = 0x6F1D2B70,
	DIDEVTYPE_JOYSTICK = 4,
	DIDEVTYPE_KEYBOARD = 3,
	DIDC_FORCEFEEDBACK = 0x100,
	CENTRE = 0x8000,
	PRESSED = 0x80,
	OBJECT_INFO_BYTES = 316, /* DIDEVICEOBJECTINSTANCEA */
	ENUM_CALLBACK = 0x1234,
	ASSIGN_HOLD_POLLS = 50,
	/* key codes the game expects, as the keyboard sends them (input.js KEYMAP) */
	KEY_VK_ESCAPE = 0x1B, KEY_SC_ESCAPE = 0x01,
	KEY_VK_F11 = 0x7A, KEY_SC_F11 = 0x57, KEY_VK_F12 = 0x7B, KEY_SC_F12 = 0x58,
	KEY_VK_RIGHT = 0x27, KEY_SC_RIGHT = 0x4D, KEY_VK_LEFT = 0x25, KEY_SC_LEFT = 0x4B,
	KEY_VK_DOWN = 0x28, KEY_SC_DOWN = 0x50, KEY_VK_UP = 0x26, KEY_SC_UP = 0x48,
};

/* Game state and glue the platform layer uses. */
static int32_t s_ctl[CTL_WORDS];
static uint8_t s_assignMode;
static uint32_t s_gameMouseX, s_gameMouseY;
static uint32_t s_gameAxes[2 * 16];
void *inControlAssignMode = &s_assignMode;
void *mousePositionX = &s_gameMouseX;
void *mousePositionY = &s_gameMouseY;
void *dword_557540 = s_gameAxes;

int32_t *web_ctl(void)
{
	return s_ctl;
}

#define KEY_LOG_SIZE 16
static struct
{
	bool down;
	int32_t vk, scancode;
} s_keys[KEY_LOG_SIZE];
static int s_keyCount;
static bool s_keyQueueFull;

bool user32_post_key(bool down, int32_t vk, int32_t scancode)
{
	if (s_keyQueueFull)
		return false;
	if (s_keyCount < KEY_LOG_SIZE)
		s_keys[s_keyCount] = (typeof(s_keys[0])){down, vk, scancode};
	++s_keyCount;
	return true;
}

static void *s_game = &s_ctl[0];
static uint32_t s_enumPads[4];
static int s_enumCalls, s_enumContinueCalls;

/* Calls into game code; for EnumDevices this is the device callback, which
 * here records the device and continues s_enumContinueCalls times. */
int32_t wrap_stdcall2_ret(void *game, GameFn func, int32_t arg0, int32_t arg1)
{
	const InstanceHead *instance = (const InstanceHead *)(uintptr_t)arg0;
	CHECK(game == s_game);
	CHECK_INT((uintptr_t)func, ENUM_CALLBACK);
	CHECK_INT(arg1, 77);
	CHECK_INT(instance->guidInstance[0], GUID_JOYSTICK);
	if (s_enumCalls < 4)
		s_enumPads[s_enumCalls] = instance->guidInstance[1];
	return ++s_enumCalls < s_enumContinueCalls;
}

/* Vtable entries: the game's thunks, only referenced by address here. */
#define THUNK(name)            \
	void name(void *game)      \
	{                          \
		(void)game;            \
	}
THUNK(Release_wrap)
THUNK(CreateDevice_wrap)
THUNK(EnumDevices_wrap)
THUNK(QueryInterface_wrap)
THUNK(GetCapabilities_wrap)
THUNK(SetProperty_wrap)
THUNK(Acquire_wrap)
THUNK(Unacquire_wrap)
THUNK(GetDeviceState_wrap)
THUNK(GetDeviceData_wrap)
THUNK(SetDataFormat_wrap)
THUNK(SetEventNotification_wrap)
THUNK(SetCooperativeLevel_wrap)
THUNK(GetObjectInfo_wrap)
THUNK(CreateEffect_wrap)
THUNK(SendForceFeedbackCommand_wrap)
THUNK(Poll_wrap)
THUNK(SetParameters_wrap)
THUNK(Start_wrap)
THUNK(Stop_wrap)
THUNK(Download_wrap)
THUNK(Unload_wrap)

static int32_t s_dinput;

static void configure(const char *text)
{
	config_reset();
	FILE *f = fmemopen((void *)text, strlen(text), "r");
	config_parse(f);
	fclose(f);
}

static int32_t create_device(uint32_t guidA, uint32_t guidB)
{
	const uint32_t guid[4] = {guidA, guidB, 0, 0};
	int32_t device = 0;
	CHECK_INT(CreateDevice(s_dinput, ARG(guid), ARG(&device), 0), 0);
	CHECK(device != 0);
	return device;
}

static int pad_word(int pad, int word)
{
	return CTL_PAD_BASE + pad * CTL_PAD_STRIDE + word;
}

static void plug_pad(int pad, int32_t buttons, int32_t axes, int32_t hats)
{
	memset(&s_ctl[pad_word(pad, 0)], 0, CTL_PAD_STRIDE * sizeof *s_ctl);
	s_ctl[pad_word(pad, PAD_CONNECTED)] = 1;
	s_ctl[pad_word(pad, PAD_NUM_BUTTONS)] = buttons;
	s_ctl[pad_word(pad, PAD_NUM_AXES)] = axes;
	s_ctl[pad_word(pad, PAD_NUM_HATS)] = hats;
}

static void press(int pad, int32_t mask)
{
	s_ctl[pad_word(pad, PAD_BUTTONS)] = mask;
}

static JoyState poll(int32_t device)
{
	JoyState state;
	memset(&state, 0x55, sizeof state);
	CHECK_INT(GetDeviceState(device, sizeof state, ARG(&state)), 0);
	return state;
}

static void check_key(int index, bool down, int32_t vk, int32_t scancode)
{
	CHECK(index < s_keyCount);
	if (index >= s_keyCount || index >= KEY_LOG_SIZE)
		return;
	CHECK_INT(s_keys[index].down, down);
	CHECK_INT(s_keys[index].vk, vk);
	CHECK_INT(s_keys[index].scancode, scancode);
}

static void test_objects(void)
{
	int32_t other = 0;
	CHECK_INT(QueryInterface(s_dinput, 0, ARG(&other)), 0);
	CHECK_INT(other, s_dinput);
	CHECK_INT(Release(other), 0);

	const uint32_t keyboard[4] = {0x6F1D2B61, 0, 0, 0};
	int32_t device = 0;
	CHECK(CreateDevice(s_dinput, ARG(keyboard), ARG(&device), 0) != 0);
	CHECK_INT(device, 0);
	/* Only CTL_PAD_COUNT joysticks have control words. */
	const uint32_t thirdPad[4] = {GUID_JOYSTICK, CTL_PAD_COUNT, 0, 0};
	CHECK(CreateDevice(s_dinput, ARG(thirdPad), ARG(&device), 0) != 0);
	CHECK_INT(device, 0);

	const int32_t joystick = create_device(GUID_JOYSTICK, 1);
	DevCaps caps = {.size = sizeof caps, .devType = 99, .rest = {1, 1, 1, 1, 1, 1}};
	CHECK_INT(GetCapabilities(joystick, ARG(&caps)), 0);
	CHECK_INT(caps.size, sizeof caps);
	CHECK_INT(caps.flags, DIDC_FORCEFEEDBACK);
	CHECK_INT(caps.devType, 0);
	CHECK_INT(caps.axes, 6);
	CHECK_INT(caps.buttons, 32);
	CHECK_INT(caps.rest[5], 0);

	const int32_t mouse = create_device(GUID_MOUSE, 0);
	caps = (DevCaps){.size = sizeof caps, .axes = 9};
	CHECK_INT(GetCapabilities(mouse, ARG(&caps)), 0);
	CHECK_INT(caps.axes, 9);

	/* Object info is cleared after its size field, up to that size. */
	uint8_t info[OBJECT_INFO_BYTES + 4];
	memset(info, 0x55, sizeof info);
	*(uint32_t *)info = 8;
	CHECK_INT(GetObjectInfo(joystick, ARG(info), 0, 0), 0);
	CHECK_INT(info[4], 0);
	CHECK_INT(info[7], 0);
	CHECK_INT(info[8], 0x55);
	*(uint32_t *)info = 0;
	CHECK_INT(GetObjectInfo(joystick, ARG(info), 0, 0), 0);
	CHECK_INT(info[OBJECT_INFO_BYTES - 1], 0);
	CHECK_INT(info[OBJECT_INFO_BYTES], 0x55);

	int32_t effect = 0;
	CHECK_INT(CreateEffect(joystick, 0, 0, ARG(&effect), 0), 0);
	CHECK(effect != 0);
	CHECK_INT(Release(effect), 0);
	CHECK_INT(Release(joystick), 0);
	CHECK_INT(Release(mouse), 0);
}

static void test_enum(void)
{
	s_enumCalls = 0;
	s_enumContinueCalls = 10;
	CHECK_INT(EnumDevices(s_dinput, DIDEVTYPE_JOYSTICK, ENUM_CALLBACK, 77, 0), 0);
	CHECK_INT(s_enumCalls, CTL_PAD_COUNT);
	CHECK_INT(s_enumPads[0], 0);
	CHECK_INT(s_enumPads[1], 1);

	s_enumCalls = 0;
	s_enumContinueCalls = 1; /* the callback stops the enumeration */
	CHECK_INT(EnumDevices(s_dinput, DIDEVTYPE_JOYSTICK, ENUM_CALLBACK, 77, 0), 0);
	CHECK_INT(s_enumCalls, 1);

	s_enumCalls = 0;
	CHECK_INT(EnumDevices(s_dinput, DIDEVTYPE_KEYBOARD, ENUM_CALLBACK, 77, 0), 0);
	CHECK_INT(s_enumCalls, 0);
}

static void test_unplugged(void)
{
	configure("");
	const int32_t joystick = create_device(GUID_JOYSTICK, 0);
	const JoyState state = poll(joystick);
	for (int i = 0; i < 8; ++i)
		CHECK_INT(state.axes[i], CENTRE);
	for (int i = 0; i < 32; ++i)
		CHECK_INT(state.buttons[i], 0);

	JoyState untouched;
	memset(&untouched, 0x55, sizeof untouched);
	CHECK_INT(GetDeviceState(joystick, sizeof untouched - 4, ARG(&untouched)), 0);
	CHECK_INT(untouched.axes[0], 0x55555555);
	const int32_t mouse = create_device(GUID_MOUSE, 0);
	CHECK_INT(GetDeviceState(mouse, sizeof untouched, ARG(&untouched)), 0);
	CHECK_INT(untouched.axes[0], 0x55555555);
	Release(mouse);
	Release(joystick);
}

static void test_buttons(void)
{
	configure("");
	const int32_t joystick = create_device(GUID_JOYSTICK, 1);
	plug_pad(1, 40, 0, 0); /* more buttons than DIJOYSTATE holds */
	press(1, (int32_t)0x80000005u);
	JoyState state = poll(joystick);
	CHECK_INT(state.buttons[0], PRESSED);
	CHECK_INT(state.buttons[1], 0);
	CHECK_INT(state.buttons[2], PRESSED);
	CHECK_INT(state.buttons[31], PRESSED);

	plug_pad(1, 2, 0, 0);
	press(1, 5);
	state = poll(joystick);
	CHECK_INT(state.buttons[0], PRESSED);
	CHECK_INT(state.buttons[2], 0); /* beyond the pad's button count */
	CHECK_INT(s_keyCount, 0);
	plug_pad(1, 0, 0, 0);
	s_ctl[pad_word(1, PAD_CONNECTED)] = 0;
	Release(joystick);
}

static void test_keys(void)
{
	configure("Joystick0EscButton=3\nJoystick0ResetButton=4\nJoystick0DPadButtons=5,6,7,8\n"
	          "Joystick1ResetButton=0\n");
	const int32_t joystick = create_device(GUID_JOYSTICK, 0);
	plug_pad(0, 12, 0, 0);
	s_keyCount = 0;

	press(0, 1 << 3 | 1 << 1);
	JoyState state = poll(joystick);
	CHECK_INT(state.buttons[3], 0); /* a key button is not reported */
	CHECK_INT(state.buttons[1], PRESSED);
	poll(joystick); /* held: no repeat */
	CHECK_INT(s_keyCount, 1);
	check_key(0, true, KEY_VK_ESCAPE, KEY_SC_ESCAPE);
	press(0, 1 << 4);
	poll(joystick);
	CHECK_INT(s_keyCount, 3);
	check_key(1, false, KEY_VK_ESCAPE, KEY_SC_ESCAPE);
	check_key(2, true, KEY_VK_F11, KEY_SC_F11); /* the first joystick resets with F11 */

	press(0, 1 << 5 | 1 << 6 | 1 << 7 | 1 << 8);
	state = poll(joystick);
	for (int i = 5; i <= 8; ++i)
		CHECK_INT(state.buttons[i], 0);
	CHECK_INT(s_keyCount, 8);
	check_key(3, false, KEY_VK_F11, KEY_SC_F11);
	check_key(4, true, KEY_VK_RIGHT, KEY_SC_RIGHT);
	check_key(5, true, KEY_VK_LEFT, KEY_SC_LEFT);
	check_key(6, true, KEY_VK_DOWN, KEY_SC_DOWN);
	check_key(7, true, KEY_VK_UP, KEY_SC_UP);
	press(0, 0);
	poll(joystick);
	CHECK_INT(s_keyCount, 12);
	check_key(11, false, KEY_VK_UP, KEY_SC_UP);
	Release(joystick);

	const int32_t second = create_device(GUID_JOYSTICK, 1);
	plug_pad(1, 4, 0, 0);
	s_keyCount = 0;
	press(1, 1);
	state = poll(second);
	CHECK_INT(state.buttons[0], 0);
	check_key(0, true, KEY_VK_F12, KEY_SC_F12); /* the second joystick resets with F12 */
	press(1, 0);
	poll(second);
	check_key(1, false, KEY_VK_F12, KEY_SC_F12);

	/* A pad that reads disconnected, as every pad does while input is
	 * disabled, releases the keys its buttons hold. */
	press(1, 1);
	poll(second);
	s_ctl[pad_word(1, PAD_CONNECTED)] = 0;
	poll(second);
	CHECK_INT(s_keyCount, 4);
	check_key(3, false, KEY_VK_F12, KEY_SC_F12);
	Release(second);
}

/* A key the input queue cannot take is posted again on the next poll, so a
 * release is never lost and no key stays stuck. */
static void test_key_queue_full(void)
{
	configure("Joystick0EscButton=0\n");
	const int32_t joystick = create_device(GUID_JOYSTICK, 0);
	plug_pad(0, 4, 0, 0);
	s_keyCount = 0;
	press(0, 1);
	poll(joystick);
	press(0, 0);
	s_keyQueueFull = true;
	poll(joystick);
	CHECK_INT(s_keyCount, 1);
	s_keyQueueFull = false;
	poll(joystick);
	CHECK_INT(s_keyCount, 2);
	check_key(1, false, KEY_VK_ESCAPE, KEY_SC_ESCAPE);
	Release(joystick);
}

static void test_hat(void)
{
	configure("Joystick0DPadButtons=1,2,3,4\n");
	const int32_t joystick = create_device(GUID_JOYSTICK, 0);
	plug_pad(0, 8, 0, 1);
	s_keyCount = 0;

	press(0, 1 << 1 | 1 << 5);
	JoyState state = poll(joystick);
	CHECK_INT(s_keyCount, 0); /* with a hat the d-pad buttons press no keys */
	CHECK_INT(state.buttons[1], 0);
	CHECK_INT(state.buttons[5], PRESSED);

	s_ctl[pad_word(0, PAD_HAT)] = HAT_UP;
	state = poll(joystick);
	CHECK_INT(state.buttons[5], 0); /* no button reports while the hat is held */
	CHECK_INT(s_keyCount, 1);
	check_key(0, true, KEY_VK_UP, KEY_SC_UP);
	s_ctl[pad_word(0, PAD_HAT)] = HAT_UP | HAT_RIGHT; /* diagonals press nothing */
	poll(joystick);
	CHECK_INT(s_keyCount, 2);
	check_key(1, false, KEY_VK_UP, KEY_SC_UP);
	const int32_t hats[3] = {HAT_RIGHT, HAT_LEFT, HAT_DOWN};
	const int32_t vks[3] = {KEY_VK_RIGHT, KEY_VK_LEFT, KEY_VK_DOWN};
	for (int i = 0; i < 3; ++i)
	{
		s_ctl[pad_word(0, PAD_HAT)] = hats[i];
		poll(joystick);
		check_key(2 + 2 * i, true, vks[i], i == 0 ? KEY_SC_RIGHT : i == 1 ? KEY_SC_LEFT : KEY_SC_DOWN);
		s_ctl[pad_word(0, PAD_HAT)] = 0;
		state = poll(joystick);
		check_key(3 + 2 * i, false, vks[i], i == 0 ? KEY_SC_RIGHT : i == 1 ? KEY_SC_LEFT : KEY_SC_DOWN);
	}
	CHECK_INT(state.buttons[5], PRESSED);
	press(0, 0);
	Release(joystick);
}

/* While the controls screen waits for an assignment, a button is reported
 * only after ASSIGN_HOLD_POLLS polls, so an axis that also reports a button
 * gets assigned as the axis. */
static void test_assign_delay(void)
{
	configure("");
	const int32_t joystick = create_device(GUID_JOYSTICK, 0);
	plug_pad(0, 4, 0, 0);
	s_assignMode = 1;
	press(0, 1);
	for (int i = 1; i < ASSIGN_HOLD_POLLS; ++i)
		CHECK_INT(poll(joystick).buttons[0], 0);
	CHECK_INT(poll(joystick).buttons[0], PRESSED);
	CHECK_INT(poll(joystick).buttons[0], PRESSED);
	press(0, 0);
	CHECK_INT(poll(joystick).buttons[0], 0);

	/* A button held part of the delay stays hidden after the screen closes
	 * until it has been released as long as it was held. */
	for (int i = 1; i < ASSIGN_HOLD_POLLS; ++i)
		poll(joystick); /* the released button winds down to zero */
	press(0, 1);
	for (int i = 0; i < 10; ++i)
		poll(joystick);
	s_assignMode = 0;
	CHECK_INT(poll(joystick).buttons[0], 0);
	press(0, 0);
	for (int i = 0; i < 10; ++i)
		poll(joystick);
	press(0, 1);
	CHECK_INT(poll(joystick).buttons[0], PRESSED);
	press(0, 0);
	Release(joystick);
}

static void set_axes(int pad, const int32_t *values, int count)
{
	for (int i = 0; i < count; ++i)
		s_ctl[pad_word(pad, PAD_AXES + i)] = values[i];
}

static void test_axes(void)
{
	configure("");
	const int32_t joystick = create_device(GUID_JOYSTICK, 0);
	plug_pad(0, 0, 6, 0);
	const int32_t values[6] = {-32768, 32767, 0, 100, -100, 5};
	set_axes(0, values, 6);
	JoyState state = poll(joystick);
	CHECK_INT(state.axes[0], 0);
	CHECK_INT(state.axes[1], 65535);
	CHECK_INT(state.axes[2], CENTRE);
	CHECK_INT(state.axes[3], CENTRE); /* lRx and lRy stay centred */
	CHECK_INT(state.axes[4], CENTRE);
	CHECK_INT(state.axes[5], CENTRE + 100); /* axes 3..5 fill lRz and the sliders */
	CHECK_INT(state.axes[6], CENTRE - 100);
	CHECK_INT(state.axes[7], CENTRE + 5);

	s_ctl[pad_word(0, PAD_NUM_AXES)] = 2;
	state = poll(joystick);
	CHECK_INT(state.axes[1], 65535);
	CHECK_INT(state.axes[2], CENTRE);
	CHECK_INT(state.axes[5], CENTRE);
	s_ctl[pad_word(0, PAD_NUM_AXES)] = 9;
	CHECK_INT(poll(joystick).axes[7], CENTRE + 5);

	/* Mapping and modes: game axis 0 reads axis 1, axis 1 is the positive
	 * half of axis 0, axis 2 the inverted half of axis 2, axis 3 is unused
	 * and axis 4 reads an axis the pad does not have. */
	configure("Joystick0Axes2=1,0,2,-1,9,5:0,1,-1,0,0,1\n");
	const int32_t halves[6] = {-32768, 32767, 32767, 7, 8, 0};
	set_axes(0, halves, 6);
	state = poll(joystick);
	CHECK_INT(state.axes[0], 65535);
	CHECK_INT(state.axes[1], CENTRE);
	CHECK_INT(state.axes[2], CENTRE);
	CHECK_INT(state.axes[5], CENTRE);
	CHECK_INT(state.axes[6], CENTRE);
	CHECK_INT(state.axes[7], CENTRE + CENTRE / 2);
	const int32_t pressed[6] = {32767, 0, -32768, 0, 0, -32768};
	set_axes(0, pressed, 6);
	state = poll(joystick);
	CHECK_INT(state.axes[1], 65535);
	CHECK_INT(state.axes[2], 65535);
	CHECK_INT(state.axes[7], CENTRE);

	/* A source axis the pad does not publish reads centred, not the word a
	 * pad with more axes left behind. */
	configure("Joystick0Axes2=5,1,2,3,4,5:0,0,0,0,0,0\n");
	s_ctl[pad_word(0, PAD_AXES + 5)] = -32768;
	s_ctl[pad_word(0, PAD_NUM_AXES)] = 2;
	CHECK_INT(poll(joystick).axes[0], CENTRE);
	s_ctl[pad_word(0, PAD_CONNECTED)] = 0;
	Release(joystick);
}

typedef struct
{
	int32_t device;
	JoyState state;
} PollJob;

static void *poll_thread(void *arg)
{
	PollJob *job = arg;
	job->state = poll(job->device);
	return NULL;
}

/* The thread that created DirectInput runs the menus. With a hat, or with
 * JoystickDisableAxesInMenu, the menus see no axes unless controls are being
 * assigned; the race thread always sees them. */
static void test_menu_axes(void)
{
	const int32_t value = 1000;
	configure("");
	const int32_t joystick = create_device(GUID_JOYSTICK, 0);
	plug_pad(0, 0, 1, 1);
	set_axes(0, &value, 1);
	CHECK_INT(poll(joystick).axes[0], CENTRE);
	s_assignMode = 1;
	CHECK_INT(poll(joystick).axes[0], CENTRE + value);
	s_assignMode = 0;

	PollJob job = {.device = joystick};
	pthread_t thread;
	CHECK_INT(pthread_create(&thread, NULL, poll_thread, &job), 0);
	CHECK_INT(pthread_join(thread, NULL), 0);
	CHECK_INT(job.state.axes[0], CENTRE + value);

	s_ctl[pad_word(0, PAD_NUM_HATS)] = 0;
	CHECK_INT(poll(joystick).axes[0], CENTRE + value);
	configure("JoystickDisableAxesInMenu=1\n");
	CHECK_INT(poll(joystick).axes[0], CENTRE);
	s_ctl[pad_word(0, PAD_CONNECTED)] = 0;
	Release(joystick);
}

static ObjectData s_events[5];

static void read_mouse(int32_t mouse, uint32_t count)
{
	memset(s_events, 0x55, sizeof s_events);
	CHECK_INT(GetDeviceData(mouse, sizeof *s_events, ARG(s_events), ARG(&count), 0), 0);
	CHECK_INT(count, 5);
}

static void move_pointer(int32_t x, int32_t y)
{
	s_ctl[CTL_MOUSE_X] = x;
	s_ctl[CTL_MOUSE_Y] = y;
	++s_ctl[CTL_MOUSE_MOVES];
}

static void test_mouse(void)
{
	s_ctl[CTL_MOUSE_MOVES] = 7;
	const int32_t mouse = create_device(GUID_MOUSE, 0);
	uint32_t three = 2;
	memset(s_events, 0x55, sizeof s_events);
	CHECK_INT(GetDeviceData(mouse, sizeof *s_events, ARG(s_events), ARG(&three), 0), 0);
	CHECK_INT(s_events[0].ofs, 0x55555555); /* fewer than three events: nothing */

	/* The first read adopts the game's cursor and reports nothing. */
	s_gameMouseX = 100;
	s_gameMouseY = 50;
	s_ctl[CTL_MOUSE_BUTTONS] = MOUSE_LEFT_LATCH;
	read_mouse(mouse, 5);
	const uint32_t offsets[5] = {0, 4, 12, 8, 8}; /* X, Y, left button, then the wheel */
	for (int i = 0; i < 5; ++i)
	{
		CHECK_INT(s_events[i].ofs, offsets[i]);
		CHECK_INT(s_events[i].data, 0);
		CHECK_INT(s_events[i].timeStamp, 0);
	}
	CHECK_INT(s_ctl[CTL_MOUSE_BUTTONS], MOUSE_LEFT_LATCH);

	/* A click shorter than a poll is reported once, from the latch. */
	read_mouse(mouse, 5);
	CHECK_INT(s_events[0].data, 0);
	CHECK_INT(s_events[2].data, 0xFFFFFFFFu);
	CHECK_INT(s_ctl[CTL_MOUSE_BUTTONS], 0);
	read_mouse(mouse, 5);
	CHECK_INT(s_events[2].data, 0);

	/* Pointer motion moves the game's cursor onto the pointer. */
	move_pointer(130, 40);
	read_mouse(mouse, 5);
	CHECK_INT(s_events[0].data, 30);
	CHECK_INT(s_events[1].data, (uint32_t)-10);
	s_gameMouseX = 130;
	s_gameMouseY = 40;
	read_mouse(mouse, 5);
	CHECK_INT(s_events[0].data, 0);
	CHECK_INT(s_events[1].data, 0);

	/* When the game moves its cursor, the next read follows it. */
	s_gameMouseX = 320;
	s_gameMouseY = 240;
	read_mouse(mouse, 5);
	CHECK_INT(s_events[0].data, 0);
	move_pointer(330, 250);
	read_mouse(mouse, 5);
	CHECK_INT(s_events[0].data, 10);
	CHECK_INT(s_events[1].data, 10);
	s_gameMouseX = 330;
	s_gameMouseY = 250;

	/* A held button is reported when it goes down, not while it stays down. */
	s_ctl[CTL_MOUSE_BUTTONS] = MOUSE_LEFT | MOUSE_LEFT_LATCH;
	read_mouse(mouse, 5);
	CHECK_INT(s_events[2].data, 0xFFFFFFFFu);
	CHECK_INT(s_ctl[CTL_MOUSE_BUTTONS], MOUSE_LEFT);
	read_mouse(mouse, 5);
	CHECK_INT(s_events[2].data, 0);
	s_ctl[CTL_MOUSE_BUTTONS] = 0;
	read_mouse(mouse, 5);
	CHECK_INT(s_events[2].data, 0);
	s_ctl[CTL_MOUSE_BUTTONS] = MOUSE_LEFT | MOUSE_LEFT_LATCH;
	read_mouse(mouse, 5);
	CHECK_INT(s_events[2].data, 0xFFFFFFFFu);
	s_ctl[CTL_MOUSE_BUTTONS] = 0;
	read_mouse(mouse, 5);

	const int32_t joystick = create_device(GUID_JOYSTICK, 0);
	uint32_t count = 5;
	memset(s_events, 0x55, sizeof s_events);
	CHECK_INT(GetDeviceData(joystick, sizeof *s_events, ARG(s_events), ARG(&count), 0), 0);
	CHECK_INT(s_events[0].ofs, 0x55555555);
	Release(joystick);
	Release(mouse);
}

/* processAxes(joystick << 20 | DIJOYSTATE word << 16 | zone): 0..255 for the
 * part of the axis the zone describes. */
static int32_t axis_control(uint32_t joy, uint32_t word, int32_t axis, uint32_t zone)
{
	s_gameAxes[joy * 16 + word] = (uint32_t)axis;
	return processAxes((int32_t)(joy << 20 | word << 16 | zone));
}

static void test_process_axes(void)
{
	configure("");
	CHECK_INT(axis_control(0, 0, 0x4000, 0x6800), 128); /* below the centre */
	CHECK_INT(axis_control(0, 0, 0, 0x6800), 255);
	CHECK_INT(axis_control(0, 0, 0x9000, 0x6800), 0);
	CHECK_INT(axis_control(0, 1, 0xC000, 0x98FF), 128); /* above the centre */
	CHECK_INT(axis_control(0, 1, 0x7000, 0x98FF), 0);
	CHECK_INT(axis_control(1, 7, 7400, 0x14B6), 100); /* the whole axis */
	CHECK_INT(axis_control(1, 5, 0xFFFF - 12800, 0x8014), 100); /* the whole axis, reversed */
	CHECK_INT(axis_control(0, 2, 0x8000, 0x1234), 0);

	configure("JoystickApplyDeadzone=1\n");
	CHECK_INT(axis_control(0, 0, 0x6800 - 780, 0x684E), 10);
	CHECK_INT(axis_control(0, 0, 0x6900, 0x684E), 0);
	CHECK_INT(axis_control(0, 0, 0x9800 + 0x1A00, 0x98B2), 84);
	CHECK_INT(axis_control(1, 7, 0x1400 + 162 * 50, 0x144A), 50);
}

int main(void)
{
	config_reset();
	CHECK_INT(DirectInputCreateA_wrap(ARG(s_game), 0, 0x500, ARG(&s_dinput), 0), 0);
	CHECK(s_dinput != 0);
	CHECK(*(GameFn **)(uintptr_t)s_dinput != NULL); /* the vtable */

	test_objects();
	test_enum();
	test_unplugged();
	test_buttons();
	test_keys();
	test_key_queue_full();
	test_hat();
	test_assign_delay();
	test_axes();
	test_menu_axes();
	test_mouse();
	test_process_axes();
	CHECK_INT(Release(s_dinput), 0);
	return check_exit();
}

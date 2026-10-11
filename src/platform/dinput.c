/* DirectInput emulation: the IDirectInputA, device and effect COM objects the
 * game uses for the mouse and two joysticks, plus the game's processAxes
 * routine. Device state comes from the control block, where JS publishes the
 * pointer and the Gamepad API state. Covers the interfaces NFSIISE's DInput.c
 * (MIT, zaps166) covers; force feedback is accepted but not rendered.
 *
 * COM layout: the game's interface pointer points at an object whose first
 * word is the vtable pointer. Every object here starts with that word, so the
 * interface pointer is the object pointer. Vtable slots hold the NFS2SE.cpp
 * "<method>_wrap" thunks, which pop the stdcall arguments and call the
 * same-named functions below. */
#include "platform.h"

#include <pthread.h>
#include <stdlib.h>
#include <string.h>

#define GUID_MOUSE 0x6F1D2B60u
#define GUID_JOYSTICK 0x6F1D2B70u
#define DIDEVTYPE_JOYSTICK 4
#define DIDC_FORCEFEEDBACK 0x100
#define DIENUM_STOP 0
#define DI_OK 0
#define DIERR_DEVICENOTREG (-1)
#define DIERR_OUTOFMEMORY ((int32_t)0x8007000Eu)

/* DIMOUSESTATE fields, as DIDEVICEOBJECTDATA.dwOfs names them */
#define DIMOFS_X 0
#define DIMOFS_Y 4
#define DIMOFS_Z 8
#define DIMOFS_BUTTON0 12

#define JOY_BUTTONS 32         /* DIJOYSTATE.rgbButtons */
#define JOY_STATE_AXES 8       /* DIJOYSTATE words before the POVs: lX..lRz and two sliders */
#define AXIS_MIN (-32768)      /* lowest browser axis value (PAD_AXES) */
#define AXIS_CENTRE 0x8000     /* DirectInput axes range over 0..AXIS_MAX */
#define AXIS_MAX 0xFFFF
#define BUTTON_DOWN 0x80       /* DIJOYSTATE button byte */
#define BUTTON_EVENT_DOWN 0xFFFFFFFFu /* DirectInput needs bit 7 for a press; every bit is set */
#define ASSIGN_HOLD_POLLS 50

typedef struct
{
	uint32_t a, b, c, d; /* b is the pad index for enumerated joysticks */
} Guid;

typedef struct
{
	uint32_t size;
	Guid guidInstance, guidProduct;
	uint32_t devType;
	char instanceName[MAX_PATH], productName[MAX_PATH];
	Guid guidFFDriver;
	uint16_t usagePage, usage;
} DeviceInstance;
_Static_assert(sizeof(DeviceInstance) == 580, "DIDEVICEINSTANCEA is 580 bytes");

typedef struct
{
	uint32_t size, flags, devType, axes, buttons, povs, ffSamplePeriod, ffMinTimeResolution, firmwareRevision,
		hardwareRevision, ffDriverVersion;
} DevCaps;
_Static_assert(sizeof(DevCaps) == 44, "DIDEVCAPS is 44 bytes");

typedef struct
{
	uint32_t ofs, data, timeStamp, sequence;
} DeviceObjectData;

typedef struct
{
	int32_t axes[JOY_STATE_AXES];
	uint32_t povs[4];
	uint8_t buttons[JOY_BUTTONS];
} JoyState;
_Static_assert(sizeof(JoyState) == 80, "DIJOYSTATE is 80 bytes");

typedef enum { OBJ_DINPUT, OBJ_DEVICE, OBJ_EFFECT } ObjectKind;

/* Common head of every COM object */
typedef struct
{
	const GameFn *vtbl;
	int32_t refs;
	ObjectKind kind;
} Object;

/* Keys a joystick presses: Escape, the car reset key, then the d-pad keys in
 * the order of JoystickDPadButtons. */
enum { KEY_ESCAPE, KEY_RESET, KEY_DPAD, JOY_KEYS = KEY_DPAD + JOY_DPAD_BUTTONS };

typedef struct
{
	Object obj;
	Guid guid; /* GUID_MOUSE, or GUID_JOYSTICK with a pad index below CTL_PAD_COUNT */
	/* mouse: the cursor position and pointer move count of the last read */
	uint32_t lastX, lastY;
	int32_t lastMoves;
	/* joystick: the emulated keys that are down */
	bool keyDown[JOY_KEYS];
} Device;

/* vtable slots used by the game */
enum { DINPUT_RELEASE = 2, DINPUT_CREATE_DEVICE = 3, DINPUT_ENUM_DEVICES = 4, DINPUT_METHODS = 8 };
enum
{
	DEV_QUERY_INTERFACE = 0, DEV_RELEASE = 2, DEV_GET_CAPABILITIES = 3, DEV_SET_PROPERTY = 6, DEV_ACQUIRE = 7,
	DEV_UNACQUIRE = 8, DEV_GET_DEVICE_STATE = 9, DEV_GET_DEVICE_DATA = 10, DEV_SET_DATA_FORMAT = 11,
	DEV_SET_EVENT_NOTIFICATION = 12, DEV_SET_COOPERATIVE_LEVEL = 13, DEV_GET_OBJECT_INFO = 14,
	DEV_CREATE_EFFECT = 18, DEV_SEND_FF_COMMAND = 22, DEV_POLL = 25, DEV_METHODS = 27
};
enum
{
	EFF_RELEASE = 2, EFF_SET_PARAMETERS = 6, EFF_START = 7, EFF_STOP = 8, EFF_DOWNLOAD = 10, EFF_UNLOAD = 11,
	EFF_METHODS = 13
};

static const GameFn s_dinputVtbl[DINPUT_METHODS] = {
	[DINPUT_RELEASE] = Release_wrap,
	[DINPUT_CREATE_DEVICE] = CreateDevice_wrap,
	[DINPUT_ENUM_DEVICES] = EnumDevices_wrap,
};

static const GameFn s_deviceVtbl[DEV_METHODS] = {
	[DEV_QUERY_INTERFACE] = QueryInterface_wrap,
	[DEV_RELEASE] = Release_wrap,
	[DEV_GET_CAPABILITIES] = GetCapabilities_wrap,
	[DEV_SET_PROPERTY] = SetProperty_wrap,
	[DEV_ACQUIRE] = Acquire_wrap,
	[DEV_UNACQUIRE] = Unacquire_wrap,
	[DEV_GET_DEVICE_STATE] = GetDeviceState_wrap,
	[DEV_GET_DEVICE_DATA] = GetDeviceData_wrap,
	[DEV_SET_DATA_FORMAT] = SetDataFormat_wrap,
	[DEV_SET_EVENT_NOTIFICATION] = SetEventNotification_wrap,
	[DEV_SET_COOPERATIVE_LEVEL] = SetCooperativeLevel_wrap,
	[DEV_GET_OBJECT_INFO] = GetObjectInfo_wrap,
	[DEV_CREATE_EFFECT] = CreateEffect_wrap,
	[DEV_SEND_FF_COMMAND] = SendForceFeedbackCommand_wrap,
	[DEV_POLL] = Poll_wrap,
};

static const GameFn s_effectVtbl[EFF_METHODS] = {
	[EFF_RELEASE] = Release_wrap,
	[EFF_SET_PARAMETERS] = SetParameters_wrap,
	[EFF_START] = Start_wrap,
	[EFF_STOP] = Stop_wrap,
	[EFF_DOWNLOAD] = Download_wrap,
	[EFF_UNLOAD] = Unload_wrap,
};

typedef struct
{
	int32_t vk, scancode;
} KeyCode;

static const KeyCode s_escapeKey = {VK_ESCAPE, SC_ESCAPE};
/* F11 resets the first player's car and F12 the second's (NFSIISE README). */
static const KeyCode s_resetKeys[CTL_PAD_COUNT] = {{VK_F11, SC_F11}, {VK_F12, SC_F12}};
static const KeyCode s_dpadKeys[JOY_DPAD_BUTTONS] = {
	{VK_RIGHT, SC_RIGHT}, {VK_LEFT, SC_LEFT}, {VK_DOWN, SC_DOWN}, {VK_UP, SC_UP}};
/* The hat position that presses each d-pad key; diagonals press none. */
static const int32_t s_dpadHat[JOY_DPAD_BUTTONS] = {HAT_RIGHT, HAT_LEFT, HAT_DOWN, HAT_UP};

/* The DIJOYSTATE word each configured axis fills, as in NFSIISE: axes 3..5 go
 * to lRz and the two sliders, so lRx and lRy (words 3 and 4) stay centred. */
static const int s_axisWords[JOY_AXES] = {0, 1, 2, 5, 6, 7};
_Static_assert(JOY_AXES <= PAD_MAX_AXES, "GetDeviceState reads source axes below JOY_AXES from the pad's axis words");

/* Mouse events GetDeviceData reports first; any further entries report the
 * wheel, which never moves. */
enum { EVENT_X, EVENT_Y, EVENT_BUTTON, MOUSE_EVENTS };
static const uint32_t s_eventOffsets[MOUSE_EVENTS] = {DIMOFS_X, DIMOFS_Y, DIMOFS_BUTTON0};

static void *s_dinputGame;      /* Game context that created DirectInput, used for callbacks */
static pthread_t s_dinputThread; /* thread that created DirectInput (the menu/message thread) */
static uint8_t s_heldPolls[CTL_PAD_COUNT][JOY_BUTTONS]; /* see button_visible */
static bool s_leftWasDown;

static void *object_new(size_t size, const GameFn *vtbl, ObjectKind kind)
{
	Object *obj = calloc(1, size);
	if (obj)
		*obj = (Object){.vtbl = vtbl, .refs = 1, .kind = kind};
	return obj;
}

/* IUnknown */

int32_t QueryInterface(Object *self, const Guid *riid, Object **out)
{
	(void)riid; /* the device also serves as IDirectInputDevice2A */
	++self->refs;
	*out = self;
	return DI_OK;
}

int32_t Release(Object *self)
{
	if (--self->refs == 0)
		free(self);
	return DI_OK;
}

/* IDirectInputA */

int32_t CreateDevice(Object *self, const Guid *guid, Device **out, void *outer)
{
	(void)self;
	(void)outer;
	if (guid->a != GUID_MOUSE && (guid->a != GUID_JOYSTICK || guid->b >= CTL_PAD_COUNT))
		return DIERR_DEVICENOTREG;
	Device *dev = object_new(sizeof *dev, s_deviceVtbl, OBJ_DEVICE);
	if (!dev)
		return DIERR_OUTOFMEMORY;
	dev->guid = *guid;
	dev->lastMoves = ctl_load(CTL_MOUSE_MOVES);
	*out = dev;
	return DI_OK;
}

/* Browsers reveal a gamepad only after its first button press, usually after
 * the game has enumerated, so every pad is listed and an absent one reads as
 * centred. */
int32_t EnumDevices(Object *self, uint32_t devType, GameFn callback, int32_t ref, uint32_t flags)
{
	(void)self;
	(void)flags;
	bool more = devType == DIDEVTYPE_JOYSTICK;
	for (uint32_t pad = 0; more && pad < CTL_PAD_COUNT; ++pad)
	{
		DeviceInstance instance = {.guidInstance = {.a = GUID_JOYSTICK, .b = pad}};
		more = wrap_stdcall2_ret(s_dinputGame, callback, (int32_t)(uintptr_t)&instance, ref) != DIENUM_STOP;
	}
	return DI_OK;
}

int32_t DirectInputCreateA_wrap(void *game, void *instance, uint32_t version, Object **out, void *outer)
{
	(void)instance;
	(void)version;
	(void)outer;
	Object *dinput = object_new(sizeof *dinput, s_dinputVtbl, OBJ_DINPUT);
	if (!dinput)
		return DIERR_OUTOFMEMORY;
	s_dinputGame = game;
	s_dinputThread = pthread_self();
	*out = dinput;
	return DI_OK;
}

/* IDirectInputDeviceA / IDirectInputDevice2A */

int32_t GetCapabilities(Device *self, DevCaps *caps)
{
	if (self->guid.a == GUID_JOYSTICK)
	{
		const uint32_t size = caps->size;
		memset(caps, 0, sizeof *caps);
		caps->size = size;
		caps->flags = DIDC_FORCEFEEDBACK;
		caps->buttons = JOY_BUTTONS;
		caps->axes = JOY_AXES;
	}
	return DI_OK;
}

int32_t SetProperty(Device *self, const Guid *prop, const void *header)
{
	(void)self;
	(void)prop;
	(void)header;
	return DI_OK;
}

int32_t Acquire(Device *self)
{
	(void)self;
	return DI_OK;
}

int32_t Unacquire(Device *self)
{
	(void)self;
	return DI_OK;
}

int32_t SetDataFormat(Device *self, const void *format)
{
	(void)self;
	(void)format;
	return DI_OK;
}

int32_t SetEventNotification(Device *self, void *event)
{
	(void)self;
	(void)event;
	return DI_OK;
}

int32_t SetCooperativeLevel(Device *self, void *hwnd, uint32_t flags)
{
	(void)self;
	(void)hwnd;
	(void)flags;
	return DI_OK;
}

/* Clears the DIDEVICEOBJECTINSTANCEA after its dwSize, bounded by dwSize. */
int32_t GetObjectInfo(Device *self, uint32_t *instance, uint32_t obj, uint32_t how)
{
	(void)self;
	(void)obj;
	(void)how;
	const uint32_t fullSize = 316;
	const uint32_t size = instance[0] >= sizeof *instance && instance[0] <= fullSize ? instance[0] : fullSize;
	memset(instance + 1, 0, size - sizeof *instance);
	return DI_OK;
}

int32_t Poll(Device *self)
{
	(void)self;
	return DI_OK;
}

static bool held(uint32_t buttonMask, int32_t button, int32_t buttons)
{
	return button >= 0 && button < buttons && (buttonMask >> button & 1);
}

/* Posts a change of an emulated key. A change the full input queue rejects is
 * posted again on the next poll, so no key stays stuck. */
static void set_key(Device *dev, int key, bool down, KeyCode code)
{
	if (dev->keyDown[key] != down && user32_post_key(down, code.vk, code.scancode))
		dev->keyDown[key] = down;
}

/* The buttons configured as Escape, the reset key and the d-pad press those
 * keys; on a pad with a hat, the hat presses the d-pad keys instead. */
static void press_keys(Device *dev, uint32_t buttonMask, int32_t buttons, bool hasHat, int32_t hat)
{
	const uint32_t joy = dev->guid.b;
	set_key(dev, KEY_ESCAPE, held(buttonMask, g_config.joystickEscButton[joy], buttons), s_escapeKey);
	set_key(dev, KEY_RESET, held(buttonMask, g_config.joystickResetButton[joy], buttons), s_resetKeys[joy]);
	for (int i = 0; i < JOY_DPAD_BUTTONS; ++i)
	{
		const bool down = hasHat ? hat == s_dpadHat[i] : held(buttonMask, g_config.joystickDPadButtons[joy][i], buttons);
		set_key(dev, KEY_DPAD + i, down, s_dpadKeys[i]);
	}
}

static bool is_key_button(uint32_t joy, int32_t button)
{
	bool used = button == g_config.joystickEscButton[joy] || button == g_config.joystickResetButton[joy];
	for (int i = 0; i < JOY_DPAD_BUTTONS; ++i)
		used |= button == g_config.joystickDPadButtons[joy][i];
	return used;
}

/* While the controls screen waits for an assignment, a button shows only
 * after ASSIGN_HOLD_POLLS polls held, so that an axis which also reports a
 * button gets assigned as the axis. A button held for part of that time stays
 * hidden afterwards until it has been released as long. */
static bool button_visible(uint8_t *heldPolls, bool down, bool assigning)
{
	if (!down && *heldPolls > 0)
		--*heldPolls;
	else if (down && assigning && *heldPolls < ASSIGN_HOLD_POLLS)
		++*heldPolls;
	return *heldPolls == ASSIGN_HOLD_POLLS || (*heldPolls == 0 && !assigning);
}

/* A browser axis value (AXIS_MIN..32767) in DirectInput's range. A half mode
 * spreads the whole travel over one side of the centre, which turns a trigger
 * or pedal into one direction of an axis: positive modes map it onto
 * centre..max, negative ones onto max..centre. */
static int32_t axis_value(int32_t raw, int32_t mode)
{
	const int32_t value = raw - AXIS_MIN;
	if (mode == 0)
		return value;
	return mode > 0 ? AXIS_CENTRE + value / 2 : AXIS_MAX - value / 2;
}

/* The menus run on the thread that created DirectInput. They get no axes when
 * the hat drives them or JoystickDisableAxesInMenu is set, except while a
 * control is being assigned. */
static bool axes_visible(bool hasHat, bool assigning)
{
	return assigning || !pthread_equal(s_dinputThread, pthread_self()) ||
	       (!hasHat && !g_config.joystickDisableAxesInMenu);
}

static int32_t pad_count(int pad, int word, int32_t limit)
{
	const int32_t count = ctl_load(pad + word);
	return count < limit ? count : limit;
}

/* Joystick only: DIJOYSTATE from the pad's control words. Buttons that press
 * keys are left out; an absent pad reads as centred with nothing pressed and
 * releases its emulated keys. */
int32_t GetDeviceState(Device *self, uint32_t size, JoyState *state)
{
	if (self->guid.a != GUID_JOYSTICK || !state || size != sizeof *state)
		return DI_OK;
	for (int i = 0; i < JOY_STATE_AXES; ++i)
		state->axes[i] = AXIS_CENTRE;
	memset(state->buttons, 0, sizeof state->buttons);

	const uint32_t joy = self->guid.b;
	const int pad = CTL_PAD_BASE + (int)joy * CTL_PAD_STRIDE;
	if (!ctl_load(pad + PAD_CONNECTED))
	{
		press_keys(self, 0, 0, false, 0);
		return DI_OK;
	}
	const uint32_t buttonMask = (uint32_t)ctl_load(pad + PAD_BUTTONS);
	const int32_t buttons = pad_count(pad, PAD_NUM_BUTTONS, JOY_BUTTONS);
	const bool hasHat = ctl_load(pad + PAD_NUM_HATS) > 0;
	const int32_t hat = hasHat ? ctl_load(pad + PAD_HAT) : 0;
	const bool assigning = *(const uint8_t *)inControlAssignMode != 0;

	press_keys(self, buttonMask, buttons, hasHat, hat);
	const int32_t reported = hat ? 0 : buttons; /* a held hat hides the buttons */
	for (int32_t i = 0; i < reported; ++i)
	{
		const bool down = buttonMask >> i & 1;
		if (!is_key_button(joy, i) && button_visible(&s_heldPolls[joy][i], down, assigning) && down)
			state->buttons[i] = BUTTON_DOWN;
	}

	if (axes_visible(hasHat, assigning))
	{
		const int32_t axes = pad_count(pad, PAD_NUM_AXES, JOY_AXES);
		for (int32_t i = 0; i < axes; ++i)
		{
			const int32_t source = g_config.joystickAxes[joy][i];
			if (source < 0)
				continue;
			const int32_t raw = source < axes ? ctl_load(pad + PAD_AXES + source) : 0;
			state->axes[s_axisWords[i]] = axis_value(raw, g_config.joystickAxisModes[joy][i]);
		}
	}
	return DI_OK;
}

/* Whether the left button went down since the last read. JS latches every
 * press, so a click shorter than one poll still counts; a held button counts
 * once. */
static bool left_button_pressed(void)
{
	const int32_t buttons = __atomic_fetch_and(&web_ctl()[CTL_MOUSE_BUTTONS], ~MOUSE_LEFT_LATCH, __ATOMIC_SEQ_CST);
	const bool down = (buttons & (MOUSE_LEFT | MOUSE_LEFT_LATCH)) != 0;
	const bool pressed = down && !s_leftWasDown;
	s_leftWasDown = down;
	return pressed;
}

/* Mouse only. The game keeps its own cursor and reads relative motion; the
 * delta reported here moves it onto the absolute browser pointer position.
 * Browsers cannot warp the pointer, so when the game moves its cursor the
 * pointer and the cursor stay apart until the pointer moves again. The button
 * reads as pressed for one read per press. */
int32_t GetDeviceData(Device *self, uint32_t objectSize, DeviceObjectData *data, uint32_t *inOut, uint32_t flags)
{
	(void)objectSize;
	(void)flags;
	if (self->guid.a != GUID_MOUSE || !data || !inOut || *inOut < MOUSE_EVENTS)
		return DI_OK;
	for (uint32_t i = 0; i < *inOut; ++i)
		data[i] = (DeviceObjectData){.ofs = i < MOUSE_EVENTS ? s_eventOffsets[i] : DIMOFS_Z};

	const uint32_t gameX = *(const uint32_t *)mousePositionX;
	const uint32_t gameY = *(const uint32_t *)mousePositionY;
	if (gameX != self->lastX || gameY != self->lastY)
	{
		self->lastX = gameX;
		self->lastY = gameY;
		return DI_OK;
	}

	const int32_t moves = ctl_load(CTL_MOUSE_MOVES);
	if (moves != self->lastMoves)
	{
		self->lastMoves = moves;
		self->lastX = (uint32_t)ctl_load(CTL_MOUSE_X);
		self->lastY = (uint32_t)ctl_load(CTL_MOUSE_Y);
		data[EVENT_X].data = self->lastX - gameX;
		data[EVENT_Y].data = self->lastY - gameY;
	}
	if (left_button_pressed())
		data[EVENT_BUTTON].data = BUTTON_EVENT_DOWN;
	return DI_OK;
}

int32_t CreateEffect(Device *self, const Guid *guid, const void *params, Object **out, void *outer)
{
	(void)self;
	(void)guid;
	(void)params;
	(void)outer;
	Object *effect = object_new(sizeof *effect, s_effectVtbl, OBJ_EFFECT);
	if (!effect)
		return DIERR_OUTOFMEMORY;
	*out = effect;
	return DI_OK;
}

int32_t SendForceFeedbackCommand(Device *self, uint32_t command)
{
	(void)self;
	(void)command;
	return DI_OK;
}

/* IDirectInputEffect: no haptics in the browser build yet. */

int32_t SetParameters(Object *self, const void *params, uint32_t flags)
{
	(void)self;
	(void)params;
	(void)flags;
	return DI_OK;
}

int32_t Start(Object *self, uint32_t iterations, uint32_t flags)
{
	(void)self;
	(void)iterations;
	(void)flags;
	return DI_OK;
}

int32_t Stop(Object *self)
{
	(void)self;
	return DI_OK;
}

int32_t Download(Object *self)
{
	(void)self;
	return DI_OK;
}

int32_t Unload(Object *self)
{
	(void)self;
	return DI_OK;
}

/* The game's own axis-to-control mapping, replaced because it assumes the
 * Windows joystick ranges. Bits 20 and up of inputValue select the joystick,
 * bits 16..18 the DIJOYSTATE word, and bits 0..15 hold two zone bytes; the
 * result is 0..255. The game reads 16 words per joystick at dword_557540.
 *
 * A zone byte places the far end of a range at a quarter or three quarters of
 * the travel: 0x1A/0x4E below the centre, 0xB2/0xE6 above it and 0x4A/0xB6 on
 * the whole axis. NFSIISE exchanges each pair, and so does this port, so the
 * controls respond as they do there. */

static int32_t swap_values(int32_t v, int32_t a, int32_t b)
{
	return v == a ? b : v == b ? a : v;
}

static int32_t scale(int32_t distance, int32_t range)
{
	return range ? distance / range : 0;
}

int32_t processAxes(uint32_t inputValue)
{
	const uint32_t *axes = dword_557540;
	int32_t axis = (int32_t)axes[((inputValue >> 20) << 4) + ((inputValue >> 16) & 7)];
	int32_t hi = (int32_t)(inputValue & 0xFF00);
	int32_t lo = (int32_t)(inputValue & 0xFF) << 8;
	const bool deadzone = g_config.joystickApplyDeadzone;
	int32_t value = 0;

	if (hi == 0x6800) /* below the centre */
	{
		const int32_t centre = deadzone ? hi : AXIS_CENTRE;
		lo = swap_values(lo, 0x4E00, 0x1A00);
		if (axis < centre)
			value = scale(centre - axis, (centre - lo) >> 8);
	}
	else if (hi == 0x9800) /* above the centre */
	{
		const int32_t centre = deadzone ? hi : AXIS_CENTRE;
		lo = swap_values(lo, 0xB200, 0xE600) + 0x100;
		if (axis > centre)
			value = scale(axis - centre, (lo - centre) >> 8);
	}
	else if (hi == 0x1400 || lo == 0x1400) /* the whole axis, possibly reversed */
	{
		if (lo == 0x1400)
		{
			axis = AXIS_MAX - axis;
			lo = hi;
			hi = 0x1400;
		}
		lo = swap_values(lo, 0xB600, 0x4A00);
		const int32_t start = deadzone ? hi : 0;
		value = scale(axis - start, (lo - start) >> 8);
	}
	return value < 0 ? 0 : value > 255 ? 255 : value;
}

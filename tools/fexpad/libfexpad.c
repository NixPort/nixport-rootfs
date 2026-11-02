// libfexpad: LD_PRELOAD shim that gives a native (ARM64) guest a virtual gamepad.
//
// The guest is an SDL3 program. SDL3 can register a virtual joystick inside the
// process that asks for it, and it then shows up in SDL_GetGamepads() like any
// other gamepad. NXP2 is selected when NXP_PAD_SOCKET is set; otherwise the
// legacy FXP1 path scans the host's event nodes and merges their input.
//
// One device, many sources: a Bluetooth pad and the on-screen controller are
// separate slots on the host, but a game sees a single gamepad whose state is
// the merge of all of them (buttons OR, sticks last non-center wins, triggers
// max) - the same merge the pad state file uses. Games that only open the first
// controller (gow's runtime_pad.c current_gamepad) then work no matter which
// slots are live.
//
// Host side (app/src/main/cpp/pad_bridge.c) serves every slot as a unix socket
// in <rootfs>/usr/share/fex-emu/fexdroid-input/event<N> (N = 200 + slot). The
// guest announces its struct input_event layout with the "FXP1" hello and then
// receives runs of input_events in the kernel's xpad layout (the same codes
// patches/fex/0006-android-gamepads.patch presents to x86 guests).
//
// The device identity is the x86 track's: "Microsoft X-Box 360 pad", 045e:028e,
// so one game behaves the same on both tracks.

#define _GNU_SOURCE
#include <dirent.h>
#include <dlfcn.h>
#include <errno.h>
#include <float.h>
#include <fcntl.h>
#include <limits.h>
#include <math.h>
#include <poll.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/un.h>
#include <time.h>
#include <unistd.h>
#include "nxp2_protocol.h"

#if defined(__linux__)
#include <sys/inotify.h>
#define FXD_PAD_INOTIFY 1
#endif

// The host serves SOCK_SEQPACKET (message boundaries = one event packet each).
// The PC test host (macOS) has no SOCK_SEQPACKET; datagrams keep the message
// boundaries the protocol needs, and the client names itself so the host can
// answer it (macOS has no autobind for unix datagram sockets).
#if defined(__linux__)
#define FXD_PAD_SOCK_TYPE SOCK_SEQPACKET
#else
#define FXD_PAD_SOCK_TYPE SOCK_DGRAM
#define FXD_PAD_CLIENT_BIND 1
#endif

#ifndef SOCK_CLOEXEC
#define SOCK_CLOEXEC 0
#endif

#define FXD_PAD_TAG "fexpad"
#define FXD_PAD_MAX_SLOTS 8
#define FXD_PAD_HELLO_MAGIC 0x31505846u // "FXP1"
#define FXD_PAD_EVENT_SIZE 24             // struct input_event on 64-bit Linux
#define FXD_PAD_FIRST_NODE 200

// xpad codes (kernel drivers/input/xpad.c) as the host sends them.
#define FXD_EV_SYN 0
#define FXD_EV_KEY 1
#define FXD_EV_ABS 3

#define FXD_XPAD_BTN_A 0x130
#define FXD_XPAD_BTN_B 0x131
#define FXD_XPAD_BTN_X 0x133
#define FXD_XPAD_BTN_Y 0x134
#define FXD_XPAD_BTN_TL 0x136
#define FXD_XPAD_BTN_TR 0x137
#define FXD_XPAD_BTN_SELECT 0x13a
#define FXD_XPAD_BTN_START 0x13b
#define FXD_XPAD_BTN_MODE 0x13c
#define FXD_XPAD_BTN_THUMBL 0x13d
#define FXD_XPAD_BTN_THUMBR 0x13e

#define FXD_ABS_X 0x00
#define FXD_ABS_Y 0x01
#define FXD_ABS_Z 0x02
#define FXD_ABS_RX 0x03
#define FXD_ABS_RY 0x04
#define FXD_ABS_RZ 0x05
#define FXD_ABS_HAT0X 0x10
#define FXD_ABS_HAT0Y 0x11

// SDL_GamepadButton order; the virtual joystick's button index is the same
// (SDL's own gamepad mapping for virtual joysticks binds them in this order).
enum {
    FXD_BTN_SOUTH = 0,
    FXD_BTN_EAST,
    FXD_BTN_WEST,
    FXD_BTN_NORTH,
    FXD_BTN_BACK,
    FXD_BTN_GUIDE,
    FXD_BTN_START,
    FXD_BTN_LEFT_STICK,
    FXD_BTN_RIGHT_STICK,
    FXD_BTN_LEFT_SHOULDER,
    FXD_BTN_RIGHT_SHOULDER,
    FXD_BTN_DPAD_UP,
    FXD_BTN_DPAD_DOWN,
    FXD_BTN_DPAD_LEFT,
    FXD_BTN_DPAD_RIGHT,
    FXD_BTN_COUNT
};

// SDL_GamepadAxis order.
enum {
    FXD_AXIS_LEFTX = 0,
    FXD_AXIS_LEFTY,
    FXD_AXIS_RIGHTX,
    FXD_AXIS_RIGHTY,
    FXD_AXIS_LEFT_TRIGGER,
    FXD_AXIS_RIGHT_TRIGGER,
    FXD_AXIS_COUNT
};

#define FXD_TRIGGER_REST (-32768)

// Mirror SDL3's SDL_VirtualJoystickTouchpadDesc and SDL_VirtualJoystickDesc.
// SDL reads these structures by offset, so keep their ABI checked below.
typedef struct {
    uint16_t nfingers;
    uint16_t padding[3];
} FxdVirtualJoystickTouchpadDesc;

typedef struct {
    uint8_t data[16];
} FxdGuid;

typedef struct {
    uint32_t version;
    uint16_t type;
    uint16_t padding;
    uint16_t vendor_id;
    uint16_t product_id;
    uint16_t naxes;
    uint16_t nbuttons;
    uint16_t nballs;
    uint16_t nhats;
    uint16_t ntouchpads;
    uint16_t nsensors;
    uint16_t padding2[2];
    uint32_t button_mask;
    uint32_t axis_mask;
    const char *name;
    const FxdVirtualJoystickTouchpadDesc *touchpads;
    const void *sensors;
    void *userdata;
    void (*Update)(void *userdata);
    void (*SetPlayerIndex)(void *userdata, int player_index);
    bool (*Rumble)(void *userdata, uint16_t low_frequency_rumble, uint16_t high_frequency_rumble);
    bool (*RumbleTriggers)(void *userdata, uint16_t left_rumble, uint16_t right_rumble);
    bool (*SetLED)(void *userdata, uint8_t red, uint8_t green, uint8_t blue);
    bool (*SendEffect)(void *userdata, const void *data, int size);
    bool (*SetSensorsEnabled)(void *userdata, bool enabled);
    void (*Cleanup)(void *userdata);
} FxdVirtualJoystickDesc;

_Static_assert(sizeof(void *) == 8, "SDL virtual joystick ABI requires 64-bit pointers");
_Static_assert(sizeof(FxdVirtualJoystickTouchpadDesc) == 8, "SDL virtual touchpad descriptor size");
_Static_assert(sizeof(FxdGuid) == 16, "SDL GUID size");
_Static_assert(sizeof(float) == 4 && FLT_RADIX == 2 && FLT_MANT_DIG == 24,
               "NXP2 touch coordinates require IEEE-754 binary32");
_Static_assert(offsetof(FxdVirtualJoystickDesc, type) == 4, "SDL virtual joystick type offset");
_Static_assert(offsetof(FxdVirtualJoystickDesc, vendor_id) == 8, "SDL virtual joystick vendor offset");
_Static_assert(offsetof(FxdVirtualJoystickDesc, ntouchpads) == 20, "SDL virtual joystick touchpad count offset");
_Static_assert(offsetof(FxdVirtualJoystickDesc, button_mask) == 28, "SDL virtual joystick button mask offset");
_Static_assert(offsetof(FxdVirtualJoystickDesc, name) == 40, "SDL virtual joystick name offset");
_Static_assert(offsetof(FxdVirtualJoystickDesc, touchpads) == 48, "SDL virtual joystick touchpads offset");
_Static_assert(offsetof(FxdVirtualJoystickDesc, userdata) == 64, "SDL virtual joystick userdata offset");
_Static_assert(offsetof(FxdVirtualJoystickDesc, Update) == 72, "SDL virtual joystick callbacks offset");
_Static_assert(sizeof(FxdVirtualJoystickDesc) == 136, "SDL virtual joystick descriptor size");

// SDL3 symbols, resolved at run time: the shim is built without SDL, and the
// guest's own SDL3 is the one it must share.
static struct {
    int (*InitSubSystem)(uint32_t flags);
    uint32_t (*JoystickAttachVirtual)(const void *desc);
    void *(*OpenJoystick)(uint32_t instance_id);
    void (*CloseJoystick)(void *joystick);
    int (*DetachVirtualJoystick)(uint32_t instance_id);
    int (*SetJoystickVirtualAxis)(void *joystick, int axis, int16_t value);
    int (*SetJoystickVirtualButton)(void *joystick, int button, uint8_t down);
    bool (*SetJoystickVirtualTouchpad)(void *joystick, int touchpad, int finger, bool down,
                                       float x, float y, float pressure);
    FxdGuid (*GetJoystickGUIDForID)(uint32_t instance_id);
    void (*GUIDToString)(FxdGuid guid, char *text, int text_size);
    int (*AddGamepadMapping)(const char *mapping);
    int (*GetGamepadButtonFromString)(const char *name);
    const char *(*GetError)(void);
    uint16_t (*GetJoystickVendorForID)(uint32_t instance_id);
    uint16_t (*GetJoystickProductForID)(uint32_t instance_id);
    const char *(*GetJoystickNameForID)(uint32_t instance_id);
} sdl;

#define SDL_INIT_JOYSTICK_HINT 0x00002000u
#define SDL_JOYSTICK_TYPE_GAMEPAD_HINT 1
#define SDL_GAMEPAD_BUTTON_TOUCHPAD_HINT 20
#define FXD_VENDOR_XBOX 0x045e
#define FXD_PRODUCT_XBOX360 0x028e

static const FxdVirtualJoystickTouchpadDesc g_nxp2_touchpad_desc = {
    .nfingers = NXP2_MAX_TOUCHES
};

// One state per host slot: what that pad last reported. The virtual gamepad's
// state is the merge of every live slot (see merge_and_push).
typedef struct {
    int slot;         // 0..n-1 (node number - 200)
    int node;         // 200 + slot
    int fd;           // socket to the host, -1 when not connected
    time_t next_try;  // backoff between connection attempts
    char client_path[PATH_MAX]; // the name this client gave itself (test hosts)
    uint8_t btn[FXD_BTN_COUNT];
    int16_t ax[FXD_AXIS_COUNT];
    int hx, hy;       // last hat direction, so a change releases the old one
} Slot;

typedef struct {
    int instance;
    void *joystick;
    uint8_t buttons[FXD_BTN_COUNT];
    int16_t axes[FXD_AXIS_COUNT];
    uint8_t touch_active[NXP2_MAX_TOUCHES];
    uint16_t touch_ids[NXP2_MAX_TOUCHES];
    float touch_x[NXP2_MAX_TOUCHES];
    float touch_y[NXP2_MAX_TOUCHES];
    uint8_t touchpad_click;
    // 震动状态：SDL 的震动回调只记录当前强度，发包由读线程按节拍做
    // （回调跑在游戏的线程上，绝不能在那里发包）。
    // The rumble state: SDL's rumble callback only records the current
    // intensity; the reader thread does the sending (the callback runs on the
    // game's own thread and must never block on a socket write).
    uint16_t rumble_strong;   // 当前强度（SDL 回调写）
    uint16_t rumble_weak;
    uint16_t rumble_sent_strong; // 上次发出的强度（读线程写）
    uint16_t rumble_sent_weak;
    uint64_t rumble_sent_ms;   // 上次发出的时刻（心跳节拍用）
} Nxp2Player;

typedef struct {
    int present;
    int active;
    uint32_t buttons;
    int16_t axes[FXD_AXIS_COUNT];
    uint8_t touch_count;
    uint16_t touch_ids[NXP2_MAX_TOUCHES];
    float touch_x[NXP2_MAX_TOUCHES];
    float touch_y[NXP2_MAX_TOUCHES];
    uint8_t touchpad_click;
} Nxp2SnapshotPlayer;

static Slot g_slots[FXD_PAD_MAX_SLOTS];
// The single virtual gamepad and its merged state.
static uint32_t g_instance;
static void *g_joystick;
static uint8_t g_btn[FXD_BTN_COUNT];
static int16_t g_ax[FXD_AXIS_COUNT];
// Startup owns SDL until the selected reader thread starts; teardown follows its join.
static pthread_t g_thread;
static int g_thread_started;
static pthread_t g_retry_thread;
static int g_retry_started;
static int g_watch_fd = -1;
static char g_dir[PATH_MAX];
static int g_debug;
static int g_nxp2_requested;
static int g_nxp2_fd = -1;
static uint64_t g_nxp2_generation;
static uint64_t g_nxp2_rx_sequence;
static uint64_t g_nxp2_tx_sequence;
static uint32_t g_nxp2_capabilities;
static uint8_t g_nxp2_player_limit = 1;
// 协商出来的协议版本：0 = 还没握手。震动报文只在 >= 3 时存在。
// The negotiated protocol version: 0 means "not negotiated yet". The rumble
// message only exists at >= 3.
static uint16_t g_nxp2_version;
// 出站帧的锁：序号自增与发送必须在同一把锁里，否则两个线程可能取到同一个
// 序号，宿主会判 BAD_SEQUENCE 而断开连接。
// The outbound lock: taking the next sequence number and sending it must
// happen together, or two threads can pick the same number and the host
// drops the connection with BAD_SEQUENCE.
static pthread_mutex_t g_nxp2_tx_lock = PTHREAD_MUTEX_INITIALIZER;
// 震动状态的锁：只在复制状态时持有，绝不跨发包持有。
// The rumble state lock: held only while copying the state, never across a
// send.
static pthread_mutex_t g_nxp2_rumble_lock = PTHREAD_MUTEX_INITIALIZER;
// 震动续发的节拍：SDL 只在强度变化时回调驱动，持续效果每 2 秒才续发一次，
// 宿主需要更细的心跳才能维持马达。
// The rumble heartbeat: SDL only calls the driver when the intensity
// changes and re-sends the same intensity every 2 s, so the host needs a
// finer heartbeat to keep the motor running.
#define NXP2_RUMBLE_HEARTBEAT_MS 100
static Nxp2Player g_nxp2_players[NXP2_MAX_PLAYERS];
static char g_nxp2_endpoint[sizeof(((struct sockaddr_un *)0)->sun_path)];
static char g_nxp2_client_path[PATH_MAX];

static void logf_(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    fputc('\n', stderr);
    fflush(stderr);
}

// Lifecycle and degradation reasons log always: they reach the session log, and a
// gamepad that is silently missing is the hardest kind of bug to file.
#define LOGI(...)                                                                                  \
    do {                                                                                           \
        logf_(FXD_PAD_TAG ": " __VA_ARGS__);                                                        \
    } while (0)

// Per-event and retry noise: only with FXD_PAD_SHIM_DEBUG=1.
#define LOGV(...)                                                                                  \
    do {                                                                                           \
        if (g_debug) logf_(FXD_PAD_TAG ": " __VA_ARGS__);                                           \
    } while (0)

static void exe_name(char *out, size_t len) {
    char path[PATH_MAX];
    ssize_t n = readlink("/proc/self/exe", path, sizeof path - 1);
    if (n <= 0) { snprintf(out, len, "?"); return; }
    path[n] = 0;
    const char *name = strrchr(path, '/');
    snprintf(out, len, "%s", name ? name + 1 : path);
}

// FEX guests get their pads from patch 0006 (/dev/input). The shim is preloaded
// into whatever process the launch chain runs - including FEXInterpreter for an
// x86 program - and must not add a second pad there.
static int inside_fex(void) {
    char path[PATH_MAX];
    ssize_t n = readlink("/proc/self/exe", path, sizeof path - 1);
    if (n <= 0) return 0;
    path[n] = 0;
    const char *name = strrchr(path, '/');
    name = name ? name + 1 : path;
    return strncmp(name, "FEX", 3) == 0;
}

// xpad event code -> SDL virtual joystick button index (-1: not a button).
static int button_of(uint16_t code) {
    switch (code) {
    case FXD_XPAD_BTN_A: return FXD_BTN_SOUTH;
    case FXD_XPAD_BTN_B: return FXD_BTN_EAST;
    case FXD_XPAD_BTN_X: return FXD_BTN_WEST;
    case FXD_XPAD_BTN_Y: return FXD_BTN_NORTH;
    case FXD_XPAD_BTN_SELECT: return FXD_BTN_BACK;
    case FXD_XPAD_BTN_MODE: return FXD_BTN_GUIDE;
    case FXD_XPAD_BTN_START: return FXD_BTN_START;
    case FXD_XPAD_BTN_THUMBL: return FXD_BTN_LEFT_STICK;
    case FXD_XPAD_BTN_THUMBR: return FXD_BTN_RIGHT_STICK;
    case FXD_XPAD_BTN_TL: return FXD_BTN_LEFT_SHOULDER;
    case FXD_XPAD_BTN_TR: return FXD_BTN_RIGHT_SHOULDER;
    default: return -1;
    }
}

// xpad absolute axis -> SDL virtual joystick axis index (-1: not an axis).
static int axis_of(uint16_t code) {
    switch (code) {
    case FXD_ABS_X: return FXD_AXIS_LEFTX;
    case FXD_ABS_Y: return FXD_AXIS_LEFTY;
    case FXD_ABS_RX: return FXD_AXIS_RIGHTX;
    case FXD_ABS_RY: return FXD_AXIS_RIGHTY;
    case FXD_ABS_Z: return FXD_AXIS_LEFT_TRIGGER;
    case FXD_ABS_RZ: return FXD_AXIS_RIGHT_TRIGGER;
    default: return -1;
    }
}

// The host sends the triggers as 0..255. SDL's joystick axis for a trigger
// spans -32768 (released) .. 32767 (full) - the same range the evdev driver
// produces (AxisCorrect maps the device's 0..255 onto it) - and SDL's gamepad
// mapping then turns that into the 0..32767 the game sees.
static int16_t trigger_scale(int32_t value) {
    if (value < 0) value = 0;
    if (value > 255) value = 255;
    return (int16_t)(-32768 + (value * 65535 + 127) / 255);
}

static int slot_live(const Slot *s) { return s->fd >= 0; }

// Recomputes the virtual gamepad's state from every live slot and pushes what
// changed: buttons OR, sticks last non-center wins, triggers max - the same
// merge the pad state file uses.
static void merge_and_push(void) {
    uint8_t btn[FXD_BTN_COUNT];
    int16_t ax[FXD_AXIS_COUNT];
    memset(btn, 0, sizeof btn);
    for (int a = 0; a < 4; a++) ax[a] = 0;
    ax[FXD_AXIS_LEFT_TRIGGER] = FXD_TRIGGER_REST;
    ax[FXD_AXIS_RIGHT_TRIGGER] = FXD_TRIGGER_REST;
    for (int i = 0; i < FXD_PAD_MAX_SLOTS; i++) {
        const Slot *s = &g_slots[i];
        if (!slot_live(s)) continue;
        for (int b = 0; b < FXD_BTN_COUNT; b++) btn[b] |= s->btn[b];
        for (int a = 0; a < 4; a++)
            if (s->ax[a] != 0) ax[a] = s->ax[a];
        if (s->ax[4] > ax[4]) ax[4] = s->ax[4];
        if (s->ax[5] > ax[5]) ax[5] = s->ax[5];
    }
    for (int b = 0; b < FXD_BTN_COUNT; b++) {
        if (btn[b] == g_btn[b]) continue;
        g_btn[b] = btn[b];
        if (g_joystick) sdl.SetJoystickVirtualButton(g_joystick, b, g_btn[b]);
    }
    for (int a = 0; a < FXD_AXIS_COUNT; a++) {
        if (ax[a] == g_ax[a]) continue;
        g_ax[a] = ax[a];
        if (g_joystick) sdl.SetJoystickVirtualAxis(g_joystick, a, g_ax[a]);
    }
}

// One packet = a run of input_events in the layout the hello announced: the
// slot's own state is updated, then the merge is recomputed and pushed.
static void handle_packet(Slot *s, const uint8_t *data, size_t len) {
    size_t n = len / FXD_PAD_EVENT_SIZE;
    for (size_t i = 0; i < n; i++) {
        const uint8_t *ev = data + i * FXD_PAD_EVENT_SIZE;
        uint16_t type = (uint16_t)(ev[16] | (ev[17] << 8));
        uint16_t code = (uint16_t)(ev[18] | (ev[19] << 8));
        int32_t value = (int32_t)((uint32_t)ev[20] | ((uint32_t)ev[21] << 8) | ((uint32_t)ev[22] << 16) |
                                  ((uint32_t)ev[23] << 24));
        switch (type) {
        case FXD_EV_KEY: {
            int b = button_of(code);
            if (b >= 0) s->btn[b] = value != 0;
            break;
        }
        case FXD_EV_ABS:
            if (code == FXD_ABS_HAT0X) {
                int v = value < 0 ? -1 : (value > 0 ? 1 : 0);
                if (v != s->hx) {
                    s->btn[FXD_BTN_DPAD_LEFT] = v < 0;
                    s->btn[FXD_BTN_DPAD_RIGHT] = v > 0;
                    s->hx = v;
                }
            } else if (code == FXD_ABS_HAT0Y) {
                int v = value < 0 ? -1 : (value > 0 ? 1 : 0);
                if (v != s->hy) {
                    s->btn[FXD_BTN_DPAD_UP] = v < 0;
                    s->btn[FXD_BTN_DPAD_DOWN] = v > 0;
                    s->hy = v;
                }
            } else {
                int index = axis_of(code);
                if (index < 0) break;
                if (index == FXD_AXIS_LEFT_TRIGGER || index == FXD_AXIS_RIGHT_TRIGGER) {
                    s->ax[index] = trigger_scale(value);
                } else {
                    if (value < -32768) value = -32768;
                    if (value > 32767) value = 32767;
                    s->ax[index] = (int16_t)value;
                }
            }
            break;
        default:
            break;
        }
    }
    merge_and_push();
}

static void slot_reset_state(Slot *s) {
    memset(s->btn, 0, sizeof s->btn);
    for (int a = 0; a < 4; a++) s->ax[a] = 0;
    s->ax[FXD_AXIS_LEFT_TRIGGER] = FXD_TRIGGER_REST;
    s->ax[FXD_AXIS_RIGHT_TRIGGER] = FXD_TRIGGER_REST;
    s->hx = 0;
    s->hy = 0;
}

// Registers the one virtual gamepad (once) with the guest's own SDL.
static int ensure_virtual(void) {
    if (g_instance) return 1;
    FxdVirtualJoystickDesc desc;
    memset(&desc, 0, sizeof desc);
    desc.version = sizeof desc;
    desc.type = SDL_JOYSTICK_TYPE_GAMEPAD_HINT;
    desc.vendor_id = FXD_VENDOR_XBOX;
    desc.product_id = FXD_PRODUCT_XBOX360;
    desc.naxes = FXD_AXIS_COUNT;
    desc.nbuttons = FXD_BTN_COUNT;
    desc.name = "Microsoft X-Box 360 pad";
    g_instance = sdl.JoystickAttachVirtual(&desc);
    if (!g_instance) {
        LOGI("SDL refused the virtual joystick: %s", sdl.GetError());
        return 0;
    }
    g_joystick = sdl.OpenJoystick(g_instance);
    if (!g_joystick) {
        LOGI("SDL_OpenJoystick failed: %s", sdl.GetError());
        sdl.DetachVirtualJoystick(g_instance);
        g_instance = 0;
        return 0;
    }
    // SDL starts every axis at 0, which its gamepad mapping reports as a
    // half-pressed trigger: put the triggers on their rest value at once.
    g_ax[FXD_AXIS_LEFT_TRIGGER] = FXD_TRIGGER_REST;
    g_ax[FXD_AXIS_RIGHT_TRIGGER] = FXD_TRIGGER_REST;
    sdl.SetJoystickVirtualAxis(g_joystick, FXD_AXIS_LEFT_TRIGGER, FXD_TRIGGER_REST);
    sdl.SetJoystickVirtualAxis(g_joystick, FXD_AXIS_RIGHT_TRIGGER, FXD_TRIGGER_REST);
    LOGI("virtual gamepad %u (\"Microsoft X-Box 360 pad\", 045e:028e) registered",
         g_instance);
    return 1;
}

static void drop_virtual(void) {
    if (g_joystick) {
        sdl.CloseJoystick(g_joystick);
        g_joystick = NULL;
    }
    if (g_instance) {
        sdl.DetachVirtualJoystick(g_instance);
        g_instance = 0;
    }
}

// SDL 的虚拟手柄震动回调：只记录当前强度，不发包。
// SDL's virtual joystick rumble callback: records the intensity only.
// 发包在读线程按节拍做（见 nxp2_pump_rumble）——这个回调跑在游戏的线程上，
// socket 有 2 秒发送超时，在这里发包会把游戏卡住。
// The send happens in the reader thread on a fixed beat (see
// nxp2_pump_rumble): this callback runs on the game's own thread and the
// socket has a 2 s send timeout, so sending here would stall the game.
static bool nxp2_rumble_callback(void *userdata, uint16_t low_frequency_rumble,
                                  uint16_t high_frequency_rumble) {
    Nxp2Player *player = (Nxp2Player *)userdata;
    if (!player) return false;
    pthread_mutex_lock(&g_nxp2_rumble_lock);
    player->rumble_strong = low_frequency_rumble;
    player->rumble_weak = high_frequency_rumble;
    pthread_mutex_unlock(&g_nxp2_rumble_lock);
    return true;
}

static void nxp2_fill_descriptor(FxdVirtualJoystickDesc *desc, uint32_t capabilities,
                                 int player_index) {
    memset(desc, 0, sizeof *desc);
    desc->version = sizeof *desc;
    desc->type = SDL_JOYSTICK_TYPE_GAMEPAD_HINT;
    desc->naxes = FXD_AXIS_COUNT;
    desc->axis_mask = (1u << FXD_AXIS_COUNT) - 1;
    desc->nbuttons = (capabilities & NXP2_CAP_TOUCH_CLICK)
                         ? SDL_GAMEPAD_BUTTON_TOUCHPAD_HINT + 1
                         : FXD_BTN_COUNT;
    // 震动回调：只有协商到 v3 才有这个报文类型，才挂回调；否则 SDL 如实
    // 报告「不支持震动」，游戏不会去尝试。
    // The rumble callback: it exists only from v3 on (the message type does);
    // without it SDL reports "no rumble" and the game never tries.
    if (player_index >= 0 && player_index < NXP2_MAX_PLAYERS && g_nxp2_version >= 3) {
        desc->Rumble = nxp2_rumble_callback;
        desc->userdata = &g_nxp2_players[player_index];
    }
    if (capabilities & (NXP2_CAP_TOUCH_CONTACTS | NXP2_CAP_TOUCH_CLICK)) {
        desc->name = "Nixport Standard Gamepad";
        if (capabilities & NXP2_CAP_TOUCH_CLICK) {
            desc->button_mask = NXP2_BUTTON_MASK | (1u << SDL_GAMEPAD_BUTTON_TOUCHPAD_HINT);
        }
        if (capabilities & NXP2_CAP_TOUCH_CONTACTS) {
            desc->ntouchpads = 1;
            desc->touchpads = &g_nxp2_touchpad_desc;
        }
    } else {
        desc->vendor_id = FXD_VENDOR_XBOX;
        desc->product_id = FXD_PRODUCT_XBOX360;
        desc->name = "Microsoft X-Box 360 pad";
    }
}

static int nxp2_attach_player(int player_index) {
    Nxp2Player *player = &g_nxp2_players[player_index];
    if (player->instance) return 1;
    FxdVirtualJoystickDesc desc;
    nxp2_fill_descriptor(&desc, g_nxp2_capabilities, player_index);
    player->instance = sdl.JoystickAttachVirtual(&desc);
    if (!player->instance) {
        LOGI("SDL refused the virtual gamepad for player %d: %s", player_index + 1,
             sdl.GetError());
        return 0;
    }
    player->joystick = sdl.OpenJoystick(player->instance);
    if (!player->joystick) {
        LOGI("SDL_OpenJoystick failed for player %d: %s", player_index + 1, sdl.GetError());
        sdl.DetachVirtualJoystick(player->instance);
        player->instance = 0;
        return 0;
    }
    memset(player->buttons, 0, sizeof player->buttons);
    for (int axis = 0; axis < 4; axis++) player->axes[axis] = 0;
    player->axes[FXD_AXIS_LEFT_TRIGGER] = FXD_TRIGGER_REST;
    player->axes[FXD_AXIS_RIGHT_TRIGGER] = FXD_TRIGGER_REST;
    sdl.SetJoystickVirtualAxis(player->joystick, FXD_AXIS_LEFT_TRIGGER, FXD_TRIGGER_REST);
    sdl.SetJoystickVirtualAxis(player->joystick, FXD_AXIS_RIGHT_TRIGGER, FXD_TRIGGER_REST);
    LOGI("virtual gamepad %u for player %d registered", player->instance, player_index + 1);
    return 1;
}

static void nxp2_detach_player(int player_index) {
    Nxp2Player *player = &g_nxp2_players[player_index];
    if (player->joystick) {
        sdl.CloseJoystick(player->joystick);
        player->joystick = NULL;
    }
    if (player->instance) {
        sdl.DetachVirtualJoystick(player->instance);
        player->instance = 0;
    }
    memset(player->buttons, 0, sizeof player->buttons);
    memset(player->axes, 0, sizeof player->axes);
    memset(player->touch_active, 0, sizeof player->touch_active);
    memset(player->touch_ids, 0, sizeof player->touch_ids);
    memset(player->touch_x, 0, sizeof player->touch_x);
    memset(player->touch_y, 0, sizeof player->touch_y);
    player->touchpad_click = 0;
}

static void nxp2_detach_all_players(void) {
    for (int i = 0; i < NXP2_MAX_PLAYERS; i++) nxp2_detach_player(i);
}

static uint16_t nxp2_u16(const uint8_t *p) {
    return (uint16_t)p[0] | (uint16_t)((uint16_t)p[1] << 8);
}

static uint32_t nxp2_u32(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) |
           ((uint32_t)p[3] << 24);
}

static uint64_t nxp2_u64(const uint8_t *p) {
    uint64_t value = 0;
    for (unsigned i = 0; i < 8; i++) value |= (uint64_t)p[i] << (i * 8);
    return value;
}

static int16_t nxp2_i16(const uint8_t *p) {
    uint16_t bits = nxp2_u16(p);
    int16_t value;
    memcpy(&value, &bits, sizeof value);
    return value;
}

static float nxp2_f32(const uint8_t *p) {
    uint32_t bits = nxp2_u32(p);
    float value;
    memcpy(&value, &bits, sizeof value);
    return value;
}

static void nxp2_put_u16(uint8_t *p, uint16_t value) {
    p[0] = (uint8_t)value;
    p[1] = (uint8_t)(value >> 8);
}

static void nxp2_put_u32(uint8_t *p, uint32_t value) {
    for (unsigned i = 0; i < 4; i++) p[i] = (uint8_t)(value >> (i * 8));
}

static void nxp2_put_u64(uint8_t *p, uint64_t value) {
    for (unsigned i = 0; i < 8; i++) p[i] = (uint8_t)(value >> (i * 8));
}

// 宿主 → 客机的报文类型：震动报文只走反方向，客机不认它。
// The host -> guest message types: the rumble message only travels the other
// way, so the guest does not know it.
static int nxp2_type_known(uint16_t type) {
    return type >= NXP2_HELLO && type <= NXP2_ERROR;
}

static int nxp2_send_frame(int fd, uint16_t version, uint16_t type, uint64_t generation,
                           uint64_t sequence, const uint8_t *payload, size_t payload_size) {
    uint8_t packet[NXP2_MAX_PACKET_SIZE];
    if (payload_size > NXP2_MAX_PAYLOAD_SIZE || sequence == 0 || sequence >= INT64_MAX)
        return 0;
    memcpy(packet, NXP2_MAGIC, 4);
    nxp2_put_u16(packet + 4, version);
    nxp2_put_u16(packet + 6, type);
    nxp2_put_u32(packet + 8, (uint32_t)payload_size);
    nxp2_put_u64(packet + 12, generation);
    nxp2_put_u64(packet + 20, sequence);
    if (payload_size) memcpy(packet + NXP2_HEADER_SIZE, payload, payload_size);
    size_t size = NXP2_HEADER_SIZE + payload_size;
    return send(fd, packet, size, MSG_NOSIGNAL) == (ssize_t)size;
}

// 当前该盖的帧头版本：握手后是协商结果，握手前是最老能说的版本。
// The frame version to stamp: the negotiated version after the handshake, the
// oldest one we speak before it.
static uint16_t nxp2_frame_version(void) {
    return g_nxp2_version ? g_nxp2_version : NXP2_VERSION_MIN;
}

// 取下一个序号并发帧：序号自增与发送必须在同一把锁里，否则两个线程可能
// 取到同一个序号，宿主会判 BAD_SEQUENCE 而断开连接。
// Takes the next sequence number and sends the frame: both happen under the
// lock, or two threads can pick the same number and the host drops the
// connection with BAD_SEQUENCE.
static int nxp2_send_locked(int fd, uint16_t type, uint64_t generation,
                            const uint8_t *payload, size_t payload_size) {
    int ok;
    pthread_mutex_lock(&g_nxp2_tx_lock);
    uint64_t sequence = g_nxp2_tx_sequence++;
    ok = nxp2_send_frame(fd, nxp2_frame_version(), type, generation, sequence, payload,
                         payload_size);
    pthread_mutex_unlock(&g_nxp2_tx_lock);
    return ok;
}

// 单调时钟的毫秒数：震动心跳的节拍基准。
// Monotonic milliseconds: the beat base of the rumble heartbeat.
static uint64_t nxp2_now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000u + (uint64_t)ts.tv_nsec / 1000000u;
}

// 震动报文（客机 → 宿主）：player + flags + 强/弱马达强度。
// The rumble message (guest -> host): player, flags and the two intensities.
static int nxp2_send_rumble(int player_index, uint16_t strong, uint16_t weak) {
    uint8_t payload[NXP2_RUMBLE_PAYLOAD_SIZE];
    payload[0] = (uint8_t)player_index;
    payload[1] = 0; // flags：保留，必须为 0 / reserved, must be 0
    nxp2_put_u16(payload + 2, strong);
    nxp2_put_u16(payload + 4, weak);
    return nxp2_send_locked(g_nxp2_fd, NXP2_RUMBLE, g_nxp2_generation, payload, sizeof payload);
}

// 把震动状态发给宿主：强度变了立刻发，持续震动按节拍续发。SDL 自己只在
// 强度变化时回调驱动、持续效果每 2 秒才续发一次，宿主需要更细的心跳才能
// 维持马达。归零时发一次 (0, 0)，宿主据此停下。
// Sends the rumble state to the host: a changed intensity goes out at once, a
// running one is refreshed on every beat. SDL calls the driver only when the
// intensity changes and re-sends the same intensity every 2 s, so the host
// needs a finer heartbeat to keep the motor running. Zero is sent as (0, 0)
// so the host stops.
static int nxp2_pump_rumble(void) {
    if (g_nxp2_version < 3 || g_nxp2_fd < 0) return 1;
    const uint64_t now = nxp2_now_ms();
    for (int i = 0; i < NXP2_MAX_PLAYERS; i++) {
        Nxp2Player *player = &g_nxp2_players[i];
        if (!player->instance) continue; // 这个玩家没有虚拟手柄
        uint16_t strong, weak;
        pthread_mutex_lock(&g_nxp2_rumble_lock);
        strong = player->rumble_strong;
        weak = player->rumble_weak;
        pthread_mutex_unlock(&g_nxp2_rumble_lock);
        const int changed =
            strong != player->rumble_sent_strong || weak != player->rumble_sent_weak;
        if (!changed && !(strong || weak)) continue;              // 没在震，也没变化
        if (!changed && now - player->rumble_sent_ms < NXP2_RUMBLE_HEARTBEAT_MS) continue;
        if (!nxp2_send_rumble(i, strong, weak)) return 0;
        player->rumble_sent_strong = strong;
        player->rumble_sent_weak = weak;
        player->rumble_sent_ms = now;
    }
    return 1;
}

static void nxp2_send_error_fd(int fd, uint64_t generation, uint64_t sequence, int error,
                               uint16_t offending_type) {
    if (!generation) return;
    uint8_t payload[4];
    nxp2_put_u16(payload, (uint16_t)error);
    nxp2_put_u16(payload + 2, offending_type);
    nxp2_send_frame(fd, nxp2_frame_version(), NXP2_ERROR, generation, sequence, payload,
                    sizeof payload);
}

static void nxp2_close_unstarted(int fd) {
    if (fd >= 0) close(fd);
    if (g_nxp2_fd == fd) g_nxp2_fd = -1;
    if (g_nxp2_client_path[0]) {
        unlink(g_nxp2_client_path);
        g_nxp2_client_path[0] = 0;
    }
}

// accept_version：0 = 握手前（落在我们支持的区间里即可），非 0 = 必须等于
// 该值（握手后，帧必须盖协商出来的版本）。
// accept_version: 0 means "before the handshake" (any version inside our
// range is fine), a non-zero value means the frame must carry exactly that
// version (after the handshake).
static int nxp2_decode_header(const uint8_t *packet, size_t size, uint16_t *type,
                              uint32_t *payload_size, uint64_t *generation, uint64_t *sequence,
                              enum nxp2_error *error, uint16_t accept_version) {
    if (size < NXP2_HEADER_SIZE || size > NXP2_MAX_PACKET_SIZE) {
        *error = NXP2_ERR_BAD_LENGTH;
        return 0;
    }
    if (memcmp(packet, NXP2_MAGIC, 4) != 0) {
        *error = NXP2_ERR_BAD_MAGIC;
        return 0;
    }
    uint16_t version = nxp2_u16(packet + 4);
    if (accept_version) {
        if (version != accept_version) {
            *error = NXP2_ERR_BAD_VERSION;
            return 0;
        }
    } else if (version < NXP2_VERSION_MIN || version > NXP2_VERSION) {
        *error = NXP2_ERR_BAD_VERSION;
        return 0;
    }
    *type = nxp2_u16(packet + 6);
    *payload_size = nxp2_u32(packet + 8);
    *generation = nxp2_u64(packet + 12);
    *sequence = nxp2_u64(packet + 20);
    if (!nxp2_type_known(*type)) {
        *error = NXP2_ERR_UNSUPPORTED_MESSAGE;
        return 0;
    }
    if (*payload_size > NXP2_MAX_PAYLOAD_SIZE ||
        size != NXP2_HEADER_SIZE + (size_t)*payload_size) {
        *error = NXP2_ERR_BAD_LENGTH;
        return 0;
    }
    if (!*sequence || *sequence >= INT64_MAX) {
        *error = NXP2_ERR_BAD_SEQUENCE;
        return 0;
    }
    if ((*type == NXP2_HELLO && *generation != 0) ||
        (*type != NXP2_HELLO && *generation == 0)) {
        *error = NXP2_ERR_BAD_GENERATION;
        return 0;
    }
    return 1;
}

static void nxp2_set_timeout(int fd) {
    const struct timeval timeout = {.tv_sec = 2, .tv_usec = 0};
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof timeout);
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof timeout);
}

static uint32_t nxp2_supported_capabilities(void) {
    uint32_t capabilities = NXP2_CAP_MULTI_PLAYER;
    if (sdl.SetJoystickVirtualTouchpad) capabilities |= NXP2_CAP_TOUCH_CONTACTS;
    if (sdl.GetJoystickGUIDForID && sdl.GUIDToString && sdl.AddGamepadMapping &&
        sdl.GetGamepadButtonFromString &&
        sdl.GetGamepadButtonFromString("touchpad") == SDL_GAMEPAD_BUTTON_TOUCHPAD_HINT)
        capabilities |= NXP2_CAP_TOUCH_CLICK;
    return capabilities;
}

static int nxp2_install_touchpad_mapping(uint32_t instance) {
    if (!sdl.GetJoystickGUIDForID || !sdl.GUIDToString || !sdl.AddGamepadMapping ||
        !sdl.GetGamepadButtonFromString ||
        sdl.GetGamepadButtonFromString("touchpad") != SDL_GAMEPAD_BUTTON_TOUCHPAD_HINT)
        return 0;

    FxdGuid guid = sdl.GetJoystickGUIDForID(instance);
    char guid_text[33];
    sdl.GUIDToString(guid, guid_text, sizeof guid_text);
    char mapping[512];
    int size = snprintf(
        mapping, sizeof mapping,
        "%s,Nixport Standard Gamepad,a:b0,b:b1,x:b2,y:b3,back:b4,guide:b5,start:b6,"
        "leftstick:b7,rightstick:b8,leftshoulder:b9,rightshoulder:b10,dpup:b11,dpdown:b12,"
        "dpleft:b13,dpright:b14,touchpad:b20,leftx:a0,lefty:a1,rightx:a2,righty:a3,"
        "lefttrigger:a4,righttrigger:a5",
        guid_text);
    if (size < 0 || (size_t)size >= sizeof mapping ||
        sdl.AddGamepadMapping(mapping) < 0) {
        LOGI("SDL could not map the virtual Touch button: %s", sdl.GetError());
        return 0;
    }
    return 1;
}

static uint32_t nxp2_probe_capabilities(uint32_t capabilities) {
    uint32_t touch_capabilities =
        capabilities & (NXP2_CAP_TOUCH_CONTACTS | NXP2_CAP_TOUCH_CLICK);
    if (!touch_capabilities) return capabilities;

    FxdVirtualJoystickDesc desc;
    // 探测用的临时设备不挂震动回调（没有玩家归属）。
    // The throwaway device for the probe gets no rumble callback (it has no
    // player).
    nxp2_fill_descriptor(&desc, capabilities, -1);
    uint32_t instance = sdl.JoystickAttachVirtual(&desc);
    if (!instance) {
        LOGI("SDL rejected the Touch virtual-device descriptor; continuing without Touch");
        return capabilities & ~(NXP2_CAP_TOUCH_CONTACTS | NXP2_CAP_TOUCH_CLICK);
    }
    int click_mapping_ready = !(capabilities & NXP2_CAP_TOUCH_CLICK) ||
                              nxp2_install_touchpad_mapping(instance);
    void *joystick = sdl.OpenJoystick(instance);
    if (!joystick) {
        LOGI("SDL could not open the Touch virtual device; continuing without Touch");
        sdl.DetachVirtualJoystick(instance);
        return capabilities & ~(NXP2_CAP_TOUCH_CONTACTS | NXP2_CAP_TOUCH_CLICK);
    }
    if (capabilities & NXP2_CAP_TOUCH_CONTACTS) {
        for (int finger = 0; finger < NXP2_MAX_TOUCHES; finger++) {
            if (sdl.SetJoystickVirtualTouchpad(joystick, 0, finger, true, 0.5f, 0.5f, 1.0f) &&
                sdl.SetJoystickVirtualTouchpad(joystick, 0, finger, false, 0.5f, 0.5f, 0.0f))
                continue;
            LOGI("SDL virtual touchpad update failed; coordinate Touch is unavailable");
            capabilities &= ~NXP2_CAP_TOUCH_CONTACTS;
            break;
        }
    }
    if (capabilities & NXP2_CAP_TOUCH_CLICK) {
        if (!click_mapping_ready ||
            !sdl.SetJoystickVirtualButton(joystick, SDL_GAMEPAD_BUTTON_TOUCHPAD_HINT, 1) ||
            !sdl.SetJoystickVirtualButton(joystick, SDL_GAMEPAD_BUTTON_TOUCHPAD_HINT, 0)) {
            LOGI("SDL virtual touchpad button is unavailable");
            capabilities &= ~NXP2_CAP_TOUCH_CLICK;
        }
    }
    sdl.CloseJoystick(joystick);
    sdl.DetachVirtualJoystick(instance);
    return capabilities;
}

static int nxp2_connect_endpoint(void) {
    struct sockaddr_un address;
    memset(&address, 0, sizeof address);
    address.sun_family = AF_UNIX;
    size_t endpoint_size = strlen(g_nxp2_endpoint);
    socklen_t address_size = (socklen_t)offsetof(struct sockaddr_un, sun_path);
    if (!endpoint_size) return -1;
    if (g_nxp2_endpoint[0] == '@') {
#if defined(__linux__)
        size_t name_size = endpoint_size - 1;
        if (name_size > sizeof address.sun_path - 1) return -1;
        memcpy(address.sun_path + 1, g_nxp2_endpoint + 1, name_size);
        address_size += (socklen_t)(name_size + 1);
#else
        LOGI("abstract NXP2 sockets are unavailable on this platform");
        return -1;
#endif
    } else {
        if (endpoint_size >= sizeof address.sun_path) return -1;
        memcpy(address.sun_path, g_nxp2_endpoint, endpoint_size + 1);
        address_size += (socklen_t)(endpoint_size + 1);
    }

    int fd = socket(AF_UNIX, FXD_PAD_SOCK_TYPE | SOCK_CLOEXEC, 0);
    if (fd < 0) {
        LOGV("NXP2 socket: %s", strerror(errno));
        return -1;
    }
#ifdef FXD_PAD_CLIENT_BIND
    snprintf(g_nxp2_client_path, sizeof g_nxp2_client_path, "/tmp/nxp2-%d.sock", (int)getpid());
    unlink(g_nxp2_client_path);
    struct sockaddr_un client_address;
    memset(&client_address, 0, sizeof client_address);
    client_address.sun_family = AF_UNIX;
    strcpy(client_address.sun_path, g_nxp2_client_path);
    socklen_t client_size =
        (socklen_t)(offsetof(struct sockaddr_un, sun_path) + strlen(g_nxp2_client_path) + 1);
    if (bind(fd, (struct sockaddr *)&client_address, client_size) < 0) {
        LOGV("NXP2 client bind: %s", strerror(errno));
        close(fd);
        g_nxp2_client_path[0] = 0;
        return -1;
    }
#endif
    if (connect(fd, (struct sockaddr *)&address, address_size) < 0) {
        LOGV("NXP2 connect: %s", strerror(errno));
        close(fd);
        if (g_nxp2_client_path[0]) {
            unlink(g_nxp2_client_path);
            g_nxp2_client_path[0] = 0;
        }
        return -1;
    }
    nxp2_set_timeout(fd);
    return fd;
}

static int nxp2_handshake(void) {
    int fd = nxp2_connect_endpoint();
    if (fd < 0) return 0;
    g_nxp2_fd = fd;

    uint32_t supported_capabilities = nxp2_supported_capabilities();
    uint8_t hello[12] = {0};
    // HELLO 的帧头写我们还愿意说的最老版本（此刻还不知道对方的范围），载荷
    // 里给出我们能说的区间；旧宿主按老规矩收下，回一个 v2 的 WELCOME。
    // The HELLO frame carries the oldest version we still speak (the peer's
    // range is unknown yet); the payload states the range we speak. An older
    // host takes it as before and answers with a v2 WELCOME.
    nxp2_put_u16(hello, NXP2_VERSION_MIN);
    nxp2_put_u16(hello + 2, NXP2_VERSION);
    nxp2_put_u32(hello + 4, supported_capabilities);
    hello[8] = NXP2_MAX_PLAYERS;
    if (!nxp2_send_frame(fd, NXP2_VERSION_MIN, NXP2_HELLO, 0, 1, hello, sizeof hello)) {
        LOGV("NXP2 HELLO send failed: %s", strerror(errno));
        nxp2_close_unstarted(fd);
        return 0;
    }

    uint8_t packet[NXP2_MAX_PACKET_SIZE];
    ssize_t got = recv(fd, packet, sizeof packet, MSG_TRUNC);
    uint16_t type = 0;
    uint32_t payload_size = 0;
    uint64_t generation = 0;
    uint64_t sequence = 0;
    enum nxp2_error error = NXP2_ERR_BAD_VALUE;
    int header_ok =
        got > 0 && nxp2_decode_header(packet, (size_t)got, &type, &payload_size, &generation,
                                      &sequence, &error, 0);
    if (!header_ok || type != NXP2_WELCOME || payload_size != 28) {
        if (header_ok && type != NXP2_WELCOME) error = NXP2_ERR_UNSUPPORTED_MESSAGE;
        if (header_ok && payload_size != 28) error = NXP2_ERR_BAD_LENGTH;
        uint64_t response_generation =
            header_ok ? generation : got >= NXP2_HEADER_SIZE ? nxp2_u64(packet + 12) : 0;
        uint16_t offending_type =
            header_ok ? type : got >= NXP2_HEADER_SIZE ? nxp2_u16(packet + 6) : 0;
        nxp2_send_error_fd(fd, response_generation, 2, error, offending_type);
        LOGI("NXP2 WELCOME rejected%s%s", got < 0 ? ": " : "",
             got < 0 ? strerror(errno) : "");
        nxp2_close_unstarted(fd);
        return 0;
    }
    const uint8_t *welcome = packet + NXP2_HEADER_SIZE;
    uint32_t capabilities = nxp2_u32(welcome + 4);
    uint8_t player_limit = welcome[8];
    // WELCOME 头一个 u16 是协商出来的版本：取它，之后所有帧都盖这个版本。
    // The first u16 of the WELCOME is the negotiated version: take it and
    // stamp it on every frame from now on.
    uint16_t negotiated = nxp2_u16(welcome);
    if (negotiated < NXP2_VERSION_MIN || negotiated > NXP2_VERSION) {
        nxp2_send_error_fd(fd, generation, 2, NXP2_ERR_BAD_VERSION, NXP2_WELCOME);
        LOGI("NXP2 WELCOME names a version we do not speak: %u", negotiated);
        nxp2_close_unstarted(fd);
        return 0;
    }
    g_nxp2_version = negotiated;
    if (nxp2_u16(welcome + 2) != 0 || (capabilities & ~supported_capabilities) ||
        player_limit < 1 ||
        player_limit > NXP2_MAX_PLAYERS ||
        (player_limit > 1 && !(capabilities & NXP2_CAP_MULTI_PLAYER)) ||
        (welcome[9] != 0 && welcome[9] != NXP2_MAX_TOUCHES) ||
        nxp2_u16(welcome + 10) != 0 ||
        (welcome[9] == NXP2_MAX_TOUCHES) !=
            ((capabilities & NXP2_CAP_TOUCH_CONTACTS) != 0)) {
        nxp2_send_error_fd(fd, generation, 2, NXP2_ERR_BAD_CAPABILITY, NXP2_WELCOME);
        LOGI("NXP2 WELCOME selected unsupported capabilities");
        nxp2_close_unstarted(fd);
        return 0;
    }
    capabilities = nxp2_probe_capabilities(capabilities);
    uint8_t ready[8] = {0};
    nxp2_put_u32(ready, capabilities);
    ready[4] = player_limit;
    ready[5] = (capabilities & NXP2_CAP_TOUCH_CONTACTS) ? NXP2_MAX_TOUCHES : 0;
    if (!nxp2_send_frame(fd, g_nxp2_version, NXP2_READY, generation, 2, ready, sizeof ready)) {
        LOGI("NXP2 READY send failed: %s", strerror(errno));
        nxp2_close_unstarted(fd);
        return 0;
    }
    g_nxp2_fd = fd;
    g_nxp2_generation = generation;
    g_nxp2_capabilities = capabilities;
    g_nxp2_player_limit = player_limit;
    g_nxp2_rx_sequence = sequence;
    g_nxp2_tx_sequence = 3;
    LOGI("NXP2 ready: generation %llu, version %u, %u player(s), capabilities 0x%x",
         (unsigned long long)generation, g_nxp2_version, player_limit, capabilities);
    return 1;
}

static int nxp2_update_touches(Nxp2Player *player, const Nxp2SnapshotPlayer *snapshot) {
    int assigned[NXP2_MAX_TOUCHES] = {-1, -1};
    uint8_t reserved_slots[NXP2_MAX_TOUCHES] = {0};
    for (int touch = 0; touch < snapshot->touch_count; touch++) {
        for (int slot = 0; slot < NXP2_MAX_TOUCHES; slot++) {
            if (player->touch_active[slot] &&
                player->touch_ids[slot] == snapshot->touch_ids[touch]) {
                assigned[touch] = slot;
                reserved_slots[slot] = 1;
                break;
            }
        }
    }
    for (int touch = 0; touch < snapshot->touch_count; touch++) {
        if (assigned[touch] >= 0) continue;
        for (int slot = 0; slot < NXP2_MAX_TOUCHES; slot++) {
            if (!reserved_slots[slot]) {
                assigned[touch] = slot;
                reserved_slots[slot] = 1;
                break;
            }
        }
        if (assigned[touch] < 0) return NXP2_ERR_BAD_VALUE;
    }

    for (int slot = 0; slot < NXP2_MAX_TOUCHES; slot++) {
        if (!player->touch_active[slot]) continue;
        int still_active = 0;
        for (int touch = 0; touch < snapshot->touch_count; touch++)
            still_active |= player->touch_ids[slot] == snapshot->touch_ids[touch];
        if (!still_active &&
            !sdl.SetJoystickVirtualTouchpad(player->joystick, 0, slot, false,
                                            player->touch_x[slot], player->touch_y[slot], 0.0f))
            return NXP2_ERR_BAD_CAPABILITY;
    }

    uint8_t next_active[NXP2_MAX_TOUCHES] = {0};
    uint16_t next_ids[NXP2_MAX_TOUCHES] = {0};
    float next_x[NXP2_MAX_TOUCHES] = {0};
    float next_y[NXP2_MAX_TOUCHES] = {0};
    for (int touch = 0; touch < snapshot->touch_count; touch++) {
        int slot = assigned[touch];
        if (!sdl.SetJoystickVirtualTouchpad(player->joystick, 0, slot, true,
                                            snapshot->touch_x[touch], snapshot->touch_y[touch],
                                            1.0f))
            return NXP2_ERR_BAD_CAPABILITY;
        next_active[slot] = 1;
        next_ids[slot] = snapshot->touch_ids[touch];
        next_x[slot] = snapshot->touch_x[touch];
        next_y[slot] = snapshot->touch_y[touch];
    }
    memcpy(player->touch_active, next_active, sizeof next_active);
    memcpy(player->touch_ids, next_ids, sizeof next_ids);
    memcpy(player->touch_x, next_x, sizeof next_x);
    memcpy(player->touch_y, next_y, sizeof next_y);
    return 0;
}

static int nxp2_apply_snapshot(const uint8_t *payload, size_t size) {
    if (size < 4 || payload[0] < 1 || payload[0] > g_nxp2_player_limit ||
        payload[1] || payload[2] || payload[3] ||
        size != 4 + (size_t)payload[0] * NXP2_PLAYER_RECORD_SIZE) return NXP2_ERR_BAD_LENGTH;
    Nxp2SnapshotPlayer snapshots[NXP2_MAX_PLAYERS] = {0};
    static const uint8_t button_map[15] = {
        FXD_BTN_SOUTH,       FXD_BTN_EAST,       FXD_BTN_WEST,        FXD_BTN_NORTH,
        FXD_BTN_LEFT_SHOULDER, FXD_BTN_RIGHT_SHOULDER, FXD_BTN_LEFT_STICK, FXD_BTN_RIGHT_STICK,
        FXD_BTN_BACK,        FXD_BTN_START,      FXD_BTN_GUIDE,       FXD_BTN_DPAD_UP,
        FXD_BTN_DPAD_DOWN,   FXD_BTN_DPAD_LEFT,  FXD_BTN_DPAD_RIGHT
    };
    int previous_player = -1;
    for (unsigned record = 0; record < payload[0]; record++) {
        const uint8_t *wire = payload + 4 + record * NXP2_PLAYER_RECORD_SIZE;
        uint8_t player_index = wire[0];
        uint8_t flags = wire[1];
        uint32_t buttons = nxp2_u32(wire + 4);
        if (player_index >= g_nxp2_player_limit || player_index <= previous_player ||
            (flags & 0xfe) || nxp2_u16(wire + 2) || (buttons & ~NXP2_BUTTON_MASK))
            return NXP2_ERR_BAD_VALUE;
        previous_player = player_index;
        if (wire[20] > NXP2_MAX_TOUCHES || wire[21] > 1 || nxp2_u16(wire + 22))
            return NXP2_ERR_BAD_VALUE;
        if (!(flags & 1)) {
            for (size_t i = 4; i < NXP2_PLAYER_RECORD_SIZE; i++)
                if (wire[i]) return NXP2_ERR_BAD_VALUE;
        }

        Nxp2SnapshotPlayer *snapshot = &snapshots[player_index];
        snapshot->present = 1;
        snapshot->active = (flags & 1) != 0;
        snapshot->buttons = buttons;
        snapshot->touch_count = wire[20];
        snapshot->touchpad_click = wire[21];
        for (int axis = 0; axis < 4; axis++)
            snapshot->axes[axis] = nxp2_i16(wire + 8 + axis * 2);
        for (int trigger = 0; trigger < 2; trigger++)
            snapshot->axes[FXD_AXIS_LEFT_TRIGGER + trigger] =
                (int16_t)((int32_t)nxp2_u16(wire + 16 + trigger * 2) - 32768);
        for (int touch = 0; touch < NXP2_MAX_TOUCHES; touch++) {
            const uint8_t *contact = wire + 24 + touch * 12;
            uint16_t id = nxp2_u16(contact);
            uint16_t reserved = nxp2_u16(contact + 2);
            uint32_t x_bits = nxp2_u32(contact + 4);
            uint32_t y_bits = nxp2_u32(contact + 8);
            if (touch >= snapshot->touch_count) {
                if (id || reserved || x_bits || y_bits) return NXP2_ERR_BAD_VALUE;
                continue;
            }
            float x = nxp2_f32(contact + 4);
            float y = nxp2_f32(contact + 8);
            if (reserved || !isfinite(x) || !isfinite(y) ||
                x < 0.0f || x > 1.0f || y < 0.0f || y > 1.0f)
                return NXP2_ERR_BAD_VALUE;
            for (int previous = 0; previous < touch; previous++)
                if (snapshot->touch_ids[previous] == id) return NXP2_ERR_BAD_VALUE;
            snapshot->touch_ids[touch] = id;
            snapshot->touch_x[touch] = x;
            snapshot->touch_y[touch] = y;
        }
        if ((snapshot->touch_count && !(g_nxp2_capabilities & NXP2_CAP_TOUCH_CONTACTS)) ||
            (snapshot->touchpad_click && !(g_nxp2_capabilities & NXP2_CAP_TOUCH_CLICK)))
            return NXP2_ERR_BAD_CAPABILITY;
    }

    for (int player_index = 0; player_index < g_nxp2_player_limit; player_index++) {
        Nxp2SnapshotPlayer *snapshot = &snapshots[player_index];
        Nxp2Player *player = &g_nxp2_players[player_index];
        if (!snapshot->present || !snapshot->active) {
            nxp2_detach_player(player_index);
            continue;
        }
        if (!nxp2_attach_player(player_index)) return NXP2_ERR_BAD_CAPABILITY;
        for (int button = 0; button < 15; button++) {
            uint8_t value = (snapshot->buttons & (1u << button)) != 0;
            int target = button_map[button];
            if (value == player->buttons[target]) continue;
            player->buttons[target] = value;
            sdl.SetJoystickVirtualButton(player->joystick, target, value);
        }
        for (int axis = 0; axis < FXD_AXIS_COUNT; axis++) {
            int16_t value = snapshot->axes[axis];
            if (value == player->axes[axis]) continue;
            player->axes[axis] = value;
            sdl.SetJoystickVirtualAxis(player->joystick, axis, value);
        }
        if (g_nxp2_capabilities & NXP2_CAP_TOUCH_CONTACTS) {
            int touch_error = nxp2_update_touches(player, snapshot);
            if (touch_error) return touch_error;
        }
        if (g_nxp2_capabilities & NXP2_CAP_TOUCH_CLICK) {
            uint8_t click = snapshot->touchpad_click;
            if (click != player->touchpad_click) {
                player->touchpad_click = click;
                sdl.SetJoystickVirtualButton(player->joystick, SDL_GAMEPAD_BUTTON_TOUCHPAD_HINT,
                                             click);
            }
        }
    }
    return 0;
}

static void nxp2_send_error(int error, uint16_t offending_type) {
    if (!g_nxp2_tx_sequence || g_nxp2_tx_sequence >= INT64_MAX) return;
    uint8_t payload[4];
    nxp2_put_u16(payload, (uint16_t)error);
    nxp2_put_u16(payload + 2, offending_type);
    nxp2_send_locked(g_nxp2_fd, NXP2_ERROR, g_nxp2_generation, payload, sizeof payload);
}

static void nxp2_clear_connection(void) {
    nxp2_detach_all_players();
    if (g_nxp2_fd >= 0) close(g_nxp2_fd);
    g_nxp2_fd = -1;
    g_nxp2_generation = 0;
    g_nxp2_rx_sequence = 0;
    g_nxp2_tx_sequence = 0;
    g_nxp2_capabilities = 0;
    g_nxp2_player_limit = 1;
    g_nxp2_version = 0;
    for (int i = 0; i < NXP2_MAX_PLAYERS; i++) {
        pthread_mutex_lock(&g_nxp2_rumble_lock);
        g_nxp2_players[i].rumble_strong = 0;
        g_nxp2_players[i].rumble_weak = 0;
        g_nxp2_players[i].rumble_sent_strong = 0;
        g_nxp2_players[i].rumble_sent_weak = 0;
        g_nxp2_players[i].rumble_sent_ms = 0;
        pthread_mutex_unlock(&g_nxp2_rumble_lock);
    }
    if (g_nxp2_client_path[0]) {
        unlink(g_nxp2_client_path);
        g_nxp2_client_path[0] = 0;
    }
}

static void *nxp2_thread(void *arg) {
    (void)arg;
    uint8_t packet[NXP2_MAX_PACKET_SIZE];
    for (;;) {
        int disconnected = 0;
        while (!disconnected) {
            struct pollfd pfd = {.fd = g_nxp2_fd, .events = POLLIN};
            // 超时即心跳的节拍：震动状态由这里发，不占用游戏线程。
            // The timeout is the heartbeat beat: the rumble state goes out
            // from here, never from the game's thread.
            int ready = poll(&pfd, 1, NXP2_RUMBLE_HEARTBEAT_MS);
            if (ready < 0) {
                if (errno == EINTR) continue;
                disconnected = 1;
                continue;
            }
            if (!nxp2_pump_rumble()) {
                disconnected = 1;
                continue;
            }
            if (pfd.revents & (POLLERR | POLLHUP | POLLNVAL)) {
                disconnected = 1;
                continue;
            }
            if (!(pfd.revents & POLLIN)) continue;
            ssize_t got = recv(g_nxp2_fd, packet, sizeof packet, MSG_TRUNC);
            if (got <= 0 || got > (ssize_t)sizeof packet) {
                disconnected = 1;
                continue;
            }

            uint16_t type = 0;
            uint32_t payload_size = 0;
            uint64_t generation = 0;
            uint64_t sequence = 0;
            enum nxp2_error error = NXP2_ERR_BAD_VALUE;
            int parsed = nxp2_decode_header(packet, (size_t)got, &type, &payload_size,
                                            &generation, &sequence, &error, g_nxp2_version);
            if (!parsed) {
                nxp2_send_error(error, 0);
                disconnected = 1;
                continue;
            }
            if (type != NXP2_SNAPSHOT || generation != g_nxp2_generation ||
                sequence <= g_nxp2_rx_sequence) {
                nxp2_send_error(type != NXP2_SNAPSHOT ? NXP2_ERR_UNSUPPORTED_MESSAGE
                                                      : generation != g_nxp2_generation
                                                            ? NXP2_ERR_BAD_GENERATION
                                                            : NXP2_ERR_BAD_SEQUENCE,
                                type);
                disconnected = 1;
                continue;
            }
            int snapshot_error =
                nxp2_apply_snapshot(packet + NXP2_HEADER_SIZE, payload_size);
            if (snapshot_error) {
                nxp2_send_error(snapshot_error, type);
                disconnected = 1;
                continue;
            }
            g_nxp2_rx_sequence = sequence;
            uint8_t ack[8];
            nxp2_put_u64(ack, sequence);
            if (!nxp2_send_locked(g_nxp2_fd, NXP2_ACK, g_nxp2_generation, ack, sizeof ack))
                disconnected = 1;
        }

        nxp2_clear_connection();
        LOGI("NXP2 connection closed; all player input was cleared");
        for (;;) {
            usleep(250000);
            if (nxp2_handshake()) break;
        }
    }
}

// Connects to the host's socket for one slot; the virtual gamepad comes up with
// the first slot that connects.
static int pad_open(Slot *s) {
    char path[PATH_MAX];
    snprintf(path, sizeof path, "%s/event%d", g_dir, s->node);

    struct sockaddr_un addr;
    memset(&addr, 0, sizeof addr);
    addr.sun_family = AF_UNIX;
    if (strlen(path) >= sizeof addr.sun_path) {
        LOGV("slot %d: path too long", s->slot);
        return 0;
    }
    strcpy(addr.sun_path, path);

    int fd = socket(AF_UNIX, FXD_PAD_SOCK_TYPE | SOCK_CLOEXEC, 0);
    if (fd < 0) {
        LOGV("slot %d: socket: %s", s->slot, strerror(errno));
        return 0;
    }
#ifdef FXD_PAD_CLIENT_BIND
    // The host answers the client's address, so the client must have one.
    char client_path[PATH_MAX];
    snprintf(client_path, sizeof client_path, "/tmp/fexpad-%d-%d.sock", (int)getpid(), s->slot);
    unlink(client_path);
    struct sockaddr_un client_addr;
    memset(&client_addr, 0, sizeof client_addr);
    client_addr.sun_family = AF_UNIX;
    strcpy(client_addr.sun_path, client_path);
    if (bind(fd, (struct sockaddr *)&client_addr, sizeof client_addr) < 0) {
        LOGV("slot %d: bind %s: %s", s->slot, client_path, strerror(errno));
        close(fd);
        return 0;
    }
    strcpy(s->client_path, client_path);
#endif
    if (connect(fd, (struct sockaddr *)&addr, sizeof addr) < 0) {
        LOGV("slot %d: connect %s: %s", s->slot, path, strerror(errno));
        close(fd);
        return 0;
    }
    uint32_t hello[2] = {FXD_PAD_HELLO_MAGIC, FXD_PAD_EVENT_SIZE};
    if (send(fd, hello, sizeof hello, MSG_NOSIGNAL) != (ssize_t)sizeof hello) {
        LOGV("slot %d: hello failed: %s", s->slot, strerror(errno));
        close(fd);
        return 0;
    }
    if (!ensure_virtual()) {
        close(fd);
        return 0;
    }
    s->fd = fd;
    slot_reset_state(s);
    merge_and_push();
    LOGV("slot %d: connected to %s", s->slot, path);
    return 1;
}

static void pad_close(Slot *s) {
    if (s->fd >= 0) {
        close(s->fd);
        s->fd = -1;
    }
    if (s->client_path[0]) {
        unlink(s->client_path);
        s->client_path[0] = 0;
    }
    slot_reset_state(s);
    if (g_instance) {
        int live = 0;
        for (int i = 0; i < FXD_PAD_MAX_SLOTS; i++) live += slot_live(&g_slots[i]);
        if (!live) {
            LOGI("the last pad went away; the virtual gamepad is withdrawn");
            drop_virtual();
        } else {
            merge_and_push(); // drop this slot's contribution
        }
    }
}

// Scans the node directory: every event<N> is one pad slot of the host.
static void rescan(void) {
    DIR *dir = opendir(g_dir);
    if (!dir) return;
    struct dirent *entry;
    while ((entry = readdir(dir)) != NULL) {
        if (strncmp(entry->d_name, "event", 5) != 0) continue;
        const char *digits = entry->d_name + 5;
        if (!*digits) continue;
        int node = atoi(digits);
        if (node < FXD_PAD_FIRST_NODE) continue;
        int slot = node - FXD_PAD_FIRST_NODE;
        if (slot >= FXD_PAD_MAX_SLOTS) continue;
        Slot *s = &g_slots[slot];
        if (slot_live(s)) continue;
        if (s->node != node) {
            s->slot = slot;
            s->node = node;
        }
        char path[PATH_MAX];
        snprintf(path, sizeof path, "%s/event%d", g_dir, node);
        struct stat st;
        if (stat(path, &st) != 0) continue; // the host has not created it (yet)
        time_t now = time(NULL);
        if (now < s->next_try) continue;
        s->next_try = now + 2;
        pad_open(s);
    }
    closedir(dir);

    // A slot whose node is gone (the pad was unplugged) is closed.
    for (int i = 0; i < FXD_PAD_MAX_SLOTS; i++) {
        Slot *s = &g_slots[i];
        if (!slot_live(s)) continue;
        char path[PATH_MAX];
        snprintf(path, sizeof path, "%s/event%d", g_dir, s->node);
        struct stat st;
        if (stat(path, &st) == 0) continue;
        LOGI("slot %d: node gone, its input is dropped from the merge", i);
        pad_close(s);
    }
}

static void *pad_thread(void *arg) {
    (void)arg;
    uint8_t buffer[FXD_PAD_EVENT_SIZE * 64];
    for (;;) {
        struct pollfd fds[1 + FXD_PAD_MAX_SLOTS];
        int who[1 + FXD_PAD_MAX_SLOTS];
        int n = 0;
        if (g_watch_fd >= 0) {
            fds[n].fd = g_watch_fd;
            fds[n].events = POLLIN;
            fds[n].revents = 0;
            who[n] = -1;
            n++;
        }
        for (int i = 0; i < FXD_PAD_MAX_SLOTS; i++) {
            if (g_slots[i].fd < 0) continue;
            fds[n].fd = g_slots[i].fd;
            fds[n].events = POLLIN;
            fds[n].revents = 0;
            who[n] = i;
            n++;
        }
        if (n == 0) {
            // Nothing to watch yet: look for nodes again in a moment.
            usleep(200000);
            rescan();
            continue;
        }
        int timeout = g_watch_fd >= 0 ? -1 : 200;
        int ready = poll(fds, n, timeout);
        if (ready < 0) {
            if (errno == EINTR) continue;
            break;
        }
        for (int i = 0; i < n; i++) {
            if (!fds[i].revents) continue;
            if (who[i] < 0) {
#ifdef FXD_PAD_INOTIFY
                char events[4096];
                ssize_t got = read(g_watch_fd, events, sizeof events);
                (void)got;
#endif
                rescan();
                continue;
            }
            Slot *s = &g_slots[who[i]];
            ssize_t got = recv(s->fd, buffer, sizeof buffer, 0);
            if (got <= 0) {
                if (got < 0 && (errno == EAGAIN || errno == EINTR)) continue;
                LOGI("slot %d: the host closed the pad", s->slot);
                pad_close(s);
                continue;
            }
            handle_packet(s, buffer, (size_t)got);
        }
#ifndef FXD_PAD_INOTIFY
        rescan();
#endif
    }
    return NULL;
}

static int g_attempts;

static int load_sdl(void) {
    // The guest's own SDL3: the same soname the game links, so the loader hands
    // out the same object and the virtual joystick is visible to the game.
    static const char *const names[] = {"libSDL3.so.0", "libSDL3.so", "libSDL3.0.dylib", "libSDL3.dylib"};
    void *lib = NULL;
    char first_error[256] = "", last_error[256] = "";
    for (size_t i = 0; i < sizeof names / sizeof names[0]; i++) {
        lib = dlopen(names[i], RTLD_NOW | RTLD_GLOBAL);
        if (lib) break;
        // dlerror() is only good for the call just made: keep the first and the
        // last reason, the ones in between are the same "no such file".
        const char *error = dlerror();
        if (i == 0) snprintf(first_error, sizeof first_error, "%s", error ? error : "?");
        snprintf(last_error, sizeof last_error, "%s", error ? error : "?");
    }
    if (!lib) {
        char exe[64];
        exe_name(exe, sizeof exe);
        if (g_attempts == 0) {
            LOGI("SDL3 not found in %s; soname search: %s", exe, first_error);
            LOGI("SDL3 not found in %s; last candidate: %s", exe, last_error);
        } else {
            LOGV("SDL3 not found in %s (attempt %d): %s", exe, g_attempts, last_error);
        }
        return 0;
    }
#define LOAD(field, symbol)                                                                       \
    do {                                                                                           \
        *(void **)(&sdl.field) = dlsym(lib, symbol);                                              \
        if (!sdl.field) {                                                                         \
            LOGI("SDL3 has no %s", symbol);                                                       \
            return 0;                                                                             \
        }                                                                                          \
    } while (0)
    LOAD(InitSubSystem, "SDL_InitSubSystem");
    LOAD(JoystickAttachVirtual, "SDL_AttachVirtualJoystick");
    LOAD(OpenJoystick, "SDL_OpenJoystick");
    LOAD(CloseJoystick, "SDL_CloseJoystick");
    LOAD(DetachVirtualJoystick, "SDL_DetachVirtualJoystick");
    LOAD(SetJoystickVirtualAxis, "SDL_SetJoystickVirtualAxis");
    LOAD(SetJoystickVirtualButton, "SDL_SetJoystickVirtualButton");
    LOAD(GetError, "SDL_GetError");
    LOAD(GetJoystickVendorForID, "SDL_GetJoystickVendorForID");
    LOAD(GetJoystickProductForID, "SDL_GetJoystickProductForID");
    LOAD(GetJoystickNameForID, "SDL_GetJoystickNameForID");
#undef LOAD
    *(void **)(&sdl.SetJoystickVirtualTouchpad) =
        dlsym(lib, "SDL_SetJoystickVirtualTouchpad");
    if (!sdl.SetJoystickVirtualTouchpad)
        LOGI("SDL3 has no SDL_SetJoystickVirtualTouchpad; coordinate Touch is disabled");
    *(void **)(&sdl.GetJoystickGUIDForID) = dlsym(lib, "SDL_GetJoystickGUIDForID");
    *(void **)(&sdl.GUIDToString) = dlsym(lib, "SDL_GUIDToString");
    *(void **)(&sdl.AddGamepadMapping) = dlsym(lib, "SDL_AddGamepadMapping");
    *(void **)(&sdl.GetGamepadButtonFromString) = dlsym(lib, "SDL_GetGamepadButtonFromString");
    return 1;
}

// Everything that needs SDL: load it, init the joystick subsystem, connect to
// every pad node the host serves. 1 = running.
static int try_start(void) {
    int ok = load_sdl();
    g_attempts++;
    if (!ok) return 0;
    if (!sdl.InitSubSystem(SDL_INIT_JOYSTICK_HINT)) {
        LOGI("SDL_InitSubSystem(JOYSTICK) failed: %s", sdl.GetError());
        return 0;
    }
    for (int i = 0; i < FXD_PAD_MAX_SLOTS; i++) {
        g_slots[i].fd = -1;
        g_slots[i].slot = i;
        slot_reset_state(&g_slots[i]);
    }
    if (g_nxp2_requested) {
        if (!nxp2_handshake()) return 0;
        if (pthread_create(&g_thread, NULL, nxp2_thread, NULL) != 0) {
            LOGI("could not start the NXP2 receive thread");
            close(g_nxp2_fd);
            g_nxp2_fd = -1;
            if (g_nxp2_client_path[0]) {
                unlink(g_nxp2_client_path);
                g_nxp2_client_path[0] = 0;
            }
            drop_virtual();
            return 0;
        }
        g_thread_started = 1;
        char exe[64];
        exe_name(exe, sizeof exe);
        LOGI("NXP2 shim ready in %s", exe);
        return 1;
    }
#ifdef FXD_PAD_INOTIFY
    g_watch_fd = inotify_init1(IN_NONBLOCK | IN_CLOEXEC);
    if (g_watch_fd >= 0) {
        inotify_add_watch(g_watch_fd, g_dir, IN_CREATE | IN_DELETE | IN_MOVED_TO | IN_MOVED_FROM);
    }
#endif
    rescan();
    if (pthread_create(&g_thread, NULL, pad_thread, NULL) != 0) return 0;
    g_thread_started = 1;
    char exe[64];
    exe_name(exe, sizeof exe);
    LOGI("shim ready in %s (%s)", exe, g_dir);
    return 1;
}

// In a game process the constructor may run before the environment the game
// itself starts with is fully in effect (launcher chains re-exec several
// times); a pad that silently never appears is the worst outcome, so keep
// trying for half a minute before giving up.
static void *retry_thread(void *arg) {
    (void)arg;
    for (int i = 0; i < 60; i++) {
        usleep(500000);
        if (try_start()) return NULL;
    }
    LOGI("no usable SDL3 after 30s of retries: the game gets no gamepad from this shim");
    return NULL;
}

__attribute__((constructor)) static void fxd_pad_init(void) {
    const char *debug = getenv("FXD_PAD_SHIM_DEBUG");
    g_debug = debug && debug[0] == '1';
    if (inside_fex()) {
        LOGI("inside FEX: the guest's pads come from patch 0006, the shim stays out");
        return;
    }
    const char *endpoint = getenv("NXP_PAD_SOCKET");
    if (endpoint && *endpoint) {
        if (strlen(endpoint) >= sizeof g_nxp2_endpoint) {
            LOGI("NXP_PAD_SOCKET exceeds the Unix socket path limit");
            return;
        }
        strcpy(g_nxp2_endpoint, endpoint);
        g_nxp2_requested = 1;
    }
    const char *root = getenv("FXD_ROOT");
    if ((!root || !*root) && !g_nxp2_requested) {
        LOGI("no FXD_ROOT: nothing to do");
        return;
    }
    if (root && *root) {
        snprintf(g_dir, sizeof g_dir, "%s/usr/share/fex-emu/fexdroid-input", root);
        struct stat st;
        if (stat(g_dir, &st) != 0 || !S_ISDIR(st.st_mode)) {
            if (!g_nxp2_requested) {
                LOGI("no pad directory at %s", g_dir);
                return;
            }
            LOGV("legacy FXP1 directory is absent; NXP2 remains selected");
        }
    }
    if (try_start()) return;
    if (pthread_create(&g_retry_thread, NULL, retry_thread, NULL) == 0) {
        g_retry_started = 1;
        LOGI("SDL3 not usable yet: retrying for up to 30s");
    }
}

__attribute__((destructor)) static void fxd_pad_fini(void) {
    if (g_retry_started) {
        pthread_cancel(g_retry_thread);
        pthread_join(g_retry_thread, NULL);
    }
    if (!g_thread_started) return;
    pthread_cancel(g_thread);
    pthread_join(g_thread, NULL);
    for (int i = 0; i < FXD_PAD_MAX_SLOTS; i++) {
        if (g_slots[i].fd >= 0) {
            close(g_slots[i].fd);
            g_slots[i].fd = -1;
        }
        if (g_slots[i].client_path[0]) {
            unlink(g_slots[i].client_path);
            g_slots[i].client_path[0] = 0;
        }
    }
    nxp2_clear_connection();
    drop_virtual();
}

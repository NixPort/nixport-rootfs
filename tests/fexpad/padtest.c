// The guest's side of the native-track shim test: it enumerates gamepads and
// reports button, axis, and touchpad state while the shim is preloaded.
//
// usage: padtest [seconds]

#include <SDL3/SDL.h>
#include <stdio.h>
#include <stdlib.h>

#define MAX_TEST_PADS 8
#define MAX_TEST_FINGERS 4

typedef struct {
    SDL_JoystickID id;
    SDL_Gamepad *gamepad;
    SDL_Joystick *joystick;
    int last_button_state[SDL_GAMEPAD_BUTTON_COUNT];
    int last_touch_state[MAX_TEST_FINGERS][4];
} TestPad;

static TestPad pads[MAX_TEST_PADS];

static TestPad *find_pad(SDL_JoystickID id) {
    for (int i = 0; i < MAX_TEST_PADS; i++)
        if (pads[i].gamepad && pads[i].id == id) return &pads[i];
    return NULL;
}

static int is_connected(SDL_JoystickID id) {
    int count = 0;
    SDL_JoystickID *ids = SDL_GetGamepads(&count);
    int connected = 0;
    for (int i = 0; i < count; i++) connected |= ids[i] == id;
    SDL_free(ids);
    return connected;
}

static TestPad *open_pad(SDL_JoystickID id) {
    TestPad *existing = find_pad(id);
    if (existing) return existing;
    for (int i = 0; i < MAX_TEST_PADS; i++) {
        if (pads[i].gamepad) continue;
        TestPad *entry = &pads[i];
        entry->id = id;
        entry->gamepad = SDL_OpenGamepad(id);
        if (!entry->gamepad) {
            printf("%s %u\n", is_connected(id) ? "OPEN_FAILED" : "STALE_ADDED", id);
            return NULL;
        }
        entry->joystick = SDL_GetJoystickFromID(id);
        for (int button = 0; button < SDL_GAMEPAD_BUTTON_COUNT; button++)
            entry->last_button_state[button] = -1;
        for (int finger = 0; finger < MAX_TEST_FINGERS; finger++)
            for (int field = 0; field < 4; field++)
                entry->last_touch_state[finger][field] = -1;
        int touchpads = SDL_GetNumGamepadTouchpads(entry->gamepad);
        int fingers = touchpads ? SDL_GetNumGamepadTouchpadFingers(entry->gamepad, 0) : 0;
        printf("GAMEPAD id=%u name=%s vendor=%04x product=%04x axes=%d buttons=%d "
               "touchpads=%d fingers=%d hasTouchClick=%d\n",
               id, SDL_GetGamepadName(entry->gamepad), SDL_GetJoystickVendorForID(id),
               SDL_GetJoystickProductForID(id), SDL_GetNumJoystickAxes(entry->joystick),
               SDL_GetNumJoystickButtons(entry->joystick), touchpads, fingers,
               SDL_GamepadHasButton(entry->gamepad, SDL_GAMEPAD_BUTTON_TOUCHPAD));
        return entry;
    }
    printf("CAPACITY_EXCEEDED %u\n", id);
    return NULL;
}

static void close_pad(SDL_JoystickID id) {
    TestPad *entry = find_pad(id);
    if (!entry) return;
    SDL_CloseGamepad(entry->gamepad);
    entry->gamepad = NULL;
    entry->joystick = NULL;
}

static void report_button_state(TestPad *entry) {
    for (int button = 0; button < SDL_GAMEPAD_BUTTON_COUNT; button++) {
        if (!SDL_GamepadHasButton(entry->gamepad, (SDL_GamepadButton)button)) continue;
        int down = SDL_GetGamepadButton(entry->gamepad, (SDL_GamepadButton)button);
        if (down == entry->last_button_state[button]) continue;
        entry->last_button_state[button] = down;
        printf("BUTTONSTATE %u %d %d\n", entry->id, button, down);
    }
}

static void report_touch_state(TestPad *entry) {
    int touchpads = SDL_GetNumGamepadTouchpads(entry->gamepad);
    if (touchpads < 1) return;
    int fingers = SDL_GetNumGamepadTouchpadFingers(entry->gamepad, 0);
    if (fingers > MAX_TEST_FINGERS) fingers = MAX_TEST_FINGERS;
    for (int finger = 0; finger < fingers; finger++) {
        bool down = false;
        float x = 0.0f, y = 0.0f, pressure = 0.0f;
        if (!SDL_GetGamepadTouchpadFinger(entry->gamepad, 0, finger, &down, &x, &y,
                                          &pressure))
            continue;
        int state[4] = {down, (int)(x * 1000.0f + 0.5f), (int)(y * 1000.0f + 0.5f),
                        (int)(pressure * 1000.0f + 0.5f)};
        int changed = 0;
        for (int field = 0; field < 4; field++)
            changed |= state[field] != entry->last_touch_state[finger][field];
        if (!changed) continue;
        for (int field = 0; field < 4; field++)
            entry->last_touch_state[finger][field] = state[field];
        printf("TOUCHSTATE %u %d %d %.3f %.3f %.3f\n", entry->id, finger, down, x, y,
               pressure);
    }
}

// 震动：游戏侧调用 SDL_RumbleGamepad，垫片的回调把它变成回给宿主的报文。
// The rumble: the game calls SDL_RumbleGamepad and the shim's callback turns
// it into a message back to the host.
static void rumble(TestPad *entry, Uint16 strong, Uint16 weak, Uint32 duration_ms) {
    if (!entry || !entry->gamepad) {
        // 手柄已经被撤走（连接断开时虚拟手柄会注销）：也要留痕，否则看不出
        // 「停震那一下」到底有没有发出去。
        // The pad is gone (the virtual pad is withdrawn when the connection
        // drops): leave a trace anyway, otherwise there is no way to tell
        // whether the stop was ever sent.
        printf("RUMBLE id=0 ok=0 strong=%u weak=%u (no gamepad)\n", strong, weak);
        return;
    }
    bool ok = SDL_RumbleGamepad(entry->gamepad, strong, weak, duration_ms);
    printf("RUMBLE id=%u ok=%d strong=%u weak=%u at=%llu\n", entry->id, ok ? 1 : 0, strong,
           weak, (unsigned long long)SDL_GetTicks());
}

int main(int argc, char **argv) {
    int seconds = argc > 1 ? atoi(argv[1]) : 4;

    if (!SDL_Init(SDL_INIT_GAMEPAD)) {
        printf("SDL_Init failed: %s\n", SDL_GetError());
        return 1;
    }

    int count = 0;
    SDL_JoystickID *ids = SDL_GetGamepads(&count);
    printf("GAMEPADS %d\n", count);
    for (int i = 0; i < count; i++) open_pad(ids[i]);
    SDL_free(ids);

    Uint64 deadline = SDL_GetTicks() + (Uint64)seconds * 1000;
    // 震动一次再停掉：停掉那一下必须是 (0, 0)，宿主据此让马达停下。
    // Rumble once and then stop: the stop must arrive as (0, 0), which is what
    // the host uses to stop the motor.
    Uint64 rumble_at = SDL_GetTicks() + 300;
    Uint64 rumble_stop_at = SDL_GetTicks() + 500;
    int rumble_started = 0, rumble_stopped = 0;
    while (SDL_GetTicks() < deadline) {
        SDL_Event event;
        while (SDL_PollEvent(&event)) {
            switch (event.type) {
            case SDL_EVENT_GAMEPAD_ADDED:
                printf("ADDED %u\n", event.gdevice.which);
                if (is_connected(event.gdevice.which))
                    open_pad(event.gdevice.which);
                else
                    printf("STALE_ADDED %u\n", event.gdevice.which);
                break;
            case SDL_EVENT_GAMEPAD_REMOVED:
                printf("REMOVED %u\n", event.gdevice.which);
                close_pad(event.gdevice.which);
                break;
            case SDL_EVENT_GAMEPAD_BUTTON_DOWN:
            case SDL_EVENT_GAMEPAD_BUTTON_UP:
                printf("BUTTON %u %d %d\n", event.gbutton.which, event.gbutton.button,
                       event.type == SDL_EVENT_GAMEPAD_BUTTON_DOWN ? 1 : 0);
                break;
            case SDL_EVENT_GAMEPAD_AXIS_MOTION:
                printf("AXIS %u %d %d\n", event.gaxis.which, event.gaxis.axis,
                       event.gaxis.value);
                break;
            case SDL_EVENT_GAMEPAD_TOUCHPAD_DOWN:
            case SDL_EVENT_GAMEPAD_TOUCHPAD_MOTION:
            case SDL_EVENT_GAMEPAD_TOUCHPAD_UP:
                printf("TOUCH %u %d %d %.3f %.3f %.3f\n", event.gtouchpad.which,
                       event.gtouchpad.type, event.gtouchpad.finger, event.gtouchpad.x,
                       event.gtouchpad.y, event.gtouchpad.pressure);
                break;
            default:
                break;
            }
        }

        for (int i = 0; i < MAX_TEST_PADS; i++) {
            if (!pads[i].gamepad) continue;
            report_button_state(&pads[i]);
            report_touch_state(&pads[i]);
        }
        if (!rumble_started && SDL_GetTicks() >= rumble_at) {
            rumble_started = 1;
            rumble(&pads[0], 40000, 20000, 1000);
        }
        if (rumble_started && !rumble_stopped && SDL_GetTicks() >= rumble_stop_at) {
            rumble_stopped = 1;
            rumble(&pads[0], 0, 0, 0);
        }
        SDL_Delay(5);
    }

    for (int i = 0; i < MAX_TEST_PADS; i++)
        if (pads[i].gamepad) SDL_CloseGamepad(pads[i].gamepad);
    SDL_Quit();
    return 0;
}

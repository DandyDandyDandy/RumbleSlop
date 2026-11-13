// Keyboard and mouse buttons as real input devices of the game.
//
// The game reads everything through "actions" (Wonder.AppInput.State.Action*, ActionAnalogValue): an action is bound
// to a pad button, and in the game's PC code also to keyboard keys and mouse buttons. The Switch build keeps the
// bindings' data but compiles the reading out: ActionAssignKeyboard.KeyDef and friends return a shared "unused"
// object and State.checkAction only asks the pad. This file puts the missing half back:
//   * the default keyboard/mouse bindings are the game's own: the class constructor of AppInput still passes them
//     to KeyDef / KeyArrayDef / AnalogKeyDef / ActionAssignMouse.ButtonDef, where hooks collect them per action;
//   * State.checkAction and State.ActionAnalogValue additionally ask the keyboard and the mouse buttons, with the
//     same gates as the pad (valid state, active layer, device mask, the chat wheel's blocked actions);
//   * keys are identified as the game does (UnityEngine.InputSystem.Key, a physical key position), read with
//     GetAsyncKeyState through the scan code, so WASD stays where it is on any keyboard layout.
//   * the mouse wheel moves through lists: one notch is a tap of the Up or Down arrow key, which the menus
//     already understand. Not while a stage is being played, where those keys steer.
// The keyboard belongs to player 1. The player's own bindings are in <exe dir>\port_keys<BR_PROFILE>.txt.
// Not done: the mouse "virtual pad" bindings (hold a button and drag to tilt or turn the camera).
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "rt.h"

#define FN(off) (GUEST_BASE + (off))
#define F_CHECK_ACTION 0x439a4c0ull         // AppInput.State.checkAction(Action.eID, Func<eButton,bool>, ...)
#define F_ACTION_ANALOG 0x439ace0ull        // AppInput.State.ActionAnalogValue(Action.eID) -> float
#define F_IS_VALID_STATE 0x4385980ull       // AppInput.isValidState (static)
#define F_LAYER_CURRENT 0x4394b70ull        // AppInput.Layer.current (static)
#define F_GET_ASSIGN_SET 0x4386f50ull       // AppInput.actionAssignSet (static)
#define F_HASHSET_CONTAINS 0x30d9ae0ull     // HashSet<enum>.Contains
#define M_HASHSET_ACTION_CONTAINS 0x611afc0ull
#define STATE_ID 0x20                       // AppInput.State fields
#define STATE_LAYER 0x24
#define STATE_DEVICE_MASK 0x78
#define STATE_FUNCS 0x88                    // eight Func<eButton,bool>: hold, down, up, repeat, then the same for FixedUpdate
#define STATE_EMOTE_UI 0x158
#define DEVICE_MASK_GAMEPAD 2
#define ARR_LEN 0x18
#define ARR_DATA 0x20
#define ACTION_COUNT 187
#define ACTION_TITLE_DECIDE 13              // "press any button": any key too
#define ACTION_MOVIE_SKIP 101
#define NKEY 112                            // UnityEngine.InputSystem.Key values 1..110
#define MAX_KEYS 4

extern int rt_text_entry_active(void);      // online_ui.c: a text box is reading the keyboard

// ------------------------------------------------------------------------------------------------ keys
// Key value -> name (as in UnityEngine.InputSystem.Key) and scan code (0x100 = extended); vk != 0 fixes the virtual
// key instead (keys whose scan code depends on NumLock, or that have no layout-dependent meaning).
static const struct { const char *name; uint16_t scan; uint8_t vk; } k_keys[NKEY] = {
    [1] = {"Space", 0x39, VK_SPACE}, {"Enter", 0x1c, VK_RETURN}, {"Tab", 0x0f, VK_TAB}, {"Backquote", 0x29, 0}, {"Quote", 0x28, 0}, {"Semicolon", 0x27, 0},
    {"Comma", 0x33, 0}, {"Period", 0x34, 0}, {"Slash", 0x35, 0}, {"Backslash", 0x2b, 0}, {"LeftBracket", 0x1a, 0}, {"RightBracket", 0x1b, 0},
    {"Minus", 0x0c, 0}, {"Equals", 0x0d, 0},
    {"A", 0x1e, 0}, {"B", 0x30, 0}, {"C", 0x2e, 0}, {"D", 0x20, 0}, {"E", 0x12, 0}, {"F", 0x21, 0}, {"G", 0x22, 0}, {"H", 0x23, 0}, {"I", 0x17, 0},
    {"J", 0x24, 0}, {"K", 0x25, 0}, {"L", 0x26, 0}, {"M", 0x32, 0}, {"N", 0x31, 0}, {"O", 0x18, 0}, {"P", 0x19, 0}, {"Q", 0x10, 0}, {"R", 0x13, 0},
    {"S", 0x1f, 0}, {"T", 0x14, 0}, {"U", 0x16, 0}, {"V", 0x2f, 0}, {"W", 0x11, 0}, {"X", 0x2d, 0}, {"Y", 0x15, 0}, {"Z", 0x2c, 0},
    {"Digit1", 0x02, 0}, {"Digit2", 0x03, 0}, {"Digit3", 0x04, 0}, {"Digit4", 0x05, 0}, {"Digit5", 0x06, 0}, {"Digit6", 0x07, 0}, {"Digit7", 0x08, 0},
    {"Digit8", 0x09, 0}, {"Digit9", 0x0a, 0}, {"Digit0", 0x0b, 0},
    {"LeftShift", 0x2a, VK_LSHIFT}, {"RightShift", 0x36, VK_RSHIFT}, {"LeftAlt", 0x38, VK_LMENU}, {"RightAlt", 0x138, VK_RMENU},
    {"LeftCtrl", 0x1d, VK_LCONTROL}, {"RightCtrl", 0x11d, VK_RCONTROL}, {"LeftMeta", 0x15b, VK_LWIN}, {"RightMeta", 0x15c, VK_RWIN},
    {"ContextMenu", 0x15d, VK_APPS}, {"Escape", 0x01, VK_ESCAPE}, {"LeftArrow", 0x14b, VK_LEFT}, {"RightArrow", 0x14d, VK_RIGHT},
    {"UpArrow", 0x148, VK_UP}, {"DownArrow", 0x150, VK_DOWN}, {"Backspace", 0x0e, VK_BACK}, {"PageDown", 0x151, VK_NEXT}, {"PageUp", 0x149, VK_PRIOR},
    {"Home", 0x147, VK_HOME}, {"End", 0x14f, VK_END}, {"Insert", 0x152, VK_INSERT}, {"Delete", 0x153, VK_DELETE}, {"CapsLock", 0x3a, VK_CAPITAL},
    {"NumLock", 0x145, VK_NUMLOCK}, {"PrintScreen", 0x137, VK_SNAPSHOT}, {"ScrollLock", 0x46, VK_SCROLL}, {"Pause", 0x45, VK_PAUSE},
    {"NumpadEnter", 0x11c, 0}, {"NumpadDivide", 0x135, VK_DIVIDE}, {"NumpadMultiply", 0x37, VK_MULTIPLY}, {"NumpadPlus", 0x4e, VK_ADD},
    {"NumpadMinus", 0x4a, VK_SUBTRACT}, {"NumpadPeriod", 0x53, VK_DECIMAL}, {"NumpadEquals", 0x59, 0},
    {"Numpad0", 0x52, VK_NUMPAD0}, {"Numpad1", 0x4f, VK_NUMPAD1}, {"Numpad2", 0x50, VK_NUMPAD2}, {"Numpad3", 0x51, VK_NUMPAD3}, {"Numpad4", 0x4b, VK_NUMPAD4},
    {"Numpad5", 0x4c, VK_NUMPAD5}, {"Numpad6", 0x4d, VK_NUMPAD6}, {"Numpad7", 0x47, VK_NUMPAD7}, {"Numpad8", 0x48, VK_NUMPAD8}, {"Numpad9", 0x49, VK_NUMPAD9},
    {"F1", 0x3b, VK_F1}, {"F2", 0x3c, VK_F2}, {"F3", 0x3d, VK_F3}, {"F4", 0x3e, VK_F4}, {"F5", 0x3f, VK_F5}, {"F6", 0x40, VK_F6}, {"F7", 0x41, VK_F7},
    {"F8", 0x42, VK_F8}, {"F9", 0x43, VK_F9}, {"F10", 0x44, VK_F10}, {"F11", 0x57, VK_F11}, {"F12", 0x58, VK_F12},
};
#define KEY_ENTER 2
#define KEY_ESCAPE 60
#define KEY_LEFT 61
#define KEY_DOWN 64
#define KEY_NUMPAD_ENTER 77
static uint8_t g_vk[NKEY];                   // virtual key of each Key under the current keyboard layout
static void vk_init(void) {
    for (int k = 1; k < NKEY; k++) {
        if (!k_keys[k].name) continue;
        g_vk[k] = k_keys[k].vk;
        if (!g_vk[k] && k != KEY_NUMPAD_ENTER) g_vk[k] = (uint8_t)MapVirtualKeyW(k_keys[k].scan, MAPVK_VSC_TO_VK);
    }
}
int pc_key_by_name(const char *name) {
    for (int k = 1; k < NKEY; k++)
        if (k_keys[k].name && !_stricmp(k_keys[k].name, name)) return k;
    return 0;
}
const char *pc_key_name(int key) { return key > 0 && key < NKEY && k_keys[key].name ? k_keys[key].name : ""; }

// State of the keys, sampled once per frame (AppInput.Update) and once per physics step (FixedUpdate), like the pad.
typedef struct {
    uint8_t cur[NKEY], prev[NKEY], repeat[NKEY];
    float timer[NKEY];
    uint8_t mouse, mouse_prev;               // eMouseButton bits: 1 left, 2 right, 4 middle, 8 back, 16 forward
    uint64_t stamp;
} KeySet;
static KeySet g_frame, g_fixed;
static float g_analog[NKEY];                  // 0..1 ramp of each key for analog bindings (stage tilt, camera)
static uint64_t g_ramp_stamp;
static uint64_t g_test_until[NKEY];           // BR_INPUT_LIVE "KEY <name> [ms]"
static float g_repeat_start = 0.4f, g_repeat_interval = 0.1f;
static uint64_t g_kbm_tick;                   // last time a key or mouse button was pressed (see pc_kbm_last_input)
uint64_t pc_kbm_last_input(void) { return g_kbm_tick; }

static int window_focused(void) {
    extern int rt_no_real_input(void);
    DWORD pid = 0;
    HWND w = GetForegroundWindow();
    if (rt_no_real_input()) return 0;
    if (w) GetWindowThreadProcessId(w, &pid);
    return pid == GetCurrentProcessId();
}
// ---- the mouse wheel. The window's messages are read through a subclassed window procedure (the wheel has no
// state to poll); notches wait in g_wheel and are played as arrow-key taps, one every WHEEL_STEP_MS.
#define KEY_UP_ARROW 63
#define WHEEL_TAP_MS 40
#define WHEEL_STEP_MS 70
#define F_MAIN_GAME_STATE 0x42a1ed0ull      // Wonder.MainGame.get_mainGameState (static): 4 = a stage is being played
static volatile LONG g_wheel;                // notches not yet played: > 0 up (away from the user), < 0 down
static WNDPROC g_wheel_old_proc;
static int g_wheel_blocked;
static LRESULT CALLBACK wheel_proc(HWND h, UINT m, WPARAM w, LPARAM l) {
    extern int rt_no_real_input(void);
    if (m == WM_MOUSEWHEEL && !rt_no_real_input()) {
        static int rest;                         // precision wheels and touchpads send fractions of a notch
        rest += GET_WHEEL_DELTA_WPARAM(w);
        int n = rest / WHEEL_DELTA;
        rest -= n * WHEEL_DELTA;
        if (n) InterlockedExchangeAdd(&g_wheel, n);
    }
    return CallWindowProcW(g_wheel_old_proc, h, m, w, l);
}
static BOOL CALLBACK wheel_window(HWND h, LPARAM out) {
    if (!IsWindowVisible(h) || GetWindow(h, GW_OWNER)) return TRUE;
    *(HWND *)out = h;
    return FALSE;
}
void pc_input_test_wheel(int notches) { InterlockedExchangeAdd(&g_wheel, notches); }      // BR_INPUT_LIVE "WHEEL <n>"
static void wheel_tick(Cpu *c) {              // once a frame, on the thread that owns the window
    if (!g_wheel_old_proc) {
        HWND w = NULL;
        EnumThreadWindows(GetCurrentThreadId(), wheel_window, (LPARAM)&w);
        if (w) g_wheel_old_proc = (WNDPROC)SetWindowLongPtrW(w, GWLP_WNDPROC, (LONG_PTR)wheel_proc);
    }
    g_wheel_blocked = (int)rt_call(c, FN(F_MAIN_GAME_STATE), 1, 0ull) == 4;
}

void pc_input_test_key(const char *name, int ms) {
    int k = pc_key_by_name(name);
    if (k) g_test_until[k] = GetTickCount64() + (uint64_t)(ms > 0 ? ms : 100);
    else rt_log("pc input: unknown key name %s", name);
}
// While a text box has the keyboard, only the keys that cannot type keep their meaning.
static int key_allowed_in_text(int k) { return k == KEY_ENTER || k == KEY_NUMPAD_ENTER || k == KEY_ESCAPE || (k >= KEY_LEFT && k <= KEY_DOWN); }
static void sample(KeySet *s) {
    static int init;
    if (!init) { init = 1; vk_init(); }
    uint64_t now = GetTickCount64();
    float dt = s->stamp ? (float)(now - s->stamp) / 1000.0f : 0;
    int focus = window_focused(), text = rt_text_entry_active();
    s->stamp = now;
    if (dt > 0.25f) dt = 0.25f;
    s->mouse_prev = s->mouse;
    s->mouse = 0;
    if (focus) {
        if (GetAsyncKeyState(VK_LBUTTON) & 0x8000) s->mouse |= 1;
        if (GetAsyncKeyState(VK_RBUTTON) & 0x8000) s->mouse |= 2;
        if (GetAsyncKeyState(VK_MBUTTON) & 0x8000) s->mouse |= 4;
        if (GetAsyncKeyState(VK_XBUTTON1) & 0x8000) s->mouse |= 8;
        if (GetAsyncKeyState(VK_XBUTTON2) & 0x8000) s->mouse |= 16;
    }
    if (s->mouse & ~s->mouse_prev) g_kbm_tick = now;
    {                                           // the wheel: the next waiting notch becomes an arrow-key tap
        static uint64_t next;
        if (g_wheel_blocked || text) InterlockedExchange(&g_wheel, 0);
        else if (g_wheel && now >= next) {
            LONG n = g_wheel;
            if (n > 8 || n < -8) n = InterlockedExchange(&g_wheel, n > 0 ? 8 : -8) > 0 ? 8 : -8;     // a spin does not queue for seconds
            InterlockedExchangeAdd(&g_wheel, n > 0 ? -1 : 1);
            g_test_until[n > 0 ? KEY_UP_ARROW : KEY_DOWN] = now + WHEEL_TAP_MS;
            next = now + WHEEL_STEP_MS;
            g_kbm_tick = now;
        }
    }
    for (int k = 1; k < NKEY; k++) {
        int down = 0;
        if (g_test_until[k] > now) down = 1;
        else if (focus && g_vk[k]) down = (GetAsyncKeyState(g_vk[k]) & 0x8000) != 0;
        else if (focus && k == KEY_NUMPAD_ENTER) down = 0;           // shares VK_RETURN with Enter: counted there
        if (text && !key_allowed_in_text(k)) down = 0;
        s->prev[k] = s->cur[k];
        s->cur[k] = (uint8_t)down;
        s->repeat[k] = 0;
        if (down && !s->prev[k]) { s->repeat[k] = 1; s->timer[k] = g_repeat_start; g_kbm_tick = now; }
        else if (down) {
            s->timer[k] -= dt;
            if (s->timer[k] <= 0) { s->repeat[k] = 1; s->timer[k] += g_repeat_interval; if (s->timer[k] < 0) s->timer[k] = 0; }
        }
    }
}

// ------------------------------------------------------------------------------------------------ bindings
typedef struct {
    uint8_t nkeys, keys[MAX_KEYS];
    uint8_t analog;                           // AnalogKeyDef: the value ramps up and down over the two spans
    uint16_t start_ms, end_ms;
    uint8_t mouse;                            // eMouseButton mask of a plain mouse button binding
} Binding;
static Binding g_default[ACTION_COUNT], g_bind[ACTION_COUNT];
static Binding g_pending;                     // defs seen since the last ActionAssignDatum constructor
static int g_loaded, g_captured;

// ActionAssignKeyboard.KeyDef(Key, bool), KeyArrayDef(Key[], bool), AnalogKeyDef(Key, int startMS, int endMS, bool)
// and ActionAssignMouse.ButtonDef(eMouseButton): called by AppInput's class constructor while it builds the default
// table, each followed by the constructor of the ActionAssignDatum they belong to.
int rt_hook_4395740(Cpu *c) { g_pending.nkeys = 1; g_pending.keys[0] = (uint8_t)W(0); g_pending.analog = 0; return 0; }
int rt_hook_4395810(Cpu *c) {
    uint64_t arr = X(0);
    int n = arr ? (int)LD32(arr + ARR_LEN) : 0;
    g_pending.nkeys = 0; g_pending.analog = 0;
    for (int i = 0; i < n && i < MAX_KEYS; i++) g_pending.keys[g_pending.nkeys++] = (uint8_t)LD32(arr + ARR_DATA + 4 * (uint64_t)i);
    return 0;
}
int rt_hook_43958e0(Cpu *c) {
    g_pending.nkeys = 1; g_pending.keys[0] = (uint8_t)W(0); g_pending.analog = 1;
    g_pending.start_ms = (uint16_t)W(1); g_pending.end_ms = (uint16_t)W(2);
    return 0;
}
int rt_hook_4395060(Cpu *c) { g_pending.mouse = (uint8_t)W(0); return 0; }
static int datum(Cpu *c) {                    // ActionAssignDatum..ctor(Action.eID in_ActionID, ...): seven overloads
    uint32_t a = W(1);
    if (a < ACTION_COUNT) { g_default[a] = g_pending; g_captured++; }
    memset(&g_pending, 0, sizeof g_pending);
    return 0;
}
int rt_hook_4395dd0(Cpu *c) { return datum(c); }
int rt_hook_4395e70(Cpu *c) { return datum(c); }
int rt_hook_4395f00(Cpu *c) { return datum(c); }
int rt_hook_4396040(Cpu *c) { return datum(c); }
int rt_hook_4396170(Cpu *c) { return datum(c); }
int rt_hook_43962b0(Cpu *c) { return datum(c); }
int rt_hook_43963e0(Cpu *c) { return datum(c); }

static void keys_path(char *out, size_t n) {
    char exe[MAX_PATH];
    const char *prof = getenv("BR_PROFILE");
    GetModuleFileNameA(NULL, exe, sizeof exe);
    char *s = strrchr(exe, '\\');
    if (s) s[1] = 0;
    snprintf(out, n, "%sport_keys%s.txt", exe, prof ? prof : "");
}
// The file: one line per changed action, "<action id> <key name> [<key name> ...]" (or "<action id> none").
static void bindings_load(void) {
    char path[MAX_PATH + 32], line[256];
    g_loaded = 1;
    memcpy(g_bind, g_default, sizeof g_bind);
    keys_path(path, sizeof path);
    FILE *f = fopen(path, "r");
    if (!f) return;
    while (fgets(line, sizeof line, f)) {
        int a = atoi(line), n = 0;
        char *tok = strtok(line, " \t\r\n");
        if (!tok || a <= 0 || a >= ACTION_COUNT) continue;
        g_bind[a].nkeys = 0;
        while ((tok = strtok(NULL, " \t\r\n")) != NULL && n < MAX_KEYS) {
            int k = pc_key_by_name(tok);
            if (k) g_bind[a].keys[n++] = (uint8_t)k;
        }
        g_bind[a].nkeys = (uint8_t)n;
    }
    fclose(f);
}
static void bindings_save(void) {
    char path[MAX_PATH + 32];
    keys_path(path, sizeof path);
    FILE *f = fopen(path, "w");
    if (!f) return;
    for (int a = 1; a < ACTION_COUNT; a++) {
        if (g_bind[a].nkeys == g_default[a].nkeys && !memcmp(g_bind[a].keys, g_default[a].keys, g_bind[a].nkeys)) continue;
        fprintf(f, "%d", a);
        for (int i = 0; i < g_bind[a].nkeys; i++) fprintf(f, " %s", pc_key_name(g_bind[a].keys[i]));
        fprintf(f, "%s\n", g_bind[a].nkeys ? "" : " none");
    }
    fclose(f);
}
const Binding *pc_binding(int action) {
    if (!g_loaded) bindings_load();
    return action > 0 && action < ACTION_COUNT ? &g_bind[action] : NULL;
}
// The key shown for an action (0 = none), and rebinding: `key` replaces all keys of the action; another action of
// the same group that had it takes the old key (a swap), as the game does for pad buttons.
int pc_action_key(int action) { const Binding *b = pc_binding(action); return b && b->nkeys ? b->keys[0] : 0; }
int pc_action_mouse(int action) { const Binding *b = pc_binding(action); return b ? b->mouse : 0; }
int pc_key_vk(int key) { if (!g_vk[KEY_ENTER]) vk_init(); return key > 0 && key < NKEY ? g_vk[key] : 0; }
void pc_action_set_key(int action, int key) {
    const Binding *old = pc_binding(action);
    if (!old || key <= 0 || key >= NKEY) return;
    int prev = old->nkeys ? old->keys[0] : 0, group = action >= 46 && action <= 93;   // main game actions share keys only among themselves
    for (int a = 1; a < ACTION_COUNT; a++) {
        if (a == action || (a >= 46 && a <= 93) != group) continue;
        for (int i = 0; i < g_bind[a].nkeys; i++)
            if (g_bind[a].keys[i] == key && g_bind[a].nkeys == 1 && prev) g_bind[a].keys[i] = (uint8_t)prev;
    }
    g_bind[action].nkeys = 1; g_bind[action].keys[0] = (uint8_t)key;
    bindings_save();
}
void pc_action_reset(int action) {
    if (!g_loaded) bindings_load();
    if (action > 0 && action < ACTION_COUNT) { g_bind[action] = g_default[action]; bindings_save(); }
}

// ------------------------------------------------------------------------------------------------ the game's side
// The gates State.checkAction applies before it asks the pad, for the keyboard: 1 = this state may see input now.
static int state_takes_input(Cpu *c, uint64_t state, uint32_t action) {
    if (LD32(state + STATE_ID) != 0) return 0;                       // the keyboard is player 1's
    if (!(LD32(state + STATE_DEVICE_MASK) & DEVICE_MASK_GAMEPAD)) return 0;
    if (!(rt_call(c, FN(F_IS_VALID_STATE), 1, 0ull) & 1)) return 0;
    if ((uint32_t)rt_call(c, FN(F_LAYER_CURRENT), 1, 0ull) != LD32(state + STATE_LAYER)) return 0;
    if (LD32(state + STATE_EMOTE_UI) & 0xff) {                       // chat wheel open: some actions are off
        uint64_t set = rt_call(c, FN(F_GET_ASSIGN_SET), 1, 0ull), arr = set ? LD64(set + 0x10) : 0;
        if (arr && LD32(arr + ARR_LEN) >= 2) {
            uint64_t mi = LD64(FN(M_HASHSET_ACTION_CONTAINS));
            if (rt_call(c, FN(F_HASHSET_CONTAINS), 3, LD64(arr + ARR_DATA + 8), (uint64_t)action, LD64(mi)) & 1) return 0;
        }
    }
    return 1;
}
// kind: 0 hold, 1 down, 2 up, 3 repeat
static int binding_active(const Binding *b, const KeySet *s, int kind, uint32_t action) {
    for (int i = 0; i < b->nkeys; i++) {
        int k = b->keys[i];
        if (k <= 0 || k >= NKEY) continue;
        if (kind == 0 ? s->cur[k] : kind == 1 ? (s->cur[k] && !s->prev[k]) : kind == 2 ? (!s->cur[k] && s->prev[k]) : s->repeat[k]) return 1;
    }
    if (b->mouse) {
        int cur = s->mouse & b->mouse, prev = s->mouse_prev & b->mouse;
        if (kind == 0 ? cur != 0 : kind == 1 || kind == 3 ? (cur & ~prev) != 0 : (prev & ~cur) != 0) return 1;
    }
    if (action == ACTION_TITLE_DECIDE || action == ACTION_MOVIE_SKIP)    // "any button": any key except Esc
        for (int k = 1; k < NKEY; k++)
            if (k != KEY_ESCAPE && (kind == 0 ? s->cur[k] : kind == 1 || kind == 3 ? (s->cur[k] && !s->prev[k]) : (!s->cur[k] && s->prev[k]))) return 1;
    return 0;
}
static int g_enabled = -1;
static int enabled(void) {
    if (g_enabled < 0) g_enabled = !getenv("BR_KB_PAD");            // BR_KB_PAD=1: the old keyboard-as-controller mapping
    return g_enabled;
}
int pc_keyboard_actions(void) { return enabled(); }

// AppInput.State.checkAction(Action.eID, Func<eButton,bool> controllerFunc, ...): the pad first, then our devices.
int rt_hook_439a4c0(Cpu *c) {
    static __thread int inside;
    if (inside || !enabled()) return 0;
    uint64_t state = X(0), func = X(2), a3 = X(3), a4 = X(4);
    uint32_t action = W(1);
    inside = 1;
    uint64_t r = rt_call(c, FN(F_CHECK_ACTION), 5, state, (uint64_t)action, func, a3, a4) & 1;
    inside = 0;
    if (!r && func && action < ACTION_COUNT) {
        int which = -1;
        for (int i = 0; i < 8 && which < 0; i++)
            if (LD64(state + STATE_FUNCS + 8 * (uint64_t)i) == func) which = i;
        const Binding *b = pc_binding((int)action);
        if (which >= 0 && b && (b->nkeys || b->mouse || action == ACTION_TITLE_DECIDE || action == ACTION_MOVIE_SKIP) && state_takes_input(c, state, action))
            r = (uint64_t)binding_active(b, which < 4 ? &g_frame : &g_fixed, which & 3, action);
    }
    c->x[0] = r;
    return 1;
}
// AppInput.State.ActionAnalogValue(Action.eID): 0..1. A held key counts fully, an analog key by its ramp.
int rt_hook_439ace0(Cpu *c) {
    static __thread int inside;
    if (inside || !enabled()) return 0;
    uint64_t state = X(0);
    uint32_t action = W(1);
    inside = 1;
    rt_call(c, FN(F_ACTION_ANALOG), 2, state, (uint64_t)action);
    inside = 0;
    float v = rt_getf(c, 0);
    const Binding *b = action < ACTION_COUNT ? pc_binding((int)action) : NULL;
    if (b && b->nkeys && v < 1.0f && state_takes_input(c, state, action))
        for (int i = 0; i < b->nkeys; i++) {
            int k = b->keys[i];
            float kv = k > 0 && k < NKEY ? (b->analog ? g_analog[k] : (float)g_frame.cur[k]) : 0;
            if (kv > v) v = kv;
        }
    rt_setf(c, 0, v);
    return 1;
}
// AppInput.Update() / FixedUpdate(): sample our devices where the game samples the pads.
// {+0x3c float repeat start time, +0x40 float repeat interval}
int rt_hook_4387310(Cpu *c) {
    if (!enabled()) return 0;
    uint64_t self = X(0);
    float a, b;
    memcpy(&a, (void *)(uintptr_t)(self + 0x3c), 4); memcpy(&b, (void *)(uintptr_t)(self + 0x40), 4);
    if (a > 0.05f && a < 2 && b > 0.01f && b < 2) { g_repeat_start = a; g_repeat_interval = b; }
    sample(&g_frame);
    uint64_t now = GetTickCount64();
    float dt = g_ramp_stamp ? (float)(now - g_ramp_stamp) : 0;       // ms
    g_ramp_stamp = now;
    if (!g_loaded) bindings_load();
    for (int act = 1; act < ACTION_COUNT; act++) {                    // ramps of the analog keys
        const Binding *bd = &g_bind[act];
        if (!bd->analog) continue;
        for (int i = 0; i < bd->nkeys; i++) {
            int k = bd->keys[i];
            if (k <= 0 || k >= NKEY) continue;
            float up = bd->start_ms ? dt / (float)bd->start_ms : 1, down = bd->end_ms ? dt / (float)bd->end_ms : 1;
            float v = g_analog[k] + (g_frame.cur[k] ? up : -down);
            g_analog[k] = v < 0 ? 0 : v > 1 ? 1 : v;
        }
    }
    return 0;
}
int rt_hook_4387af0(Cpu *c) {
    if (enabled()) sample(&g_fixed);
    return 0;
}

// ------------------------------------------------------------------------------------------------ mouse on the UI
// Every button of the game already has a mouse handler (UIButtonMouseAction: the detector of its screen, the input
// it stands for, click and hover callbacks); what this build lacks is the pointer events that call them. They are
// produced here: SetMouseAction registers each button, and when a screen polls its detector the pending click or
// the moved cursor is tested against that detector's buttons and handed to the handler, exactly what the engine's
// event system would have done. Because it happens while the screen polls, a screen that is not taking input (a
// dialog is up, a fade is running) is not clickable either.
#define F_MONITOR_INPUT 0x4549be0ull        // UIUserInputDetector.MonitorInputOnUpdate()
#define F_CHECK_FADE 0x4549cb0ull           // UIUserInputDetector.ckeckFadeState(): 1 = no fade in the way
#define F_MOUSE_ON_CLICK 0x4548d70ull       // UIButtonMouseAction.OnClick(UIPartViewButtonBase)
#define F_MOUSE_ON_ENTER 0x4548da0ull       // UIButtonMouseAction.OnPointerEnter(UIPartViewButtonBase)
#define F_PLAY_ANIMATION 0x44ec3b0ull       // UIPartViewButtonBase.PlayAnimation(eAnimationKind, bool isDisable, float normalizedTime)
#define F_GCHANDLE_NEW 0x4657a0ull
#define F_GCHANDLE_TARGET 0x4657f0ull
#define F_GCHANDLE_FREE 0x465810ull
#define F_OBJ_ALIVE 0x3ee4c20ull            // UnityEngine.Object::op_Implicit (static)
#define F_OBJ_NAME 0x3ee4e30ull
#define F_GET_GAMEOBJECT 0x3edc5c0ull
#define DET_USER_INPUT 0x38                 // UIUserInputDetector: m_UserInput, targetLayer, standing Back button
#define DET_LAYER 0x28
#define DET_RETURN_BUTTON 0x50
#define VIEW_DISABLED 0x28                  // UIPartViewButtonBase: isDisable, buttonMouseAction
#define VIEW_MOUSE_ACTION 0x30
#define MA_DETECTOR 0x18                    // UIButtonMouseAction.m_InputActionDetector
#define MA_SELECTABLE 0x20
#define INPUT_CANCEL 2                      // eUserInput
#define INPUT_PAUSE 17
#define ACTION_CANCEL 2
#define ACTION_PAUSE 3
#define MAX_BUTTONS 768
extern int rt_ui_click_pending(void);       // online_ui.c
extern void rt_ui_click_consume(void);
extern int rt_ui_mouse_moved(void);
extern float rt_ui_hit(Cpu *c, uint64_t view, int hover);
extern void rt_ui_press_anim(Cpu *c, uint64_t view);
extern uint64_t name_ui_after_monitor(Cpu *c, uint64_t det, uint64_t r);
static struct { uint64_t view; uint32_t handle; } g_buttons[MAX_BUTTONS];
static uint64_t g_hover_view;

// UIPartViewButtonBase.SetMouseAction(UIButtonMouseAction): remember the button.
int rt_hook_44ec770(Cpu *c) {
    uint64_t view = X(0), action = X(1), lr = c->x[30];
    int free_slot = -1;
    if (!view || !action) return 0;
    for (int i = 0; i < MAX_BUTTONS; i++) {
        if (g_buttons[i].view == view) return 0;
        if (!g_buttons[i].handle && free_slot < 0) free_slot = i;
    }
    if (free_slot >= 0) {
        g_buttons[free_slot].handle = (uint32_t)rt_call(c, FN(F_GCHANDLE_NEW), 2, view, 0ull);
        g_buttons[free_slot].view = view;
        c->x[0] = view; c->x[1] = action; c->x[30] = lr;
    }
    return 0;
}
static void buttons_sweep(Cpu *c) {          // forget destroyed buttons, a few per frame
    static int at;
    for (int n = 0; n < 16; n++, at = (at + 1) % MAX_BUTTONS) {
        if (!g_buttons[at].handle) continue;
        if (!(rt_call(c, FN(F_OBJ_ALIVE), 2, g_buttons[at].view, 0ull) & 1)) {
            rt_call(c, FN(F_GCHANDLE_FREE), 1, (uint64_t)g_buttons[at].handle);
            g_buttons[at].handle = 0; g_buttons[at].view = 0;
        }
    }
}
// The button of this detector under the click (or the cursor): the smallest one when several overlap.
static uint64_t button_at(Cpu *c, uint64_t det, int hover) {
    uint64_t best = 0;
    float best_area = 0;
    for (int i = 0; i < MAX_BUTTONS; i++) {
        uint64_t v = g_buttons[i].view, ma = v ? LD64(v + VIEW_MOUSE_ACTION) : 0;
        if (!ma || LD64(ma + MA_DETECTOR) != det) continue;
        if (!(rt_call(c, FN(F_OBJ_ALIVE), 2, v, 0ull) & 1)) continue;
        float area = rt_ui_hit(c, v, hover);
        if (area > 0 && (!best || area < best_area)) { best = v; best_area = area; }
    }
    return best;
}
static int name_has(Cpu *c, uint64_t view, const char *part) {
    char name[96] = "";
    uint64_t s = rt_call(c, FN(F_OBJ_NAME), 2, rt_call(c, FN(F_GET_GAMEOBJECT), 2, view, 0ull), 0ull);
    for (int i = 0; s && i < 95 && i < (int)LD32(s + 0x10); i++) { name[i] = (char)LD16(s + 0x14 + 2 * (uint64_t)i); name[i + 1] = 0; }
    return strstr(name, part) != NULL;
}
static uint64_t ui_mouse(Cpu *c, uint64_t det, uint64_t r) {
    if ((uint32_t)r != 0 || (!rt_ui_click_pending() && !rt_ui_mouse_moved())) return r;
    uint32_t layer = LD32(det + DET_LAYER);
    if ((uint32_t)rt_call(c, FN(F_LAYER_CURRENT), 1, 0ull) != layer) return r;
    if ((layer & ~1u) != 4 && !(rt_call(c, FN(F_CHECK_FADE), 2, det, 0ull) & 1)) return r;
    if (rt_ui_click_pending()) {
        uint64_t back = LD64(det + DET_RETURN_BUTTON), v;
        if (back && (rt_call(c, FN(F_OBJ_ALIVE), 2, back, 0ull) & 1) && rt_ui_hit(c, back, 0) > 0) {   // the standing Back button: Cancel
            rt_ui_click_consume();
            c->x[0] = back; c->x[1] = 6; c->x[2] = LD32(back + VIEW_DISABLED) & 0xff; c->x[3] = 0;     // eAnimationKind.PressedToHighlighted
            rt_setf(c, 0, 0.0f);
            rt_call_regs(c, FN(F_PLAY_ANIMATION));
            ST32(det + DET_USER_INPUT, INPUT_CANCEL);
            return INPUT_CANCEL;
        }
        if ((v = button_at(c, det, 0)) != 0) {
            rt_ui_click_consume();
            if (name_has(c, v, "DisplayPassword") || name_has(c, v, "CopyRoomName")) rt_ui_press_anim(c, v);
            rt_call(c, FN(F_MOUSE_ON_CLICK), 3, LD64(v + VIEW_MOUSE_ACTION), v, 0ull);
            g_kbm_tick = GetTickCount64();
            return LD32(det + DET_USER_INPUT);
        }
        return r;
    }
    uint64_t v = button_at(c, det, 1);                               // the cursor moved: focus follows it
    if (v && v != g_hover_view && (LD32(LD64(v + VIEW_MOUSE_ACTION) + MA_SELECTABLE) & 0xff)) {
        g_hover_view = v;
        rt_call(c, FN(F_MOUSE_ON_ENTER), 3, LD64(v + VIEW_MOUSE_ACTION), v, 0ull);
        return LD32(det + DET_USER_INPUT);
    }
    if (!v) g_hover_view = 0;
    return r;
}
// UIUserInputDetector.MonitorInputOnUpdate(): the input the screen sees this frame.
int rt_hook_4549be0(Cpu *c) {
    static __thread int inside;
    if (inside) return 0;
    uint64_t det = X(0);
    inside = 1;
    uint64_t r = rt_call(c, FN(F_MONITOR_INPUT), 2, det, 0ull);
    inside = 0;
    // Esc is both Cancel and Pause in the game's key bindings and the detector lets Pause win, which menus ignore:
    // a Pause that came from the key that is also Cancel counts as Cancel.
    if ((uint32_t)r == INPUT_PAUSE && enabled() && binding_active(pc_binding(ACTION_PAUSE), &g_frame, 1, ACTION_PAUSE) &&
        binding_active(pc_binding(ACTION_CANCEL), &g_frame, 1, ACTION_CANCEL)) {
        r = INPUT_CANCEL;
        ST32(det + DET_USER_INPUT, INPUT_CANCEL);
    }
    r = ui_mouse(c, det, r);
    r = name_ui_after_monitor(c, det, r);
    c->x[0] = r;
    return 1;
}

// ------------------------------------------------------------------------------------------------ key configuration
// Options > Key Configuration is the game's own screen for pad buttons: pick a row, "Press the button you want to
// use!", and the edit state (UIStateOptionKeyConfigEdit) hands the pad state to the row's item, which reads the
// pressed button from it. A pressed keyboard key takes the same road: the edit state is told a button was pushed,
// and the item (UIOptionChoicesItemData_KeyConfig {+0x40 its current SelectButton.eID, +0x44 Action.eID}) binds
// the key to its action instead and reports its pad button unchanged. Esc closes the dialog without a change.
#define F_GET_STATE 0x437ae60ull            // AppInput.GetState(int playerIndex, eLayer) (static)
#define F_EDIT_ON_PUSH 0x44d7d40ull         // UIStateOptionKeyConfigEdit.onPushButton(AppInput.State)
static __thread int t_edit_key;              // the key being assigned (-1: none, just close), while onPushButton runs
int rt_hook_44d7970(Cpu *c) {                // UIStateOptionKeyConfigEdit.exec(float): {+0xa0 eLayer, +0xa8 int playerIndex}
    uint64_t self = X(0);
    int key = 0;
    if (!enabled() || LD32(self + 0xa8) != 0) return 0;
    for (int k = 1; k < NKEY && !key; k++)
        if (g_frame.cur[k] && !g_frame.prev[k]) key = k;
    if (!key) return 0;
    uint64_t state = rt_call(c, FN(F_GET_STATE), 3, (uint64_t)LD32(self + 0xa8), (uint64_t)LD32(self + 0xa0), 0ull);
    t_edit_key = key == KEY_ESCAPE ? -1 : key;
    rt_call(c, FN(F_EDIT_ON_PUSH), 3, self, state, 0ull);
    t_edit_key = 0;
    return 1;
}
// UIOptionChoicesItemData_KeyConfig.ResetValue(): Options > Reset puts the row's pad button back; its key as well.
int rt_hook_453f8e0(Cpu *c) {
    if (enabled()) pc_action_reset((int)LD32(X(0) + 0x44));
    return 0;
}
int rt_hook_45429f0(Cpu *c) {                // UIOptionChoicesItemData_KeyConfig.SetKeyConfigEditButton(AppInput.State) -> SelectButton.eID
    if (!t_edit_key) return 0;
    uint64_t item = X(0);
    if (t_edit_key > 0) {
        pc_action_set_key((int)LD32(item + 0x44), t_edit_key);
        rt_log("pc input: action %d is now on key %s", (int)LD32(item + 0x44), pc_key_name(t_edit_key));
    }
    c->x[0] = LD32(item + 0x40);
    return 1;
}

// ------------------------------------------------------------------------------------------------ diagnostics
void pc_input_tick(Cpu *c) {
    static int dump = -1;
    buttons_sweep(c);
    wheel_tick(c);
    if (dump < 0) dump = getenv("BR_INPUT_DUMP") != NULL;
    if (dump != 1 || !g_captured) return;
    dump = 2;
    if (!g_loaded) bindings_load();
    rt_log("pc input: %d default bindings captured from the game", g_captured);
    for (int a = 1; a < ACTION_COUNT; a++) {
        const Binding *b = &g_bind[a];
        char line[160] = "";
        int n = 0;
        if (!b->nkeys && !b->mouse) continue;
        for (int i = 0; i < b->nkeys; i++) n += snprintf(line + n, sizeof line - n, " %s", pc_key_name(b->keys[i]));
        if (b->analog) n += snprintf(line + n, sizeof line - n, " (analog %d/%d ms)", b->start_ms, b->end_ms);
        if (b->mouse) n += snprintf(line + n, sizeof line - n, " mouse %d", b->mouse);
        rt_log("pc input: action %3d:%s", a, line);
    }
}

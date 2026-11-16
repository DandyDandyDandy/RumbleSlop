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
//   * the mouse wheel scrolls the list under the cursor and a list's scroll bar can be dragged; both move the
//     list itself (UnityEngine.UI.ScrollRect), not the focus. While the mouse is the device used last, the lists
//     do not follow the focused item, as in the game's own PC code. On a screen without a list a wheel notch is
//     a tap of the Up or Down arrow key. Not while a stage is being played, where those keys steer.
// The keyboard belongs to player 1. The player's own bindings are in <exe dir>\port_keys<BR_PROFILE>.txt.
// Not done: the mouse "virtual pad" bindings (hold a button and drag to tilt or turn the camera).
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include "rt.h"

#define FN(off) (GUEST_BASE + (off))
#define F_CHECK_ACTION 0x43c8530ull         // AppInput.State.checkAction(Action.eID, Func<eButton,bool>, ...)
#define F_ACTION_ANALOG 0x43c8d50ull        // AppInput.State.ActionAnalogValue(Action.eID) -> float
#define F_IS_VALID_STATE 0x43b39f0ull       // AppInput.isValidState (static)
#define F_LAYER_CURRENT 0x43c2be0ull        // AppInput.Layer.current (static)
#define F_GET_ASSIGN_SET 0x43b4fc0ull       // AppInput.actionAssignSet (static)
#define F_HASHSET_CONTAINS 0x3100190ull     // HashSet<enum>.Contains
#define M_HASHSET_ACTION_CONTAINS 0x616c098ull
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
#define ACTION_REWIND_FORWARD 90          // MainGame_Rewind_Forward (game's key: RightArrow)
#define ACTION_REWIND_BACKWARD 91         // MainGame_Rewind_Backward (LeftArrow)
#define ACTION_CAMERA_UP 55               // MainGame_Camera_Up, _Down, _Left, _Right, _ZoomIn, _ZoomOut (55..60)
#define ACTION_PHOTO_SELECT 66            // MainGame_PhotoFacialNext, _Previous, EffectNext, _Previous (66..69)
#define ACTION_CAMERA_PAN_UP 74           // MainGame_Camera_Pan_Up, _Down, _Left, _Right (74..77)
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
#define F_MAIN_GAME_STATE 0x42cfd10ull      // Wonder.MainGame.get_mainGameState (static): 4 = a stage is being played
static volatile LONG g_wheel;                // notches not yet played: > 0 up (away from the user), < 0 down
static volatile LONG g_wheel_cam;            // the same notches for the photo / View Stage cameras (mouse camera)
static void photo_taps(uint64_t now);
static uint64_t g_cam_tick;                  // when such a camera last read the wheel
static WNDPROC g_wheel_old_proc;
static int g_wheel_blocked;
static LRESULT CALLBACK wheel_proc(HWND h, UINT m, WPARAM w, LPARAM l) {
    extern int rt_no_real_input(void);
    if (m == WM_MOUSEWHEEL && !rt_no_real_input()) {
        static int rest;                         // precision wheels and touchpads send fractions of a notch
        rest += GET_WHEEL_DELTA_WPARAM(w);
        int n = rest / WHEEL_DELTA;
        rest -= n * WHEEL_DELTA;
        if (n) { InterlockedExchangeAdd(&g_wheel, n); InterlockedExchangeAdd(&g_wheel_cam, n); }
    }
    return CallWindowProcW(g_wheel_old_proc, h, m, w, l);
}
static BOOL CALLBACK wheel_window(HWND h, LPARAM out) {
    if (!IsWindowVisible(h) || GetWindow(h, GW_OWNER)) return TRUE;
    *(HWND *)out = h;
    return FALSE;
}
void pc_input_test_wheel(int notches) { InterlockedExchangeAdd(&g_wheel, notches); InterlockedExchangeAdd(&g_wheel_cam, notches); }      // BR_INPUT_LIVE "WHEEL <n>"

// ---- lists. Every list of the game sits in a UnityEngine.UI.ScrollRect; the game's own code moves its content
// towards the focused item (three Update functions, which the game's PC code switches off while the mouse is the
// device used last: the port takes that decision, see mouse_mode). The mouse moves the ScrollRect itself:
//   * the wheel scrolls the list under the cursor (or the only list on screen) by WHEEL_PART of its height a notch,
//     eased over a few frames;
//   * a press on the scroll bar, or within GRAB_PX beside it, drags the handle: the list is where the handle is.
// The ScrollRects are known from their LateUpdate (hooked); a GC handle keeps each object valid for the host.
#define F_OBJ_ALIVE_ 0x3f10610ull            // UnityEngine.Object::op_Implicit (static)
#define F_GCHANDLE_NEW_ 0x469da0ull
#define F_GCHANDLE_FREE_ 0x469e10ull
#define F_GET_TRANSFORM_ 0x3f07f60ull        // UnityEngine.Component::get_transform
#define F_T_IS_CHILD_OF 0x3f1e670ull         // UnityEngine.Transform::IsChildOf(Transform)
#define F_RT_ANCHORED_POS 0x3f182c0ull       // UnityEngine.RectTransform::get_anchoredPosition
#define F_SR_VIEW_RECT 0x4188a40ull          // UnityEngine.UI.ScrollRect::get_viewRect
#define F_SR_GET_VNORM 0x418b650ull          // ::get_verticalNormalizedPosition (1 = top)
#define F_SR_SET_VNORM 0x418b7f0ull
#define F_SR_GET_HNORM 0x418b530ull          // ::get_horizontalNormalizedPosition (0 = left)
#define F_SR_SET_HNORM 0x418b7d0ull
#define F_SCROLLBAR_DIRECTION 0x41857c0ull   // UnityEngine.UI.Scrollbar::get_direction (3 = top to bottom)
#define SR_CONTENT 0x20                      // ScrollRect: m_Content, m_Horizontal, m_Vertical, m_VerticalScrollbar
#define SR_HORIZONTAL 0x28
#define SR_VERTICAL 0x29
#define SR_VBAR 0x50
#define SR_CONTENT_EXTENT 0x94               // m_ContentBounds.extents (x, y): half the content's size
#define SR_VIEW_EXTENT 0xac                  // m_ViewBounds.extents
#define GRAB_PX 14.0f
#define WHEEL_PART 0.22f
#define WHEEL_EASE 16.0f
#define MAX_LISTS 12
typedef struct {
    uint64_t sr;
    uint32_t handle, seen;                   // GC handle; the frame its LateUpdate ran last
    int cur, axis, has_bar;                  // on screen this frame; 1 scrolls vertically, 2 horizontally, 0 fits
    int dead;                                // its rows belong to screens that are not taking input (list_dead)
                                             // or, during play, to no screen taking input
    uint32_t dead_frame;
    float view[4], bar[4];                   // viewport and scroll bar on screen: x, y, width, height
} List;
static List g_lists[MAX_LISTS];
static uint32_t g_list_frame;
static uint32_t g_click_action, g_click_frame;    // a header button's click: its action reads as pressed in that frame
static int g_list_count;                      // lists on screen that can scroll
static uint64_t g_drag_sr;                    // the list whose bar is being dragged
static uint64_t g_mouse_tick, g_key_tick;     // last use of the mouse on the UI / of a keyboard key
extern uint64_t rt_pad_last_input(uint32_t id);
extern int rt_ui_mouse(float *x, float *y, int *down);
extern int rt_ui_mouse_moved(void);
extern void rt_ui_mouse_touch(void);
extern int rt_ui_rect(Cpu *c, uint64_t view, float r[4]);
extern void rt_ui_click_consume(void);

// Detectors (UIUserInputDetector) that polled input lately, i.e. the screens taking input now (rt_hook_457ac20).
#define MAX_POLLED 16
static struct { uint64_t det; uint32_t frame; } g_polled[MAX_POLLED];
static void polled_note(uint64_t det) {
    int slot = 0;
    for (int i = 0; i < MAX_POLLED; i++) {
        if (g_polled[i].det == det) { slot = i; break; }
        if (g_polled[i].frame < g_polled[slot].frame) slot = i;
    }
    g_polled[slot].det = det; g_polled[slot].frame = g_list_frame;
}
static int polled_now(uint64_t det) {
    for (int i = 0; i < MAX_POLLED; i++)
        if (g_polled[i].det == det && g_list_frame - g_polled[i].frame <= 2) return 1;
    return 0;
}
static float ldf(uint64_t a) { uint32_t u = LD32(a); float f; memcpy(&f, &u, 4); return f; }
static void stf(uint64_t a, float f) { uint32_t u; memcpy(&u, &f, 4); ST32(a, u); }
static int alive(Cpu *c, uint64_t o) { return o && (rt_call(c, FN(F_OBJ_ALIVE_), 2, o, 0ull) & 1); }
static int in_rect(const float r[4], float x, float y, float pad) { return x >= r[0] - pad && x <= r[0] + r[2] + pad && y >= r[1] && y <= r[1] + r[3]; }
static float clamp01(float v) { return v < 0 ? 0 : v > 1 ? 1 : v; }
static float call_f(Cpu *c, uint64_t fn, uint64_t o) { rt_call(c, FN(fn), 2, o, 0ull); return rt_getf(c, 0); }
static void call_set_f(Cpu *c, uint64_t fn, uint64_t o, float v) { c->x[0] = o; c->x[1] = 0; rt_setf(c, 0, v); rt_call_regs(c, FN(fn)); }

// The mouse is the device used last (moved over the window, clicked, wheel): lists stay where the mouse put them.
static int mouse_mode(void) { return g_mouse_tick && g_mouse_tick >= g_key_tick && g_mouse_tick >= rt_pad_last_input(0); }
int pc_mouse_mode(void) { return mouse_mode(); }      // pc_shell.c: the pointer is shown only then

int rt_hook_418aba0(Cpu *c) {                // UnityEngine.UI.ScrollRect.LateUpdate: this list is in use
    uint64_t sr = X(0), lr = c->x[30];
    int free_slot = -1;
    for (int i = 0; i < MAX_LISTS; i++) {
        if (g_lists[i].sr == sr && g_lists[i].handle) { g_lists[i].seen = g_list_frame; return 0; }
        if (!g_lists[i].handle && free_slot < 0) free_slot = i;
    }
    if (free_slot < 0 || !sr) return 0;
    memset(&g_lists[free_slot], 0, sizeof g_lists[0]);
    g_lists[free_slot].handle = (uint32_t)rt_call(c, FN(F_GCHANDLE_NEW_), 2, sr, 0ull);
    g_lists[free_slot].sr = sr;
    g_lists[free_slot].seen = g_list_frame;
    c->x[0] = sr; c->x[30] = lr;
    return 0;
}
// The three functions that move a list's content towards the focused item's place (`target`, a Vector2). With the
// mouse in use they are left out, and the place is kept equal to where the list is, so nothing jumps when a key or
// the pad takes over: the list then moves when the focus does.
static int list_follow(Cpu *c, uint64_t sr, uint64_t target) {
    uint64_t self = X(0), lr = c->x[30];
    if (!mouse_mode() || !sr) return 0;
    uint64_t content = LD64(sr + SR_CONTENT);
    if (content) {
        rt_call(c, FN(F_RT_ANCHORED_POS), 2, content, 0ull);
        stf(target, rt_getf(c, 0)); stf(target + 4, rt_getf(c, 1));
    }
    c->x[0] = self; c->x[30] = lr;
    return 1;
}
int rt_hook_45220f0(Cpu *c) { return list_follow(c, LD64(X(0) + 0x78), X(0) + 0x68); }   // UIPartViewBaseVirtualScrollRect.Update
int rt_hook_45594c0(Cpu *c) { return list_follow(c, LD64(X(0) + 0x10), X(0) + 0x1c); }   // Scroll.UICalcContentPosModule_Base.OnUpdate
int rt_hook_455a260(Cpu *c) { return list_follow(c, LD64(X(0) + 0x28), X(0) + 0x54); }   // Scroll.UIPartView_ScrollRect_ChoicesOp_Duplicatoin_Vertical.Update

// 1 when the button is an item of a list and the point is outside what the list shows: the part of an item that
// is cut off at the list's edge is not there for the mouse.
static int list_hides(Cpu *c, uint64_t view, float x, float y) {
    for (int i = 0; i < MAX_LISTS; i++) {
        List *l = &g_lists[i];
        if (!l->handle || !l->cur || in_rect(l->view, x, y, 0)) continue;
        if (!alive(c, l->sr)) continue;
        uint64_t content = LD64(l->sr + SR_CONTENT);
        if (!alive(c, content)) continue;
        if (rt_call(c, FN(F_T_IS_CHILD_OF), 3, rt_call(c, FN(F_GET_TRANSFORM_), 2, view, 0ull), content, 0ull) & 1) return 1;
    }
    return 0;
}
// 1 when the list's rows are buttons of screens that are not taking input: a screen left behind that is still
// there (Select World's list stays alive, at the same place, under Select Stage) must not take the bar or the
// wheel. A list without such buttons counts as alive, except while a stage is played: then only a list of a screen
// taking input (the pause menu's Options) is used, nothing else may take the mouse from the game. Worked out again
// every 10 frames, or at once with `now`.
static int list_rows_polled(Cpu *c, uint64_t content);      // -1 no rows known, 0 none of a polling screen, 1 some
static int list_dead(Cpu *c, List *l, int now) {
    if (!now && l->dead_frame && g_list_frame - l->dead_frame < 10) return l->dead;
    uint64_t content = LD64(l->sr + SR_CONTENT);
    int rows = alive(c, content) ? list_rows_polled(c, content) : -1;
    l->dead = rows == 0 || (g_wheel_blocked && rows != 1);
    l->dead_frame = g_list_frame ? g_list_frame : 1;
    return l->dead;
}
static List *list_of(uint64_t sr) {
    for (int i = 0; i < MAX_LISTS; i++)
        if (g_lists[i].handle && g_lists[i].cur && g_lists[i].sr == sr) return &g_lists[i];
    return NULL;
}
// Handle length as a part of the bar, and whether the bar runs top to bottom.
static float bar_size(Cpu *c, List *l, int *reverse) {
    float content = ldf(l->sr + SR_CONTENT_EXTENT + 4), view = ldf(l->sr + SR_VIEW_EXTENT + 4);
    *reverse = (int)rt_call(c, FN(F_SCROLLBAR_DIRECTION), 2, LD64(l->sr + SR_VBAR), 0ull) == 3;
    return content > 0 ? clamp01(view / content) : 1;
}
static void lists_tick(Cpu *c) {              // once a frame, after the mouse was read (online_ui.c)
    static int was_down, settle;
    static float grab, anim_left;
    static uint64_t anim_sr, last;
    float x, y;
    int down = 0, inside = rt_ui_mouse(&x, &y, &down), fresh = down && !was_down, log = getenv("BR_DRAG_LOG") != NULL;
    uint64_t now = GetTickCount64();
    float dt = last ? (float)(now - last) / 1000.0f : 0;
    List *over = NULL, *single = NULL;
    was_down = down;
    last = now;
    if (dt > 0.1f) dt = 0.1f;
    if (rt_ui_mouse_moved() || (fresh && inside)) g_mouse_tick = now;
    g_list_count = 0;
    for (int i = 0; i < MAX_LISTS; i++) {
        List *l = &g_lists[i];
        if (!l->handle) continue;
        l->cur = 0;
        if (!alive(c, l->sr) || g_list_frame - l->seen > 600) {     // destroyed, or switched off for a while
            rt_call(c, FN(F_GCHANDLE_FREE_), 1, (uint64_t)l->handle);
            l->handle = 0; l->sr = 0;
            continue;
        }
        if (l->seen != g_list_frame) continue;
        uint64_t vr = rt_call(c, FN(F_SR_VIEW_RECT), 2, l->sr, 0ull);
        if (!alive(c, vr) || !rt_ui_rect(c, vr, l->view)) continue;
        l->cur = 1;
        float hid_x = 2 * (ldf(l->sr + SR_CONTENT_EXTENT) - ldf(l->sr + SR_VIEW_EXTENT));
        float hid_y = 2 * (ldf(l->sr + SR_CONTENT_EXTENT + 4) - ldf(l->sr + SR_VIEW_EXTENT + 4));
        l->axis = (LD32(l->sr + SR_VERTICAL) & 0xff) && hid_y > 1 ? 1 : (LD32(l->sr + SR_HORIZONTAL) & 0xff) && hid_x > 1 ? 2 : 0;
        uint64_t bar = LD64(l->sr + SR_VBAR);
        l->has_bar = l->axis == 1 && alive(c, bar) && rt_ui_rect(c, bar, l->bar) && l->bar[3] >= 8;
        if (!l->axis || list_dead(c, l, 0)) continue;
        g_list_count++;
        single = l;
        if (inside && (in_rect(l->view, x, y, 0) || (l->has_bar && in_rect(l->bar, x, y, GRAB_PX))) &&
            (!over || l->view[2] * l->view[3] < over->view[2] * over->view[3])) over = l;
    }
    g_list_frame++;
    if (!down || !inside) g_drag_sr = 0;
    if (fresh && inside && log)
        for (int i = 0; i < MAX_LISTS; i++) {
            List *l = &g_lists[i];
            if (!l->handle) continue;
            rt_log("drag: press %.0f %.0f; list %llx cur %d axis %d bar %llx has %d (%.0f %.0f %.0f %.0f) view (%.0f %.0f %.0f %.0f) dead %d",
                   x, y, (unsigned long long)l->sr, l->cur, l->axis, (unsigned long long)LD64(l->sr + SR_VBAR), l->has_bar,
                   l->bar[0], l->bar[1], l->bar[2], l->bar[3], l->view[0], l->view[1], l->view[2], l->view[3], l->dead);
        }
    if (fresh && inside) {                       // a press on a scroll bar takes hold of the handle
        for (int i = 0; i < MAX_LISTS; i++) {
            List *l = &g_lists[i];
            int reverse;
            if (!l->handle || !l->cur || !l->has_bar || !in_rect(l->bar, x, y, GRAB_PX) || list_dead(c, l, 1)) continue;
            float size = bar_size(c, l, &reverse), norm = clamp01(call_f(c, F_SR_GET_VNORM, l->sr));
            float from = (reverse ? 1 - norm : norm) * (1 - size), at = (y - l->bar[1]) / l->bar[3];
            grab = at >= from && at <= from + size ? at - from : size / 2;      // beside the handle: it jumps under the cursor
            g_drag_sr = l->sr;
            anim_sr = 0;
            rt_ui_click_consume();
            if (log) rt_log("drag: bar %.0f %.0f %.0f %.0f handle %.2f at %.2f, press at %.2f", l->bar[0], l->bar[1], l->bar[2], l->bar[3], size, from, at);
            break;
        }
    }
    if (g_drag_sr) {
        List *l = list_of(g_drag_sr);
        int reverse;
        float size = l && l->has_bar ? bar_size(c, l, &reverse) : 1;
        if (!l || !l->has_bar) g_drag_sr = 0;
        else if (size < 0.999f) {
            float norm = clamp01(((y - l->bar[1]) / l->bar[3] - grab) / (1 - size));
            call_set_f(c, F_SR_SET_VNORM, l->sr, reverse ? 1 - norm : norm);
            g_mouse_tick = now;
        }
    }

    LONG n = g_list_count ? InterlockedExchange(&g_wheel, 0) : 0;       // without a list the notches are key taps (sample)
    List *t = over ? over : g_list_count == 1 ? single : NULL;
    if (n && t && !g_drag_sr) {
        if (anim_sr != t->sr) { anim_sr = t->sr; anim_left = 0; }
        anim_left += (float)n * WHEEL_PART;
        if (anim_left > 3) anim_left = 3;
        if (anim_left < -3) anim_left = -3;
        g_mouse_tick = now;
        if (log) rt_log("wheel: %d on a list (%s), axis %d", (int)n, over ? "under the cursor" : "the only one", t->axis);
    }
    if (anim_sr) {
        List *l = list_of(anim_sr);
        if (!l || !l->axis) { anim_sr = 0; anim_left = 0; }
        else {
            int v = l->axis == 1;
            float step = anim_left * (1 - expf(-dt * WHEEL_EASE));
            float view = 2 * ldf(l->sr + SR_VIEW_EXTENT + (v ? 4 : 0)), hid = 2 * ldf(l->sr + SR_CONTENT_EXTENT + (v ? 4 : 0)) - view;
            if (anim_left > -0.004f && anim_left < 0.004f) { step = anim_left; anim_sr = 0; }
            anim_left -= step;
            float norm = call_f(c, v ? F_SR_GET_VNORM : F_SR_GET_HNORM, l->sr);
            float to = clamp01(norm + (v ? step : -step) * view / (hid > 1 ? hid : 1));
            if ((to <= 0 || to >= 1) && to == clamp01(norm)) { anim_sr = 0; anim_left = 0; }     // at the end of the list
            call_set_f(c, v ? F_SR_SET_VNORM : F_SR_SET_HNORM, l->sr, to);
            settle = 3;
        }
    }
    if (settle > 0) { settle--; rt_ui_mouse_touch(); }                 // the item now under the cursor gets the focus
}
static void wheel_tick(Cpu *c) {              // once a frame, on the thread that owns the window
    if (!g_wheel_old_proc) {
        HWND w = NULL;
        EnumThreadWindows(GetCurrentThreadId(), wheel_window, (LPARAM)&w);
        if (w) g_wheel_old_proc = (WNDPROC)SetWindowLongPtrW(w, GWLP_WNDPROC, (LONG_PTR)wheel_proc);
    }
    g_wheel_blocked = (int)rt_call(c, FN(F_MAIN_GAME_STATE), 1, 0ull) == 4;
    lists_tick(c);
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
    if (s == &g_frame) photo_taps(now);
    s->mouse_prev = s->mouse;
    s->mouse = 0;
    if (focus) {
        if (GetAsyncKeyState(VK_LBUTTON) & 0x8000) s->mouse |= 1;
        if (GetAsyncKeyState(VK_RBUTTON) & 0x8000) s->mouse |= 2;
        if (GetAsyncKeyState(VK_MBUTTON) & 0x8000) s->mouse |= 4;
        if (GetAsyncKeyState(VK_XBUTTON1) & 0x8000) s->mouse |= 8;
        if (GetAsyncKeyState(VK_XBUTTON2) & 0x8000) s->mouse |= 16;
    }
    {                                           // BR_INPUT_LIVE "HOLD x y": the left button (BR_TEST_HOLD_BUTTON=2 right, 4 middle)
        extern int rt_ui_test_held(void);
        static int button = -1;
        if (button < 0) { const char *e = getenv("BR_TEST_HOLD_BUTTON"); button = e && atoi(e) > 0 ? atoi(e) : 1; }
        if (rt_ui_test_held()) s->mouse |= button;
    }
    if (s->mouse & ~s->mouse_prev) g_kbm_tick = now;
    {                                           // the wheel on a screen without a list: the next waiting notch becomes an arrow-key tap
        static uint64_t next;
        if ((g_wheel_blocked && !g_list_count) || text || now - g_cam_tick < 250) InterlockedExchange(&g_wheel, 0);     // in play / a camera has the wheel: no key taps
        else if (g_wheel && !g_list_count && now >= next) {
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
        if (down && !s->prev[k]) { s->repeat[k] = 1; s->timer[k] = g_repeat_start; g_kbm_tick = g_key_tick = now; }
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
    uint8_t vp_button;                        // VirtualPadDef: the mouse button held while dragging
    uint8_t gesture;                          // GestureDef (not acted on)
    uint16_t vp_sens;                         // its drag length for a full tilt, in pixels of a 1080-line screen
    uint32_t vp_dir;                          // Def.Input.eButton: which way of the drag this action is
} Binding;
static Binding g_default[ACTION_COUNT], g_bind[ACTION_COUNT];
static Binding g_pending;                     // defs seen since the last ActionAssignDatum constructor
static int g_loaded, g_captured;

// ActionAssignKeyboard.KeyDef(Key, bool), KeyArrayDef(Key[], bool), AnalogKeyDef(Key, int startMS, int endMS, bool)
// and ActionAssignMouse.ButtonDef(eMouseButton): called by AppInput's class constructor while it builds the default
// table, each followed by the constructor of the ActionAssignDatum they belong to.
int rt_hook_43c37b0(Cpu *c) { g_pending.nkeys = 1; g_pending.keys[0] = (uint8_t)W(0); g_pending.analog = 0; return 0; }
int rt_hook_43c3880(Cpu *c) {
    uint64_t arr = X(0);
    int n = arr ? (int)LD32(arr + ARR_LEN) : 0;
    g_pending.nkeys = 0; g_pending.analog = 0;
    for (int i = 0; i < n && i < MAX_KEYS; i++) g_pending.keys[g_pending.nkeys++] = (uint8_t)LD32(arr + ARR_DATA + 4 * (uint64_t)i);
    return 0;
}
int rt_hook_43c3950(Cpu *c) {
    g_pending.nkeys = 1; g_pending.keys[0] = (uint8_t)W(0); g_pending.analog = 1;
    g_pending.start_ms = (uint16_t)W(1); g_pending.end_ms = (uint16_t)W(2);
    return 0;
}
int rt_hook_43c30d0(Cpu *c) { g_pending.mouse = (uint8_t)W(0); return 0; }
// ActionAssignMouse.VirtualPadDef(eMouseButton, eButton analogDir, int sensitivity) and GestureDef(eButton, int)
int rt_hook_43c3150(Cpu *c) { g_pending.vp_button = (uint8_t)W(0); g_pending.vp_dir = W(1); g_pending.vp_sens = (uint16_t)W(2); return 0; }
int rt_hook_43c31e0(Cpu *c) { g_pending.gesture = 1; g_pending.vp_dir = W(0); g_pending.vp_sens = (uint16_t)W(1); return 0; }
static int datum(Cpu *c) {                    // ActionAssignDatum..ctor(Action.eID in_ActionID, ...): seven overloads
    uint32_t a = W(1);
    if (a < ACTION_COUNT) {
        // The Helper's rewind steps with the arrow keys only; players who move with WASD get D / A as well.
        if ((a == ACTION_REWIND_FORWARD || a == ACTION_REWIND_BACKWARD) && g_pending.nkeys == 1)
            g_pending.keys[g_pending.nkeys++] = (uint8_t)pc_key_by_name(a == ACTION_REWIND_FORWARD ? "D" : "A");
        // The photo mode / View Stage camera (turn up, down, left, right, zoom in, out) has no keys in the game's PC
        // table (it is the mouse's there); WASD selects in photo mode and the arrows move the camera, so turning gets
        // I J K L and zoom + and -.
        // Photo mode on PC flies with WASD (the free camera): the menu's choices (66..69, WASD in the game's table) go
        // to the arrows, and the camera's move (74..77, the arrows) to WASD, so the guide names them so.
        if (a >= ACTION_PHOTO_SELECT && a <= ACTION_PHOTO_SELECT + 3 && g_pending.nkeys == 1) {
            static const char *const sel[4] = {"RightArrow", "LeftArrow", "DownArrow", "UpArrow"};
            g_pending.keys[0] = (uint8_t)pc_key_by_name(sel[a - ACTION_PHOTO_SELECT]);
        }
        if (a >= ACTION_CAMERA_PAN_UP && a <= ACTION_CAMERA_PAN_UP + 3 && g_pending.nkeys == 1) {
            static const char *const mvk[4] = {"W", "S", "A", "D"};
            g_pending.keys[0] = (uint8_t)pc_key_by_name(mvk[a - ACTION_CAMERA_PAN_UP]);
        }
        if (a >= ACTION_CAMERA_UP && a <= ACTION_CAMERA_UP + 5 && !g_pending.nkeys) {
            static const char *const cam_keys[6] = {"I", "K", "J", "L", "Equals", "Minus"};
            g_pending.keys[0] = (uint8_t)pc_key_by_name(cam_keys[a - ACTION_CAMERA_UP]);
            g_pending.nkeys = 1;
        }
        g_default[a] = g_pending; g_captured++;
    }
    memset(&g_pending, 0, sizeof g_pending);
    return 0;
}
int rt_hook_43c3e40(Cpu *c) { return datum(c); }
int rt_hook_43c3ee0(Cpu *c) { return datum(c); }
int rt_hook_43c3f70(Cpu *c) { return datum(c); }
int rt_hook_43c40b0(Cpu *c) { return datum(c); }
int rt_hook_43c41e0(Cpu *c) { return datum(c); }
int rt_hook_43c4320(Cpu *c) { return datum(c); }
int rt_hook_43c4450(Cpu *c) { return datum(c); }

static void keys_path(char *out, size_t n) {
    const char *prof = getenv("BR_PROFILE");
    snprintf(out, n, "%sport_keys%s.txt", rt_user_dir(), prof ? prof : "");
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
int pc_key_pressed(int key) { return key > 0 && key < NKEY && g_frame.cur[key] && !g_frame.prev[key]; }   // went down this frame
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

// ------------------------------------------------------------------------------------------------ mouse virtual pad
// The game's PC layout binds the tilt and camera actions to the mouse as a "virtual pad"
// (AppActionMouse.ActionParamVirtualPad, whose component does not run in this build): while the binding's mouse
// button is held, the place where it went down is the stick's centre and the drag away from it is the stick.
// As in the game: the drag only counts after a small distance, and the centre is then moved by that distance so
// the value starts at zero; the action's value is the part of the drag in its direction, full at the binding's
// sensitivity in pixels (scaled to a 1080-line window), stretched or shortened by the Mouse Sensitivity option.
static float g_sens_ratio = 1.0f;             // pc_mouse_sensitivity
extern int rt_ui_mouse(float *x, float *y, int *down);      // online_ui.c: cursor in window pixels from the bottom left
extern int rt_ui_test_held(void);
#define VPAD_MIN 10.0f
typedef struct { int state; float cx, cy, dx, dy; } VPad;   // state: 0 waiting, 1 finding the centre, 2 giving values
static VPad g_vpad[5];
static void vpad_update(void) {
    float x, y, scale = 1.0f;
    int down, inside = rt_ui_mouse(&x, &y, &down);
    RECT cr;
    HWND fg = GetForegroundWindow();
    if (fg && GetClientRect(fg, &cr) && cr.bottom > 0) scale = 1080.0f / (float)cr.bottom;
    for (int i = 0; i < 5; i++) {
        VPad *p = &g_vpad[i];
        if (!(g_frame.mouse >> i & 1) || (!inside && !p->state)) { p->state = 0; p->dx = p->dy = 0; continue; }
        if (!p->state) { p->cx = x; p->cy = y; p->state = 1; p->dx = p->dy = 0; }
        float ddx = (x - p->cx) * scale, ddy = (y - p->cy) * scale, len = sqrtf(ddx * ddx + ddy * ddy);
        if (p->state == 1 && len >= VPAD_MIN) {
            p->cx += ddx / len * VPAD_MIN / scale; p->cy += ddy / len * VPAD_MIN / scale;
            ddx = (x - p->cx) * scale; ddy = (y - p->cy) * scale;
            p->state = 2;
        }
        if (p->state == 2) { p->dx = ddx; p->dy = ddy; }
    }
}
static float vpad_value(const Binding *b) {
    if (!b->vp_button || b->gesture) return 0;
    int i = __builtin_ctz(b->vp_button);
    if (i > 4 || g_vpad[i].state != 2) return 0;
    float dx = g_vpad[i].dx, dy = g_vpad[i].dy, len = sqrtf(dx * dx + dy * dy);
    if (len < 1e-3f) return 0;
    uint32_t d = b->vp_dir;
    float part = d == 0x100 || d == 0x1000000 ? dy : d == 0x200 || d == 0x2000000 ? -dy : d == 0x400 || d == 0x4000000 ? -dx :
                 d == 0x800 || d == 0x8000000 ? dx : 0;
    float v = part / ((b->vp_sens ? (float)b->vp_sens : 100.0f) * g_sens_ratio);
    return v <= 0 ? 0 : v > 1 ? 1 : v;
}
// Options > Controls > Mouse Sensitivity (the port's row, display_opt.c). The game's own arithmetic
// (AppMouse.SetSensitivityReviseParamByInt, ActionAssignMouse.CalcRevisedSensitivity, neither called in this
// build): 5 is the binding's drag length as it is; 1 doubles it in four steps (less sensitive); 6..10 shorten it
// to a tenth in five steps. BR_MOUSE_SENS=<n> forces a value.
void pc_mouse_sensitivity(int n) {
    static int last = -1;
    const char *force = getenv("BR_MOUSE_SENS");
    if (force && *force) n = atoi(force);
    if (n == last) return;
    last = n;
    float p = n < 5 ? (float)(5 - n) * 0.25f : n > 5 ? -(float)(n - 5) / 5.0f : 0;
    if (p > 1) p = 1;
    if (p < -1) p = -1;
    g_sens_ratio = p > 0 ? 1.0f + p : p < 0 ? 0.1f + (p + 1.0f) * 0.9f : 1.0f;
    rt_log("mouse: sensitivity option %d, drag length x%.2f", n, (double)g_sens_ratio);
}

// ------------------------------------------------------------------------------------------------ mouse camera
// Photo mode (MgPhotoModeCamera) and View Stage (MgViewStageCamera) have the game's own PC mouse control: a left
// drag turns the camera, a middle drag moves it, the wheel zooms. They read the mouse through State.GetMouseButton
// / GetMouseButtonDown / mousePosition / mouseScrollDelta, whose device functions the Switch build never sets, so
// they always read "no mouse". The port answers them for player 1. Positions are in pixels of a 1080-line window
// stretched by the Mouse Sensitivity option, so a drag turns as far at any window size. While one of these cameras
// reads the wheel, its notches are not played as arrow-key taps.
// View Stage: a right drag moves the camera as well (it stands for the middle button), as in the port's stage editor.
// Photo mode has the free camera below instead: the game's left drag turns; moving and the wheel are the port's.
extern int rt_ui_client_h(void);              // online_ui.c
static int enabled(void);
static uint64_t g_photo_tick;                 // photo mode's camera ran (its updateTRS) at this time
static int photo_active(void) { return g_photo_tick && GetTickCount64() - g_photo_tick < 250; }
static uint64_t g_photo_opening;
int pc_photo_active(void) { return photo_active() || (g_photo_opening && GetTickCount64() - g_photo_opening < 3000); }
uint64_t pc_photo_opening(void) { return g_photo_opening; }
// While photo mode runs, the free camera's keys (W A S D Q E R) belong to it alone: the game's menus read WASD as
// directions (the photo menu would change its choices), so they reach no action of the game.
static int photo_fly_key(int k) {
    static int keys[7];
    if (!keys[0]) { static const char *const n[7] = {"W", "A", "S", "D", "Q", "E", "R"}; for (int i = 0; i < 7; i++) keys[i] = pc_key_by_name(n[i]); }
    if (!photo_active()) return 0;
    for (int i = 0; i < 7; i++) if (keys[i] == k) return 1;
    return 0;
}
static int shift_held(void) {
    static int ls, rs;
    if (!ls) { ls = pc_key_by_name("LeftShift"); rs = pc_key_by_name("RightShift"); }
    return g_frame.cur[ls] || g_frame.cur[rs];
}
static int cam_state_ok(uint64_t state) { return enabled() && state && LD32(state + STATE_ID) == 0 && !rt_text_entry_active(); }
static int cam_buttons(int mask, int which) {  // which: 0 held, 1 went down this frame
    int cur = g_frame.mouse, prev = g_frame.mouse_prev;
    if (photo_active()) {                     // the free camera does it all (photo_orbit, photo_free)
        cur = prev = 0;
    } else {
        if (cur & 2) cur |= 4;                // right = middle
        if (prev & 2) prev |= 4;
    }
    return which ? (cur & ~prev & mask) != 0 : (cur & mask) != 0;
}
int rt_hook_43c9f30(Cpu *c) { c->x[0] = cam_state_ok(X(0)) && cam_buttons((int)W(1), 0); return 1; }    // GetMouseButton
int rt_hook_43ca110(Cpu *c) { c->x[0] = cam_state_ok(X(0)) && cam_buttons((int)W(1), 1); return 1; }    // GetMouseButtonDown
int rt_hook_43ca330(Cpu *c) {                                                                            // mousePosition
    float x, y, k = 1.0f;
    int down, h = rt_ui_client_h();
    rt_ui_mouse(&x, &y, &down);
    if (h > 0) k = 1080.0f / (float)h / g_sens_ratio;
    rt_setf(c, 0, cam_state_ok(X(0)) ? x * k : 0); rt_setf(c, 1, cam_state_ok(X(0)) ? y * k : 0);
    return 1;
}
int rt_hook_43ca4d0(Cpu *c) {                                                                            // mouseScrollDelta
    uint64_t now = GetTickCount64();
    if (now - g_cam_tick > 250) InterlockedExchange(&g_wheel_cam, 0);      // notches from before the camera was on
    g_cam_tick = now;
    LONG n = cam_state_ok(X(0)) && !photo_active() ? InterlockedExchange(&g_wheel_cam, 0) : 0;   // photo mode: the free camera's
    rt_setf(c, 0, 0); rt_setf(c, 1, (float)n);
    return 1;
}

// ------------------------------------------------------------------------------------------------ free photo camera
// Photo mode on PC moves like the port's stage editor and a 3D editor's fly camera. The game's camera
// (MgPhotoModeCamera.updateTRS) puts its point of interest at the player plus a pan offset, the camera at that point
// plus yaw / pitch / distance, both clamped to a small range; then it pushes the camera out of the stage
// (collideStage) and smooths. The port runs updateTRS and then moves both points by a free offset of its own and
// scales the distance by a free zoom; the controller keeps the game's controls on top. Keyboard and mouse (player 1):
//   W A S D   fly forward / left / back / right (along the view)     Q / E   down / up      Shift   three times faster
//   left drag turns (the game's)    right, middle or Shift + left drag moves the view    wheel zooms    R   back to start
// The menu's choices are on the arrow keys (actions 66..69); WASD is the camera's (74..77, its guide says
// "Move Camera"), and while photo mode runs the keyboard does not feed the game's own clamped pan. Once moved
// freely, the camera is not pushed out of the stage (it may go anywhere, as in an editor).
#define F_PHOTO_UPDATE_TRS 0x428de60ull     // MgPhotoModeCamera.updateTRS()
#define PH_TARGET_INTR 0x274                // Vector3 m_TargetIntr (point looked at), m_TargetPos at 0x28c
#define PH_TARGET_POS 0x28c
static float g_ph_off[3], g_ph_zoom = 1.0f, g_ph_last_x, g_ph_last_y;
static int g_ph_dragging, g_ph_free;
static uint64_t g_ph_hold;                    // until then the game's distance is kept (the view glides back after R)
static double g_ph_time;
static double now_s(void) {
    static LARGE_INTEGER f;
    LARGE_INTEGER t;
    if (!f.QuadPart) QueryPerformanceFrequency(&f);
    QueryPerformanceCounter(&t);
    return (double)t.QuadPart / (double)f.QuadPart;
}
// Left drag: orbit (as the port's stage editor), done here on the game's own angles (m_rotX pitch, m_rotY yaw; the
// game re-reads them from the camera afterwards, as it does for its stick). Only a drag that starts above the photo
// menu turns: clicks on the menu are for the menu.
#define PH_ROT_X 0x2d8
#define PH_ROT_Y 0x2dc
#define PH_ROT_X_MIN 0x2f0
#define PH_ROT_X_MAX 0x2f4
#define PH_MENU_TOP 350.0f                  // the photo menu's panel ends this many 1080-line pixels above the bottom
static int g_ph_orbit;
static float g_ph_orbit_x, g_ph_orbit_y;
static void photo_orbit(uint64_t self) {
    float x, y;
    int down, h = rt_ui_client_h(), m = g_frame.mouse, prev = g_frame.mouse_prev;
    int inside = rt_ui_mouse(&x, &y, &down);
    float k = h > 0 ? 1080.0f / (float)h : 1.0f;
    int left = (m & 1) && !shift_held();
    if (!left || !inside) { g_ph_orbit = 0; return; }
    if (!g_ph_orbit) {                                               // a new press: above the menu only
        if (!(prev & 1) && y * k > PH_MENU_TOP) { g_ph_orbit = 1; g_ph_orbit_x = x; g_ph_orbit_y = y; }
        return;
    }
    float dx = (x - g_ph_orbit_x) * k / g_sens_ratio, dy = (y - g_ph_orbit_y) * k / g_sens_ratio;
    g_ph_orbit_x = x; g_ph_orbit_y = y;
    float rx = ldf(self + PH_ROT_X) - dy * 0.3f, ry = ldf(self + PH_ROT_Y) + dx * 0.3f;
    float lo = ldf(self + PH_ROT_X_MIN), hi = ldf(self + PH_ROT_X_MAX);
    if (lo < hi) { if (rx < lo) rx = lo; if (rx > hi) rx = hi; }
    stf(self + PH_ROT_X, rx); stf(self + PH_ROT_Y, ry);
}
// Clicks on the photo menu: its four pickers (Frame, Effect, Expression, Pose; ui_PartView_Template_Vertical_Picker_N
// with a value box, an increase arrow above it and a decrease arrow below) take the click as the arrow keys would:
// Left / Right to the clicked picker (the focused one has an active ImageBackGround_Line_Sel), then Up on the upper
// arrow, Down on the lower one. The taps are queued (photo_taps) and played one by one.
#define F_GO_FIND_ 0x3f0c5a0ull             // GameObject.Find(string) (static)
#define F_GO_TRANSFORM 0x3f0bb40ull         // GameObject.transform
#define F_STRING_NEW_UTF16_ 0x45af94ull
#define F_CHILD_COUNT_ 0x3f1e1f0ull
#define F_GET_CHILD_ 0x3f1ea10ull
#define F_GO_ACTIVE_IN_HIER_ 0x3f0bdd0ull
#define F_GET_GAMEOBJECT_ 0x3f07fb0ull      // Component.gameObject
#define F_OBJ_NAME_ 0x3f10820ull            // Object.name
extern void rt_ui_point(int hover, float *x, float *y);
extern int rt_ui_click_pending(void);
extern void rt_ui_click_consume(void);
static int g_ph_taps[16], g_ph_ntaps;
static uint64_t ph_str(Cpu *c, const char *s) {
    uint16_t w[64];
    int n = 0;
    for (; s[n] && n < 63; n++) w[n] = (uint8_t)s[n];
    return rt_call(c, FN(F_STRING_NEW_UTF16_), 2, (uint64_t)(uintptr_t)w, (uint64_t)n);
}
static uint64_t ph_child(Cpu *c, uint64_t t, const char *name, int depth) {     // active transform named `name` under t
    int n = t ? (int)rt_call(c, FN(F_CHILD_COUNT_), 2, t, 0ull) : 0;
    for (int i = 0; i < n; i++) {
        uint64_t ch = rt_call(c, FN(F_GET_CHILD_), 3, t, (uint64_t)i, 0ull);
        uint64_t go = rt_call(c, FN(F_GET_GAMEOBJECT_), 2, ch, 0ull);
        if (!(rt_call(c, FN(F_GO_ACTIVE_IN_HIER_), 2, go, 0ull) & 1)) continue;
        uint64_t s = rt_call(c, FN(F_OBJ_NAME_), 2, go, 0ull);
        int len = s ? (int)LD32(s + 0x10) : 0, k = 0;
        for (; k < len && name[k] && LD16(s + 0x14 + 2 * (uint64_t)k) == (uint8_t)name[k]; k++) {}
        if (k == len && !name[k]) return ch;
        uint64_t hit = depth > 0 ? ph_child(c, ch, name, depth - 1) : 0;
        if (hit) return hit;
    }
    return 0;
}
static int ph_in(Cpu *c, uint64_t view, float x, float y, float pad) {
    float r[4];
    return view && rt_ui_rect(c, view, r) && x >= r[0] - pad && x <= r[0] + r[2] + pad && y >= r[1] - pad && y <= r[1] + r[3] + pad;
}
static int photo_click(Cpu *c) {
    if (!rt_ui_click_pending()) return 0;
    float x, y;
    rt_ui_point(0, &x, &y);
    uint64_t go = rt_call(c, FN(F_GO_FIND_), 2, ph_str(c, "ui_View_PhotoModeMenu"), 0ull);
    uint64_t root = go ? rt_call(c, FN(F_GO_TRANSFORM), 2, go, 0ull) : 0;
    if (getenv("BR_PHOTO_LOG")) rt_log("photo cam: click at %.0f %.0f, menu %s", x, y, root ? "found" : "not found");
    if (!root) return 0;
    int cur = -1, hit = -1, arrow = 0;
    for (int i = 0; i < 4; i++) {
        char nm[48];
        snprintf(nm, sizeof nm, "ui_PartView_Template_Vertical_Picker_%d", i);
        uint64_t p = ph_child(c, root, nm, 4);
        if (!p) continue;
        uint64_t bg = ph_child(c, p, "ImageBackGround", 0), inc = ph_child(c, p, "ui_PartView_PickerIncreaseArrow", 0),
                 dec = ph_child(c, p, "ui_PartView_PickerDecreaseArrow", 0);
        if (bg && ph_child(c, bg, "ImageBackGround_Line_Sel", 0)) cur = i;
        if (ph_in(c, inc, x, y, 22)) { hit = i; arrow = 1; }
        else if (ph_in(c, dec, x, y, 22)) { hit = i; arrow = -1; }
        else if (ph_in(c, bg, x, y, 4)) hit = i;
    }
    if (hit < 0) return 0;
    rt_ui_click_consume();
    if (cur < 0) cur = hit;
    int left = pc_key_by_name("LeftArrow"), right = pc_key_by_name("RightArrow");
    g_ph_ntaps = 0;
    for (int i = cur; i != hit && g_ph_ntaps < 8; i += hit > cur ? 1 : -1) g_ph_taps[g_ph_ntaps++] = hit > cur ? right : left;
    if (arrow) g_ph_taps[g_ph_ntaps++] = pc_key_by_name(arrow > 0 ? "UpArrow" : "DownArrow");
    if (getenv("BR_PHOTO_LOG")) rt_log("photo cam: click on picker %d (focus %d), arrow %d, %d taps", hit, cur, arrow, g_ph_ntaps);
    return 1;
}
// Plays the queued taps, one every PH_TAP_MS (called from sample()).
#define PH_TAP_MS 90
static void photo_taps(uint64_t now) {
    static uint64_t next;
    if (!g_ph_ntaps || now < next) return;
    g_test_until[g_ph_taps[0]] = now + PH_TAP_MS / 2;
    memmove(g_ph_taps, g_ph_taps + 1, sizeof(int) * (size_t)--g_ph_ntaps);
    next = now + PH_TAP_MS;
}
static int key_held(const char *name) { int k = pc_key_by_name(name); return k > 0 && k < NKEY && g_frame.cur[k]; }
static void photo_free(Cpu *c, uint64_t self) {
    double t = now_s();
    float dt = (float)(t - g_ph_time);
    if (dt > 0.5f || dt < 0) {                                       // photo mode (re)opened: start from the game's view
        g_ph_off[0] = g_ph_off[1] = g_ph_off[2] = 0; g_ph_zoom = 1.0f; g_ph_dragging = 0; g_ph_free = 0;
        InterlockedExchange(&g_wheel_cam, 0);
        dt = 0;
    }
    if (dt > 0.1f) dt = 0.1f;
    g_ph_time = t;
    g_photo_tick = g_cam_tick = GetTickCount64();
    float ti[3], tp[3], d[3];
    for (int i = 0; i < 3; i++) { ti[i] = ldf(self + PH_TARGET_INTR + 4 * i); tp[i] = ldf(self + PH_TARGET_POS + 4 * i); d[i] = tp[i] - ti[i]; }
    float len = sqrtf(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]);
    if (getenv("BR_PHOTO_LOG")) {
        static double said;
        if (t - said > 0.5) { said = t; rt_log("photo cam: game distance %.2f, zoom %.2f, offset %.2f %.2f %.2f", len, g_ph_zoom, g_ph_off[0], g_ph_off[1], g_ph_off[2]); }
    }
    if (!(len > 1e-4f)) return;
    float fw[3] = {-d[0] / len, -d[1] / len, -d[2] / len};          // camera -> point looked at
    float rt[3] = {fw[2], 0, -fw[0]};                                // Unity: right = Cross(up, forward)
    float rl = sqrtf(rt[0] * rt[0] + rt[2] * rt[2]);
    if (rl < 1e-4f) { rt[0] = 1; rt[2] = 0; } else { rt[0] /= rl; rt[2] /= rl; }
    float up[3] = {fw[1] * rt[2] - fw[2] * rt[1], fw[2] * rt[0] - fw[0] * rt[2], fw[0] * rt[1] - fw[1] * rt[0]};   // Cross(forward, right)
    float eff = len * g_ph_zoom;
    int ok = enabled() && !rt_text_entry_active();
    if (ok) {
        float sp = (eff > 1.5f ? eff : 1.5f) * 1.2f * (shift_held() ? 3.0f : 1.0f) * dt, mv[3] = {0, 0, 0};
        float f = (float)(key_held("W") - key_held("S")), s = (float)(key_held("D") - key_held("A")), v = (float)(key_held("E") - key_held("Q"));
        for (int i = 0; i < 3; i++) mv[i] = (fw[i] * f + rt[i] * s) * sp;
        mv[1] += v * sp;
        // a drag with the right or middle button, or Shift + left, moves the view (the scene follows the cursor)
        float x, y;
        int down, h = rt_ui_client_h(), m = g_frame.mouse;
        int inside = rt_ui_mouse(&x, &y, &down), pan = (m & 6) || ((m & 1) && shift_held());
        if (pan && inside) {
            if (g_ph_dragging) {
                float k = eff * 0.0016f * (h > 0 ? 1080.0f / (float)h : 1.0f) / g_sens_ratio, dx = x - g_ph_last_x, dy = y - g_ph_last_y;
                for (int i = 0; i < 3; i++) mv[i] += (-rt[i] * dx - up[i] * dy) * k;
            }
            g_ph_dragging = 1; g_ph_last_x = x; g_ph_last_y = y;
        } else g_ph_dragging = 0;
        for (int i = 0; i < 3; i++) g_ph_off[i] += mv[i];
        LONG n = InterlockedExchange(&g_wheel_cam, 0);                // wheel: zoom toward the point looked at
        if (n) {
            g_ph_zoom *= expf(-0.15f * (float)n);
            if (len * g_ph_zoom < 0.3f) g_ph_zoom = 0.3f / len;
            if (len * g_ph_zoom > 400.0f) g_ph_zoom = 400.0f / len;
        }
        if (pc_key_pressed(pc_key_by_name("R"))) { g_ph_off[0] = g_ph_off[1] = g_ph_off[2] = 0; if (g_ph_zoom != 1.0f) g_ph_hold = GetTickCount64() + 1500; g_ph_zoom = 1.0f; }
    }
    g_ph_free = g_ph_off[0] != 0 || g_ph_off[1] != 0 || g_ph_off[2] != 0 || g_ph_zoom != 1.0f;
    for (int i = 0; i < 3; i++) {
        stf(self + PH_TARGET_INTR + 4 * i, ti[i] + g_ph_off[i]);
        stf(self + PH_TARGET_POS + 4 * i, ti[i] + d[i] * g_ph_zoom + g_ph_off[i]);
    }
    (void)c;
}
int rt_hook_428de60(Cpu *c) {                                    // MgPhotoModeCamera.updateTRS
    static __thread int inside;
    if (inside || !enabled()) return 0;
    uint64_t self = X(0);
    if (!rt_text_entry_active()) { photo_orbit(self); photo_click(c); }
    inside = 1;
    rt_call(c, FN(F_PHOTO_UPDATE_TRS), 1, self);
    inside = 0;
    if (!c->eh_unwinding) photo_free(c, self);
    return 1;
}
// MgPhotoModeCamera.updateRotAndDist() reads the distance back from where the camera now is, which includes the
// free zoom; while that zoom is on, the game's own distance is kept as it was, or the zoom would feed into it.
#define F_PHOTO_ROT_AND_DIST 0x428e3b0ull
#define PH_DIST 0x2cc
int rt_hook_428e3b0(Cpu *c) {
    static __thread int inside;
    if (inside || !enabled()) return 0;
    uint64_t self = X(0);
    float dist = ldf(self + PH_DIST);
    inside = 1;
    rt_call(c, FN(F_PHOTO_ROT_AND_DIST), 1, self);
    inside = 0;
    if (!c->eh_unwinding && (g_ph_zoom != 1.0f || GetTickCount64() < g_ph_hold) && photo_active()) stf(self + PH_DIST, dist);   // the game's own distance stays
    return 1;
}
// MgPhotoModeController.Begin: photo mode is opening (its guide is drawn before its camera runs).
int rt_hook_428f750(Cpu *c) { (void)c; g_photo_opening = GetTickCount64(); if (getenv("BR_PHOTO_LOG")) rt_log("photo cam: photo mode begins"); return 0; }
// MgPhotoModeCamera.collideStage(Vector3 targetPos) -> Vector3: a freely placed camera is left where it is.
int rt_hook_428e050(Cpu *c) { return enabled() && g_ph_free && photo_active(); }

// ------------------------------------------------------------------------------------------------ chat wheel
// The chat wheel (UIChoicesGroupModule_SelectCommAction, in play and in the wheel editor of Options / the lobby)
// picks its slice from the direction of a stick: four analog actions (its virtual getters, slots 7..10: up, down,
// left, right), read by calcForChoosingItem, need a length of 0.4. With the mouse, the direction is the cursor's
// from the wheel's centre: the slice the cursor points at is chosen, near the centre none. This starts once the
// mouse moved while the wheel is open and ends when a key is pressed (keys and pad keep working as before).
// A left click sends the chosen slice: in play the wheel sends when its button (action 96) is let go, so a click
// makes that action read as released once; in the editor the click is the wheel's Decide.
#define F_WHEEL_CALC 0x4662a30ull           // UIChoicesGroupModule_SelectCommAction.calcForChoosingItem(State, float)
#define F_FOCUS_INDEX 0x45a85a0ull          // UIFocusOfChoicesCtrl_OneDimension.get_focusIndex
#define WM_VIEW 0x20                        // module: m_SelectCommunicationActionView, m_FocusOnChoicesController
#define WM_FOCUS 0x28
#define WM_DETECTOR 0x70                    // ForCustomize: m_InputDetector
#define WV_RECT 0x48                        // UIViewSelectCommAction.m_RectTrs
#define ACTION_WHEEL_HOLD 96
#define ACTION_EDITOR_UP 0x93               // the editor's module reads its own four actions
static __thread uint64_t t_wheel;            // the module whose calcForChoosingItem is running
static uint32_t g_wheel_act[4];              // its four actions
static float g_wheel_aim[4];                 // what the cursor gives each of them
static uint64_t g_wheel_mod, g_wheel_open_tick, g_wheel_editor_det;
static uint32_t g_wheel_frame;
static int g_wheel_send;                     // a click is waiting to send the slice (in play)
static int enabled(void);
extern int rt_ui_click_pending(void);
static int wheel_log(void) { static int l = -1; if (l < 0) l = getenv("BR_WHEEL_LOG") != NULL; return l; }
static uint32_t wheel_action(Cpu *c, uint64_t m, int slot) {
    uint64_t k = LD64(m), off = 0x138 + 0x10 * (uint64_t)slot;
    return (uint32_t)rt_call(c, LD64(k + off), 2, m, LD64(k + off + 8));
}
int rt_hook_4662a30(Cpu *c) {
    static __thread int inside;
    if (inside || !enabled()) return 0;
    uint64_t m = X(0), state = X(1), now = GetTickCount64();
    float dt = rt_getf(c, 0);
    if (m != g_wheel_mod || g_list_frame - g_wheel_frame > 2) {      // the wheel opened
        g_wheel_open_tick = now; g_wheel_send = 0;
        if (wheel_log()) rt_log("wheel: open, actions %u %u %u %u", wheel_action(c, m, 7), wheel_action(c, m, 8), wheel_action(c, m, 9), wheel_action(c, m, 10));
    }
    g_wheel_mod = m; g_wheel_frame = g_list_frame;
    for (int i = 0; i < 4; i++) { g_wheel_act[i] = wheel_action(c, m, 7 + i); g_wheel_aim[i] = 0; }
    int editor = g_wheel_act[0] == ACTION_EDITOR_UP;
    g_wheel_editor_det = editor ? LD64(m + WM_DETECTOR) : 0;
    float x, y, r[4];
    int down, inside_win = rt_ui_mouse(&x, &y, &down);
    uint64_t view = LD64(m + WM_VIEW), rect = view ? LD64(view + WV_RECT) : 0;
    int aim = g_mouse_tick > g_wheel_open_tick && g_mouse_tick >= g_key_tick && inside_win && rect && rt_ui_rect(c, rect, r);
    if (aim) {
        float dx = x - (r[0] + r[2] / 2), dy = y - (r[1] + r[3] / 2), len = sqrtf(dx * dx + dy * dy);
        float dead = 0.12f * (r[2] < r[3] ? r[2] : r[3]);
        if (dead < 12) dead = 12;
        if (len >= dead) {
            g_wheel_aim[0] = dy > 0 ? dy / len : 0; g_wheel_aim[1] = dy < 0 ? -dy / len : 0;
            g_wheel_aim[2] = dx < 0 ? -dx / len : 0; g_wheel_aim[3] = dx > 0 ? dx / len : 0;
        }
        if (wheel_log() && rt_ui_mouse_moved())
            rt_log("wheel: cursor %.0f %.0f, wheel %.0f %.0f %.0f %.0f, aim u%.2f d%.2f l%.2f r%.2f", x, y, r[0], r[1], r[2], r[3],
                   g_wheel_aim[0], g_wheel_aim[1], g_wheel_aim[2], g_wheel_aim[3]);
    }
    t_wheel = m;
    inside = 1;
    c->x[0] = m; c->x[1] = state; rt_setf(c, 0, dt);
    rt_call_regs(c, FN(F_WHEEL_CALC));
    inside = 0;
    t_wheel = 0;
    if (!editor && rt_ui_click_pending() && inside_win) {             // a click sends the slice now chosen
        rt_ui_click_consume();
        g_wheel_send = 1;
        if (wheel_log()) rt_log("wheel: click, focus %d", (int)rt_call(c, FN(F_FOCUS_INDEX), 2, LD64(m + WM_FOCUS), 0ull));
    }
    return 1;
}
static float wheel_value(uint64_t state, uint32_t action) {
    if (!t_wheel || LD32(state + STATE_ID) != 0) return 0;
    for (int i = 0; i < 4; i++)
        if (g_wheel_act[i] == action) return g_wheel_aim[i];
    return 0;
}
// The editor's detector, polled now: a click while a slice is chosen is Decide (ui_mouse).
static int wheel_editor_click(Cpu *c, uint64_t det) {
    if (!det || det != g_wheel_editor_det || g_list_frame - g_wheel_frame > 2 || !rt_ui_click_pending()) return 0;
    if ((int)rt_call(c, FN(F_FOCUS_INDEX), 2, LD64(g_wheel_mod + WM_FOCUS), 0ull) < 0) return 0;
    rt_ui_click_consume();
    if (wheel_log()) rt_log("wheel: click in the editor, Decide");
    return 1;
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
        if (k <= 0 || k >= NKEY || photo_fly_key(k)) continue;
        if (kind == 0 ? s->cur[k] : kind == 1 ? (s->cur[k] && !s->prev[k]) : kind == 2 ? (!s->cur[k] && s->prev[k]) : s->repeat[k]) return 1;
    }
    if (b->mouse) {
        // Photo mode: right / middle drag and Shift + left drag move the free camera; they are not the game's buttons
        // (right is Cancel in the game's table and would leave photo mode).
        int own = photo_active() ? 6 | (shift_held() ? 1 : 0) : 0;
        int cur = s->mouse & b->mouse & ~own, prev = s->mouse_prev & b->mouse & ~own;
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
int rt_hook_43c8530(Cpu *c) {
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
        if (action >= ACTION_CAMERA_PAN_UP && action <= ACTION_CAMERA_PAN_UP + 3 && photo_active()) which = -1;   // WASD flies (free photo camera)
        if (which >= 0 && b && (b->nkeys || b->mouse || action == ACTION_TITLE_DECIDE || action == ACTION_MOVIE_SKIP) && state_takes_input(c, state, action))
            r = (uint64_t)binding_active(b, which < 4 ? &g_frame : &g_fixed, which & 3, action);
        if (!r && which == 1 && action == g_click_action && g_list_frame == g_click_frame && state_takes_input(c, state, action))
            r = 1;                                                   // a click on a header button (ui_mouse)
    }
    // A click sent the slice: the wheel's button reads as let go until it really is, so a button still held after
    // the click does not open the wheel again.
    if (action == ACTION_WHEEL_HOLD && g_wheel_send && LD32(state + STATE_ID) == 0 && func &&
        (func == LD64(state + STATE_FUNCS) || func == LD64(state + STATE_FUNCS + 32))) {
        if (!r) g_wheel_send = 0;
        r = 0;
    }
    c->x[0] = r;
    return 1;
}
// AppInput.State.ActionAnalogValue(Action.eID): 0..1. A held key counts fully, an analog key by its ramp.
int rt_hook_43c8d50(Cpu *c) {
    static __thread int inside;
    if (inside || !enabled()) return 0;
    uint64_t state = X(0);
    uint32_t action = W(1);
    inside = 1;
    rt_call(c, FN(F_ACTION_ANALOG), 2, state, (uint64_t)action);
    inside = 0;
    float v = rt_getf(c, 0);
    float wv = wheel_value(state, action);
    if (wv > v) v = wv;
    const Binding *b = action < ACTION_COUNT ? pc_binding((int)action) : NULL;
    if (action >= ACTION_CAMERA_PAN_UP && action <= ACTION_CAMERA_PAN_UP + 3 && photo_active()) b = NULL;   // WASD flies (free photo camera)
    if (b && (b->nkeys || b->vp_button) && v < 1.0f && state_takes_input(c, state, action)) {
        float mv = vpad_value(b);
        if (mv > v) v = mv;
        for (int i = 0; i < b->nkeys; i++) {
            int k = b->keys[i];
            float kv = k > 0 && k < NKEY && !photo_fly_key(k) ? (b->analog ? g_analog[k] : (float)g_frame.cur[k]) : 0;
            if (kv > v) v = kv;
        }
    }
    rt_setf(c, 0, v);
    return 1;
}
// AppInput.Update() / FixedUpdate(): sample our devices where the game samples the pads.
// {+0x3c float repeat start time, +0x40 float repeat interval}
int rt_hook_43b5380(Cpu *c) {
    if (!enabled()) return 0;
    uint64_t self = X(0);
    float a, b;
    memcpy(&a, (void *)(uintptr_t)(self + 0x3c), 4); memcpy(&b, (void *)(uintptr_t)(self + 0x40), 4);
    if (a > 0.05f && a < 2 && b > 0.01f && b < 2) { g_repeat_start = a; g_repeat_interval = b; }
    sample(&g_frame);
    vpad_update();
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
int rt_hook_43b5b60(Cpu *c) {
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
#define F_MONITOR_INPUT 0x457ac20ull        // UIUserInputDetector.MonitorInputOnUpdate()
#define F_CHECK_FADE 0x457acf0ull           // UIUserInputDetector.ckeckFadeState(): 1 = no fade in the way
#define F_MOUSE_ON_CLICK 0x4579db0ull       // UIButtonMouseAction.OnClick(UIPartViewButtonBase)
#define F_MOUSE_ON_ENTER 0x4579de0ull       // UIButtonMouseAction.OnPointerEnter(UIPartViewButtonBase)
#define F_PLAY_ANIMATION 0x451cee0ull       // UIPartViewButtonBase.PlayAnimation(eAnimationKind, bool isDisable, float normalizedTime)
#define F_GCHANDLE_NEW 0x469da0ull
#define F_GCHANDLE_TARGET 0x469df0ull
#define F_GCHANDLE_FREE 0x469e10ull
#define F_OBJ_ALIVE 0x3f10610ull            // UnityEngine.Object::op_Implicit (static)
#define F_OBJ_NAME 0x3f10820ull
#define F_GET_GAMEOBJECT 0x3f07fb0ull
#define DET_USER_INPUT 0x38                 // UIUserInputDetector: m_UserInput, targetLayer, standing Back button
#define DET_LAYER 0x28
#define DET_RETURN_BUTTON 0x50
#define VIEW_DISABLED 0x28                  // UIPartViewButtonBase: isDisable, buttonMouseAction
#define VIEW_MOUSE_ACTION 0x30
#define MA_DETECTOR 0x18                    // UIButtonMouseAction.m_InputActionDetector
#define MA_SELECTABLE 0x20
#define INPUT_DECIDE 1                      // eUserInput
#define INPUT_CANCEL 2
#define INPUT_PAUSE 17
#define ACTION_CANCEL 2
#define ACTION_PAUSE 3
#define MAX_BUTTONS 768
extern int rt_ui_click_pending(void);       // online_ui.c
extern void rt_ui_click_consume(void);
extern int rt_ui_mouse_moved(void);
extern float rt_ui_hit(Cpu *c, uint64_t view, int hover);
extern void rt_ui_point(int hover, float *x, float *y);
extern void rt_ui_press_anim(Cpu *c, uint64_t view);
extern uint64_t name_ui_after_monitor(Cpu *c, uint64_t det, uint64_t r);
static struct { uint64_t view; uint32_t handle; } g_buttons[MAX_BUTTONS];
static uint64_t g_hover_view;

// UIPartViewButtonBase.SetMouseAction(UIButtonMouseAction): remember the button.
int rt_hook_451d2a0(Cpu *c) {
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
static int list_rows_polled(Cpu *c, uint64_t content) {
    int seen = 0;
    for (int i = 0; i < MAX_BUTTONS; i++) {
        uint64_t v = g_buttons[i].view, ma = v ? LD64(v + VIEW_MOUSE_ACTION) : 0;
        if (!ma || !(rt_call(c, FN(F_OBJ_ALIVE), 2, v, 0ull) & 1)) continue;
        if (!(rt_call(c, FN(F_T_IS_CHILD_OF), 3, rt_call(c, FN(F_GET_TRANSFORM_), 2, v, 0ull), content, 0ull) & 1)) continue;
        if (polled_now(LD64(ma + MA_DETECTOR))) return 1;
        seen = 1;
    }
    return seen ? 0 : -1;
}
// How far the point is from the nearest shown child of the button (its label or picture), in pixels. Buttons that
// share one rectangle - the pause menu's tabs "Game Menu" and "Help Menu" both span the whole header strip - are
// told apart by what is drawn of them.
#define F_T_CHILD_COUNT 0x3f1e1f0ull        // Transform.get_childCount
#define F_T_GET_CHILD 0x3f1ea10ull          // Transform.GetChild(int)
extern int rt_ui_rect(Cpu *c, uint64_t view, float r[4]);
static float child_distance(Cpu *c, uint64_t v, float x, float y) {
    uint64_t t = rt_call(c, FN(F_GET_TRANSFORM_), 2, v, 0ull);
    int n = t ? (int)rt_call(c, FN(F_T_CHILD_COUNT), 2, t, 0ull) : 0;
    float best = 1e30f;
    for (int i = 0; i < n && i < 16; i++) {
        float r[4];
        uint64_t ch = rt_call(c, FN(F_T_GET_CHILD), 3, t, (uint64_t)i, 0ull);
        if (!ch || !rt_ui_rect(c, ch, r) || r[2] <= 0 || r[3] <= 0) continue;
        float dx = x < r[0] ? r[0] - x : x > r[0] + r[2] ? x - r[0] - r[2] : 0;
        float dy = y < r[1] ? r[1] - y : y > r[1] + r[3] ? y - r[1] - r[3] : 0;
        float d = dx * dx + dy * dy;
        if (d < best) best = d;
    }
    return best;
}
// The button of this detector under the click (or the cursor): the smallest one when several overlap; of buttons
// with the very same rectangle, the one whose drawn part is nearest.
static uint64_t button_at(Cpu *c, uint64_t det, int hover) {
    uint64_t best = 0;
    float best_area = 0, best_dist = -1, x, y;
    rt_ui_point(hover, &x, &y);
    for (int i = 0; i < MAX_BUTTONS; i++) {
        uint64_t v = g_buttons[i].view, ma = v ? LD64(v + VIEW_MOUSE_ACTION) : 0;
        if (!ma || LD64(ma + MA_DETECTOR) != det) continue;
        if (!(rt_call(c, FN(F_OBJ_ALIVE), 2, v, 0ull) & 1)) continue;
        float area = rt_ui_hit(c, v, hover);
        if (area <= 0 || list_hides(c, v, x, y)) continue;
        if (best && area == best_area) {                             // a twin: the nearer drawn part wins
            if (best_dist < 0) best_dist = child_distance(c, best, x, y);
            float d = child_distance(c, v, x, y);
            if (!hover && getenv("BR_CLICK_LOG")) rt_log("click: twin buttons at (%.0f, %.0f): distances %g and %g", x, y, best_dist, d);
            if (d < best_dist) { best = v; best_dist = d; }
        } else if (!best || area < best_area) { best = v; best_area = area; best_dist = -1; }
    }
    return best;
}
static int name_has(Cpu *c, uint64_t view, const char *part) {
    char name[96] = "";
    uint64_t s = rt_call(c, FN(F_OBJ_NAME), 2, rt_call(c, FN(F_GET_GAMEOBJECT), 2, view, 0ull), 0ull);
    for (int i = 0; s && i < 95 && i < (int)LD32(s + 0x10); i++) { name[i] = (char)LD16(s + 0x14 + 2 * (uint64_t)i); name[i + 1] = 0; }
    return strstr(name, part) != NULL;
}
// A click on the standing Back button is Cancel for every detector of that button polled in the same frame, as
// Esc is: some screens poll two (the Style screen: the item list's and the screen's own, and only the second one
// leaves the screen), and the click is taken by the first.
static uint64_t g_back_click;
static uint32_t g_back_frame;
// Header buttons such as Options' Practice Stage and Reset (UIPartViewIndependentButton) get their mouse action on
// one player module's detector, which the screen never polls; the pad reaches the screen because the same input is
// added to every detector's action list. So a click on such a button is a one-frame press of the action its button
// guide shows, read by every detector as the pad's press would be (rt_hook_43c8530).
#define VIEW_GUIDE 0x50                     // UIPartViewButtonBase.m_ButtonGuideView
#define GUIDE_MODULE 0x38                   // UIPartViewBtnGuide_ForInputAction.m_DisplayModule
#define GUIDE_ACTION 0x48                   // UIBtnGuideDisplayModule_ForInputAction.m_InputAction
static uint32_t guide_action(Cpu *c, uint64_t v) {
    uint64_t g = LD64(v + VIEW_GUIDE), m;
    if (!g || !(rt_call(c, FN(F_OBJ_ALIVE), 2, g, 0ull) & 1) || !(m = LD64(g + GUIDE_MODULE))) return 0;
    uint32_t a = LD32(m + GUIDE_ACTION);
    return a < ACTION_COUNT ? a : 0;
}
static uint64_t unpolled_button_at(Cpu *c, uint32_t layer, uint32_t *action) {
    uint64_t best = 0;
    float best_area = 0;
    for (int i = 0; i < MAX_BUTTONS; i++) {
        uint64_t v = g_buttons[i].view, ma = v ? LD64(v + VIEW_MOUSE_ACTION) : 0, d;
        if (!ma || (LD32(ma + MA_SELECTABLE) & 0xff) || !(d = LD64(ma + MA_DETECTOR)) || polled_now(d) || LD32(d + DET_LAYER) != layer) continue;
        if (!(rt_call(c, FN(F_OBJ_ALIVE), 2, v, 0ull) & 1) || (LD32(v + VIEW_DISABLED) & 0xff)) continue;
        float area = rt_ui_hit(c, v, 0);
        uint32_t a;
        if (area > 0 && (!best || area < best_area) && (a = guide_action(c, v)) != 0) { best = v; best_area = area; *action = a; }
    }
    return best;
}
// Tabs that share one rectangle (the pause menu's "Game Menu" and "Help Menu" both span the header strip; the
// label tells them apart, button_at). The game's click on such a tab acts on the button it last pointed at, not
// on the clicked one, so only the first click switches. A click on the leftmost or rightmost label is therefore
// a click on the group's arrow on that side (ui_PartView_TabSide_Button_Left / _Right, the Q and E buttons),
// which walks the tabs as the keys do; on the tab already shown the arrow does nothing.
static float label_x(Cpu *c, uint64_t v) {
    uint64_t t = rt_call(c, FN(F_GET_TRANSFORM_), 2, v, 0ull), ch;
    float r[4];
    if (!t || !rt_call(c, FN(F_T_CHILD_COUNT), 2, t, 0ull) || !(ch = rt_call(c, FN(F_T_GET_CHILD), 3, t, 0ull, 0ull)) || !rt_ui_rect(c, ch, r)) return 0;
    return r[0] + r[2] / 2;
}
static uint64_t tab_arrow_for(Cpu *c, uint64_t det, uint64_t v) {
    float rv[4], r[4];
    if (!rt_ui_rect(c, v, rv)) return v;
    float xv = label_x(c, v);
    int twins = 0, left_of = 0, right_of = 0;
    uint64_t arrow_l = 0, arrow_r = 0;
    for (int i = 0; i < MAX_BUTTONS; i++) {
        uint64_t b = g_buttons[i].view, ma = b ? LD64(b + VIEW_MOUSE_ACTION) : 0;
        if (!ma || b == v || LD64(ma + MA_DETECTOR) != det || !(rt_call(c, FN(F_OBJ_ALIVE), 2, b, 0ull) & 1)) continue;
        if (name_has(c, b, "TabSide_Button_Left")) { arrow_l = b; continue; }
        if (name_has(c, b, "TabSide_Button_Right")) { arrow_r = b; continue; }
        if (!rt_ui_rect(c, b, r) || r[0] != rv[0] || r[1] != rv[1] || r[2] != rv[2] || r[3] != rv[3]) continue;
        twins++;
        if (label_x(c, b) < xv) left_of++; else right_of++;
    }
    if (!twins) return v;
    uint64_t arrow = !left_of ? arrow_l : !right_of ? arrow_r : 0;
    if (getenv("BR_CLICK_LOG")) rt_log("click: tab label (%d left of it, %d right): %s", left_of, right_of, arrow == arrow_l && arrow ? "left arrow" : arrow ? "right arrow" : "the tab");
    return arrow && (rt_call(c, FN(F_OBJ_ALIVE), 2, arrow, 0ull) & 1) ? arrow : v;
}
static uint64_t ui_mouse(Cpu *c, uint64_t det, uint64_t r) {
    if (g_back_click && g_back_frame == g_list_frame && (uint32_t)r == 0 && LD64(det + DET_RETURN_BUTTON) == g_back_click &&
        (uint32_t)rt_call(c, FN(F_LAYER_CURRENT), 1, 0ull) == LD32(det + DET_LAYER)) {
        ST32(det + DET_USER_INPUT, INPUT_CANCEL);
        return INPUT_CANCEL;
    }
    if ((uint32_t)r != 0 || (!rt_ui_click_pending() && !rt_ui_mouse_moved())) return r;
    uint32_t layer = LD32(det + DET_LAYER);
    if ((uint32_t)rt_call(c, FN(F_LAYER_CURRENT), 1, 0ull) != layer) return r;
    if ((layer & ~1u) != 4 && !(rt_call(c, FN(F_CHECK_FADE), 2, det, 0ull) & 1)) return r;
    if (wheel_editor_click(c, det)) { ST32(det + DET_USER_INPUT, INPUT_DECIDE); return INPUT_DECIDE; }
    if (rt_ui_click_pending() && photo_active() && photo_click(c)) return r;     // the photo menu's pickers (free photo camera)
    if (rt_ui_click_pending()) {
        uint64_t back = LD64(det + DET_RETURN_BUTTON), v;
        if (back && (rt_call(c, FN(F_OBJ_ALIVE), 2, back, 0ull) & 1) && rt_ui_hit(c, back, 0) > 0) {   // the standing Back button: Cancel
            rt_ui_click_consume();
            g_back_click = back; g_back_frame = g_list_frame;
            if (getenv("BR_CLICK_LOG")) rt_log("click: the Back button, Cancel");
            c->x[0] = back; c->x[1] = 6; c->x[2] = LD32(back + VIEW_DISABLED) & 0xff; c->x[3] = 0;     // eAnimationKind.PressedToHighlighted
            rt_setf(c, 0, 0.0f);
            rt_call_regs(c, FN(F_PLAY_ANIMATION));
            ST32(det + DET_USER_INPUT, INPUT_CANCEL);
            return INPUT_CANCEL;
        }
        if ((v = button_at(c, det, 0)) != 0) {
            v = tab_arrow_for(c, det, v);
            rt_ui_click_consume();
            if (name_has(c, v, "DisplayPassword") || name_has(c, v, "CopyRoomName")) rt_ui_press_anim(c, v);
            rt_call(c, FN(F_MOUSE_ON_CLICK), 3, LD64(v + VIEW_MOUSE_ACTION), v, 0ull);
            g_kbm_tick = GetTickCount64();
            return LD32(det + DET_USER_INPUT);
        }
        uint32_t action = 0;
        if ((v = unpolled_button_at(c, layer, &action)) != 0) {
            rt_ui_click_consume();
            rt_ui_press_anim(c, v);
            g_click_action = action; g_click_frame = g_list_frame + 1;
            g_kbm_tick = GetTickCount64();
            if (getenv("BR_CLICK_LOG")) rt_log("click: header button, action %u", action);
        }
        return r;
    }
    if (g_drag_sr) return r;                                         // a scroll bar is being dragged
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
int rt_hook_457ac20(Cpu *c) {
    static __thread int inside;
    if (inside) return 0;
    uint64_t det = X(0);
    polled_note(det);
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
#define F_GET_STATE 0x43a8eb0ull            // AppInput.GetState(int playerIndex, eLayer) (static)
#define F_EDIT_ON_PUSH 0x4508ae0ull         // UIStateOptionKeyConfigEdit.onPushButton(AppInput.State)
static __thread int t_edit_key;              // the key being assigned (-1: none, just close), while onPushButton runs
int rt_hook_4508710(Cpu *c) {                // UIStateOptionKeyConfigEdit.exec(float): {+0xa0 eLayer, +0xa8 int playerIndex}
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
int rt_hook_456fe00(Cpu *c) {
    if (enabled()) pc_action_reset((int)LD32(X(0) + 0x44));
    return 0;
}
int rt_hook_4572a10(Cpu *c) {                // UIOptionChoicesItemData_KeyConfig.SetKeyConfigEditButton(AppInput.State) -> SelectButton.eID
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
        if (!b->nkeys && !b->mouse && !b->vp_button) continue;
        for (int i = 0; i < b->nkeys; i++) n += snprintf(line + n, sizeof line - n, " %s", pc_key_name(b->keys[i]));
        if (b->analog) n += snprintf(line + n, sizeof line - n, " (analog %d/%d ms)", b->start_ms, b->end_ms);
        if (b->mouse) n += snprintf(line + n, sizeof line - n, " mouse %d", b->mouse);
        if (b->vp_button) n += snprintf(line + n, sizeof line - n, " %s button %d dir %#x sensitivity %d", b->gesture ? "gesture" : "virtual pad",
                                         b->vp_button, b->vp_dir, b->vp_sens);
        else if (b->gesture) n += snprintf(line + n, sizeof line - n, " gesture dir %#x sensitivity %d", b->vp_dir, b->vp_sens);
        rt_log("pc input: action %3d:%s", a, line);
    }
}

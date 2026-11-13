// UI changes for PC room codes (see online.c). The game's screens are built for its own six-character codes; a PC
// code has twelve characters (shown as XXXX-XXXX-XXXX):
//   * the code entry screen has six one-character boxes: they are merged into one text box for the whole code;
//   * the lobby shows the code in a field sized for six characters: the text is fitted on one line, and revealing
//     the code (R) also puts it on the Windows clipboard so it can be pasted to friends.
// Texts of six characters or fewer (the game's own passwords for local-wireless rooms) are left as they were.
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "rt.h"

#define FN(off) (GUEST_BASE + (off))
#define F_STRING_NEW_UTF16 0x456994ull      // il2cpp::vm::String::NewUtf16(const Il2CppChar *, int32_t)
#define F_TMP_GET_TEXT 0x3e06400ull         // TMP_Text.get_text()
#define F_TMP_SET_TEXT 0x3e0caa0ull         // TMP_Text.SetText(string, bool syncTextInputBox)
#define F_TMP_GET_FONT_SIZE 0x3e07440ull
#define F_TMP_SET_AUTO_SIZING 0x3e076d0ull
#define F_TMP_GET_AUTO_SIZING 0x3e076c0ull
#define F_TMP_SET_FONT_SIZE_MIN 0x3e07740ull
#define F_TMP_SET_FONT_SIZE_MAX 0x3e077b0ull
#define F_TMP_SET_CHAR_SPACING 0x3e07980ull
#define F_TMP_SET_WORD_WRAPPING 0x3e07c20ull
#define F_SET_EXPLANATION 0x46bf430ull
#define F_GET_GAMEOBJECT 0x3edc5c0ull       // UnityEngine.Component::get_gameObject
#define F_GET_TRANSFORM 0x3edc570ull        // UnityEngine.Component::get_transform
#define F_SET_ACTIVE 0x3ee0320ull           // UnityEngine.GameObject::SetActive(bool)
#define F_GO_ACTIVE_SELF 0x3ee0390ull   // UnityEngine.GameObject::get_activeSelf
#define F_RT_GET_ANCHOR_MIN 0x3eec590ull    // UnityEngine.RectTransform
#define F_RT_GET_ANCHOR_MAX 0x3eec730ull
#define F_RT_GET_ANCHORED_POS 0x3eec8d0ull
#define F_RT_SET_ANCHORED_POS 0x3eec9a0ull
#define F_RT_GET_SIZE_DELTA 0x3eeca70ull
#define F_RT_SET_SIZE_DELTA 0x3eecb40ull
#define STR_LEN 0x10
#define STR_CHARS 0x14
#define ARR_LEN 0x18
#define ARR_DATA 0x20

static uint64_t new_str16(Cpu *c, const uint16_t *s, int n) {
    return rt_call(c, FN(F_STRING_NEW_UTF16), 2, (uint64_t)(uintptr_t)s, (uint64_t)n);
}
static void call_f(Cpu *c, uint64_t fn, uint64_t obj, float v) {     // instance setter with one float argument
    c->x[0] = obj; c->x[1] = 0;
    rt_setf(c, 0, v);
    rt_call_regs(c, FN(fn));
}
static void get_v2(Cpu *c, uint64_t fn, uint64_t o, float out[2]) {
    rt_call(c, FN(fn), 2, o, 0ull);
    out[0] = rt_getf(c, 0); out[1] = rt_getf(c, 1);
}
static void set_v2(Cpu *c, uint64_t fn, uint64_t o, float x, float y) {   // Vector2 argument: floats in v0, v1
    c->x[0] = o; c->x[1] = 0;
    rt_setf(c, 0, x); rt_setf(c, 1, y);
    rt_call_regs(c, FN(fn));
}
// Make a text shrink to fit its box on one line.
static void fit_one_line(Cpu *c, uint64_t tmp, float spacing) {
    if (!tmp) return;
    int autos = (int)(rt_call(c, FN(F_TMP_GET_AUTO_SIZING), 2, tmp, 0ull) & 1);
    rt_call(c, FN(F_TMP_SET_WORD_WRAPPING), 3, tmp, 0ull, 0ull);
    call_f(c, F_TMP_SET_CHAR_SPACING, tmp, spacing);
    call_f(c, F_TMP_SET_FONT_SIZE_MIN, tmp, 6.0f);
    if (!autos) {                                                    // never grow beyond the designed size
        rt_call(c, FN(F_TMP_GET_FONT_SIZE), 2, tmp, 0ull);
        float size = rt_getf(c, 0);
        call_f(c, F_TMP_SET_FONT_SIZE_MAX, tmp, size);
        rt_call(c, FN(F_TMP_SET_AUTO_SIZING), 3, tmp, 1ull, 0ull);
    }
}// letters and digits of a managed string, upper case
static int code_chars(uint64_t s, uint16_t *out, int max) {
    int n = 0, len = s ? (int)LD32(s + STR_LEN) : 0;
    for (int i = 0; i < len && n < max; i++) {
        uint16_t ch = LD16(s + STR_CHARS + 2 * (uint64_t)i);
        if (ch >= 'a' && ch <= 'z') ch = (uint16_t)(ch - 32);
        if ((ch >= '0' && ch <= '9') || (ch >= 'A' && ch <= 'Z')) out[n++] = ch;
    }
    return n;
}

// UIViewBattleRoyaleFriendMatchEnterPassword.SetRoomNameText(string content): {+0x60 UIPartViewPasswordChar[] boxes},
// UIPartViewPasswordChar {+0x28 TextMeshProUGUI}. The six one-character boxes become one text box: the first box is
// widened over the place of all six, the others are switched off, and it shows the whole text.
int rt_hook_46bf4a0(Cpu *c) {
    uint64_t self = X(0), boxes = LD64(self + 0x60);
    uint16_t code[32], shown[40];
    int n = code_chars(X(1), code, 32), nb = boxes ? (int)LD32(boxes + ARR_LEN) : 0, m = 0;
    if (nb < 2) return 0;
    uint64_t box0 = LD64(boxes + ARR_DATA), last = LD64(boxes + ARR_DATA + 8 * (uint64_t)(nb - 1));
    uint64_t tmp = box0 ? LD64(box0 + 0x28) : 0;
    if (!box0 || !last || !tmp) return 0;
    uint64_t go1 = rt_call(c, FN(F_GET_GAMEOBJECT), 2, LD64(boxes + ARR_DATA + 8), 0ull);
    if (rt_call(c, FN(F_GO_ACTIVE_SELF), 2, go1, 0ull) & 1) {       // first time for this view object
        uint64_t t0 = rt_call(c, FN(F_GET_TRANSFORM), 2, box0, 0ull), t5 = rt_call(c, FN(F_GET_TRANSFORM), 2, last, 0ull);
        uint64_t tt = rt_call(c, FN(F_GET_TRANSFORM), 2, tmp, 0ull);
        float p0[2], p5[2], sz[2], tsz[2], amin[2], amax[2];
        get_v2(c, F_RT_GET_ANCHORED_POS, t0, p0);
        get_v2(c, F_RT_GET_ANCHORED_POS, t5, p5);
        get_v2(c, F_RT_GET_SIZE_DELTA, t0, sz);
        get_v2(c, F_RT_GET_SIZE_DELTA, tt, tsz);
        get_v2(c, F_RT_GET_ANCHOR_MIN, tt, amin);
        get_v2(c, F_RT_GET_ANCHOR_MAX, tt, amax);
        float span = p5[0] - p0[0];
        rt_log("online ui: code boxes at x %.0f..%.0f, size %.0fx%.0f; text rect size %.0fx%.0f anchors %.1f..%.1f", p0[0], p5[0], sz[0], sz[1],
               tsz[0], tsz[1], amin[0], amax[0]);
        for (int i = 1; i < nb; i++) {
            uint64_t b = LD64(boxes + ARR_DATA + 8 * (uint64_t)i);
            if (b) rt_call(c, FN(F_SET_ACTIVE), 3, rt_call(c, FN(F_GET_GAMEOBJECT), 2, b, 0ull), 0ull, 0ull);
        }
        set_v2(c, F_RT_SET_ANCHORED_POS, t0, p0[0] + span / 2, p0[1]);
        set_v2(c, F_RT_SET_SIZE_DELTA, t0, sz[0] + span, sz[1]);
        if (amin[0] == amax[0]) set_v2(c, F_RT_SET_SIZE_DELTA, tt, tsz[0] + span, tsz[1]);   // a fixed-width text rect grows too
        fit_one_line(c, tmp, 0.0f);
    }
    for (int i = 0; i < n && m < 38; i++) {                        // twelve characters read as XXXX-XXXX-XXXX
        if (n == 12 && i && i % 4 == 0) shown[m++] = '-';
        shown[m++] = code[i];
    }
    if (!m) shown[m++] = ' ';
    rt_call(c, FN(F_TMP_SET_TEXT), 4, tmp, new_str16(c, shown, m), 1ull, 0ull);
    return 1;
}
// UIViewBattleRoyaleFriendMatchEnterPassword.SetExplanationText(eTypeOfUse): {+0x68 TextMeshProUGUI explanation}.
// The English text says "six-character".
int rt_hook_46bf430(Cpu *c) {
    static __thread int inside;
    static const char from[] = "six-character", to[] = "12-character";
    if (inside) return 0;
    uint64_t self = X(0);
    inside = 1; c->pc = FN(F_SET_EXPLANATION); rt_dispatch(c); inside = 0;
    uint64_t tmp = LD64(self + 0x68), s = tmp ? rt_call(c, FN(F_TMP_GET_TEXT), 2, tmp, 0ull) : 0;
    int len = s ? (int)LD32(s + STR_LEN) : 0, fl = (int)sizeof from - 1, tl = (int)sizeof to - 1;
    if (len < fl || len > 500) return 1;
    for (int i = 0; i + fl <= len; i++) {
        int hit = 1;
        for (int k = 0; k < fl && hit; k++) hit = LD16(s + STR_CHARS + 2 * (uint64_t)(i + k)) == (uint16_t)from[k];
        if (!hit) continue;
        uint16_t out[520];
        int n = 0;
        for (int k = 0; k < i; k++) out[n++] = LD16(s + STR_CHARS + 2 * (uint64_t)k);
        for (int k = 0; k < tl; k++) out[n++] = (uint16_t)to[k];
        for (int k = i + fl; k < len; k++) out[n++] = LD16(s + STR_CHARS + 2 * (uint64_t)k);
        rt_call(c, FN(F_TMP_SET_TEXT), 4, tmp, new_str16(c, out, n), 1ull, 0ull);
        break;
    }
    return 1;
}

static void clipboard_set(const uint16_t *s, int n) {
    HGLOBAL h = GlobalAlloc(GMEM_MOVEABLE, ((size_t)n + 1) * 2);
    if (!h) return;
    uint16_t *p = GlobalLock(h);
    memcpy(p, s, (size_t)n * 2);
    p[n] = 0;
    GlobalUnlock(h);
    if (OpenClipboard(NULL)) {
        EmptyClipboard();
        if (SetClipboardData(CF_UNICODETEXT, h)) h = NULL;
        CloseClipboard();
    }
    if (h) GlobalFree(h);
}

// UIPasswordDisplayStatusModule.setupPasswordView(): {+0x10 bool visible, +0x18 UIPartViewMatchingPassword view};
// the view: {+0x28 TextMeshProUGUI, +0x50 string cached password}. Runs when the lobby is set up and each time the
// player shows or hides the code (R). It starts hidden (the game's default).
int rt_hook_4637b40(Cpu *c) {
    uint64_t self = X(0), view = LD64(self + 0x18), s = view ? LD64(view + 0x50) : 0;
    int len = s ? (int)LD32(s + STR_LEN) : 0;
    if (len > 6 && len < 64) {
        uint64_t tmp = LD64(view + 0x28);
        fit_one_line(c, tmp, 0.0f);
        c->x[0] = self;
    }
    return 0;
}
// ---------------------------------------------------------------------------------------------------- typing
// Text entry in the game's own screen instead of the console's software keyboard. The game has a PC path for this
// (UIViewCtrl_EnterPassword.createChoicesModule_Windows: no "open keyboard" button, a paste button, the Search button)
// next to the console path the Switch build uses; the hooks below switch the screen to it. The console's input
// module object (CharacterInputModule_SwitchForPassword) stays, and its per-frame MonitorCharacterInput, empty on the
// console, reads the PC keyboard here: letters and digits append (upper case), Backspace deletes, Ctrl+V pastes.
// CharacterInputModule_Base {+0x10 StringBuilder sbInputed}.
#define F_CHOICES_WINDOWS 0x461cd20ull      // UIViewCtrl_EnterPassword.createChoicesModule_Windows()
#define F_SB_LENGTH 0x3743960ull            // StringBuilder.get_Length()
#define F_SB_APPEND 0x3744700ull            // StringBuilder.Append(string)
#define F_SB_REMOVE 0x3744dc0ull            // StringBuilder.Remove(int start, int length)
#define F_SB_CLEAR 0x3743cf0ull             // StringBuilder.Clear()
#define F_ON_UPDATE_INPUT 0x47a6d00ull      // CharacterInputModule_Base.onUpdateInputStr()
#define F_BUTTON_SET_TEXT 0x44ed430ull      // UIPartViewIndependentButton.SetText(string)
#define CODE_MAX 12                         // characters of a PC room code
static uint64_t g_typing_tick;
static int g_code_len;                       // characters in the text box, as of the last frame it was read

// Test input: a BR_INPUT_LIVE line "TYPE <text>" is typed into the text box as if from the keyboard ('<' = Backspace).
static char g_typed[128];
static volatile LONG g_typed_w, g_typed_r;
void rt_text_entry_type(const char *s) {
    extern void name_ui_type(const char *s);
    name_ui_type(s);
    for (; *s; s++) { g_typed[(uint32_t)g_typed_w % sizeof g_typed] = *s; InterlockedIncrement(&g_typed_w); }
}

// 1 while a text entry screen is reading the keyboard: imports_hid.c then keeps letter, digit and Backspace keys
// from acting as controller buttons.
int rt_text_entry_active(void) { return GetTickCount64() - g_typing_tick < 200; }
void rt_text_entry_keepalive(void) { g_typing_tick = GetTickCount64(); }   // name_ui.c's text box, once a frame

static int window_focused(void) {
    extern int rt_no_real_input(void);
    DWORD pid = 0;
    HWND w = GetForegroundWindow();
    if (rt_no_real_input()) return 0;
    if (w) GetWindowThreadProcessId(w, &pid);
    return pid == GetCurrentProcessId();
}
static int sb_append(Cpu *c, uint64_t sb, int len, uint16_t ch) {
    if (len >= CODE_MAX) return len;
    if (ch >= 'a' && ch <= 'z') ch = (uint16_t)(ch - 32);
    if (!((ch >= '0' && ch <= '9') || (ch >= 'A' && ch <= 'Z'))) return len;
    rt_call(c, FN(F_SB_APPEND), 3, sb, new_str16(c, &ch, 1), 0ull);
    return len + 1;
}
// CharacterInputModule_Base.MonitorCharacterInput(): called every frame while the screen accepts input.
int rt_hook_47a6ce0(Cpu *c) {
    static uint8_t down[256];
    uint64_t self = X(0), sb = LD64(self + 0x10), now = GetTickCount64();
    int fresh = now - g_typing_tick > 500, focus = window_focused(), changed = 0;
    g_typing_tick = now;
    if (!sb) return 1;
    int len = (int)rt_call(c, FN(F_SB_LENGTH), 2, sb, 0ull);
    int ctrl = (GetAsyncKeyState(VK_CONTROL) & 0x8000) != 0;
    for (int vk = 8; vk < 0x6a; vk++) {
        int digit = vk >= '0' && vk <= '9', pad = vk >= VK_NUMPAD0 && vk <= VK_NUMPAD9, letter = vk >= 'A' && vk <= 'Z';
        if (!(digit || pad || letter || vk == VK_BACK || vk == VK_DELETE)) continue;
        int is = focus && (GetAsyncKeyState(vk) & 0x8000) != 0, edge = is && !down[vk];
        down[vk] = (uint8_t)is;
        if (vk == VK_BACK || vk == VK_DELETE) {                     // held: deletes again after 400 ms, then every 45 ms
            static uint64_t again[2];
            uint64_t *t = &again[vk == VK_DELETE];
            if (edge) *t = now + 400;
            else if (is && now >= *t && !fresh) { edge = 1; *t = now + 45; }
        }
        if (!edge || fresh) continue;                               // keys already held when the screen opened do not type
        if (vk == VK_BACK || vk == VK_DELETE) {
            if (len > 0) { rt_call(c, FN(F_SB_REMOVE), 4, sb, (uint64_t)(len - 1), 1ull, 0ull); len--; changed = 1; }
        } else if (ctrl && vk == 'V') {
            if (OpenClipboard(NULL)) {
                HANDLE h = GetClipboardData(CF_UNICODETEXT);
                const uint16_t *s = h ? GlobalLock(h) : NULL;
                if (s) {
                    uint16_t buf[64];
                    int n = 0;
                    for (; n < 63 && s[n]; n++) buf[n] = s[n];
                    GlobalUnlock(h);
                    rt_call(c, FN(F_SB_CLEAR), 2, sb, 0ull);
                    len = 0;
                    for (int i = 0; i < n; i++) len = sb_append(c, sb, len, buf[i]);
                    changed = 1;
                }
                CloseClipboard();
            }
        } else if (!ctrl) {
            int before = len;
            len = sb_append(c, sb, len, (uint16_t)(pad ? '0' + (vk - VK_NUMPAD0) : vk));
            changed |= len != before;
        }
    }
    while (g_typed_r != g_typed_w) {
        char ch = g_typed[(uint32_t)g_typed_r % sizeof g_typed];
        InterlockedIncrement(&g_typed_r);
        if (ch == '<') { if (len > 0) { rt_call(c, FN(F_SB_REMOVE), 4, sb, (uint64_t)(len - 1), 1ull, 0ull); len--; changed = 1; } }
        else { int before = len; len = sb_append(c, sb, len, (uint16_t)(uint8_t)ch); changed |= len != before; }
    }
    g_code_len = len;
    if (changed) rt_call(c, FN(F_ON_UPDATE_INPUT), 2, self, 0ull);
    return 1;
}
// CharacterInputModule_SwitchForPassword.OpenKeyboard(): there is no keyboard to open, typing is always on.
int rt_hook_47a6f20(Cpu *c) { (void)c; return 1; }
// The game counts a password as complete at six characters: it then enables the Search button and presses it by
// itself. A PC room code is complete at CODE_MAX characters.
// UIViewCtrl_EnterPassword.SetEnableSearchBtn(bool isDisable)
int rt_hook_461c360(Cpu *c) {
    if (rt_text_entry_active()) c->x[1] = g_code_len < CODE_MAX;
    return 0;
}
// UIViewCtrl_EnterPassword.ForceFocusOnDecide()
int rt_hook_461c5b0(Cpu *c) {
    (void)c;
    return rt_text_entry_active() && g_code_len < CODE_MAX;
}
// CharacterInputModule_SwitchForPassword.SetStr(string): where the game's paste button delivers the clipboard text.
int rt_hook_47a7000(Cpu *c) {
    uint64_t self = X(0), s = X(1), sb = LD64(self + 0x10);
    uint16_t buf[64];
    int n = 0, len = 0, sl = s ? (int)LD32(s + STR_LEN) : 0;
    if (!sb) return 1;
    for (; n < sl && n < 64; n++) buf[n] = LD16(s + STR_CHARS + 2 * (uint64_t)n);
    rt_call(c, FN(F_SB_CLEAR), 2, sb, 0ull);
    for (int i = 0; i < n; i++) len = sb_append(c, sb, len, buf[i]);
    g_code_len = len;
    rt_call(c, FN(F_ON_UPDATE_INPUT), 2, self, 0ull);
    return 1;
}

// The PC layout of the screen: UIViewCtrl_EnterPassword {+0x28 view}; the view {+0x50 paste button}. The button has
// no caption in this build.
static void pc_layout(Cpu *c) {
    uint64_t self = X(0);
    c->pc = FN(F_CHOICES_WINDOWS);
    rt_dispatch(c);
    uint64_t view = LD64(self + 0x28), btn = view ? LD64(view + 0x50) : 0;
    static const uint16_t label[] = {'P', 'a', 's', 't', 'e', ' ', '(', 'C', 't', 'r', 'l', '+', 'V', ')'};
    if (btn) rt_call(c, FN(F_BUTTON_SET_TEXT), 3, btn, new_str16(c, label, 14), 0ull);
}
// UIViewCtrl_EnterPassword.createChoicesModule_TouchScreenKeyboard(): use the PC layout.
int rt_hook_461c8e0(Cpu *c) {
    pc_layout(c);
    return 1;
}
// UIViewCtrl_EnterPassword.setupChoicesGroupModule(): the console version builds the touch layout and then hides
// the paste button; build the PC layout and leave the button.
int rt_hook_461c8b0(Cpu *c) {
    pc_layout(c);
    return 1;
}
// ---------------------------------------------------------------------------------------------------- copy button
// The lobby has a "copy the room code" button and a ClipboardCopy input (eUserInput 41) that the Switch build
// switches off: UIViewCtrl_BtlSettingsLobby_Host.setupCopyRoomNameBtn only hides the button. Set it up the way the
// class sets up its other buttons (setupPickKickBtn): show it and register the class's own onCopyRoomNameBtn, which
// ends in GUIUtility.systemCopyBuffer = the session code.
// UIViewCtrl_BtlSettingsLobby_Operable {+0x68 choices module {+0x10 -> {+0x20 UIChoicesEventCaller_External}}};
// UIViewBattleSettingsLobby {+0x50 copy button}.
#define F_INIT_METADATA 0x44c0d0ull         // il2cpp_codegen_initialize_runtime_metadata(uintptr_t *slot)
#define F_OBJECT_NEW 0x44c4a0ull            // il2cpp_codegen_object_new(klass)
#define F_CLASS_GET_METHODS 0x4654b0ull     // il2cpp_class_get_methods(klass, void **iter)
#define F_VIEW_SET_ACTIVE 0x44e53b0ull      // UIPartViewBase.SetActive(bool)
#define F_REGISTERED_EVENT_CTOR 0x4579e80ull   // RegisteredEvent..ctor(object target, IntPtr method)
#define F_SET_INDEPENDENT_BUTTON 0x457aca0ull  // UIChoicesEventCaller_External.SetIndependentButtonEvent(input, button, event, onPointerEnter, needDecideSE)
#define M_CLASS_REGISTERED_EVENT 0x611f548ull
#define INPUT_CLIPBOARD_COPY 41
static uint64_t find_method(Cpu *c, uint64_t klass, const char *name) {
    uint64_t iter = 0, m;
    while ((m = rt_call(c, FN(F_CLASS_GET_METHODS), 2, klass, (uint64_t)(uintptr_t)&iter)) != 0)
        if (!strcmp((const char *)(uintptr_t)LD64(m + 0x18), name)) return m;      // MethodInfo.name
    return 0;
}
static uint64_t class_of_slot(Cpu *c, uint64_t slot_ptr) {
    uint64_t p = LD64(FN(slot_ptr));
    if (LD64(p) & 1) rt_call(c, FN(F_INIT_METADATA), 1, p);
    return LD64(p);
}
// The button is an empty pill in this build. Make it a small square icon button like the lobby's other ones and put
// it at the left end of the password panel. The icon is Google's Material "content copy" (see icons\README.txt).
#define F_GAMEOBJECT_CTOR 0x3ee0840ull      // GameObject..ctor(string name)
#define F_GO_GET_TRANSFORM 0x3ee0150ull     // GameObject.get_transform()
#define F_GO_ADD_COMPONENT 0x2462270ull     // GameObject.AddComponent<T>() (shared; MethodInfo selects T)
#define F_T_SET_PARENT 0x3eefbe0ull         // Transform.SetParent(Transform, bool worldPositionStays)
#define F_T_GET_POSITION 0x3eee800ull
#define F_T_SET_POSITION 0x3eee8d0ull
#define F_T_LOSSY_SCALE 0x3ef2bb0ull
#define F_RT_GET_RECT 0x3eec4c0ull          // RectTransform.get_rect(): x, y, width, height in v0..v3
#define F_TEX_CTOR 0x3ebd6d0ull             // Texture2D..ctor(int, int)
#define F_LOAD_IMAGE 0x3f27ae0ull           // ImageConversion.LoadImage(Texture2D, byte[])
#define F_SPRITE_CREATE 0x3ef6790ull        // Sprite.Create(Texture2D, Rect, Vector2 pivot)
#define F_IMAGE_SET_SPRITE 0x3fb00e0ull
#define F_GRAPHIC_SET_RAYCAST 0x3fb8c20ull
#define F_GRAPHIC_SET_COLOR 0x3fb8bc0ull     // Graphic.set_color(Color): four floats in v0..v3
#define F_SZARRAY_NEW 0x44c230ull
#define F_GCHANDLE_NEW 0x4657a0ull          // il2cpp_gchandle_new(obj, pinned)
#define F_GCHANDLE_TARGET 0x4657f0ull
#define M_CLASS_GAMEOBJECT 0x60c9e98ull
#define M_CLASS_TEXTURE2D 0x60d0658ull
#define M_CLASS_BYTE_ARR 0x60cf328ull
#define M_METHOD_ADD_IMAGE 0x60ffb08ull     // GameObject.AddComponent<UnityEngine.UI.Image>
#define F_OBJ_ALIVE 0x3ee4c20ull            // UnityEngine.Object::op_Implicit (static)
#define F_GCHANDLE_FREE 0x465810ull
#define F_OBJ_SET_HIDE_FLAGS 0x3ee66e0ull   // UnityEngine.Object::set_hideFlags
static const uint8_t k_copy_png[] = {
#include "icons/content_copy_white.inc"
};
static void get_v3(Cpu *c, uint64_t fn, uint64_t o, float out[4]) {
    rt_call(c, FN(fn), 2, o, 0ull);
    for (int i = 0; i < 4; i++) out[i] = rt_getf(c, i);
}
static void set_v3(Cpu *c, uint64_t fn, uint64_t o, float x, float y, float z) {
    c->x[0] = o; c->x[1] = 0;
    rt_setf(c, 0, x); rt_setf(c, 1, y); rt_setf(c, 2, z);
    rt_call_regs(c, FN(fn));
}
// The icon sprite, made once and kept alive through a GC handle (host memory is not scanned by the collector).
static uint64_t copy_sprite(Cpu *c) {
    static uint32_t handle;
    if (handle) {                                                    // still loaded? (see the hide flags below)
        uint64_t old = rt_call(c, FN(F_GCHANDLE_TARGET), 1, (uint64_t)handle);
        if (old && (rt_call(c, FN(F_OBJ_ALIVE), 2, old, 0ull) & 1)) return old;
        rt_call(c, FN(F_GCHANDLE_FREE), 1, (uint64_t)handle);
        handle = 0;
    }
    uint64_t tex = rt_call(c, FN(F_OBJECT_NEW), 1, class_of_slot(c, M_CLASS_TEXTURE2D));
    rt_call(c, FN(F_TEX_CTOR), 4, tex, 2ull, 2ull, 0ull);
    uint64_t bytes = rt_call(c, FN(F_SZARRAY_NEW), 2, class_of_slot(c, M_CLASS_BYTE_ARR), (uint64_t)sizeof k_copy_png);
    memcpy((void *)(uintptr_t)(bytes + ARR_DATA), k_copy_png, sizeof k_copy_png);
    if (!(rt_call(c, FN(F_LOAD_IMAGE), 3, tex, bytes, 0ull) & 1)) { rt_log("online ui: the copy icon could not be decoded"); return 0; }
    c->x[0] = tex; c->x[1] = 0;                                     // Sprite.Create(tex, Rect(0, 0, 96, 96), Vector2(0.5, 0.5))
    rt_setf(c, 0, 0); rt_setf(c, 1, 0); rt_setf(c, 2, 96); rt_setf(c, 3, 96); rt_setf(c, 4, 0.5f); rt_setf(c, 5, 0.5f);
    rt_call_regs(c, FN(F_SPRITE_CREATE));
    uint64_t sprite = c->x[0];
    if (sprite) handle = (uint32_t)rt_call(c, FN(F_GCHANDLE_NEW), 2, sprite, 0ull);
    // The engine unloads assets nothing in a scene refers to when a lobby closes; a sprite unloaded that way left a
    // plain dark square in the next lobby. HideFlags.DontUnloadUnusedAsset keeps both objects.
    rt_call(c, FN(F_OBJ_SET_HIDE_FLAGS), 3, tex, 32ull, 0ull);
    if (sprite) rt_call(c, FN(F_OBJ_SET_HIDE_FLAGS), 3, sprite, 32ull, 0ull);
    return sprite;
}
// The password panel and the copy button, reworked for a 12-character code. Objects by name, sizes in the UI's
// 1920x1080 units (from a dump of the prefab):
//   ui_PartVIew_Matching_Password 610x150
//     ImageBackGround (stretched)
//       TextPasswordStr "Password"  140 wide at x -286        Bg_Pass (the dark field) 360x60 at x 40
//         TextPassword 320x48                                  DisplayPassword button 50x50 at x 216
//       ButtonGuide (R) 48x48 at x 264                         Bg_Line 560 wide, TextInviteFriend 580 wide
//   ui_PartView_Info_IndependentButton_CopyRoomName 364x64, elsewhere in the lobby (bottom left)
//     ImageBackGround 364x64 > Text (TMP), ui_PartView_ButtonGuide
// The panel becomes 800 wide with a 440 field, and the copy button moves into it as a 50x50 icon button after the
// field, left of the show/hide (eye) control, drawn as a bare dark icon like the eye. Everything stays inside the
// game's own panel, so it shows and hides with it.
#define PANEL_W 800.0f
#define F_CHILD_COUNT 0x3ef2800ull
#define F_GET_CHILD 0x3ef3020ull
#define F_OBJ_NAME 0x3ee4e30ull
#define F_RT_SET_ANCHOR_MIN 0x3eec660ull
#define F_RT_SET_ANCHOR_MAX 0x3eec800ull
#define F_RT_SET_PIVOT 0x3eecce0ull
#define F_T_GET_PARENT 0x3eee6b0ull
#define F_GO_ACTIVE_IN_HIER 0x3ee03e0ull
static uint64_t child_named(Cpu *c, uint64_t t, const char *name) {
    int n = t ? (int)rt_call(c, FN(F_CHILD_COUNT), 2, t, 0ull) : 0;
    for (int i = 0; i < n; i++) {
        uint64_t ch = rt_call(c, FN(F_GET_CHILD), 3, t, (uint64_t)i, 0ull);
        uint64_t s = rt_call(c, FN(F_OBJ_NAME), 2, rt_call(c, FN(F_GET_GAMEOBJECT), 2, ch, 0ull), 0ull);
        int len = s ? (int)LD32(s + STR_LEN) : 0, k = 0;
        for (; k < len && name[k] && LD16(s + STR_CHARS + 2 * (uint64_t)k) == (uint8_t)name[k]; k++) {}
        if (k == len && !name[k]) return ch;
    }
    return 0;
}
static void place(Cpu *c, uint64_t t, float x, float w) {          // new x position and width, the rest unchanged
    float pos[2], size[2];
    if (!t) return;
    get_v2(c, F_RT_GET_ANCHORED_POS, t, pos);
    get_v2(c, F_RT_GET_SIZE_DELTA, t, size);
    set_v2(c, F_RT_SET_ANCHORED_POS, t, x, pos[1]);
    if (w > 0) set_v2(c, F_RT_SET_SIZE_DELTA, t, w, size[1]);
}
static void hide(Cpu *c, uint64_t t) {
    if (t) rt_call(c, FN(F_SET_ACTIVE), 3, rt_call(c, FN(F_GET_GAMEOBJECT), 2, t, 0ull), 0ull, 0ull);
}
// The lobby whose copy button can be clicked: handles to the button and its password view (see copy_tick).
static uint32_t g_copy_btn_handle, g_copy_view_handle;
void rt_ui_press_anim(Cpu *c, uint64_t view);
// The chat log (system canvas: ui_View_ChatLog > ui_PartView_ChatLogRoot, with a translucent grey strip 600 units
// wide at the top left) is drawn above the lobby's own canvas, so its strip lay over the left end of the widened
// password panel. The panel gets a Canvas of its own that sorts one step above the chat log's canvas, but only
// while the lobby itself is in front (panel_layer_tick): overlays of the lobby (the chat wheel editor, How to
// Play, the kick list) and dialogs are drawn between the lobby and the chat log and must cover the panel.
#define F_GO_GET_COMPONENT 0x24623a0ull     // GameObject.GetComponent<T>() (shared; MethodInfo selects T)
#define F_GO_FIND 0x3ee0bb0ull              // GameObject.Find(string) (static)
#define F_CANVAS_SET_OVERRIDE_SORTING 0x413c440ull
#define F_CANVAS_GET_SORTING_ORDER 0x413c4b0ull
#define F_CANVAS_SET_SORTING_ORDER 0x413c500ull
#define F_CANVAS_GET_ROOT 0x413c8c0ull
#define M_METHOD_GO_GET_IMAGE 0x60f4ad8ull  // GameObject.GetComponent<UnityEngine.UI.Image>
#define M_METHOD_GO_GET_CANVAS 0x60ce6c8ull // GameObject.GetComponent<UnityEngine.Canvas>
#define M_METHOD_GO_ADD_CANVAS 0x60ce6e0ull // GameObject.AddComponent<UnityEngine.Canvas>
static uint32_t g_panel_canvas_h;
static int g_panel_order, g_panel_raised;
static void panel_above_chat(Cpu *c, uint64_t panel_t) {
    static const uint16_t name[] = {'u', 'i', '_', 'V', 'i', 'e', 'w', '_', 'C', 'h', 'a', 't', 'L', 'o', 'g'};
    uint64_t chat = rt_call(c, FN(F_GO_FIND), 2, new_str16(c, name, 15), 0ull);
    uint64_t pgo = rt_call(c, FN(F_GET_GAMEOBJECT), 2, panel_t, 0ull);
    if (!chat || rt_call(c, FN(F_GO_GET_COMPONENT), 2, pgo, class_of_slot(c, M_METHOD_GO_GET_CANVAS))) return;
    uint64_t cc = 0;
    for (uint64_t t = rt_call(c, FN(F_GO_GET_TRANSFORM), 2, chat, 0ull); t && !cc; t = rt_call(c, FN(F_T_GET_PARENT), 2, t, 0ull))
        cc = rt_call(c, FN(F_GO_GET_COMPONENT), 2, rt_call(c, FN(F_GET_GAMEOBJECT), 2, t, 0ull), class_of_slot(c, M_METHOD_GO_GET_CANVAS));
    if (!cc) { rt_log("online ui: the chat log's canvas was not found"); return; }
    int order = (int)rt_call(c, FN(F_CANVAS_GET_SORTING_ORDER), 2, rt_call(c, FN(F_CANVAS_GET_ROOT), 2, cc, 0ull), 0ull);
    uint64_t canvas = rt_call(c, FN(F_GO_ADD_COMPONENT), 2, pgo, class_of_slot(c, M_METHOD_GO_ADD_CANVAS));
    if (!canvas) return;
    if (g_panel_canvas_h) rt_call(c, FN(F_GCHANDLE_FREE), 1, (uint64_t)g_panel_canvas_h);
    g_panel_canvas_h = (uint32_t)rt_call(c, FN(F_GCHANDLE_NEW), 2, canvas, 0ull);
    g_panel_order = order + 1;
    g_panel_raised = 0;
    rt_log("online ui: password panel can be drawn above the chat log (canvas order %d -> %d)", order, order + 1);
}
static void panel_layer_tick(Cpu *c) {
    extern int rt_name_screen_live(void);                            // name_ui.c: the lobby is the screen taking input
    if (!g_panel_canvas_h) return;
    uint64_t canvas = rt_call(c, FN(F_GCHANDLE_TARGET), 1, (uint64_t)g_panel_canvas_h);
    if (!canvas || !(rt_call(c, FN(F_OBJ_ALIVE), 2, canvas, 0ull) & 1)) { rt_call(c, FN(F_GCHANDLE_FREE), 1, (uint64_t)g_panel_canvas_h); g_panel_canvas_h = 0; return; }
    int want = rt_name_screen_live();
    if (want == g_panel_raised) return;
    g_panel_raised = want;
    rt_call(c, FN(F_CANVAS_SET_OVERRIDE_SORTING), 3, canvas, (uint64_t)want, 0ull);
    if (want) rt_call(c, FN(F_CANVAS_SET_SORTING_ORDER), 3, canvas, (uint64_t)(uint32_t)g_panel_order, 0ull);
}
static void restyle_password_panel(Cpu *c, uint64_t password_view, uint64_t btn, float shift) {
    uint64_t pt = rt_call(c, FN(F_GET_TRANSFORM), 2, password_view, 0ull), bg = child_named(c, pt, "ImageBackGround");
    uint64_t bt = rt_call(c, FN(F_GET_TRANSFORM), 2, btn, 0ull);
    float psize[2], ppos[2], y[2];
    get_v2(c, F_RT_GET_SIZE_DELTA, pt, psize);
    if (!bg || psize[0] > 700) return;                              // not the panel this was made for, or already done
    uint64_t field = child_named(c, bg, "Bg_Pass"), eye = child_named(c, bg, "ui_PartView_DisplayPassword_IndependentButton");
    get_v2(c, F_RT_GET_ANCHORED_POS, pt, ppos);
    set_v2(c, F_RT_SET_SIZE_DELTA, pt, PANEL_W, psize[1]);
    set_v2(c, F_RT_SET_ANCHORED_POS, pt, ppos[0] + shift, ppos[1]);
    place(c, child_named(c, bg, "TextPasswordStr"), -381, 0);
    place(c, field, -15, 440);
    place(c, child_named(c, field, "TextPassword"), -6, 400);
    place(c, eye, 294, 0);
    place(c, child_named(c, bg, "ui_PartView_ButtonGuide_ForInputAction"), 343, 0);
    place(c, child_named(c, bg, "Bg_Line"), 0, PANEL_W - 50);
    // the copy button: into the panel, 50x50, icon only
    uint64_t bbg = child_named(c, bt, "ImageBackGround");
    get_v2(c, F_RT_GET_ANCHORED_POS, eye ? eye : field, y);
    rt_call(c, FN(F_T_SET_PARENT), 4, bt, bg, 0ull, 0ull);
    set_v2(c, F_RT_SET_ANCHOR_MIN, bt, 0.5f, 0.5f);
    set_v2(c, F_RT_SET_ANCHOR_MAX, bt, 0.5f, 0.5f);
    set_v2(c, F_RT_SET_PIVOT, bt, 0.5f, 0.5f);
    set_v2(c, F_RT_SET_SIZE_DELTA, bt, 50, 50);
    set_v2(c, F_RT_SET_ANCHORED_POS, bt, 238, y[1]);
    hide(c, bbg);       // the button's pill background (with its caption and guide): squeezed to 50x50 it is a broken shape
    uint64_t sprite = copy_sprite(c);
    if (sprite) {
        static const uint16_t name[] = {'C', 'o', 'p', 'y', 'I', 'c', 'o', 'n'};
        uint64_t go = rt_call(c, FN(F_OBJECT_NEW), 1, class_of_slot(c, M_CLASS_GAMEOBJECT));
        rt_call(c, FN(F_GAMEOBJECT_CTOR), 3, go, new_str16(c, name, 8), 0ull);
        uint64_t image = rt_call(c, FN(F_GO_ADD_COMPONENT), 2, go, class_of_slot(c, M_METHOD_ADD_IMAGE));
        uint64_t it = rt_call(c, FN(F_GO_GET_TRANSFORM), 2, go, 0ull);
        rt_call(c, FN(F_T_SET_PARENT), 4, it, bt, 0ull, 0ull);
        set_v2(c, F_RT_SET_ANCHORED_POS, it, 0, 0);
        set_v2(c, F_RT_SET_SIZE_DELTA, it, 40, 40);
        rt_call(c, FN(F_IMAGE_SET_SPRITE), 3, image, sprite, 0ull);
        rt_call(c, FN(F_GRAPHIC_SET_RAYCAST), 3, image, 0ull, 0ull);
        c->x[0] = image; c->x[1] = 0;                               // Graphic.color: dark, like the eye icon beside it
        rt_setf(c, 0, 0.13f); rt_setf(c, 1, 0.11f); rt_setf(c, 2, 0.10f); rt_setf(c, 3, 1.0f);
        rt_call_regs(c, FN(F_GRAPHIC_SET_COLOR));
    }
    if (g_copy_btn_handle) rt_call(c, FN(F_GCHANDLE_FREE), 1, (uint64_t)g_copy_btn_handle);
    if (g_copy_view_handle) rt_call(c, FN(F_GCHANDLE_FREE), 1, (uint64_t)g_copy_view_handle);
    g_copy_btn_handle = (uint32_t)rt_call(c, FN(F_GCHANDLE_NEW), 2, btn, 0ull);
    g_copy_view_handle = (uint32_t)rt_call(c, FN(F_GCHANDLE_NEW), 2, password_view, 0ull);
    panel_above_chat(c, pt);
}
// BR_MOUSE_LOG: the objects under a UI element with their rectangles (to see what a button is made of).
static void dump_tree(Cpu *c, uint64_t t, int depth) {
    float r[4], sd[2], amin[2], amax[2], ap[2];
    char name[64] = "";
    uint64_t go = rt_call(c, FN(F_GET_GAMEOBJECT), 2, t, 0ull), s = rt_call(c, FN(F_OBJ_NAME), 2, go, 0ull);
    for (int i = 0; s && i < 63 && i < (int)LD32(s + STR_LEN); i++) { name[i] = (char)LD16(s + STR_CHARS + 2 * (uint64_t)i); name[i + 1] = 0; }
    get_v3(c, F_RT_GET_RECT, t, r);
    get_v2(c, F_RT_GET_SIZE_DELTA, t, sd); get_v2(c, F_RT_GET_ANCHOR_MIN, t, amin); get_v2(c, F_RT_GET_ANCHOR_MAX, t, amax); get_v2(c, F_RT_GET_ANCHORED_POS, t, ap);
    rt_log("ui tree: %*s%s rect %.0f %.0f %.0f %.0f sizeDelta %.0f %.0f anchors %.2f %.2f..%.2f %.2f pos %.0f %.0f active %d", depth * 2, "", name, r[0], r[1], r[2], r[3],
           sd[0], sd[1], amin[0], amin[1], amax[0], amax[1], ap[0], ap[1], (int)(rt_call(c, FN(F_GO_ACTIVE_SELF), 2, go, 0ull) & 1));
    int n = (int)rt_call(c, FN(F_CHILD_COUNT), 2, t, 0ull);
    for (int i = 0; i < n && depth < 4; i++) dump_tree(c, rt_call(c, FN(F_GET_CHILD), 3, t, (uint64_t)i, 0ull), depth + 1);
}
static void setup_copy_button(Cpu *c, uint64_t self, uint64_t btn, uint64_t caller, uint64_t password_view, float shift) {
    uint64_t mi = find_method(c, LD64(self), "onCopyRoomNameBtn");
    if (!btn || !caller || !mi || !password_view) { rt_log("online ui: copy button not set up (button %d, events %d, handler %d)", btn != 0, caller != 0, mi != 0); return; }
    if (getenv("BR_MOUSE_LOG")) { dump_tree(c, rt_call(c, FN(F_GET_TRANSFORM), 2, password_view, 0ull), 0); dump_tree(c, rt_call(c, FN(F_GET_TRANSFORM), 2, btn, 0ull), 0); }
    rt_call(c, FN(F_VIEW_SET_ACTIVE), 3, btn, 1ull, 0ull);
    restyle_password_panel(c, password_view, btn, shift);
    uint64_t ev = rt_call(c, FN(F_OBJECT_NEW), 1, class_of_slot(c, M_CLASS_REGISTERED_EVENT));
    rt_call(c, FN(F_REGISTERED_EVENT_CTOR), 4, ev, self, mi, 0ull);
    rt_call(c, FN(F_SET_INDEPENDENT_BUTTON), 7, caller, (uint64_t)INPUT_CLIPBOARD_COPY, btn, ev, 0ull, 1ull, 0ull);
}// The button's handlers. Battle's would reach GUIUtility.systemCopyBuffer through an observer; Adventure's calls an
// action nothing sets in this build. Both copy the lobby's code here. {+0x50 string cached password} in the
// password view.
static void copy_code(uint64_t password_view) {
    uint64_t s = password_view ? LD64(password_view + 0x50) : 0;
    int len = s ? (int)LD32(s + STR_LEN) : 0;
    if (len < 1 || len > 64) return;
    clipboard_set((const uint16_t *)(uintptr_t)(s + STR_CHARS), len);
    rt_log("online: room code copied to the clipboard");
}
// Battle. UIViewCtrl_BtlSettingsLobby_Host.setupCopyRoomNameBtn(lobbyView): {+0x68 choices module {+0x10 ->
// {+0x20 event caller}}}; UIViewBattleSettingsLobby {+0x48 password view, +0x50 copy button}.
int rt_hook_461f3e0(Cpu *c) {
    uint64_t self = X(0), view = X(1), module = LD64(self + 0x68), inner = module ? LD64(module + 0x10) : 0;
    if (!view) return 0;
    setup_copy_button(c, self, LD64(view + 0x50), inner ? LD64(inner + 0x20) : 0, LD64(view + 0x48), -90.0f);   // clear of the player list on the right
    return 1;
}
// UIViewCtrl_BtlSettingsLobby_Host.onCopyRoomNameBtn(...): {+0x28 lobby view}
int rt_hook_461f790(Cpu *c) {
    uint64_t view = LD64(X(0) + 0x28);
    copy_code(view ? LD64(view + 0x48) : 0);
    return 1;
}
// Adventure. UIViewCtrl_AdvMultiMatching_Friend.SetupForHost(string sessionCode): {+0x20 view, +0x28 -> {+0x20
// event caller}}; UIViewAdvMulti_Matching {+0x58 password view, +0x60 copy button}. The original ends by hiding
// the copy button.
int rt_hook_4610c90(Cpu *c) {
    static __thread int inside;
    if (inside) return 0;
    uint64_t self = X(0), code = X(1);
    inside = 1; c->pc = FN(0x4610c90ull); rt_dispatch(c); inside = 0;
    uint64_t view = LD64(self + 0x20), m = LD64(self + 0x28);
    if (view && code && LD32(code + STR_LEN)) setup_copy_button(c, self, LD64(view + 0x60), m ? LD64(m + 0x20) : 0, LD64(view + 0x58), 0.0f);
    return 1;
}
// UIViewCtrl_AdvMultiMatching_Friend.onCopyRoomNameBtn(...)
int rt_hook_4611470(Cpu *c) {
    uint64_t view = LD64(X(0) + 0x20);
    copy_code(view ? LD64(view + 0x58) : 0);
    return 1;
}
// BR_MOUSE_LOG=1: every 2 seconds, whether the game's mouse layer has a device and which device it saw last.
void mouse_diag_tick(Cpu *c) {
    static int on = -1;
    static uint64_t next;
    if (on < 0) on = getenv("BR_MOUSE_LOG") != NULL;
    if (!on || GetTickCount64() < next) return;
    next = GetTickCount64() + 2000;
    uint64_t mouse = rt_call(c, FN(0x3b5dc80ull), 1, 0ull), kb = rt_call(c, FN(0x3b5c450ull), 1, 0ull);   // Mouse.current, Keyboard.current
    int mask = (int)rt_call(c, FN(0x4384a40ull), 1, 0ull);                                                 // AppInput.latestInputDeviceMask
    float pos[2] = {0, 0};
    int pressed = -1;
    if (mouse) {
        uint64_t position = rt_call(c, FN(0x3b5f080ull), 2, mouse, 0ull);                                  // Pointer.position
        if (position) { rt_call(c, FN(0x311aa70ull), 2, position, 0ull); pos[0] = rt_getf(c, 0); pos[1] = rt_getf(c, 1); }
        uint64_t left = rt_call(c, FN(0x3b5dbc0ull), 2, mouse, 0ull);
        if (left) pressed = (int)(rt_call(c, FN(0x3b2a1c0ull), 2, left, 0ull) & 1);
    }
    rt_log("mouse: device %s, keyboard %s, latest input device mask %d, position %.0f %.0f, left button %d", mouse ? "present" : "none",
           kb ? "present" : "none", mask, pos[0], pos[1], pressed);
}
// The copy button by Ctrl+C (a mouse click reaches it like every other button, see the mouse part of pc_input.c).
// Called once a frame.
static BOOL CALLBACK main_window(HWND h, LPARAM out) {
    if (IsWindowVisible(h) && !GetWindow(h, GW_OWNER)) { *(HWND *)out = h; return FALSE; }
    return TRUE;
}
#define F_T_SET_LOCAL_SCALE 0x3eef8c0ull
// The mouse for the UI, in window pixels from the bottom left (ui_mouse_frame, once a frame, at the end of the
// frame): the cursor, whether it moved, and a left click, which stays pending through the next frame's updates until
// something takes it (rt_ui_click_consume). BR_INPUT_LIVE "CLICK x y" and "MOVE x y" stand in for the real mouse.
static int g_click, g_test_click, g_test_move, g_test_x, g_test_y, g_mouse_moved, g_mouse_inside;
static float g_click_x, g_click_y, g_mouse_x = -1, g_mouse_y = -1;
void rt_ui_test_click(int x, int y) { g_test_x = x; g_test_y = y; g_test_click = 1; }
void rt_ui_test_move(int x, int y) { g_test_x = x; g_test_y = y; g_test_move = 1; }
static void ui_mouse_frame(void) {
    static int was_down;
    HWND w = NULL;
    POINT p;
    RECT cr;
    int down = (GetAsyncKeyState(VK_LBUTTON) & 0x8000) != 0, edge = down && !was_down && window_focused();
    was_down = down;
    g_click = 0; g_mouse_moved = 0;
    EnumThreadWindows(GetCurrentThreadId(), main_window, (LPARAM)&w);
    if (!w || !GetClientRect(w, &cr)) return;
    if (g_test_click || g_test_move) { p.x = g_test_x; p.y = g_test_y; edge = g_test_click; g_test_click = g_test_move = 0; }
    else if (!window_focused() || !GetCursorPos(&p) || !ScreenToClient(w, &p)) { g_mouse_inside = 0; return; }
    float x = (float)p.x, y = (float)(cr.bottom - p.y);
    g_mouse_inside = p.x >= 0 && p.y >= 0 && p.x < cr.right && p.y < cr.bottom;
    g_mouse_moved = g_mouse_inside && (x != g_mouse_x || y != g_mouse_y);
    g_mouse_x = x; g_mouse_y = y;
    if (edge && g_mouse_inside) { g_click = 1; g_click_x = x; g_click_y = y; }
}
int rt_ui_click_pending(void) { return g_click; }
void rt_ui_click_consume(void) { g_click = 0; }
int rt_ui_mouse_moved(void) { return g_mouse_moved; }
// The element's rectangle on screen; 0 when it is not shown.
static int ui_rect(Cpu *c, uint64_t view, float r[4]) {
    float pos[4], rect[4], scale[4];
    if (!view || !(rt_call(c, FN(F_GO_ACTIVE_IN_HIER), 2, rt_call(c, FN(F_GET_GAMEOBJECT), 2, view, 0ull), 0ull) & 1)) return 0;
    uint64_t t = rt_call(c, FN(F_GET_TRANSFORM), 2, view, 0ull);
    get_v3(c, F_T_GET_POSITION, t, pos);
    get_v3(c, F_RT_GET_RECT, t, rect);
    get_v3(c, F_T_LOSSY_SCALE, t, scale);
    r[0] = pos[0] + rect[0] * scale[0]; r[1] = pos[1] + rect[1] * scale[1];
    r[2] = rect[2] * scale[0]; r[3] = rect[3] * scale[1];
    return 1;
}
// Area in pixels of the element when the point (the pending click, or the cursor for hover = 1) is on it, else 0.
float rt_ui_hit(Cpu *c, uint64_t view, int hover) {
    float r[4], x = hover ? g_mouse_x : g_click_x, y = hover ? g_mouse_y : g_click_y;
    if (hover ? !g_mouse_inside : !g_click) return 0;
    if (!ui_rect(c, view, r)) return 0;
    return x >= r[0] && x <= r[0] + r[2] && y >= r[1] && y <= r[1] + r[3] ? r[2] * r[3] + 1 : 0;
}
// 1 when the pending click landed on the UI element (a view or any component of it).
int rt_ui_clicked(Cpu *c, uint64_t view) { return rt_ui_hit(c, view, 0) > 0; }
// A quick "pressed" motion for a button the port handles itself: the button shrinks to 82% and springs back over
// PRESS_MS. One button at a time.
#define PRESS_MS 160
static uint32_t g_press_handle;
static uint64_t g_press_t0;
void rt_ui_press_anim(Cpu *c, uint64_t view) {
    if (g_press_handle) {
        uint64_t old = rt_call(c, FN(F_GCHANDLE_TARGET), 1, (uint64_t)g_press_handle);
        if (old && (rt_call(c, FN(F_OBJ_ALIVE), 2, old, 0ull) & 1)) set_v3(c, F_T_SET_LOCAL_SCALE, rt_call(c, FN(F_GET_TRANSFORM), 2, old, 0ull), 1, 1, 1);
        rt_call(c, FN(F_GCHANDLE_FREE), 1, (uint64_t)g_press_handle);
    }
    g_press_handle = (uint32_t)rt_call(c, FN(F_GCHANDLE_NEW), 2, view, 0ull);
    g_press_t0 = GetTickCount64();
}
static void press_anim_tick(Cpu *c) {
    if (!g_press_handle) return;
    uint64_t v = rt_call(c, FN(F_GCHANDLE_TARGET), 1, (uint64_t)g_press_handle);
    float t = (float)(GetTickCount64() - g_press_t0) / PRESS_MS, k = t >= 1 ? 1.0f : 1.0f - 0.18f * 4 * t * (1 - t);
    if (v && (rt_call(c, FN(F_OBJ_ALIVE), 2, v, 0ull) & 1)) set_v3(c, F_T_SET_LOCAL_SCALE, rt_call(c, FN(F_GET_TRANSFORM), 2, v, 0ull), k, k, 1);
    if (t >= 1 || !v) { rt_call(c, FN(F_GCHANDLE_FREE), 1, (uint64_t)g_press_handle); g_press_handle = 0; }
}
void copy_button_tick(Cpu *c) {
    static int was_c;
    ui_mouse_frame();
    press_anim_tick(c);
    panel_layer_tick(c);
    if (!g_copy_btn_handle) return;
    uint64_t btn = rt_call(c, FN(F_GCHANDLE_TARGET), 1, (uint64_t)g_copy_btn_handle);
    uint64_t pv = rt_call(c, FN(F_GCHANDLE_TARGET), 1, (uint64_t)g_copy_view_handle);
    if (!btn || !pv || !(rt_call(c, FN(F_OBJ_ALIVE), 2, btn, 0ull) & 1)) {         // the lobby is gone
        rt_call(c, FN(F_GCHANDLE_FREE), 1, (uint64_t)g_copy_btn_handle);
        rt_call(c, FN(F_GCHANDLE_FREE), 1, (uint64_t)g_copy_view_handle);
        g_copy_btn_handle = g_copy_view_handle = 0;
        return;
    }
    int ckey = (GetAsyncKeyState(VK_CONTROL) & 0x8000) && (GetAsyncKeyState('C') & 0x8000) && window_focused(), key = ckey && !was_c;
    was_c = ckey;
    if (!key) return;                                                 // a mouse click arrives as the button's own event
    if (!(rt_call(c, FN(F_GO_ACTIVE_IN_HIER), 2, rt_call(c, FN(F_GET_GAMEOBJECT), 2, btn, 0ull), 0ull) & 1)) return;
    rt_ui_press_anim(c, btn);
    copy_code(pv);
}

// The online display name on PC. The Switch takes it from the console's user account; here the player sets it in the
// game: a small square button in the bottom-left button stack of the play-style screen and of the Adventure and
// Battle lobbies (L, or a mouse click; in the Battle lobby the mouse only, every controller button the game can
// draw a glyph for is taken there) opens a text box, "Set your online display name!". The name is saved in <exe dir>\port_name.txt and,
// in a lobby, sent to everyone at once (the game's own peer-info update).
//
// How it hangs together:
//   * Every screen polls its buttons through a UIUserInputDetector. When a screen registers a button we know (the
//     hook on SetIndependentButtonAction), a copy of one of its square buttons is made, given the name icon and
//     registered with the same detector under an input of its own; the game then draws the right button glyph on it
//     and reports the press from MonitorInputOnUpdate, where the second hook picks it up. The game only polls the
//     detector while the screen takes input, so dialogs and transitions block the button without further checks.
//   * The text box is the game's own system dialog (GameStateManager.OpenSystemDialog), whose text is rewritten as
//     the player types. Characters come from the window's WM_CHAR messages. Enter (or A) confirms, Esc cancels.
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "rt.h"

#define FN(off) (GUEST_BASE + (off))
#define F_STRING_NEW_UTF16 0x45af94ull      // il2cpp::vm::String::NewUtf16(const Il2CppChar *, int32_t)
#define F_INIT_METADATA 0x4506d0ull         // il2cpp_codegen_initialize_runtime_metadata(uintptr_t *slot)
#define F_OBJECT_NEW 0x450aa0ull
#define F_SZARRAY_NEW 0x450830ull
#define F_GCHANDLE_NEW 0x469da0ull          // il2cpp_gchandle_new(obj, pinned)
#define F_GCHANDLE_TARGET 0x469df0ull
#define F_GCHANDLE_FREE 0x469e10ull
#define F_GET_GAMEOBJECT 0x3f07fb0ull       // UnityEngine.Component::get_gameObject
#define F_GET_TRANSFORM 0x3f07f60ull        // UnityEngine.Component::get_transform
#define F_SET_ACTIVE 0x3f0bd10ull           // UnityEngine.GameObject::SetActive(bool)
#define F_GO_ACTIVE_SELF 0x3f0bd80ull
#define F_GO_GET_COMPONENT 0x246cfb0ull     // GameObject.GetComponent<T>() (shared; MethodInfo selects T)
#define F_GO_GET_COMPONENT_NAME 0x3f0adf0ull   // GameObject.GetComponent(string type)
#define F_OBJ_NAME 0x3f10820ull             // UnityEngine.Object::get_name
#define F_OBJ_ALIVE 0x3f10610ull            // UnityEngine.Object::op_Implicit (static)
#define F_OBJ_SET_HIDE_FLAGS 0x3f120d0ull
#define F_INSTANTIATE 0x3f11860ull          // Object.Instantiate(Object original, Transform parent, bool worldSpace) (static)
#define F_T_GET_PARENT 0x3f1a0a0ull
#define F_CHILD_COUNT 0x3f1e1f0ull
#define F_GET_CHILD 0x3f1ea10ull
#define F_RT_GET_ANCHORED_POS 0x3f182c0ull
#define F_RT_SET_ANCHORED_POS 0x3f18390ull
#define F_RT_SET_SIZE_DELTA 0x3f18530ull
#define F_RT_SET_ANCHOR_MIN 0x3f18050ull
#define F_RT_SET_ANCHOR_MAX 0x3f181f0ull
#define F_RT_SET_PIVOT 0x3f186d0ull
#define F_BUTTON_SET_TEXT 0x45117c0ull      // UIPartViewSelectableButton.SetText(string)
#define F_LAYER_CURRENT 0x43c2be0ull       // AppInput.Layer.current (static)
#define F_GO_FIND 0x3f0c5a0ull              // GameObject.Find(string) (static)
#define F_RETURN_BTN_SHOW 0x45128e0ull      // UIViewStandingReturnButton.PlayInAnimAndActivate(float normalizedTime, int controllerIdx)
#define RETURN_BTN_STATE 0x4c               // UIViewStandingReturnButton.m_DisplayState: 0 hidden, 1 shown
#define DET_LAYER 0x28                      // UIUserInputDetector.targetLayer
#define BTN_GUIDE 0x50                      // UIPartViewButtonBase.m_ButtonGuideView
#define F_TEX_CTOR 0x3ee90c0ull             // Texture2D..ctor(int, int)
#define F_LOAD_IMAGE 0x3f534d0ull           // ImageConversion.LoadImage(Texture2D, byte[])
#define F_SPRITE_CREATE 0x3f22180ull        // Sprite.Create(Texture2D, Rect, Vector2 pivot)
#define F_IMAGE_SET_SPRITE 0x3fdbad0ull
#define F_SET_BUTTON_ACTION 0x457b1f0ull    // UIUserInputDetector.SetIndependentButtonAction(input, button, onPointerEnter)
#define F_ADD_ANY_INPUT 0x457b7c0ull        // UIUserInputDetector.AddAnyUserInputList(input)
#define F_OPEN_SYSTEM_DIALOG 0x44e4770ull   // GameStateManager.OpenSystemDialog(string) (static) -> SubGameStateHandle
#define F_SET_DIALOG_TEXT 0x4510f70ull      // UIViewDialog.SetDialogText(string)
#define F_UPDATE_LOCAL_PEER 0x46964f0ull    // MatchingPeersInfo.UpdateLocalPeerInfo() (static)
#define M_CLASS_TEXTURE2D 0x6121658ull
#define M_CLASS_BYTE_ARR 0x6120328ull
#define M_METHOD_GO_GET_IMAGE 0x6145ac0ull  // GameObject.GetComponent<UnityEngine.UI.Image>
#define STR_LEN 0x10
#define STR_CHARS 0x14
#define ARR_DATA 0x20
#define DET_USER_INPUT 0x38                 // UIUserInputDetector.m_UserInput
#define HANDLE_IS_END 0x10                  // SubGameStateHandle.isEnd
#define INPUT_COMM_CUSTOMIZE 53             // eUserInput.CommCustomize: drawn and read as L
#define INPUT_NONE (-1)                     // no controller button: the button is clicked with the mouse
#define SLOT 104.0f                         // distance between two buttons of the stack, in UI units
#define NAME_MAX 16                         // characters

extern void rt_text_entry_keepalive(void);  // online_ui.c
extern int rt_ui_clicked(Cpu *c, uint64_t view);
extern void rt_ui_press_anim(Cpu *c, uint64_t view);
extern void rt_input_inject_a(int ms);      // imports_hid.c

// ------------------------------------------------------------------------------------------------ the name
static char g_name[33];
static int g_name_loaded;
static void name_path(char *out, size_t n) {
    const char *prof = getenv("BR_PROFILE");
    snprintf(out, n, "%sport_name%s.txt", rt_user_dir(), prof ? prof : "");
}
// The player's name, UTF-8, at most 32 bytes: the saved one, else BR_NICKNAME, else "Player".
const char *rt_player_name(void) {
    if (!g_name_loaded) {
        char path[MAX_PATH + 32], line[64] = "";
        const char *env = getenv("BR_NICKNAME");
        name_path(path, sizeof path);
        FILE *f = fopen(path, "r");
        if (f) { if (!fgets(line, sizeof line, f)) line[0] = 0; fclose(f); }
        line[strcspn(line, "\r\n")] = 0;
        snprintf(g_name, sizeof g_name, "%s", line[0] ? line : env && *env ? env : "Player");
        g_name_loaded = 1;
    }
    return g_name;
}
static void name_save(const char *utf8) {
    char path[MAX_PATH + 32];
    snprintf(g_name, sizeof g_name, "%s", utf8);
    g_name_loaded = 1;
    name_path(path, sizeof path);
    FILE *f = fopen(path, "w");
    if (f) { fprintf(f, "%s\n", g_name); fclose(f); }
}

// ------------------------------------------------------------------------------------------------ helpers
static uint64_t new_str16(Cpu *c, const uint16_t *s, int n) { return rt_call(c, FN(F_STRING_NEW_UTF16), 2, (uint64_t)(uintptr_t)s, (uint64_t)n); }
static uint64_t new_str(Cpu *c, const char *s) {
    uint16_t w[128];
    int n = 0;
    for (; s[n] && n < 127; n++) w[n] = (uint8_t)s[n];
    return new_str16(c, w, n);
}
static uint64_t class_of_slot(Cpu *c, uint64_t slot_ptr) {
    uint64_t p = LD64(FN(slot_ptr));
    if (LD64(p) & 1) rt_call(c, FN(F_INIT_METADATA), 1, p);
    return LD64(p);
}
static uint64_t target(Cpu *c, uint32_t h) { return h ? rt_call(c, FN(F_GCHANDLE_TARGET), 1, (uint64_t)h) : 0; }
static int alive(Cpu *c, uint64_t o) { return o && (rt_call(c, FN(F_OBJ_ALIVE), 2, o, 0ull) & 1); }
static void drop(Cpu *c, uint32_t *h) { if (*h) rt_call(c, FN(F_GCHANDLE_FREE), 1, (uint64_t)*h); *h = 0; }
static int name_is(Cpu *c, uint64_t component, const char *name) {
    uint64_t s = rt_call(c, FN(F_OBJ_NAME), 2, rt_call(c, FN(F_GET_GAMEOBJECT), 2, component, 0ull), 0ull);
    int len = s ? (int)LD32(s + STR_LEN) : 0, k = 0;
    for (; k < len && name[k] && LD16(s + STR_CHARS + 2 * (uint64_t)k) == (uint8_t)name[k]; k++) {}
    return k == len && !name[k];
}
static uint64_t child_named(Cpu *c, uint64_t t, const char *name) {
    int n = t ? (int)rt_call(c, FN(F_CHILD_COUNT), 2, t, 0ull) : 0;
    for (int i = 0; i < n; i++) {
        uint64_t ch = rt_call(c, FN(F_GET_CHILD), 3, t, (uint64_t)i, 0ull);
        if (name_is(c, ch, name)) return ch;
    }
    return 0;
}
static uint64_t find_named(Cpu *c, uint64_t t, const char *name, int depth) {   // first descendant with that name
    int n = t ? (int)rt_call(c, FN(F_CHILD_COUNT), 2, t, 0ull) : 0;
    for (int i = 0; i < n; i++) {
        uint64_t ch = rt_call(c, FN(F_GET_CHILD), 3, t, (uint64_t)i, 0ull), hit = name_is(c, ch, name) ? ch : depth > 0 ? find_named(c, ch, name, depth - 1) : 0;
        if (hit) return hit;
    }
    return 0;
}
static void get_v2(Cpu *c, uint64_t fn, uint64_t o, float out[2]) {
    rt_call(c, FN(fn), 2, o, 0ull);
    out[0] = rt_getf(c, 0); out[1] = rt_getf(c, 1);
}
static void set_v2(Cpu *c, uint64_t fn, uint64_t o, float x, float y) {
    c->x[0] = o; c->x[1] = 0;
    rt_setf(c, 0, x); rt_setf(c, 1, y);
    rt_call_regs(c, FN(fn));
}
// A sprite from PNG data, made once and kept (see copy_sprite in online_ui.c for the hide flags).
static uint64_t png_sprite(Cpu *c, uint32_t *handle, const uint8_t *png, size_t size, int w, int h) {
    uint64_t old = target(c, *handle);
    if (alive(c, old)) return old;
    drop(c, handle);
    uint64_t tex = rt_call(c, FN(F_OBJECT_NEW), 1, class_of_slot(c, M_CLASS_TEXTURE2D));
    rt_call(c, FN(F_TEX_CTOR), 4, tex, 2ull, 2ull, 0ull);
    uint64_t bytes = rt_call(c, FN(F_SZARRAY_NEW), 2, class_of_slot(c, M_CLASS_BYTE_ARR), (uint64_t)size);
    memcpy((void *)(uintptr_t)(bytes + ARR_DATA), png, size);
    if (!(rt_call(c, FN(F_LOAD_IMAGE), 3, tex, bytes, 0ull) & 1)) return 0;
    c->x[0] = tex; c->x[1] = 0;
    rt_setf(c, 0, 0); rt_setf(c, 1, 0); rt_setf(c, 2, (float)w); rt_setf(c, 3, (float)h); rt_setf(c, 4, 0.5f); rt_setf(c, 5, 0.5f);
    rt_call_regs(c, FN(F_SPRITE_CREATE));
    uint64_t sprite = c->x[0];
    if (!sprite) return 0;
    *handle = (uint32_t)rt_call(c, FN(F_GCHANDLE_NEW), 2, sprite, 0ull);
    rt_call(c, FN(F_OBJ_SET_HIDE_FLAGS), 3, tex, 32ull, 0ull);     // HideFlags.DontUnloadUnusedAsset
    rt_call(c, FN(F_OBJ_SET_HIDE_FLAGS), 3, sprite, 32ull, 0ull);
    return sprite;
}
static const uint8_t k_badge_png[] = {
#include "icons/name_badge_white.inc"
};
static const uint8_t k_button_png[] = {
#include "icons/name_button.inc"
};

// ------------------------------------------------------------------------------------------------ the button
// One name button at a time (the screen that registered last): the detector it is registered with, the button, the
// button it is stacked on ("below") and the button whose visibility it follows.
static uint32_t g_det_h, g_btn_h, g_below_h, g_follow_h;
static int g_input, g_log = -1;
static uint64_t g_live_tick;                 // last time the button's screen was taking input
// 1 while the screen with the name button (a lobby, the play-style screen) is the one taking input: not during a
// dialog, an overlay screen or a fade.
int rt_name_screen_live(void) { return g_btn_h && GetTickCount64() - g_live_tick < 150; }
static void forget_button(Cpu *c) { drop(c, &g_det_h); drop(c, &g_btn_h); drop(c, &g_below_h); drop(c, &g_follow_h); }

// `registered` is a button the screen has just registered with `det`. The new button is a copy of the sibling named
// `below` (or of `registered` itself) and goes one slot above it.
static void make_button(Cpu *c, uint64_t det, uint64_t registered, const char *below, int input) {
    static __thread int inside;
    static uint32_t badge_h, button_h;
    if (inside) return;
    if (target(c, g_det_h) == det && alive(c, target(c, g_btn_h))) return;       // this screen has one
    uint64_t rt = rt_call(c, FN(F_GET_TRANSFORM), 2, registered, 0ull), parent = rt_call(c, FN(F_T_GET_PARENT), 2, rt, 0ull);
    uint64_t bt = below ? child_named(c, parent, below) : rt;
    uint64_t src = bt ? rt_call(c, FN(F_GO_GET_COMPONENT_NAME), 3, rt_call(c, FN(F_GET_GAMEOBJECT), 2, bt, 0ull), new_str(c, "UIPartViewIndependentButton"), 0ull) : 0;
    if (!src) {                                                     // e.g. the main menu's own "Change Character" button
        if (g_log) rt_log("name ui: no button to copy (%s: transform %d)", below ? below : "the registered one", bt != 0);
        return;
    }
    uint64_t btn = rt_call(c, FN(F_INSTANTIATE), 4, src, parent, 0ull, 0ull);
    if (!btn) return;
    uint64_t go = rt_call(c, FN(F_GET_GAMEOBJECT), 2, btn, 0ull), t = rt_call(c, FN(F_GET_TRANSFORM), 2, btn, 0ull);
    rt_call(c, FN(F_SET_ACTIVE), 3, go, 1ull, 0ull);
    // The picture: where the button's icon is a part of its own, swap it for the name badge; where the icon is
    // painted into the box, swap the whole box.
    uint64_t bg = child_named(c, t, "ImageBackGround"), icon = child_named(c, bg, "Icon");
    if (!icon) icon = child_named(c, bg, "Image_Icon");
    uint64_t part = icon ? icon : bg;
    uint64_t img = part ? rt_call(c, FN(F_GO_GET_COMPONENT), 2, rt_call(c, FN(F_GET_GAMEOBJECT), 2, part, 0ull), class_of_slot(c, M_METHOD_GO_GET_IMAGE)) : 0;
    uint64_t sprite = icon ? png_sprite(c, &badge_h, k_badge_png, sizeof k_badge_png, 96, 96)
                           : png_sprite(c, &button_h, k_button_png, sizeof k_button_png, 112, 112);
    if (img && sprite) {
        rt_call(c, FN(F_IMAGE_SET_SPRITE), 3, img, sprite, 0ull);
        if (icon) set_v2(c, F_RT_SET_SIZE_DELTA, icon, 60, 60);
    } else rt_log("name ui: the button's picture was not set (image %d, sprite %d)", img != 0, sprite != 0);
    // The glyph: the game draws it on the button's guide part. A button whose guide is a fixed text (the kick button's
    // "-/+") has none; it gets a copy of the registered button's guide at its bottom right corner instead.
    if (!LD64(btn + BTN_GUIDE) && LD64(registered + BTN_GUIDE) && bg) {
        uint64_t guide = rt_call(c, FN(F_INSTANTIATE), 4, LD64(registered + BTN_GUIDE), bg, 0ull, 0ull);
        uint64_t gt = guide ? rt_call(c, FN(F_GET_TRANSFORM), 2, guide, 0ull) : 0;
        int n = (int)rt_call(c, FN(F_CHILD_COUNT), 2, bg, 0ull);
        for (int i = 0; i < n; i++) {                                // the fixed text guide goes
            uint64_t ch = rt_call(c, FN(F_GET_CHILD), 3, bg, (uint64_t)i, 0ull);
            if (ch != gt && ch != icon) rt_call(c, FN(F_SET_ACTIVE), 3, rt_call(c, FN(F_GET_GAMEOBJECT), 2, ch, 0ull), 0ull, 0ull);
        }
        if (gt) {
            set_v2(c, F_RT_SET_ANCHOR_MIN, gt, 1, 0); set_v2(c, F_RT_SET_ANCHOR_MAX, gt, 1, 0); set_v2(c, F_RT_SET_PIVOT, gt, 0.5f, 0.5f);
            set_v2(c, F_RT_SET_ANCHORED_POS, gt, 4, 17);
            rt_call(c, FN(F_SET_ACTIVE), 3, rt_call(c, FN(F_GET_GAMEOBJECT), 2, guide, 0ull), 1ull, 0ull);
            rt_set_ref(c, btn + BTN_GUIDE, guide);
        }
    }
    if (input == INPUT_NONE) {                                      // no glyph
        if (LD64(btn + BTN_GUIDE)) rt_call(c, FN(F_SET_ACTIVE), 3, rt_call(c, FN(F_GET_GAMEOBJECT), 2, LD64(btn + BTN_GUIDE), 0ull), 0ull, 0ull);
    } else {
        inside = 1;
        rt_call(c, FN(F_SET_BUTTON_ACTION), 5, det, (uint64_t)input, btn, 0ull, 0ull);   // glyph of `input` on the button
        rt_call(c, FN(F_ADD_ANY_INPUT), 3, det, (uint64_t)input, 0ull);                   // and the detector reports it
        inside = 0;
    }
    forget_button(c);
    g_det_h = (uint32_t)rt_call(c, FN(F_GCHANDLE_NEW), 2, det, 0ull);
    g_btn_h = (uint32_t)rt_call(c, FN(F_GCHANDLE_NEW), 2, btn, 0ull);
    g_below_h = (uint32_t)rt_call(c, FN(F_GCHANDLE_NEW), 2, bt, 0ull);
    g_follow_h = (uint32_t)rt_call(c, FN(F_GCHANDLE_NEW), 2, registered, 0ull);
    g_input = input;
    rt_log("name ui: name button added above %s (input %d)", below ? below : "the sub-menu button", input);
}
// UIUserInputDetector.SetIndependentButtonAction(eUserInput, UIPartViewIndependentButton, onPointerEnter): every
// button a screen registers passes here. BR_NAME_LOG=1 logs them.
int rt_hook_457b1f0(Cpu *c) {
    uint64_t a[9], lr = c->x[30], det = X(0), btn = X(2);
    if (g_log < 0) g_log = getenv("BR_NAME_LOG") != NULL;
    if (!btn) return 0;
    for (int i = 0; i < 9; i++) a[i] = c->x[i];
    if (g_log) {
        char name[80] = "";
        uint64_t s = rt_call(c, FN(F_OBJ_NAME), 2, rt_call(c, FN(F_GET_GAMEOBJECT), 2, btn, 0ull), 0ull);
        for (int i = 0; s && i < 79 && i < (int)LD32(s + STR_LEN); i++) { name[i] = (char)LD16(s + STR_CHARS + 2 * (uint64_t)i); name[i + 1] = 0; }
        rt_log("name ui: detector %llx registers input %d on %s", (unsigned long long)det, (int)(uint32_t)a[1], name);
    }
    if (name_is(c, btn, "ui_PartView_SelectPlayStyle_Side_SelectableButton_Sub")) make_button(c, det, btn, NULL, INPUT_COMM_CUSTOMIZE);
    else if (name_is(c, btn, "ui_PartView_Info_IndependentButton_CharaChange")) make_button(c, det, btn, "ui_PartView_IndependentButton_PickKick", INPUT_COMM_CUSTOMIZE);
    else if (name_is(c, btn, "ui_PartView_Info_IndependentButton_CharaSelect"))
        make_button(c, det, btn, "UI_PartView_IndependentButton_CommCustomize", INPUT_NONE);
    for (int i = 0; i < 9; i++) c->x[i] = a[i];
    c->x[30] = lr;
    return 0;
}

// ------------------------------------------------------------------------------------------------ the text box
static struct {
    int open, cancelled, len, expect_view, caret;
    uint16_t text[NAME_MAX + 1];
    uint32_t handle_h, view_h, return_h;     // return_h: the Back button view, when it was showing as the box opened
    uint64_t closed_tick;
} g_box;
static uint16_t g_chars[64];                 // typed characters (WM_CHAR), window thread -> game thread
static volatile LONG g_chars_w, g_chars_r;
static WNDPROC g_old_proc;
static void push_char(uint16_t ch) { g_chars[(uint32_t)g_chars_w % 64] = ch; InterlockedIncrement(&g_chars_w); }
static LRESULT CALLBACK name_proc(HWND h, UINT m, WPARAM w, LPARAM l) {
    if (m == WM_CHAR && g_box.open) push_char((uint16_t)w);
    return CallWindowProcW(g_old_proc, h, m, w, l);
}
static BOOL CALLBACK main_window(HWND h, LPARAM out) {
    if (IsWindowVisible(h) && !GetWindow(h, GW_OWNER)) { *(HWND *)out = h; return FALSE; }
    return TRUE;
}
void name_ui_type(const char *s) {           // BR_INPUT_LIVE "TYPE <text>": '<' = Backspace, '!' = Esc
    if (!g_box.open) return;
    for (; *s; s++) push_char(*s == '<' ? 8 : *s == '!' ? 27 : (uint16_t)(uint8_t)*s);
}
static uint64_t box_text(Cpu *c) {
    static const wchar_t head[] = L"\n<size=135%><b>";
    static const char caret_on[] = "_", caret_off[] = "<color=#00000000>_</color>";
    static const wchar_t tail[] = L"</b></size>\n<size=62%>", tail2[] = L"</size>";
    extern int rt_tr_utf16(Cpu *c, const char *english, uint16_t *out, int max);      // port_text.c
    uint16_t w[400];
    int n = rt_tr_utf16(c, "Set your online display name!", w, 100);
    for (const wchar_t *p = head; *p; p++) w[n++] = (uint16_t)*p;
    for (int i = 0; i < g_box.len; i++) w[n++] = g_box.text[i];
    for (const char *p = g_box.caret ? caret_on : caret_off; *p; p++) w[n++] = (uint8_t)*p;
    for (const wchar_t *p = tail; *p; p++) w[n++] = (uint16_t)*p;
    n += rt_tr_utf16(c, "Enter: OK      Esc: Cancel", w + n, 100);
    for (const wchar_t *p = tail2; *p; p++) w[n++] = (uint16_t)*p;
    return new_str16(c, w, n);
}
static void box_open(Cpu *c) {
    if (g_box.open) return;
    if (!g_old_proc) {                                              // typed characters: the window's WM_CHAR
        HWND w = NULL;
        EnumThreadWindows(GetCurrentThreadId(), main_window, (LPARAM)&w);
        if (w) g_old_proc = (WNDPROC)SetWindowLongPtrW(w, GWLP_WNDPROC, (LONG_PTR)name_proc);
        if (!g_old_proc) rt_log("name ui: the window's messages could not be read (window %d); typing will not work", w != NULL);
    }
    memset(&g_box, 0, sizeof g_box);
    g_box.len = MultiByteToWideChar(CP_UTF8, 0, rt_player_name(), -1, (LPWSTR)g_box.text, NAME_MAX + 1);
    g_box.len = g_box.len > 0 ? g_box.len - 1 : 0;                  // the count includes the terminator; 0 = did not fit
    if (g_box.len > NAME_MAX) g_box.len = NAME_MAX;
    g_chars_r = g_chars_w;
    g_box.caret = 1;
    // The system dialog takes the standing Back button away and nothing brings it back (the screen underneath is not
    // re-entered as it is after the game's own dialogs): remember it and show it again after the box (box_tick).
    uint64_t rgo = rt_call(c, FN(F_GO_FIND), 2, new_str(c, "ui_View_System_ReturnButton"), 0ull);
    uint64_t rv = rgo ? rt_call(c, FN(F_GO_GET_COMPONENT_NAME), 3, rgo, new_str(c, "UIViewStandingReturnButton"), 0ull) : 0;
    if (rv && LD32(rv + RETURN_BTN_STATE) == 1) g_box.return_h = (uint32_t)rt_call(c, FN(F_GCHANDLE_NEW), 2, rv, 0ull);
    g_box.expect_view = 1;
    uint64_t handle = rt_call(c, FN(F_OPEN_SYSTEM_DIALOG), 2, box_text(c), 0ull);
    g_box.expect_view = 0;
    if (!handle) { rt_log("name ui: the dialog could not be opened"); return; }
    g_box.handle_h = (uint32_t)rt_call(c, FN(F_GCHANDLE_NEW), 2, handle, 0ull);
    g_box.open = 1;
    uint64_t view = target(c, g_box.view_h), ok = view ? find_named(c, rt_call(c, FN(F_GET_TRANSFORM), 2, view, 0ull), "ui_PartView_Dialog_SelectableButton", 6) : 0;
    uint64_t part = ok ? rt_call(c, FN(F_GO_GET_COMPONENT_NAME), 3, rt_call(c, FN(F_GET_GAMEOBJECT), 2, ok, 0ull), new_str(c, "UIPartViewSelectableButton"), 0ull) : 0;
    if (part) rt_call(c, FN(F_BUTTON_SET_TEXT), 3, part, new_str(c, "OK"), 0ull);   // the dialog's own caption is "Yes"
    rt_log("name ui: text box opened (dialog view %s)", g_box.view_h ? "found" : "NOT found");
}
// UIViewDialog.SetDialogText(string): remembers the dialog view our text box is shown in.
int rt_hook_4510f70(Cpu *c) {
    if (!g_box.expect_view || g_box.view_h) return 0;
    uint64_t a0 = X(0), a1 = X(1), a2 = X(2), lr = c->x[30];
    g_box.view_h = (uint32_t)rt_call(c, FN(F_GCHANDLE_NEW), 2, a0, 0ull);
    c->x[0] = a0; c->x[1] = a1; c->x[2] = a2; c->x[30] = lr;
    return 0;
}
static void box_close(Cpu *c) {
    char utf8[80] = "";
    int n = g_box.len;
    while (n > 0 && g_box.text[n - 1] == ' ') n--;
    int start = 0;
    while (start < n && g_box.text[start] == ' ') start++;
    for (; n > start; n--) {                                        // at most 32 bytes (the account name's size)
        int bytes = WideCharToMultiByte(CP_UTF8, 0, (LPCWSTR)g_box.text + start, n - start, utf8, sizeof utf8 - 1, NULL, NULL);
        utf8[bytes > 0 ? bytes : 0] = 0;
        if (bytes > 0 && bytes <= 32) break;
    }
    drop(c, &g_box.handle_h); drop(c, &g_box.view_h);
    g_box.open = 0;
    g_box.closed_tick = GetTickCount64();
    if (g_box.cancelled || n <= start || !strcmp(utf8, rt_player_name())) { rt_log("name ui: text box closed, name unchanged"); return; }
    name_save(utf8);
    rt_log("name ui: name set to \"%s\"", utf8);
    rt_call(c, FN(F_UPDATE_LOCAL_PEER), 1, 0ull);                   // in a room: new peer info, sent to everyone
}
static void box_tick(Cpu *c) {
    uint64_t handle = target(c, g_box.handle_h), view = target(c, g_box.view_h);
    int changed = 0, caret = (GetTickCount64() / 500) & 1;
    rt_text_entry_keepalive();                                       // letters do not act as controller buttons
    if (!handle || (LD32(handle + HANDLE_IS_END) & 0xff)) { box_close(c); return; }
    while (g_chars_r != g_chars_w) {
        uint16_t ch = g_chars[(uint32_t)g_chars_r % 64];
        InterlockedIncrement(&g_chars_r);
        if (g_box.cancelled) continue;
        if (ch == 27) { g_box.cancelled = 1; rt_input_inject_a(120); }           // Esc: close without saving
        else if (ch == 8) { if (g_box.len > 0) { g_box.len--; changed = 1; } }
        else if (ch == 22) {                                                      // Ctrl+V
            if (OpenClipboard(NULL)) {
                HANDLE h = GetClipboardData(CF_UNICODETEXT);
                const uint16_t *s = h ? GlobalLock(h) : NULL;
                for (int i = 0; s && s[i] && i < 64; i++)
                    if (s[i] >= 32 && s[i] != '<' && s[i] != '>' && g_box.len < NAME_MAX) { g_box.text[g_box.len++] = s[i]; changed = 1; }
                if (s) GlobalUnlock(h);
                CloseClipboard();
            }
        } else if (ch >= 32 && ch != 127 && ch != '<' && ch != '>' && g_box.len < NAME_MAX) { g_box.text[g_box.len++] = ch; changed = 1; }
    }
    if ((changed || caret != g_box.caret) && alive(c, view)) {
        g_box.caret = caret;
        rt_call(c, FN(F_SET_DIALOG_TEXT), 3, view, box_text(c), 0ull);
    }
}

// After UIUserInputDetector.MonitorInputOnUpdate() (the hook is in pc_input.c): r is the input the screen sees this
// frame. Ours opens the text box and is taken away from the screen.
uint64_t name_ui_after_monitor(Cpu *c, uint64_t det, uint64_t r) {
    if (!g_det_h || det != target(c, g_det_h)) return r;
    uint64_t btn = target(c, g_btn_h);
    if ((uint32_t)rt_call(c, FN(F_LAYER_CURRENT), 1, 0ull) == LD32(det + DET_LAYER)) g_live_tick = GetTickCount64();
    int hit = g_input != INPUT_NONE && (int)(uint32_t)r == g_input;
    if (!hit && (uint32_t)r == 0 && g_input == INPUT_NONE && rt_ui_clicked(c, btn)) hit = 1;   // the button without a glyph: mouse only
    if (hit && !g_box.open) {
        ST32(det + DET_USER_INPUT, 0);
        r = 0;
        if (alive(c, btn)) rt_ui_press_anim(c, btn);
        box_open(c);
    }
    return r;
}

// Once a frame: keep the button in its place (one slot above the button below it, or in that button's place while
// it is hidden) and as visible as the button it follows; run the text box.
void name_ui_tick(Cpu *c) {
    if (g_box.open) box_tick(c);
    else if (g_box.return_h && GetTickCount64() - g_box.closed_tick > 250) {     // the dialog is gone: Back button again
        uint64_t rv = target(c, g_box.return_h);
        if (alive(c, rv) && LD32(rv + RETURN_BTN_STATE) == 0) {
            c->x[0] = rv; c->x[1] = 0; c->x[2] = 0;
            rt_setf(c, 0, 0.0f);
            rt_call_regs(c, FN(F_RETURN_BTN_SHOW));
        }
        drop(c, &g_box.return_h);
    }
    if (!g_btn_h) return;
    uint64_t btn = target(c, g_btn_h), below = target(c, g_below_h), follow = target(c, g_follow_h);
    if (!alive(c, btn) || !alive(c, below) || !alive(c, follow)) { forget_button(c); return; }
    float pos[2];
    uint64_t go = rt_call(c, FN(F_GET_GAMEOBJECT), 2, btn, 0ull);
    int below_on = (int)(rt_call(c, FN(F_GO_ACTIVE_SELF), 2, rt_call(c, FN(F_GET_GAMEOBJECT), 2, below, 0ull), 0ull) & 1);
    int want = (int)(rt_call(c, FN(F_GO_ACTIVE_SELF), 2, rt_call(c, FN(F_GET_GAMEOBJECT), 2, follow, 0ull), 0ull) & 1);
    if (want != (int)(rt_call(c, FN(F_GO_ACTIVE_SELF), 2, go, 0ull) & 1)) rt_call(c, FN(F_SET_ACTIVE), 3, go, (uint64_t)want, 0ull);
    get_v2(c, F_RT_GET_ANCHORED_POS, below, pos);
    set_v2(c, F_RT_SET_ANCHORED_POS, rt_call(c, FN(F_GET_TRANSFORM), 2, btn, 0ull), pos[0], pos[1] + (below_on ? SLOT : 0));
}

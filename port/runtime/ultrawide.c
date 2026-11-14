// Screens wider than 16:9 (21:9, 32:9) and taller than 16:9 (16:10, e.g. the Steam Deck's 1280x800).
//
// The game's UI is authored for 16:9. Every UI canvas (reference 1920x1080, scale mode Expand) has one child,
// "SafeMargin", that parents all of its UI. In the menus the game keeps SafeMargin at 1920x1080 in the middle of
// the screen and covers everything outside it with four black images (UISystemViewManager's "out of range black"
// view); in a stage it leaves SafeMargin as wide as the screen, so the HUD sits at the screen edges, and hides the
// black view. What the port changes on a screen that is not 16:9:
//   * the black images are switched off, so the 3D scene behind a menu fills the screen;
//   * full-screen childless UI elements named like a backdrop (flat backgrounds, dimming layers, the blurred menu
//     backdrop) are resized from 1920x1080 to the canvas size (wider on a wide screen, higher on a tall one), and
//     that blurred backdrop is rendered at the screen's aspect ratio;
//   * on a wide screen the in-game HUD canvases get the menus' centred 1920x1080 SafeMargin (single-player view
//     only; with split screen the game's own per-view layout is kept). On a tall screen the HUD keeps the game's
//     layout, which reaches the top and bottom edges.
//   * cameras that use physical properties with a horizontal gate fit (the main menu's 3D scene) are switched to a
//     vertical fit on a wide screen. With a horizontal fit the width of the view is fixed and a wider screen
//     cuts off top and bottom, which zooms in on the characters; with a vertical fit the view keeps its 16:9
//     height and gains width.
// Nothing changes at 16:9, and everything follows a resolution change at once.
//
// Diagnostics. BR_SCREEN_DUMP=<seconds>[,<seconds>...]: at those times after the first frame, log every camera
// (rect, pixel rect, target texture, depth) and every canvas that has a CanvasScaler (render mode, camera, scaler
// settings, pixel rect). BR_SCREEN_TREE=<depth> adds the active objects under each canvas with their pixel boxes.
// With BR_SCREEN_DUMP set, every widened element is logged too. BR_SCREEN_RESIZE=<seconds>:<w>x<h>[,...] switches
// to a window of that size at those times, to test a resolution change without the menu.
//
// Layouts used (from the lifted code): CanvasScaler +0x20 scale mode, +0x2c reference resolution, +0x34 screen
// match mode, +0x38 match, +0x50 Canvas. UIRootCanvasCtrl +0x20 CanvasScaler, +0x28 SafeMargin RectTransform.
// UISystemViewManager +0x38 the black view's GameObject (children: top, bottom, left, right image).
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "rt.h"

#define FN(off) (GUEST_BASE + (off))
#define F_OBJ_ALIVE 0x3f10610ull           // UnityEngine.Object::op_Implicit (static)
#define F_OBJ_NAME 0x3f10820ull            // UnityEngine.Object::get_name
#define F_GET_GAMEOBJECT 0x3f07fb0ull      // UnityEngine.Component::get_gameObject
#define F_GET_TRANSFORM 0x3f07f60ull       // UnityEngine.Component::get_transform
#define F_GO_TRANSFORM 0x3f0bb40ull        // UnityEngine.GameObject::get_transform
#define F_ACTIVE_IN_HIER 0x3f0bdd0ull      // UnityEngine.GameObject::get_activeInHierarchy
#define F_SET_ACTIVE 0x3f0bd10ull          // UnityEngine.GameObject::SetActive(bool)
#define F_SCREEN_W 0x3ebcb40ull
#define F_SCREEN_H 0x3ebcb80ull
#define F_SET_RESOLUTION 0x3ebced0ull      // UnityEngine.Screen::SetResolution(int, int, FullScreenMode)
#define F_CAM_ALL 0x3eaf2f0ull             // UnityEngine.Camera::get_allCameras (static) -> Camera[]
#define F_CAM_RECT 0x3eac320ull            // Rect results come back in v0..v3
#define F_CAM_PIXEL_RECT 0x3eac4c0ull
#define F_CAM_TARGET 0x3eac7a0ull
#define F_CAM_DEPTH 0x3eaa430ull
#define F_CANVAS_RENDER_MODE 0x4167730ull
#define F_CANVAS_IS_ROOT 0x41677f0ull
#define F_CANVAS_PIXEL_RECT 0x4167840ull
#define F_CANVAS_WORLD_CAMERA 0x41686f0ull
#define F_CANVAS_SCALE_FACTOR 0x4167910ull
#define F_CHILD_COUNT 0x3f1e1f0ull         // UnityEngine.Transform
#define F_GET_CHILD 0x3f1ea10ull
#define F_T_PARENT 0x3f1a0a0ull
#define F_T_POSITION 0x3f1a1f0ull
#define F_T_LOSSY_SCALE 0x3f1e5a0ull
#define F_RT_RECT 0x3f17eb0ull             // UnityEngine.RectTransform
#define F_RT_GET_ANCHOR_MIN 0x3f17f80ull
#define F_RT_SET_ANCHOR_MIN 0x3f18050ull
#define F_RT_GET_ANCHOR_MAX 0x3f18120ull
#define F_RT_SET_ANCHOR_MAX 0x3f181f0ull
#define F_RT_GET_ANCHORED_POS 0x3f182c0ull
#define F_RT_SET_ANCHORED_POS 0x3f18390ull
#define F_RT_GET_SIZE_DELTA 0x3f18460ull
#define F_RT_SET_SIZE_DELTA 0x3f18530ull

static uint64_t g_scalers[64];             // BR_SCREEN_DUMP: the CanvasScalers handled this frame (rt_hook_4176dc0)
static int g_nscalers, g_dump_collect;
static uint64_t g_t0;

static int alive(Cpu *c, uint64_t o) { return o && (rt_call(c, FN(F_OBJ_ALIVE), 2, o, 0ull) & 1); }
static const char *name_of(Cpu *c, uint64_t o, char *buf, int cap) {   // System.String: length +0x10, UTF-16 +0x14
    snprintf(buf, cap, "(null)");
    if (!alive(c, o)) return buf;
    uint64_t s = rt_call(c, FN(F_OBJ_NAME), 2, o, 0ull);
    if (!s) return buf;
    int n = WideCharToMultiByte(CP_UTF8, 0, (const wchar_t *)(uintptr_t)(s + 0x14), (int)LD32(s + 0x10), buf, cap - 1, NULL, NULL);
    buf[n > 0 ? n : 0] = 0;
    return buf;
}
static const char *class_of(uint64_t o) { return (const char *)(uintptr_t)LD64(LD64(o) + 0x10); }   // Il2CppClass::name
static float vf(Cpu *c, int i) { float f; memcpy(&f, &c->v[i], sizeof f); return f; }
static void rect_of(Cpu *c, uint64_t fn, uint64_t o, float r[4]) {
    rt_call(c, FN(fn), 2, o, 0ull);
    for (int i = 0; i < 4; i++) r[i] = vf(c, i);
}
static void get_v2(Cpu *c, uint64_t fn, uint64_t o, float out[2]) {
    rt_call(c, FN(fn), 2, o, 0ull);
    out[0] = vf(c, 0); out[1] = vf(c, 1);
}
static void set_v2(Cpu *c, uint64_t fn, uint64_t o, float x, float y) {   // Vector2 argument: floats in v0, v1
    uint32_t bx, by;
    memcpy(&bx, &x, 4); memcpy(&by, &y, 4);
    c->x[0] = o; c->x[1] = 0; c->v[0] = bx; c->v[1] = by;
    rt_call_regs(c, FN(fn));
}

// ------------------------------------------------------------------------------------------- black side images
#define F_SET_BLACK_VIEW 0x452f4b0ull
#define F_SINGLETON_EXISTS 0x28671d0ull    // Framework.SingletonBase<T>::get_Exists (MethodInfo in x0)
#define F_SINGLETON_GET 0x2866fd0ull       // Framework.SingletonBase<T>::get_Instance (MethodInfo in x0)
#define SLOT_SYSVIEW_EXISTS 0x61714c0ull   // metadata slots SetActiveOutOfRangeBlackView passes to the two above
#define SLOT_SYSVIEW_GET 0x6174c58ull
static __thread int t_in_black;

// Wonder.UI.UISystemViewManager::SetActiveOutOfRangeBlackView(bool): the view is four black images around the 16:9
// area (top, bottom, left, right), which can only show on a screen that is not 16:9; switch them off for good.
int rt_hook_452f4b0(Cpu *c) {
    if (t_in_black) return 0;
    t_in_black = 1; c->pc = FN(F_SET_BLACK_VIEW); rt_dispatch(c); t_in_black = 0;
    if (c->eh_unwinding) return 1;
    if (!(rt_call(c, FN(F_SINGLETON_EXISTS), 1, LD64(LD64(FN(SLOT_SYSVIEW_EXISTS)))) & 1)) return 1;
    uint64_t mgr = rt_call(c, FN(F_SINGLETON_GET), 1, LD64(LD64(FN(SLOT_SYSVIEW_GET))));
    uint64_t view = mgr ? LD64(mgr + 0x38) : 0;
    if (!alive(c, view)) return 1;
    uint64_t t = rt_call(c, FN(F_GO_TRANSFORM), 2, view, 0ull);
    int n = (int)rt_call(c, FN(F_CHILD_COUNT), 2, t, 0ull);
    for (int i = 0; i < n && i < 4; i++) {
        uint64_t ch = rt_call(c, FN(F_GET_CHILD), 3, t, (uint64_t)i, 0ull);
        if (ch) rt_call(c, FN(F_SET_ACTIVE), 3, rt_call(c, FN(F_GET_GAMEOBJECT), 2, ch, 0ull), 0ull, 0ull);
    }
    return 1;
}

// No pointer to a managed object is kept between frames anywhere in this file: host memory is not scanned by the
// collector, so a remembered object can be freed and its memory reused (that crashed the render thread when the
// title scene was torn down). The work is done inside the game's own per-frame calls on the live object instead.
static int g_wide, g_tall;                 // this frame's screen is wider / taller than 16:9 (set by ultrawide_tick)
static float g_cur_w = 1920, g_cur_h = 1080, g_prev_w = 1920, g_prev_h = 1080;   // canvas size in canvas units now, and before the last change
static int near_f(float a, float b) { return a > b - 1 && a < b + 1; }
static int g_restore_frames;

// ------------------------------------------------------------------------------------------------ in-game HUD
// Wonder.UI.UIRootCanvasCtrl::Setup(bool useMargin): with useMargin the game sizes SafeMargin to 1920x1080 itself
// (the menu canvases, whose SafeMargin is anchored to the centre). UIManager_MainGame passes false for the HUD
// canvases, whose SafeMargin is stretched over the canvas; those are marked here, in the object's padding bytes
// (+0x59 "HUD canvas", +0x5a "re-anchored"; the last field is the bool at +0x58), and re-anchored from the
// object's own Update while the screen is wide. Other callers that pass false (the fade canvas) stay full screen.
#define MAINGAME_INIT_LO 0x46218d0ull      // Wonder.UI.MainGame.UIManager_MainGame+<initAsync>d__13::MoveNext
#define MAINGAME_INIT_HI 0x4621a10ull
#define F_VIEWPORT_TYPE 0x4443a80ull       // Wonder.Util+Camera::GetViewPortType (static): 1 = one full-screen view
static float g_hud_orig[4][2];             // SafeMargin's own anchors, position and size (the same in every HUD canvas)
static int g_hud_orig_set;

int rt_hook_451cc70(Cpu *c) {
    uint64_t ra = c->x[30] - GUEST_BASE;
    if (!(c->x[1] & 1) && ra >= MAINGAME_INIT_LO && ra < MAINGAME_INIT_HI) { ST8(c->x[0] + 0x59, 1); ST8(c->x[0] + 0x5a, 0); }
    return 0;
}

// Wonder.UI.UIRootCanvasCtrl::Update()
int rt_hook_451cd90(Cpu *c) {
    uint64_t ctrl = c->x[0];
    if (!LD8(ctrl + 0x59)) return 0;
    int applied = LD8(ctrl + 0x5a);
    if (!g_wide && !applied) return 0;
    uint64_t sx[31]; __uint128_t sv[8];
    memcpy(sx, c->x, sizeof sx); memcpy(sv, c->v, sizeof sv);
    uint64_t rt = LD64(ctrl + 0x28);
    if (alive(c, rt)) {
        int want = g_wide && (int)rt_call(c, FN(F_VIEWPORT_TYPE), 1, 0ull) == 1;
        if (want && !applied) {
            if (!g_hud_orig_set) {
                get_v2(c, F_RT_GET_ANCHOR_MIN, rt, g_hud_orig[0]); get_v2(c, F_RT_GET_ANCHOR_MAX, rt, g_hud_orig[1]);
                get_v2(c, F_RT_GET_ANCHORED_POS, rt, g_hud_orig[2]); get_v2(c, F_RT_GET_SIZE_DELTA, rt, g_hud_orig[3]);
                g_hud_orig_set = 1;
            }
            set_v2(c, F_RT_SET_ANCHOR_MIN, rt, 0.5f, 0.5f); set_v2(c, F_RT_SET_ANCHOR_MAX, rt, 0.5f, 0.5f);
            set_v2(c, F_RT_SET_ANCHORED_POS, rt, 0, 0); set_v2(c, F_RT_SET_SIZE_DELTA, rt, 1920, 1080);
            ST8(ctrl + 0x5a, 1);
        } else if (!want && applied) {
            set_v2(c, F_RT_SET_ANCHOR_MIN, rt, g_hud_orig[0][0], g_hud_orig[0][1]); set_v2(c, F_RT_SET_ANCHOR_MAX, rt, g_hud_orig[1][0], g_hud_orig[1][1]);
            set_v2(c, F_RT_SET_ANCHORED_POS, rt, g_hud_orig[2][0], g_hud_orig[2][1]); set_v2(c, F_RT_SET_SIZE_DELTA, rt, g_hud_orig[3][0], g_hud_orig[3][1]);
            ST8(ctrl + 0x5a, 0);
        }
    }
    memcpy(c->x, sx, sizeof sx); memcpy(c->v, sv, sizeof sv);
    return 0;
}

// ------------------------------------------------------------------------------------- full-screen backgrounds
// Every frame on a screen that is not 16:9, for every canvas (from its CanvasScaler's per-frame Handle): walk the
// active objects under it, going down only through elements at least as wide as the 16:9 area, and give every
// childless element that is exactly 1920x1080 and is a backdrop by its name the size of the screen (in canvas
// units: 1080 * aspect wide on a wide screen, 1920 / aspect high on a tall one). Childless only, because resizing
// a parent would move children that are anchored to its sides; not the empty containers, because they can get
// children later. An element's size tells whether it was already done, so no per-object state is needed:
// 1920x1080 means untouched, the previous screen size means it has to follow a resolution change.
// Whether a childless full-screen element is something drawn (an image, a dimming layer) rather than an empty
// container, by its name. (Asking for its Graphic component instead, Component.GetComponent<Graphic> every frame,
// made the engine's render thread crash in its allocator when the title scene was unloaded.)
static int is_backdrop_name(const char *name) {
    static const char *const yes[] = {"image", "background", "black", "blur", "bg", "white", "fade", "flash", "filter", "curtain",
                                      "mask", "cover", "color", "overlay", "touchzone"};
    char low[160];
    size_t n = 0;
    for (; name[n] && n < sizeof low - 1; n++) low[n] = (char)(name[n] >= 'A' && name[n] <= 'Z' ? name[n] + 32 : name[n]);
    low[n] = 0;
    if (strstr(low, "root")) return 0;
    for (size_t i = 0; i < sizeof yes / sizeof *yes; i++)
        if (strstr(low, yes[i])) return 1;
    return 0;
}

static void widen_walk(Cpu *c, uint64_t t, int depth) {
    if (strcmp(class_of(t), "RectTransform")) return;
    uint64_t go = rt_call(c, FN(F_GET_GAMEOBJECT), 2, t, 0ull);
    if (!(rt_call(c, FN(F_ACTIVE_IN_HIER), 2, go, 0ull) & 1)) return;
    float r[4];
    rect_of(c, F_RT_RECT, t, r);
    if (depth > 0 && r[2] < 1900) return;
    int n = (int)rt_call(c, FN(F_CHILD_COUNT), 2, t, 0ull);
    if (n == 0) {
        if (depth < 3) return;      // 0 canvas, 1 SafeMargin, 2 its three roots
        int from_base = near_f(r[2], 1920) && near_f(r[3], 1080), from_prev = near_f(r[2], g_prev_w) && near_f(r[3], g_prev_h);
        if (!(from_base || from_prev) || (near_f(r[2], g_cur_w) && near_f(r[3], g_cur_h))) return;
        char a[160];
        if (!is_backdrop_name(name_of(c, go, a, sizeof a))) return;
        static int log = -1;
        if (log < 0) log = getenv("BR_SCREEN_DUMP") != NULL;
        if (log) {
            char b[160];
            rt_log("screen: resize %s (in %s) from %.0fx%.0f to %.0fx%.0f", a, name_of(c, rt_call(c, FN(F_T_PARENT), 2, t, 0ull), b, sizeof b),
                   r[2], r[3], g_cur_w, g_cur_h);
        }
        float sd[2];
        get_v2(c, F_RT_GET_SIZE_DELTA, t, sd);
        set_v2(c, F_RT_SET_SIZE_DELTA, t, sd[0] + g_cur_w - r[2], sd[1] + g_cur_h - r[3]);
        return;
    }
    if (depth >= 6) return;
    for (int i = 0; i < n; i++) {
        uint64_t ch = rt_call(c, FN(F_GET_CHILD), 3, t, (uint64_t)i, 0ull);
        if (ch) widen_walk(c, ch, depth + 1);
    }
}

// UnityEngine.UI.CanvasScaler::Handle(): runs every frame for every enabled scaler.
int rt_hide_ui;                              // set by a capture mode: every canvas with a scaler is switched off
#define F_BEHAVIOUR_SET_ENABLED 0x3f07320ull
int rt_hook_4176dc0(Cpu *c) {
    uint64_t s = c->x[0];
    if (rt_hide_ui) {
        uint64_t sx[31]; __uint128_t sv[8];
        memcpy(sx, c->x, sizeof sx); memcpy(sv, c->v, sizeof sv);
        uint64_t canvas = LD64(s + 0x50);
        if (alive(c, canvas)) rt_call(c, FN(F_BEHAVIOUR_SET_ENABLED), 3, canvas, 0ull, 0ull);
        memcpy(c->x, sx, sizeof sx); memcpy(c->v, sv, sizeof sv);
    }
    if (g_dump_collect && g_nscalers < 64) g_scalers[g_nscalers++] = s;      // for this frame's BR_SCREEN_DUMP only
    if (!g_wide && !g_tall && g_restore_frames <= 0) return 0;
    float rx, ry;
    memcpy(&rx, (void *)(uintptr_t)(s + 0x2c), 4); memcpy(&ry, (void *)(uintptr_t)(s + 0x30), 4);
    if ((int)LD32(s + 0x20) != 1 || (int)LD32(s + 0x34) != 1 || rx != 1920 || ry != 1080) return 0;   // Scale With Screen Size, Expand
    uint64_t sx[31]; __uint128_t sv[8];
    memcpy(sx, c->x, sizeof sx); memcpy(sv, c->v, sizeof sv);
    widen_walk(c, rt_call(c, FN(F_GET_TRANSFORM), 2, s, 0ull), 0);
    memcpy(c->x, sx, sizeof sx); memcpy(c->v, sv, sizeof sv);
    return 0;
}

// UnityEngine.RenderTexture::.ctor(int width, int height, int depth): the menus' blurred backdrop
// (UIViewMainMenuBottomBlur::EnableImage / createTexture) renders a camera into a fixed 1920x1080 texture and shows
// it in a full-screen image. Give the texture the screen's aspect ratio, so the resized image is not stretched.
#define BLUR_LO 0x450b9f0ull
#define BLUR_HI 0x450bc40ull
int rt_hook_3ef1eb0(Cpu *c) {
    uint64_t ra = c->x[30] - GUEST_BASE;
    if (ra < BLUR_LO || ra >= BLUR_HI || (uint32_t)c->x[1] != 1920 || (uint32_t)c->x[2] != 1080) return 0;
    uint64_t sx[31];
    memcpy(sx, c->x, sizeof sx);
    int sw = (int)rt_call(c, FN(F_SCREEN_W), 1, 0ull), sh = (int)rt_call(c, FN(F_SCREEN_H), 1, 0ull);
    memcpy(c->x, sx, sizeof sx);
    if (sh > 0 && sw * 9 > sh * 16 + 16) c->x[1] = (uint64_t)((1080 * sw + sh / 2) / sh);
    else if (sw > 0 && sw * 9 < sh * 16 - 16) c->x[2] = (uint64_t)((1920 * sh + sw / 2) / sw);
    return 0;
}

// ------------------------------------------------------------------------------------------------ diagnostics
static void tree(Cpu *c, uint64_t t, int depth, int max) {
    char a[160];
    uint64_t go = rt_call(c, FN(F_GET_GAMEOBJECT), 2, t, 0ull);
    if (!(rt_call(c, FN(F_ACTIVE_IN_HIER), 2, go, 0ull) & 1)) return;
    float r[4] = {0, 0, 0, 0}, p[3], s[3];
    if (!strcmp(class_of(t), "RectTransform")) rect_of(c, F_RT_RECT, t, r);
    rt_call(c, FN(F_T_POSITION), 2, t, 0ull);
    for (int i = 0; i < 3; i++) p[i] = vf(c, i);
    rt_call(c, FN(F_T_LOSSY_SCALE), 2, t, 0ull);
    for (int i = 0; i < 3; i++) s[i] = vf(c, i);
    int n = (int)rt_call(c, FN(F_CHILD_COUNT), 2, t, 0ull);
    rt_log("screen: %*s%s  x %.0f..%.0f  y %.0f..%.0f  (%d children)", depth * 2 + 2, "", name_of(c, go, a, sizeof a),
           p[0] + r[0] * s[0], p[0] + (r[0] + r[2]) * s[0], p[1] + r[1] * s[1], p[1] + (r[1] + r[3]) * s[1], n);
    if (depth >= max) return;
    for (int i = 0; i < n; i++) {
        uint64_t ch = rt_call(c, FN(F_GET_CHILD), 3, t, (uint64_t)i, 0ull);
        if (ch) tree(c, ch, depth + 1, max);
    }
}

static void dump(Cpu *c) {
    char a[160], b[160];
    const char *td = getenv("BR_SCREEN_TREE");
    float r[4], p[4];
    int sw = (int)rt_call(c, FN(F_SCREEN_W), 1, 0ull), sh = (int)rt_call(c, FN(F_SCREEN_H), 1, 0ull);
    rt_log("screen: ==== %.1f s, Screen %dx%d", (double)(rt_ticks() - g_t0) / 19200000.0, sw, sh);
    uint64_t cams = rt_call(c, FN(F_CAM_ALL), 1, 0ull);
    int n = cams ? (int)LD32(cams + 0x18) : 0;
    for (int i = 0; i < n; i++) {
        uint64_t cam = LD64(cams + 0x20 + 8ull * i);
        if (!alive(c, cam)) continue;
        rect_of(c, F_CAM_RECT, cam, r);
        rect_of(c, F_CAM_PIXEL_RECT, cam, p);
        uint64_t tt = rt_call(c, FN(F_CAM_TARGET), 2, cam, 0ull);
        rt_call(c, FN(F_CAM_DEPTH), 2, cam, 0ull);
        float depth = vf(c, 0);
        rt_call(c, FN(0x3ea9a20ull), 2, cam, 0ull);              // Camera.get_fieldOfView (vertical, degrees)
        float fov = vf(c, 0);
        rt_call(c, FN(0x3eaa4f0ull), 2, cam, 0ull);              // Camera.get_aspect
        float aspect = vf(c, 0);
        int physical = (int)(rt_call(c, FN(0x3eab4d0ull), 2, cam, 0ull) & 1), fit = (int)rt_call(c, FN(0x3eac070ull), 2, cam, 0ull);
        rt_log("screen: camera %-40s depth %5.1f rect %.3f %.3f %.3f %.3f  pixels %.0f %.0f %.0f %.0f  target %s  fov %.2f aspect %.3f physical %d gate fit %d",
               name_of(c, cam, a, sizeof a), depth, r[0], r[1], r[2], r[3], p[0], p[1], p[2], p[3], tt ? name_of(c, tt, b, sizeof b) : "screen",
               fov, aspect, physical, fit);
    }
    for (int i = 0; i < g_nscalers; i++) {
        uint64_t s = g_scalers[i];
        if (!alive(c, s)) continue;
        uint64_t go = rt_call(c, FN(F_GET_GAMEOBJECT), 2, s, 0ull), canvas = LD64(s + 0x50);
        if (!(rt_call(c, FN(F_ACTIVE_IN_HIER), 2, go, 0ull) & 1) || !alive(c, canvas)) continue;
        int mode = (int)rt_call(c, FN(F_CANVAS_RENDER_MODE), 2, canvas, 0ull), root = (int)(rt_call(c, FN(F_CANVAS_IS_ROOT), 2, canvas, 0ull) & 1);
        uint64_t wc = rt_call(c, FN(F_CANVAS_WORLD_CAMERA), 2, canvas, 0ull);
        rect_of(c, F_CANVAS_PIXEL_RECT, canvas, p);
        rt_call(c, FN(F_CANVAS_SCALE_FACTOR), 2, canvas, 0ull);
        float sf = vf(c, 0), rx, ry, match;
        memcpy(&rx, (void *)(uintptr_t)(s + 0x2c), 4); memcpy(&ry, (void *)(uintptr_t)(s + 0x30), 4); memcpy(&match, (void *)(uintptr_t)(s + 0x38), 4);
        rt_log("screen: canvas %-40s %s mode %d camera %s  scaler mode %d ref %.0fx%.0f match-mode %d match %.2f  scale %.3f  pixels %.0f %.0f %.0f %.0f",
               name_of(c, go, a, sizeof a), root ? "root" : "nested", mode, name_of(c, wc, b, sizeof b), (int)LD32(s + 0x20), rx, ry,
               (int)LD32(s + 0x34), match, sf, p[0], p[1], p[2], p[3]);
        if (td) tree(c, rt_call(c, FN(F_GET_TRANSFORM), 2, s, 0ull), 0, atoi(td));
    }
}

static volatile int g_dump_request;
void ultrawide_request_dump(void) { g_dump_request = 1; }   // dump cameras and canvases (and the UI tree with BR_SCREEN_TREE) now
// Camera.gateFit of physical cameras that draw to the screen: Vertical (1) on a wide screen, back to Horizontal
// (2) otherwise for the ones this changed (found again by the fit they have). Looked at twice a second:
// Camera.allCameras makes a new array each time.
#define F_CAM_USE_PHYSICAL 0x3eab4d0ull    // UnityEngine.Camera::get_usePhysicalProperties
#define F_CAM_GET_GATE_FIT 0x3eac070ull
#define F_CAM_SET_GATE_FIT 0x3eac0c0ull
static void gate_fit_tick(Cpu *c) {
    static uint64_t next;
    static int changed;
    uint64_t now = GetTickCount64();
    if (now < next || (!g_wide && !changed)) return;
    next = now + 500;
    uint64_t cams = rt_call(c, FN(F_CAM_ALL), 1, 0ull);
    int n = cams ? (int)LD32(cams + 0x18) : 0, left = 0;
    for (int i = 0; i < n; i++) {
        uint64_t cam = LD64(cams + 0x20 + 8ull * i);
        if (!alive(c, cam) || !(rt_call(c, FN(F_CAM_USE_PHYSICAL), 2, cam, 0ull) & 1) || rt_call(c, FN(F_CAM_TARGET), 2, cam, 0ull)) continue;
        int fit = (int)rt_call(c, FN(F_CAM_GET_GATE_FIT), 2, cam, 0ull);
        if (g_wide && fit == 2) {
            char a[96];
            rt_call(c, FN(F_CAM_SET_GATE_FIT), 3, cam, 1ull, 0ull);
            if (!changed) rt_log("screen: wide screen: camera %s fits its view vertically", name_of(c, cam, a, sizeof a));
            changed = 1;
        } else if (!g_wide && changed && fit == 1) rt_call(c, FN(F_CAM_SET_GATE_FIT), 3, cam, 2ull, 0ull);
        if (g_wide && (fit == 1 || fit == 2)) left++;
    }
    if (!g_wide && !left) changed = 0;
}

void ultrawide_tick(Cpu *c) {   // once per frame, after every Update; the caller preserves the registers
    static const char *spec;
    static int next = -1;
    if (!g_t0) { g_t0 = rt_ticks(); spec = getenv("BR_SCREEN_DUMP"); if (spec) next = atoi(spec); }
    static const char *resize;
    static int resize_init;
    if (!resize_init) { resize_init = 1; resize = getenv("BR_SCREEN_RESIZE"); }
    int rs, rw, rh;
    if (resize && sscanf(resize, "%d:%dx%d", &rs, &rw, &rh) == 3 && (rt_ticks() - g_t0) / 19200000ull >= (uint64_t)rs) {
        rt_log("screen: BR_SCREEN_RESIZE at %d s -> %dx%d window", rs, rw, rh);
        rt_call(c, FN(F_SET_RESOLUTION), 4, (uint64_t)rw, (uint64_t)rh, 3ull, 0ull);
        resize = strchr(resize, ',');
        if (resize) resize++;
    }
    int sw = (int)rt_call(c, FN(F_SCREEN_W), 1, 0ull), sh = (int)rt_call(c, FN(F_SCREEN_H), 1, 0ull);
    g_wide = sh > 0 && sw * 9 > sh * 16 + 16;
    g_tall = sw > 0 && sw * 9 < sh * 16 - 16;
    float w = g_wide ? 1080.0f * (float)sw / (float)sh : 1920, h = g_tall ? 1920.0f * (float)sh / (float)sw : 1080;
    gate_fit_tick(c);
    if (w != g_cur_w || h != g_cur_h) { g_prev_w = g_cur_w; g_prev_h = g_cur_h; g_cur_w = w; g_cur_h = h; g_restore_frames = 600; }
    else if (!g_wide && !g_tall && g_restore_frames > 0) g_restore_frames--;   // back at 16:9: keep walking a while to undo
    if (g_dump_request) {                 // BR_INPUT_LIVE line "DUMP": collect during the next frame, dump at its end
        if (g_dump_collect) { dump(c); g_dump_request = 0; g_dump_collect = 0; }
        else g_dump_collect = 1;
    } else if (spec && next >= 0) {
        int due = (rt_ticks() - g_t0) / 19200000ull >= (uint64_t)next;
        if (due && g_dump_collect) {      // the scalers of this frame were collected
            dump(c);
            spec = strchr(spec, ',');
            if (spec) next = atoi(++spec); else next = -1;
            due = 0;
        }
        g_dump_collect = due;             // collect during the next frame, dump at its end
    }
    g_nscalers = 0;
}

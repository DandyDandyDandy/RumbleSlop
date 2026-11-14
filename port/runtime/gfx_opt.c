// The graphics rows of Options > Display that the game lists but does not act on.
//
// Version 2.20 shows Graphics Settings, Shadow Quality, Post-Processing Effects (Low / Medium / High),
// Anti-Aliasing and Texture Smoothness (On / Off). They are saved, but the functions that would apply them are empty
// in this build (OptionElementSaveData_Quality.changeValue and _OnOff.changeValue are a bare `ret`, and nothing reads
// the saved values). The port applies them. Medium / On is what the console shows, so a save that never touched
// the rows looks as before, except for 4x instead of 2x MSAA:
//   Graphics Settings        render scale 0.85 / the level's own (1.2 in a stage, 1.1 in the menus) / 1.5, and the
//                            distance at which models switch to simpler versions (LOD bias x0.7 / x1 / x2)
//   Shadow Quality           main light shadow map 512 / the level's own (1024 in a stage, 2048 in the menus) / 4096
//   Post-Processing Effects  Low: off; Medium: the game's; High: the game's plus SMAA edge smoothing
//   Anti-Aliasing            MSAA 4x (the console has 2x) / off
//   Texture Smoothness       anisotropic filtering forced on / off
//   HUD Size                 Large: the in-game HUD 1.2 times as big (ultrawide.c, by the canvas scaler)
// The values are read from the save every frame (five loads) and applied when one changes or when the game has
// switched its quality level (each level has its own render pipeline asset, and the engine resets the rest).
// BR_GFX_LOG=1 logs what is applied.
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "rt.h"

#define FN(off) (GUEST_BASE + (off))
#define F_USER_PARAM 0x4403f70ull           // SaveDataContainer.UserParam.entity (static) -> SaveUserParam
#define USER_OPTION 0x30                    // SaveUserParam.m_OptionParam
#define OPT_AA 0x40                         // OptionParam.m_DataAntialiasing (OnOff: 1 on, 2 off)
#define OPT_ANISO 0xd0
#define OPT_HUD 0xc8                        // StandardLarge: 1 standard, 2 large
#define OPT_GRAPHIC 0xd8                    // Quality: 1 low, 2 middle, 3 high
#define OPT_SHADOW 0xe0
#define OPT_POST 0xe8
#define ELEM_VALUE 0x10
#define F_URP_CURRENT 0x43b08e0ull          // GraphicsManager.URPAssets.get_current (static)
#define F_GET_MSAA 0x3d2cac0ull
#define F_SET_MSAA 0x3d2cad0ull
#define F_GET_SCALE 0x3d2cae0ull
#define F_SET_SCALE 0x3d2caf0ull
#define F_GET_SHADOW_RES 0x3d2cce0ull
#define F_SET_SHADOW_RES 0x3d2ccf0ull
#define F_GET_LOD_BIAS 0x3ec1cb0ull         // QualitySettings (static)
#define F_SET_LOD_BIAS 0x3ec1cf0ull
#define F_GET_QUALITY_LEVEL 0x3ec2110ull
#define F_ALL_CAMERAS 0x3eaf2f0ull          // Camera.allCameras
#define F_CAMERA_DATA 0x3d89380ull          // CameraExtensions.GetUniversalAdditionalCameraData(Camera)
#define F_GET_POST 0x3d8aee0ull             // UniversalAdditionalCameraData.renderPostProcessing
#define F_SET_POST 0x3d8aef0ull
#define F_GET_CAM_AA 0x3d8af00ull           // .antialiasing: 0 none, 1 FXAA, 2 SMAA
#define F_SET_CAM_AA 0x3d8af10ull
#define F_SET_CAM_AA_QUALITY 0x3d8af30ull   // 0 low, 1 medium, 2 high
#define CACHED_PTR 0x10

void *rt_icall_host(const char *name);
int gfx_hud_large;                          // read by ultrawide.c (UIRootCanvasCtrl.Update of the HUD canvases)
static int g_last[5] = {-1, -1, -1, -1, -1}, g_dirty = 1, g_log = -1;
static float g_lod_base = -1;
static float g_lod_of_level[32];             // each quality level's own LOD bias, read the first time the level is seen
static uint32_t g_lod_seen;
// what each render pipeline asset had before the port touched it (the assets live as long as the game)
static struct { uint64_t asset; float scale; int msaa, shadow; } g_base[16];
static int g_nbase;

static float call_f(Cpu *c, uint64_t fn, uint64_t self) {
    rt_call(c, FN(fn), 2, self, 0ull);
    return rt_getf(c, 0);
}
static void set_f(Cpu *c, uint64_t fn, uint64_t self, float v) {
    c->x[0] = self; c->x[1] = 0;
    rt_setf(c, 0, v);
    rt_call_regs(c, FN(fn));
}
// The game switched its quality level (display_opt.c, after QualitySettings.SetQualityLevel): the engine has put
// the level's own LOD bias and filtering back.
static void lod_base(Cpu *c) {
    // The engine's setters change the active level's stored settings, so a level's own value can only be read
    // before the port has written to that level.
    int level = (int32_t)rt_call(c, FN(F_GET_QUALITY_LEVEL), 1, 0ull);
    if (level < 0 || level > 31) level = 31;
    if (!(g_lod_seen >> level & 1)) { g_lod_seen |= 1u << level; g_lod_of_level[level] = call_f(c, F_GET_LOD_BIAS, 0); }
    g_lod_base = g_lod_of_level[level];
}
void gfx_quality_changed(Cpu *c) {
    lod_base(c);
    g_dirty = 1;
}

// Post-processing per camera. A camera the game itself has post-processing off for (UI, overlays) is left alone:
// Low turns it off for cameras that have it on and remembers them (by their native object), Medium and High turn
// it on again for exactly those. High also gives a camera without post-process anti-aliasing SMAA, remembered the
// same way, and Medium / Low take it back.
static uint64_t g_off_ids[32], g_smaa_ids[32];
static int g_noff, g_nsmaa;
static int id_find(const uint64_t *ids, int n, uint64_t id) { for (int k = 0; k < n; k++) if (ids[k] == id) return k; return -1; }
static void post_apply(Cpu *c, int level) {
    uint64_t cams = rt_call(c, FN(F_ALL_CAMERAS), 1, 0ull);
    int n = cams ? (int32_t)LD32(cams + 0x18) : 0;
    for (int i = 0; i < n && i < 16; i++) {
        uint64_t cam = LD64(cams + 0x20 + 8ull * i);
        if (!cam || !LD64(cam + CACHED_PTR)) continue;
        uint64_t data = rt_call(c, FN(F_CAMERA_DATA), 2, cam, 0ull);
        cams = rt_call(c, FN(F_ALL_CAMERAS), 1, 0ull);      // host locals are not roots: take the array again after a call that can allocate
        if (!cams) return;
        if (!data || !LD64(data + CACHED_PTR)) continue;
        uint64_t id = LD64(data + CACHED_PTR);
        int on = (int)(rt_call(c, FN(F_GET_POST), 2, data, 0ull) & 1), mine = id_find(g_off_ids, g_noff, id);
        if (level == 1 && on) {
            rt_call(c, FN(F_SET_POST), 3, data, 0ull, 0ull);
            if (mine < 0 && g_noff < 32) g_off_ids[g_noff++] = id;
            on = 0;
        } else if (level != 1 && !on && mine >= 0) {
            rt_call(c, FN(F_SET_POST), 3, data, 1ull, 0ull);
            g_off_ids[mine] = g_off_ids[--g_noff];
            on = 1;
        }
        int aa = (int32_t)rt_call(c, FN(F_GET_CAM_AA), 2, data, 0ull), smaa = id_find(g_smaa_ids, g_nsmaa, id);
        if (level == 3 && on && aa == 0 && g_nsmaa < 32) {
            rt_call(c, FN(F_SET_CAM_AA), 3, data, 2ull, 0ull);
            rt_call(c, FN(F_SET_CAM_AA_QUALITY), 3, data, 2ull, 0ull);
            if (smaa < 0) g_smaa_ids[g_nsmaa++] = id;
        } else if (level != 3 && smaa >= 0) {
            if (aa == 2) rt_call(c, FN(F_SET_CAM_AA), 3, data, 0ull, 0ull);
            g_smaa_ids[smaa] = g_smaa_ids[--g_nsmaa];
        }
    }
}

static void apply(Cpu *c, const int v[5]) {
    uint64_t asset = rt_call(c, FN(F_URP_CURRENT), 1, 0ull);
    if (!asset) return;
    int b = -1;
    for (int i = 0; i < g_nbase; i++) if (g_base[i].asset == asset) b = i;
    if (b < 0 && g_nbase < 16) {
        b = g_nbase++;
        g_base[b].asset = asset;
        g_base[b].scale = call_f(c, F_GET_SCALE, asset);
        g_base[b].msaa = (int32_t)rt_call(c, FN(F_GET_MSAA), 2, asset, 0ull);
        g_base[b].shadow = (int32_t)rt_call(c, FN(F_GET_SHADOW_RES), 2, asset, 0ull);
        rt_log("graphics: a render pipeline asset of the game: render scale %.2f, MSAA %d, shadow map %d", (double)g_base[b].scale,
               g_base[b].msaa, g_base[b].shadow);
    }
    if (b < 0) return;
    lod_base(c);
    int gs = v[0], sh = v[1], pe = v[2], aa = v[3], af = v[4];
    float scale = gs == 1 ? 0.85f : gs == 3 ? 1.5f : g_base[b].scale;
    if (gs == 1 && scale > g_base[b].scale) scale = g_base[b].scale;      // never above the level's own on Low
    if (gs == 3 && scale < g_base[b].scale) scale = g_base[b].scale;
    int shadow = sh == 1 ? 512 : sh == 3 ? 4096 : g_base[b].shadow;
    if (sh == 1 && shadow > g_base[b].shadow) shadow = g_base[b].shadow;
    if (sh == 3 && shadow < g_base[b].shadow) shadow = g_base[b].shadow;
    int msaa = aa == 2 ? 1 : g_base[b].msaa < 4 ? 4 : g_base[b].msaa;
    set_f(c, F_SET_SCALE, asset, scale);
    rt_call(c, FN(F_SET_SHADOW_RES), 3, asset, (uint64_t)shadow, 0ull);
    rt_call(c, FN(F_SET_MSAA), 3, asset, (uint64_t)msaa, 0ull);
    set_f(c, F_SET_LOD_BIAS, 0, g_lod_base * (gs == 1 ? 0.7f : gs == 3 ? 2.0f : 1.0f));
    static void (*set_aniso)(int);
    static int tried;
    if (!tried) { tried = 1; set_aniso = (void (*)(int))rt_icall_host("UnityEngine.QualitySettings::set_anisotropicFiltering"); }
    if (set_aniso) set_aniso(af == 2 ? 0 : 2);                             // Disable / ForceEnable
    post_apply(c, pe);
    if (g_log) rt_log("graphics: applied render scale %.2f, shadow map %d, MSAA %d, LOD bias %.2f, anisotropic %s, post-processing level %d",
                      (double)scale, shadow, msaa, (double)(g_lod_base * (gs == 1 ? 0.7f : gs == 3 ? 2.0f : 1.0f)), af == 2 ? "off" : "on", pe);
}

// Once per rendered frame on the main thread (from the per-frame hook in display_opt.c).
void gfx_opt_tick(Cpu *c) {
    static uint64_t last_asset;
    static int frame;
    if (g_log < 0) g_log = getenv("BR_GFX_LOG") != NULL;
    if (getenv("BR_GFX_OFF")) return;                                      // comparison: the rows do nothing, as before
    uint64_t user = rt_call(c, FN(F_USER_PARAM), 1, 0ull), opt = user ? LD64(user + USER_OPTION) : 0;
    if (!opt) return;
    { uint64_t e = LD64(opt + OPT_HUD); gfx_hud_large = e && LD32(e + ELEM_VALUE) == 2; }
    static const int off[5] = {OPT_GRAPHIC, OPT_SHADOW, OPT_POST, OPT_AA, OPT_ANISO};
    int v[5];
    for (int i = 0; i < 5; i++) {
        uint64_t e = LD64(opt + off[i]);
        v[i] = e ? (int32_t)LD32(e + ELEM_VALUE) : 0;
        if (v[i] < 1 || v[i] > 3) v[i] = i < 3 ? 2 : 1;                    // not set: Medium / On
    }
    static const char *force;                                               // test: BR_GFX_FORCE=<graphics>,<shadow>,<post>,<aa>,<aniso>[,<hud>]
    if (!force) { force = getenv("BR_GFX_FORCE"); if (!force) force = ""; }
    if (*force) { int hud = 0; if (sscanf(force, "%d,%d,%d,%d,%d,%d", &v[0], &v[1], &v[2], &v[3], &v[4], &hud) == 6) gfx_hud_large = hud == 2; }
    uint64_t asset = rt_call(c, FN(F_URP_CURRENT), 1, 0ull);
    int changed = g_dirty || asset != last_asset || memcmp(v, g_last, sizeof v) != 0;
    // cameras come and go with scenes: the post-processing level is looked at again twice a second
    if (!changed && (++frame % 120 || (v[2] == 2 && !g_noff && !g_nsmaa))) return;
    if (changed) { g_dirty = 0; last_asset = asset; memcpy(g_last, v, sizeof v); apply(c, v); }
    else post_apply(c, v[2]);
}

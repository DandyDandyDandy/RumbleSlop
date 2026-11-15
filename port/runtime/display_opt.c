// PC display options inside the game's own Options > Display page.
//
// The game's PC option code has one "Screen Mode" row that only switches Screen.fullScreenMode between
// FullScreenWindow and Windowed, and it pins Application.targetFrameRate to 60 (30 with half-rate vsync). The
// port turns that into three rows:
//   Screen Mode       Windowed / Borderless / Full Screen        (the game's own row, extended)
//   Resolution        the 16:9 sizes that fit the desktop (and the 16:10 ones on a 16:10 desktop), plus the
//                     desktop size
//   Frame Rate Limit  30 / 60 / 120 / 144 / Unlimited
//   VSync             Off / On   (the game has a VSync option object but does not list it on this page; with
//                     VSync on, Unity ignores the frame rate limit and follows the display)
// A change applies at once, as the game's own rows do.
//
// Steam Deck: detected at start-up (is_steam_deck). There the game starts borderless at the display's own size
// (1280x800 on the built-in screen) whatever the game's save says, and the settings live in a file of their own,
// port_display_deck.txt, so a folder shared with a desktop PC keeps both machines' choices.
//
// How: the functions below are entry hooks (port/recomp/hooks.txt -> generated `if (rt_hook_<offset>(c)) return;`).
// The two new rows are extra objects of the game's UIOptionChoicesItemData_ScreenMode class: after
// UIOptionScrollModule::CheckActiveChoicesItemList has built the row list of a page, the Screen Mode row found in
// it is copied twice and the copies are inserted after it. A copy carries a role number in the padding word at
// +0x54 (0 in the game's own object), and the hooks on the class's methods answer by role. Row labels come from
// UIOptionChoicesItemData::GetContentStr, hooked for the copies. The game's saved value keeps its meaning
// (1 = windowed, anything else = full screen); everything else is stored by the port in <exe dir>\port_display.txt.
// Every screen-mode change of the game, including the one made when its save is loaded at start-up, goes through
// Screen.set_fullScreenMode and is turned into Screen.SetResolution with the port's settings.
//
// Object layout used (from the lifted code): item +0x20 picker index, +0x28 its copy, +0x30 option record,
// +0x38 item table, +0x40 save element (value at +0x10), +0x48 List<int> of mode values, +0x50 initial index.
// List<T>: items array +0x10 (elements from +0x20, capacity +0x18), size +0x18, version +0x1c.
#include <windows.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "rt.h"

enum { MODE_VALUE_WINDOW = 1, MAX_RES = 16, ROLE_MODE = 0, ROLE_RES = 1, ROLE_FPS = 2, ROLE_VSYNC = 3, ROLE_SMOOTH = 5, ROLE_DOLE = 6, ROLE_MOUSE = 8 };
enum { M_WINDOWED, M_BORDERLESS, M_FULLSCREEN, M_COUNT };
#define FN(off) (GUEST_BASE + (off))
#define F_GET_VALUE_STR 0x4575d40ull
#define F_CHANGE_VALUE 0x4575ea0ull
#define F_SET_INIT_INDEX 0x4575bf0ull
#define F_CHECK_ACTIVE_LIST 0x455d030ull
#define F_SET_RESOLUTION 0x3ebced0ull       // UnityEngine.Screen::SetResolution(int, int, FullScreenMode)
#define F_SET_TARGET_FRAME_RATE 0x3ea81e0ull
#define F_SET_VSYNC_COUNT 0x3ec1de0ull       // UnityEngine.QualitySettings::set_vSyncCount(int)
#define F_STRING_NEW_UTF16 0x45af94ull      // il2cpp::vm::String::NewUtf16(const Il2CppChar *, int32_t)
#define F_OBJECT_NEW 0x469f40ull            // il2cpp_object_new(klass)
#define F_GC_DISABLE 0x469d10ull
#define F_GC_ENABLE 0x469d00ull
#define F_LIST_ADD_WITH_RESIZE 0x33bfab0ull // List<object>::AddWithResize(item, MethodInfo *)
#define LIST_OBJECT_CLASS_SLOT 0x6175d40ull // metadata slot CheckActiveChoicesItemList takes List<object> from
#define CONTROLS_ROW "DeadZoneInsideRangeL"   // a row class only the Controls page has
void pc_mouse_sensitivity(int n);            // pc_input.c
#define GAMEPLAY_ROW "DisplayTips"          // a row class only the General page (the game play settings) has

static const int UNITY_MODE[M_COUNT] = {3 /* Windowed */, 1 /* FullScreenWindow */, 0 /* ExclusiveFullScreen */};
static const int FPS[] = {30, 60, 120, 144, -1};
#define NFPS ((int)(sizeof FPS / sizeof *FPS))

static int g_res[MAX_RES][2], g_nres, g_k, g_desk_w, g_desk_h, g_mode = -1, g_fps = 1, g_def_k, g_vsync = 1, g_smooth = 1, g_dole, g_deck, g_mouse = 5;
static uint64_t g_sm_klass;
static uint32_t g_pattern_h;                // GC handle of the game's Screen Mode row object
#define F_GCHANDLE_NEW 0x469da0ull          // il2cpp_gchandle_new(obj, pinned)
#define F_GCHANDLE_TARGET 0x469df0ull
#define F_GCHANDLE_FREE 0x469e10ull
static INIT_ONCE g_once = INIT_ONCE_STATIC_INIT;
static __thread int t_in_str, t_in_change, t_in_init, t_in_list, t_in_fps, t_in_vsync;

// Steam sets SteamDeck=1 for everything it starts on a Deck (Proton hands it to the game). With Windows installed on
// the Deck there is no Steam variable, but the firmware names the machine: Valve, Jupiter (LCD) or Galileo (OLED).
static int is_steam_deck(void) {
    const char *e = getenv("SteamDeck");
    if (e) return *e == '1';
    static const char key[] = "HARDWARE\\DESCRIPTION\\System\\BIOS";
    char maker[64] = "", product[64] = "";
    DWORD n = sizeof maker - 1;
    RegGetValueA(HKEY_LOCAL_MACHINE, key, "SystemManufacturer", RRF_RT_REG_SZ, NULL, maker, &n);
    n = sizeof product - 1;
    RegGetValueA(HKEY_LOCAL_MACHINE, key, "SystemProductName", RRF_RT_REG_SZ, NULL, product, &n);
    return !strcmp(maker, "Valve") && (!strcmp(product, "Jupiter") || !strcmp(product, "Galileo"));
}
static void cfg_path(char *out, size_t n) {
    snprintf(out, n, "%sport_display%s.txt", rt_user_dir(), g_deck ? "_deck" : "");
}
static void add_res(int w, int h) {
    for (int i = 0; i < g_nres; i++)
        if (g_res[i][0] == w && g_res[i][1] == h) return;
    if (g_nres < MAX_RES) { g_res[g_nres][0] = w; g_res[g_nres][1] = h; g_nres++; }
}
static BOOL CALLBACK init_once(PINIT_ONCE o, PVOID p, PVOID *ctx) {
    (void)o; (void)p; (void)ctx;
    static const int all[][2] = {{1280, 720}, {1280, 800}, {1440, 900}, {1600, 900}, {1680, 1050}, {1920, 1080}, {1920, 1200},
                                 {2560, 1440}, {2560, 1600}, {3200, 1800}, {3840, 2160}};
    g_deck = is_steam_deck();
    DEVMODEA dm = {.dmSize = sizeof dm};
    if (EnumDisplaySettingsA(NULL, ENUM_CURRENT_SETTINGS, &dm)) { g_desk_w = (int)dm.dmPelsWidth; g_desk_h = (int)dm.dmPelsHeight; }
    const char *fake = getenv("BR_DESKTOP_SIZE");      // testing: pretend the desktop has this size (1280x800 = a Steam Deck)
    int fw, fh;
    if (fake && sscanf(fake, "%dx%d", &fw, &fh) == 2) { g_desk_w = fw; g_desk_h = fh; }
    if (g_desk_w <= 0 || g_desk_h <= 0) { g_desk_w = 1920; g_desk_h = 1080; }
    for (size_t i = 0; i < sizeof all / sizeof *all; i++)
        if (all[i][0] <= g_desk_w && all[i][1] <= g_desk_h && (all[i][0] * 9 == all[i][1] * 16 || g_desk_w * 10 == g_desk_h * 16))
            add_res(all[i][0], all[i][1]);      // the 16:10 sizes only on a 16:10 desktop
    add_res(g_desk_w, g_desk_h);      // last entry: the desktop size itself
    // default: the largest size smaller than the desktop, so a window fits with its frame
    for (int i = 0; i < g_nres; i++)
        if (g_res[i][0] < g_desk_w && g_res[i][1] < g_desk_h) g_def_k = i;
    if (g_deck) { g_def_k = g_nres - 1; g_mode = M_BORDERLESS; }      // Steam Deck: fill the display
    g_k = g_def_k;
    char path[MAX_PATH + 32], line[128];
    cfg_path(path, sizeof path);
    FILE *f = fopen(path, "r");
    int w = 0, h = 0, v;
    while (f && fgets(line, sizeof line, f)) {
        if (sscanf(line, "mode=%d", &v) == 1 && v >= 0 && v < M_COUNT) g_mode = v;
        else if (sscanf(line, "width=%d", &v) == 1) w = v;
        else if (sscanf(line, "height=%d", &v) == 1) h = v;
        else if (sscanf(line, "vsync=%d", &v) == 1) g_vsync = v < 0 ? 0 : v > 4 ? 4 : v;   // the menu only sets 0 or 1
        else if (sscanf(line, "smooth=%d", &v) == 1) g_smooth = v != 0;
        else if (sscanf(line, "dole=%d", &v) == 1) g_dole = v != 0;
        else if (sscanf(line, "mouse=%d", &v) == 1 && v >= 1 && v <= 10) g_mouse = v;
        else if (sscanf(line, "fps=%d", &v) == 1)
            for (int i = 0; i < NFPS; i++)
                if (FPS[i] == v) g_fps = i;
    }
    if (f) fclose(f);
    pc_mouse_sensitivity(g_mouse);
    for (int i = 0; i < g_nres; i++)
        if (g_res[i][0] == w && g_res[i][1] == h) g_k = i;
    rt_log("display: %sdesktop %dx%d, %d sizes, selected %dx%d, mode %d, frame rate limit %d", g_deck ? "Steam Deck, " : "",
           g_desk_w, g_desk_h, g_nres, g_res[g_k][0], g_res[g_k][1], g_mode, FPS[g_fps]);
    return TRUE;
}
static void init(void) { InitOnceExecuteOnce(&g_once, init_once, NULL, NULL); }
static void save_cfg(void) {
    char path[MAX_PATH + 32];
    cfg_path(path, sizeof path);
    FILE *f = fopen(path, "w");
    if (f) { fprintf(f, "mode=%d\nwidth=%d\nheight=%d\nfps=%d\nvsync=%d\nsmooth=%d\ndole=%d\nmouse=%d\n", g_mode, g_res[g_k][0], g_res[g_k][1], FPS[g_fps], g_vsync, g_smooth, g_dole, g_mouse); fclose(f); }
}

static void call_orig(Cpu *c, uint64_t off) { c->pc = FN(off); rt_dispatch(c); }
uint64_t rt_tr_str(Cpu *c, const char *english);      // port_text.c: the text in the game's language
static uint64_t new_str(Cpu *c, const char *s) { return rt_tr_str(c, s); }
static void apply_fps(Cpu *c) {
    t_in_fps = 1;
    rt_call(c, FN(F_SET_TARGET_FRAME_RATE), 2, (uint64_t)(uint32_t)FPS[g_fps], 0ull);
    t_in_fps = 0;
}
static void apply_vsync(Cpu *c) {
    t_in_vsync = 1;
    rt_call(c, FN(F_SET_VSYNC_COUNT), 2, (uint64_t)g_vsync, 0ull);
    t_in_vsync = 0;
}
// Borderless always covers the desktop at its own size; the chosen size is for a window or exclusive full screen.
static void apply_display(Cpu *c) {
    int w = g_mode == M_BORDERLESS ? g_desk_w : g_res[g_k][0], h = g_mode == M_BORDERLESS ? g_desk_h : g_res[g_k][1];
    const char *size = getenv("BR_DISPLAY_SIZE");      // testing: <width>x<height>, any aspect ratio
    int tw, th;
    if (size && sscanf(size, "%dx%d", &tw, &th) == 2 && tw > 0 && th > 0) { w = tw; h = th; }
    rt_log("display: mode %d -> SetResolution(%d, %d, %d)", g_mode, w, h, UNITY_MODE[g_mode]);
    rt_call(c, FN(F_SET_RESOLUTION), 4, (uint64_t)w, (uint64_t)h, (uint64_t)UNITY_MODE[g_mode], 0ull);
}

// Options > Controls > Mouse Sensitivity, 1..10 (`mouse=` in the settings file; 5 = the game's drag length). The
// game has this option and its arithmetic but shows no row for it in this build; pc_input.c applies it to the drag.
static void set_mouse(int n) {
    g_mouse = n < 1 ? 1 : n > 10 ? 10 : n;
    save_cfg();
    pc_mouse_sensitivity(g_mouse);
}

static int role_of(uint64_t item) { return (int32_t)LD32(item + 0x54); }
static int32_t mode_value(uint64_t item) { return (int32_t)LD32(LD64(item + 0x40) + 0x10); }
// Index in the game's own mode list of the windowed (want_window) or the full-screen value.
static int game_index(uint64_t item, int want_window) {
    uint64_t list = LD64(item + 0x48), items = list ? LD64(list + 0x10) : 0;
    int n = list ? (int32_t)LD32(list + 0x18) : 0;
    for (int i = 0; items && i < n; i++)
        if (((int32_t)LD32(items + 0x20 + 4ull * i) == MODE_VALUE_WINDOW) == !!want_window) return i;
    return want_window ? 1 : 0;
}
static int role_index(int role) { return role == ROLE_MOUSE ? g_mouse - 1 : role == ROLE_DOLE ? g_dole : role == ROLE_SMOOTH ? g_smooth : role == ROLE_RES ? g_k : role == ROLE_FPS ? g_fps : role == ROLE_VSYNC ? g_vsync : g_mode < 0 ? M_BORDERLESS : g_mode; }
static int role_count(int role) { return role == ROLE_MOUSE ? 10 : role == ROLE_SMOOTH || role == ROLE_DOLE ? 2 : role == ROLE_RES ? g_nres : role == ROLE_FPS ? NFPS : role == ROLE_VSYNC ? 2 : M_COUNT; }
static void set_indices(uint64_t item, int idx) { ST32(item + 0x20, idx); ST32(item + 0x28, idx); ST32(item + 0x50, idx); }
// The added rows of the page on screen (GC handles), so that Reset can put their picker positions back.
static uint32_t g_copy_h[12];
static int g_ncopy;
static void forget_copies(Cpu *c) {
    for (int i = 0; i < g_ncopy; i++) rt_call(c, FN(F_GCHANDLE_FREE), 1, (uint64_t)g_copy_h[i]);
    g_ncopy = 0;
}
static void track_copy(Cpu *c, uint64_t obj) {
    if (g_ncopy < 12) g_copy_h[g_ncopy++] = (uint32_t)rt_call(c, FN(F_GCHANDLE_NEW), 2, obj, 0ull);
}
// A copy of the Screen Mode row object `pattern` with a role, appended to a page's row list.
static void append_copy(Cpu *c, uint64_t list, uint64_t pattern, int role) {
    rt_call(c, FN(F_GC_DISABLE), 0);      // the copy is only reachable from host locals until it is in the list
    uint64_t cls = LD64(LD64(FN(LIST_OBJECT_CLASS_SLOT)));
    uint64_t add_mi = LD64(LD64(LD64(cls + 0x20) + 0xc0) + 0x70);
    uint64_t obj = rt_call(c, FN(F_OBJECT_NEW), 1, g_sm_klass);
    if (obj && !c->eh_unwinding) {
        memcpy((void *)(uintptr_t)(obj + 0x10), (const void *)(uintptr_t)(pattern + 0x10), 0x44);
        ST32(obj + 0x54, role);
        set_indices(obj, role_index(role));
        track_copy(c, obj);
        rt_call(c, FN(F_LIST_ADD_WITH_RESIZE), 3, list, obj, add_mi);
    }
    rt_call(c, FN(F_GC_ENABLE), 0);
}

// ------------------------------------------------------------------------------------- Dole Collaboration
// Game 2.20 ended the Dole collaboration: its look (the Dole logo under the start of a stage, bananas with the
// Dole sticker, the main menu's Dole materials) is shown only while a selected character wears a Dole item. The
// game keeps that in one flag, RuntimeParam.mainGame.isDoleCollabActive, and sets it from
// Util.CharaCustomize.IsExistDoleItemSelectedChara / IsExistDoleItemCoordinate (a replay's outfit) whenever the
// characters or their outfits change. The row Options > General > Dole Collaboration (Off by default, `dole=` in
// port_display.txt) answers both questions instead: On shows the look whatever is worn, Off never. A change also
// writes the flag, so the next stage and the next main menu load follow it.
#define F_GET_MAIN_GAME 0x43db8d0ull        // Wonder.RuntimeParam::get_mainGame (static): null before the object exists
#define MAINGAME_DOLE_ACTIVE 0x42
static void set_dole(Cpu *c, int on) {
    g_dole = on != 0;
    save_cfg();
    uint64_t mg = rt_call(c, FN(F_GET_MAIN_GAME), 1, 0ull);
    if (mg && !c->eh_unwinding) ST8(mg + MAINGAME_DOLE_ACTIVE, g_dole);
}
int rt_hook_4444bc0(Cpu *c) { init(); c->x[0] = (uint64_t)g_dole; return 1; }
int rt_hook_443b3b0(Cpu *c) { init(); c->x[0] = (uint64_t)g_dole; return 1; }


// UnityEngine.Screen::set_fullScreenMode(FullScreenMode value): the game's saved value decides windowed or not.
int rt_hook_3ebcda0(Cpu *c) {
    init();
    int windowed = (uint32_t)c->x[0] == 3;
    static int first = 1;
    if (g_deck) ;      // Steam Deck: the port's own setting decides (init_once), not the game's save
    else if (windowed) g_mode = M_WINDOWED;
    else if (g_mode < 0 || g_mode == M_WINDOWED) g_mode = M_BORDERLESS;
    const char *force = getenv("BR_DISPLAY_MODE");      // testing: 0 windowed, 1 borderless, 2 full screen
    if (force && *force >= '0' && *force <= '2') g_mode = *force - '0';
    apply_display(c);
    if (first) { first = 0; apply_vsync(c); apply_fps(c); }
    return 1;
}

// UnityEngine.Application::set_targetFrameRate(int): the game sets 60 or 30; the port's limit wins.
int rt_hook_3ea81e0(Cpu *c) {
    if (t_in_fps) return 0;
    init();
    c->x[0] = (uint64_t)(uint32_t)FPS[g_fps];
    return 0;
}

// UnityEngine.QualitySettings::set_vSyncCount(int): the game applies its own value with every quality change.
int rt_hook_3ec1de0(Cpu *c) {
    if (t_in_vsync) return 0;
    init();
    c->x[0] = (uint64_t)g_vsync;
    return 0;
}

// UnityEngine.QualitySettings::SetQualityLevel(int): graphics and frame rate are the same in every mode on PC.
// The engine takes the level's own vSyncCount. Some of the game's levels (the ones it picks for Battle and for
// split screen with three or four players) carry vSyncCount 2: GraphicsManager.setQualityCore
// reads it back right after and then runs the game at 30 frames per second with the 30 Hz physics mode
// (TimeManager.ChangeFixedUpdateMode(3)), as on the console. Put the port's VSync value back before the game looks,
// so that it always takes its 60 Hz path and the port's frame rate limit applies.
int rt_hook_3ec1ab0(Cpu *c) {
    static __thread int inside;
    if (inside || getenv("BR_KEEP_30")) return 0;      // BR_KEEP_30=1: leave the console behaviour (for comparison)
    init();
    int level = (int32_t)c->x[0];
    {
        static int seen;
        if (level >= 0 && level < 31 && !(seen & (1 << level))) { seen |= 1 << level; rt_log("display: the game selects quality level %d", level); }
    }
    // Levels 2..6 are the console's cut-down settings for online Adventure, split screen and Battle (render scale
    // 0.85 or 1.0 instead of 1.2, no MSAA, 512 shadow map, half the shadow distance or no shadows, simpler renderer).
    // A PC shows those modes like the main game: level 1 (SwitchDefault). The renderer lists of all levels have the
    // same order, so cameras keep finding their renderer. The menu levels (7, 8) stay.
    if (level >= 2 && level <= 6) {
        static int said;
        if (!(said & (1 << level))) { said |= 1 << level; rt_log("display: quality level %d replaced by the main game's level 1", level); }
        c->x[0] = 1;
    }
    inside = 1; call_orig(c, 0x3ec1ab0ull); inside = 0;
    { extern void gfx_quality_changed(Cpu *c); gfx_quality_changed(c); }      // gfx_opt.c: the graphics rows are applied again
    int was = (int32_t)rt_call(c, FN(0x3ec1da0ull), 1, 0ull);        // QualitySettings.get_vSyncCount
    apply_vsync(c);
    if (was != g_vsync) rt_log("display: quality level %d asks for vSyncCount %d; kept at %d", level, was, g_vsync);
    return 1;
}

// setInitIndex(): initial picker index. For the game's row the original runs first (it reads the saved value).
// For the added rows this is only reached from ResetValue(), i.e. "back to default".
int rt_hook_4575bf0(Cpu *c) {
    if (t_in_init) return 0;
    init();
    uint64_t item = c->x[0];
    if (!g_sm_klass) g_sm_klass = LD64(item);
    int role = role_of(item);
    if (role == ROLE_MODE) {      // the game's own row: kept as the pattern for a row on another page (the General page)
        uint64_t sx[19], lr = c->x[30];
        memcpy(sx, c->x, sizeof sx);
        if (g_pattern_h) rt_call(c, FN(F_GCHANDLE_FREE), 1, (uint64_t)g_pattern_h);
        g_pattern_h = (uint32_t)rt_call(c, FN(F_GCHANDLE_NEW), 2, item, 0ull);
        memcpy(c->x, sx, sizeof sx); c->x[30] = lr;
    }
    if (role == ROLE_RES) { g_k = g_def_k; save_cfg(); apply_display(c); ST32(item + 0x50, g_k); return 1; }
    if (role == ROLE_FPS) { g_fps = 1; save_cfg(); apply_fps(c); ST32(item + 0x50, g_fps); return 1; }
    if (role == ROLE_VSYNC) { g_vsync = 1; save_cfg(); apply_vsync(c); ST32(item + 0x50, g_vsync); return 1; }
    if (role == ROLE_SMOOTH) { g_smooth = 1; save_cfg(); ST32(item + 0x50, g_smooth); return 1; }
    if (role == ROLE_DOLE) { set_dole(c, 0); ST32(item + 0x50, 0); return 1; }
    if (role == ROLE_MOUSE) { set_mouse(5); ST32(item + 0x50, 4); return 1; }
    t_in_init = 1; call_orig(c, F_SET_INIT_INDEX); t_in_init = 0;
    if (c->eh_unwinding) return 1;
    if (g_deck) {      // the port's setting decides; make the game's saved value agree, so the row's own text does
        uint64_t list = LD64(item + 0x48), items = list ? LD64(list + 0x10) : 0;
        if (items && (mode_value(item) == MODE_VALUE_WINDOW) != (g_mode == M_WINDOWED))
            ST32(LD64(item + 0x40) + 0x10, LD32(items + 0x20 + 4ull * game_index(item, g_mode == M_WINDOWED)));
    }
    else if (mode_value(item) == MODE_VALUE_WINDOW) g_mode = M_WINDOWED;
    else if (g_mode < 0 || g_mode == M_WINDOWED) g_mode = M_BORDERLESS;
    ST32(item + 0x50, g_mode);
    return 1;
}

// UIStateOption.activate(): it also runs when the Reset button's dialog ("Return all options to their default
// settings?") closes. On Yes it calls ResetValue on every row of the open page's own list
// (m_OptionCategoryDict); the port's rows are copies that exist only in the list on screen, so the game never
// reaches them. Their settings are put back here, before the game's loop; the page is drawn anew afterwards.
// The open page is UIStateOption.m_TabSM's current state (eInnerState, a Nullable).
int rt_hook_4564c70(Cpu *c) {
    uint64_t self = c->x[0], mod = self ? LD64(self + 0x90) : 0, h = mod ? LD64(mod + 0x68) : 0, sm = self ? LD64(self + 0xe0) : 0;
    if (!h || LD32(h + 0x14) != 2 || !sm) return 0;                // m_StateHandleInit.result: 2 = Yes
    uint64_t st = LD64(sm + 0x10);
    if (!(st & 0xff)) return 0;
    init();
    uint64_t sx[31];
    memcpy(sx, c->x, sizeof sx);
    int page = (int)(st >> 32);
    if (page == 0) {                                               // General
        set_dole(c, 0);
    } else if (page == 2) {                                        // Controls
        set_mouse(5);
    } else if (page == 4) {                                        // Display
        g_k = g_def_k; g_fps = 1; g_vsync = 1; g_smooth = 1;
        save_cfg();
        apply_display(c); apply_fps(c); apply_vsync(c);
    }
    for (int i = 0; i < g_ncopy; i++) {
        uint64_t obj = rt_call(c, FN(F_GCHANDLE_TARGET), 1, (uint64_t)g_copy_h[i]);
        if (obj) set_indices(obj, role_index(role_of(obj)));
    }
    rt_log("options: Reset on page %d: the port's rows are back at their defaults", page);
    memcpy(c->x, sx, sizeof sx);
    return 0;
}

// CanBeSelectedToDecrease() / CanBeSelectedToIncrease(): bounds of each row's picker.
int rt_hook_4575d70(Cpu *c) { init(); c->x[0] = (int32_t)LD32(c->x[0] + 0x20) > 0; return 1; }
int rt_hook_4575de0(Cpu *c) {
    init();
    uint64_t item = c->x[0];
    c->x[0] = (int32_t)LD32(item + 0x20) < role_count(role_of(item)) - 1;
    return 1;
}

// ChangeValue(int index)
int rt_hook_4575ea0(Cpu *c) {
    if (t_in_change) return 0;
    init();
    uint64_t item = c->x[0];
    int idx = (int32_t)c->x[1], role = role_of(item), n = role_count(role);
    if (idx < 0) idx = 0;
    if (idx >= n) idx = n - 1;
    if (role == ROLE_RES) { g_k = idx; save_cfg(); apply_display(c); return 1; }
    if (role == ROLE_FPS) { g_fps = idx; save_cfg(); apply_fps(c); return 1; }
    if (role == ROLE_VSYNC) { g_vsync = idx; save_cfg(); apply_vsync(c); return 1; }
    if (role == ROLE_SMOOTH) { g_smooth = idx; save_cfg(); return 1; }
    if (role == ROLE_DOLE) { set_dole(c, idx); return 1; }
    if (role == ROLE_MOUSE) { set_mouse(idx + 1); return 1; }
    g_mode = idx;
    save_cfg();
    // the game's own handler stores its value and ends in set_fullScreenMode, which applies the new mode
    c->x[1] = (uint64_t)game_index(item, idx == M_WINDOWED);
    t_in_change = 1; call_orig(c, F_CHANGE_VALUE); t_in_change = 0;
    return 1;
}

// GetValueStr()
int rt_hook_4575d40(Cpu *c) {
    if (t_in_str) return 0;
    init();
    uint64_t item = c->x[0];
    int role = role_of(item);
    char s[48];
    if (role == ROLE_RES) {
        snprintf(s, sizeof s, "%d x %d", g_res[g_k][0], g_res[g_k][1]);
        c->x[0] = new_str(c, s);
    } else if (role == ROLE_FPS) {
        if (FPS[g_fps] < 0) snprintf(s, sizeof s, "Unlimited");
        else snprintf(s, sizeof s, "%d", FPS[g_fps]);
        c->x[0] = new_str(c, s);
    } else if (role == ROLE_VSYNC) {
        c->x[0] = new_str(c, g_vsync ? "On" : "Off");
    } else if (role == ROLE_SMOOTH) {
        c->x[0] = new_str(c, g_smooth ? "On" : "Off");
    } else if (role == ROLE_MOUSE) {
        snprintf(s, sizeof s, "%d", g_mouse);
        c->x[0] = new_str(c, s);
    } else if (role == ROLE_DOLE) {
        c->x[0] = new_str(c, g_dole ? "On" : "Off");
    } else if (g_mode == M_BORDERLESS) {
        c->x[0] = new_str(c, "Borderless");
    } else {      // the game's localized "Windowed" / "Full Screen"
        t_in_str = 1; call_orig(c, F_GET_VALUE_STR); t_in_str = 0;
    }
    return 1;
}

// UIOptionChoicesItemData::GetContentStr(): the row label; the added rows have their own.
int rt_hook_4534720(Cpu *c) {
    uint64_t item = c->x[0];
    if (!item || !g_sm_klass || LD64(item) != g_sm_klass) return 0;
    int role = role_of(item);
    if (role == ROLE_MOUSE) { c->x[0] = new_str(c, "Mouse Sensitivity"); return 1; }
    if (role != ROLE_DOLE && role != ROLE_SMOOTH && role != ROLE_RES && role != ROLE_FPS && role != ROLE_VSYNC) return 0;
    c->x[0] = new_str(c, role == ROLE_RES ? "Resolution" : role == ROLE_FPS ? "Frame Rate Limit" : role == ROLE_DOLE ? "Dole Collaboration" : role == ROLE_SMOOTH ? "Smooth Motion" : "VSync");
    return 1;
}

// CheckActiveChoicesItemList(): returns the new List<object> of the page's rows. Insert the added rows after the
// game's Screen Mode row.
int rt_hook_455d030(Cpu *c) {
    if (t_in_list) return 0;
    init();
    t_in_list = 1; call_orig(c, F_CHECK_ACTIVE_LIST); t_in_list = 0;
    uint64_t list = c->x[0];
    if (c->eh_unwinding || !list || !g_sm_klass) return 1;
    forget_copies(c);
    uint64_t items = LD64(list + 0x10);
    int n = (int32_t)LD32(list + 0x18), at = -1;
    for (int i = 0; items && i < n; i++) {
        uint64_t it = LD64(items + 0x20 + 8ull * i);
        if (it && LD64(it) == g_sm_klass && role_of(it) == ROLE_MODE) at = i;
    }
    if (at < 0) {
        // Not the Display page. The General page (told by a row of its own) gets the Dole Collaboration row as its
        // last one. BR_OPT_LOG=1 logs the row classes of every page built.
        static int opt_log = -1;
        if (opt_log < 0) opt_log = getenv("BR_OPT_LOG") != NULL;
        int gameplay = 0, controls = 0;
        for (int i = 0; items && i < n; i++) {
            uint64_t it = LD64(items + 0x20 + 8ull * i);
            const char *name = it ? (const char *)(uintptr_t)LD64(LD64(it) + 0x10) : NULL;      // Il2CppClass.name
            if (opt_log) rt_log("options: row %d of %d: %s", i, n, name ? name : "?");
            if (name && strstr(name, GAMEPLAY_ROW)) gameplay = 1;
            if (name && strstr(name, CONTROLS_ROW)) controls = 1;
        }
        uint64_t dole_pattern = gameplay && g_pattern_h ? rt_call(c, FN(F_GCHANDLE_TARGET), 1, (uint64_t)g_pattern_h) : 0;
        if (dole_pattern) append_copy(c, list, dole_pattern, ROLE_DOLE);
        else if (gameplay) rt_log("options: no Screen Mode row seen yet, the Dole Collaboration row is left out");
        // The Controls page (told by its dead zone rows) gets the Mouse Sensitivity row as its last one.
        uint64_t mouse_pattern = controls && g_pattern_h ? rt_call(c, FN(F_GCHANDLE_TARGET), 1, (uint64_t)g_pattern_h) : 0;
        if (mouse_pattern) append_copy(c, list, mouse_pattern, ROLE_MOUSE);
        c->x[0] = list;
        return 1;
    }
    uint64_t real = LD64(items + 0x20 + 8ull * at);
    set_indices(real, role_index(ROLE_MODE));
    // The copies are only reachable from host locals until they are in the list: no collection meanwhile.
    rt_call(c, FN(F_GC_DISABLE), 0);
    uint64_t cls = LD64(LD64(FN(LIST_OBJECT_CLASS_SLOT)));
    uint64_t add_mi = LD64(LD64(LD64(cls + 0x20) + 0xc0) + 0x70);
    static const int roles[] = {ROLE_RES, ROLE_FPS, ROLE_VSYNC, ROLE_SMOOTH};
    int added = 0;
    for (size_t r = 0; r < sizeof roles / sizeof *roles; r++) {
        uint64_t obj = rt_call(c, FN(F_OBJECT_NEW), 1, g_sm_klass);
        if (!obj || c->eh_unwinding) break;
        memcpy((void *)(uintptr_t)(obj + 0x10), (const void *)(uintptr_t)(real + 0x10), 0x44);
        ST32(obj + 0x54, roles[r]);
        set_indices(obj, role_index(roles[r]));
        track_copy(c, obj);
        rt_call(c, FN(F_LIST_ADD_WITH_RESIZE), 3, list, obj, add_mi);
        if (c->eh_unwinding) break;
        added++;
    }
    // move the appended rows up to just after the Screen Mode row
    items = LD64(list + 0x10);
    int total = (int32_t)LD32(list + 0x18);
    if (added && total == n + added) {
        uint64_t tmp[8];
        for (int i = 0; i < added; i++) tmp[i] = LD64(items + 0x20 + 8ull * (n + i));
        for (int i = n - 1; i > at; i--) ST64(items + 0x20 + 8ull * (i + added), LD64(items + 0x20 + 8ull * i));
        for (int i = 0; i < added; i++) ST64(items + 0x20 + 8ull * (at + 1 + i), tmp[i]);
    }
    rt_call(c, FN(F_GC_ENABLE), 0);
    c->x[0] = list;
    return 1;
}

// Is motion to be blended between the 60 Hz steps (interp.c)? When the Smooth Motion row is on and the picture can be
// drawn more than 60 times a second with the current settings; at 60 and below a blend would only add its 16.7 ms
// of delay. With VSync on the engine follows the display and ignores the limit.
int rt_display_above_60(void) {
    static uint64_t at;
    static int hz = 60;
    uint64_t now = GetTickCount64();
    if (now - at > 2000) {
        DEVMODEW dm = {.dmSize = sizeof dm};
        at = now;
        if (EnumDisplaySettingsW(NULL, ENUM_CURRENT_SETTINGS, &dm) && dm.dmDisplayFrequency > 1) hz = (int)dm.dmDisplayFrequency;
    }
    init();
    if (!g_smooth) return 0;
    if (g_vsync) return hz > 62;
    return FPS[g_fps] < 0 || FPS[g_fps] > 60;
}

// ---------------------------------------------------------------------------------------------- measurement
// BR_FPS_LOG=1: Wonder.MultiPlatformManager::LateUpdate runs once per rendered frame on the main thread; every two
// seconds log the measured frame rate and how fast the engine's clocks advance against the wall clock (1.00 means
// game time runs at normal speed whatever the frame rate).
#define F_TIME_GET_TIME 0x3f16020ull
#define F_TIME_GET_FIXED_TIME 0x3f160a0ull
#define F_TIME_GET_FIXED_DELTA 0x3f161e0ull
#define F_TIME_GET_FRAME_COUNT 0x3f163a0ull
static float call_float(Cpu *c, uint64_t off) {
    rt_call(c, FN(off), 1, 0ull);
    float f;
    memcpy(&f, &c->v[0], sizeof f);
    return f;
}
int rt_hook_43d58c0(Cpu *c) {
    static int on = -1;
    static uint64_t t0, frames;
    static float time0, fixed0;
    static uint32_t fc0;
    static double every = 2;
    if (on < 0) { const char *e = getenv("BR_FPS_LOG"); on = e != NULL; if (e && atof(e) > 0.1 && atof(e) != 1) every = atof(e); }   // 1 = every 2 s, else the seconds
    {   // per-frame ticks of the other parts of the port
        extern void chara_dump_tick(Cpu *c);
        extern void window_title_tick(void);
        extern void ultrawide_tick(Cpu *c);
        extern void online_tick(Cpu *c);
        extern void mouse_diag_tick(Cpu *c);
        extern void copy_button_tick(Cpu *c);
        extern void name_ui_tick(Cpu *c);
        extern void pc_input_tick(Cpu *c);
        extern void test_aids_tick(Cpu *c);
        extern void interp_tick(Cpu *c);
        extern void pc_shell_tick(Cpu *c);
        extern void gfx_opt_tick(Cpu *c);
        uint64_t sx[19]; __uint128_t sv[8];
        memcpy(sx, c->x, sizeof sx); memcpy(sv, c->v, sizeof sv);
        chara_dump_tick(c);
        window_title_tick();
        ultrawide_tick(c);
        test_aids_tick(c);
        online_tick(c);
        mouse_diag_tick(c);
        copy_button_tick(c);
        name_ui_tick(c);
        pc_input_tick(c);
        interp_tick(c);
        pc_shell_tick(c);
        gfx_opt_tick(c);
        memcpy(c->x, sx, sizeof sx); memcpy(c->v, sv, sizeof sv);
    }
    if (!on) return 0;
    // How smooth does it look: in how many rendered frames did a camera actually move? (A picture that only changes
    // 60 times a second looks like 60 fps however many frames are drawn.)
    static uint32_t cam_moved, cam_count;
    static float cam_prev[8][3], cam_step[8];
    static uint32_t cam_uneven, cam_pairs;
    static int cam_trace = -1;
    {
        uint64_t sx[19]; __uint128_t sv[8];
        memcpy(sx, c->x, sizeof sx); memcpy(sv, c->v, sizeof sv);
        uint64_t cams = rt_call(c, FN(0x3eaf2f0ull), 1, 0ull);      // Camera.allCameras
        int n = cams ? (int32_t)LD32(cams + 0x18) : 0, moved = 0;
        for (int i = 0; i < n && i < 8; i++) {
            uint64_t cam = LD64(cams + 0x20 + 8ull * i);
            if (!cam) continue;
            uint64_t tr = rt_call(c, FN(0x3f07f60ull), 2, cam, 0ull);  // Component.get_transform
            rt_call(c, FN(0x3f1a1f0ull), 2, tr, 0ull);                 // Transform.get_position
            float p[3];
            for (int k = 0; k < 3; k++) { uint32_t u = (uint32_t)c->v[k]; memcpy(&p[k], &u, 4); }
            if (p[0] != cam_prev[i][0] || p[1] != cam_prev[i][1] || p[2] != cam_prev[i][2]) moved = 1;
            {   // evenness: a step less than half or more than twice the one before it (a frame that repeats the picture counts)
                float dx = p[0] - cam_prev[i][0], dy = p[1] - cam_prev[i][1], dz = p[2] - cam_prev[i][2], d = sqrtf(dx * dx + dy * dy + dz * dz);
                if (cam_step[i] > 1e-4f || d > 1e-4f) { cam_pairs++; if (d < 0.5f * cam_step[i] || d > 2.0f * cam_step[i]) cam_uneven++; }
                if (cam_trace < 0) cam_trace = getenv("BR_CAM_TRACE") != NULL;   // every frame: camera number, step length
                if (cam_trace && (d > 1e-4f || cam_step[i] > 1e-4f)) {   // with the wall time since the frame before, in ms
                    static uint64_t last[8];
                    uint64_t tk = rt_ticks();
                    rt_log("cam %d %.5f %.2f", i, (double)d, (double)(tk - last[i]) / 19200.0);
                    last[i] = tk;
                }
                cam_step[i] = d;
            }
            memcpy(cam_prev[i], p, sizeof p);
        }
        cam_moved += (uint32_t)moved;
        cam_count = (uint32_t)n;
        memcpy(c->x, sx, sizeof sx); memcpy(c->v, sv, sizeof sv);
    }
    uint64_t now = rt_ticks();
    frames++;
    if (!t0 || now - t0 >= (uint64_t)(every * 19200000.0)) {
        uint64_t x0 = c->x[0], x1 = c->x[1];
        __uint128_t v0 = c->v[0];
        float t = call_float(c, F_TIME_GET_TIME), ft = call_float(c, F_TIME_GET_FIXED_TIME), fd = call_float(c, F_TIME_GET_FIXED_DELTA);
        uint32_t fc = (uint32_t)rt_call(c, FN(F_TIME_GET_FRAME_COUNT), 1, 0ull);
        int eng_target = (int32_t)rt_call(c, FN(0x3ea81a0ull), 1, 0ull), eng_vsync = (int32_t)rt_call(c, FN(0x3ec1da0ull), 1, 0ull);
        DWORD fpid = 0;
        HWND fg = GetForegroundWindow();
        if (fg) GetWindowThreadProcessId(fg, &fpid);
        int logic = (int32_t)rt_call(c, FN(0x440d6e0ull), 1, 0ull);   // Wonder.TimeManager.get_fixedUpdateMode: 1 = 60 Hz, 2 = 50 Hz, 3 = 30 Hz
        rt_log("fps: game logic mode %d (1 = 60 Hz, 3 = 30 Hz); engine reports targetFrameRate %d, vSyncCount %d; game window focused: %s", logic, eng_target, eng_vsync,
               fpid == GetCurrentProcessId() ? "yes" : "no");
        if (t0) {
            double wall = (double)(now - t0) / 19200000.0;
            rt_log("fps: %.1f frames/s (engine frame count %.1f/s); game time %.3f s per second, fixed time %.3f s per second "
                   "(%.1f physics steps/s); limit %d, vsync %d", (double)frames / wall, (double)(fc - fc0) / wall,
                   (double)(t - time0) / wall, (double)(ft - fixed0) / wall, fd > 0 ? (double)(ft - fixed0) / wall / fd : 0.0,
                   FPS[g_fps], g_vsync);
            rt_log("fps: a camera moved in %u of %llu frames (%.1f picture changes/s), %u cameras; uneven steps %u of %u", cam_moved,
                   (unsigned long long)frames, (double)cam_moved / wall, cam_count, cam_uneven, cam_pairs);
        }
        cam_moved = cam_uneven = cam_pairs = 0;
        t0 = now; frames = 0; time0 = t; fixed0 = ft; fc0 = fc;
        c->x[0] = x0; c->x[1] = x1; c->v[0] = v0;
    }
    return 0;
}

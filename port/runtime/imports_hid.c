// nn::hid for the Nintendo plug-in glue: one connected Pro-Controller-style npad (NpadId No.1, style FullKey) fed by
// the first XInput controller, with the keyboard as a fallback while a window of this process has the focus.
// Button order follows libnx HidNpadButton (ISC); the approach follows xc2-recomp runtime/input.c / input_win.c
// (MIT, see include/LICENSE.xc2-recomp), reduced to XInput + GetAsyncKeyState.
//
// Guest layouts: Npad*State {s64 samplingNumber; u64 buttons; s32 lx, ly, rx, ry; u32 attributes} (0x28 bytes, the
// same for every style); NpadStyleSet bits: FullKey 0, Handheld 1, JoyDual 2, JoyLeft 3, JoyRight 4;
// VibrationValue {float amplitudeLow, frequencyLow, amplitudeHigh, frequencyHigh};
// SixAxisSensorState 0x60 bytes {s64 delta, s64 sampling, float accel[3], angVel[3], angle[3], direction[9], u32 attr}.
//
// Keyboard: WASD left stick, arrows d-pad, IJKL right stick, Enter/Space A, Backspace/Esc B, Q X, E Y, 1 L, 3 R,
// 2 ZL, 4 ZR, Tab minus, P plus. BR_PAD_LAYOUT=positional maps the XInput face buttons by position instead of label.
// BR_INPUT_SCRIPT=<file>: lines "<ms> <BUTTON> [hold ms]" (A B X Y L R ZL ZR PLUS MINUS LEFT UP RIGHT DOWN) pressed at
// that time after the first state read, ORed into the real input (for headless runs).
#include <windows.h>
#include <xinput.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "rt.h"
#include "pad_sony.h"

enum { B_A, B_B, B_X, B_Y, B_STICK_L, B_STICK_R, B_L, B_R, B_ZL, B_ZR, B_PLUS, B_MINUS, B_LEFT, B_UP, B_RIGHT, B_DOWN,
       B_SL_LEFT, B_SL_UP, B_SL_RIGHT, B_SL_DOWN, B_SR_LEFT, B_SR_UP, B_SR_RIGHT, B_SR_DOWN };
enum { STYLE_FULLKEY = 1, STICK_MAX = 32767, STICK_DIR = 16384 };

typedef struct { uint64_t buttons; int32_t lx, ly, rx, ry; } Pad;
static SRWLOCK g_lock = SRWLOCK_INIT;
static int64_t g_sampling;
static uint32_t g_hold_type;
static uint64_t g_reads;

static int key(int vk) { return (GetAsyncKeyState(vk) & 0x8000) != 0; }
// BR_NO_REAL_INPUT=1 (tests): the real keyboard, mouse and controllers are ignored, so a game driven by
// BR_INPUT_LIVE is not disturbed by whoever is using the PC meanwhile.
int rt_no_real_input(void) {
    static int v = -1;
    if (v < 0) v = getenv("BR_NO_REAL_INPUT") != NULL;
    return v;
}
static int focused(void) {
    HWND w = GetForegroundWindow();
    DWORD pid = 0;
    if (!w || rt_no_real_input()) return 0;
    GetWindowThreadProcessId(w, &pid);
    return pid == GetCurrentProcessId();
}

typedef struct { uint64_t at_ms, hold_ms; int bit, pad; } ScriptEv;
static ScriptEv *g_script;
static int g_nscript, g_script_state;
static uint64_t g_script_t0;
// Pseudo buttons 100..103: left stick fully up / down / left / right (script names LSU LSD LSL LSR).
static const struct { const char *name; int bit; } script_names[] = {
    {"A", B_A}, {"B", B_B}, {"X", B_X}, {"Y", B_Y}, {"L", B_L}, {"R", B_R}, {"ZL", B_ZL}, {"ZR", B_ZR}, {"PLUS", B_PLUS},
    {"MINUS", B_MINUS}, {"LEFT", B_LEFT}, {"UP", B_UP}, {"RIGHT", B_RIGHT}, {"DOWN", B_DOWN},
    {"LSU", 100}, {"LSD", 101}, {"LSL", 102}, {"LSR", 103}};
// A button name may carry a player prefix: "2:A" presses A on the second pad (default: the first).
static int script_pad(const char **name) {
    if ((*name)[0] >= '1' && (*name)[0] <= '4' && (*name)[1] == ':') { int pad = (*name)[0] - '1'; *name += 2; return pad; }
    return 0;
}
// A button press made by the port itself (name_ui.c closes its text box this way): pad No.1 holds A for ms.
static volatile uint64_t g_inject_bits, g_inject_until;
void rt_input_inject_a(int ms) { g_inject_bits = 1ull << B_A; g_inject_until = GetTickCount64() + (uint64_t)ms; }
// BR_INPUT_LIVE=<file>: lines "<BUTTON> [hold ms]" appended to the file while the game runs are pressed one after
// the other as they appear (for driving a running game from a test).
static void script_live(uint64_t now) {
    static const char *path;
    static long offset;
    static uint64_t next_poll, last_end;
    static int init;
    if (!init) { init = 1; path = getenv("BR_INPUT_LIVE"); }
    if (!path || !*path || now < next_poll) return;
    next_poll = now + 50;
    FILE *f = fopen(path, "r");
    if (!f) return;
    char line[128], name[32];
    fseek(f, offset, SEEK_SET);
    while (fgets(line, sizeof line, f)) {
        unsigned long long hold = 100;
        if (!strchr(line, '\n')) break;                              // a line still being written
        offset = ftell(f);
        if (sscanf(line, "%31s %llu", name, &hold) < 1) continue;
        if (!strcmp(name, "TYPE")) {                                 // text for an in-game text box (online_ui.c)
            extern void rt_text_entry_type(const char *s);
            char text[64] = "";
            sscanf(line, "%*s %63s", text);
            rt_text_entry_type(text);
            continue;
        }
        if (!strcmp(name, "CLICK") || !strcmp(name, "MOVE")) {       // a left click at / the cursor moved to window pixel x y, from the top left
            extern void rt_ui_test_click(int x, int y);
            extern void rt_ui_test_move(int x, int y);
            int x = 0, y = 0;
            if (sscanf(line, "%*s %d %d", &x, &y) == 2) { if (name[0] == 'C') rt_ui_test_click(x, y); else rt_ui_test_move(x, y); }
            continue;
        }
        if (!strcmp(name, "KEY")) {                                  // a keyboard key (pc_input.c): KEY <name> [hold ms]
            extern void pc_input_test_key(const char *name, int ms);
            char key[32] = "";
            int ms = 100;
            if (sscanf(line, "%*s %31s %d", key, &ms) >= 1) pc_input_test_key(key, ms);
            continue;
        }
        if (!strcmp(name, "DUMP")) {                                 // screen dump now (ultrawide.c, BR_SCREEN_TREE)
            extern void ultrawide_request_dump(void);
            ultrawide_request_dump();
            continue;
        }
        const char *btn = name;
        int pad = script_pad(&btn);
        for (size_t i = 0; i < sizeof script_names / sizeof *script_names; i++)
            if (!strcmp(btn, script_names[i].name)) {
                uint64_t at = now > last_end + 150 ? now : last_end + 150;
                g_script = realloc(g_script, (size_t)(g_nscript + 1) * sizeof *g_script);
                g_script[g_nscript++] = (ScriptEv){at, hold, script_names[i].bit, pad};
                last_end = at + hold;
            }
    }
    fclose(f);
}
static void script_load(void) {
#define names script_names
    g_script_state = 1;
    g_script_t0 = GetTickCount64();
    const char *path = getenv("BR_INPUT_SCRIPT");
    FILE *f = path && *path ? fopen(path, "r") : NULL;
    if (!f) return;
    char line[128], name[32];
    while (fgets(line, sizeof line, f)) {
        unsigned long long at, hold = 100;
        if (line[0] == '#' || sscanf(line, "%llu %31s %llu", &at, name, &hold) < 2) continue;
        const char *btn = name;
        int pad = script_pad(&btn);
        for (size_t i = 0; i < sizeof names / sizeof *names; i++)
            if (!strcmp(btn, names[i].name)) {
                g_script = realloc(g_script, (size_t)(g_nscript + 1) * sizeof *g_script);
                g_script[g_nscript++] = (ScriptEv){at, hold, names[i].bit, pad};
            }
    }
    fclose(f);
#undef names
    rt_log("input: script %s, %d events", path, g_nscript);
}

// Players and devices. Up to four pads: npad No.1..4 are the connected controllers in this order: XInput users
// 0..3, then a directly read Sony pad. The keyboard and un-prefixed scripted input always belong to No.1, which is
// reported connected even without a controller. BR_PADS=<n> reports n pads regardless of devices (tests).
#define MAX_PADS 4
typedef struct { int xinput; } Dev;           // XInput user index, or -1 for the Sony pad
static XINPUT_STATE g_xs[4];
static int g_xconn[4];
static uint64_t g_xprobe[4], g_xlast;
static Dev g_dev[MAX_PADS];
static int g_ndev, g_npads = 1;
static void devices_refresh(void) {
    uint64_t now = GetTickCount64();
    static int forced = -1;
    if (forced < 0) { const char *e = getenv("BR_PADS"); forced = e ? atoi(e) : 0; if (forced > MAX_PADS) forced = MAX_PADS; }
    AcquireSRWLockExclusive(&g_lock);
    if (now != g_xlast) {                       // at most once a millisecond; an empty XInput slot is probed once a second
        g_xlast = now;
        int n = 0;
        for (int i = 0; i < 4; i++) {
            if (g_xconn[i] || now >= g_xprobe[i]) {
                g_xconn[i] = !rt_no_real_input() && XInputGetState((DWORD)i, &g_xs[i]) == ERROR_SUCCESS;
                if (!g_xconn[i]) g_xprobe[i] = now + 1000;
            }
            if (g_xconn[i] && n < MAX_PADS) g_dev[n++].xinput = i;
        }
        SonyPad sp;
        if (n < MAX_PADS && rt_sony_pad(&sp)) g_dev[n++].xinput = -1;
        if (n != g_ndev) rt_log("input: %d controller%s connected", n, n == 1 ? "" : "s");
        g_ndev = n;
        g_npads = forced > 0 ? forced : n < 1 ? 1 : n;
    }
    ReleaseSRWLockExclusive(&g_lock);
}

// For the glyphs (pc_glyphs.c): what is behind pad `id` (0 nothing, 1 an XInput pad, 2 DualSense, 3 DualShock 4), when
// it last gave input, and whether the face buttons are mapped by position.
static uint64_t g_pad_tick[MAX_PADS];
static int positional_layout(void) {
    static int positional = -1;
    if (positional < 0) { const char *e = getenv("BR_PAD_LAYOUT"); positional = e && !strcmp(e, "positional"); }
    return positional;
}
int rt_pad_kind(uint32_t id) { return id >= (uint32_t)g_ndev ? 0 : g_dev[id].xinput >= 0 ? 1 : rt_sony_kind() ? rt_sony_kind() : 2; }
uint64_t rt_pad_last_input(uint32_t id) { return id < MAX_PADS ? g_pad_tick[id] : 0; }
int rt_pad_positional(void) { return positional_layout(); }

static void poll(Pad *p, uint32_t id) {
    memset(p, 0, sizeof *p);
    uint64_t b = 0;
    int positional = positional_layout();
    devices_refresh();
    int have = id < (uint32_t)g_ndev, xi = have ? g_dev[id].xinput : -2;
    if (xi >= 0) {
        XINPUT_STATE xs = g_xs[xi];
        WORD w = xs.Gamepad.wButtons;
        if (w & XINPUT_GAMEPAD_A) b |= 1ull << (positional ? B_B : B_A);
        if (w & XINPUT_GAMEPAD_B) b |= 1ull << (positional ? B_A : B_B);
        if (w & XINPUT_GAMEPAD_X) b |= 1ull << (positional ? B_Y : B_X);
        if (w & XINPUT_GAMEPAD_Y) b |= 1ull << (positional ? B_X : B_Y);
        if (w & XINPUT_GAMEPAD_LEFT_THUMB) b |= 1ull << B_STICK_L;
        if (w & XINPUT_GAMEPAD_RIGHT_THUMB) b |= 1ull << B_STICK_R;
        if (w & XINPUT_GAMEPAD_LEFT_SHOULDER) b |= 1ull << B_L;
        if (w & XINPUT_GAMEPAD_RIGHT_SHOULDER) b |= 1ull << B_R;
        if (xs.Gamepad.bLeftTrigger > 64) b |= 1ull << B_ZL;
        if (xs.Gamepad.bRightTrigger > 64) b |= 1ull << B_ZR;
        if (w & XINPUT_GAMEPAD_START) b |= 1ull << B_PLUS;
        if (w & XINPUT_GAMEPAD_BACK) b |= 1ull << B_MINUS;
        if (w & XINPUT_GAMEPAD_DPAD_LEFT) b |= 1ull << B_LEFT;
        if (w & XINPUT_GAMEPAD_DPAD_UP) b |= 1ull << B_UP;
        if (w & XINPUT_GAMEPAD_DPAD_RIGHT) b |= 1ull << B_RIGHT;
        if (w & XINPUT_GAMEPAD_DPAD_DOWN) b |= 1ull << B_DOWN;
        #define DZ(v, dz) ((v) > (dz) || (v) < -(dz) ? (v) : 0)
        p->lx = DZ(xs.Gamepad.sThumbLX, XINPUT_GAMEPAD_LEFT_THUMB_DEADZONE);
        p->ly = DZ(xs.Gamepad.sThumbLY, XINPUT_GAMEPAD_LEFT_THUMB_DEADZONE);
        p->rx = DZ(xs.Gamepad.sThumbRX, XINPUT_GAMEPAD_RIGHT_THUMB_DEADZONE);
        p->ry = DZ(xs.Gamepad.sThumbRY, XINPUT_GAMEPAD_RIGHT_THUMB_DEADZONE);
    }
    SonyPad sp;
    if (xi == -1 && rt_sony_pad(&sp)) {      // DualSense / DualShock 4: cross is the bottom button, like A on an Xbox pad
        uint32_t w = sp.buttons;
        if (w & SONY_CROSS) b |= 1ull << (positional ? B_B : B_A);
        if (w & SONY_CIRCLE) b |= 1ull << (positional ? B_A : B_B);
        if (w & SONY_SQUARE) b |= 1ull << (positional ? B_Y : B_X);
        if (w & SONY_TRIANGLE) b |= 1ull << (positional ? B_X : B_Y);
        if (w & SONY_L3) b |= 1ull << B_STICK_L;
        if (w & SONY_R3) b |= 1ull << B_STICK_R;
        if (w & SONY_L1) b |= 1ull << B_L;
        if (w & SONY_R1) b |= 1ull << B_R;
        if (w & SONY_L2) b |= 1ull << B_ZL;
        if (w & SONY_R2) b |= 1ull << B_ZR;
        if (w & SONY_OPTIONS) b |= 1ull << B_PLUS;
        if (w & SONY_CREATE) b |= 1ull << B_MINUS;
        if (w & SONY_LEFT) b |= 1ull << B_LEFT;
        if (w & SONY_UP) b |= 1ull << B_UP;
        if (w & SONY_RIGHT) b |= 1ull << B_RIGHT;
        if (w & SONY_DOWN) b |= 1ull << B_DOWN;
        if (sp.lx) p->lx = sp.lx;
        if (sp.ly) p->ly = sp.ly;
        if (sp.rx) p->rx = sp.rx;
        if (sp.ry) p->ry = sp.ry;
    }
    if (have && id < MAX_PADS && (b || p->lx || p->ly || p->rx || p->ry)) g_pad_tick[id] = GetTickCount64();
    extern int rt_text_entry_active(void);
    extern int pc_keyboard_actions(void);                            // pc_input.c: the keyboard is read as the game's own device
    if (pc_keyboard_actions()) {
    } else if (id == 0 && focused() && rt_text_entry_active()) {      // an in-game text box has the letters, digits and Backspace
        if (key(VK_RETURN)) b |= 1ull << B_A;
        if (key(VK_ESCAPE)) b |= 1ull << B_B;
        if (key(VK_LEFT)) b |= 1ull << B_LEFT;
        if (key(VK_UP)) b |= 1ull << B_UP;
        if (key(VK_RIGHT)) b |= 1ull << B_RIGHT;
        if (key(VK_DOWN)) b |= 1ull << B_DOWN;
    } else if (id == 0 && focused()) {
        if (key(VK_RETURN) || key(VK_SPACE)) b |= 1ull << B_A;
        if (key(VK_BACK) || key(VK_ESCAPE)) b |= 1ull << B_B;
        if (key('Q')) b |= 1ull << B_X;
        if (key('E')) b |= 1ull << B_Y;
        if (key('1')) b |= 1ull << B_L;
        if (key('3')) b |= 1ull << B_R;
        if (key('2')) b |= 1ull << B_ZL;
        if (key('4')) b |= 1ull << B_ZR;
        if (key(VK_TAB)) b |= 1ull << B_MINUS;
        if (key('P')) b |= 1ull << B_PLUS;
        if (key(VK_LEFT)) b |= 1ull << B_LEFT;
        if (key(VK_UP)) b |= 1ull << B_UP;
        if (key(VK_RIGHT)) b |= 1ull << B_RIGHT;
        if (key(VK_DOWN)) b |= 1ull << B_DOWN;
        int x = key('D') - key('A'), y = key('W') - key('S');
        if (x) p->lx = x * STICK_MAX;
        if (y) p->ly = y * STICK_MAX;
        x = key('L') - key('J'); y = key('I') - key('K');
        if (x) p->rx = x * STICK_MAX;
        if (y) p->ry = y * STICK_MAX;
    }
    if (id == 0 && GetTickCount64() < g_inject_until) b |= g_inject_bits;   // rt_input_inject_a
    AcquireSRWLockExclusive(&g_lock);
    if (!g_script_state) script_load();
    uint64_t now = GetTickCount64() - g_script_t0;
    script_live(now);
    for (int i = 0; i < g_nscript; i++)
        if ((uint32_t)g_script[i].pad == id && now >= g_script[i].at_ms && now < g_script[i].at_ms + g_script[i].hold_ms) {
            int bit = g_script[i].bit;
            if (id < MAX_PADS) g_pad_tick[id] = GetTickCount64();   // scripted presses count as the pad being used
            if (bit < 64) b |= 1ull << bit;
            else if (bit == 100) p->ly = STICK_MAX;
            else if (bit == 101) p->ly = -STICK_MAX;
            else if (bit == 102) p->lx = -STICK_MAX;
            else if (bit == 103) p->lx = STICK_MAX;
        }
    ReleaseSRWLockExclusive(&g_lock);
    if (p->lx < -STICK_DIR) b |= 1ull << B_SL_LEFT;
    if (p->lx > STICK_DIR) b |= 1ull << B_SL_RIGHT;
    if (p->ly > STICK_DIR) b |= 1ull << B_SL_UP;
    if (p->ly < -STICK_DIR) b |= 1ull << B_SL_DOWN;
    if (p->rx < -STICK_DIR) b |= 1ull << B_SR_LEFT;
    if (p->rx > STICK_DIR) b |= 1ull << B_SR_RIGHT;
    if (p->ry > STICK_DIR) b |= 1ull << B_SR_UP;
    if (p->ry < -STICK_DIR) b |= 1ull << B_SR_DOWN;
    p->buttons = b;
}

// state for style `style` of npad `id`: the connected pads are FullKey controllers on No.1..
static void get_state(Cpu *c, uint32_t style) {
    uint64_t out = X(0);
    uint32_t id = LD32(X(1));
    memset(PTR(0), 0, 0x28);
    ST64(out, (uint64_t)InterlockedIncrement64(&g_sampling));
    g_reads++;
    if (style != STYLE_FULLKEY || id >= MAX_PADS) return;
    Pad p;
    poll(&p, id);
    if (id >= (uint32_t)g_npads) return;
    ST64(out + 8, p.buttons);
    ST32(out + 0x10, p.lx); ST32(out + 0x14, p.ly); ST32(out + 0x18, p.rx); ST32(out + 0x1c, p.ry);
    ST32(out + 0x20, 3);   // IsConnected | IsWired
}HOSTFN(_ZN2nn3hid14InitializeNpadEv) { (void)c; }
HOSTFN(_ZN2nn3hid24SetSupportedNpadStyleSetENS_4util10BitFlagSetILi32ENS0_12NpadStyleTagEEE) { rt_log("hid: supported npad styles %#x", W(0)); }
HOSTFN(_ZN2nn3hid22SetSupportedNpadIdTypeEPKjm) { (void)c; }
HOSTFN(_ZN2nn3hid15GetNpadStyleSetERKj) { devices_refresh(); RET(LD32(X(0)) < (uint32_t)g_npads ? STYLE_FULLKEY : 0); }
HOSTFN(_ZN2nn3hid12GetNpadStateEPNS0_16NpadFullKeyStateERKj) { get_state(c, STYLE_FULLKEY); }
HOSTFN(_ZN2nn3hid12GetNpadStateEPNS0_17NpadHandheldStateERKj) { get_state(c, 2); }
HOSTFN(_ZN2nn3hid12GetNpadStateEPNS0_16NpadJoyDualStateERKj) { get_state(c, 4); }
HOSTFN(_ZN2nn3hid12GetNpadStateEPNS0_16NpadJoyLeftStateERKj) { get_state(c, 8); }
HOSTFN(_ZN2nn3hid12GetNpadStateEPNS0_17NpadJoyRightStateERKj) { get_state(c, 16); }
HOSTFN(_ZN2nn3hid18SetNpadJoyHoldTypeENS0_15NpadJoyHoldTypeE) { g_hold_type = W(0); }
HOSTFN(_ZN2nn3hid18GetNpadJoyHoldTypeEv) { RET(g_hold_type); }

// Six-axis sensor: one handle for the FullKey pad, at rest.
HOSTFN(_ZN2nn3hid23GetSixAxisSensorHandlesEPNS0_19SixAxisSensorHandleEiRKjNS_4util10BitFlagSetILi32ENS0_12NpadStyleTagEEE) {
    int n = LD32(X(2)) < MAX_PADS && (W(3) & STYLE_FULLKEY) && (int32_t)W(1) > 0;
    if (n) ST32(X(0), 0x00000103 | LD32(X(2)) << 24);   // FullKey, device 1, npad in the top byte (opaque to the game)
    RET(n);
}
HOSTFN(_ZN2nn3hid18StartSixAxisSensorERKNS0_19SixAxisSensorHandleE) { (void)c; }
HOSTFN(_ZN2nn3hid17StopSixAxisSensorERKNS0_19SixAxisSensorHandleE) { (void)c; }
HOSTFN(_ZN2nn3hid21GetSixAxisSensorStateEPNS0_18SixAxisSensorStateERKNS0_19SixAxisSensorHandleE) {
    uint8_t *s = PTR(0);
    float one = 1.0f, g = -1.0f;
    memset(s, 0, 0x60);
    ST64(X(0) + 8, (uint64_t)InterlockedIncrement64(&g_sampling));
    memcpy(s + 0x10 + 8, &g, 4);                                   // acceleration (0, 0, -1): lying flat
    memcpy(s + 0x34, &one, 4); memcpy(s + 0x44, &one, 4); memcpy(s + 0x54, &one, 4);   // identity direction
    ST32(X(0) + 0x58, 1);                                          // IsConnected
}

// Vibration: the two motors of the XInput controller stand in for the left and right linear actuators.
static float g_amp[MAX_PADS][2];
HOSTFN(_ZN2nn3hid25GetVibrationDeviceHandlesEPNS0_21VibrationDeviceHandleEiRKjNS_4util10BitFlagSetILi32ENS0_12NpadStyleTagEEE) {
    int n = 0;
    if (LD32(X(2)) < MAX_PADS && (W(3) & STYLE_FULLKEY))
        for (; n < 2 && n < (int32_t)W(1); n++) ST32(X(0) + 4 * (uint64_t)n, 0x00000003u | (uint32_t)n << 16 | LD32(X(2)) << 24);
    RET(n);
}
HOSTFN(_ZN2nn3hid22GetVibrationDeviceInfoEPNS0_19VibrationDeviceInfoERKNS0_21VibrationDeviceHandleE) {
    ST32(X(0), 1);                                  // LinearResonantActuator
    ST32(X(0) + 4, 1 + ((LD32(X(1)) >> 16) & 1));   // position: 1 left, 2 right
}
HOSTFN(_ZN2nn3hid25InitializeVibrationDeviceERKNS0_21VibrationDeviceHandleE) { (void)c; }
HOSTFN(_ZN2nn3hid18SendVibrationValueERKNS0_21VibrationDeviceHandleERKNS0_14VibrationValueE) {
    float lo, hi;
    memcpy(&lo, PTR(1), 4); memcpy(&hi, (char *)PTR(1) + 8, 4);
    float a = lo > hi ? lo : hi;
    if (a < 0) a = 0;
    if (a > 1) a = 1;
    uint32_t id = (LD32(X(0)) >> 24) & (MAX_PADS - 1);
    g_amp[id][(LD32(X(0)) >> 16) & 1] = a;
    if (id < (uint32_t)g_ndev && g_dev[id].xinput >= 0) {
        XINPUT_VIBRATION v = {(WORD)(g_amp[id][0] * 65535), (WORD)(g_amp[id][1] * 65535)};
        XInputSetState((DWORD)g_dev[id].xinput, &v);
    }
}
HOSTFN(_ZN2nn3hid18VibrationConverterC1Ei) { (void)c; }
HOSTFN(_ZN2nn3hid18VibrationConverterD1Ev) { (void)c; }
HOSTFN(_ZN2nn3hid18VibrationConverter12ProcessChunkEPNS0_14VibrationValueEPKsi) { memset(PTR(1), 0, 16); }
// .bnvib vibration files: u32 metaDataSize (4, or 12 / 16 with a loop), u16 formatId (3), u16 samplingRate (200),
// [u32 loopStart, u32 loopEnd, [u32 loopInterval]], u32 dataSize, then 4 bytes per sample
// {amplitudeLow, frequencyLow, amplitudeHigh, frequencyHigh}: amplitude = v / 255, frequency = 10 * 2^(v / 32) Hz.
// VibrationFileInfo {u32 metaDataSize; u16 formatId; u16 samplingRate; u32 dataSize; s32 sampleLength; s32 isLoop;
// u32 loopStartPosition, loopEndPosition, loopInterval}; the parser context (80 bytes) keeps {samples, count}.
#define HID_VIBRATION_FILE_INVALID 0x1acau   // module 202, description 13: not a vibration file
HOSTFN(_ZN2nn3hid18ParseVibrationFileEPNS0_17VibrationFileInfoEPNS0_26VibrationFileParserContextEPKvm) {
    const uint8_t *d = PTR(2);
    uint64_t size = X(3);
    if (!d || size < 12) { RET(HID_VIBRATION_FILE_INVALID); return; }
    uint32_t meta = LD32(d), loop_start = 0, loop_end = 0, interval = 0;
    if ((meta != 4 && meta != 12 && meta != 16) || LD16(d + 4) != 3 || size < 8ull + meta) { RET(HID_VIBRATION_FILE_INVALID); return; }
    if (meta >= 12) { loop_start = LD32(d + 8); loop_end = LD32(d + 12); }
    if (meta >= 16) interval = LD32(d + 16);
    uint32_t data_size = LD32(d + 4 + meta);
    if (size < 8ull + meta + data_size) { RET(HID_VIBRATION_FILE_INVALID); return; }
    memset(PTR(0), 0, 0x20);
    ST32(X(0), meta); ST16(X(0) + 4, 3); ST16(X(0) + 6, LD16(d + 6)); ST32(X(0) + 8, data_size);
    ST32(X(0) + 12, data_size / 4); ST32(X(0) + 16, meta >= 12);
    ST32(X(0) + 20, loop_start); ST32(X(0) + 24, loop_end); ST32(X(0) + 28, interval);
    memset(PTR(1), 0, 80);
    ST64(X(1), (uint64_t)(uintptr_t)(d + 8 + meta)); ST64(X(1) + 8, data_size / 4);
    RET(0);
}
HOSTFN(_ZN2nn3hid22RetrieveVibrationValueEPNS0_14VibrationValueEiPNS0_26VibrationFileParserContextE) {
    float v[4] = {0, 160.0f, 0, 320.0f};
    uint64_t data = LD64(X(2)), count = LD64(X(2) + 8);
    if (data && (uint64_t)(int32_t)W(1) < count) {
        const uint8_t *smp = (const uint8_t *)(uintptr_t)data + 4ull * W(1);
        v[0] = smp[0] / 255.0f; v[1] = 10.0f * exp2f(smp[1] / 32.0f);
        v[2] = smp[2] / 255.0f; v[3] = 10.0f * exp2f(smp[3] / 32.0f);
    }
    memcpy(PTR(0), v, 16);
}
// The controller applet (players press L+R to join) has no PC counterpart: every connected pad joins, in order.
// ControllerSupportArg {u8 playerCountMin, playerCountMax, ...}; ControllerSupportResultInfo {s8 playerCount;
// u32 selectedId}. With fewer pads than the game asks for, the applet "is cancelled".
#define HID_CONTROLLER_SUPPORT_CANCELED 0x183acau   // module 202, description 3101
HOSTFN(_ZN2nn3hid21ShowControllerSupportEPNS0_27ControllerSupportResultInfoERKNS0_20ControllerSupportArgE) {
    devices_refresh();
    int min = X(1) ? LD8(X(1)) : 1, max = X(1) ? LD8(X(1) + 1) : 1, n = g_npads;
    if (max < 1) max = 1;
    if (n > max) n = max;
    rt_log("hid: controller applet for %d..%d players: %d pad%s", min, max, g_npads, g_npads == 1 ? "" : "s");
    if (n < min) { if (X(0)) { ST32(X(0), 0); ST32(X(0) + 4, 0); } RET(HID_CONTROLLER_SUPPORT_CANCELED); return; }
    if (X(0)) { ST32(X(0), (uint32_t)n); ST32(X(0) + 4, 0); }
    RET(0);
}
uint64_t rt_hid_reads(void) { return g_reads; }

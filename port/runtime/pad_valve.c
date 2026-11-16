// Valve's controllers for pad_hid.c: the Steam Deck's built-in controller (28de:1205) and the Steam Controller of
// 2026 (Triton: 28de:1302 USB, 1303 Bluetooth, 1304 / 1305 its wireless dongles). Report layouts, scales and the
// settings messages follow SDL's HIDAPI drivers (SDL_hidapi_steamdeck.c, SDL_hidapi_steam_triton.c and
// steam/controller_structs.h, zlib licence).
//
// Two roles (rt_hid_pad_set_full, decided in imports_hid.c):
//   * motion only (the usual case): Steam Input already gives the game this controller as a virtual Xbox pad, with
//     its buttons, sticks and rumble. The pad here is no player of its own; its accelerometer is lent to that
//     virtual pad. Nothing is changed on the controller except, for the Steam Controller, that it sends raw
//     motion values (Steam may leave them off).
//   * full pad: no Steam Input (e.g. Windows on a Deck without Steam). Buttons and sticks are read here too, and the
//     controller's keyboard / mouse emulation ("lizard mode") is switched off, as SDL does, or the right trackpad
//     would move the pointer and buttons would type keys. The controller turns it back on by itself a few seconds
//     after the game stops renewing the setting. No rumble in this role yet.
// NOT TESTED on any of these devices (written 2026-11-15 without the hardware): see TECHNICAL.md "Steam Deck and
// Steam Controller".
#include <windows.h>
#include <hidsdi.h>
#include <string.h>
#include "rt.h"
#include "pad_hid_dev.h"

#define PID_DECK 0x1205
static int is_dongle(USHORT pid) { return pid == 0x1304 || pid == 0x1305; }
static int match(USHORT vid, USHORT pid) { return vid == 0x28de && (pid == PID_DECK || (pid >= 0x1302 && pid <= 0x1305)); }
// The Deck and the controllers also show a keyboard and a mouse interface; only the vendor-defined one carries the
// controller. A dongle has one interface per paired controller: 2..5.
static int interface_ok(const char *path, USHORT pid, USHORT usage_page) {
    if (usage_page < 0xff00) return 0;
    if (!is_dongle(pid)) return 1;
    const char *mi = strstr(path, "mi_0");
    if (!mi) mi = strstr(path, "MI_0");
    return mi && mi[4] >= '2' && mi[4] <= '5';
}

enum { ID_CLEAR_DIGITAL_MAPPINGS = 0x81, ID_SET_SETTINGS_VALUES = 0x87 };
enum { SETTING_LEFT_TRACKPAD_MODE = 7, SETTING_RIGHT_TRACKPAD_MODE = 8, SETTING_LIZARD_MODE = 9,
       SETTING_SMOOTH_ABSOLUTE_MOUSE = 24, SETTING_IMU_MODE = 48, SETTING_LEFT_TRACKPAD_CLICK_PRESSURE = 52,
       SETTING_RIGHT_TRACKPAD_CLICK_PRESSURE = 53 };
enum { TRACKPAD_NONE = 7, LIZARD_MODE_OFF = 0, IMU_RAW_ACCEL_AND_GYRO = 0x08 | 0x10 };

// A settings message (type, then 3-byte settings: number, value little-endian) as a feature report. The Deck's
// reports have no number (0 in front), the Steam Controller's are report 1.
static void settings(PadSlot *s, uint8_t type, const uint16_t (*set)[2], int n) {
    uint8_t m[64] = {s->pid == PID_DECK ? 0 : 1, type, (uint8_t)(3 * n)};
    for (int i = 0; i < n && 3 + 3 * i + 2 < (int)sizeof m; i++) {
        m[3 + 3 * i] = (uint8_t)set[i][0];
        m[4 + 3 * i] = (uint8_t)set[i][1]; m[5 + 3 * i] = (uint8_t)(set[i][1] >> 8);
    }
    pad_feature(s, m, 3 + 3 * (DWORD)n);
}
// Renewed every 3 s (SDL: the Steam Controller every 3 s; the Deck's watchdog about every 0.8 s, its timeout is
// longer). Runs on the reader thread between reads.
static void maintain(PadSlot *s) {
    uint64_t now = GetTickCount64();
    int full = s->full != 0;
    if (s->maint_at && now - s->maint_at < 3000 && full == s->maint_full) return;
    s->maint_at = now;
    if (full != s->maint_full) rt_log("input: %s %d: %s", s->drv->name, s->slot, full ? "a pad of its own, keyboard / mouse emulation off" : "lends its motion to Steam Input's pad");
    s->maint_full = full;
    if (s->pid == PID_DECK) {
        if (!full) return;                       // the Deck sends motion always; leave Steam's settings alone
        static const uint16_t off[][2] = {{SETTING_SMOOTH_ABSOLUTE_MOUSE, 0}, {SETTING_LEFT_TRACKPAD_MODE, TRACKPAD_NONE},
            {SETTING_RIGHT_TRACKPAD_MODE, TRACKPAD_NONE}, {SETTING_LEFT_TRACKPAD_CLICK_PRESSURE, 0xffff},
            {SETTING_RIGHT_TRACKPAD_CLICK_PRESSURE, 0xffff}};
        settings(s, ID_CLEAR_DIGITAL_MAPPINGS, NULL, 0);
        settings(s, ID_SET_SETTINGS_VALUES, off, 5);
        return;
    }
    uint16_t set[2][2] = {{SETTING_IMU_MODE, IMU_RAW_ACCEL_AND_GYRO}, {SETTING_LIZARD_MODE, LIZARD_MODE_OFF}};
    settings(s, ID_SET_SETTINGS_VALUES, set, full ? 2 : 1);
}
static void open_pad(PadSlot *s) { s->maint_full = -1; }

static int16_t i16(const uint8_t *d) { return (int16_t)(d[0] | d[1] << 8); }
static uint32_t u32(const uint8_t *d) { return d[0] | d[1] << 8 | d[2] << 16 | (uint32_t)d[3] << 24; }
static int dz(int v) { if (v > 32767) v = 32767; if (v < -32767) v = -32767; return v > 3500 || v < -3500 ? v : 0; }
// Accelerometer x y z at 16384 per g on the controller's axes; HidPad wants x, z, -y at 8192 per g (SDL's turn).
// Gyro the same turn, raw scale halved (2000 degrees a second = 16384; the game does not read it).
static void motion(HidPad *p, const uint8_t *acc, const uint8_t *gyr) {
    p->accel[0] = (int16_t)(i16(acc) / 2); p->accel[1] = (int16_t)(i16(acc + 4) / 2); p->accel[2] = (int16_t)(-i16(acc + 2) / 2);
    p->gyro[0] = (int16_t)(i16(gyr) / 2); p->gyro[1] = (int16_t)(i16(gyr + 4) / 2); p->gyro[2] = (int16_t)(-i16(gyr + 2) / 2);
    p->has_motion = 1;
}

// Steam Deck: input report without a number, 64 bytes: version 1 (u16), type 9, length 64, then the state.
static int deck_report(const uint8_t *d, DWORD n, HidPad *p) {
    if (n < 56 || d[0] != 1 || d[1] != 0 || d[2] != 9) return 0;
    uint32_t l = u32(d + 8), h = u32(d + 12), b = 0;
    if (l & 0x00000080) b |= PAD_SOUTH;          // A
    if (l & 0x00000020) b |= PAD_EAST;           // B
    if (l & 0x00000040) b |= PAD_WEST;           // X
    if (l & 0x00000010) b |= PAD_NORTH;          // Y
    if (l & 0x00000008) b |= PAD_L;
    if (l & 0x00000004) b |= PAD_R;
    if (l & 0x00000002) b |= PAD_ZL;             // trigger pulled through
    if (l & 0x00000001) b |= PAD_ZR;
    if (l & 0x00001000) b |= PAD_SELECT;         // View
    if (l & 0x00004000) b |= PAD_START;          // Menu
    if (l & 0x00400000) b |= PAD_LSTICK;
    if (l & 0x04000000) b |= PAD_RSTICK;
    if (l & 0x00000100) b |= PAD_UP;
    if (l & 0x00000200) b |= PAD_RIGHT;
    if (l & 0x00000400) b |= PAD_LEFT;
    if (l & 0x00000800) b |= PAD_DOWN;
    (void)h;
    if ((uint16_t)(d[44] | d[45] << 8) > 12000) b |= PAD_ZL;      // raw triggers 0..32767
    if ((uint16_t)(d[46] | d[47] << 8) > 12000) b |= PAD_ZR;
    p->buttons = b;
    p->lx = dz(i16(d + 48)); p->ly = dz(i16(d + 50)); p->rx = dz(i16(d + 52)); p->ry = dz(i16(d + 54));   // up positive
    motion(p, d + 24, d + 30);
    p->kind = PAD_KIND_STEAM_DECK;
    return 1;
}
// Steam Controller: numbered reports. 0x42 / 0x45 state, 0x47 state with a short motion time stamp (same offsets
// for sticks and motion), 0x46 / 0x79 a dongle's controller connecting (2) or going (1).
static int triton_report(const uint8_t *r, DWORD n, HidPad *p) {
    if ((r[0] == 0x46 || r[0] == 0x79) && n >= 2) return r[1] == 1 ? 2 : 0;
    if ((r[0] != 0x42 && r[0] != 0x45 && r[0] != 0x47) || n < 1 + 45) return 0;
    const uint8_t *d = r + 1;
    uint32_t w = u32(d + 1), b = 0;
    if (w & 0x00000001) b |= PAD_SOUTH;          // A
    if (w & 0x00000002) b |= PAD_EAST;           // B
    if (w & 0x00000004) b |= PAD_WEST;           // X
    if (w & 0x00000008) b |= PAD_NORTH;          // Y
    if (w & 0x00080000) b |= PAD_L;
    if (w & 0x00000200) b |= PAD_R;
    if (w & 0x08000000) b |= PAD_ZL;             // trigger click
    if (w & 0x00800000) b |= PAD_ZR;
    if (w & 0x00004000) b |= PAD_SELECT;         // as SDL: this bit is Back
    if (w & 0x00000040) b |= PAD_START;          // and this one Start
    if (w & 0x00008000) b |= PAD_LSTICK;
    if (w & 0x00000020) b |= PAD_RSTICK;
    if (w & 0x00002000) b |= PAD_UP;
    if (w & 0x00000800) b |= PAD_RIGHT;
    if (w & 0x00001000) b |= PAD_LEFT;
    if (w & 0x00000400) b |= PAD_DOWN;
    if (i16(d + 5) > 12000) b |= PAD_ZL;
    if (i16(d + 7) > 12000) b |= PAD_ZR;
    p->buttons = b;
    p->lx = dz(i16(d + 9)); p->ly = dz(i16(d + 11)); p->rx = dz(i16(d + 13)); p->ry = dz(i16(d + 15));
    motion(p, d + 33, d + 39);
    p->kind = PAD_KIND_STEAM_CONTROLLER;
    return 1;
}
static int report(PadSlot *s, const uint8_t *rep, DWORD got, HidPad *p) {
    maintain(s);
    int k;
    if (s->pid == PID_DECK) {
        int skip = got == 65 && rep[0] == 0;     // Windows puts report number 0 in front
        k = deck_report(rep + skip, got - (DWORD)skip, p);
    } else k = triton_report(rep, got, p);
    if (k == 2) { memset(p, 0, sizeof *p); return 1; }      // a dongle's controller went away
    if (k != 1) return 0;
    p->connected = 1;
    if (!s->ready) { InterlockedExchange(&s->ready, 1); rt_log("input: %s %d sends its state (motion %d %d %d)", s->drv->name, s->slot, p->accel[0], p->accel[1], p->accel[2]); }
    return 1;
}
static void motors(PadSlot *s, uint8_t low, uint8_t high) { (void)s; (void)low; (void)high; }   // see the top

const PadDriver pad_driver_valve = {"Steam controller", match, open_pad, report, motors, interface_ok};

// PlayStation pads for pad_hid.c: DualSense, DualSense Edge, DualShock 4, USB or Bluetooth. Rumble writes only the
// two motor fields of the output report; lights, triggers and audio stay as they are.
#include <windows.h>
#include <hidsdi.h>
#include <string.h>
#include "rt.h"
#include "pad_hid_dev.h"

static int is_ds4(USHORT pid) { return pid == 0x05c4 || pid == 0x09cc; }
static int is_dualsense(USHORT pid) { return pid == 0x0ce6 || pid == 0x0df2; }
static int match(USHORT vid, USHORT pid) { return vid == 0x054c && (is_ds4(pid) || is_dualsense(pid)); }

static int stick(uint8_t v, int invert) {
    int s = ((int)v - 128) * 258;
    if (invert) s = -s;
    if (s > 32767) s = 32767;
    if (s < -32767) s = -32767;
    return s > 3500 || s < -3500 ? s : 0;       // about 10% dead zone
}
// d points at the left stick X byte; face/btn1 are the two button bytes.
static void decode(HidPad *p, const uint8_t *d, uint8_t face, uint8_t btn1) {
    static const uint32_t hat[9] = {PAD_UP, PAD_UP | PAD_RIGHT, PAD_RIGHT, PAD_RIGHT | PAD_DOWN, PAD_DOWN,
                                    PAD_DOWN | PAD_LEFT, PAD_LEFT, PAD_LEFT | PAD_UP, 0};
    uint32_t b = hat[(face & 15) > 8 ? 8 : (face & 15)];
    if (face & 0x10) b |= PAD_WEST;             // square
    if (face & 0x20) b |= PAD_SOUTH;            // cross
    if (face & 0x40) b |= PAD_EAST;             // circle
    if (face & 0x80) b |= PAD_NORTH;            // triangle
    if (btn1 & 0x01) b |= PAD_L;
    if (btn1 & 0x02) b |= PAD_R;
    if (btn1 & 0x04) b |= PAD_ZL;
    if (btn1 & 0x08) b |= PAD_ZR;
    if (btn1 & 0x10) b |= PAD_SELECT;           // create / share
    if (btn1 & 0x20) b |= PAD_START;            // options
    if (btn1 & 0x40) b |= PAD_LSTICK;
    if (btn1 & 0x80) b |= PAD_RSTICK;
    p->connected = 1; p->buttons = b;
    p->lx = stick(d[0], 0); p->ly = stick(d[1], 1); p->rx = stick(d[2], 0); p->ry = stick(d[3], 1);
}
// Motion sensors: three gyro then three accelerometer values, little-endian int16, at g. Already on the HidPad axes.
static void motion(HidPad *p, const uint8_t *g) {
    for (int i = 0; i < 3; i++) {
        p->gyro[i] = (int16_t)(g[2 * i] | g[2 * i + 1] << 8);
        p->accel[i] = (int16_t)(g[6 + 2 * i] | g[7 + 2 * i] << 8);
    }
    p->has_motion = 1;
}

static void open_pad(PadSlot *s) {
    if (s->bt && is_dualsense(s->pid)) {       // reading this feature report switches a Bluetooth DualSense to full reports
        uint8_t f[64] = {0x05};
        HidD_GetFeature(s->in, f, 41);
    }
}
static int report(PadSlot *s, const uint8_t *rep, DWORD got, HidPad *p) {
    USHORT pid = s->pid;
    int bt = s->bt;
    if (is_dualsense(pid) && rep[0] == 0x01 && !bt) { decode(p, rep + 1, rep[8], rep[9]); if (got >= 28) motion(p, rep + 16); }   // USB
    else if (is_dualsense(pid) && rep[0] == 0x31 && got >= 12) { decode(p, rep + 2, rep[9], rep[10]); if (got >= 29) motion(p, rep + 17); }   // Bluetooth, full
    else if (rep[0] == 0x01) { decode(p, rep + 1, rep[5], rep[6]); if (is_ds4(pid) && !bt && got >= 25) motion(p, rep + 13); }   // DualShock 4 USB, or a Bluetooth pad in basic mode
    else if (is_ds4(pid) && rep[0] == 0x11 && got >= 12) { decode(p, rep + 3, rep[7], rep[8]); if (got >= 27) motion(p, rep + 15); }   // DualShock 4 Bluetooth
    else return 0;
    p->kind = is_ds4(pid) ? PAD_KIND_DUALSHOCK4 : PAD_KIND_DUALSENSE;
    return 1;
}

static uint32_t crc_feed(uint32_t state, const uint8_t *d, size_t n) {      // CRC-32 state (start 0xffffffff, result ~state)
    for (size_t i = 0; i < n; i++) {
        state ^= d[i];
        for (int k = 0; k < 8; k++) state = state >> 1 ^ (0xedb88320u & (0u - (state & 1)));
    }
    return state;
}
static void motors(PadSlot *s, uint8_t low, uint8_t high) {
    uint8_t r[96] = {0};
    DWORD len;
    if (is_dualsense(s->pid)) {
        int o = s->bt ? 2 : 1;               // start of the common part
        if (s->bt) { r[0] = 0x31; r[1] = 0x02; len = 78; } else { r[0] = 0x02; len = 48; }
        r[o] = 0x03;                         // valid: rumble emulation on, haptics off
        r[o + 2] = high; r[o + 3] = low;
    } else if (s->bt) {
        r[0] = 0x11; r[1] = 0xc0; r[3] = 0x01;   // valid: motors only
        r[6] = high; r[7] = low;
        len = 78;
    } else {
        r[0] = 0x05; r[1] = 0x01;
        r[4] = high; r[5] = low;
        len = 32;
    }
    if (s->bt) {                             // Bluetooth reports end with a CRC that starts over the byte 0xa2
        static const uint8_t head = 0xa2;
        uint32_t crc = ~crc_feed(crc_feed(0xffffffffu, &head, 1), r, len - 4);
        r[len - 4] = (uint8_t)crc; r[len - 3] = (uint8_t)(crc >> 8); r[len - 2] = (uint8_t)(crc >> 16); r[len - 1] = (uint8_t)(crc >> 24);
    }
    static volatile LONG said;
    if (pad_write(s, r, len) && InterlockedIncrement(&said) == 1) rt_log("input: Sony pad %d: first rumble report sent (%lu bytes)", s->slot, (unsigned long)len);
}

const PadDriver pad_driver_ps = {"Sony pad", match, open_pad, report, motors};

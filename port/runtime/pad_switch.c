// The Nintendo Switch Pro Controller (057e:2009) for pad_hid.c, USB or Bluetooth. Protocol as documented in
// dekuNukem's Nintendo_Switch_Reverse_Engineering and used by SDL's HIDAPI Switch driver. Over USB it first needs the
// 0x80 handshake; then subcommands (output report 0x01) turn on the motion sensors and vibration, read the factory
// stick calibration from SPI flash, light the player LED and switch to full input reports (0x30). Rumble is output
// report 0x10 with HD-rumble amplitudes. Its face buttons are named and placed like the game expects (A right).
#include <windows.h>
#include <math.h>
#include <string.h>
#include "rt.h"
#include "pad_hid_dev.h"

static int match(USHORT vid, USHORT pid) { return vid == 0x057e && pid == 0x2009; }

static const uint8_t neutral[4] = {0x00, 0x01, 0x40, 0x40};          // 320 Hz / 160 Hz, both silent
static void subcommand(PadSlot *s, uint8_t id, const uint8_t *arg, int n) {
    uint8_t r[64] = {0x01, (uint8_t)(s->counter++ & 15)};
    memcpy(r + 2, neutral, 4); memcpy(r + 6, neutral, 4);
    r[10] = id;
    memcpy(r + 11, arg, (size_t)n);
    pad_write(s, r, 11 + (DWORD)n);
    Sleep(30);
}
static DWORD WINAPI setup(void *arg) {
    PadSlot *s = arg;
    for (int tries = 0; tries < 5 && s->used && !s->ready; tries++) {
        if (!s->bt) {                            // handshake, 3 Mbit, handshake, USB only (no Bluetooth fallback)
            static const uint8_t cmd[] = {0x02, 0x03, 0x02, 0x04};
            for (int i = 0; i < 4; i++) { uint8_t r[2] = {0x80, cmd[i]}; pad_write(s, r, 2); Sleep(30); }
        }
        uint8_t a = 0x01, spi[5] = {0x3d, 0x60, 0x00, 0x00, 18};
        subcommand(s, 0x40, &a, 1);              // motion sensors on
        subcommand(s, 0x48, &a, 1);              // vibration on
        subcommand(s, 0x10, spi, 5);             // factory stick calibration, 0x603d..0x604e
        a = (uint8_t)(1 << s->slot); subcommand(s, 0x30, &a, 1);   // player LED
        a = 0x30; subcommand(s, 0x03, &a, 1);    // full input reports
        for (int i = 0; i < 30 && s->used && !s->ready; i++) Sleep(50);
    }
    if (s->used && !s->ready) rt_log("input: Pro Controller in slot %d sends no input reports", s->slot);
    return 0;
}
static void open_pad(PadSlot *s) {
    for (int k = 0; k < 4; k++) { s->cal[k][0] = 2048; s->cal[k][1] = s->cal[k][2] = 1400; }   // until the calibration is read
    CloseHandle(CreateThread(NULL, 0, setup, s, 0, NULL));
}

// Two 12-bit values packed in three bytes.
static void pair12(const uint8_t *d, uint16_t *a, uint16_t *b) { *a = (uint16_t)(d[0] | (d[1] & 15) << 8); *b = (uint16_t)(d[1] >> 4 | d[2] << 4); }
static void calibration(PadSlot *s, const uint8_t *d) {
    uint16_t v[12];
    for (int i = 0; i < 6; i++) pair12(d + 3 * i, &v[2 * i], &v[2 * i + 1]);
    // left: above x y, centre x y, below x y; right: centre x y, below x y, above x y
    uint16_t c[4][3] = {{v[2], v[4], v[0]}, {v[3], v[5], v[1]}, {v[6], v[8], v[10]}, {v[7], v[9], v[11]}};
    for (int k = 0; k < 4; k++)
        if (c[k][0] < 1000 || c[k][0] > 3000 || c[k][1] < 400 || c[k][1] > 2000 || c[k][2] < 400 || c[k][2] > 2000) {
            rt_log("input: Pro Controller %d: no usable stick calibration, using defaults", s->slot);
            return;
        }
    memcpy(s->cal, c, sizeof c);
    rt_log("input: Pro Controller %d: stick centres %u %u / %u %u", s->slot, c[0][0], c[1][0], c[2][0], c[3][0]);
}
static int stick(uint16_t raw, const uint16_t *c) {
    int v = (int)raw - c[0], s = v * 32767 / (v < 0 ? c[1] : c[2]);
    if (s > 32767) s = 32767;
    if (s < -32767) s = -32767;
    return s > 3500 || s < -3500 ? s : 0;
}
static int report(PadSlot *s, const uint8_t *rep, DWORD got, HidPad *p) {
    if (rep[0] == 0x21 && got >= 38 && rep[14] == 0x10 && rep[15] == 0x3d && rep[16] == 0x60) calibration(s, rep + 20);   // SPI read reply
    if (rep[0] != 0x30 || got < 49) return 0;
    if (!s->ready) { InterlockedExchange(&s->ready, 1); rt_log("input: Pro Controller %d sends full reports", s->slot); }
    uint8_t r = rep[3], m = rep[4], l = rep[5];
    uint32_t b = 0;
    if (r & 0x01) b |= PAD_WEST;                 // Y
    if (r & 0x02) b |= PAD_NORTH;                // X
    if (r & 0x04) b |= PAD_SOUTH;                // B
    if (r & 0x08) b |= PAD_EAST;                 // A
    if (r & 0x40) b |= PAD_R;
    if (r & 0x80) b |= PAD_ZR;
    if (m & 0x01) b |= PAD_SELECT;               // minus
    if (m & 0x02) b |= PAD_START;                // plus
    if (m & 0x04) b |= PAD_RSTICK;
    if (m & 0x08) b |= PAD_LSTICK;
    if (l & 0x01) b |= PAD_DOWN;
    if (l & 0x02) b |= PAD_UP;
    if (l & 0x04) b |= PAD_RIGHT;
    if (l & 0x08) b |= PAD_LEFT;
    if (l & 0x40) b |= PAD_L;
    if (l & 0x80) b |= PAD_ZL;
    uint16_t st[4];
    pair12(rep + 6, &st[0], &st[1]); pair12(rep + 9, &st[2], &st[3]);
    p->connected = 1; p->buttons = b; p->kind = PAD_KIND_SWITCH_PRO;
    p->lx = stick(st[0], s->cal[0]); p->ly = stick(st[1], s->cal[1]);
    p->rx = stick(st[2], s->cal[2]); p->ry = stick(st[3], s->cal[3]);
    // First of the three motion samples: accelerometer x y z, gyro x y z (x forward, y left, z up; 4096 per g).
    // Turned onto the HidPad axes and scale, the same shuffle SDL makes.
    int16_t v[6];
    for (int i = 0; i < 6; i++) v[i] = (int16_t)(rep[13 + 2 * i] | rep[14 + 2 * i] << 8);
    p->accel[0] = pad_clamp16(-2 * v[1]); p->accel[1] = pad_clamp16(2 * v[2]); p->accel[2] = pad_clamp16(-2 * v[0]);
    p->gyro[0] = pad_clamp16(-v[4]); p->gyro[1] = v[5]; p->gyro[2] = pad_clamp16(-v[3]);
    p->has_motion = 1;
    return 1;
}

// HD-rumble amplitude code (0..100) for 0..1, after dekuNukem's formula; below 0.12 linear.
static int amp_code(float a) {
    if (a <= 0.01f) return 0;
    if (a > 1) a = 1;
    float e = a > 0.23f ? log2f(a * 8.7f) * 32 : a > 0.12f ? log2f(a * 17) * 16 : a * 137;
    return e < 0 ? 0 : e > 100 ? 100 : (int)(e + 0.5f);
}
// One side's 4 bytes: high band 320 Hz at `hi`, low band 160 Hz at `lo`.
static void rumble_side(uint8_t *o, float lo, float hi) {
    int ha = amp_code(hi), la = amp_code(lo);
    if (!ha && !la) { memcpy(o, neutral, 4); return; }
    uint16_t lf_amp = (uint16_t)((la & 1 ? 0x8000 : 0) | (la / 2 + 0x40));
    o[0] = 0x00;                                 // high frequency 320 Hz: (log2(320 / 10) * 32 - 0x60) * 4 = 0x100
    o[1] = (uint8_t)(ha * 2 + 0x01);
    o[2] = (uint8_t)(0x40 | (lf_amp >> 8 & 0x80));   // low frequency 160 Hz: log2(160 / 10) * 32 - 0x40 = 0x40
    o[3] = (uint8_t)lf_amp;
}
static void motors(PadSlot *s, uint8_t low, uint8_t high) {      // both sides the same: low band = heavy, high band = light
    uint8_t r[10] = {0x10, (uint8_t)(s->counter++ & 15)};
    rumble_side(r + 2, low / 255.0f, high / 255.0f);
    memcpy(r + 6, r + 2, 4);
    pad_write(s, r, 10);
}

const PadDriver pad_driver_switch = {"Pro Controller", match, open_pad, report, motors};

// Latest state of the directly read Sony pads (pad_sony.c). Sticks are -32767..32767 with up positive.
#pragma once
#include <stdint.h>
enum {
    SONY_CROSS = 1 << 0, SONY_CIRCLE = 1 << 1, SONY_SQUARE = 1 << 2, SONY_TRIANGLE = 1 << 3,
    SONY_L1 = 1 << 4, SONY_R1 = 1 << 5, SONY_L2 = 1 << 6, SONY_R2 = 1 << 7,
    SONY_CREATE = 1 << 8, SONY_OPTIONS = 1 << 9, SONY_L3 = 1 << 10, SONY_R3 = 1 << 11,
    SONY_UP = 1 << 12, SONY_RIGHT = 1 << 13, SONY_DOWN = 1 << 14, SONY_LEFT = 1 << 15,
};
#define SONY_SLOTS 4
typedef struct {
    int connected, kind;                 // kind: 2 DualSense, 3 DualShock 4
    uint32_t buttons;
    int lx, ly, rx, ry;
    int has_motion;
    int16_t gyro[3], accel[3];           // raw sensor values as the pad reports them
} SonyPad;
int rt_sony_pad(int slot, SonyPad *out);                 // 1 when a pad is connected in that slot (0..SONY_SLOTS-1)
void rt_sony_rumble(int slot, float low, float high);    // motor strengths 0..1 (heavy, light)

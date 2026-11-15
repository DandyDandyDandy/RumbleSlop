// Latest state of the pads read directly over HID (pad_hid.c): controllers that are not XInput devices, i.e. Sony
// pads (pad_ps.c) and the Switch Pro Controller (pad_switch.c). Sticks are -32767..32767 with up positive. Face buttons
// are named by position, so a DualSense's cross and a Pro Controller's B are both PAD_SOUTH. Motion values of every pad
// are on one set of axes and one scale: x right, y up out of the face, z toward the player; accelerometer 8192 per g.
#pragma once
#include <stdint.h>
enum {
    PAD_SOUTH = 1 << 0, PAD_EAST = 1 << 1, PAD_WEST = 1 << 2, PAD_NORTH = 1 << 3,
    PAD_L = 1 << 4, PAD_R = 1 << 5, PAD_ZL = 1 << 6, PAD_ZR = 1 << 7,
    PAD_SELECT = 1 << 8, PAD_START = 1 << 9, PAD_LSTICK = 1 << 10, PAD_RSTICK = 1 << 11,
    PAD_UP = 1 << 12, PAD_RIGHT = 1 << 13, PAD_DOWN = 1 << 14, PAD_LEFT = 1 << 15,
};
enum { PAD_KIND_DUALSENSE = 2, PAD_KIND_DUALSHOCK4 = 3, PAD_KIND_SWITCH_PRO = 4 };   // the values of rt_pad_kind
#define HID_PAD_SLOTS 4
typedef struct {
    int connected, kind;                 // kind: PAD_KIND_*
    uint32_t buttons;                    // PAD_* bits
    int lx, ly, rx, ry;
    int has_motion;
    int16_t gyro[3], accel[3];
} HidPad;
int rt_hid_pad(int slot, HidPad *out);                   // 1 when a pad is connected in that slot (0..HID_PAD_SLOTS-1)
void rt_hid_pad_rumble(int slot, float low, float high); // motor strengths 0..1 (heavy, light)

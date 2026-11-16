// Inside pad_hid.c and its device protocols (pad_ps.c, pad_switch.c, pad_valve.c): one slot per connected pad, and what a
// protocol has to provide.
#pragma once
#include <windows.h>
#include "pad_hid.h"

typedef struct PadDriver PadDriver;
typedef struct {
    volatile LONG used;                      // slot taken (reader running)
    char path[400];
    HANDLE in, out;                          // the reader's handle, and a second one for writes
    USHORT vid, pid;
    DWORD in_len, out_len, feat_len;
    int bt, slot;
    const PadDriver *drv;
    HidPad state;
    uint8_t low, high;                       // last motor values sent
    uint64_t sent_at;
    // protocol's own
    uint8_t counter;                         // Switch: packet counter of output reports
    volatile LONG ready;                     // Switch: full input reports arrive
    uint16_t cal[4][3];                      // Switch: sticks lx ly rx ry: centre, range below, range above
    volatile LONG full;                      // Valve: a player of its own (rt_hid_pad_set_full), else motion only
    int maint_full;                          // Valve: the role the last settings were sent for
    uint64_t maint_at;                       // Valve: when they were sent
} PadSlot;

struct PadDriver {
    const char *name;
    int (*match)(USHORT vid, USHORT pid);
    void (*open)(PadSlot *s);                // on the reader thread, before the first read
    // One input report; returns 1 when it filled *p (buttons, sticks, motion, kind).
    int (*report)(PadSlot *s, const uint8_t *rep, DWORD got, HidPad *p);
    void (*motors)(PadSlot *s, uint8_t low, uint8_t high);   // 0..255: heavy, light; called under the slot lock
    // Optional: 0 for an interface of a matching device that is not the controller (keyboard, mouse, a dongle's
    // other interfaces).
    int (*interface_ok)(const char *path, USHORT pid, USHORT usage_page);
};
extern const PadDriver pad_driver_ps, pad_driver_switch, pad_driver_valve;

// Writes an output report padded to the length the driver wants (pad_hid.c).
int pad_write(PadSlot *s, const uint8_t *d, DWORD n);
// Sends a feature report (first byte its number), padded likewise (pad_hid.c).
int pad_feature(PadSlot *s, const uint8_t *d, DWORD n);
int16_t pad_clamp16(int v);

// Pads read directly over HID, because they are not XInput devices: Sony pads (pad_ps.c) and the Switch Pro
// Controller (pad_switch.c). Up to four at once, USB or Bluetooth. A scanner thread looks for pads once a second;
// each pad found gets a slot and a reader thread that keeps its latest state. A slot keeps its number while its pad
// stays connected, so players do not swap when another pad comes or goes; plugging in while the game runs works.
// Rumble: rt_hid_pad_rumble hands the two motor strengths to the pad's protocol, which writes them through a second
// handle, so a write never waits for the reader's pending read. BR_HID_PADS=off (or 0) turns all of this off.
#include <windows.h>
#include <setupapi.h>
#include <hidsdi.h>
#include <stdio.h>
#include <string.h>
#include "rt.h"
#include "pad_hid_dev.h"

static const PadDriver *const g_drivers[] = {&pad_driver_ps, &pad_driver_switch};
static PadSlot g_s[HID_PAD_SLOTS];
static volatile LONG g_started;
static SRWLOCK g_lock = SRWLOCK_INIT;        // state and rumble fields of all slots

int16_t pad_clamp16(int v) { return (int16_t)(v > 32767 ? 32767 : v < -32768 ? -32768 : v); }
int pad_write(PadSlot *s, const uint8_t *d, DWORD n) {
    uint8_t r[512] = {0};                    // the driver wants the full report length (a Bluetooth Pro Controller: 362)
    DWORD len = s->out_len > n && s->out_len <= sizeof r ? s->out_len : n, wrote;
    if (len > sizeof r) return 0;
    memcpy(r, d, n);
    if (WriteFile(s->out, r, len, &wrote, NULL)) return 1;
    static volatile LONG said;
    if (InterlockedIncrement(&said) <= 3) rt_log("input: %s %d: report %02x not accepted (error %lu)", s->drv->name, s->slot, d[0], GetLastError());
    return 0;
}

static DWORD WINAPI reader(void *arg) {
    PadSlot *s = arg;
    uint8_t rep[512];                        // a Pro Controller over Bluetooth has 362-byte reports; a smaller buffer fails the read
    rt_log("input: %s %04x connected over %s (slot %d, report length %lu)", s->drv->name, s->pid, s->bt ? "Bluetooth" : "USB",
           s->slot, (unsigned long)s->in_len);
    s->drv->open(s);
    DWORD got, len = s->in_len < sizeof rep ? s->in_len : sizeof rep;
    while (ReadFile(s->in, rep, len, &got, NULL) && got >= 10) {
        HidPad p = {0};
        if (!s->drv->report(s, rep, got, &p)) continue;
        AcquireSRWLockExclusive(&g_lock);
        s->state = p;
        ReleaseSRWLockExclusive(&g_lock);
    }
    DWORD err = GetLastError();
    AcquireSRWLockExclusive(&g_lock);
    memset(&s->state, 0, sizeof s->state);
    s->low = s->high = 0;
    HANDLE in = s->in, out = s->out;
    s->in = s->out = INVALID_HANDLE_VALUE;
    ReleaseSRWLockExclusive(&g_lock);
    CloseHandle(in);
    if (out != INVALID_HANDLE_VALUE) CloseHandle(out);
    rt_log("input: %s in slot %d disconnected (error %lu)", s->drv->name, s->slot, err);
    s->path[0] = 0;
    InterlockedExchange(&s->used, 0);
    return 0;
}

// Give every connected known pad that has no slot yet a slot and a reader.
static void scan(void) {
    GUID hid;
    HidD_GetHidGuid(&hid);
    HDEVINFO set = SetupDiGetClassDevsA(&hid, NULL, NULL, DIGCF_PRESENT | DIGCF_DEVICEINTERFACE);
    if (set == INVALID_HANDLE_VALUE) return;
    SP_DEVICE_INTERFACE_DATA ifd = {.cbSize = sizeof ifd};
    for (DWORD i = 0; SetupDiEnumDeviceInterfaces(set, NULL, &hid, i, &ifd); i++) {
        union { SP_DEVICE_INTERFACE_DETAIL_DATA_A d; char raw[1024]; } det;
        det.d.cbSize = sizeof(SP_DEVICE_INTERFACE_DETAIL_DATA_A);
        if (!SetupDiGetDeviceInterfaceDetailA(set, &ifd, &det.d, sizeof det, NULL, NULL)) continue;
        int have = 0, free_slot = -1;
        for (int k = 0; k < HID_PAD_SLOTS; k++) {
            if (g_s[k].used && !_stricmp(g_s[k].path, det.d.DevicePath)) have = 1;
            if (!g_s[k].used && free_slot < 0) free_slot = k;
        }
        if (have || free_slot < 0 || strlen(det.d.DevicePath) >= sizeof g_s[0].path) continue;
        HANDLE h = CreateFileA(det.d.DevicePath, GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING, 0, NULL);
        if (h == INVALID_HANDLE_VALUE) continue;
        HIDD_ATTRIBUTES at = {.Size = sizeof at};
        PHIDP_PREPARSED_DATA pp = NULL;
        HIDP_CAPS caps;
        const PadDriver *drv = NULL;
        if (HidD_GetAttributes(h, &at))
            for (size_t d = 0; d < sizeof g_drivers / sizeof *g_drivers && !drv; d++)
                if (g_drivers[d]->match(at.VendorID, at.ProductID)) drv = g_drivers[d];
        int ok = drv && HidD_GetPreparsedData(h, &pp) && HidP_GetCaps(pp, &caps) == HIDP_STATUS_SUCCESS && caps.InputReportByteLength >= 10;
        if (pp) HidD_FreePreparsedData(pp);
        if (!ok) { CloseHandle(h); continue; }
        PadSlot *s = &g_s[free_slot];
        AcquireSRWLockExclusive(&g_lock);
        memset(s, 0, sizeof *s);
        snprintf(s->path, sizeof s->path, "%s", det.d.DevicePath);
        s->in = h;
        s->out = CreateFileA(det.d.DevicePath, GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING, 0, NULL);
        s->vid = at.VendorID; s->pid = at.ProductID; s->drv = drv;
        s->in_len = caps.InputReportByteLength; s->out_len = caps.OutputReportByteLength;
        s->bt = caps.InputReportByteLength > 64;   // USB reports are 64 bytes; Bluetooth ones longer (78, 362)
        s->slot = free_slot;
        ReleaseSRWLockExclusive(&g_lock);
        InterlockedExchange(&s->used, 1);
        CloseHandle(CreateThread(NULL, 0, reader, s, 0, NULL));
    }
    SetupDiDestroyDeviceInfoList(set);
}
static DWORD WINAPI scanner(void *unused) {
    (void)unused;
    for (;;) { scan(); Sleep(1000); }
    return 0;
}
static void start(void) {
    if (InterlockedExchange(&g_started, 1)) return;
    for (int k = 0; k < HID_PAD_SLOTS; k++) g_s[k].in = g_s[k].out = INVALID_HANDLE_VALUE;
    const char *e = getenv("BR_HID_PADS");
    if (!e) e = getenv("BR_SONY_PAD");       // the old name
    if (!(e && (!strcmp(e, "off") || !strcmp(e, "0")))) CloseHandle(CreateThread(NULL, 0, scanner, NULL, 0, NULL));
}

int rt_hid_pad(int slot, HidPad *out) {
    start();
    if (slot < 0 || slot >= HID_PAD_SLOTS) { memset(out, 0, sizeof *out); return 0; }
    AcquireSRWLockShared(&g_lock);
    *out = g_s[slot].state;
    ReleaseSRWLockShared(&g_lock);
    return out->connected;
}
// Motor strengths 0..1. Sent when they change, and again every half second while a motor runs (a Bluetooth pad
// stops by itself otherwise).
void rt_hid_pad_rumble(int slot, float low, float high) {
    if (slot < 0 || slot >= HID_PAD_SLOTS) return;
    PadSlot *s = &g_s[slot];
    uint8_t l = (uint8_t)(low <= 0 ? 0 : low >= 1 ? 255 : low * 255.0f + 0.5f), h = (uint8_t)(high <= 0 ? 0 : high >= 1 ? 255 : high * 255.0f + 0.5f);
    uint64_t now = GetTickCount64();
    AcquireSRWLockExclusive(&g_lock);
    int send = s->used && s->out != INVALID_HANDLE_VALUE && s->state.connected &&
               (l != s->low || h != s->high || ((l || h) && now - s->sent_at >= 500));
    if (send) { s->low = l; s->high = h; s->sent_at = now; s->drv->motors(s, l, h); }
    ReleaseSRWLockExclusive(&g_lock);
}

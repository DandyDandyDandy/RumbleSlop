// Sony pads (DualSense, DualSense Edge, DualShock 4) read directly over HID, because they are not XInput devices.
// Up to four at once, USB or Bluetooth. A scanner thread looks for pads once a second; each pad found gets a slot
// and a reader thread that keeps its latest state. A slot keeps its number while its pad stays connected, so
// players do not swap when another pad comes or goes; plugging in while the game runs works.
// Rumble: rt_sony_rumble sends the two motor strengths in the pad's output report (through a second handle, so a
// write never waits for the reader's pending read). Nothing else of the report is marked valid: lights, triggers
// and audio stay as they are.
#include <windows.h>
#include <setupapi.h>
#include <hidsdi.h>
#include <stdio.h>
#include <string.h>
#include "rt.h"
#include "pad_sony.h"

#define SLOTS SONY_SLOTS
typedef struct {
    volatile LONG used;                      // slot taken (reader running)
    char path[400];
    HANDLE in, out;
    USHORT pid;
    DWORD in_len, out_len;
    int bt, slot;
    SonyPad state;
    uint8_t low, high;                       // last motor values sent
    uint64_t sent_at;
} Slot;
static Slot g_s[SLOTS];
static volatile LONG g_started;
static SRWLOCK g_lock = SRWLOCK_INIT;        // state and rumble fields of all slots

static int is_ds4(USHORT pid) { return pid == 0x05c4 || pid == 0x09cc; }
static int is_dualsense(USHORT pid) { return pid == 0x0ce6 || pid == 0x0df2; }

static int stick(uint8_t v, int invert) {
    int s = ((int)v - 128) * 258;
    if (invert) s = -s;
    if (s > 32767) s = 32767;
    if (s < -32767) s = -32767;
    return s > 3500 || s < -3500 ? s : 0;       // about 10% dead zone
}
// d points at the left stick X byte; face/btn1 are the two button bytes.
static void decode(SonyPad *p, const uint8_t *d, uint8_t face, uint8_t btn1) {
    static const uint32_t hat[9] = {SONY_UP, SONY_UP | SONY_RIGHT, SONY_RIGHT, SONY_RIGHT | SONY_DOWN, SONY_DOWN,
                                    SONY_DOWN | SONY_LEFT, SONY_LEFT, SONY_LEFT | SONY_UP, 0};
    uint32_t b = hat[(face & 15) > 8 ? 8 : (face & 15)];
    if (face & 0x10) b |= SONY_SQUARE;
    if (face & 0x20) b |= SONY_CROSS;
    if (face & 0x40) b |= SONY_CIRCLE;
    if (face & 0x80) b |= SONY_TRIANGLE;
    if (btn1 & 0x01) b |= SONY_L1;
    if (btn1 & 0x02) b |= SONY_R1;
    if (btn1 & 0x04) b |= SONY_L2;
    if (btn1 & 0x08) b |= SONY_R2;
    if (btn1 & 0x10) b |= SONY_CREATE;
    if (btn1 & 0x20) b |= SONY_OPTIONS;
    if (btn1 & 0x40) b |= SONY_L3;
    if (btn1 & 0x80) b |= SONY_R3;
    p->connected = 1; p->buttons = b;
    p->lx = stick(d[0], 0); p->ly = stick(d[1], 1); p->rx = stick(d[2], 0); p->ry = stick(d[3], 1);
}
// Motion sensors: three gyro then three accelerometer values, little-endian int16, at g.
static void motion(SonyPad *p, const uint8_t *g) {
    for (int i = 0; i < 3; i++) {
        p->gyro[i] = (int16_t)(g[2 * i] | g[2 * i + 1] << 8);
        p->accel[i] = (int16_t)(g[6 + 2 * i] | g[7 + 2 * i] << 8);
    }
    p->has_motion = 1;
}

static uint32_t crc_feed(uint32_t state, const uint8_t *d, size_t n) {      // CRC-32 state (start 0xffffffff, result ~state)
    for (size_t i = 0; i < n; i++) {
        state ^= d[i];
        for (int k = 0; k < 8; k++) state = state >> 1 ^ (0xedb88320u & (0u - (state & 1)));
    }
    return state;
}
// The output report with the two motors (0..255: low = the heavy left motor, high = the light right one).
static void send_motors(Slot *s, uint8_t low, uint8_t high) {
    uint8_t r[96] = {0};
    DWORD len, wrote;
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
    if (s->out_len > len && s->out_len <= sizeof r) len = s->out_len;      // the driver wants the full report length
    if (!WriteFile(s->out, r, len, &wrote, NULL)) {
        static volatile LONG said;
        if (InterlockedIncrement(&said) <= 3) rt_log("input: Sony pad %d: rumble report not accepted (error %lu)", s->slot, GetLastError());
    } else {
        static volatile LONG said;
        if (InterlockedIncrement(&said) == 1) rt_log("input: Sony pad %d: first rumble report sent (%lu bytes)", s->slot, (unsigned long)len);
    }
}

static DWORD WINAPI reader(void *arg) {
    Slot *s = arg;
    uint8_t rep[256];
    USHORT pid = s->pid;
    int bt = s->bt;
    if (bt && is_dualsense(pid)) {           // reading this feature report switches a Bluetooth DualSense to full reports
        uint8_t f[64] = {0x05};
        HidD_GetFeature(s->in, f, 41);
    }
    rt_log("input: Sony pad %04x connected over %s (slot %d, report length %lu)", pid, bt ? "Bluetooth" : "USB", s->slot, (unsigned long)s->in_len);
    DWORD got, len = s->in_len < sizeof rep ? s->in_len : sizeof rep;
    while (ReadFile(s->in, rep, len, &got, NULL) && got >= 10) {
        SonyPad p = {0};
        if (is_dualsense(pid) && rep[0] == 0x01 && !bt) { decode(&p, rep + 1, rep[8], rep[9]); if (got >= 28) motion(&p, rep + 16); }   // USB
        else if (is_dualsense(pid) && rep[0] == 0x31 && got >= 12) { decode(&p, rep + 2, rep[9], rep[10]); if (got >= 29) motion(&p, rep + 17); }   // Bluetooth, full
        else if (rep[0] == 0x01) { decode(&p, rep + 1, rep[5], rep[6]); if (is_ds4(pid) && !bt && got >= 25) motion(&p, rep + 13); }   // DualShock 4 USB, or a Bluetooth pad in basic mode
        else if (is_ds4(pid) && rep[0] == 0x11 && got >= 12) { decode(&p, rep + 3, rep[7], rep[8]); if (got >= 27) motion(&p, rep + 15); }   // DualShock 4 Bluetooth
        else continue;
        p.kind = is_ds4(pid) ? 3 : 2;
        AcquireSRWLockExclusive(&g_lock);
        s->state = p;
        ReleaseSRWLockExclusive(&g_lock);
    }
    AcquireSRWLockExclusive(&g_lock);
    memset(&s->state, 0, sizeof s->state);
    s->low = s->high = 0;
    HANDLE in = s->in, out = s->out;
    s->in = s->out = INVALID_HANDLE_VALUE;
    ReleaseSRWLockExclusive(&g_lock);
    CloseHandle(in);
    if (out != INVALID_HANDLE_VALUE) CloseHandle(out);
    rt_log("input: Sony pad in slot %d disconnected", s->slot);
    s->path[0] = 0;
    InterlockedExchange(&s->used, 0);
    return 0;
}

// Give every connected Sony pad that has no slot yet a slot and a reader.
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
        for (int k = 0; k < SLOTS; k++) {
            if (g_s[k].used && !_stricmp(g_s[k].path, det.d.DevicePath)) have = 1;
            if (!g_s[k].used && free_slot < 0) free_slot = k;
        }
        if (have || free_slot < 0 || strlen(det.d.DevicePath) >= sizeof g_s[0].path) continue;
        HANDLE h = CreateFileA(det.d.DevicePath, GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING, 0, NULL);
        if (h == INVALID_HANDLE_VALUE) continue;
        HIDD_ATTRIBUTES at = {.Size = sizeof at};
        PHIDP_PREPARSED_DATA pp = NULL;
        HIDP_CAPS caps;
        int ok = HidD_GetAttributes(h, &at) && at.VendorID == 0x054c && (is_ds4(at.ProductID) || is_dualsense(at.ProductID)) &&
                 HidD_GetPreparsedData(h, &pp) && HidP_GetCaps(pp, &caps) == HIDP_STATUS_SUCCESS && caps.InputReportByteLength >= 10;
        if (pp) HidD_FreePreparsedData(pp);
        if (!ok) { CloseHandle(h); continue; }
        Slot *s = &g_s[free_slot];
        AcquireSRWLockExclusive(&g_lock);
        snprintf(s->path, sizeof s->path, "%s", det.d.DevicePath);
        s->in = h;
        s->out = CreateFileA(det.d.DevicePath, GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING, 0, NULL);
        s->pid = at.ProductID; s->in_len = caps.InputReportByteLength; s->out_len = caps.OutputReportByteLength;
        s->bt = caps.InputReportByteLength > 64;
        s->slot = free_slot; s->low = s->high = 0; s->sent_at = 0;
        memset(&s->state, 0, sizeof s->state);
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
    for (int k = 0; k < SLOTS; k++) g_s[k].in = g_s[k].out = INVALID_HANDLE_VALUE;
    const char *e = getenv("BR_SONY_PAD");
    if (!(e && (!strcmp(e, "off") || !strcmp(e, "0")))) CloseHandle(CreateThread(NULL, 0, scanner, NULL, 0, NULL));
}

int rt_sony_pad(int slot, SonyPad *out) {
    start();
    if (slot < 0 || slot >= SLOTS) { memset(out, 0, sizeof *out); return 0; }
    AcquireSRWLockShared(&g_lock);
    *out = g_s[slot].state;
    ReleaseSRWLockShared(&g_lock);
    return out->connected;
}
// Motor strengths 0..1. Sent when they change, and again every half second while a motor runs (a Bluetooth pad
// stops by itself otherwise).
void rt_sony_rumble(int slot, float low, float high) {
    if (slot < 0 || slot >= SLOTS) return;
    Slot *s = &g_s[slot];
    uint8_t l = (uint8_t)(low <= 0 ? 0 : low >= 1 ? 255 : low * 255.0f + 0.5f), h = (uint8_t)(high <= 0 ? 0 : high >= 1 ? 255 : high * 255.0f + 0.5f);
    uint64_t now = GetTickCount64();
    AcquireSRWLockExclusive(&g_lock);
    int send = s->used && s->out != INVALID_HANDLE_VALUE && s->state.connected &&
               (l != s->low || h != s->high || ((l || h) && now - s->sent_at >= 500));
    if (send) { s->low = l; s->high = h; s->sent_at = now; send_motors(s, l, h); }
    ReleaseSRWLockExclusive(&g_lock);
}

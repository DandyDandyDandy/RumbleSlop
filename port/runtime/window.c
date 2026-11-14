// The game window's title and icon. The game's player settings name the product by its code name ("Wonder"), which
// the engine puts in the title bar; the port shows its own name instead (GAME_NAME, given by the build) and the
// exe's icon. Both are set by a short-lived thread as soon as the window exists (window_early_start, from the
// runtime's start), about five seconds before the game's first frame; the per-frame tick stays as a second chance.
// The window is white during those seconds: that is the engine presenting an empty picture, not an unpainted
// window (painting it black from here, by the class brush and directly, changes nothing on the screen).
#include <windows.h>
#include "rt.h"

#ifndef GAME_NAME
#define GAME_NAME "RumbleSlop"
#endif

static volatile LONG g_done;

static BOOL CALLBACK find_main(HWND h, LPARAM out) {
    DWORD pid = 0;
    wchar_t cls[64] = L"";
    GetWindowThreadProcessId(h, &pid);
    if (pid != GetCurrentProcessId() || GetWindow(h, GW_OWNER)) return TRUE;
    GetClassNameW(h, cls, 64);
    if (wcscmp(cls, L"UnityWndClass")) return TRUE;      // a dialog or the text box, not the game window
    *(HWND *)out = h;
    return FALSE;
}
static int setup(int early) {
    HWND w = NULL;
    EnumWindows(find_main, (LPARAM)&w);
    if (!w) return 0;
    if (InterlockedExchange(&g_done, 1)) return 1;
    SetWindowTextW(w, L"" GAME_NAME);
    // the program icon (resource 103 of the exe, port\launcher\launcher.rc) in the title bar and the task bar
    HINSTANCE exe = GetModuleHandleW(NULL);
    HICON big = LoadImageW(exe, MAKEINTRESOURCEW(103), IMAGE_ICON, GetSystemMetrics(SM_CXICON), GetSystemMetrics(SM_CYICON), 0);
    HICON small = LoadImageW(exe, MAKEINTRESOURCEW(103), IMAGE_ICON, GetSystemMetrics(SM_CXSMICON), GetSystemMetrics(SM_CYSMICON), 0);
    if (big) SendMessageTimeoutW(w, WM_SETICON, ICON_BIG, (LPARAM)big, SMTO_ABORTIFHUNG, 200, NULL);
    if (small) SendMessageTimeoutW(w, WM_SETICON, ICON_SMALL, (LPARAM)small, SMTO_ABORTIFHUNG, 200, NULL);
    rt_log("window: title set to %s%s", GAME_NAME, early ? " (as soon as the window existed)" : "");
    return 1;
}
static DWORD WINAPI early(void *unused) {
    (void)unused;
    for (int i = 0; i < 2000 && !setup(1); i++) Sleep(10);      // up to 20 s; without a window (batch mode) it just ends
    return 0;
}
void window_early_start(void) { CloseHandle(CreateThread(NULL, 0, early, NULL, 0, NULL)); }

// called once per frame from the per-frame hook in display_opt.c
void window_title_tick(void) {
    static int tries;
    if (g_done || (++tries & 15)) return;          // look every 16th frame until the window is there
    setup(0);
}

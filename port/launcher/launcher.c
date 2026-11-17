// The game's executable: a small program that starts Unity's Windows player (UnityPlayer.dll) the way Unity's own
// stub does, with the options this port needs.
//
//  * -force-vulkan is added unless the command line already names a graphics API: the converted shaders only carry
//    Vulkan programs.
//  * The player finds its data in "<name of this exe>_Data" next to the exe, and loads GameAssembly.dll (the
//    recompiled game) from the exe's folder.
//  * Unity's player log is off (-nolog) when playing. --log turns it on as player.log in the game folder, together
//    with the port's own port.log (log.c); a command line with its own -logFile keeps that.
#include <windows.h>
#include <wchar.h>

typedef int (*UnityMainFn)(HINSTANCE, HINSTANCE, LPWSTR, int);

static int has_arg(const wchar_t *cmd, const wchar_t *arg) {
    size_t n = wcslen(arg);
    for (const wchar_t *p = cmd; (p = wcsstr(p, arg)) != NULL; p += n)
        if ((p == cmd || p[-1] == L' ' || p[-1] == L'\t') && (p[n] == 0 || p[n] == L' ' || p[n] == L'\t')) return 1;
    return 0;
}

int WINAPI wWinMain(HINSTANCE inst, HINSTANCE prev, LPWSTR cmd, int show) {
    static wchar_t dir[1024], dll[1100], args[8192];
    DWORD n = GetModuleFileNameW(NULL, dir, 1000);
    while (n && dir[n - 1] != L'\\' && dir[n - 1] != L'/') n--;
    dir[n] = 0;
    _snwprintf(dll, 1100, L"%lsUnityPlayer.dll", dir);
    HMODULE player = LoadLibraryExW(dll, NULL, LOAD_WITH_ALTERED_SEARCH_PATH);
    UnityMainFn unity_main = player ? (UnityMainFn)(void *)GetProcAddress(player, "UnityMain") : NULL;
    if (!unity_main) {
        MessageBoxW(NULL, L"UnityPlayer.dll is missing or damaged. Run setup again.", L"" GAME_NAME_W, MB_OK | MB_ICONERROR);
        return 1;
    }
    static const wchar_t *apis[] = {L"-force-vulkan", L"-force-d3d11", L"-force-d3d12", L"-force-glcore", L"-nographics"};
    int chosen = 0;
    for (int i = 0; i < 5; i++) chosen |= has_arg(cmd ? cmd : L"", apis[i]);
    static wchar_t log_args[1200];
    if (has_arg(cmd ? cmd : L"", L"-logFile") || has_arg(cmd ? cmd : L"", L"-nolog")) log_args[0] = 0;
    else if (has_arg(cmd ? cmd : L"", L"--log")) _snwprintf(log_args, 1200, L" -logFile \"%lsplayer.log\"", dir);
    else _snwprintf(log_args, 1200, L" -nolog");
    _snwprintf(args, 8190, L"%ls%ls%ls", cmd ? cmd : L"", chosen ? L"" : L" -force-vulkan", log_args);
    return unity_main(inst, prev, args, show);
}

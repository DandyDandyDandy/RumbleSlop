// Switch platform services behind the Nintendo plug-in glue (nn.dll, nnUserPlugin.Runtime.dll) and the CRIWARE
// platform layer: account, add-on content, shop, error viewer, language, time, network status, software keyboard,
// sockets, nn::mem, assertions. Nothing here talks to a Horizon service; each call is answered by the policy below.
// The policies follow xc2-recomp runtime/ov_system.c and swkbd.c (MIT, see include/LICENSE.xc2-recomp):
//   account   one local user, always open-able; no Nintendo Account (network service account unavailable)
//   aoc / ec  add-on content is what <exe dir>dlc\ holds (one folder per index); the shop does nothing
//   oe        language from BR_LANG (default en-US); docked / performance mode is the engine's business
//   nifm      no network
//   swkbd     a Windows text box (swkbd_win.c)
//   time      the host clock
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "rt.h"

// ------------------------------------------------------------------------------------------------ account
// Uid {u64 lo, hi}; UserHandle {Uid, u64 token} (24 bytes); Nickname char[33].
#define UID_LO 0x10000000ba7a7a01ull
#define UID_HI 0x0000000000000001ull
#define ACCOUNT_CANCELLED 0x7cu   // module 124, description 0
HOSTFN(_ZN2nn7account10InitializeEv) { (void)c; }
HOSTFN(_ZN2nn7account17GetLastOpenedUserEPNS0_3UidE) { ST64(X(0), UID_LO); ST64(X(0) + 8, UID_HI); RET(0); }
HOSTFN(_ZN2nn7account11GetNicknameEPNS0_8NicknameERKNS0_3UidE) {
    extern const char *rt_player_name(void);                        // name_ui.c: the saved name, BR_NICKNAME or "Player"
    memset(PTR(0), 0, 33);
    snprintf(PTR(0), 33, "%s", rt_player_name());
    RET(0);
}
HOSTFN(_ZN2nn7account22TryOpenPreselectedUserEPNS0_10UserHandleE) {
    ST64(X(0), UID_LO); ST64(X(0) + 8, UID_HI); ST64(X(0) + 16, 1);
    RET(1);
}
HOSTFN(_ZN2nn7account9CloseUserERKNS0_10UserHandleE) { (void)c; }
HOSTFN(_ZN2nn7account9GetUserIdEPNS0_3UidERKNS0_10UserHandleE) { ST64(X(0), UID_LO); ST64(X(0) + 8, UID_HI); RET(0); }
HOSTFN(_ZN2nn7account36EnsureNetworkServiceAccountAvailableERKNS0_10UserHandleE) { RET(ACCOUNT_CANCELLED); }

// ------------------------------------------------------------------------------------------------ aoc / ec / err
// Installed add-on content = the numbered folders in <exe dir>dlc\ (decimal add-on content index, made from the
// user's own DLC dumps by port/assets/install_dlc.py). Scanned once; nn::fs::MountAddOnContent mounts the folder.
#define AOC_MAX 256
static int g_aoc[AOC_MAX], g_aoc_n = -1;
static int aoc_cmp(const void *a, const void *b) { return *(const int *)a - *(const int *)b; }
static int aoc_scan(void) {
    static SRWLOCK lock = SRWLOCK_INIT;
    AcquireSRWLockExclusive(&lock);
    if (g_aoc_n < 0) {
        char pat[600];
        WIN32_FIND_DATAA fd;
        int n = 0;
        snprintf(pat, sizeof pat, "%sdlc\\*", rt_exe_dir());
        HANDLE h = FindFirstFileA(pat, &fd);
        if (h != INVALID_HANDLE_VALUE) {
            do {
                char *end;
                long v = strtol(fd.cFileName, &end, 10);
                if ((fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) && end != fd.cFileName && !*end && v > 0 && n < AOC_MAX) g_aoc[n++] = (int)v;
            } while (FindNextFileA(h, &fd));
            FindClose(h);
        }
        qsort(g_aoc, (size_t)n, sizeof g_aoc[0], aoc_cmp);
        rt_log("aoc: %d add-on content folders in %sdlc\\", n, rt_exe_dir());
        g_aoc_n = n;
    }
    ReleaseSRWLockExclusive(&lock);
    return g_aoc_n;
}
HOSTFN(_ZN2nn3aoc17CountAddOnContentEv) { RET(aoc_scan()); }
HOSTFN(_ZN2nn3aoc16ListAddOnContentEPiii) {   // (int *out, int offset, int count) -> number written
    int n = aoc_scan(), off = (int)W(1), cnt = (int)W(2), k = 0;
    for (; k < cnt && off >= 0 && off + k < n; k++) ST32(X(0) + 4 * (uint64_t)k, (uint32_t)g_aoc[off + k]);
    RET(k);
}
HOSTFN(_ZN2nn2ec24ShowShopAddOnContentListEv) { rt_log("ec: ShowShopAddOnContentList ignored (no shop)"); }
HOSTFN(_ZN2nn3err9ShowErrorENS_6ResultE) {
    rt_log("err: ShowError(result %#x: module %u, description %u)", W(0), 2000 + (W(0) & 0x1ff), (W(0) >> 9) & 0x1fff);
}

// ------------------------------------------------------------------------------------------------ oe
// nn::settings::LanguageCode is char[8], returned by value in x0.
HOSTFN(_ZN2nn2oe18GetDesiredLanguageEv) {
    // The language a new save starts in: BR_LANG, else the Windows display language (a save that has chosen a
    // language in Options keeps it). Codes as the console reports them.
    const char *e = getenv("BR_LANG");
    char code[8] = {0};
    if (!(e && *e)) {
        LANGID id = GetUserDefaultUILanguage();
        WORD lang = PRIMARYLANGID(id), sub = SUBLANGID(id);
        e = lang == LANG_JAPANESE ? "ja" : lang == LANG_FRENCH ? (sub == SUBLANG_FRENCH_CANADIAN ? "fr-CA" : "fr") : lang == LANG_GERMAN ? "de" :
            lang == LANG_ITALIAN ? "it" : lang == LANG_SPANISH ? (sub == SUBLANG_SPANISH || sub == SUBLANG_SPANISH_MODERN ? "es" : "es-419") :
            lang == LANG_CHINESE ? (sub == SUBLANG_CHINESE_TRADITIONAL || sub == SUBLANG_CHINESE_HONGKONG || sub == SUBLANG_CHINESE_MACAU ? "zh-Hant" : "zh-Hans") :
            lang == LANG_KOREAN ? "ko" : lang == LANG_PORTUGUESE ? (sub == SUBLANG_PORTUGUESE_BRAZILIAN ? "pt-BR" : "pt") : lang == LANG_DUTCH ? "nl" :
            lang == LANG_RUSSIAN ? "ru" : lang == LANG_ENGLISH && sub == SUBLANG_ENGLISH_UK ? "en-GB" : "en-US";
        static int said;
        if (!said++) rt_log("language: Windows display language %#x -> %s", id, e);
    }
    snprintf(code, sizeof code, "%s", e);
    uint64_t v;
    memcpy(&v, code, 8);
    RET(v);
}
HOSTFN(_ZN2nn2oe25GetProgramTotalActiveTimeEv) { RET((uint64_t)(((__int128)rt_ticks() * 1000000000) / 19200000)); }   // TimeSpan (ns)

// ------------------------------------------------------------------------------------------------ time
HOSTFN(_ZN2nn4time10InitializeEv) { RET(0); }
HOSTFN(_ZN2nn4time26StandardNetworkSystemClock14GetCurrentTimeEPNS0_9PosixTimeE) { ST64(X(0), (uint64_t)time(NULL)); RET(0); }
HOSTFN(_ZN2nn4time23StandardUserSystemClock14GetCurrentTimeEPNS0_9PosixTimeE) { ST64(X(0), (uint64_t)time(NULL)); RET(0); }

// ------------------------------------------------------------------------------------------------ nifm / socket
#define NIFM_NOT_CONNECTED 0x3ea6eu   // module 110, description 501
HOSTFN(_ZN2nn4nifm10InitializeEv) { RET(0); }
HOSTFN(_ZN2nn4nifm20SubmitNetworkRequestEv) { (void)c; }
HOSTFN(_ZN2nn4nifm20CancelNetworkRequestEv) { (void)c; }
HOSTFN(_ZN2nn4nifm22IsNetworkRequestOnHoldEv) { RET(0); }
HOSTFN(_ZN2nn4nifm18IsNetworkAvailableEv) { RET(0); }
HOSTFN(_ZN2nn4nifm26GetCurrentPrimaryIpAddressEP7in_addr) { if (X(0)) ST32(X(0), 0); RET(NIFM_NOT_CONNECTED); }
HOSTALIAS(_ZN2nn4nifm26GetCurrentPrimaryIpAddressEPNS_6socket6InAddrE, _ZN2nn4nifm26GetCurrentPrimaryIpAddressEP7in_addr)
// nn::socket as CRIWARE's monitoring server would use it: no network stack, every socket call fails.
#define SOCK_ENETDOWN 100
HOSTFN(_ZN2nn6socket10InitializeERKNS0_6ConfigE) { RET(0); }
HOSTFN(_ZN2nn6socket8FinalizeEv) { RET(0); }
HOSTFN(_ZN2nn6socket6SocketENS0_6FamilyENS0_4TypeENS0_8ProtocolE) { RET(-1); }
HOSTFN(_ZN2nn6socket12GetLastErrorEv) { RET(SOCK_ENETDOWN); }
HOSTFN(_ZN2nn6socket5CloseEi) { RET(-1); }
HOSTALIAS(_ZN2nn6socket4BindEiPKNS0_8SockAddrEj, _ZN2nn6socket5CloseEi)
HOSTALIAS(_ZN2nn6socket6ListenEii, _ZN2nn6socket5CloseEi)
HOSTALIAS(_ZN2nn6socket6AcceptEiPNS0_8SockAddrEPj, _ZN2nn6socket5CloseEi)
HOSTALIAS(_ZN2nn6socket4SendEiPKvmNS0_7MsgFlagE, _ZN2nn6socket5CloseEi)
HOSTALIAS(_ZN2nn6socket4RecvEiPvmNS0_7MsgFlagE, _ZN2nn6socket5CloseEi)
HOSTALIAS(_ZN2nn6socket10SetSockOptEiNS0_5LevelENS0_6OptionEPKvj, _ZN2nn6socket5CloseEi)
HOSTALIAS(_ZN2nn6socket8ShutdownEiNS0_14ShutdownMethodE, _ZN2nn6socket5CloseEi)
HOSTFN(_ZN2nn6socket9InetHtonsEt) { RET(__builtin_bswap16((uint16_t)W(0))); }
HOSTFN(_ZN2nn6socket8InetNtoaENS0_6InAddrE) { RET("0.0.0.0"); }
HOSTFN(_ZN2nn6socket8InetNtopENS0_6FamilyEPKvPcj) { RET(0); }

// ------------------------------------------------------------------------------------------------ swkbd
// The Switch software keyboard is a small Windows dialog with one text box (swkbd_win.c). The game shows it for
// room codes and room passwords. ShowKeyboard(String *out, const ShowKeyboardArg &): String = {pointer, byte size},
// the text is UTF-16.
#define SWKBD_CANCELED 0x29fu
int swkbd_prompt(const uint16_t *header, const uint16_t *sub, const uint16_t *initial, uint16_t *out, int max_chars);
static uint16_t g_kb_header[128], g_kb_sub[256], g_kb_initial[128];
static void kb_copy(uint16_t *dst, int max, uint64_t src) {
    int n = 0;
    if (src) for (; n < max - 1 && LD16(src + 2 * (uint64_t)n); n++) dst[n] = LD16(src + 2 * (uint64_t)n);
    dst[n] = 0;
}
HOSTFN(_ZN2nn5swkbd27GetRequiredStringBufferSizeEv) { RET(0x7d4); }
HOSTFN(_ZN2nn5swkbd25GetRequiredWorkBufferSizeEb) { RET(0xd000); }
HOSTFN(_ZN2nn5swkbd34GetRequiredTextCheckWorkBufferSizeEv) { RET(0x1000); }
HOSTFN(_ZN2nn5swkbd24InitializeKeyboardConfigEPNS0_14KeyboardConfigE) { g_kb_header[0] = g_kb_sub[0] = g_kb_initial[0] = 0; }
HOSTFN(_ZN2nn5swkbd10MakePresetEPNS0_14KeyboardConfigENS0_6PresetE) { g_kb_header[0] = g_kb_sub[0] = g_kb_initial[0] = 0; }
HOSTFN(_ZN2nn5swkbd13SetHeaderTextEPNS0_14KeyboardConfigEPKDs) { kb_copy(g_kb_header, 128, X(1)); }
HOSTFN(_ZN2nn5swkbd10SetSubTextEPNS0_14KeyboardConfigEPKDs) { kb_copy(g_kb_sub, 256, X(1)); }
HOSTFN(_ZN2nn5swkbd14SetInitialTextEPNS0_15ShowKeyboardArgEPKDs) { kb_copy(g_kb_initial, 128, X(1)); }
HOSTFN(_ZN2nn5swkbd12ShowKeyboardEPNS0_6StringERKNS0_15ShowKeyboardArgE) {
    uint64_t buf = LD64(X(0)), size = LD64(X(0) + 8);
    if (!buf || size < 4) RET(SWKBD_CANCELED);
    else {
        int max = (int)(size / 2 > 256 ? 256 : size / 2);
        int ok = swkbd_prompt(g_kb_header, g_kb_sub, g_kb_initial, (uint16_t *)(uintptr_t)buf, max);
        if (!ok) ST16(buf, 0);
        rt_log("swkbd: text box %s", ok ? "confirmed" : "cancelled");
        RET(ok ? 0 : SWKBD_CANCELED);
    }
}
// ------------------------------------------------------------------------------------------------ nn::mem
// StandardAllocator over the runtime heap: the buffer handed to Initialize is not used.
HOSTFN(_ZN2nn3mem17StandardAllocatorC1Ev) { (void)c; }
HOSTFN(_ZN2nn3mem17StandardAllocator10InitializeEPvm) { (void)c; }
HOSTFN(_ZN2nn3mem17StandardAllocator8FinalizeEv) { (void)c; }
HOSTFN(_ZN2nn3mem17StandardAllocator8AllocateEmm) { RET(rt_memalign(X(2) < 16 ? 16 : X(2), X(1))); }
HOSTFN(_ZN2nn3mem17StandardAllocator8AllocateEm) { RET(rt_malloc(X(1))); }
HOSTFN(_ZN2nn3mem17StandardAllocator4FreeEPv) { rt_free(PTR(1)); }
HOSTFN(_ZN2nn3mem17StandardAllocator10ReallocateEPvm) { RET(rt_realloc(PTR(1), X(2))); }

// ------------------------------------------------------------------------------------------------ assertions
HOSTFN(__assert_fail) {
    rt_fatal_c(c, "guest assertion failed: %s (%s:%u, %s)", X(0) ? STR(0) : "?", X(1) ? STR(1) : "?", W(2), X(3) ? STR(3) : "?");
}
// OnAssertionFailure(AssertionType, const char *condition, const char *function, const char *file, int line, ...)
HOSTFN(_ZN2nn4diag6detail18OnAssertionFailureENS0_13AssertionTypeE) {
    rt_fatal_c(c, "guest SDK assertion failed: %s (%s, %s:%u)", X(1) ? STR(1) : "?", X(2) ? STR(2) : "?", X(3) ? STR(3) : "?", W(4));
}

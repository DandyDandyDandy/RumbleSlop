// First start on PC. On a save that has not played the tutorial the title sends the player straight into the opening
// movie and the tutorial (UIStateTitle, state Title_ClearCache: Util.Movie.RequestMgMovieToTutorial). Here the game
// asks first, in its own Yes/No dialog over the title: "Would you like to skip the intro tutorial?".
//   Yes: the save is marked as having played the tutorial (what the tutorial's own ending does) and written, and the
//        title goes on to the play-style screen by its own branch on the next frame.
//   No:  the request goes through unchanged (movie, then tutorial).
// The tutorial stays available from the play-style screen (X sub-menu).
#include <windows.h>
#include <stdlib.h>
#include <string.h>
#include "rt.h"

#define FN(off) (GUEST_BASE + (off))
#define F_STRING_NEW_UTF16 0x45af94ull      // il2cpp::vm::String::NewUtf16(const Il2CppChar *, int32_t)
#define F_INIT_METADATA 0x4506d0ull         // il2cpp_codegen_initialize_runtime_metadata(uintptr_t *slot)
#define F_OBJECT_NEW 0x450aa0ull
#define F_GCHANDLE_NEW 0x469da0ull          // il2cpp_gchandle_new(obj, pinned)
#define F_GCHANDLE_TARGET 0x469df0ull
#define F_GCHANDLE_FREE 0x469e10ull
#define F_USER_PARAM 0x4403f70ull           // SaveDataContainer.UserParam.entity (static) -> SaveUserParam
#define F_USER_PARAM_SAVE 0x44040f0ull      // SaveDataContainer.UserParam.Save() (static): the game's own save request
#define F_RIGHT_FOR_DIALOG 0x44729c0ull     // UIOperationRightData.CreateForDialog(UIStateBase, bool isSystemView) (static)
#define F_YESNO_ARG_CTOR 0x4510cc0ull       // UIStateArgYesNoDialog..ctor(text, focus, onYes, onNo, system view, open SE, wait sec, no cancel)
#define F_INTERRUPT_STATE 0x453a940ull      // UIStateManager.RequestInterruptState(kind, right, arg) (static) -> UIStateHandle
#define M_CLASS_YESNO_ARG 0x61679d0ull
#define STATE_YESNO_DIALOG 0x3b             // Def_UIState.eStateKind
#define USER_ADVENTURE 0x38                 // SaveUserParam -> AdventureParam
#define ADV_PLAYED_TUTORIAL 0x28            // AdventureParam.m_playedTutorial (bool)
#define HANDLE_STATUS 0x10                  // UIStateHandle.status: 3 = Finished
#define HANDLE_RESULT 0x14                  // UIStateHandle.result: 2 = Yes, 3 = No
#define USER_UPDATE_INFO 0xa0               // SaveUserParam -> UpdateInfoParam
#define INFO_DICT 0x18                      // UpdateInfoParam.m_Dict (SimpleSerializableDict: +0x10 List of entries)
#define ENTRY_VALUE 0x18                    // SimpleSerializableDictEntry.m_Value -> UpdateInfoParamItem
#define ITEM_PLAYED 0x10                    // UpdateInfoParamItem.m_IsPlayed (bool)

static uint32_t g_handle_h;                  // the open dialog's UIStateHandle
static uint64_t g_declined_tick;             // "No" was answered then: the request that follows is not asked about again

static uint64_t new_str(Cpu *c, const char *s) {
    uint16_t w[128];
    int n = 0;
    for (; s[n] && n < 127; n++) w[n] = (uint8_t)s[n];
    return rt_call(c, FN(F_STRING_NEW_UTF16), 2, (uint64_t)(uintptr_t)w, (uint64_t)n);
}
static uint64_t class_of_slot(Cpu *c, uint64_t slot_ptr) {
    uint64_t p = LD64(FN(slot_ptr));
    if (LD64(p) & 1) rt_call(c, FN(F_INIT_METADATA), 1, p);
    return LD64(p);
}

// 1: the request is held back (the question is on screen, or was answered Yes); 0: it goes through.
static int ask(Cpu *c, uint64_t title) {
    uint64_t user = rt_call(c, FN(F_USER_PARAM), 1, 0ull), adv = user ? LD64(user + USER_ADVENTURE) : 0;
    if (!title || !adv || LD8(adv + ADV_PLAYED_TUTORIAL)) return 0;         // not a first start
    if (g_declined_tick && GetTickCount64() - g_declined_tick < 30000) return 0;
    uint64_t handle = g_handle_h ? rt_call(c, FN(F_GCHANDLE_TARGET), 1, (uint64_t)g_handle_h) : 0;
    if (!handle) {
        uint64_t right = rt_call(c, FN(F_RIGHT_FOR_DIALOG), 3, title, 1ull, 0ull);
        uint64_t arg = rt_call(c, FN(F_OBJECT_NEW), 1, class_of_slot(c, M_CLASS_YESNO_ARG));
        uint64_t text = new_str(c, "Would you like to skip the intro tutorial?");
        c->x[0] = arg; c->x[1] = text; c->x[2] = 0;                         // focus starts on Yes
        c->x[3] = 0; c->x[4] = 0;                                           // no callbacks: the handle carries the answer
        c->x[5] = 1; c->x[6] = 1; c->x[7] = 0;                              // system view, open sound, B answers No
        rt_setf(c, 0, 0.0f);
        rt_call_regs(c, FN(F_YESNO_ARG_CTOR));
        handle = rt_call(c, FN(F_INTERRUPT_STATE), 4, (uint64_t)STATE_YESNO_DIALOG, right, arg, 0ull);
        if (handle) {
            g_handle_h = (uint32_t)rt_call(c, FN(F_GCHANDLE_NEW), 2, handle, 0ull);
            rt_log("tutorial skip: first start, asking");
        }
        return 1;
    }
    if (LD32(handle + HANDLE_STATUS) != 3) return 1;
    int yes = LD32(handle + HANDLE_RESULT) == 2;
    rt_call(c, FN(F_GCHANDLE_FREE), 1, (uint64_t)g_handle_h);
    g_handle_h = 0;
    rt_log("tutorial skip: answered %s", yes ? "Yes, tutorial marked as played" : "No, on to the movie and the tutorial");
    if (yes) {                                                              // saved at once: nothing else writes the save
        ST8(adv + ADV_PLAYED_TUTORIAL, 1);                                  // before the main menu, and quitting on the
        rt_call(c, FN(F_USER_PARAM_SAVE), 1, 0ull);                         // play-style screen would bring the question back
        return 1;
    }
    g_declined_tick = GetTickCount64();
    return 0;
}

// Util.Movie.RequestMgMovieToTutorial(UIStateBase state, Movie.eID movie, bool viewOnly, bool withFollowing): called by
// the title only, every frame of its last start-up state on a first start, and once from the play-style screen's
// tutorial entry (where the save already has the tutorial marked as played).
int rt_hook_4452e90(Cpu *c) {
    uint64_t a[5], lr = c->x[30];
    for (int i = 0; i < 5; i++) a[i] = c->x[i];
    int held = ask(c, a[0]);
    for (int i = 0; i < 5; i++) c->x[i] = a[i];
    c->x[30] = lr;
    return held;
}

// Util.Unlock.CreateUpdateInfo(Season.eID) (static, bool): queues a main-menu notice for every update announcement
// the save has not shown yet ("new stages", "new Battle modes", "the collaboration is over": news for a player who
// knew the game before the update). NoticeManager.Start calls it once the save is loaded, before the title. A save
// that has not played the tutorial is a new one: nothing in the game is news to it, so every announcement is marked
// as shown and none is queued. A save from before the update keeps its one-time notices.
int rt_hook_4466c20(Cpu *c) {
    uint64_t season = c->x[0], lr = c->x[30];
    uint64_t user = rt_call(c, FN(F_USER_PARAM), 1, 0ull);
    uint64_t adv = user ? LD64(user + USER_ADVENTURE) : 0, info = user ? LD64(user + USER_UPDATE_INFO) : 0;
    c->x[0] = season; c->x[1] = 0; c->x[30] = lr;
    if (!adv || !info || LD8(adv + ADV_PLAYED_TUTORIAL)) return 0;
    uint64_t dict = LD64(info + INFO_DICT), list = dict ? LD64(dict + 0x10) : 0, items = list ? LD64(list + 0x10) : 0;
    int n = items ? (int)LD32(list + 0x18) : 0, marked = 0;
    for (int i = 0; i < n; i++) {
        uint64_t entry = LD64(items + 0x20 + 8ull * i), item = entry ? LD64(entry + ENTRY_VALUE) : 0;
        if (item && !LD8(item + ITEM_PLAYED)) { ST8(item + ITEM_PLAYED, 1); marked++; }
    }
    rt_log("new save: %d of %d update notices marked as shown", marked, n);
    c->x[0] = 0;
    return 1;
}

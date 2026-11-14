// Things a PC game does that a console game has no reason to.
//
//  * Quit. The game has no way out (a console game is closed from the system menu). Backing out of the first menu
//    (the play-style screen: B / Esc / the Back button) used to return to the "press any button" logo; now the
//    game's own Yes/No dialog asks "Quit the game?", starting on No. Yes ends the program the regular way
//    (Application.Quit, so the engine shuts down in order).
//  * The mouse pointer is hidden while the pad or the keyboard is the device in use and comes back when the mouse
//    moves (Cursor.visible).
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
#define F_RIGHT_FOR_DIALOG 0x44729c0ull     // UIOperationRightData.CreateForDialog(UIStateBase, bool isSystemView) (static)
#define F_YESNO_ARG_CTOR 0x4510cc0ull       // UIStateArgYesNoDialog..ctor(text, focus, onYes, onNo, system view, open SE, wait sec, no cancel)
#define F_INTERRUPT_STATE 0x453a940ull      // UIStateManager.RequestInterruptState(kind, right, arg) (static) -> UIStateHandle
#define M_CLASS_YESNO_ARG 0x61679d0ull
#define STATE_YESNO_DIALOG 0x3b             // Def_UIState.eStateKind
#define HANDLE_STATUS 0x10                  // UIStateHandle.status: 3 = Finished
#define HANDLE_RESULT 0x14                  // UIStateHandle.result: 2 = Yes, 3 = No

void *rt_icall_host(const char *name);       // icall.c: the engine's own function behind an internal call
static uint32_t g_quit_h;                    // the open dialog's UIStateHandle

uint64_t rt_tr_str(Cpu *c, const char *english);      // port_text.c: the text in the game's language
static uint64_t new_str(Cpu *c, const char *s) { return rt_tr_str(c, s); }
static uint64_t class_of_slot(Cpu *c, uint64_t slot_ptr) {
    uint64_t p = LD64(FN(slot_ptr));
    if (LD64(p) & 1) rt_call(c, FN(F_INIT_METADATA), 1, p);
    return LD64(p);
}

// UIStateTitle.onCancel_SelectPlayStyle(): backing out of the play-style screen.
int rt_hook_45524c0(Cpu *c) {
    uint64_t self = c->x[0], lr = c->x[30];
    if (!self || getenv("BR_NO_QUIT_DIALOG")) return 0;
    if (!g_quit_h) {
        uint64_t right = rt_call(c, FN(F_RIGHT_FOR_DIALOG), 3, self, 1ull, 0ull);
        uint64_t arg = rt_call(c, FN(F_OBJECT_NEW), 1, class_of_slot(c, M_CLASS_YESNO_ARG));
        uint64_t text = new_str(c, "Quit the game?");
        c->x[0] = arg; c->x[1] = text; c->x[2] = 1;                         // focus starts on No
        c->x[3] = 0; c->x[4] = 0;                                           // no callbacks: the handle carries the answer
        c->x[5] = 1; c->x[6] = 1; c->x[7] = 0;                              // system view, open sound, B answers No
        rt_setf(c, 0, 0.0f);
        rt_call_regs(c, FN(F_YESNO_ARG_CTOR));
        uint64_t handle = rt_call(c, FN(F_INTERRUPT_STATE), 4, (uint64_t)STATE_YESNO_DIALOG, right, arg, 0ull);
        if (handle) {
            g_quit_h = (uint32_t)rt_call(c, FN(F_GCHANDLE_NEW), 2, handle, 0ull);
            rt_log("quit: asking");
        }
    }
    c->x[0] = self; c->x[30] = lr;
    return 1;
}

static void quit_tick(Cpu *c) {
    if (!g_quit_h) return;
    uint64_t handle = rt_call(c, FN(F_GCHANDLE_TARGET), 1, (uint64_t)g_quit_h);
    if (handle && LD32(handle + HANDLE_STATUS) != 3) return;
    int yes = handle && LD32(handle + HANDLE_RESULT) == 2;
    rt_call(c, FN(F_GCHANDLE_FREE), 1, (uint64_t)g_quit_h);
    g_quit_h = 0;
    rt_log("quit: answered %s", yes ? "Yes" : "No");
    if (!yes) return;
    void (*quit)(int) = (void (*)(int))rt_icall_host("UnityEngine.Application::Quit");
    if (quit) quit(0);
    else ExitProcess(0);
}

static void cursor_tick(void) {
    extern int pc_mouse_mode(void);          // pc_input.c: the mouse gave the last input
    extern int rt_no_real_input(void);
    static void (*set_visible)(unsigned char);
    static int tried, shown = -1;
    if (!tried) { tried = 1; set_visible = (void (*)(unsigned char))rt_icall_host("UnityEngine.Cursor::set_visible"); }
    if (!set_visible) { static int said; if (!said++) rt_log("cursor: the engine has no Cursor.set_visible"); }
    if (!set_visible || rt_no_real_input()) return;
    int want = pc_mouse_mode();
    if (want == shown) return;
    shown = want;
    set_visible((unsigned char)want);
    static int said;
    if (said++ < 4) rt_log("cursor: %s", want ? "shown (mouse in use)" : "hidden (pad or keyboard in use)");
}

// Once per rendered frame on the main thread (from the per-frame hook in display_opt.c).
void pc_shell_tick(Cpu *c) {
    quit_tick(c);
    cursor_tick();
}

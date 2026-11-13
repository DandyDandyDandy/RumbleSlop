// BR_DLC_LOG=1: log what the game concludes about each add-on content pack, once per pack.
// Entry hook on Wonder.DlcImpl_Switch::IsInstalled(this, Dlc.Kind.Record record): runs the original and logs the
// record's NxIndex and Name with the answer. Without the variable the hook does nothing.
#include <windows.h>
#include <stdlib.h>
#include "rt.h"

#define FN(off) (GUEST_BASE + (off))
#define F_IS_INSTALLED 0x43aa000ull   // Wonder.DlcImpl_Switch::IsInstalled
#define F_NAME 0x47cbc50ull           // Wonder.DB.Dlc.Kind.Record::Name
#define F_NX_INDEX 0x47cbd50ull       // Wonder.DB.Dlc.Kind.Record::NxIndex

static __thread int t_in;
int rt_hook_43aa000(Cpu *c) {
    static int on = -1;
    static unsigned char seen[4096];
    if (on < 0) { const char *e = getenv("BR_DLC_LOG"); on = e && *e && *e != '0'; }
    if (!on || t_in) return 0;
    uint64_t self = c->x[0], rec = c->x[1];
    t_in = 1;
    uint32_t idx = (uint32_t)rt_call(c, FN(F_NX_INDEX), 2, rec, 0ull);
    uint64_t r = rt_call(c, FN(F_IS_INSTALLED), 2, self, rec) & 1;
    t_in = 0;
    if (idx < sizeof seen && seen[idx] != 1 + r) {
        char name[128] = "?";
        uint64_t str = rt_call(c, FN(F_NAME), 2, rec, 0ull);   // System.String: length +0x10, UTF-16 +0x14
        if (str) {
            int n = WideCharToMultiByte(CP_UTF8, 0, (const wchar_t *)(uintptr_t)(str + 0x14), (int)LD32(str + 0x10), name, sizeof name - 1, NULL, NULL);
            name[n > 0 ? n : 0] = 0;
        }
        seen[idx] = (unsigned char)(1 + r);
        rt_log("dlc: pack index %u (%s) -> %s", idx, name, r ? "installed" : "NOT installed");
    }
    c->x[0] = r;
    return 1;
}

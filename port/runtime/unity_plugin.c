// The engine side of Unity's native plug-in interface for plug-ins statically linked into main (CRIWARE).
// main+0x1109540 is the engine's UnityRegisterRenderingPlugin(UnityPluginLoad, UnityPluginUnload); the Switch engine
// is not lifted, so the host calls UnityPluginLoad itself with a guest-visible IUnityInterfaces whose only interface
// is IUnityGraphics. GetRenderer answers kUnityGfxRendererNull: no native rendering path exists yet (the CRI Mana
// plug-in's NVN texture path needs a Vulkan counterpart; see the port notes).
#include <windows.h>
#include <string.h>
#include "rt.h"

#define GFX_GUID_HI 0x7cba0a9ca4ddb544ull
#define GFX_GUID_LO 0x8c5ad4926eb17b11ull
enum { kUnityGfxRendererNull = 4 };

static uint64_t g_interfaces[4], g_graphics[4];   // guest-visible vtables-as-structs of function pointers
static uint64_t g_device_cb, g_unload;

static void gfx_get_renderer(Cpu *c) { RET(kUnityGfxRendererNull); }
static void gfx_register_cb(Cpu *c) { g_device_cb = X(0); }
static void gfx_unregister_cb(Cpu *c) { if (g_device_cb == X(0)) g_device_cb = 0; }
static void gfx_reserve_ids(Cpu *c) { static int next = 0x1000; int n = (int)W(0); RET(next); next += n; }
// GetInterface(UnityInterfaceGUID): the Switch ABI passes the 16-byte GUID by reference.
static void if_get(Cpu *c) {
    uint64_t hi = LD64(X(0)), lo = LD64(X(0) + 8);
    if (hi == GFX_GUID_HI && lo == GFX_GUID_LO) { RET(g_graphics); return; }
    rt_log("unity plugin: GetInterface(%016llx-%016llx) -> none", (unsigned long long)hi, (unsigned long long)lo);
    RET(0);
}
static void if_get_split(Cpu *c) {
    if (X(0) == GFX_GUID_HI && X(1) == GFX_GUID_LO) { RET(g_graphics); return; }
    rt_log("unity plugin: GetInterfaceSplit(%016llx-%016llx) -> none", (unsigned long long)X(0), (unsigned long long)X(1));
    RET(0);
}
static void if_register(Cpu *c) { (void)c; }

HOSTFN(engine_register_plugin) {   // main+0x1109540 (load, unload)
    uint64_t load = X(0);
    if (!g_interfaces[0]) {
        g_interfaces[0] = rt_syn_alloc(if_get, "IUnityInterfaces::GetInterface", NULL);
        g_interfaces[1] = rt_syn_alloc(if_register, "IUnityInterfaces::RegisterInterface", NULL);
        g_interfaces[2] = rt_syn_alloc(if_get_split, "IUnityInterfaces::GetInterfaceSplit", NULL);
        g_interfaces[3] = rt_syn_alloc(if_register, "IUnityInterfaces::RegisterInterfaceSplit", NULL);
        g_graphics[0] = rt_syn_alloc(gfx_get_renderer, "IUnityGraphics::GetRenderer", NULL);
        g_graphics[1] = rt_syn_alloc(gfx_register_cb, "IUnityGraphics::RegisterDeviceEventCallback", NULL);
        g_graphics[2] = rt_syn_alloc(gfx_unregister_cb, "IUnityGraphics::UnregisterDeviceEventCallback", NULL);
        g_graphics[3] = rt_syn_alloc(gfx_reserve_ids, "IUnityGraphics::ReserveEventIDRange", NULL);
    }
    g_unload = X(1);
    rt_log("unity plugin: registered (load main+%#llx, unload main+%#llx); renderer reported as Null",
           (unsigned long long)(load - GUEST_BASE), (unsigned long long)(g_unload - GUEST_BASE));
    if (load) rt_call(c, load, 1, (uint64_t)(uintptr_t)g_interfaces);
}

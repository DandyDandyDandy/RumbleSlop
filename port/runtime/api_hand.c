// Hand-written il2cpp_* exports: functions that have no wrapper in the Switch binary and are not a plain forward
// to a vm:: function (inline accessors and constants of il2cpp-api.cpp, UTF-16 variants), plus il2cpp_init.
// Offsets into Il2CppClass / MethodInfo / PropertyInfo / Il2CppImage are those of the Switch build (verified against
// code in main by the API map, work/unity/api/il2cpp_api_map.tsv).
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "api.h"

extern void rt_log_install_veh(void *module);
extern IMAGE_DOS_HEADER __ImageBase;

static INIT_ONCE g_once = INIT_ONCE_STATIC_INIT;
void rt_bind_engine_helpers(void);

static BOOL CALLBACK init_cb(PINIT_ONCE o, PVOID p, PVOID *x) {
    (void)o; (void)p; (void)x;
    rt_log_init();
    rt_log("RumbleSlop runtime starting (GameAssembly.dll at %p)", (void *)&__ImageBase);
    rt_log_install_veh(&__ImageBase);
    { extern void window_early_start(void); window_early_start(); }      // window.c: the port's title and icon as soon as the window exists
    rt_dispatch_init();
    rt_load_image();
    rt_bind_engine_helpers();
    rt_gc_init();
    rt_run_init_array();
    return TRUE;
}
void rt_init_once(void) { InitOnceExecuteOnce(&g_once, init_cb, NULL, NULL); }

Cpu *api_enter(const char *name) {
    rt_init_once();
    Cpu *c = rt_cpu();
    (void)name;
    return c;
}
void api_call(Cpu *c, uint64_t offset, const char *name) {
    if (__builtin_expect(rt_trace & 2, 0))
        rt_log("api %s(%#llx, %#llx, %#llx, %#llx)", name, (unsigned long long)c->x[0], (unsigned long long)c->x[1],
               (unsigned long long)c->x[2], (unsigned long long)c->x[3]);
    rt_call_regs(c, GUEST_BASE + offset);
    rt_escape_check(c, name);
    if (__builtin_expect(rt_trace & 2, 0)) rt_log("api %s -> %#llx", name, (unsigned long long)c->x[0]);
}
void api_stub(const char *name, int *logged) {
    rt_init_once();
    if (!*logged) { *logged = 1; rt_log("STUB: %s is not implemented (returns 0)", name); }
}

typedef struct { const char *name; uint64_t addr; } ApiAddr;
extern const ApiAddr api_addrs[];
extern const int api_addr_count;
uint64_t rt_api_addr(const char *name) {
    for (int i = 0; i < api_addr_count; i++)
        if (!strcmp(api_addrs[i].name, name)) return GUEST_BASE + api_addrs[i].addr;
    return 0;
}
uint64_t api_guest(const char *name) {
    uint64_t a = rt_api_addr(name);
    if (!a) rt_fatal("no guest address known for %s", name);
    return a;
}
static uint64_t call_named(const char *name, int n, uint64_t a0, uint64_t a1, uint64_t a2) {
    Cpu *c = api_enter(name);
    c->x[0] = a0; c->x[1] = a1; c->x[2] = a2;
    (void)n;
    api_call(c, api_guest(name) - GUEST_BASE, name);
    return c->x[0];
}
static char *utf8_from_utf16(const uint16_t *w) {
    if (!w) return NULL;
    int n = WideCharToMultiByte(CP_UTF8, 0, (const wchar_t *)w, -1, NULL, 0, NULL, NULL);
    char *s = malloc((size_t)n + 1);
    WideCharToMultiByte(CP_UTF8, 0, (const wchar_t *)w, -1, s, n, NULL, NULL);
    return s;
}

void rt_icall_report(void);
EXPORT int il2cpp_init(const char *domain_name) {
    api_enter("il2cpp_init");
    rt_icall_report();
    rt_log("il2cpp_init(\"%s\")", domain_name ? domain_name : "(null)");
    int ok = (int)(call_named("il2cpp_init", 1, (uint64_t)(uintptr_t)domain_name, 0, 0) & 1);
    rt_log("il2cpp_init returned %d", ok);
    return ok;
}
EXPORT int il2cpp_init_utf16(const uint16_t *domain_name) {
    char *s = utf8_from_utf16(domain_name);
    int ok = il2cpp_init(s);
    free(s);
    return ok;
}
EXPORT void il2cpp_set_commandline_arguments_utf16(int argc, const uint16_t *const argv[], const char *basedir) {
    char **v = malloc(sizeof(char *) * ((size_t)argc + 1));
    for (int i = 0; i < argc; i++) v[i] = utf8_from_utf16(argv[i]);
    v[argc] = NULL;
    call_named("il2cpp_set_commandline_arguments", 3, (uint64_t)argc, (uint64_t)(uintptr_t)v, (uint64_t)(uintptr_t)basedir);
}
EXPORT void il2cpp_set_config_utf16(const uint16_t *executablePath) {
    call_named("il2cpp_set_config", 1, (uint64_t)(uintptr_t)utf8_from_utf16(executablePath), 0, 0);
}
EXPORT void il2cpp_set_temp_dir(const char *temp_path) { (void)temp_path; api_enter("il2cpp_set_temp_dir"); }

// constants of this configuration
EXPORT bool il2cpp_gc_has_strict_wbarriers(void) { return false; }
EXPORT void il2cpp_gc_set_external_allocation_tracker(void *f) { (void)f; }
EXPORT void il2cpp_gc_set_external_wbarrier_tracker(void *f) { (void)f; }
EXPORT uint32_t il2cpp_object_header_size(void) { return 16; }
EXPORT uint32_t il2cpp_array_object_header_size(void) { return 32; }
EXPORT uint32_t il2cpp_offset_of_array_length_in_array_object_header(void) { return 24; }
EXPORT uint32_t il2cpp_offset_of_array_bounds_in_array_object_header(void) { return 16; }
EXPORT uint32_t il2cpp_allocation_granularity(void) { return 16; }
EXPORT bool il2cpp_is_debugger_attached(void) { return false; }
EXPORT void il2cpp_register_debugger_agent_transport(void *t) { (void)t; }
EXPORT void il2cpp_unity_set_android_network_up_state_func(void *f) { (void)f; }

// inline accessors
#define P(x) ((uint64_t)(uintptr_t)(x))
EXPORT bool il2cpp_class_is_inited(const void *klass) { return (LD8(P(klass) + 0x135) >> 1) & 1; }
EXPORT uint32_t il2cpp_class_get_type_token(void *klass) { return LD32(P(klass) + 0x11c); }
EXPORT int il2cpp_class_get_data_size(const void *klass) { return (int)LD32(P(klass) + 0x10c); }
EXPORT void *il2cpp_class_get_static_field_data(const void *klass) { return (void *)(uintptr_t)LD64(P(klass) + 0xb8); }
EXPORT bool il2cpp_field_is_literal(void *field) { return (LD16(LD64(P(field) + 8) + 8) & 0x40) != 0; }   // field->type->attrs
EXPORT uint32_t il2cpp_method_get_flags(const void *method, uint32_t *iflags) {
    if (iflags) *iflags = LD16(P(method) + 0x4e);
    return LD16(P(method) + 0x4c);
}
EXPORT uint32_t il2cpp_property_get_flags(void *prop) { return LD32(P(prop) + 0x20); }
EXPORT const void *il2cpp_property_get_get_method(void *prop) { return (void *)(uintptr_t)LD64(P(prop) + 0x10); }
EXPORT const void *il2cpp_property_get_set_method(void *prop) { return (void *)(uintptr_t)LD64(P(prop) + 0x18); }
EXPORT const char *il2cpp_property_get_name(void *prop) { return (const char *)(uintptr_t)LD64(P(prop) + 8); }
EXPORT void *il2cpp_property_get_parent(void *prop) { return (void *)(uintptr_t)LD64(P(prop)); }
EXPORT bool il2cpp_type_is_pointer_type(const void *type) { return LD8(P(type) + 0xa) == 0x0f; }
EXPORT const void *il2cpp_image_get_assembly(const void *image) { return (void *)(uintptr_t)LD64(P(image) + 0x10); }
EXPORT const char *il2cpp_image_get_name(const void *image) { return (const char *)(uintptr_t)LD64(P(image)); }
EXPORT const char *il2cpp_image_get_filename(const void *image) { return (const char *)(uintptr_t)LD64(P(image)); }
EXPORT const void **il2cpp_domain_get_assemblies(const void *domain, size_t *size) {
    (void)domain;
    Cpu *c = api_enter("il2cpp_domain_get_assemblies");   // vm::Assembly::GetAllAssemblies returns &s_Assemblies (std::vector)
    api_call(c, api_guest("il2cpp_domain_get_assemblies") - GUEST_BASE, "il2cpp_domain_get_assemblies");
    uint64_t vec = c->x[0];
    uint64_t begin = LD64(vec), end = LD64(vec + 8);
    if (size) *size = (size_t)((end - begin) / 8);
    return (const void **)(uintptr_t)begin;
}
EXPORT bool il2cpp_monitor_wait(void *obj) {
    Cpu *c = api_enter("il2cpp_monitor_wait");
    c->x[0] = P(obj); c->x[1] = 0xffffffffu;
    api_call(c, api_guest("il2cpp_monitor_try_wait") - GUEST_BASE, "il2cpp_monitor_wait");
    return (c->x[0] & 0xff) != 0;
}
EXPORT void il2cpp_shutdown(void) { rt_log("il2cpp_shutdown (ignored: the process is exiting)"); }

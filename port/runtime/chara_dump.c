// BR_CHARA_DUMP=<substring of the character prefab name, or * for all>: text dump of what a spawned character
// really renders with, after the game's own set-up code has run. For each renderer in the character's
// CharaComponentReference.rendererList: object name, active/enabled, mesh (name, vertex and submesh count) and
// every material slot (material name, main texture name). Dumped 45 and 240 frames after the character's Awake.
// BR_CHARA_DUMP_EXIT=1 ends the process after the second dump of the first matching character, so a test run
// lasts exactly as long as it takes the game to show that character.
// Hooks: Wonder.CharaComponentReference::Awake (records the object); the tick is called from the per-frame hook
// in display_opt.c (Wonder.MultiPlatformManager::LateUpdate).
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "rt.h"

#define FN(off) (GUEST_BASE + (off))
#define F_GET_RENDERER_LIST 0x486d350ull   // Wonder.CharaComponentReference::get_rendererList -> List<Renderer>
#define F_GET_GAMEOBJECT 0x3f07fb0ull      // UnityEngine.Component::get_gameObject
#define F_ACTIVE_IN_HIER 0x3f0bdd0ull      // UnityEngine.GameObject::get_activeInHierarchy
#define F_OBJ_ALIVE 0x3f10610ull           // UnityEngine.Object::op_Implicit (static)
#define F_OBJ_NAME 0x3f10820ull            // UnityEngine.Object::get_name
#define F_R_ENABLED 0x3ec5d60ull           // UnityEngine.Renderer::get_enabled
#define F_R_SHARED_MATS 0x3ec6860ull       // UnityEngine.Renderer::get_sharedMaterials
#define F_SMR_SHARED_MESH 0x3ed84c0ull     // UnityEngine.SkinnedMeshRenderer::get_sharedMesh
#define F_MESH_VERTS 0x3edb6a0ull          // UnityEngine.Mesh::get_vertexCount
#define F_MESH_SUBS 0x3edbb30ull           // UnityEngine.Mesh::get_subMeshCount
#define F_MAT_GET_TEXTURE 0x3eced00ull     // UnityEngine.Material::GetTexture(int nameID)
#define F_PROPERTY_TO_ID 0x3ec4d40ull      // UnityEngine.Shader::PropertyToID (static)
#define F_STRING_NEW_UTF16 0x45af94ull      // il2cpp::vm::String::NewUtf16(const Il2CppChar *, int32_t)

typedef struct { uint64_t obj; int frames, dumps; } Entry;
static Entry g_e[64];
static const char *g_want;
static uint64_t g_t0;

static int alive(Cpu *c, uint64_t o) { return o && (rt_call(c, FN(F_OBJ_ALIVE), 2, o, 0ull) & 1); }
static const char *name_of(Cpu *c, uint64_t o, char *buf, int cap) {   // System.String: length +0x10, UTF-16 +0x14
    snprintf(buf, cap, "(null)");
    if (!alive(c, o)) return buf;
    uint64_t s = rt_call(c, FN(F_OBJ_NAME), 2, o, 0ull);
    if (!s) return buf;
    int n = WideCharToMultiByte(CP_UTF8, 0, (const wchar_t *)(uintptr_t)(s + 0x14), (int)LD32(s + 0x10), buf, cap - 1, NULL, NULL);
    buf[n > 0 ? n : 0] = 0;
    return buf;
}
static const char *class_of(uint64_t o) { return (const char *)(uintptr_t)LD64(LD64(o) + 0x10); }   // Il2CppClass::name

static int base_map_id(Cpu *c) {   // Shader.PropertyToID("_BaseMap")
    static int id = -1;
    if (id < 0) {
        static const uint16_t name[] = {'_', 'B', 'a', 's', 'e', 'M', 'a', 'p'};
        uint64_t s = rt_call(c, FN(F_STRING_NEW_UTF16), 2, (uint64_t)(uintptr_t)name, 8ull);
        id = (int)rt_call(c, FN(F_PROPERTY_TO_ID), 2, s, 0ull);
    }
    return id;
}

static void dump(Cpu *c, Entry *e) {
    char a[160], b[160], d[160];
    uint64_t go = rt_call(c, FN(F_GET_GAMEOBJECT), 2, e->obj, 0ull);
    uint64_t list = rt_call(c, FN(F_GET_RENDERER_LIST), 2, e->obj, 0ull);
    uint64_t items = list ? LD64(list + 0x10) : 0;
    int n = list ? (int)LD32(list + 0x18) : 0;
    rt_log("chara: ==== %s, dump %d: %d renderers, %.1f s after the first frame", name_of(c, go, a, sizeof a), e->dumps + 1, n, (double)(rt_ticks() - g_t0) / 19200000.0);
    for (int i = 0; i < n; i++) {
        uint64_t r = LD64(items + 0x20 + 8 * (uint64_t)i);
        if (!alive(c, r)) { rt_log("chara:   [%d] destroyed", i); continue; }
        uint64_t rgo = rt_call(c, FN(F_GET_GAMEOBJECT), 2, r, 0ull);
        int act = (int)(rt_call(c, FN(F_ACTIVE_IN_HIER), 2, rgo, 0ull) & 1), en = (int)(rt_call(c, FN(F_R_ENABLED), 2, r, 0ull) & 1);
        char mesh[220] = "-";
        if (!strcmp(class_of(r), "SkinnedMeshRenderer")) {
            uint64_t m = rt_call(c, FN(F_SMR_SHARED_MESH), 2, r, 0ull);
            if (alive(c, m))
                snprintf(mesh, sizeof mesh, "%s (%d verts, %d submeshes)", name_of(c, m, b, sizeof b),
                         (int)rt_call(c, FN(F_MESH_VERTS), 2, m, 0ull), (int)rt_call(c, FN(F_MESH_SUBS), 2, m, 0ull));
            else snprintf(mesh, sizeof mesh, "none");
        }
        if (!act || !en) { rt_log("chara:   [%d] %s  hidden (%s)  mesh %s", i, name_of(c, rgo, a, sizeof a), !act ? "inactive" : "disabled", mesh); continue; }
        rt_log("chara:   [%d] %s  VISIBLE  mesh %s", i, name_of(c, rgo, a, sizeof a), mesh);
        uint64_t mats = rt_call(c, FN(F_R_SHARED_MATS), 2, r, 0ull);
        int nm = mats ? (int)LD32(mats + 0x18) : 0;
        for (int k = 0; k < nm; k++) {
            uint64_t m = LD64(mats + 0x20 + 8 * (uint64_t)k);
            uint64_t t = alive(c, m) ? rt_call(c, FN(F_MAT_GET_TEXTURE), 3, m, (uint64_t)base_map_id(c), 0ull) : 0;
            rt_log("chara:        slot %d: material %s, texture %s", k, name_of(c, m, b, sizeof b), name_of(c, t, d, sizeof d));
        }
    }
}

int rt_hook_486d3d0(Cpu *c) {   // CharaComponentReference::Awake
    if (!g_want) { g_want = getenv("BR_CHARA_DUMP"); if (!g_want) g_want = ""; }
    if (*g_want)
        for (int i = 0; i < 64; i++)
            if (!g_e[i].obj) { g_e[i].obj = c->x[0]; g_e[i].frames = 0; g_e[i].dumps = 0; break; }
    return 0;
}

void chara_dump_tick(Cpu *c) {   // once per frame; the caller preserves the registers
    if (!g_t0) g_t0 = rt_ticks();   // first frame
    if (!g_want || !*g_want) return;
    for (int i = 0; i < 64; i++) {
        Entry *e = &g_e[i];
        if (!e->obj) continue;
        if (!alive(c, e->obj)) { e->obj = 0; continue; }
        e->frames++;
        if (e->frames != 45 && e->frames != 240) continue;
        char a[160];
        name_of(c, rt_call(c, FN(F_GET_GAMEOBJECT), 2, e->obj, 0ull), a, sizeof a);
        int match = !strcmp(g_want, "*") || strstr(a, g_want);
        if (match) dump(c, e); else if (e->frames == 45) rt_log("chara: spawned %s (not dumped)", a);
        e->dumps++;
        if (e->frames == 240) {
            e->obj = 0;
            const char *x = getenv("BR_CHARA_DUMP_EXIT");
            if (match && x && *x == '1') { rt_log("chara: dump done, exiting (BR_CHARA_DUMP_EXIT)"); TerminateProcess(GetCurrentProcess(), 0); }
        }
    }
}

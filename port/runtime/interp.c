// Smooth motion above 60 frames a second.
//
// The game moves everything in its 60 Hz fixed step (ball, camera, stage parts, other players, items), so a
// picture drawn 144 times a second still only changes 60 times. This file makes every rendered frame show the
// moment it belongs to: a transform the game wrote during a fixed step is drawn between its value before that
// step and its value after it, by how far the frame's time lies between the two steps (what Unity's own Rigidbody
// interpolation does, for everything the game moves by hand). The picture is therefore up to one step (16.7 ms)
// behind the simulation; nothing of the simulation changes.
//
// How: the engine's Transform internal calls go through wrappers (interp_wrap, called when the engine registers
// its icalls). They tell the phase of the frame by Time.inFixedTimeStep:
//   * a setter during a fixed step notes the transform and the part written (position, rotation, scale) with the
//     value it had before the step;
//   * the first Transform call after the fixed steps of a frame ("render phase") reads the value after the step
//     and writes the blend, so every Update / LateUpdate script and the renderer see the same blended pose;
//   * the first Transform call of the next fixed step puts the true values back first, so game logic never sees
//     a blend.
// A part that a script writes during the render phase belongs to that script until a fixed step writes it again:
// it is neither blended nor put back. A jump of more than SNAP units in one step (restart, wormhole, camera cut)
// is shown at once. Local values are blended, so a moved child of a moved parent comes out right.
//
// Managed Transform objects are kept by strong GC handles while they are tracked (host memory is not scanned by
// the collector); an entry goes when the object is destroyed or was not written for two seconds.
//
// BR_INTERP=0 / 1 forces it off / on (default: on when the frame rate can exceed 60, see rt_display_above_60).
// BR_INTERP_LOG=1: every two seconds the number of tracked transforms and blended parts, and the names of
// transforms that are written in both phases.
#include <windows.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "rt.h"

#define FN(off) (GUEST_BASE + (off))
#define F_GCHANDLE_NEW 0x469da0ull          // il2cpp_gchandle_new(obj, pinned)
#define F_GCHANDLE_FREE 0x469e10ull
#define F_OBJ_NAME 0x3f10820ull             // UnityEngine.Object::get_name
#define CACHED_PTR 0x10                     // UnityEngine.Object.m_CachedPtr: 0 once the object is destroyed
#define SNAP 6.0f                           // units in one step (360 a second): a jump, not a motion
#define IDLE_STEPS 120

enum { P = 1, R = 2, S = 4, NCOMP = 3 };
typedef struct {
    uint32_t step;                          // the fixed step that wrote this part last
    uint8_t have_cur, ovr, snap, same;
    uint8_t have2, snap2, dirty;            // prev2 valid; prev2 -> prev was a jump; a blend was written this frame
    float prev2[4], prev[4], cur[4];        // before the step before, before the last step, after it
} Comp;
typedef struct { uint64_t obj; uint32_t handle; uint8_t both; Comp c[NCOMP]; } Entry;

#define MAP_SIZE 16384
#define MAX_ENTRIES 6000
static Entry g_e[MAX_ENTRIES];
static int g_n;
static uint16_t g_map[MAP_SIZE];            // index + 1
static uint32_t g_step = 1, g_main_tid;
static int g_on, g_off_req, g_applied, g_applied_frame = -1, g_log;
static double g_last_fixed = -1;
static uint64_t g_stat_blend, g_stat_frames;

// engine functions (Win64), taken when the engine registers them
typedef void (*Get2)(void *self, float *out);
typedef void (*Set2)(void *self, const float *v);
static Get2 o_get_lp, o_get_lr, o_get_ls;
static Set2 o_set_lp, o_set_lr, o_set_ls;
static unsigned char (*o_in_fixed)(void);
static int (*o_frame)(void);
static double (*o_time_d)(void), (*o_fixed_d)(void);
static float (*o_time_f)(void), (*o_fixed_f)(void), (*o_fixed_dt)(void);

static inline uint32_t hash_ptr(uint64_t p) { return (uint32_t)((p >> 4) * 0x9E3779B97F4A7C15ull >> 50) & (MAP_SIZE - 1); }
static Entry *find(uint64_t obj) {
    for (uint32_t h = hash_ptr(obj);; h = (h + 1) & (MAP_SIZE - 1)) {
        if (!g_map[h]) return NULL;
        if (g_e[g_map[h] - 1].obj == obj) return &g_e[g_map[h] - 1];
    }
}
static void map_add(int i) {
    uint32_t h = hash_ptr(g_e[i].obj);
    while (g_map[h]) h = (h + 1) & (MAP_SIZE - 1);
    g_map[h] = (uint16_t)(i + 1);
}
// guest calls from inside an icall: keep the caller-saved registers the thunk may still look at
static uint64_t guest_call(uint64_t fn, uint64_t a0, uint64_t a1) {
    Cpu *c = rt_cpu();
    uint64_t sx[19]; __uint128_t sv[8];
    memcpy(sx, c->x, sizeof sx); memcpy(sv, c->v, sizeof sv);
    uint64_t r = rt_call(c, FN(fn), 2, a0, a1);
    memcpy(c->x, sx, sizeof sx); memcpy(c->v, sv, sizeof sv);
    return r;
}
static Entry *add(uint64_t obj) {
    if (g_n >= MAX_ENTRIES) return NULL;
    Entry *e = &g_e[g_n];
    memset(e, 0, sizeof *e);
    e->obj = obj;
    e->handle = (uint32_t)guest_call(F_GCHANDLE_NEW, obj, 0);
    map_add(g_n++);
    return e;
}
static inline int alive(const Entry *e) { return LD64(e->obj + CACHED_PTR) != 0; }
static void drop_all(void) {
    for (int i = 0; i < g_n; i++) guest_call(F_GCHANDLE_FREE, g_e[i].handle, 0);
    g_n = 0;
    memset(g_map, 0, sizeof g_map);
}
// remove destroyed and idle entries, rebuild the map
static void purge(void) {
    int k = 0;
    for (int i = 0; i < g_n; i++) {
        Entry *e = &g_e[i];
        uint32_t last = 0;
        for (int j = 0; j < NCOMP; j++) if (e->c[j].step > last) last = e->c[j].step;
        if (!alive(e) || last + IDLE_STEPS < g_step) { guest_call(F_GCHANDLE_FREE, e->handle, 0); continue; }
        if (k != i) g_e[k] = *e;
        k++;
    }
    if (k == g_n) return;
    g_n = k;
    memset(g_map, 0, sizeof g_map);
    for (int i = 0; i < g_n; i++) map_add(i);
}

static void eng_get(uint64_t obj, int j, float *v) {
    Get2 f = j == 0 ? o_get_lp : j == 1 ? o_get_lr : o_get_ls;
    v[3] = 0;
    f((void *)(uintptr_t)obj, v);
}
static void eng_set(uint64_t obj, int j, const float *v) {
    Set2 f = j == 0 ? o_set_lp : j == 1 ? o_set_lr : o_set_ls;
    f((void *)(uintptr_t)obj, v);
}
static void blend(int j, const float *a, const float *b, float t, float *o) {
    if (j == 1) {                           // rotation: shortest way, normalised
        float d = a[0] * b[0] + a[1] * b[1] + a[2] * b[2] + a[3] * b[3], s = d < 0 ? -1.0f : 1.0f, n = 0;
        for (int k = 0; k < 4; k++) { o[k] = a[k] + (s * b[k] - a[k]) * t; n += o[k] * o[k]; }
        n = n > 1e-12f ? 1.0f / sqrtf(n) : 1.0f;
        for (int k = 0; k < 4; k++) o[k] *= n;
    } else {
        for (int k = 0; k < 3; k++) o[k] = a[k] + (b[k] - a[k]) * t;
        o[3] = 0;
    }
}

// the true values back, before game logic of the next fixed step reads them
static void restore(void) {
    for (int i = 0; i < g_n; i++) {
        Entry *e = &g_e[i];
        if (!alive(e)) continue;
        for (int j = 0; j < NCOMP; j++) {
            Comp *m = &e->c[j];
            if (m->step == g_step && m->dirty && !m->ovr) eng_set(e->obj, j, m->cur);
            m->dirty = 0;
        }
    }
}
// The moment a frame shows. Time.time is when the frame began, and that is uneven when the display paces the frames
// (VSync): the frame that runs a fixed step takes longer and the frames queued behind it start early, so at 240 Hz
// frames began 3.3, 8.1, 2.2, 2.2 ms apart while the display shows them 4.2 ms apart (measured); blending by
// Time.time puts that 60 Hz rhythm back into the motion. The render clock advances by the average frame time each
// frame and is pulled gently towards Time.time minus a lead of a quarter step, so it never runs ahead of the
// simulation. Behind the last step's start it blends the step before (prev2 -> prev).
static double render_time(double t, double dt) {
    static double r = -1, last = -1, avg;
    double lead = 0.25 * dt, iv = t - last;
    last = t;
    if (r < 0 || !(iv > 0) || iv > 0.1) {               // first frame, pause (time stands), a hitch: start again
        avg = iv > 0 && iv < 0.1 ? iv : dt;
        return r = t - lead;
    }
    avg += (iv - avg) * 0.05;
    r += avg;
    r += (t - lead - r) * 0.1;
    if (r > t) r = t;
    if (r < t - dt) r = t - dt;
    return r;
}
static void apply(void) {
    double t = o_time_d ? o_time_d() : (double)o_time_f(), ft = o_fixed_d ? o_fixed_d() : (double)o_fixed_f(), dt = (double)o_fixed_dt();
    float a = dt > 0 ? (float)((render_time(t, dt) - ft) / dt) : 1.0f;
    if (!(a >= -1)) a = -1;
    if (a > 1) a = 1;
    int dead = 0;
    for (int i = 0; i < g_n; i++) {
        Entry *e = &g_e[i];
        if (!alive(e)) { dead = 1; continue; }
        int snap = 0;
        for (int j = 0; j < NCOMP; j++) {
            Comp *m = &e->c[j];
            if (m->step != g_step || m->ovr || m->have_cur) continue;
            eng_get(e->obj, j, m->cur);
            m->have_cur = 1;
            m->same = !memcmp(m->cur, m->prev, j == 1 ? 16 : 12);
            if (j == 0) {
                float dx = m->cur[0] - m->prev[0], dy = m->cur[1] - m->prev[1], dz = m->cur[2] - m->prev[2];
                snap = dx * dx + dy * dy + dz * dz > SNAP * SNAP;
            }
            m->snap = 0;
        }
        if (snap) for (int j = 0; j < NCOMP; j++) if (e->c[j].step == g_step) e->c[j].snap = 1;   // the whole transform jumps
        for (int j = 0; j < NCOMP; j++) {
            Comp *m = &e->c[j];
            if (m->step != g_step || m->ovr || m->snap) continue;
            float v[4];
            if (a >= 0) {
                if (m->same) continue;
                blend(j, m->prev, m->cur, a, v);
            } else if (m->have2 && !m->snap2) {
                blend(j, m->prev2, m->prev, a + 1, v);
            } else {
                if (m->same) continue;
                memcpy(v, m->prev, sizeof v);
            }
            eng_set(e->obj, j, v);
            m->dirty = 1;
            g_stat_blend++;
        }
    }
    g_applied = 1;
    g_stat_frames++;
    static uint32_t tick;
    if (dead || !(++tick & 63)) purge();
}

// Every wrapper starts here. Returns 1 during a fixed step, 0 in the render phase, -1 when the port is not blending.
static int phase(void) {
    if (!g_on || GetCurrentThreadId() != g_main_tid) return -1;
    if (o_in_fixed()) {
        double ft = o_fixed_d ? o_fixed_d() : (double)o_fixed_f();
        if (ft != g_last_fixed) {           // a new step begins
            g_last_fixed = ft;
            if (g_applied) { restore(); g_applied = 0; }
            g_step++;
            if (g_off_req) { g_off_req = 0; g_on = 0; drop_all(); return -1; }
        }
        return 1;
    }
    int fc = o_frame();
    if (!g_applied || fc != g_applied_frame) { g_applied_frame = fc; apply(); }
    return 0;
}
static void wrote(void *self, int mask, int ph) {
    uint64_t obj = (uint64_t)(uintptr_t)self;
    if (ph < 0 || !obj || !LD64(obj + CACHED_PTR)) return;
    Entry *e = find(obj);
    if (!ph) {                              // render phase: the part is this script's now
        if (e)
            for (int j = 0; j < NCOMP; j++)
                if ((mask >> j & 1) && e->c[j].step == g_step && !e->c[j].ovr) { e->c[j].ovr = 1; e->both = 1; }
        return;
    }
    if (!e && !(e = add(obj))) return;
    for (int j = 0; j < NCOMP; j++) {
        Comp *m = &e->c[j];
        if (!(mask >> j & 1) || m->step == g_step) continue;
        if (m->step + 1 == g_step && m->have_cur && !m->ovr) {    // written in the step before too: keep its start
            memcpy(m->prev2, m->prev, sizeof m->prev);
            memcpy(m->prev, m->cur, sizeof m->prev);                // the value after the last step
            m->have2 = 1;
            m->snap2 = m->snap;
        } else {
            eng_get(obj, j, m->prev);
            memcpy(m->prev2, m->prev, sizeof m->prev);             // it stood still before this step
            m->have2 = 1;
            m->snap2 = 0;
        }
        m->step = g_step;
        m->have_cur = m->ovr = m->snap = m->same = m->dirty = 0;
    }
}

// ---- wrappers. Unity 2022.3: "_Injected" icalls take the managed object and pointers to the structs.
#define GET2(id) static void (*o_##id)(void *, void *); static void w_##id(void *s, void *a) { phase(); o_##id(s, a); }
#define GET3(id) static void (*o_##id)(void *, void *, void *); static void w_##id(void *s, void *a, void *b) { phase(); o_##id(s, a, b); }
#define SET2(id, mask) static void (*o_##id)(void *, void *); \
    static void w_##id(void *s, void *a) { int ph = phase(); if (ph == 1) wrote(s, mask, 1); o_##id(s, a); if (!ph) wrote(s, mask, 0); }
#define SET3(id, mask) static void (*o_##id)(void *, void *, void *); \
    static void w_##id(void *s, void *a, void *b) { int ph = phase(); if (ph == 1) wrote(s, mask, 1); o_##id(s, a, b); if (!ph) wrote(s, mask, 0); }
#define SETF(id, mask) static void (*o_##id)(void *, void *, float); \
    static void w_##id(void *s, void *a, float f) { int ph = phase(); if (ph == 1) wrote(s, mask, 1); o_##id(s, a, f); if (!ph) wrote(s, mask, 0); }
GET2(get_position_Injected) GET2(get_localPosition_Injected) GET2(get_rotation_Injected) GET2(get_localRotation_Injected)
GET2(get_localScale_Injected) GET2(get_lossyScale_Injected) GET2(get_localToWorldMatrix_Injected) GET2(get_worldToLocalMatrix_Injected)
GET3(GetPositionAndRotation) GET3(GetLocalPositionAndRotation) GET3(GetLocalEulerAngles_Injected)
GET3(TransformPoint_Injected) GET3(InverseTransformPoint_Injected) GET3(TransformDirection_Injected) GET3(InverseTransformDirection_Injected)
GET3(TransformVector_Injected) GET3(InverseTransformVector_Injected)
SET2(set_position_Injected, P) SET2(set_localPosition_Injected, P) SET2(set_rotation_Injected, R) SET2(set_localRotation_Injected, R)
SET2(set_localScale_Injected, S)
SET3(SetPositionAndRotation_Injected, P | R) SET3(SetLocalPositionAndRotation_Injected, P | R) SET3(SetLocalEulerAngles_Injected, R)
SET3(Internal_LookAt_Injected, R) SET3(SetParent, P | R | S)
SETF(RotateAroundInternal_Injected, R) SETF(RotateAroundLocal_Injected, R) SETF(RotateAround_Injected, R)

// ---- Animators the game steps itself during a fixed step (MainGameStage.FixedUpdate: the Animators of a stage's
// m_StageAnimators, which on imported Mania stages is all their motion; measured on SMB2 Expert 12 Brandished: 60
// updates a second, so at 144 fps the parts moved in 120 of 288 frames, every step uneven; with this 288 of 288).
// Animator.Update writes the transforms inside the engine, past the
// wrappers above, so before it runs every transform below the Animator is noted as written in this step, as a setter
// would note it; parts the clip does not move come out unchanged and are not blended. BR_INTERP_ANIM=0 leaves them
// at 60 Hz (test).
#define MAX_TREE 4096
static void (*o_anim_update)(void *, float);
static void *(*o_comp_transform)(void *);
static int (*o_child_count)(void *);
static void *(*o_get_child)(void *, int);
static void (*o_get_pos)(void *, float *);
static int g_anim = -1, g_probe_on = -1;
static uint64_t g_probe;                    // BR_FPS_LOG: the animated transform that moved farthest (under 1) in a step
static uint64_t g_anim_calls, g_anim_noted;
static int tree(void *animator, void **out) {
    void *root = o_comp_transform(animator);
    if (!root) return 0;
    int n = 0;
    out[n++] = root;
    for (int i = 0; i < n; i++) {
        int k = o_child_count(out[i]);
        for (int j = 0; j < k && n < MAX_TREE; j++) {
            void *ch = o_get_child(out[i], j);
            if (ch) out[n++] = ch;
        }
    }
    return n;
}
static void w_anim_update(void *self, float dt) {
    int ph = phase();
    if (ph != 1 || !self || !LD64((uint64_t)(uintptr_t)self + CACHED_PTR) || !o_comp_transform || !o_child_count || !o_get_child) {
        o_anim_update(self, dt);
        return;
    }
    if (g_anim < 0) { const char *e = getenv("BR_INTERP_ANIM"); g_anim = !(e && *e == '0'); }
    if (g_probe_on < 0) g_probe_on = getenv("BR_FPS_LOG") != NULL && o_get_pos;
    static void *t[MAX_TREE];
    static float before[MAX_TREE][3];
    int n = tree(self, t);
    if (g_anim) for (int i = 0; i < n; i++) wrote(t[i], P | R | S, 1);
    g_anim_calls++; g_anim_noted += (uint64_t)n;
    int pick = g_probe_on && (!g_probe || !LD64(g_probe + CACHED_PTR));
    if (pick) for (int i = 0; i < n; i++) o_get_pos(t[i], before[i]);
    o_anim_update(self, dt);
    if (pick) {
        float best = 1e-4f;
        for (int i = 0; i < n; i++) {
            float p[3];
            o_get_pos(t[i], p);
            float dx = p[0] - before[i][0], dy = p[1] - before[i][1], dz = p[2] - before[i][2], d = dx * dx + dy * dy + dz * dz;
            if (d > best && d < 1.0f) { best = d; g_probe = (uint64_t)(uintptr_t)t[i]; }   // BR_FPS_LOG judges steps under 1
        }
    }
}
// for BR_FPS_LOG (display_opt.c): a stage part moved by an Animator in the fixed step, 0 if none
uint64_t interp_anim_probe(void) { return g_probe && LD64(g_probe + CACHED_PTR) ? g_probe : 0; }

typedef struct { const char *name; void **orig; void *wrap; } Wrap;
#define WR(id) {"UnityEngine.Transform::" #id, (void **)&o_##id, (void *)w_##id}
#define TAKE(name, var) {name, (void **)&var, NULL}
static const Wrap g_wraps[] = {
    WR(get_position_Injected), WR(get_localPosition_Injected), WR(get_rotation_Injected), WR(get_localRotation_Injected),
    WR(get_localScale_Injected), WR(get_lossyScale_Injected), WR(get_localToWorldMatrix_Injected), WR(get_worldToLocalMatrix_Injected),
    WR(GetPositionAndRotation), WR(GetLocalPositionAndRotation), WR(GetLocalEulerAngles_Injected),
    WR(TransformPoint_Injected), WR(InverseTransformPoint_Injected), WR(TransformDirection_Injected), WR(InverseTransformDirection_Injected),
    WR(TransformVector_Injected), WR(InverseTransformVector_Injected),
    WR(set_position_Injected), WR(set_localPosition_Injected), WR(set_rotation_Injected), WR(set_localRotation_Injected),
    WR(set_localScale_Injected), WR(SetPositionAndRotation_Injected), WR(SetLocalPositionAndRotation_Injected),
    WR(SetLocalEulerAngles_Injected), WR(Internal_LookAt_Injected), WR(SetParent),
    WR(RotateAroundInternal_Injected), WR(RotateAroundLocal_Injected), WR(RotateAround_Injected),
    TAKE("UnityEngine.Time::get_inFixedTimeStep", o_in_fixed), TAKE("UnityEngine.Time::get_frameCount", o_frame),
    TAKE("UnityEngine.Time::get_timeAsDouble", o_time_d), TAKE("UnityEngine.Time::get_fixedTimeAsDouble", o_fixed_d),
    TAKE("UnityEngine.Time::get_time", o_time_f), TAKE("UnityEngine.Time::get_fixedTime", o_fixed_f),
    TAKE("UnityEngine.Time::get_fixedDeltaTime", o_fixed_dt),
    {"UnityEngine.Animator::Update", (void **)&o_anim_update, (void *)w_anim_update},
    TAKE("UnityEngine.Component::get_transform", o_comp_transform), TAKE("UnityEngine.Transform::get_childCount", o_child_count),
    TAKE("UnityEngine.Transform::GetChild", o_get_child),
};
// Called by il2cpp_add_internal_call for every icall the engine registers: the function the game is to call.
void *rt_interp_wrap(const char *name, void *method) {
    if (strncmp(name, "UnityEngine.T", 13) && strncmp(name, "UnityEngine.A", 13) && strncmp(name, "UnityEngine.C", 13)) return method;
    for (size_t i = 0; i < sizeof g_wraps / sizeof *g_wraps; i++)
        if (!strcmp(g_wraps[i].name, name)) {
            *g_wraps[i].orig = method;
            return g_wraps[i].wrap ? g_wraps[i].wrap : method;
        }
    return method;
}

static void log_name(Cpu *c, uint64_t obj, char *out, int size) {
    uint64_t s = rt_call(c, FN(F_OBJ_NAME), 2, obj, 0ull);
    int n = s ? (int32_t)LD32(s + 0x10) : 0, k = 0;
    for (; k < n && k < size - 1; k++) { uint16_t ch = *(const uint16_t *)(uintptr_t)(s + 0x14 + 2ull * k); out[k] = ch < 127 && ch >= 32 ? (char)ch : '?'; }
    out[k] = 0;
}
// Once per rendered frame on the main thread (from the per-frame hook in display_opt.c).
int rt_display_above_60(void);
void interp_tick(Cpu *c) {
    static int forced = -2, ready;
    if (forced == -2) {
        const char *e = getenv("BR_INTERP");
        forced = e && *e ? atoi(e) != 0 : -1;
        g_log = getenv("BR_INTERP_LOG") != NULL;
        g_main_tid = GetCurrentThreadId();
        // the part setters are never the game's own way in, so they may be missing from its table: nothing to do then
        o_get_lp = (Get2)o_get_localPosition_Injected; o_get_lr = (Get2)o_get_localRotation_Injected; o_get_ls = (Get2)o_get_localScale_Injected;
        o_set_lp = (Set2)o_set_localPosition_Injected; o_set_lr = (Set2)o_set_localRotation_Injected; o_set_ls = (Set2)o_set_localScale_Injected;
        o_get_pos = (void (*)(void *, float *))o_get_position_Injected;
        ready = o_get_lp && o_get_lr && o_get_ls && o_set_lp && o_set_lr && o_set_ls && o_in_fixed && o_frame && o_fixed_dt &&
                (o_time_d || o_time_f) && (o_fixed_d || o_fixed_f) && (!o_time_d == !o_fixed_d);
        rt_log("interp: %s", ready ? "ready" : "engine functions missing, motion stays at 60 Hz");
    }
    if (!ready) return;
    int want = forced >= 0 ? forced : rt_display_above_60();
    if (want && !g_on && !g_off_req) { g_applied = 0; g_last_fixed = -1; g_on = 1; rt_log("interp: on"); }
    else if (want && g_off_req) g_off_req = 0;
    else if (!want && g_on && !g_off_req) { g_off_req = 1; rt_log("interp: off"); }
    if (!g_log || !g_on) return;
    static uint64_t t0;
    uint64_t now = GetTickCount64();
    if (now - t0 < 2000) return;
    t0 = now;
    int act = 0, ovr = 0, snap = 0;
    for (int i = 0; i < g_n; i++)
        for (int j = 0; j < NCOMP; j++)
            if (g_e[i].c[j].step == g_step) { act++; ovr += g_e[i].c[j].ovr; snap += g_e[i].c[j].snap; }
    rt_log("interp: %d transforms tracked, %d parts written in the last step (%d taken over by a frame script, %d jumped); "
           "%.1f parts blended a frame", g_n, act, ovr, snap, g_stat_frames ? (double)g_stat_blend / (double)g_stat_frames : 0.0);
    g_stat_blend = g_stat_frames = 0;
    if (g_anim_calls) rt_log("interp: %llu Animator steps in fixed steps, %.0f transforms below each", (unsigned long long)g_anim_calls,
                             (double)g_anim_noted / (double)g_anim_calls);
    g_anim_calls = g_anim_noted = 0;
    int shown = 0;
    for (int i = 0; i < g_n && shown < 12; i++) {
        Entry *e = &g_e[i];
        if (!e->both || !alive(e)) continue;
        char name[64];
        log_name(c, e->obj, name, sizeof name);
        rt_log("interp:   written in both phases: %s (parts now%s%s%s)", name, e->c[0].ovr ? " position" : "", e->c[1].ovr ? " rotation" : "", e->c[2].ovr ? " scale" : "");
        e->both = 0;
        shown++;
    }
}

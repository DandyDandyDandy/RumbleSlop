// Test aids driven through BR_INPUT_LIVE (imports_hid.c), for checks that would otherwise need a stage to be played:
//   GOAL    every local player's ball enters the stage's first goal now (the game's own goal handler is called)
// BR_START_STAGE=<n>: a world picked in split screen or online starts at its stage n (from 0) instead of its
// first stage, so a test reaches a late stage without clearing the ones before it. Online, give it to every
// instance. n must be a stage the world has.
// The request is taken up by the per-frame hook (display_opt.c), on the game's main thread.
// BR_UNLOAD_EVERY=<frames>: Resources.UnloadUnusedAssets is called every that many frames (the engine's sweep of
// unused assets, which stops the world and walks the managed heap, normally runs only between scenes; this makes
// a fault in it show up within minutes).
// BR_BOOT_STAGE="<course id> <stage index>": straight from the title into that stage (see the end of this file).
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "rt.h"

#define FN(off) (GUEST_BASE + (off))
#define F_MAIN_GAME_STAGE 0x42cf030ull      // Wonder.MainGame::get_mainGameStage (through the singleton; null outside a stage)
#define F_PLAYER_LIST 0x42cf6c0ull          // Wonder.MainGame::get_playerList -> List<MgPlayer>
#define F_GOAL_COLI_LIST 0x431f2e0ull       // Wonder.MainGameStage::get_goalColiList -> List<MgGoalColi>
#define F_PLAYER_BALL 0x42f6a40ull          // Wonder.MgPlayer::get_ball -> PhysicsBall
#define F_GOAL_ENTER 0x4330e70ull           // Wonder.MgGoalColi::OnTriggerEnterPlayer(PhysicsBall)
#define F_OBJ_ALIVE 0x3f10610ull            // UnityEngine.Object::op_Implicit (static)
#define F_UNLOAD_UNUSED 0x3f05f20ull        // UnityEngine.Resources::UnloadUnusedAssets (static)
#define PLAYER_IS_LOCAL 0x1c0               // MgPlayer.m_IsLocal
#define LIST_ITEMS 0x10
#define LIST_SIZE 0x18
#define ARR_DATA 0x20

static volatile LONG g_goal;

void test_aids_request(const char *what) {
    if (!strcmp(what, "GOAL")) InterlockedExchange(&g_goal, 1);
}

// Wonder.BattleStarter.OnRecvPrepareAdvGame(Course.eID course, int stageIndex, settings) (static): every machine
// of a split-screen or online Adventure is told which stage of which world starts.
static int start_stage(void) {
    static int start = -1;
    if (start < 0) { const char *e = getenv("BR_START_STAGE"); start = e ? atoi(e) : 0; if (start < 0) start = 0; }
    return start;
}

int rt_hook_4690da0(Cpu *c) {               // online
    int start = start_stage();
    if (start && (uint32_t)c->x[1] == 0) {
        rt_log("test: BR_START_STAGE: course %u starts at stage %d", (uint32_t)c->x[0], start);
        c->x[1] = (uint64_t)start;
    }
    return 0;
}

// Split screen: the world picker's Confirm (UIStateAdvMultiSetting_Offline.<setupInnerFSM>b__9_3) sets the course,
// which puts the stage list on its first stage, and then asks for the scene of the first stage
// (UIStateArgTransitionToMainGameScene..ctor(kind, scene, ...)). Between the two, the list is moved to stage n and
// the scene asked for becomes that stage's.
#define F_GET_SELECTOR 0x43dbad0ull         // Wonder.RuntimeParam::get_selector (static) -> SelectorParam
#define F_STAGE_COUNT 0x43561a0ull          // Wonder.StageList::get_stageCount
#define F_SET_INDEX 0x4356970ull            // Wonder.StageList::SetIndex(int)
#define F_CURRENT_RECORD 0x4356370ull       // Wonder.StageList::get_currentStageRecord -> Stage.Record
#define F_RECORD_SCENE 0x47b3b50ull         // Wonder.DB.Stage.Record::SceneID
#define SELECTOR_STAGELIST 0x28             // SelectorParam.m_StageList
static int g_confirming;

int rt_hook_47042b0(Cpu *c) {
    g_confirming = start_stage() > 0;
    return 0;
}

int rt_hook_454d230(Cpu *c) {
    if (!g_confirming) return 0;
    g_confirming = 0;
    uint64_t r[8], lr = c->x[30];
    memcpy(r, c->x, sizeof r);
    int start = start_stage();
    uint64_t sel = rt_call(c, FN(F_GET_SELECTOR), 1, 0ull), list = sel ? LD64(sel + SELECTOR_STAGELIST) : 0;
    if (list && start < (int)rt_call(c, FN(F_STAGE_COUNT), 2, list, 0ull)) {
        rt_call(c, FN(F_SET_INDEX), 3, list, (uint64_t)start, 0ull);
        uint64_t rec = rt_call(c, FN(F_CURRENT_RECORD), 2, list, 0ull);
        if (rec) {
            r[2] = (uint32_t)rt_call(c, FN(F_RECORD_SCENE), 2, rec, 0ull);
            rt_log("test: BR_START_STAGE: the world starts at stage %d (scene %u)", start, (uint32_t)r[2]);
        }
    }
    memcpy(c->x, r, sizeof r);
    c->x[30] = lr;
    return 0;
}

static uint64_t item(uint64_t list, int i) {
    uint64_t items = list ? LD64(list + LIST_ITEMS) : 0;
    return items && i < (int)LD32(list + LIST_SIZE) ? LD64(items + ARR_DATA + 8 * (uint64_t)i) : 0;
}

void test_aids_tick(Cpu *c) {   // once per frame; the caller preserves the registers
    static int every = -1, frame;
    static double hang_at = -1;                              // BR_TEST_HANG=<s>: freeze the main thread (watchdog test)
    if (hang_at < 0) { const char *e = getenv("BR_TEST_HANG"); hang_at = e ? atof(e) : 0; }
    static uint64_t first;                                    // the first frame, in ms
    if (!first) first = GetTickCount64();
    if (hang_at > 0 && (GetTickCount64() - first) / 1000.0 > hang_at) { rt_log("test: freezing the main thread"); Sleep(INFINITE); }
    if (every < 0) { const char *e = getenv("BR_UNLOAD_EVERY"); every = e ? atoi(e) : 0; }
    if (every > 0 && ++frame % every == 0) rt_call(c, FN(F_UNLOAD_UNUSED), 1, 0ull);
    if (!InterlockedExchange(&g_goal, 0)) return;
    uint64_t stage = rt_call(c, FN(F_MAIN_GAME_STAGE), 1, 0ull);
    uint64_t players = rt_call(c, FN(F_PLAYER_LIST), 1, 0ull);
    uint64_t goals = stage ? rt_call(c, FN(F_GOAL_COLI_LIST), 2, stage, 0ull) : 0;
    uint64_t goal = item(goals, 0), ball[8];
    int n = players ? (int)LD32(players + LIST_SIZE) : 0, nb = 0;
    for (int i = 0; i < n && nb < 8; i++) {                 // every local player (split screen has several)
        uint64_t p = item(players, i);
        if (p && LD8(p + PLAYER_IS_LOCAL) && (ball[nb] = rt_call(c, FN(F_PLAYER_BALL), 2, p, 0ull))) nb++;
    }
    if (!goal || !nb || !(rt_call(c, FN(F_OBJ_ALIVE), 2, goal, 0ull) & 1)) {
        rt_log("test: GOAL: no stage, goal or local player here");
        return;
    }
    for (int i = 0; i < nb; i++) {
        rt_log("test: GOAL: the local player's ball enters the goal");
        rt_call(c, FN(F_GOAL_ENTER), 3, goal, ball[i], 0ull);
    }
}

// BR_BOOT_STAGE="<course id> <stage index>": straight from the title screen into that stage of single-player
// Adventure, without the menus (a test aid: the walk through the title, the main menu, Select World and Select Stage
// takes most of a minute). Course ids are Wonder.DB.Course.eID (RumbleManiaSlop's mania\tools\go_stage.py turns an
// imported stage's st<id> into course and stage). The title state's exec (UIStateTitle, every frame while it is up)
// waits one second for the save, then does what the tutorial does to start its stage (MgTutorial.StartMainGame):
// one offline player, main game category 1, the course, the stage list index, a fresh battle param, and
// Util.Adventure.StartSingleAdventureScene(the stage's scene). The character is the one the save has selected.
#define F_SET_RUNTIME_OFFLINE 0x443b8d0ull  // Wonder.Util+Adventure::SetRuntimeParam_Offline(int players, int bots) (static)
#define F_GET_ADV_MULTI 0x43dbbd0ull        // Wonder.RuntimeParam::get_advMulti (static)
#define F_GET_BATTLE_PARAM 0x43dbb50ull     // Wonder.RuntimeParam::get_battle (static)
#define F_SET_CATEGORY 0x43ddac0ull         // SelectorParam.set_mainGameCategoryID(int)
#define F_SET_PLAYERS 0x43ddd40ull          // SelectorParam.SetNumOfPlayers(int)
#define F_SET_COURSE 0x43dde80ull           // SelectorParam.SetCourseID(Course.eID, bool isIgnoreStageList, bool isForceReset)
#define F_START_SINGLE 0x443bae0ull         // Wonder.Util+Adventure::StartSingleAdventureScene(AppScene.eID) (static)
#define VT_RESET 0x178                      // the virtual both the tutorial calls on advMulti and battle (vtable slot + method)

static void call_virtual(Cpu *c, uint64_t obj) {
    if (!obj) return;
    uint64_t k = LD64(obj);
    rt_call(c, LD64(k + VT_RESET), 2, obj, LD64(k + VT_RESET + 8));
}

int rt_hook_4551e80(Cpu *c) {               // UIStateTitle.exec(float deltaTime)
    static int course = -1, stage, frames, done;
    if (done) return 0;
    if (course < 0) {
        const char *e = getenv("BR_BOOT_STAGE");
        if (!e || sscanf(e, "%d %d", &course, &stage) != 2) { done = 1; return 0; }
    }
    if (++frames < 60) return 0;
    done = 1;
    uint64_t r[19], lr = c->x[30];
    memcpy(r, c->x, sizeof r);
    rt_call(c, FN(F_SET_RUNTIME_OFFLINE), 3, 1ull, 0ull, 0ull);
    call_virtual(c, rt_call(c, FN(F_GET_ADV_MULTI), 1, 0ull));
    uint64_t sel = rt_call(c, FN(F_GET_SELECTOR), 1, 0ull);
    if (sel) {
        rt_call(c, FN(F_SET_PLAYERS), 3, sel, 1ull, 0ull);
        rt_call(c, FN(F_SET_CATEGORY), 3, sel, 1ull, 0ull);
        rt_call(c, FN(F_SET_COURSE), 5, sel, (uint64_t)course, 0ull, 0ull, 0ull);
    }
    uint64_t list = sel ? LD64(sel + SELECTOR_STAGELIST) : 0, rec = 0;
    if (list && stage < (int)rt_call(c, FN(F_STAGE_COUNT), 2, list, 0ull)) {
        rt_call(c, FN(F_SET_INDEX), 3, list, (uint64_t)stage, 0ull);
        rec = rt_call(c, FN(F_CURRENT_RECORD), 2, list, 0ull);
    }
    if (rec && !c->eh_unwinding) {
        uint32_t scene = (uint32_t)rt_call(c, FN(F_RECORD_SCENE), 2, rec, 0ull);
        call_virtual(c, rt_call(c, FN(F_GET_BATTLE_PARAM), 1, 0ull));
        rt_log("test: BR_BOOT_STAGE: course %d stage %d (scene %u) straight from the title", course, stage, scene);
        rt_call(c, FN(F_START_SINGLE), 2, (uint64_t)scene, 0ull);
    } else {
        rt_log("test: BR_BOOT_STAGE: course %d has no stage %d; the title stays", course, stage);
    }
    memcpy(c->x, r, sizeof r);
    c->x[30] = lr;
    return 0;
}

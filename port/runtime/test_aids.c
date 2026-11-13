// Test aids driven through BR_INPUT_LIVE (imports_hid.c), for checks that would otherwise need a stage to be played:
//   GOAL    the local player's ball enters the stage's first goal now (the game's own goal handler is called)
// The request is taken up by the per-frame hook (display_opt.c), on the game's main thread.
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "rt.h"

#define FN(off) (GUEST_BASE + (off))
#define F_MAIN_GAME_STAGE 0x42a11f0ull      // Wonder.MainGame::get_mainGameStage (through the singleton; null outside a stage)
#define F_PLAYER_LIST 0x42a1880ull          // Wonder.MainGame::get_playerList -> List<MgPlayer>
#define F_GOAL_COLI_LIST 0x42f0680ull       // Wonder.MainGameStage::get_goalColiList -> List<MgGoalColi>
#define F_PLAYER_BALL 0x42c80b0ull          // Wonder.MgPlayer::get_ball -> PhysicsBall
#define F_GOAL_ENTER 0x4301e40ull           // Wonder.MgGoalColi::OnTriggerEnterPlayer(PhysicsBall)
#define F_OBJ_ALIVE 0x3ee4c20ull            // UnityEngine.Object::op_Implicit (static)
#define PLAYER_IS_LOCAL 0x1b8               // MgPlayer.m_IsLocal
#define LIST_ITEMS 0x10
#define LIST_SIZE 0x18
#define ARR_DATA 0x20

static volatile LONG g_goal;

void test_aids_request(const char *what) {
    if (!strcmp(what, "GOAL")) InterlockedExchange(&g_goal, 1);
}

static uint64_t item(uint64_t list, int i) {
    uint64_t items = list ? LD64(list + LIST_ITEMS) : 0;
    return items && i < (int)LD32(list + LIST_SIZE) ? LD64(items + ARR_DATA + 8 * (uint64_t)i) : 0;
}

void test_aids_tick(Cpu *c) {   // once per frame; the caller preserves the registers
    if (!InterlockedExchange(&g_goal, 0)) return;
    uint64_t stage = rt_call(c, FN(F_MAIN_GAME_STAGE), 1, 0ull);
    uint64_t players = rt_call(c, FN(F_PLAYER_LIST), 1, 0ull);
    uint64_t goals = stage ? rt_call(c, FN(F_GOAL_COLI_LIST), 2, stage, 0ull) : 0;
    uint64_t goal = item(goals, 0), ball = 0;
    int n = players ? (int)LD32(players + LIST_SIZE) : 0;
    for (int i = 0; i < n && !ball; i++) {
        uint64_t p = item(players, i);
        if (p && LD8(p + PLAYER_IS_LOCAL)) ball = rt_call(c, FN(F_PLAYER_BALL), 2, p, 0ull);
    }
    if (!goal || !ball || !(rt_call(c, FN(F_OBJ_ALIVE), 2, goal, 0ull) & 1)) {
        rt_log("test: GOAL: no stage, goal or local player here");
        return;
    }
    rt_log("test: GOAL: the local player's ball enters the goal");
    rt_call(c, FN(F_GOAL_ENTER), 3, goal, ball, 0ull);
}

// "Select Your Playstyle!" screen on PC: no Nintendo eShop button (top right, -/+) and no Local Communication
// card (local wireless between consoles does not exist here). The three remaining cards are centred.
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include "rt.h"

#define FN(off) (GUEST_BASE + (off))
#define F_GET_GAMEOBJECT 0x3f07fb0ull       // UnityEngine.Component::get_gameObject
#define F_GET_TRANSFORM 0x3f07f60ull        // UnityEngine.Component::get_transform
#define F_SET_ACTIVE 0x3f0bd10ull           // UnityEngine.GameObject::SetActive(bool)
#define F_OBJ_NAME 0x3f10820ull             // UnityEngine.Object::get_name
#define F_RT_GET_ANCHORED_POS 0x3f182c0ull  // UnityEngine.RectTransform
#define F_RT_SET_ANCHORED_POS 0x3f18390ull
#define F_SET_SELECT_MAIN 0x454ef20ull
#define STR_LEN 0x10
#define STR_CHARS 0x14
#define LIST_ITEMS 0x10                     // System.Collections.Generic.List<T>
#define LIST_SIZE 0x18
#define LIST_VERSION 0x1c
#define ARR_DATA 0x20

static int name_ends_with(Cpu *c, uint64_t component, const char *tail) {
    uint64_t s = rt_call(c, FN(F_OBJ_NAME), 2, rt_call(c, FN(F_GET_GAMEOBJECT), 2, component, 0ull), 0ull);
    int len = s ? (int)LD32(s + STR_LEN) : 0, tl = 0;
    while (tail[tl]) tl++;
    if (len < tl) return 0;
    for (int k = 0; k < tl; k++)
        if (LD16(s + STR_CHARS + 2 * (uint64_t)(len - tl + k)) != (uint8_t)tail[k]) return 0;
    return 1;
}
static uint64_t list_at(uint64_t list, int i) { return LD64(LD64(list + LIST_ITEMS) + ARR_DATA + 8 * (uint64_t)i); }
static void list_remove_at(uint64_t list, int i) {
    uint64_t items = LD64(list + LIST_ITEMS);
    int n = (int)LD32(list + LIST_SIZE);
    for (int k = i; k + 1 < n; k++) ST64(items + ARR_DATA + 8 * (uint64_t)k, LD64(items + ARR_DATA + 8 * (uint64_t)(k + 1)));
    ST64(items + ARR_DATA + 8 * (uint64_t)(n - 1), 0);
    ST32(list + LIST_SIZE, n - 1);
    ST32(list + LIST_VERSION, LD32(list + LIST_VERSION) + 1);
}
static float button_x(Cpu *c, uint64_t button, float *y) {
    rt_call(c, FN(F_RT_GET_ANCHORED_POS), 2, rt_call(c, FN(F_GET_TRANSFORM), 2, button, 0ull), 0ull);
    if (y) *y = rt_getf(c, 1);
    return rt_getf(c, 0);
}

// UIChoicesGroupModule_TwoDimension_Param..ctor(parent items, child item lists, card views, ...): made once, by the
// title state, for the play-style cards. The three lists are parallel (one entry per card). The Local Communication
// entry is taken out of all three before the module is built from them, so it can be neither seen nor focused.
static void drop_local_card(Cpu *c, uint64_t parents, uint64_t children, uint64_t views);
int rt_hook_458b250(Cpu *c) {
    uint64_t args[9], lr = c->x[30];                        // the constructor runs afterwards: its arguments must survive the calls
    for (int i = 0; i < 9; i++) args[i] = c->x[i];
    if (args[1] && args[2] && args[3]) drop_local_card(c, args[1], args[2], args[3]);
    for (int i = 0; i < 9; i++) c->x[i] = args[i];
    c->x[30] = lr;
    return 0;
}
static void drop_local_card(Cpu *c, uint64_t parents, uint64_t children, uint64_t views) {
    int n = (int)LD32(views + LIST_SIZE), at = -1;
    if (n < 2 || (int)LD32(parents + LIST_SIZE) != n || (int)LD32(children + LIST_SIZE) != n) return;
    for (int i = 0; i < n && at < 0; i++)
        if (name_ends_with(c, list_at(views, i), "PlayOnLocalCommunication")) at = i;
    if (at < 0) return;
    uint64_t gone = list_at(views, at);
    // Close the gap: cards after the removed one take the place of their predecessor; then the whole row moves
    // right by half a card spacing plus 30 units (the four-card row sat that much left of the panel's centre), which
    // puts the middle card on the centre of the panel.
    float x_gone = button_x(c, gone, NULL), x_near = button_x(c, list_at(views, at ? at - 1 : at + 1), NULL);
    float half = (x_gone > x_near ? x_gone - x_near : x_near - x_gone) / 2 + 30, prev = x_gone;
    for (int i = at + 1; i < n; i++) {
        uint64_t t = rt_call(c, FN(F_GET_TRANSFORM), 2, list_at(views, i), 0ull);
        float y, x = button_x(c, list_at(views, i), &y);
        c->x[0] = t; c->x[1] = 0; rt_setf(c, 0, prev); rt_setf(c, 1, y); rt_call_regs(c, FN(F_RT_SET_ANCHORED_POS));
        prev = x;
    }
    rt_call(c, FN(F_SET_ACTIVE), 3, rt_call(c, FN(F_GET_GAMEOBJECT), 2, gone, 0ull), 0ull, 0ull);
    list_remove_at(views, at); list_remove_at(parents, at); list_remove_at(children, at);
    for (int i = 0; i < n - 1; i++) {
        uint64_t t = rt_call(c, FN(F_GET_TRANSFORM), 2, list_at(views, i), 0ull);
        float y, x = button_x(c, list_at(views, i), &y);
        c->x[0] = t; c->x[1] = 0; rt_setf(c, 0, x + half); rt_setf(c, 1, y); rt_call_regs(c, FN(F_RT_SET_ANCHORED_POS));
    }
    rt_log("playstyle ui: removed card %d of %d (Local Communication), row moved by %.0f", at, n, half);
}
// UIViewSelectPlayStyle.SetPlayStyleSelectMain(): switches the screen's parts on, among them
// {+0x68 UIPartViewIndependentButton eShop button}. The eShop button stays off.
int rt_hook_454ef20(Cpu *c) {
    static __thread int inside;
    if (inside) return 0;
    uint64_t self = X(0);
    inside = 1; rt_call(c, FN(F_SET_SELECT_MAIN), 2, self, 0ull); inside = 0;
    uint64_t shop = LD64(self + 0x68);
    if (shop) rt_call(c, FN(F_SET_ACTIVE), 3, rt_call(c, FN(F_GET_GAMEOBJECT), 2, shop, 0ull), 0ull, 0ull);
    return 1;
}
// UIStateTitle.onCallEShop(...): the -/+ press that opened the eShop. Nothing to open on PC.
int rt_hook_4552670(Cpu *c) { (void)c; return 1; }

// UIStateBase.RequestStateTransition(int stateId, UIStateArg arg, bool, ...). Online Play > Adventure and > Battle
// open a "friends or random people" choice. Random matchmaking does not exist on PC, so the choice is skipped: the
// main menu goes straight to the friends screen (create or join a room) and that screen's Back returns to the main
// menu. State ids: 9 main menu, 36 Adventure choice, 40 Adventure friends, 45 Battle choice, 47 Battle friends.
// BR_STATE_LOG=1 logs each request.
int rt_hook_45384e0(Cpu *c) {
    static int log = -1;
    if (log < 0) log = getenv("BR_STATE_LOG") != NULL;
    const char *cls = (const char *)(uintptr_t)LD64(LD64(X(0)) + 0x10);
    int to = (int)W(1), now = to;
    if (!strcmp(cls, "UIStateTopMenu")) now = to == 36 ? 40 : to == 45 ? 47 : to;
    else if (!strcmp(cls, "UIStateAdvMulti_Friend_SelectHostOrGuest") && to == 36) now = 9;
    else if (!strcmp(cls, "UIStateBattleRoyaleFriendChooseHostOrGuest") && to == 45) now = 9;
    if (log) rt_log("ui state: %s requests state %d%s", cls, to, now != to ? " (redirected)" : "");
    c->x[1] = (uint64_t)now;
    return 0;
}
// UIStateArgTransitionToMainMenuScene_AdvMulti_Friend_SelectHostOrGuest / _FriendMatchingChooseHostOrGuest
// .get_stateAtSceneStart(): the state the main menu scene opens in after an online lobby is left or closed (Leave
// Room, Disband Room, a lost connection). The game answers 40 / 47, the Create Room / Join Room screens. On PC those
// are one step below the main menu and nothing above them exists, so leaving a room ends on the main menu (9), where
// the room was entered from.
int rt_hook_454d860(Cpu *c) { c->x[0] = 9; return 1; }
int rt_hook_454d800(Cpu *c) { c->x[0] = 9; return 1; }

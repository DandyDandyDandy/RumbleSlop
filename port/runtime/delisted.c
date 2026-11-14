// Six characters that cannot be bought or earned any more are owned in every save, from the start and without
// notices or "new" marks: Dr. Bad-Boon (needed an online battle on Nintendo's service) and the five collaboration
// characters (their add-on content was taken off sale). Their data is in every copy of the game; whether a player
// has a character is one value in the save: SaveUserParam.ShopItemParam.m_Character, a dictionary
// Def.Chara.eID -> ShopItemParamCharacterItem {+0x10 ShopItemConditionParam {+0x10 Shop.Item.Condition.eID}}.
// The hooks set that value to Have (5) for the six whenever the save's shop data is created or loaded, and again
// whenever one of them is looked up, so nothing the game does in between can leave them locked. The other half
// is port\assets\delisted.py, which switches off their rows of the unlock table (no unlock notices).
// The mod "Delisted Character Fix" by iswimfly (gamebanana.com/mods/690229) showed that these characters only
// need their unlock rule changed; this is a different route to the same end and uses nothing of the mod.
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "rt.h"

#define FN(off) (GUEST_BASE + (off))
#define F_CHARA_GET_ITEM 0x43fbd10ull       // ShopItemParamCharacter.GetItem(Def.Chara.eID): makes the item when it is missing
#define F_CHARA_GET_ITEM2 0x4400890ull      // ShopItemParamCharacter.GetItem(Def.Chara.eID, Def.Chara.Table)
#define F_SHOP_ON_CREATE 0x43fa930ull       // SaveUserParam.ShopItemParam.OnCreate()
#define F_SHOP_ON_AFTER_LOAD 0x43faae0ull   // SaveUserParam.ShopItemParam.OnAfterLoad()
#define SHOP_CHARACTER 0x28                 // ShopItemParam.m_Character
#define CHARACTER_DICT 0x10                 // ShopItemParamCharacter.m_Dict
#define ITEM_CONDITION 0x10                 // ShopItemParamItemBase -> ShopItemConditionParam
#define CONDITION_ID 0x10                   // ShopItemConditionParam.m_ConditionID
#define CONDITION_NOT_HAVE 3
#define CONDITION_BRAND_NEW_HAVE 4
#define CONDITION_HAVE 5

static const uint32_t g_charas[] = {12, 19, 20, 21, 22, 23};   // Def.Chara.eID: Madhihi, Collab01..Collab05 (as in delisted.py)

// A character counts only when the game data has its model. The 2.2.0 update, which the port is made for, carries
// all six (the game before it had Dr. Bad-Boon and two collaboration characters only); the check stays as a guard.
static int has_model(uint32_t chara) {
    char pattern[700];
    WIN32_FIND_DATAA fd;
    snprintf(pattern, sizeof pattern, "%s" GAME_NAME "_Data\\StreamingAssets\\aa\\Switch\\chara_model_assets_chara\\model\\%02u*",
             rt_exe_dir(), chara);
    HANDLE h = FindFirstFileA(pattern, &fd);
    if (h == INVALID_HANDLE_VALUE) return 0;
    FindClose(h);
    return 1;
}
static int ours(uint32_t chara) {
    static int have[sizeof g_charas / sizeof *g_charas], init;
    if (!init) {
        int n = 0;
        for (size_t i = 0; i < sizeof g_charas / sizeof *g_charas; i++) n += have[i] = has_model(g_charas[i]);
        init = 1;
        rt_log("delisted: %d of %d characters have model data and are owned in every save", n, (int)(sizeof g_charas / sizeof *g_charas));
    }
    for (size_t i = 0; i < sizeof g_charas / sizeof *g_charas; i++)
        if (g_charas[i] == chara) return have[i];
    return 0;
}
static int listed(uint32_t chara) {
    for (size_t i = 0; i < sizeof g_charas / sizeof *g_charas; i++)
        if (g_charas[i] == chara) return 1;
    return 0;
}
// Owned when the model is there; not owned when it is not (a save may say "owned" for a character this copy of
// the game cannot show: it would be an empty slot with a placeholder name).
static void settle(uint32_t chara, uint64_t item) {
    uint64_t cond = item ? LD64(item + ITEM_CONDITION) : 0;
    if (!cond || !listed(chara)) return;
    uint32_t want = ours(chara) ? CONDITION_HAVE : CONDITION_NOT_HAVE, now = LD32(cond + CONDITION_ID);
    if (now != want && (want == CONDITION_HAVE || now == CONDITION_HAVE || now == CONDITION_BRAND_NEW_HAVE)) ST32(cond + CONDITION_ID, want);
}

static __thread int g_inside;

static int get_item(Cpu *c, uint64_t fn) {
    if (g_inside) return 0;
    uint64_t lr = c->x[30];
    uint32_t chara = (uint32_t)c->x[1];
    g_inside = 1;
    uint64_t item = rt_call(c, FN(fn), 4, c->x[0], c->x[1], c->x[2], c->x[3]);
    g_inside = 0;
    settle(chara, item);
    c->x[0] = item;
    c->x[30] = lr;
    return 1;
}
int rt_hook_43fbd10(Cpu *c) { return get_item(c, F_CHARA_GET_ITEM); }
int rt_hook_4400890(Cpu *c) { return get_item(c, F_CHARA_GET_ITEM2); }

static int shop_event(Cpu *c, uint64_t fn) {
    if (g_inside) return 0;
    uint64_t lr = c->x[30], self = c->x[0];
    g_inside = 1;
    rt_call(c, FN(fn), 2, self, c->x[1]);
    // A new save: OnCreate makes m_Character without its dictionary (GetItem would read through null); the
    // dictionary is filled by the OnAfterLoad that follows, which passes here again.
    uint64_t list = self ? LD64(self + SHOP_CHARACTER) : 0;
    if (list && !LD64(list + CHARACTER_DICT)) list = 0;
    for (size_t i = 0; list && i < sizeof g_charas / sizeof *g_charas; i++)
        settle(g_charas[i], rt_call(c, FN(F_CHARA_GET_ITEM), 3, list, (uint64_t)g_charas[i], 0ull));
    g_inside = 0;
    c->x[30] = lr;
    return 1;
}
int rt_hook_43fa930(Cpu *c) { return shop_event(c, F_SHOP_ON_CREATE); }
int rt_hook_43faae0(Cpu *c) { return shop_event(c, F_SHOP_ON_AFTER_LOAD); }

// Button glyphs for the device in use: Xbox, DualSense, DualShock 4, or keyboard and mouse.
//
// The game draws button glyphs by controller kind (Wonder.DB.Input.ControllerKind: the Switch kinds, PS4, XboxOne,
// Steam, PS5) and re-draws every glyph on screen when the kind of a player's controller changes
// (UIInputDeviceSwitchingDetector). The Switch build only has pictures for the Switch kinds. So:
//   * each frame every pad state gets the kind of the device behind it (XboxOne for an XInput pad, PS5 / PS4 for a
//     Sony pad, Steam standing for "keyboard and mouse"); player 1 has the kind of whatever was used last;
//   * UIIconSpriteManager_ControllerBtn.GetSprite, where all pad glyphs come from, returns the port's own picture
//     for those kinds: a cell of icons\glyphs.png (made by port\tools\make_glyphs.py from Kenney's CC0 pack);
//   * for the keyboard the glyph is the key bound to the action (pc_input.c). GetSprite only knows the pad button,
//     so the callers that know the action (or the fixed keyboard icon of a guide) leave it in a context for it.
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "rt.h"

#define FN(off) (GUEST_BASE + (off))
#define F_INIT_METADATA 0x4506d0ull
#define F_OBJECT_NEW 0x450aa0ull
#define F_SZARRAY_NEW 0x450830ull
#define F_GCHANDLE_NEW 0x469da0ull
#define F_GCHANDLE_TARGET 0x469df0ull
#define F_GCHANDLE_FREE 0x469e10ull
#define F_OBJ_ALIVE 0x3f10610ull
#define F_OBJ_SET_HIDE_FLAGS 0x3f120d0ull
#define F_TEX_CTOR 0x3ee90c0ull             // Texture2D..ctor(int, int)
#define F_LOAD_IMAGE 0x3f534d0ull           // ImageConversion.LoadImage(Texture2D, byte[])
#define F_SPRITE_CREATE 0x3f22180ull        // Sprite.Create(Texture2D, Rect, Vector2 pivot)
#define F_GET_ASSIGN_PAD 0x43b50d0ull       // AppInput.GetActionAssignGamePad(Action.eID) (static)
#define M_CLASS_TEXTURE2D 0x6121658ull
#define M_CLASS_BYTE_ARR 0x6120328ull
#define ARR_DATA 0x20
#define STATE_ID 0x20                       // AppInput.State
#define STATE_KIND 0x6c
#define KIND_PS4 8                          // Wonder.DB.Input.ControllerKind.eID
#define KIND_XBOX 9
#define KIND_KEYBOARD 10                    // "Steam": used here for keyboard and mouse
#define KIND_PS5 11
#define CELL 96
#define COLS 16
#define ROWS 14
#define NCELLS (COLS * ROWS)
#define CELL_KEYS 96                        // + UnityEngine.InputSystem.Key
#define CELL_MOUSE 208                      // whole, left, right, middle, back, forward
#define CELL_WASD 214
#define CELL_ARROWS 215
#define ACTION_COUNT 187

extern const uint8_t rt_glyphs_png[], rt_glyphs_png_end[];   // glyph_data.S
extern int rt_pad_kind(uint32_t id);        // imports_hid.c: 0 no device, 1 XInput, 2 DualSense, 3 DualShock 4
extern uint64_t rt_pad_last_input(uint32_t id);
extern int rt_pad_positional(void);
extern uint64_t pc_kbm_last_input(void);    // pc_input.c
extern int pc_keyboard_actions(void);
extern int pc_action_key(int action);
extern int pc_action_mouse(int action);
extern int pc_key_vk(int key);

static uint64_t class_of_slot(Cpu *c, uint64_t slot_ptr) {
    uint64_t p = LD64(FN(slot_ptr));
    if (LD64(p) & 1) rt_call(c, FN(F_INIT_METADATA), 1, p);
    return LD64(p);
}
static uint64_t target(Cpu *c, uint32_t h) { return h ? rt_call(c, FN(F_GCHANDLE_TARGET), 1, (uint64_t)h) : 0; }
static int alive(Cpu *c, uint64_t o) { return o && (rt_call(c, FN(F_OBJ_ALIVE), 2, o, 0ull) & 1); }

// ------------------------------------------------------------------------------------------------ the pictures
static uint32_t g_tex_h, g_sprite_h[NCELLS];
static uint64_t glyph_sprite(Cpu *c, int cell) {
    if (cell < 0 || cell >= NCELLS) return 0;
    uint64_t sprite = target(c, g_sprite_h[cell]);
    if (alive(c, sprite)) return sprite;
    uint64_t tex = target(c, g_tex_h);
    if (!alive(c, tex)) {
        size_t size = (size_t)(rt_glyphs_png_end - rt_glyphs_png);
        tex = rt_call(c, FN(F_OBJECT_NEW), 1, class_of_slot(c, M_CLASS_TEXTURE2D));
        rt_call(c, FN(F_TEX_CTOR), 4, tex, 2ull, 2ull, 0ull);
        uint64_t bytes = rt_call(c, FN(F_SZARRAY_NEW), 2, class_of_slot(c, M_CLASS_BYTE_ARR), (uint64_t)size);
        memcpy((void *)(uintptr_t)(bytes + ARR_DATA), rt_glyphs_png, size);
        if (!(rt_call(c, FN(F_LOAD_IMAGE), 3, tex, bytes, 0ull) & 1)) { rt_log("glyphs: the atlas could not be decoded"); return 0; }
        rt_call(c, FN(F_OBJ_SET_HIDE_FLAGS), 3, tex, 32ull, 0ull);  // HideFlags.DontUnloadUnusedAsset
        if (g_tex_h) rt_call(c, FN(F_GCHANDLE_FREE), 1, (uint64_t)g_tex_h);
        g_tex_h = (uint32_t)rt_call(c, FN(F_GCHANDLE_NEW), 2, tex, 0ull);
        rt_log("glyphs: atlas loaded (%u bytes)", (unsigned)size);
    }
    c->x[0] = tex; c->x[1] = 0;                                     // Sprite.Create(tex, Rect(x, y, w, h), Vector2(0.5, 0.5)); y from the bottom
    rt_setf(c, 0, (float)((cell % COLS) * CELL)); rt_setf(c, 1, (float)((ROWS - 1 - cell / COLS) * CELL));
    rt_setf(c, 2, CELL); rt_setf(c, 3, CELL); rt_setf(c, 4, 0.5f); rt_setf(c, 5, 0.5f);
    rt_call_regs(c, FN(F_SPRITE_CREATE));
    sprite = c->x[0];
    if (!sprite) return 0;
    rt_call(c, FN(F_OBJ_SET_HIDE_FLAGS), 3, sprite, 32ull, 0ull);
    if (g_sprite_h[cell]) rt_call(c, FN(F_GCHANDLE_FREE), 1, (uint64_t)g_sprite_h[cell]);
    g_sprite_h[cell] = (uint32_t)rt_call(c, FN(F_GCHANDLE_NEW), 2, sprite, 0ull);
    return sprite;
}

// ------------------------------------------------------------------------------------------------ which device
// The kind of the device behind pad `id`, 0 = leave the game's own (no device, or the old keyboard-as-pad mode).
static int device_kind(uint32_t id) {
    static int forced = -1;
    if (forced < 0) { const char *e = getenv("BR_KIND"); forced = e ? atoi(e) : 0; }   // test: 8 PS4, 9 Xbox, 10 keyboard, 11 PS5
    if (forced) return forced;
    int pad = rt_pad_kind(id), kind = pad == 1 ? KIND_XBOX : pad == 2 ? KIND_PS5 : pad == 3 ? KIND_PS4 : 0;
    if (id == 0 && pc_keyboard_actions() && (!kind || pc_kbm_last_input() > rt_pad_last_input(0))) kind = KIND_KEYBOARD;
    return kind;
}
// AppInput.State.CopyToActiveLayer(State) / CopyToInactiveLayer(State): each frame the pad's own state (this) is
// copied to the states the game reads, the kind with it. (The pad state itself gets its kind back from the npad
// style every frame, so it is set here, just before it is copied.)
static int set_kind(Cpu *c) {
    uint64_t state = X(0);
    int kind = device_kind(LD32(state + STATE_ID));
    if (kind) ST32(state + STATE_KIND, kind);
    return 0;
}
int rt_hook_43caab0(Cpu *c) { return set_kind(c); }
int rt_hook_43cab80(Cpu *c) { return set_kind(c); }

// ------------------------------------------------------------------------------------------------ which picture
// Wonder.DB.UI.ButtonIcon.eID
enum { BI_UP = 1, BI_DOWN, BI_LEFT, BI_RIGHT, BI_DUP, BI_DDOWN, BI_DLEFT, BI_DRIGHT, BI_LS_UP, BI_LS_DOWN, BI_LS_LEFT, BI_LS_RIGHT,
       BI_EAST, BI_WEST, BI_SOUTH, BI_NORTH, BI_L, BI_R, BI_ZL, BI_ZR, BI_STICK_L, BI_STICK_R, BI_PLUS, BI_MINUS,
       BI_RS_UP, BI_RS_DOWN, BI_RS_LEFT, BI_RS_RIGHT, BI_PUSH_L, BI_PUSH_R, BI_DPAD };
static int pad_cell(int kind, int icon) {
    int base = kind == KIND_XBOX ? 0 : kind == KIND_PS5 ? 32 : 64, pos = rt_pad_positional();
    if (icon < 1 || icon > 31) return -1;
    // The face cells hold A B X Y (cross circle square triangle) at 13..16. Which of them a face button of the
    // game's pad is depends on how imports_hid.c maps the device: by name (A is A) or by position (BR_PAD_LAYOUT).
    if (icon == BI_EAST) icon = pos ? 14 : 13;
    else if (icon == BI_SOUTH) icon = pos ? 13 : 14;
    else if (icon == BI_NORTH) icon = pos ? 16 : 15;
    else if (icon == BI_WEST) icon = pos ? 15 : 16;
    return base + icon;
}
// The cell of a key, by what the key is called on the current keyboard layout where the atlas has that character.
static int key_cell(int key) {
    static const char chars[] = "`';,./\\[]-=";                      // Key 4..14: Backquote Quote Semicolon Comma Period Slash Backslash brackets Minus Equals
    if (key <= 0 || key > 110) return -1;
    if ((key >= 4 && key <= 50)) {                                   // a character key
        UINT ch = MapVirtualKeyW((UINT)pc_key_vk(key), MAPVK_VK_TO_CHAR) & 0x7fff;
        if (ch >= 'a' && ch <= 'z') ch -= 32;
        if (ch >= 'A' && ch <= 'Z') return CELL_KEYS + 15 + (int)(ch - 'A');
        if (ch >= '1' && ch <= '9') return CELL_KEYS + 41 + (int)(ch - '1');
        if (ch == '0') return CELL_KEYS + 50;
        const char *p = ch && ch < 128 ? strchr(chars, (int)ch) : NULL;
        if (p) return CELL_KEYS + 4 + (int)(p - chars);
    }
    return CELL_KEYS + key;
}
static int mouse_cell(int mask) { return mask == 1 ? CELL_MOUSE + 1 : mask == 2 ? CELL_MOUSE + 2 : mask == 4 ? CELL_MOUSE + 3 : mask == 8 ? CELL_MOUSE + 4 : mask == 16 ? CELL_MOUSE + 5 : mask ? CELL_MOUSE : -1; }
static int action_cell(int action) {
    int key = pc_action_key(action);
    return key ? key_cell(key) : mouse_cell(pc_action_mouse(action));
}
// Wonder.DB.UI.KeyboardKeyIcon.eID -> cell
static int kbicon_cell(int id) {
    static const uint8_t key[] = {0, 1, 65, 71, 3, 2, 76, 60, 84, 85, 86, 87, 88, 89, 90, 91, 92, 93, 82, 78, 79, 81, 80, 77, 83, 63, 64, 62, 61, 70, 68, 69, 67, 66,
                                  94, 95, 96, 97, 98, 99, 100, 101, 102, 103, 104, 105, 50, 41, 42, 43, 44, 45, 46, 47, 48, 49, 8, 9, 5, 6, 14,
                                  15, 16, 17, 18, 19, 20, 21, 22, 23, 24, 25, 26, 27, 28, 29, 30, 31, 32, 33, 34, 35, 36, 37, 38, 39, 40, 52, 51, 56, 55, 54, 53};
    if (id >= 1 && id < (int)sizeof key) return key_cell(key[id]);
    switch (id) {
    case 93: return CELL_MOUSE; case 94: return CELL_MOUSE + 3; case 95: return CELL_MOUSE + 1; case 96: return CELL_MOUSE + 2;
    case 97: return CELL_MOUSE + 3; case 98: return CELL_MOUSE + 4; case 99: return CELL_MOUSE + 5; case 100: return CELL_MOUSE;
    case 101: return CELL_WASD; case 102: return CELL_ARROWS;
    }
    return -1;
}
// A pad glyph asked for without an action (fixed guides): the key of the action that button stands for most often.
static int button_action_cell(Cpu *c, int icon) {
    static const struct { uint8_t icon, action; } fixed[] = {
        {BI_EAST, 1}, {BI_SOUTH, 2}, {BI_UP, 4}, {BI_DOWN, 5}, {BI_LEFT, 6}, {BI_RIGHT, 7}, {BI_DUP, 8}, {BI_DDOWN, 9}, {BI_DLEFT, 10}, {BI_DRIGHT, 11},
        {BI_LS_UP, 46}, {BI_LS_DOWN, 47}, {BI_LS_LEFT, 48}, {BI_LS_RIGHT, 49}, {BI_RS_UP, 51}, {BI_RS_DOWN, 52}, {BI_RS_LEFT, 53}, {BI_RS_RIGHT, 54},
        {BI_L, 20}, {BI_R, 21}, {BI_PLUS, 3}};
    static const uint32_t bit[32] = {0, 1, 2, 4, 8, 16, 32, 64, 128, 256, 512, 1024, 2048, 0x1000, 0x8000, 0x2000, 0x4000, 0x10000, 0x20000, 0x40000, 0x80000,
                                     0x103490, 0x202300, 0x4045f0, 0x800000, 0x1000000, 0x2000000, 0x4000000, 0x8055000, 0x103490, 0x202300, 0};
    if (icon == BI_STICK_L) return CELL_WASD;
    if (icon == BI_STICK_R || icon == BI_DPAD) return CELL_ARROWS;
    for (size_t i = 0; i < sizeof fixed / sizeof *fixed; i++)
        if (fixed[i].icon == icon) return action_cell(fixed[i].action);
    if (icon < 1 || icon > 31 || !bit[icon]) return -1;
    for (int a = 14; a < ACTION_COUNT; a++) {                         // first action on that pad button that has a key
        uint64_t pad = rt_call(c, FN(F_GET_ASSIGN_PAD), 2, (uint64_t)a, 0ull);
        if (pad && !(LD32(pad + 0x10) & 0xff) && LD32(pad + 0x14) == bit[icon] && action_cell(a) >= 0) return action_cell(a);
    }
    return -1;
}

static __thread int t_action, t_kbicon;      // what the caller of GetSprite is drawing (0 = unknown)
// UIIconSpriteManager_ControllerBtn.GetSprite(ButtonIcon.eID, ControllerKind.eID)
int rt_hook_4543c80(Cpu *c) {
    int icon = (int)W(1), kind = (int)W(2), cell;
    static __thread int inside;
    if (inside) return 0;
    if (getenv("BR_GLYPH_LOG")) {                                    // the game's own picture for this icon: size and scale
        static int logged[32];
        if (icon > 0 && icon < 32 && !logged[icon]) {
            uint64_t mgr = X(0);
            logged[icon] = 1;
            inside = 1;
            uint64_t sp = rt_call(c, FN(0x4543c80ull), 4, mgr, (uint64_t)icon, 1ull, 0ull);
            inside = 0;
            if (sp) {
                rt_call(c, FN(0x3f20760ull), 2, sp, 0ull);
                float w = rt_getf(c, 2), h = rt_getf(c, 3);
                rt_call(c, FN(0x3f20a80ull), 2, sp, 0ull);
                rt_log("glyphs: the game's icon %d is %.0f x %.0f px at %.0f px per unit", icon, w, h, rt_getf(c, 0));
            }
            c->x[0] = mgr; c->x[1] = (uint64_t)icon; c->x[2] = (uint64_t)kind;
        }
    }
    if (kind == KIND_XBOX || kind == KIND_PS5 || kind == KIND_PS4) cell = pad_cell(kind, icon);
    else if (kind == KIND_KEYBOARD) cell = t_action ? action_cell(t_action) : t_kbicon ? kbicon_cell(t_kbicon) : button_action_cell(c, icon);
    else return 0;
    c->x[0] = glyph_sprite(c, cell);
    return 1;
}
// The callers that know more than the pad button. Each runs the original with the context set.
#define CONTEXT_HOOK(addr, nargs, ACTION, KBICON)                                              \
    int rt_hook_##addr(Cpu *c) {                                                               \
        static __thread int inside;                                                            \
        if (inside) return 0;                                                                  \
        uint64_t a0 = X(0), a1 = X(1), a2 = X(2), a3 = X(3);                                   \
        int pa = t_action, pk = t_kbicon;                                                      \
        t_action = (ACTION); t_kbicon = (KBICON);                                              \
        inside = 1;                                                                            \
        uint64_t r = rt_call(c, FN(0x##addr##ull), nargs, a0, a1, a2, a3);                     \
        inside = 0;                                                                            \
        t_action = pa; t_kbicon = pk;                                                          \
        c->x[0] = r;                                                                           \
        return 1;                                                                              \
    }
// UIBtnGuideDisplayModule_ForInputAction.getControllerBtnSprite(ActionAssignSet, ControllerKind): {+0x48 Action.eID}
CONTEXT_HOOK(4543800, 4, (int)LD32(a0 + 0x48), 0)
// UIBtnGuideDisplayModule_ForDB.getControllerBtnSprite_Main / _Sub: {+0x58 / +0x5c KeyboardKeyIcon.eID of the guide}
CONTEXT_HOOK(45432c0, 4, 0, (int)LD32(a0 + 0x58))
CONTEXT_HOOK(45432f0, 4, 0, (int)LD32(a0 + 0x5c))
// UIPartViewBtnGuideFixed_ForInputAction.Setup(Action.eID, ControllerKind)
CONTEXT_HOOK(44735f0, 4, (int)(uint32_t)a1, 0)
// The rows of Options > Key Configuration: UIPartView_KeyConfig_SelectableBtn.SetButton(UIOptionChoicesItemData_KeyConfig
// {+0x44 Action.eID}, ControllerKind) and SetUp(Action.eID, ControllerKind)
CONTEXT_HOOK(45643d0, 4, a1 ? (int)LD32(a1 + 0x44) : 0, 0)
CONTEXT_HOOK(45643f0, 4, (int)(uint32_t)a1, 0)

// ------------------------------------------------------------------------------------------------ glyphs in text
// Texts show buttons as TextMeshPro sprites: UITextEmojiDisplayModule turns its tags into "<sprite name=X>", X being
// the name the DB gives the button for the controller kind (ButtonIcon.EmojiTag). The names of the other kinds are in
// the DB ("ps5_button_..."), their pictures are not in the game's sprite sheet, so they printed as raw text. The
// port adds a sprite asset of its own for the atlas: a copy of the game's asset (same material and metrics), its
// tables replaced by one sprite per atlas cell named br_<cell>, added as a fallback of the sprite assets the texts
// use, and the tag functions answer the name br_<cell> for the port's kinds.
#define F_INSTANTIATE 0x3f112d0ull          // Object.Instantiate(Object) (static)
#define F_OBJ_SET_NAME 0x3f10920ull
#define F_STRING_NEW_UTF16 0x45af94ull
#define F_TMP_GET_SPRITE_ASSET 0x3e324c0ull // TMP_Text.spriteAsset
#define F_TMP_DEFAULT_SPRITE_ASSET 0x3e4e880ull   // TMP_Settings.defaultSpriteAsset (static)
#define F_ADD_SPRITE_ASSET 0x3dfc620ull     // MaterialReferenceManager.AddSpriteAsset(int hashCode, TMP_SpriteAsset) (static)
#define F_SIMPLE_HASH 0x3e528d0ull          // TMP_TextUtilities.GetSimpleHashCode(string) (static)
#define F_SEARCH_SPRITE 0x3e53060ull        // TMP_SpriteAsset.SearchForSpriteByHashCode(asset, int hash, bool fallbacks, out int index) (static)
#define F_UPDATE_LOOKUP 0x3e519b0ull        // TMP_SpriteAsset.UpdateLookupTables()
#define F_MAT_SET_MAIN_TEX 0x3ecedf0ull     // Material.mainTexture = Texture
#define F_BUTTON_ICON_TAG 0x45490e0ull      // UITextEmojiDisplayModule.getbuttonIconSpriteTag(ButtonIcon.eID, ControllerKind)
#define F_EMOJI_TAG 0x4549200ull            // UITextEmojiDisplayModule.getEmojiTag(Action.eID, eDeviceMask, ControllerKind)
#define ASSET_HASH 0x1c                     // TMP_Asset / TMP_SpriteAsset
#define ASSET_MATERIAL 0x20
#define ASSET_SHEET 0xa8
#define ASSET_CHARS 0xb0
#define ASSET_GLYPHS 0xc0
#define ASSET_DIRTY 0xe0
#define LIST_ITEMS 0x10
#define LIST_SIZE 0x18
#define ARR_LEN 0x18
#define F_LIST_CTOR 0x33bf070ull            // List<object>..ctor() (shared; the MethodInfo names the class)
#define M_LIST_SPRITE_ASSET_ADD 0x617c4c0ull   // List<TMP_SpriteAsset>.Add
#define ASSET_FALLBACKS 0xd8                // TMP_SpriteAsset.fallbackSpriteAssets
static uint32_t g_asset_h;
static int g_asset_failed;
static __thread int t_tag_inside;           // set while the game's own getbuttonIconSpriteTag runs for the port
static int pad_cell(int kind, int icon);
static int button_action_cell(Cpu *c, int icon);
#define NALIAS (4 * 31)                     // the game's own sprite names of the port's kinds, see emoji_asset
// Our asset as a fallback of a sprite asset of the game: "<sprite name=br_...>" in a text that uses that asset then
// finds the sprite there.
static void add_fallback(Cpu *c, uint64_t asset, uint64_t ours) {
    static uint64_t done[16];
    static int ndone;
    if (!asset || asset == ours) return;
    for (int i = 0; i < ndone; i++)
        if (done[i] == asset) return;
    if (ndone < 16) done[ndone++] = asset;
    uint64_t list = LD64(asset + ASSET_FALLBACKS);
    if (!list) {
        uint64_t mi = LD64(LD64(FN(M_LIST_SPRITE_ASSET_ADD)));
        if (!mi || (mi & 1)) { rt_call(c, FN(F_INIT_METADATA), 1, LD64(FN(M_LIST_SPRITE_ASSET_ADD))); mi = LD64(LD64(FN(M_LIST_SPRITE_ASSET_ADD))); }
        list = rt_call(c, FN(F_OBJECT_NEW), 1, LD64(mi + 0x20));     // MethodInfo.klass
        rt_call(c, FN(F_LIST_CTOR), 2, list, mi);
        ST64(asset + ASSET_FALLBACKS, list);
    }
    uint64_t items = LD64(list + LIST_ITEMS);
    int n = (int)LD32(list + LIST_SIZE), cap = items ? (int)LD32(items + ARR_LEN) : 0;
    for (int i = 0; i < n; i++)
        if (LD64(items + ARR_DATA + 8 * (uint64_t)i) == ours) return;
    if (!items) { rt_log("glyphs: a sprite asset's fallback list has no storage"); return; }
    if (n >= cap) {
        uint64_t bigger = rt_call(c, FN(F_SZARRAY_NEW), 2, LD64(items), (uint64_t)(n + 4));
        memcpy((void *)(uintptr_t)(bigger + ARR_DATA), (void *)(uintptr_t)(items + ARR_DATA), 8 * (size_t)n);
        ST64(list + LIST_ITEMS, bigger);
        items = bigger;
    }
    ST64(items + ARR_DATA + 8 * (uint64_t)n, ours);
    ST32(list + LIST_SIZE, n + 1);
    ST32(list + LIST_SIZE + 4, LD32(list + LIST_SIZE + 4) + 1);      // version
}
static uint64_t str8(Cpu *c, const char *s) {
    uint16_t w[96];
    int n = 0;
    for (; s[n] && n < 95; n++) w[n] = (uint8_t)s[n];
    return rt_call(c, FN(F_STRING_NEW_UTF16), 2, (uint64_t)(uintptr_t)w, (uint64_t)n);
}
static int emoji_asset(Cpu *c, uint64_t module) {
    uint64_t text = LD64(module + 0x10), asset = text ? rt_call(c, FN(F_TMP_GET_SPRITE_ASSET), 2, text, 0ull) : 0;
    uint64_t ours = target(c, g_asset_h);
    if (alive(c, ours)) {                                            // made: this text's asset (or the default) must know it
        add_fallback(c, asset ? asset : rt_call(c, FN(F_TMP_DEFAULT_SPRITE_ASSET), 1, 0ull), ours);
        return 1;
    }
    if (g_asset_failed) return 0;
    uint64_t text_asset = asset;
    if (!asset) asset = rt_call(c, FN(F_TMP_DEFAULT_SPRITE_ASSET), 1, 0ull);
    // a button sprite of the game's own asset is the model for size and placement in a line of text
    uint64_t name = asset ? rt_call(c, FN(F_BUTTON_ICON_TAG), 4, module, (uint64_t)BI_EAST, 1ull, 0ull) : 0;
    int idx = -1;
    if (name) {                                                      // it may be in a fallback asset of the text's asset
        uint64_t found = rt_call(c, FN(F_SEARCH_SPRITE), 5, asset, (uint64_t)(uint32_t)rt_call(c, FN(F_SIMPLE_HASH), 2, name, 0ull), 1ull,
                                 (uint64_t)(uintptr_t)&idx, 0ull);
        if (!found) {
            uint64_t def = rt_call(c, FN(F_TMP_DEFAULT_SPRITE_ASSET), 1, 0ull);
            found = def ? rt_call(c, FN(F_SEARCH_SPRITE), 5, def, (uint64_t)(uint32_t)rt_call(c, FN(F_SIMPLE_HASH), 2, name, 0ull), 1ull,
                                  (uint64_t)(uintptr_t)&idx, 0ull) : 0;
        }
        asset = found;
        if (!found) idx = -1;
    }
    uint64_t chars = asset ? LD64(asset + ASSET_CHARS) : 0, glyphs = asset ? LD64(asset + ASSET_GLYPHS) : 0;
    if (idx < 0 || !chars || !glyphs || idx >= (int)LD32(chars + LIST_SIZE) || !glyph_sprite(c, CELL_KEYS + 1)) {
        rt_log("glyphs: no sprite asset for text (asset %d, model sprite %d); texts keep the game's own button pictures", asset != 0, idx);
        g_asset_failed = 1;
        return 0;
    }
    uint64_t tc = LD64(LD64(chars + LIST_ITEMS) + ARR_DATA + 8 * (uint64_t)idx), tg = LD64(tc + 0x20), tex = target(c, g_tex_h);
    uint64_t clone = rt_call(c, FN(F_INSTANTIATE), 2, asset, 0ull), mat = rt_call(c, FN(F_INSTANTIATE), 2, LD64(asset + ASSET_MATERIAL), 0ull);
    if (!clone || !mat || !tg) { g_asset_failed = 1; return 0; }
    rt_call(c, FN(F_OBJ_SET_NAME), 3, clone, str8(c, "BRGlyphs"), 0ull);
    rt_call(c, FN(F_OBJ_SET_HIDE_FLAGS), 3, clone, 32ull, 0ull);
    rt_call(c, FN(F_OBJ_SET_HIDE_FLAGS), 3, mat, 32ull, 0ull);
    rt_call(c, FN(F_MAT_SET_MAIN_TEX), 3, mat, tex, 0ull);
    int hash = (int)rt_call(c, FN(F_SIMPLE_HASH), 2, str8(c, "BRGlyphs"), 0ull);
    ST64(clone + ASSET_MATERIAL, mat);
    ST64(clone + ASSET_SHEET, tex);
    ST32(clone + ASSET_HASH, hash);
    uint64_t cl = LD64(clone + ASSET_CHARS), gl = LD64(clone + ASSET_GLYPHS);
    uint64_t carr = rt_call(c, FN(F_SZARRAY_NEW), 2, LD64(LD64(cl + LIST_ITEMS)), (uint64_t)(NCELLS + NALIAS));
    uint64_t garr = rt_call(c, FN(F_SZARRAY_NEW), 2, LD64(LD64(gl + LIST_ITEMS)), (uint64_t)NCELLS);
    for (int cell = 0; cell < NCELLS; cell++) {
        char nm[16];
        uint64_t g = rt_call(c, FN(F_OBJECT_NEW), 1, LD64(tg)), ch = rt_call(c, FN(F_OBJECT_NEW), 1, LD64(tc));
        memcpy((void *)(uintptr_t)(g + 0x10), (void *)(uintptr_t)(tg + 0x10), 0x40);     // TMP_SpriteGlyph: metrics, scale, ...
        ST32(g + 0x10, cell);                                                             // index
        ST32(g + 0x28, (cell % COLS) * CELL); ST32(g + 0x2c, (ROWS - 1 - cell / COLS) * CELL);   // glyph rect, from the bottom left
        ST32(g + 0x30, CELL); ST32(g + 0x34, CELL);
        ST64(g + 0x48, 0);                                                                // sprite
        memcpy((void *)(uintptr_t)(ch + 0x10), (void *)(uintptr_t)(tc + 0x10), 0x30);     // TMP_SpriteCharacter: type, scale
        snprintf(nm, sizeof nm, "br_%03d", cell);
        uint64_t ns = str8(c, nm);
        ST32(ch + 0x14, 0xe000 + cell);                                                   // unicode (private use)
        ST64(ch + 0x18, clone); ST64(ch + 0x20, g); ST32(ch + 0x28, cell);                // asset, glyph, glyph index
        ST64(ch + 0x30, ns); ST32(ch + 0x38, (uint32_t)rt_call(c, FN(F_SIMPLE_HASH), 2, ns, 0ull));
        ST64(carr + ARR_DATA + 8 * (uint64_t)cell, ch);
        ST64(garr + ARR_DATA + 8 * (uint64_t)cell, g);
    }
    // Not every text asks the hooked tag functions for a name: a group of actions that is one stick or the d-pad
    // (replaceInputActionGroupToEmoji: "tilt the stage with <left stick>") takes the name straight from the DB
    // (ButtonIcon.EmojiTag[kind]). Those names get sprites here too, as further names of the same cells, so
    // such a text shows the picture instead of printing its tag.
    int nchars = NCELLS;
    static const int kinds[4] = {KIND_XBOX, KIND_PS5, KIND_PS4, KIND_KEYBOARD};
    for (int k = 0; k < 4; k++)
        for (int icon = 1; icon <= 31; icon++) {
            int cell = kinds[k] == KIND_KEYBOARD ? button_action_cell(c, icon) : pad_cell(kinds[k], icon);
            if (cell < 0 || cell >= NCELLS) continue;
            t_tag_inside = 1;
            uint64_t ns = rt_call(c, FN(F_BUTTON_ICON_TAG), 4, module, (uint64_t)icon, (uint64_t)kinds[k], 0ull);
            t_tag_inside = 0;
            if (!ns || !LD32(ns + 0x10)) continue;                                        // no name in the DB
            uint64_t ch = rt_call(c, FN(F_OBJECT_NEW), 1, LD64(tc));
            memcpy((void *)(uintptr_t)(ch + 0x10), (void *)(uintptr_t)(tc + 0x10), 0x30);
            ST32(ch + 0x14, 0xe000 + nchars);
            ST64(ch + 0x18, clone); ST64(ch + 0x20, LD64(garr + ARR_DATA + 8 * (uint64_t)cell)); ST32(ch + 0x28, cell);
            ST64(ch + 0x30, ns); ST32(ch + 0x38, (uint32_t)rt_call(c, FN(F_SIMPLE_HASH), 2, ns, 0ull));
            ST64(carr + ARR_DATA + 8 * (uint64_t)nchars, ch);
            nchars++;
        }
    ST64(cl + LIST_ITEMS, carr); ST32(cl + LIST_SIZE, nchars);
    ST64(gl + LIST_ITEMS, garr); ST32(gl + LIST_SIZE, NCELLS);
    ST32(clone + ASSET_DIRTY, (LD32(clone + ASSET_DIRTY) & ~0xffu) | 1);
    rt_call(c, FN(F_UPDATE_LOOKUP), 2, clone, 0ull);
    rt_call(c, FN(F_ADD_SPRITE_ASSET), 3, (uint64_t)(uint32_t)hash, clone, 0ull);
    g_asset_h = (uint32_t)rt_call(c, FN(F_GCHANDLE_NEW), 2, clone, 0ull);
    ST64(clone + ASSET_FALLBACKS, 0);                                 // the copy is nobody's parent
    add_fallback(c, text_asset ? text_asset : rt_call(c, FN(F_TMP_DEFAULT_SPRITE_ASSET), 1, 0ull), clone);
    rt_log("glyphs: sprite asset for text added (%d sprites, %d more names from the game's table, modelled on the game's sprite %d)",
           NCELLS, nchars - NCELLS, idx);
    return 1;
}
static uint64_t cell_tag(Cpu *c, int cell) {
    char tag[64];
    snprintf(tag, sizeof tag, "<sprite name=br_%03d>", cell);
    return str8(c, tag);
}
static uint64_t cell_name(Cpu *c, int cell) {
    char nm[16];
    snprintf(nm, sizeof nm, "br_%03d", cell);
    return str8(c, nm);
}
static int kind_is_ours(int kind) { return kind == KIND_XBOX || kind == KIND_PS5 || kind == KIND_PS4 || kind == KIND_KEYBOARD; }
// The sprite name of a button for a kind. A kind of ours: the cell's name; without the asset, the Switch name.
static int icon_tag(Cpu *c, uint64_t module, int icon, int kind) {
    if (t_tag_inside || !kind_is_ours(kind)) return 0;
    if (!emoji_asset(c, module)) {
        t_tag_inside = 1;
        uint64_t r = rt_call(c, FN(F_BUTTON_ICON_TAG), 4, module, (uint64_t)icon, 1ull, 0ull);
        t_tag_inside = 0;
        c->x[0] = r;
        return 1;
    }
    int cell = kind == KIND_KEYBOARD ? (t_action ? action_cell(t_action) : button_action_cell(c, icon)) : pad_cell(kind, icon);
    c->x[0] = cell >= 0 ? cell_name(c, cell) : str8(c, "");
    return 1;
}
// UITextEmojiDisplayModule.getbuttonIconSpriteTag(ButtonIcon.eID, ControllerKind) and (eButton, ControllerKind)
int rt_hook_45490e0(Cpu *c) { return icon_tag(c, X(0), (int)W(1), (int)W(2)); }
int rt_hook_45494f0(Cpu *c) {
    static const uint8_t icon_of_bit[28] = {BI_UP, BI_DOWN, BI_LEFT, BI_RIGHT, BI_DUP, BI_DDOWN, BI_DLEFT, BI_DRIGHT, BI_LS_UP, BI_LS_DOWN, BI_LS_LEFT, BI_LS_RIGHT,
                                            BI_EAST, BI_SOUTH, BI_NORTH, BI_WEST, BI_L, BI_R, BI_ZL, BI_ZR, BI_STICK_L, BI_STICK_R, BI_PLUS, BI_MINUS,
                                            BI_RS_UP, BI_RS_DOWN, BI_RS_LEFT, BI_RS_RIGHT};
    uint32_t button = W(1);
    int icon = 0;
    for (int i = 0; i < 28 && !icon; i++)
        if (button == (1u << i)) icon = icon_of_bit[i];
    return icon ? icon_tag(c, X(0), icon, (int)W(2)) : 0;
}
// UITextEmojiDisplayModule.getEmojiTag(Action.eID, eDeviceMask, ControllerKind): the whole tag for an action. On
// the keyboard that is the action's one key (the pad may have several buttons for it, which the original would
// each turn into the same key).
int rt_hook_4549200(Cpu *c) {
    static __thread int inside;
    if (inside) return 0;
    uint64_t module = X(0), a1 = X(1), a2 = X(2), a3 = X(3);
    if ((int)(uint32_t)a3 == KIND_KEYBOARD && action_cell((int)(uint32_t)a1) >= 0 && emoji_asset(c, module)) {
        c->x[0] = cell_tag(c, action_cell((int)(uint32_t)a1));
        return 1;
    }
    int pa = t_action;
    t_action = (int)(uint32_t)a1;
    inside = 1;
    uint64_t r = rt_call(c, FN(F_EMOJI_TAG), 5, module, a1, a2, a3, 0ull);
    inside = 0;
    t_action = pa;
    c->x[0] = r;
    return 1;
}

// A text inside a button (the kick button's "-/+") names pad buttons, not the action. For the keyboard the action
// matters: UIPartViewButtonBase.SetupButtonGuide(Action.eID, int controllerIdx) tells which action a button stands
// for, its text is remembered, and UITextEmojiDisplayModule.updateText(string, eDeviceMask, ControllerKind) writes
// the action's one key into such a text instead of translating its pad buttons.
#define F_GET_GAMEOBJECT 0x3f07fb0ull
#define F_GET_TRANSFORM 0x3f07f60ull
#define F_CHILD_COUNT 0x3f1e1f0ull
#define F_GET_CHILD 0x3f1ea10ull
#define F_OBJ_NAME 0x3f10820ull
#define F_GO_GET_COMPONENT_NAME 0x3f0adf0ull   // GameObject.GetComponent(string type)
#define F_TMP_SET_TEXT 0x3e38300ull         // TMP_Text.SetText(string, bool)
static struct { uint64_t text; int action; } g_button_text[64];
static int g_button_text_next;
static uint64_t find_text(Cpu *c, uint64_t t, int depth) {            // the transform named TextEmoji under t
    int n = t ? (int)rt_call(c, FN(F_CHILD_COUNT), 2, t, 0ull) : 0;
    for (int i = 0; i < n; i++) {
        uint64_t ch = rt_call(c, FN(F_GET_CHILD), 3, t, (uint64_t)i, 0ull);
        uint64_t s = rt_call(c, FN(F_OBJ_NAME), 2, rt_call(c, FN(F_GET_GAMEOBJECT), 2, ch, 0ull), 0ull);
        static const char want[] = "TextEmoji";
        int len = s ? (int)LD32(s + 0x10) : 0, k = 0;
        for (; k < len && want[k] && LD16(s + 0x14 + 2 * (uint64_t)k) == (uint8_t)want[k]; k++) {}
        if (k == len && !want[k]) return ch;
        uint64_t hit = depth > 0 ? find_text(c, ch, depth - 1) : 0;
        if (hit) return hit;
    }
    return 0;
}
int rt_hook_4512980(Cpu *c) {
    uint64_t view = X(0), a1 = X(1), a2 = X(2), lr = c->x[30];
    if (view) {
        uint64_t t = find_text(c, rt_call(c, FN(F_GET_TRANSFORM), 2, view, 0ull), 3);
        uint64_t text = t ? rt_call(c, FN(F_GO_GET_COMPONENT_NAME), 3, rt_call(c, FN(F_GET_GAMEOBJECT), 2, t, 0ull), str8(c, "TextMeshProUGUI"), 0ull) : 0;
        if (text) {
            int at = -1;
            for (int i = 0; i < 64 && at < 0; i++)
                if (g_button_text[i].text == text) at = i;
            if (at < 0) at = g_button_text_next++ % 64;
            g_button_text[at].text = text; g_button_text[at].action = (int)(uint32_t)a1;
        }
        c->x[0] = view; c->x[1] = a1; c->x[2] = a2; c->x[30] = lr;
    }
    return 0;
}
int rt_hook_4548030(Cpu *c) {
    uint64_t module = X(0), a1 = X(1), a2 = X(2), a3 = X(3), lr = c->x[30], text = LD64(module + 0x10);
    if (!kind_is_ours((int)(uint32_t)a3) || !text) return 0;
    // every text that may show a button of the port's kinds must know the port's sprite asset, also when none of
    // its tags goes through the hooked tag functions (see the names taken from the DB in emoji_asset)
    emoji_asset(c, module);
    for (int i = 0; i < 64 && (int)(uint32_t)a3 == KIND_KEYBOARD; i++) {
        if (g_button_text[i].text != text) continue;
        int cell = action_cell(g_button_text[i].action);
        if (cell >= 0 && emoji_asset(c, module)) {
            rt_call(c, FN(F_TMP_SET_TEXT), 4, text, cell_tag(c, cell), 1ull, 0ull);
            return 1;
        }
        break;
    }
    c->x[0] = module; c->x[1] = a1; c->x[2] = a2; c->x[3] = a3; c->x[30] = lr;
    return 0;
}

// Style menu for the add-on characters.
//
// Whether a character has a Style entry (outfits, ball, ball effect...) is one flag of its row in the game's
// character table, `Def_Chara.IsCustomize`, read only through Wonder.DB.Def.Chara.Record::IsCustomize. The save
// keeps a CharaCustomizeCharacter per character that has the flag (made on demand by
// SaveUserParam.CharaCustomizeParam.GetCharacterParam), and the character select screen shows the Style button
// only when that entry exists. The six SEGA characters (Sonic .. Axel) and the five collaboration characters
// (Collab01..05) have the flag off, so they had no Style button at all.
//
// The hook answers "yes" for them. A second flag, `IsCustomizeFull`, is left as the table has it: off for these
// characters, as for Dr. Bad-Boon, so they get his reduced Style menu (ball, ball effect and the other
// non-outfit items) rather than the monkeys' outfit parts, which do not fit their models.
// BR_STYLE_LOG=1 logs each character's two flags once.
#include <stdio.h>
#include <stdlib.h>
#include "rt.h"

#define RECORD_ID 0x18              // IDRecord<...>.id (Def.Chara.eID)
#define RECORD_RAW 0x20             // IDRecord<...>.rawRecord (Def.Chara.RawRecord)
#define RAW_IS_CUSTOMIZE 0x18       // RawRecord.IsCustomize
#define RAW_IS_CUSTOMIZE_FULL 0x70  // RawRecord.IsCustomizeFull
#define CHARA_FIRST_ADDON 13        // Def.Chara.eID Sonic
#define CHARA_LAST_ADDON 23         // Def.Chara.eID Collab05

// Wonder.DB.Def.Chara.Record::IsCustomize
int rt_hook_47cdba0(Cpu *c) {
    uint64_t rec = c->x[0], raw = rec ? LD64(rec + RECORD_RAW) : 0;
    uint32_t id = rec ? LD32(rec + RECORD_ID) : 0;
    int table = raw ? LD8(raw + RAW_IS_CUSTOMIZE) != 0 : 0;
    int yes = table || (raw && id >= CHARA_FIRST_ADDON && id <= CHARA_LAST_ADDON);
    static char seen[64];
    static int log = -1;
    if (log < 0) { const char *e = getenv("BR_STYLE_LOG"); log = e && *e == '1'; }
    if (log && id < sizeof seen && !seen[id]) {
        seen[id] = 1;
        rt_log("style: chara %u table IsCustomize=%d IsCustomizeFull=%d -> %d", id, table,
               raw ? LD8(raw + RAW_IS_CUSTOMIZE_FULL) != 0 : 0, yes);
    }
    c->x[0] = (uint64_t)yes;
    return 1;
}

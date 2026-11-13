"""delisted.py - takes six characters out of the game's unlock table; the port gives them to every save.

Banana Rumble's data holds six characters that a player today cannot get: Dr. Bad-Boon (unlocked by playing one
online battle on Nintendo's service) and the five collaboration characters (Def.Chara.eID Collab01..05), whose
add-on content was taken off sale. Their models, icons and statistics are in every copy of the game; only the
save says whether a player has them, and the table `Unlock_Subject_Chara` says when a save gets them (the mod
"Delisted Character Fix" by iswimfly, https://gamebanana.com/mods/690229, found that and rewrites those rows so
the six unlock with World 10; nothing of the mod is used here).

On PC they are simply there from the start, without unlock notices or "new" marks: port\\runtime\\delisted.c marks
them as owned whenever a save is created or loaded (those whose model is in the game data: version 2.00 has
Dr. Bad-Boon and the first two collaboration characters, the other three come with their add-on content). This
script does the other half: it switches off their rows
of the unlock table, so the game's own unlock rules (an online battle, owning the add-on content) never fire a
notice for a character the player already has.

A row of the table (little endian ints): _id, TermsID (Unlock.Terms.eID), CharaID (Def.Chara.eID), ConditionID
(Shop.Item.Condition.eID), NoticeID (Notice.Kind.eID), DlcID (Dlc.Kind.eID), _empty, _order.

    python delisted.py <...\\StreamingAssets\\aa\\Switch>
"""
import os
import struct
import sys

import UnityPy

TABLE = 'Unlock_Subject_Chara'
BUNDLE = os.path.join('db_assets_db', 'unlock_subject_chara.asset.bundle')
CHARAS = (12, 19, 20, 21, 22, 23)   # Def.Chara.eID: Madhihi (Dr. Bad-Boon), Collab01..Collab05 (the same list is in delisted.c)


def patch_rows(raw):
    """The table's bytes with the six characters' rows switched off; None when nothing changes."""
    n = struct.unpack_from('<I', raw, 0x1c)[0]
    if raw[0x20:0x20 + n].decode('ascii', 'replace') != TABLE:
        raise ValueError('not the table ' + TABLE)
    at = 0x20 + (n + 3) // 4 * 4
    count = struct.unpack_from('<I', raw, at)[0]
    if at + 4 + 32 * count != len(raw):
        raise ValueError(f'{TABLE}: unexpected size')
    out = bytearray(raw)
    for i in range(count):
        row = at + 4 + 32 * i
        if struct.unpack_from('<I', raw, row + 8)[0] in CHARAS:
            struct.pack_into('<I', out, row + 24, 1)        # _empty
    return None if out == raw else bytes(out)


def patch(aa_switch):
    """Rewrite the table in the game's asset folder. Returns True when the file was changed."""
    path = os.path.join(aa_switch, BUNDLE)
    if not os.path.exists(path):
        return False
    with open(path, 'rb') as f:              # from memory, so the file is not held open while it is replaced
        env = UnityPy.load(f.read())
    obj = next((o for o in env.objects if o.type.name == 'MonoBehaviour' and o.peek_name() == TABLE), None)
    if obj is None:
        return False
    new = patch_rows(bytes(obj.get_raw_data()))
    if new is None:
        return False
    obj.set_raw_data(new)
    data = env.file.save(packer='lz4')
    tmp = path + '.tmp'
    with open(tmp, 'wb') as f:
        f.write(data)
    os.remove(path)                 # the file may be a hard link to the build tree: replace it, do not write through it
    os.replace(tmp, path)
    return True


if __name__ == '__main__':
    print('changed' if patch(sys.argv[1]) else 'nothing to change')

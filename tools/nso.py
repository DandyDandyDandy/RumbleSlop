"""NSO0 loader: decompress segments, parse MOD0/.dynamic/.dynsym.

    python nso.py info <nso> [...]            layout, build-id, dynamic info, symbol counts
    python nso.py dump <nso> <out.bin>        write the flat memory image (text+rodata+data)
    python nso.py pack <image.bin> <header.hex> <out nso>   the reverse, with the file's original header
    python nso.py syms <nso> <out.tsv>        write dynsym: kind(imp/exp) type bind value size name
"""
import struct, sys, hashlib
import lz4.block


class Nso:
    def __init__(self, path):
        f = open(path, 'rb').read()
        assert f[:4] == b'NSO0', 'not NSO0'
        self.version, _, self.flags = struct.unpack_from('<III', f, 4)
        self.segs = []
        for i, name in enumerate(('text', 'rodata', 'data')):
            foff, moff, size = struct.unpack_from('<III', f, 0x10 + 0x10 * i)
            csize = struct.unpack_from('<I', f, 0x60 + 4 * i)[0]
            raw = f[foff:foff + csize]
            comp = bool(self.flags & (1 << i))
            d = lz4.block.decompress(raw, uncompressed_size=size) if comp else raw
            assert len(d) == size
            hash_ok = None
            if self.flags & (8 << i):
                hash_ok = hashlib.sha256(d).digest() == f[0xA0 + 0x20 * i:0xC0 + 0x20 * i]
            self.segs.append(dict(name=name, foff=foff, moff=moff, size=size, csize=csize, comp=comp, hash_ok=hash_ok, data=d))
        self.bss = struct.unpack_from('<I', f, 0x3C)[0]
        self.build_id = f[0x40:0x60].hex()
        self.hdr_dynstr = struct.unpack_from('<II', f, 0x90)
        self.hdr_dynsym = struct.unpack_from('<II', f, 0x98)
        end = max(s['moff'] + s['size'] for s in self.segs)
        img = bytearray(end)
        for s in self.segs:
            img[s['moff']:s['moff'] + s['size']] = s['data']
        self.image = img
        self.text = (self.segs[0]['moff'], self.segs[0]['size'])
        self.rodata = (self.segs[1]['moff'], self.segs[1]['size'])
        self.data = (self.segs[2]['moff'], self.segs[2]['size'])
        self._dyn()

    def _dyn(self):
        img = self.image
        mod = struct.unpack_from('<I', img, 4)[0]
        self.mod0 = mod
        assert img[mod:mod + 4] == b'MOD0', 'no MOD0'
        dynoff, bss0, bss1, eh0, eh1, modobj = struct.unpack_from('<iiiiii', img, mod + 4)
        self.dynamic = mod + dynoff
        self.bss_range = (mod + bss0, mod + bss1)
        self.eh_frame_hdr = (mod + eh0, mod + eh1)
        self.dt = {}
        self.needed = []
        p = self.dynamic
        while True:
            tag, val = struct.unpack_from('<qQ', img, p)
            p += 16
            if tag == 0:
                break
            if tag == 1:
                self.needed.append(val)
            else:
                self.dt[tag] = val
        st, sy = self.dt.get(5), self.dt.get(6)
        self.syms = []
        if st is None or sy is None:
            return
        # symbol count: dynsym runs up to dynstr in Switch modules (header size field as cross-check)
        n = self.hdr_dynsym[1] // 24 if self.hdr_dynsym[1] else (st - sy) // 24
        self.nsyms = n
        for i in range(n):
            nm, info, other, shndx, value, size = struct.unpack_from('<IBBHQQ', img, sy + 24 * i)
            e = img.index(b'\0', st + nm)
            self.syms.append(dict(name=img[st + nm:e].decode('latin1'), type=info & 15, bind=info >> 4, shndx=shndx, value=value, size=size))
        self.needed = [img[st + v:img.index(b'\0', st + v)].decode() for v in self.needed]

    def imports(self):
        return [s for s in self.syms if s['shndx'] == 0 and s['name']]

    def exports(self):
        return [s for s in self.syms if s['shndx'] != 0 and s['name']]


DT = {2: 'PLTRELSZ', 3: 'PLTGOT', 4: 'HASH', 5: 'STRTAB', 6: 'SYMTAB', 7: 'RELA', 8: 'RELASZ', 9: 'RELAENT', 10: 'STRSZ', 11: 'SYMENT',
      12: 'INIT', 13: 'FINI', 14: 'SONAME', 20: 'PLTREL', 23: 'JMPREL', 25: 'INIT_ARRAY', 26: 'FINI_ARRAY', 27: 'INIT_ARRAYSZ',
      28: 'FINI_ARRAYSZ', 30: 'FLAGS', 0x6ffffef5: 'GNU_HASH', 0x6ffffff9: 'RELACOUNT', 0x6ffffffb: 'FLAGS_1'}

def pack(image_path, header_path, out_path):
    """The reverse of dump: an uncompressed NSO from the flat image (text+rodata+data, as written by dump) and the
    file's original header. A finished game folder keeps the image, so the port's code can be generated again from
    the game folder alone. Fails when the image does not have the size the header names."""
    hdr = bytearray.fromhex(''.join(l.strip() for l in open(header_path) if not l.startswith('#')))
    img = open(image_path, 'rb').read()
    assert len(hdr) == 0x100 and hdr[:4] == b'NSO0', header_path
    segs = [struct.unpack_from('<III', hdr, 0x10 + 0x10 * i)[1:] for i in range(3)]      # (memory offset, size)
    if len(img) < segs[2][0] + segs[2][1]:
        sys.exit(f'{image_path}: {len(img)} bytes, too short for this version of the game executable')
    struct.pack_into('<I', hdr, 0xC, 0)                                                 # not compressed, no hash check
    off, parts = 0x100, []
    for i, (moff, size) in enumerate(segs):
        struct.pack_into('<I', hdr, 0x10 + 0x10 * i, off)
        struct.pack_into('<I', hdr, 0x60 + 4 * i, size)
        parts.append(img[moff:moff + size])
        off += size
    with open(out_path, 'wb') as f:
        f.write(bytes(hdr))
        for d in parts:
            f.write(d)


if __name__ == '__main__':
    cmd = sys.argv[1]
    if cmd == 'info':
        for p in sys.argv[2:]:
            n = Nso(p)
            print(f'== {p}  flags={n.flags:#x} build_id={n.build_id}')
            for s in n.segs:
                print(f"  {s['name']:6} mem={s['moff']:#10x} size={s['size']:#10x} ({s['size']}) filesz={s['csize']} lz4={s['comp']} sha256_ok={s['hash_ok']}")
            print(f'  bss={n.bss:#x}  MOD0@{n.mod0:#x} dynamic@{n.dynamic:#x} eh_frame_hdr={n.eh_frame_hdr[0]:#x}-{n.eh_frame_hdr[1]:#x}')
            print('  dynamic:', ', '.join(f'{DT.get(k, hex(k))}={v:#x}' for k, v in n.dt.items()))
            print('  needed:', n.needed)
            imp, exp = n.imports(), n.exports()
            print(f'  dynsym entries={len(n.syms)} imports={len(imp)} (func={sum(s["type"]==2 for s in imp)} obj={sum(s["type"]==1 for s in imp)} notype={sum(s["type"]==0 for s in imp)} weak={sum(s["bind"]==2 for s in imp)}) exports={len(exp)}')
    elif cmd == 'pack':                      # pack <image.bin> <header.hex> <out nso>: see pack()
        pack(sys.argv[2], sys.argv[3], sys.argv[4])
    elif cmd == 'dump':
        open(sys.argv[3], 'wb').write(Nso(sys.argv[2]).image)
    elif cmd == 'syms':
        n = Nso(sys.argv[2])
        with open(sys.argv[3], 'w', encoding='utf-8') as o:
            for s in n.syms:
                if s['name']:
                    o.write(f"{'imp' if s['shndx']==0 else 'exp'}\t{s['type']}\t{s['bind']}\t{s['value']:#x}\t{s['size']:#x}\t{s['name']}\n")

"""Convert Switch texture data to what the Windows player can use.

    python tex_convert.py tree <in aa dir> <out aa dir> [--jobs N] [--cache DIR]
    python tex_convert.py file <in file> <out file> [--cache DIR]
    python tex_convert.py check <in aa dir> [--jobs N] [--extra file]     (layout validation only, writes nothing)

Two things are wrong with the Switch data on Windows:
  * Non-crunched textures carrying an m_PlatformBlob are stored in the Tegra block-linear ("swizzled") layout, each
    mip padded and at the offset given in the blob. They are rewritten as tightly packed linear mip chains and the
    blob is removed.
  * ASTC 4x4 (format 48) cannot be decoded by the Windows player. It is transcoded to BC7 (format 25), which has
    the same 16-byte 4x4 block geometry, so sizes and mip layout are unchanged.
Texture data that lives in a .resS is patched in place (the new data is never larger), so other users of the
resource file (meshes) keep their offsets. In tree mode untouched files are hard-linked.
"""
import sys, os, argparse, struct, hashlib, traceback, multiprocessing as mp, collections
import numpy as np

ASTC, BC7 = 48, 25
# format -> (bytes per block, block width, block height) in the swizzled layout
GEOM = {1: (1, 1, 1), 3: (4, 1, 1), 4: (4, 1, 1), 5: (4, 1, 1), 14: (4, 1, 1), 10: (8, 4, 4), 12: (16, 4, 4),
        24: (16, 4, 4), 25: (16, 4, 4), 26: (8, 4, 4), 27: (16, 4, 4), 48: (16, 4, 4), 63: (1, 1, 1), 62: (2, 1, 1),
        9: (2, 1, 1), 7: (2, 1, 1), 13: (2, 1, 1), 2: (2, 1, 1), 17: (8, 1, 1), 20: (16, 1, 1), 15: (2, 1, 1),
        16: (4, 1, 1), 18: (4, 1, 1), 19: (8, 1, 1)}
TYPES = ('Texture2D', 'Cubemap', 'Texture2DArray', 'Texture3D', 'CubemapArray')
_idx_cache = {}
CACHE = None
# ASTC textures whose size is not a power of two become RGBA32 instead of BC7: the Windows Vulkan player showed
# non-power-of-two BC7 textures as garbage although the data decodes correctly (TEX_NPOT=bc7 disables this)
RGBA_NPOT = os.environ.get('TEX_NPOT', 'rgba') != 'bc7'


def align(v, a):
    return (v + a - 1) // a * a


def mip_index(stride, rows, bh):
    """Index array (rows x stride) of source byte offsets inside one block-linear mip, and the mip's stored size."""
    key = (stride, rows, bh)
    r = _idx_cache.get(key)
    if r is None:
        sp = align(stride, 64)
        rob = 512 * bh * (sp // 64)
        y = np.arange(rows, dtype=np.int64); x = np.arange(stride, dtype=np.int64)
        yt = (y // 8 // bh) * rob + ((y // 8) % bh) * 512 + ((y % 8) // 2) * 64 + (y % 2) * 16
        xt = (x // 64) * 512 * bh + ((x % 64) // 32) * 256 + ((x % 32) // 16) * 32 + (x % 16)
        r = ((yt[:, None] + xt[None, :]).ravel(), sp * align(rows, 8 * bh))
        if len(_idx_cache) > 64:
            _idx_cache.clear()
        _idx_cache[key] = r
    return r


def deswizzle(data, fmt, w, h, mips, blob):
    """-> linear bytes (tight mip chain). Raises ValueError when the layout does not match the blob."""
    u = struct.unpack('<%dI' % (len(blob) // 4), blob)
    if len(u) != 23 or u[1] != 0x20:
        raise ValueError(f'unknown blob shape {len(u)} {u[:4]}')
    if fmt not in GEOM:
        raise ValueError(f'no geometry for format {fmt}')
    bpb, bw, bhp = GEOM[fmt]
    offs = list(u[4:4 + mips]) + [u[22]]
    if u[21] != u[22] or u[22] > len(data):
        raise ValueError(f'layer size {u[21]} {u[22]} data {len(data)}')
    bh = 1 << u[2]
    src = np.frombuffer(data, dtype=np.uint8)
    out = []
    for i in range(mips):
        mw, mh = max(1, w >> i), max(1, h >> i)
        stride, rows = -(-mw // bw) * bpb, -(-mh // bhp)
        while bh > 1 and rows <= bh * 8 // 2:
            bh //= 2
        idx, size = mip_index(stride, rows, bh)
        if align(size, 512) != offs[i + 1] - offs[i] and not (i == mips - 1 and offs[i] + size <= offs[i + 1]):
            raise ValueError(f'mip {i} {mw}x{mh} size {size} (bh {bh}) but blob says {offs[i + 1] - offs[i]}')
        m = src[offs[i]:offs[i] + size][idx]
        if fmt == 3:   # RGB24 is stored as 4 bytes per pixel when swizzled
            m = m.reshape(-1, 4)[:, :3].ravel()
        out.append(m.tobytes())
    return b''.join(out)


def astc_to_bc7(data):
    """Transcode a stream of 16-byte ASTC 4x4 blocks to BC7 blocks (same count, same order)."""
    if len(data) % 16:
        raise ValueError('ASTC data is not a whole number of blocks')
    key = None
    if CACHE:
        key = os.path.join(CACHE, hashlib.sha1(data).hexdigest())
        try:
            with open(key, 'rb') as f:
                r = f.read()
            if len(r) == len(data):
                return r
        except OSError:      # not stored yet, or another worker is storing this very texture right now
            pass
    import texture2ddecoder, etcpak
    n = len(data) // 16
    k = min(n, 256)
    rows = -(-n // k)
    # lay the blocks out as a k x rows block image; both codecs store blocks row-major, so order is preserved
    padded = bytes(data) + b'\0' * ((k * rows - n) * 16)
    bgra = texture2ddecoder.decode_astc(padded, k * 4, rows * 4, 4, 4)
    px = np.frombuffer(bgra, dtype=np.uint8).reshape(-1, 4)[:, [2, 1, 0, 3]]
    r = etcpak.compress_bc7(px.tobytes(), k * 4, rows * 4)[:n * 16]
    if len(r) != len(data):
        raise ValueError('BC7 size mismatch')
    if key:
        tmp = f'{key}.{os.getpid()}.tmp'
        with open(tmp, 'wb') as f:
            f.write(r)
        try:
            os.replace(tmp, key)
        except OSError:      # another worker stored the same texture first
            os.remove(tmp)
    return r


def astc_to_rgba(data, w, h, mips, images):
    """Decode a linear ASTC 4x4 mip chain (per image) to RGBA32."""
    import texture2ddecoder
    out = []; off = 0
    for _ in range(images):
        for i in range(mips):
            mw, mh = max(1, w >> i), max(1, h >> i)
            bw, bh = -(-mw // 4), -(-mh // 4)
            n = bw * bh * 16
            bgra = texture2ddecoder.decode_astc(bytes(data[off:off + n]), bw * 4, bh * 4, 4, 4)
            px = np.frombuffer(bgra, dtype=np.uint8).reshape(bh * 4, bw * 4, 4)[:mh, :mw][:, :, [2, 1, 0, 3]]
            out.append(px.tobytes()); off += n
    if off != len(data):
        raise ValueError(f'ASTC chain {off} != data {len(data)}')
    return b''.join(out)


def convert_file(src, dst, check=False):
    """-> (status, n_deswizzled, n_astc, errors). status: 'same' (nothing to do), 'ok', 'err'."""
    import UnityPy
    from UnityPy.files import SerializedFile, BundleFile
    from UnityPy.streams import EndianBinaryReader
    env = UnityPy.load(src)
    root = env.file
    res = {}      # resS name -> bytearray (modified copy)
    nd = na = 0
    errs = []

    def res_of(sf, path):
        name = path.replace('\\', '/').split('/')[-1]
        owner = sf.parent if sf.parent is not None and hasattr(sf.parent, 'files') else root
        if name not in owner.files:
            raise ValueError(f'resource {path} not in container')
        key = (id(owner), name)
        if key not in res:
            f = owner.files[name]
            res[key] = [owner, name, bytearray(f.bytes), f]
        return res[key][2]

    for o in env.objects:
        if o.type.name not in TYPES:
            continue
        try:
            t = o.read_typetree()
            fkey = 'm_TextureFormat' if 'm_TextureFormat' in t else 'm_Format'
            fmt = t[fkey]
            blob = bytes(t.get('m_PlatformBlob') or b'')
            if not blob and fmt != ASTC:
                continue
            if fmt != ASTC and o.type.name != 'Texture2D':
                raise ValueError(f'{o.type.name} with a platform blob')
            sd = t.get('m_StreamData') or {}
            streamed = bool(sd.get('path'))
            if streamed:
                buf = res_of(o.assets_file, sd['path'])
                data = bytes(buf[sd['offset']:sd['offset'] + sd['size']])
            else:
                data = bytes(t['image data'])
            if not data:
                continue
            if blob:
                if t.get('m_ImageCount', 1) != 1:
                    raise ValueError('swizzled texture with several images')
                data2 = deswizzle(data, fmt, t['m_Width'], t['m_Height'], t['m_MipCount'], blob)
                nd += 1
            else:
                data2 = data
            if fmt == ASTC:
                if not check:
                    if RGBA_NPOT and (t['m_Width'] & (t['m_Width'] - 1) or t['m_Height'] & (t['m_Height'] - 1)):
                        data2 = astc_to_rgba(data2, t['m_Width'], t['m_Height'], t['m_MipCount'], t.get('m_ImageCount', 1))
                        t[fkey] = 4
                    else:
                        data2 = astc_to_bc7(data2)
                        t[fkey] = BC7
                else:
                    t[fkey] = BC7
                na += 1
            if check:
                continue
            t['m_PlatformBlob'] = []
            if blob or len(data2) != len(data):
                t['m_CompleteImageSize'] = len(data2) // max(1, t.get('m_ImageCount', 1))
            if streamed:
                if len(data2) > len(data):       # does not fit in place: append to the resource file
                    buf.extend(b'\0' * (-len(buf) % 16))
                    sd['offset'] = len(buf)
                    buf.extend(data2)
                else:
                    buf[sd['offset']:sd['offset'] + len(data2)] = data2
                sd['size'] = len(data2)
            else:
                t['image data'] = data2
            o.save_typetree(t)
        except Exception as e:
            errs.append(f'{o.type.name} {o.path_id}: {type(e).__name__} {e}')
    if check or (nd == 0 and na == 0):
        return ('err' if errs else 'same', nd, na, errs)
    for owner, name, buf, old in res.values():
        r = EndianBinaryReader(bytes(buf), endian=old.endian)
        r.flags = getattr(old, 'flags', 0)
        owner.files[name] = r
    data = root.save(packer='lz4') if isinstance(root, BundleFile) else root.save()
    tmp = dst + '.tmp'
    os.makedirs(os.path.dirname(os.path.abspath(dst)), exist_ok=True)
    with open(tmp, 'wb') as f:
        f.write(data)
    if os.path.exists(dst):
        os.remove(dst)
    os.replace(tmp, dst)
    return ('err' if errs else 'ok', nd, na, errs)


def work(task):
    src, dst, check, cache = task
    global CACHE
    CACHE = cache
    try:
        if dst and os.path.exists(dst) and os.path.getmtime(dst) >= os.path.getmtime(src):
            return ('skip', src, 0, 0, [])
        with open(src, 'rb') as f:
            unity = f.read(7) == b'UnityFS'
        st, nd, na, errs = convert_file(src, dst, check) if unity else ('same', 0, 0, [])
        if st == 'same' and dst:
            os.makedirs(os.path.dirname(dst), exist_ok=True)
            if os.path.exists(dst):
                os.remove(dst)
            os.link(src, dst)
        return (st, src, nd, na, errs)
    except Exception:
        return ('err', src, 0, 0, [traceback.format_exc(limit=3).strip().splitlines()[-1]])


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('mode', choices=['tree', 'file', 'check'])
    ap.add_argument('src'); ap.add_argument('dst', nargs='?')
    ap.add_argument('--jobs', type=int, default=8); ap.add_argument('--cache'); ap.add_argument('--extra', action='append', default=[])
    a = ap.parse_args()
    if a.cache:
        os.makedirs(a.cache, exist_ok=True)
    if a.mode == 'file':
        tasks = [(a.src, a.dst, False, a.cache)]
    else:
        tasks = [(e, None, True, None) for e in a.extra]
        for r, _, fs in os.walk(a.src):
            for f in fs:
                s = os.path.join(r, f)
                tasks.append((s, os.path.join(a.dst, os.path.relpath(s, a.src)) if a.mode == 'tree' else None, a.mode == 'check', a.cache))
    stat = collections.Counter(); nd = na = 0; bad = []
    # big files first so the pool stays busy
    tasks.sort(key=lambda t: -os.path.getsize(t[0]))
    with mp.Pool(a.jobs) as pool:
        for i, (st, s, d, n, errs) in enumerate(pool.imap_unordered(work, tasks, chunksize=1 if len(tasks) < 64 else 4)):
            stat[st] += 1; nd += d; na += n
            for e in errs:
                bad.append((s, e)); print('ERR', s, e, flush=True)
            if i % 1000 == 999:
                print(i + 1, dict(stat), flush=True)
    print('done', dict(stat), 'deswizzled', nd, 'astc', na, 'errors', len(bad))
    if a.mode == 'tree':
        with open(os.path.join(a.dst, 'tex_convert_errors.tsv'), 'w', encoding='utf-8') as o:
            for s, e in bad:
                o.write(f'{s}\t{e}\n')
    sys.exit(1 if bad else 0)


if __name__ == '__main__':
    main()

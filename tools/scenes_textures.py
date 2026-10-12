# Textures of the scene streams (.x360.str) <-> PC .dds files (used by tools/scenes_extract.py, which builds
# the mods\...\X.x360.str\ folder with write_stream_folder; the runtime does the reverse in src/scene_assets.cpp).
# An edited .dds must keep the format, size and mipmap count of the original (the space in the stream is fixed).
#
# Where each texture is: in the .soi, records {name[0x100], ..., 360 D3D header with the 6-dword fetch constant
# (BaseFlush/MipFlush signature = FFFF0000 FFFF0000 at +0x124)}; in the .toc, {name[0x100], u32 stream record,
# u32 index, u32 index, u32 position in the record's decompressed part A, u32 size, u32 1}. Mipmaps live at
# mip_address (relative to the texture start); levels <= 16 texels are packed into one tile (Xenia's GetPackedMipOffset).
import argparse
import bisect
import os
import re
import struct
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import scenes_extract as se   # noqa: E402

# Xenos format -> (block width, bytes per block, DDS fourcc or None for RGB, RGB masks)
FORMATS = {
    18: (4, 8, b'DXT1'), 19: (4, 16, b'DXT3'), 20: (4, 16, b'DXT5'), 49: (4, 16, b'ATI2'), 59: (4, 8, b'ATI1'),
    51: (4, 8, b'DXT1'), 52: (4, 16, b'DXT3'), 53: (4, 16, b'DXT5'),
    6: (1, 4, None), 14: (1, 4, None), 50: (1, 4, None),
    4: (1, 2, '565'), 3: (1, 2, '1555'), 15: (1, 2, '4444'), 2: (1, 1, 'L8'), 10: (1, 2, 'A8L8'),
}


def log2_ceil(v):
    r = 0
    while (1 << r) < v:
        r += 1
    return r


def next_pow2(v):
    return 1 << log2_ceil(v)


def align(v, a):
    return (v + a - 1) // a * a


def tiled_offset(x, y, pitch, bpp_log2):
    # Xenia's texture_address.h (same as the runtime's TiledOffset2D); pitch in blocks, already aligned to 32
    outer = ((y >> 5) * (pitch >> 5) + (x >> 5)) << 6
    inner = (((y >> 1) & 7) << 3) | (x & 7)
    oib = (outer | inner) << bpp_log2
    bank = (y >> 4) & 1
    pipe = ((x >> 3) & 3) ^ (((y >> 3) & 1) << 1)
    return (((y & 1) << 4) | (pipe << 6) | (bank << 11) | (oib & 0xF) | (((oib >> 4) & 1) << 5) |
            (((oib >> 5) & 7) << 8) | ((oib >> 8) << 12))


def packed_mip_offset(w, h, mip, bw):
    lw, lh = log2_ceil(w), log2_ceil(h)
    ls = min(lw, lh)
    if ls > 4 + mip:
        return None
    base = ls - 4 if ls > 4 else 0
    pm = mip - base
    if pm < 3:
        x, y = (0, 16 >> pm) if lw > lh else (16 >> pm, 0)
    else:
        if lw > lh:
            x, y = (1 << (lw - base)) >> (pm - 2), 0
        else:
            x, y = 0, (1 << (lh - base)) >> (pm - 2)
    shift = 2 if bw == 4 else 0
    return x >> shift, y >> shift


class Fetch:
    def __init__(self, d):
        self.raw = list(d)
        self.tiled = d[0] >> 31
        self.pitch = ((d[0] >> 22) & 0x1FF) << 5
        self.format = d[1] & 0x3F
        self.endian = (d[1] >> 6) & 3
        self.width = (d[2] & 0x1FFF) + 1
        self.height = ((d[2] >> 13) & 0x1FFF) + 1
        self.swizzle = (d[3] >> 1) & 0xFFF
        self.max_level = (d[4] >> 6) & 0xF
        self.dimension = (d[5] >> 9) & 3
        self.packed = (d[5] >> 11) & 1
        self.mip_address = (d[5] >> 12) << 12


def levels(f, bw, bpb):
    # (level, width, height, storage offset, pitch in blocks, x/y in blocks, step between faces):
    # Xenia's GetGuestTextureLayout (2D and cube). Mips: power-of-two dimensions, aligned to 32x32 blocks (linear:
    # rows aligned to 256 bytes), in sequence starting at mip_address (each level with the 6 faces for cubes); from the
    # packed level on (smaller side <= 16 texels) all levels share one storage, each at an offset.
    w, h = f.width, f.height
    faces = 6 if f.dimension == 3 else 1
    max_level = min(f.max_level, max(w, h).bit_length() - 1)
    small = min(log2_ceil(w), log2_ceil(h))
    packed_level = (small - 4 if small > 4 else 0) if f.packed else 99
    lin_align = 32 if f.tiled else max(256 // bpb, 32)

    def stride(pitch, height):
        return align(pitch * bpb * align(align(height, bw) // bw, 32), 4096)

    pitch0 = align(align(f.pitch or w, bw) // bw, 32)
    ox, oy = packed_mip_offset(w, h, 0, bw) if packed_level == 0 else (0, 0)
    out = [(0, w, h, 0, pitch0, ox, oy, stride(pitch0, next_pow2(h) if packed_level == 0 else h))]
    if max_level == 0 or not f.mip_address:
        return out
    storage = {}
    if packed_level == 0:
        pitch = align(align(next_pow2(w), bw) // bw, lin_align)
        storage[0] = (0, pitch, stride(pitch, next_pow2(h)))
    else:
        mip_off = 0
        for lv in range(1, min(max_level, packed_level) + 1):
            pitch = align(align(max(next_pow2(w) >> lv, 1), bw) // bw, lin_align)
            st = stride(pitch, max(next_pow2(h) >> lv, 1))
            storage[lv] = (mip_off, pitch, st)
            mip_off += st * faces
    for lv in range(1, max_level + 1):
        if lv >= packed_level:
            off, pitch, st = storage[packed_level if packed_level else 0]
            x, y = packed_mip_offset(w, h, lv, bw)
        else:
            (off, pitch, st), x, y = storage[lv], 0, 0
        out.append((lv, max(w >> lv, 1), max(h >> lv, 1), f.mip_address + off, pitch, x, y, st))
    return out


def swap(data, endian):
    if endian == 1:
        b = bytearray(data)
        b[0::2], b[1::2] = data[1::2], data[0::2]
        return bytes(b)
    if endian == 2:
        b = bytearray(data)
        b[0::4], b[1::4], b[2::4], b[3::4] = data[3::4], data[2::4], data[1::4], data[0::4]
        return bytes(b)
    if endian == 3:
        b = bytearray(data)
        b[0::4], b[1::4], b[2::4], b[3::4] = data[2::4], data[3::4], data[0::4], data[1::4]
        return bytes(b)
    return data


def level_blocks(f, lv, bw, bpb, face=0):
    _, lw, lh, base, pitch, ox, oy, st = lv
    base += face * st
    bx, by = (lw + bw - 1) // bw, (lh + bw - 1) // bw
    bl = bpb.bit_length() - 1
    for y in range(by):
        if f.tiled:
            for x in range(bx):
                yield base + tiled_offset(x + ox, y + oy, pitch, bl)
        else:
            row = base + (y + oy) * pitch * bpb + ox * bpb
            for x in range(bx):
                yield row + x * bpb


def rgba_order(swizzle):
    # position (0-3) of each R, G, B, A channel byte after the endian swap; 4/5 = 0/1
    return [(swizzle >> (3 * i)) & 7 for i in range(4)]


def dds_header(f, fmt, n_levels, bw, bpb):
    fourcc = fmt[2]
    flags = 0x1 | 0x2 | 0x4 | 0x1000 | 0x20000 | (0x80000 if bw == 4 else 0x8)
    bx = (f.width + bw - 1) // bw
    pitch_or_size = bx * ((f.height + bw - 1) // bw) * bpb if bw == 4 else f.width * (4 if fourcc is None else bpb)
    if isinstance(fourcc, bytes):
        pf = struct.pack('<II4sIIIII', 32, 0x4, fourcc, 0, 0, 0, 0, 0)
    elif fourcc is None:
        pf = struct.pack('<II4sIIIII', 32, 0x41, b'\0\0\0\0', 32, 0xFF0000, 0xFF00, 0xFF, 0xFF000000)
    elif fourcc == '565':
        pf = struct.pack('<II4sIIIII', 32, 0x40, b'\0\0\0\0', 16, 0xF800, 0x7E0, 0x1F, 0)
    elif fourcc == '1555':
        pf = struct.pack('<II4sIIIII', 32, 0x41, b'\0\0\0\0', 16, 0x7C00, 0x3E0, 0x1F, 0x8000)
    elif fourcc == '4444':
        pf = struct.pack('<II4sIIIII', 32, 0x41, b'\0\0\0\0', 16, 0xF00, 0xF0, 0xF, 0xF000)
    elif fourcc == 'L8':
        pf = struct.pack('<II4sIIIII', 32, 0x20000, b'\0\0\0\0', 8, 0xFF, 0, 0, 0)
    else:   # A8L8
        pf = struct.pack('<II4sIIIII', 32, 0x20001, b'\0\0\0\0', 16, 0xFF, 0, 0, 0xFF00)
    cube = f.dimension == 3
    caps = 0x1000 | (0x400008 if n_levels > 1 else 0) | (0x8 if cube else 0)
    caps2 = 0xFE00 if cube else 0
    return (b'DDS ' + struct.pack('<7I', 124, flags, f.height, f.width, pitch_or_size, 0, n_levels) + b'\0' * 44 + pf +
            struct.pack('<5I', caps, caps2, 0, 0, 0))


def export_texture(f, fmt, data):
    bw, bpb = fmt[0], fmt[1]
    data = swap(data, f.endian)
    lvls = levels(f, bw, bpb)
    out = bytearray()
    order = rgba_order(f.swizzle) if fmt[2] is None else None
    for face, lv in ((fc, lv) for fc in range(6 if f.dimension == 3 else 1) for lv in lvls):
        for o in level_blocks(f, lv, bw, bpb, face):
            blk = data[o:o + bpb]
            if len(blk) < bpb:
                blk = blk.ljust(bpb, b'\0')
            if order:
                c = [blk[i] if i < 4 else (0 if i == 4 else 255) for i in order]
                blk = bytes((c[2], c[1], c[0], c[3]))   # DDS A8R8G8B8 = bytes B G R A
            out += blk
    return dds_header(f, fmt, len(lvls), bw, bpb) + bytes(out), len(lvls)


def import_texture(f, fmt, data, dds):
    bw, bpb = fmt[0], fmt[1]
    lvls = levels(f, bw, bpb)
    expected = dds_header(f, fmt, len(lvls), bw, bpb)
    blocks = sum(((lv[1] + bw - 1) // bw) * ((lv[2] + bw - 1) // bw) for lv in lvls) * (6 if f.dimension == 3 else 1)
    if len(dds) != 128 + blocks * bpb or dds[:128] != expected:
        raise ValueError('format/size/mipmaps differ from the original')
    buf = bytearray(swap(data, f.endian))
    order = rgba_order(f.swizzle) if fmt[2] is None else None
    p = 128
    for face, lv in ((fc, lv) for fc in range(6 if f.dimension == 3 else 1) for lv in lvls):
        for o in level_blocks(f, lv, bw, bpb, face):
            blk = dds[p:p + bpb]
            p += bpb
            if order:
                rgba = (blk[2], blk[1], blk[0], blk[3])
                cur = bytearray(buf[o:o + 4])
                for ch, src in enumerate(order):
                    if src < 4:
                        cur[src] = rgba[ch]
                blk = bytes(cur)
            if o + bpb <= len(buf):
                buf[o:o + bpb] = blk
    return swap(bytes(buf), f.endian)


def name_starts(buf):
    # resource names in the .toc/.soi: "C\..." or "c\..." (CT_SM uses lowercase)
    return [m.start() for m in re.finditer(rb'[Cc]\\', buf)]


def scene_textures(res_dir, raw_size, no_records=False):
    # [(name, Fetch, [(position in the raw .str, size), ...])]: the texture list of each stream record comes right
    # after it in the .toc (in HUB, one per zone; the same texture can be in several zones). no_records: stream without
    # records (MiniMap, already raw on disk) -> positions directly in the file
    names = os.listdir(res_dir)
    soi_name = [n for n in names if n.lower().endswith('.soi')]
    toc_name = [n for n in names if n.lower().endswith('.toc')]
    if not soi_name or not toc_name:
        return []
    soi = open(os.path.join(res_dir, soi_name[0]), 'rb').read()
    toc = open(os.path.join(res_dir, toc_name[0]), 'rb').read()
    recs = sorted(se.find_records(toc, raw_size), key=lambda r: r[0])
    if not recs and not no_records:
        return []
    sections = not recs   # no records: position of each piece in its own "Section_N" (below)
    if not recs:
        recs = [(0, 0, raw_size, 0, 1, 0, (raw_size,))]   # one part A the size of the file
    starts = [r[0] for r in recs]
    places = {}
    for i in name_starts(toc):
        if i + 0x118 <= len(toc):
            name = toc[i:i + 0x100].split(b'\0')[0]
            fields = struct.unpack_from('>6I', toc, i + 0x100)
            if sections and i >= 16:
                # MiniMap: each piece has its own "Section_N" ending in {position, total, total, 0} right before
                # the name; the texture position (fields[3]) is relative to that section
                off, tot, da = struct.unpack_from(">3I", toc, i - 16)
                if tot and tot == da and off % 0x800 == 0 and off + tot <= raw_size and fields[3] + fields[4] <= tot:
                    fields = fields[:3] + (off + fields[3],) + fields[4:]
            k = bisect.bisect_right(starts, i) - 1
            if len(name) > 4 and fields[5] == 1 and fields[4] and fields[3] % 0x1000 == 0 and k >= 0 and \
                    fields[3] + fields[4] <= recs[k][2]:
                places.setdefault(name.lower(), []).append((recs[k][1] + fields[3], fields[4]))
    out = []
    seen = set()
    sig = bytes.fromhex('ffff0000ffff0000')
    i = soi.find(sig)
    while i >= 0:
        n = i - 0x124
        if n >= 0:
            name = soi[n:n + 0x100].split(b'\0')[0]
            if name[:2].lower() == b'c\\' and name.lower() not in seen and name.lower() in places:
                seen.add(name.lower())
                out.append((name.decode('latin-1'), Fetch(struct.unpack_from('>6I', soi, i + 8)), places[name.lower()]))
        i = soi.find(sig, i + 1)
    return out


def write_stream_folder(raw, res_dir, folder, merge_gap=0x4000):
    # Stream folder (mods\...\X.x360.str\), read by the runtime (scene_assets.cpp) instead of the .str:
    #   <texture name>.dds     each texture of the stream (all copies in the stream get the same .dds)
    #   _data\XXXXXXXX.bin     the rest of the stream (geometry and tile padding), at position XXXXXXXX
    #   _index.txt             stream size, textures (fetch constant, size, positions) and data
    # The runtime zeroes the stream, copies the _data and writes the blocks of each .dds (with Xbox tiling/endian) on top.
    texs = scene_textures(res_dir, len(raw))
    rest = bytearray(raw)
    lines = ['# Scene stream built by the runtime from this folder (tools/scenes_extract.py).',
             '# Edit the .dds files keeping format, size and mipmap count (see the columns). Do not edit this index.',
             'size %X' % len(raw),
             '# texture <fetch constant: 6 dwords> <size> <positions in the stream> <file>  | format width x height mipmaps']
    written = 0
    for name, f, places in texs:
        fmt = FORMATS.get(f.format)
        pos, size = places[0]
        data = raw[pos:pos + size]
        if not fmt or f.dimension not in (1, 3) or any(raw[q:q + sz] != data for q, sz in places):
            continue
        bw, bpb = fmt[0], fmt[1]
        dds, n = export_texture(f, fmt, data)
        rel = name.replace('/', '\\') + '.dds'
        dst = os.path.join(folder, *rel.split('\\'))
        os.makedirs(os.path.dirname(dst), exist_ok=True)
        with open(dst, 'wb') as fp:
            fp.write(dds)
        faces = 6 if f.dimension == 3 else 1
        lv = levels(f, bw, bpb)
        for q, _ in places:
            for face in range(faces):
                for level in lv:
                    for o in level_blocks(f, level, bw, bpb, face):
                        if o + bpb <= size:
                            rest[q + o:q + o + bpb] = bytes(bpb)
        lines.append('texture %s %X %s %s  | fmt %d %dx%d mips %d%s' % (
            ' '.join('%08X' % v for v in f.raw), size, ','.join('%X' % q for q, _ in places), rel, f.format, f.width,
            f.height, n, ' cube' if faces == 6 else ''))
        written += 1
    # non-zero runs of the rest (merging small holes)
    runs = []
    i = 0
    n = len(rest)
    zero = bytes(4096)
    while i < n:
        if rest[i:i + 4096] == zero[:min(4096, n - i)]:
            i += 4096
            continue
        start = i
        gap = 0
        while i < n and gap < merge_gap:
            chunk = rest[i:i + 4096]
            if chunk == zero[:len(chunk)]:
                gap += len(chunk)
            else:
                gap = 0
            i += len(chunk)
        end = i - gap
        runs.append((start, end))
    os.makedirs(os.path.join(folder, '_data'), exist_ok=True)
    for start, end in runs:
        fn = '_data\\%08X.bin' % start
        with open(os.path.join(folder, '_data', '%08X.bin' % start), 'wb') as fp:
            fp.write(rest[start:end])
        lines.append('data %X %s' % (start, fn))
    with open(os.path.join(folder, '_index.txt'), 'w', encoding='latin-1', newline='\r\n') as fp:
        fp.write('\n'.join(lines) + '\n')
    return written, len(runs)


def index_file(folder):
    # _index.txt; _indice.txt in folders extracted by older versions of this tool
    new = os.path.join(folder, '_index.txt')
    return new if os.path.exists(new) else os.path.join(folder, '_indice.txt')


def old_keywords(line):
    # Portuguese keywords of older indexes (tamanho/textura/dados)
    for old, new in (('tamanho ', 'size '), ('textura ', 'texture '), ('dados ', 'data ')):
        if line.startswith(old):
            return new + line[len(old):]
    return line


def build_from_folder(folder):
    # The same as the runtime does (scene_assets.cpp): used to check the folder right after extracting
    size = 0
    texs = []
    datas = []
    for line in open(index_file(folder), encoding='latin-1'):
        line = old_keywords(line.rstrip('\r\n'))
        if line.startswith('size '):
            size = int(line.split()[1], 16)
        elif line.startswith('texture '):
            parts = line.split('  |')[0].split(' ', 9)
            fetch = [int(x, 16) for x in parts[1:7]]
            texs.append((Fetch(fetch), int(parts[7], 16), [int(x, 16) for x in parts[8].split(',')], parts[9]))
        elif line.startswith('data '):
            _, pos, fn = line.split(' ', 2)
            datas.append((int(pos, 16), fn))
    buf = bytearray(size)
    for pos, fn in datas:
        d = open(os.path.join(folder, *fn.split('\\')), 'rb').read()
        buf[pos:pos + len(d)] = d
    for f, tsize, places, rel in texs:
        fmt = FORMATS[f.format]
        dds = open(os.path.join(folder, *rel.split('\\')), 'rb').read()
        new = import_texture(f, fmt, bytes(buf[places[0]:places[0] + tsize]), dds)
        for q in places:
            buf[q:q + tsize] = new
    return bytes(buf)


if __name__ == '__main__':
    print('Use tools/scenes_extract.py (this file is the texture part).')

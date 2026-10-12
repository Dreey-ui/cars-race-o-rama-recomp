# Unpacks the scene packages (C\Scenes\**\*.x360.res + .x360.str) into folders in mods, which the runtime reads:
#   mods\C\Scenes\X\X.x360.res\   each file inside the .res, decompressed (including the ZLIBME ones) + _index.txt
#   mods\C\Scenes\X\X.x360.str\   each texture of the stream as .dds + the rest (geometry) in _data\ + _index.txt
#                                  (tools/scenes_textures.py); verified: the folder rebuilds the stream byte for byte
# The .toc inside the .res holds the block size table of the .str: it is rewritten for the raw stream.
# The runtime (kernel_io.cpp) builds an uncompressed .res from the folder when the game opens the file and delivers the raw
# .str blocks bypassing zlib (game_hooks.cpp, uncompress sub_822702F8). The original files are not changed.
# (Superseded for modding by tools/scenes_decompile.py + loose scenes; scenes_decompile.py reads these folders.)
# Usage: python -I tools/scenes_extract.py [--game "C:\Games\Cars Race-O-Rama Recomp"] [--mods <folder>] [--only FE,CR_01]
#
# .res format (v3, reader in sub_821C3580 / sub_821C1AA0), little-endian values:
#   +0 version (2|3), +4 header size after +8, [v3: +8 flags, +12 0], [flags & 0x40: u32 n + n x u16 compressed size
#   of each 0x6000 block (size 0x6000 = block stored uncompressed)], directory: u32 count,
#   u32 directory size, entries {u32 name length, name, u32 offset, u32 size}; data right after.
#   flags: 0x100 zlib blocks, 0x40 block table, 0x200 sizes-1, 0x80/0x400 XOR cipher (not used by the scenes).
# .str format: stream records (one per scene or one per zone in HUB) with independent zlib blocks of 128 KB
#   decompressed (the last one of each part can be smaller), at positions that are multiples of 0x800. Each record lives in the .toc
#   (see find_records). The MiniMap .str files have no records: they are already raw on disk and stay as they are.
import argparse
import os
import struct
import sys
import zlib

CHUNK = 0x6000
BLOCK = 0x20000


def read_res(path):
    data = open(path, 'rb').read()
    ver, hdr = struct.unpack_from('<II', data, 0)
    pos = 8
    flags = 0
    sizes = []
    if ver != 2:
        flags, _ = struct.unpack_from('<II', data, 8)
        pos = 16
    if flags & 0x40:
        n = struct.unpack_from('<I', data, pos)[0]
        sizes = list(struct.unpack_from('<%dH' % n, data, pos + 4))
    if flags & 0x480:
        raise ValueError('encrypted package (flags %X): not supported' % flags)
    pos = 8 + hdr
    count, dir_size = struct.unpack_from('<II', data, pos)
    pos += 8
    data_start = pos + dir_size
    entries = []
    for _ in range(count):
        n = struct.unpack_from('<I', data, pos)[0]
        name = data[pos + 4:pos + 4 + n].decode('latin-1')
        off, size = struct.unpack_from('<II', data, pos + 4 + n)
        entries.append((name, off, size))
        pos += 12 + n
    if flags & 0x100:
        blob = bytearray()
        p = data_start
        for s in sizes:
            if flags & 0x200:
                s += 1
            if s == 0:
                break
            if s == CHUNK:
                blob += data[p:p + CHUNK]
            else:
                out = zlib.decompress(data[p:p + s])
                blob += out.ljust(CHUNK, b'\0')
            p += s
    else:
        blob = data[data_start:]
    files = []
    for name, off, size in entries:
        files.append((name, bytes(blob[off:off + size])))
    return ver, flags, files


def unzlibme(content):
    if content[:6] == b'ZLIBME' and len(content) >= 16:
        comp, raw = struct.unpack_from('>II', content, 8)
        out = zlib.decompress(content[16:16 + comp])
        if len(out) == raw:
            return out, True
    return content, False


def find_records(toc, size):
    # Stream record in the .toc (big-endian): position in the .str, total, decompressed A, B, compressed A, B, blocks A, B,
    # table (size of each block in the .str). The reader (sub_820B0398) reads each block and has it decompressed into 128 KB.
    recs = []
    for i in range(4, len(toc) - 28):
        off, tot, da, db, ca, cb, na, nb = struct.unpack_from('>8I', toc, i - 4)
        if not tot or tot != da + db or not (na or nb) or na >= 4096 or nb >= 4096 or off % 0x800 or off + ca + cb > size:
            continue
        # a part can be empty (0 blocks and 0 bytes); the reader only skips decompression if both are empty
        if not ((da == 0 if na == 0 else (na - 1) * BLOCK < da <= na * BLOCK) and
                (db == 0 if nb == 0 else (nb - 1) * BLOCK < db <= nb * BLOCK)):
            continue
        if i + 28 + 4 * (na + nb) > len(toc):
            continue
        tab = struct.unpack_from('>%dI' % (na + nb), toc, i + 28)
        if sum(tab[:na]) == ca and sum(tab[na:]) == cb:
            recs.append((i, off, da, db, na, nb, tab))
    recs.sort(key=lambda r: r[1])
    pos = 0
    for r in recs:
        if r[1] != pos:
            raise ValueError('.toc records do not cover the .str (hole/overlap at %X)' % pos)
        pos = r[1] + sum(r[6])
    if recs and pos != size:
        raise ValueError('.toc records end at %X, the .str has %X' % (pos, size))
    return recs


def convert_str(toc, data):
    # Raw stream: each block decompressed, aligned to 0x1000 (the game's file layer reads straight into the buffer), with
    # the position/sizes/table of each record rewritten in the .toc (the .toc size does not change)
    recs = find_records(toc, len(data))
    if not recs:
        return None, None
    new = bytearray(toc)
    out = bytearray()
    for i, off, da, db, na, nb, tab in recs:
        new_off = len(out)
        pos = off
        slots = []
        for first, n, dsize in ((0, na, da), (na, nb, db)):
            remaining = dsize
            for k in range(n):
                slot = tab[first + k]
                d = zlib.decompressobj()
                block = d.decompress(data[pos:pos + slot])
                expected = min(remaining, BLOCK)
                # the last block of a part can be up to 0x1000 smaller than declared (the game does not touch the rest of the destination)
                if not d.eof or not (expected - 0x1000 < len(block) <= expected):
                    raise ValueError('block at %X: %X bytes (expected %X)' % (pos, len(block), expected))
                remaining -= expected
                padded = (expected + 0xFFF) & ~0xFFF
                out += block + b'\0' * (padded - len(block))
                slots.append(padded)
                pos += slot
        struct.pack_into('>I', new, i - 4, new_off)
        struct.pack_into('>II', new, i + 12, sum(slots[:na]), sum(slots[na:]))
        struct.pack_into('>%dI' % len(slots), new, i + 28, *slots)
    return bytes(new), bytes(out)


def extract(game, mods, rel_res):
    src_res = os.path.join(game, rel_res)
    src_str = src_res[:-4] + '.str'
    out_dir = os.path.join(mods, rel_res)
    ver, flags, files = read_res(src_res)
    toc_index = [i for i, (n, _) in enumerate(files) if n.lower().endswith('.toc')]
    raw_str = None
    if os.path.exists(src_str) and toc_index:
        i = toc_index[0]
        new_toc, raw = convert_str(files[i][1], open(src_str, 'rb').read())
        if raw is not None:   # no records (MiniMap): the .str is already raw on disk, the original stays
            files[i] = (files[i][0], new_toc)
            raw_str = raw
    os.makedirs(out_dir, exist_ok=True)
    lines = ['# package extracted from %s (version %d, flags %X). Order of the files in the .res built by the runtime;' % (rel_res, ver, flags),
             '# new files in the folder go at the end. "zlibme" = was compressed with ZLIBME (the game accepts it uncompressed).']
    unz = 0
    for name, content in files:
        content, was = unzlibme(content)
        unz += was
        dst = os.path.join(out_dir, *name.replace('/', '\\').split('\\'))
        os.makedirs(os.path.dirname(dst), exist_ok=True)
        with open(dst, 'wb') as f:
            f.write(content)
        lines.append(name + (' zlibme' if was else ''))
    with open(os.path.join(out_dir, '_index.txt'), 'w', encoding='latin-1', newline='\r\n') as f:
        f.write('\n'.join(lines) + '\n')
    msg = '%s: %d files (%d ZLIBME decompressed)' % (rel_res, len(files), unz)
    if raw_str is not None:
        sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
        import scenes_textures as st
        str_dir = os.path.join(mods, rel_res[:-4] + '.str')
        if os.path.isfile(str_dir):
            os.remove(str_dir)   # raw .str from an older version of this tool
        n_tex, n_dat = st.write_stream_folder(raw_str, out_dir, str_dir)
        if st.build_from_folder(str_dir) != raw_str:
            raise ValueError('the folder %s does not rebuild the same stream' % str_dir)
        msg += ' | .str %.1f MB -> folder with %d .dds + %d data blocks (%.1f MB)' % (
            os.path.getsize(src_str) / 1048576.0, n_tex, n_dat, len(raw_str) / 1048576.0)
    print(msg)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--game', default=r'C:\Games\Cars Race-O-Rama Recomp')
    ap.add_argument('--mods', default=None)
    ap.add_argument('--only', default='', help='comma-separated list of scenes (folders of C\\Scenes)')
    a = ap.parse_args()
    mods = a.mods or os.path.join(a.game, 'mods')
    only = [s.strip().lower() for s in a.only.split(',') if s.strip()]
    scenes = os.path.join(a.game, 'C', 'Scenes')
    for root, _, names in os.walk(scenes):
        for n in sorted(names):
            if not n.lower().endswith('.x360.res'):
                continue
            rel = os.path.relpath(os.path.join(root, n), a.game)
            scene = os.path.relpath(root, scenes).split(os.sep)[0].lower()
            if only and scene not in only:
                continue
            try:
                extract(a.game, mods, rel)
            except Exception as e:
                print('%s: ERROR %s' % (rel, e))
                sys.exit(1)


if __name__ == '__main__':
    main()

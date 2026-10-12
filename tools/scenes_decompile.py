# Decompiles the game's scene packages (C\Scenes\**\*.x360.res + *.x360.str) into loose files, in the same folder
# structure as the source scenes (like a development C\Scenes):
#   <out>\<Scene>\geo\*.xng   models (the .soi "xgs" header + the stream data; identical to the source .xng)
#   <out>\<Scene>\tex\*.dds   textures (PC DDS, untiled, all mipmaps)    + *.xnm text materials
#   <out>\<Scene>\col\*.gol   collisions (.soi header + data from part B of the stream, packed like the .gol)
#   <out>\<Scene>\...         everything that was already loose in the game (Eco, Anim, Skies, .scn, .toy, .aud...)
#   <out>\<Scene>\_package\   original .soi/.toc (scene index: placement, zones) + X.files.txt (CRC32 of each loose file)
# Usage: python -I tools/scenes_decompile.py [--game <folder>] [--out <folder>] [--only FE,HUB] [--compare <loose scenes>]
# Reads the folders extracted by tools/scenes_extract.py (--source; default <game>\_old_mod_folders or <game>\mods) and
# builds each stream with "CarsRecomp.exe --build-folder" (the same code the game uses).
import argparse
import bisect
import collections
import os
import shutil
import struct
import subprocess
import sys
import tempfile
import zlib

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import scenes_extract as se       # noqa: E402
import scenes_textures as st      # noqa: E402

STREAM_BITS = [(0x01, 12), (0x02, 12), (0x08, 4), (0x04, 8), (0x10, 8), (0x4000, 8), (0x20, 12)]


def vstride(flags):
    return sum(n for b, n in STREAM_BITS if flags & b)


def xgs_to_xng(soi, x, blob):
    """xgs header at soi[x:] + stream data block -> .xng (each submesh's data right after its record)"""
    out = bytearray(b'xng\0')
    count = struct.unpack_from('>I', soi, x + 8)[0]
    out += soi[x + 4:x + 12]
    p, q = x + 12, 0
    for _ in range(count):
        nmat = struct.unpack_from('>I', soi, p + 0xE0)[0]
        head = 0xE4 + nmat * 0x40
        out += soi[p:p + head + 4]
        nlod = soi[p + head]
        p += head + 4
        for _ in range(nlod):
            out += soi[p:p + 8]
            nsub = struct.unpack_from('>I', soi, p + 4)[0]
            p += 8
            for _ in range(nsub):
                flags = struct.unpack_from('>I', soi, p + 4)[0]
                nf = soi[p + 8]
                rec = bytearray(soi[p:p + 18 + 4 * nf])
                nv, ni = struct.unpack_from('>HH', rec, 14 + 4 * nf)
                rec[10] = 0   # data in the file itself (1 = in the stream)
                out += rec
                p += 18 + 4 * nf
                size = ((ni * 2 + 3) & ~3) + nv * vstride(flags)
                out += blob[q:q + size]
                q += size
    return bytes(out), q


def col_to_gol(header, blob):
    """Collision header in the .soi (0x58 bytes) + stream data -> .gol (float3 vertices, 32-byte nodes, 22-byte triangles)"""
    h = bytearray(header[:0x58])
    struct.pack_into('>I', h, 0x0C, 0)   # 9 = streamed
    nv, nn, nt = struct.unpack_from('>III', h, 0x48)
    out = bytearray(h)
    p = 0
    for i in range(nv):
        out += blob[p:p + 12]
        p += 16
    p = (p + 31) & ~31   # in the stream the nodes start aligned to 32 bytes
    out += b'\0\0\0\0'
    for i in range(nn):
        n = blob[p:p + 64]
        out += n[36:40] + n[0:12] + n[16:28] + n[32:36]
        p += 64
    for i in range(nt):
        t = blob[p:p + 32]
        out += t[16:20] + t[0:12] + t[20:26]
        p += 32
    return bytes(out), p


def xgs_len(soi, x):
    """Size of the xgs header at soi[x:] (same walk as xgs_to_xng, without the data)"""
    count = struct.unpack_from('>I', soi, x + 8)[0]
    p = x + 12
    for _ in range(count):
        nmat = struct.unpack_from('>I', soi, p + 0xE0)[0]
        nlod = soi[p + 0xE4 + nmat * 0x40]
        p += 0xE4 + nmat * 0x40 + 4
        for _ in range(nlod):
            nsub = struct.unpack_from('>I', soi, p + 4)[0]
            p += 8
            for _ in range(nsub):
                p += 18 + 4 * soi[p + 8]
    return p - x


def soi_records(soi):
    """.soi records: [(type 0 model/1 texture/2 collision, name, content position, content size)].
    Header: +0xC collisions, +0x10 models, +0x18 stream textures, +0x1C .soi-only textures (PC DDS, right after
    the others), +0x28/+0x30/+0x34 position (relative to 0x3C) of the 1st record of models/collisions/textures. Record:
    0x3C of placement before it, u32 index, name[0x100], u32, u32, u32 n, n x 0x208 properties, content."""
    h = struct.unpack_from('>15I', soi, 0)
    out = []
    for kind, n, off in ((2, h[3], h[12]), (0, h[4], h[10]), (1, h[6] + h[7], h[13])):
        p = 0x3C + off
        for _ in range(n):
            name = soi[p + 4:p + 0x104].split(b'\0')[0].decode('latin-1')
            q = p + 0x110 + struct.unpack_from('>I', soi, p + 0x10C)[0] * 0x208
            if kind == 1:
                size = 0x38 if soi[q + 0x18:q + 0x20] == bytes.fromhex('ffff0000ffff0000') else 4 + struct.unpack_from('>I', soi, q)[0]
            elif kind == 2:
                size = 0x7C
            else:
                size = xgs_len(soi, q)
            out.append((kind, name, q, size))
            p = q + size + 0x3C
    return out


def resources(toc):
    out = []
    for i in st.name_starts(toc):
        if i + 0x118 <= len(toc):
            name = toc[i:i + 0x100].split(b'\0')[0].decode('latin-1')
            f = struct.unpack_from('>6I', toc, i + 0x100)
            if len(name) > 4 and f[5] in (0, 1, 2) and (f[5] == 1 or f[1] == 0xFFFFFFFF):
                out.append((i, name, f))
    return out


def norm(name):
    # the .toc sometimes has spaces that the xgs internal name does not have ("wood catwalk01" / "woodcatwalk01")
    return name.lower().replace(' ', '')


# files already written for the current scene: the same resource in two packages of the same folder (CR_05 and
# CR_05_Landmark, HUB and HUB_Landmark) becomes a single file; the first package's version (the main one) wins, sometimes
# with slightly different mipmaps
WRITTEN = set()


def first_write(dst, stats):
    key = os.path.normcase(os.path.abspath(dst))
    if key in WRITTEN:
        stats['repeated in another package of the scene (first kept)'] += 1
        return False
    WRITTEN.add(key)
    return True


def out_path(out_root, scene_rel, pack_rel_dir, name, ext):
    # "C\Scenes\<Scene>\geo\X" -> <out>\<Scene>\geo\X; others (C\ShadowMap\Tex\X...) -> package folder\last 2 parts
    parts = name.replace('/', '\\').split('\\')
    if len(parts) > 3 and parts[0].lower() == 'c' and parts[1].lower() == 'scenes':
        rel = os.path.join(*parts[2:])
    else:
        rel = os.path.join(pack_rel_dir, *parts[-2:])
    return os.path.join(out_root, rel + ext)


def build_stream(exe, folder, tmpdir):
    out = os.path.join(tmpdir, 'stream.bin')
    subprocess.run([exe, '--build-folder', folder, out], check=True, capture_output=True)
    return open(out, 'rb').read()


def decompile_pack(game, mods, out_root, exe, res_rel, stats, cmp_root):
    res_dir = os.path.join(mods, res_rel)
    pack_dir = os.path.dirname(res_rel)                      # C\Scenes\CR_01\ShadowMap
    pack_rel_dir = os.path.relpath(pack_dir, os.path.join('C', 'Scenes'))   # CR_01\ShadowMap
    names = os.listdir(res_dir)
    soi_n = [n for n in names if n.lower().endswith('.soi')]
    toc_n = [n for n in names if n.lower().endswith('.toc')]
    if not soi_n or not toc_n:
        return
    soi = open(os.path.join(res_dir, soi_n[0]), 'rb').read()
    toc = open(os.path.join(res_dir, toc_n[0]), 'rb').read()
    str_folder = res_dir[:-4] + '.str'
    with tempfile.TemporaryDirectory() as tmp:
        if os.path.isdir(str_folder):
            raw = build_stream(exe, str_folder, tmp)
        else:
            orig = os.path.join(game, res_rel[:-4] + '.str')
            raw = open(orig, 'rb').read() if os.path.exists(orig) else b''
    recs = sorted(se.find_records(toc, len(raw)), key=lambda r: r[0]) if raw else []
    starts = [r[0] for r in recs]

    def section(toc_pos, kind):
        if not recs:
            # stream without records (MiniMap): each piece has its own "Section_N" ending in {position, total, total, 0}
            # right before the name
            if toc_pos < 16:
                return 0
            off, tot, da = struct.unpack_from('>3I', toc, toc_pos - 16)
            return off if tot and tot == da and off % 0x800 == 0 and off + tot <= len(raw) else 0
        r = recs[bisect.bisect_right(starts, toc_pos) - 1]
        off, na, tab = r[1], r[4], r[6]
        return off if kind != 2 else off + sum(tab[:na])

    pack_out = os.path.join(out_root, pack_rel_dir, '_package')
    os.makedirs(pack_out, exist_ok=True)
    for n in soi_n + toc_n:
        shutil.copyfile(os.path.join(res_dir, n), os.path.join(pack_out, n))
    res = resources(toc)
    # models: the .soi xgs by its internal name
    xgs = {}
    x = soi.find(b'xgs\0')
    while x >= 0:
        if soi[x + 0xE8:x + 0xEC] == b'\xff\xff\xff\xff':
            xgs.setdefault(norm(soi[x + 12:x + 12 + 0x80].split(b'\0')[0].decode('latin-1')), x)
        x = soi.find(b'xgs\0', x + 4)
    # collisions: header 00 00 04 D2 'col' preceded by the record name in the .soi
    cols = {}
    c = soi.find(b'\x00\x00\x04\xd2col\x00')
    while c >= 0:
        j = max(soi.rfind(b'C\\', max(0, c - 0x400), c), soi.rfind(b'c\\', max(0, c - 0x400), c))
        if j >= 0:
            cols.setdefault(norm(soi[j:j + 0x100].split(b'\0')[0].decode('latin-1')), c)
        c = soi.find(b'\x00\x00\x04\xd2col\x00', c + 8)
    done = set()
    for toc_pos, name, f in res:
        key = name.lower()
        if (key, f[5]) in done:
            continue   # the same thing in another HUB zone
        base = section(toc_pos, f[5])
        blob = raw[base + f[3]:base + f[3] + f[4]]
        if f[5] == 0:
            x = xgs.get(norm(key.rsplit('\\', 1)[-1]))
            if x is None:
                stats['model without header'] += 1
                continue
            data, used = xgs_to_xng(soi, x, blob)
            dst = out_path(out_root, None, pack_rel_dir, name, '.xng')
            stats['models'] += 1
            stats['model with incomplete block'] += used != len(blob)
        elif f[5] == 2:
            c = cols.get(norm(key))
            if c is None:
                stats['collision without header'] += 1
                continue
            data, used = col_to_gol(soi[c:c + 0x58], blob)
            dst = out_path(out_root, None, pack_rel_dir, name, '.gol')
            stats['collisions'] += 1
            stats['collision with incomplete block'] += used != len(blob)
        elif f[5] == 1 and blob[:4] == b'DDS ':
            # texture already in the stream as a whole PC DDS (CR_01 MiniMap); the Xenos ones come out below
            data = blob
            dst = out_path(out_root, None, pack_rel_dir, name, '.dds')
            stats['textures (PC DDS in the stream)'] += 1
        else:
            continue
        done.add((key, f[5]))
        if not first_write(dst, stats):
            continue
        os.makedirs(os.path.dirname(dst), exist_ok=True)
        with open(dst, 'wb') as fp:
            fp.write(data)
        compare(cmp_root, dst, out_root, data, stats)
    # textures
    if raw:
        for name, fetch, places in st.scene_textures(res_dir, len(raw), no_records=True):
            fmt = st.FORMATS.get(fetch.format)
            pos, size = places[0]
            if not fmt or fetch.dimension not in (1, 3):
                stats['unsupported texture'] += 1
                continue
            dst = out_path(out_root, None, pack_rel_dir, name, '.dds')
            if not first_write(dst, stats):
                continue
            dds, _ = st.export_texture(fetch, fmt, raw[pos:pos + size])
            os.makedirs(os.path.dirname(dst), exist_ok=True)
            with open(dst, 'wb') as fp:
                fp.write(dds)
            stats['textures'] += 1
            compare(cmp_root, dst, out_root, dds, stats, skip_header=True)
    # textures that only exist in the .soi, as PC DDS (86 in HUB, outside the stream): the whole .dds comes out
    for kind, name, q, size in soi_records(soi):
        if kind != 1 or size == 0x38 or soi[q + 4:q + 8] != b'DDS ' or (name.lower(), 1) in done:
            continue
        done.add((name.lower(), 1))
        dst = out_path(out_root, None, pack_rel_dir, name, '.dds')
        if not first_write(dst, stats):
            continue
        data = soi[q + 4:q + size]
        os.makedirs(os.path.dirname(dst), exist_ok=True)
        with open(dst, 'wb') as fp:
            fp.write(data)
        stats['textures (PC DDS in the .soi)'] += 1
        compare(cmp_root, dst, out_root, data, stats)
    # materials (.xnm.bpb from the .res) -> text .xnm
    for line in open(st.index_file(res_dir), encoding='latin-1'):
        n = line.strip().replace(' zlibme', '')
        if line.startswith('#') or not n.lower().endswith('.xnm.bpb'):
            continue
        src = os.path.join(res_dir, *n.split('\\'))
        data = open(src, 'rb').read()
        try:
            txt = bpb_to_ini(data).encode('latin-1')
            dst = out_path(out_root, None, pack_rel_dir, n[:-4], '')
            stats['materials'] += 1
        except Exception:   # noqa: BLE001
            txt = data   # stays .xnm.bpb at the game path: the game reads the binary straight from the folder
            dst = out_path(out_root, None, pack_rel_dir, n, '')
            stats['material kept as .bpb'] += 1
        os.makedirs(os.path.dirname(dst), exist_ok=True)
        with open(dst, 'wb') as fp:
            fp.write(txt)
    # CRC32 of each loose file of the package as extracted: the runtime compares and logs "(modified)" for what changed
    ext = {0: '.xng', 1: '.dds', 2: '.gol'}
    paths = {out_path(out_root, None, pack_rel_dir, name, ext[f[5]]) for _, name, f in res if f[5] in ext}
    paths |= {out_path(out_root, None, pack_rel_dir, name, '.dds') for kind, name, q, size in soi_records(soi)
              if kind == 1 and size != 0x38}
    lines = []
    for path in sorted(paths):
        if os.path.exists(path):
            lines.append('%08X %s' % (zlib.crc32(open(path, 'rb').read()) & 0xFFFFFFFF, os.path.relpath(path, out_root)))
    stem = soi_n[0][:-4]
    with open(os.path.join(pack_out, stem + '.files.txt'), 'w', encoding='latin-1', newline='\r\n') as fp:
        fp.write('# CRC32 of this package\'s loose files as extracted (the runtime logs what changed)\n')
        fp.write('\n'.join(lines) + '\n')


def fmt_float(v):
    return '%.1f' % v if v == int(v) and abs(v) < 1e7 else '%.6g' % v


def bpb_to_ini(d):
    """The engine's binary INI ("BPB\\0" / "BPBh"): records {hash, name|type, suffix, n/value} + name and value tables"""
    if d[:3] != b'BPB':
        raise ValueError('not a BPB')
    _, _, nsec, nkey, names_off, names_size, vals_off, vals_size = struct.unpack_from('>IIHHIIII', d, 4)
    names, vals = d[names_off:names_off + names_size], d[vals_off:vals_off + vals_size]
    cs = lambda pool, o: pool[o:pool.index(b'\0', o)].decode('latin-1')   # noqa: E731
    recs = [struct.unpack_from('>IIHHI', d, 0x20 + 16 * i) for i in range(nsec + nkey)]
    secs, keys = recs[:nsec], recs[nsec:]
    out = []
    for _, noff, suf, count, first in secs:
        out.append('[%s%s]' % (cs(names, noff & 0x3FFFFFFF), '' if suf == 0xFFFF else suf))
        for _, koff, ksuf, _, val in keys[first:first + count]:
            kind = koff >> 30
            kname = cs(names, koff & 0x3FFFFFFF) + ('' if ksuf == 0xFFFF else str(ksuf))
            if kind == 2:
                v = str(struct.unpack('>i', struct.pack('>I', val))[0])
            elif kind == 3:
                v = fmt_float(struct.unpack('>f', struct.pack('>I', val))[0])
            else:
                v = cs(vals, val)
            out.append('%s=%s' % (kname, v))
        out.append('')
    return '\r\n'.join(out).rstrip('\r\n') + '\r\n'


def compare(cmp_root, dst, out_root, data, stats, skip_header=False):
    if not cmp_root:
        return
    rel = os.path.relpath(dst, out_root).lower()
    ref = cmp_root.get(rel) or cmp_root.get(os.path.basename(rel))
    if not ref:
        return
    other = open(ref, 'rb').read()
    same = other[128:] == data[128:] if skip_header else other == data
    stats['same as source (%s)' % os.path.splitext(dst)[1]] += same
    stats['different from source (%s)' % os.path.splitext(dst)[1]] += not same


def copy_loose(game, out_root, scene):
    # everything that is already loose in the game (except the packages)
    src = os.path.join(game, 'C', 'Scenes', scene)
    n = 0
    for root, _, files in os.walk(src):
        for f in files:
            if f.lower().endswith(('.x360.res', '.x360.str')):
                continue
            rel = os.path.relpath(os.path.join(root, f), src)
            dst = os.path.join(out_root, scene, rel)
            if not os.path.exists(dst):
                os.makedirs(os.path.dirname(dst), exist_ok=True)
                shutil.copy2(os.path.join(root, f), dst)
                n += 1
    return n


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--game', default=r'C:\Games\Cars Race-O-Rama Recomp')
    ap.add_argument('--out', default=None, help=r'output folder (default <game>\extracted_scenes)')
    ap.add_argument('--only', default='', help='comma-separated scenes')
    ap.add_argument('--compare', default=None, help='folder of loose source scenes to compare against')
    ap.add_argument('--source', default=None, help=r'root with the C\Scenes\...\X.x360.res folders extracted by scenes_extract.py')
    a = ap.parse_args()
    out_root = a.out or os.path.join(a.game, 'extracted_scenes')
    # folders extracted from the packages (scenes_extract.py): written to mods; after switching to loose scenes they
    # live in _old_mod_folders
    mods = a.source or next((p for p in (os.path.join(a.game, '_old_mod_folders'), os.path.join(a.game, 'mods'))
                             if os.path.isdir(os.path.join(p, 'C', 'Scenes', 'FE', 'FE.x360.res'))), os.path.join(a.game, 'mods'))
    exe = os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', 'build', 'Release', 'CarsRecomp.exe')
    if not os.path.exists(exe):
        exe = os.path.join(a.game, 'Cars Race-O-Rama.exe')
    only = [s.strip().lower() for s in a.only.split(',') if s.strip()]
    scenes_root = os.path.join(mods, 'C', 'Scenes')
    cmp_root = None
    if a.compare:
        cmp_root = {}
        for r, _, ns in os.walk(a.compare):
            for n in ns:
                full = os.path.join(r, n)
                cmp_root.setdefault(os.path.relpath(full, a.compare).lower(), full)
                cmp_root.setdefault(n.lower(), full)
    for scene in sorted(os.listdir(scenes_root)):
        if only and scene.lower() not in only:
            continue
        stats = collections.Counter()
        WRITTEN.clear()
        for root, dirs, _ in os.walk(os.path.join(scenes_root, scene)):
            dirs.sort()   # X.x360.res before X_Landmark.x360.res
            for d in dirs:
                if d.lower().endswith('.x360.res'):
                    decompile_pack(a.game, mods, out_root, exe, os.path.relpath(os.path.join(root, d), mods), stats, cmp_root)
        stats['loose files copied'] = copy_loose(a.game, out_root, scene)
        print('%s: %s' % (scene, ', '.join('%s %d' % kv for kv in sorted(stats.items()) if kv[1])))
        sys.stdout.flush()


if __name__ == '__main__':
    main()

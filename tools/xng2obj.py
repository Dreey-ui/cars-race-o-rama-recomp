# The game engine's .xng models -> .obj (+ .mtl with the .dds textures), and whole scenes assembled from the complete .scn.
#   python -I tools/xng2obj.py <loose scenes folder> [--only FE,HUB]
# The folder is the one with the scenes as loose files (source C\Scenes: <scene>\Geo\*.xng, <scene>\Tex\*.dds and *.xnm,
# <scene>\<scene>.x360.scn with [ModelN] SLT/Position/LookVector/UpVector). Output in <scene>\_obj\:
#   models\<name>.obj   each .xng (all LODs as groups; LOD 0 first)
#   <scene>.obj         the scene: each [ModelN] of the .scn with LOD 0 of the model at its position/orientation
#   materials.mtl       material -> texture (TextureMap_1 of the .xnm)
#
# .xng format (big-endian), the same as the "xgs" in the .soi of the packed scenes but with the data in the file itself:
#   'xng\0', u32 version (5), u32 objects; per object: name[0x80], 4x4 matrix (floats), bounds (7 floats),
#   FFFFFFFF, u32 materials, materials x name[0x40], u8 LODs (+3), per LOD: f32 distance, u32 submeshes, per submesh:
#   {u32 material, u32 streams, u8 n, u8, u8 (0 = data here, 1 = in the stream), u8, u16, n x f32, u16 vertices,
#   u16 indices} followed by the data: u16 indices (strip, FFFF restarts) aligned to 4, then one stream per attribute in
#   the order position (0x01, 3 floats), normal (0x02, 3 floats), color (0x08, ARGB), UV0 (0x04, 2 floats), UV1 (0x10),
#   UV2 (0x4000), tangent (0x20, 3 floats).
import argparse
import math
import os
import re
import struct
import sys

STREAMS = [(0x01, 'pos', 12), (0x02, 'nrm', 12), (0x08, 'col', 4), (0x04, 'uv0', 8), (0x10, 'uv1', 8), (0x4000, 'uv2', 8),
           (0x20, 'tan', 12)]
KNOWN = sum(b for b, _, _ in STREAMS)


def cstr(b):
    return b.split(b'\0')[0].decode('latin-1')


def parse_xng(d):
    if d[:4] != b'xng\0':
        raise ValueError('not an .xng')
    ver, count = struct.unpack_from('>II', d, 4)
    p = 12
    objs = []
    for _ in range(count):
        name = cstr(d[p:p + 0x80])
        matrix = struct.unpack_from('>16f', d, p + 0x80)
        p += 0x80 + 0x40 + 0x1C
        if d[p:p + 4] != b'\xff\xff\xff\xff':
            raise ValueError('unexpected header at %X' % p)
        nmat = struct.unpack_from('>I', d, p + 4)[0]
        p += 8
        mats = [cstr(d[p + k * 0x40:p + (k + 1) * 0x40]) for k in range(nmat)]
        p += 0x40 * nmat
        nlod = d[p]
        p += 4
        lods = []
        for _ in range(nlod):
            dist, nsub = struct.unpack_from('>fI', d, p)
            p += 8
            subs = []
            for _ in range(nsub):
                mat, flags = struct.unpack_from('>II', d, p)
                nf = d[p + 8]
                nv, ni = struct.unpack_from('>HH', d, p + 14 + 4 * nf)
                p += 18 + 4 * nf
                if flags & ~KNOWN:
                    raise ValueError('unknown streams %X' % flags)
                idx = struct.unpack_from('>%dH' % ni, d, p)
                q = p + ((ni * 2 + 3) & ~3)
                sub = {'mat': mat, 'idx': idx, 'nv': nv}
                for bit, key, size in STREAMS:
                    if flags & bit:
                        if key == 'col':
                            sub[key] = [d[q + i * 4:q + i * 4 + 4] for i in range(nv)]
                        else:
                            sub[key] = list(struct.iter_unpack('>%df' % (size // 4), d[q:q + nv * size]))
                        q += nv * size
                p = q
                subs.append(sub)
            lods.append((dist, subs))
        objs.append({'name': name, 'matrix': matrix, 'mats': mats, 'lods': lods})
    if p != len(d):
        raise ValueError('%d bytes left over' % (len(d) - p))
    return objs


def strip_tris(idx):
    # strips separated by FFFF; the parity (vertex order) restarts in each strip; degenerate triangles are dropped
    tris = []
    strip = []
    for v in list(idx) + [0xFFFF]:
        if v != 0xFFFF:
            strip.append(v)
            continue
        for i in range(len(strip) - 2):
            a, b, c = strip[i], strip[i + 1], strip[i + 2]
            if i & 1:
                a, b = b, a
            if a != b and b != c and a != c:
                tris.append((a, b, c))
        strip = []
    return tris


def xnm_texture(path):
    try:
        for line in open(path, encoding='latin-1'):
            m = re.match(r'\s*TextureMap_1\s*=\s*(\S+)', line)
            if m:
                return m.group(1)
    except OSError:
        pass
    return None


class Writer:
    def __init__(self, f):
        self.f = f
        self.base = 0

    def mesh(self, sub, mat_name, xf=None):
        f = self.f
        nv = sub['nv']
        pos = sub['pos']
        nrm = sub.get('nrm')
        uv = sub.get('uv0')
        col = sub.get('col')
        for i in range(nv):
            x, y, z = pos[i]
            if xf:
                x, y, z = xf(x, y, z, 1.0)
            line = 'v %.6g %.6g %.6g' % (x, y, -z)
            if col:
                c = col[i]
                line += ' %.3f %.3f %.3f' % (c[1] / 255.0, c[2] / 255.0, c[3] / 255.0)
            f.write(line + '\n')
        for i in range(nv):
            u, v = uv[i] if uv else (0.0, 0.0)
            f.write('vt %.6g %.6g\n' % (u, 1.0 - v))
        for i in range(nv):
            nx, ny, nz = nrm[i] if nrm else (0.0, 1.0, 0.0)
            if xf:
                nx, ny, nz = xf(nx, ny, nz, 0.0)
            f.write('vn %.4g %.4g %.4g\n' % (nx, ny, -nz))
        f.write('usemtl %s\n' % mat_name)
        b = self.base + 1
        for a, c, d in strip_tris(sub['idx']):
            if max(a, c, d) >= nv:
                continue
            # Z flipped: vertex order reversed
            f.write('f %d/%d/%d %d/%d/%d %d/%d/%d\n' % (a + b, a + b, a + b, d + b, d + b, d + b, c + b, c + b, c + b))
        self.base += nv


def placement(look, up, pos):
    # .scn orientation: LookVector = model Z axis, UpVector = Y axis, X = Y x Z (D3D left-handed). The two
    # vectors are NOT unit length: the length is the model scale (e.g. 1.755) -- checked against the GPU capture
    lx, ly, lz = look
    ux, uy, uz = up
    ln = math.sqrt(lx * lx + ly * ly + lz * lz) or 1
    un = math.sqrt(ux * ux + uy * uy + uz * uz) or 1
    rx, ry, rz = uy * lz - uz * ly, uz * lx - ux * lz, ux * ly - uy * lx
    rn = math.sqrt(rx * rx + ry * ry + rz * rz) or 1
    s = (ln + un) / 2   # X axis scale (the .scn only stores Y and Z)
    rx, ry, rz = rx / rn * s, ry / rn * s, rz / rn * s
    px, py, pz = pos

    def xf(x, y, z, w):
        return (x * rx + y * ux + z * lx + w * px, x * ry + y * uy + z * ly + w * py, x * rz + y * uz + z * lz + w * pz)
    return xf


def parse_scn(path):
    models = []
    cur = None
    for line in open(path, encoding='latin-1'):
        line = line.strip()
        m = re.match(r'\[Model\d+\]', line)
        if m:
            cur = {}
            models.append(cur)
            continue
        if line.startswith('['):
            cur = None
            continue
        if cur is not None and '=' in line:
            k, v = line.split('=', 1)
            cur.setdefault(k.strip().lower(), v.strip())
    return models


def vec(s, default):
    try:
        v = [float(x) for x in s.split(',')[:3]]
        return v if len(v) == 3 else default
    except (AttributeError, ValueError):
        return default


def convert_scene(scene_dir, scene):
    out = os.path.join(scene_dir, '_obj')
    os.makedirs(os.path.join(out, 'models'), exist_ok=True)
    files = {}
    for root, _, names in os.walk(scene_dir):
        if os.path.relpath(root, scene_dir).split(os.sep)[0] == '_obj':
            continue
        for n in names:
            files.setdefault(n.lower(), os.path.join(root, n))
    # materials: .xnm name -> texture -> .dds
    materials = {}

    def material(name):
        key = name.lower()
        if key not in materials:
            tex = xnm_texture(files[key + '.xnm']) if key + '.xnm' in files else None
            dds = files.get((tex or name).lower() + '.dds')
            materials[key] = os.path.relpath(dds, out).replace('\\', '/') if dds else None
        return 'm_' + re.sub(r'[^A-Za-z0-9_]', '_', key)

    parsed = {}
    errors = []
    for n, path in sorted(files.items()):
        if not n.endswith('.xng'):
            continue
        try:
            objs = parse_xng(open(path, 'rb').read())
        except Exception as e:   # noqa: BLE001 - log and go on
            errors.append('%s: %s' % (n, e))
            continue
        parsed[n[:-4]] = objs
        with open(os.path.join(out, 'models', n[:-4] + '.obj'), 'w', encoding='latin-1') as f:
            f.write('# %s (%s)\nmtllib ../materials.mtl\n' % (os.path.basename(path), scene))
            w = Writer(f)
            for o in objs:
                for li, (dist, subs) in enumerate(o['lods']):
                    f.write('o %s_LOD%d\n' % (o['name'], li))
                    for s in subs:
                        if 'pos' in s:
                            w.mesh(s, material(o['mats'][s['mat']] if s['mat'] < len(o['mats']) else 'no_material'))
    placed = missing = 0
    scn = files.get(scene.lower() + '.x360.scn')
    if scn:
        with open(os.path.join(out, scene + '.obj'), 'w', encoding='latin-1') as f:
            f.write('# scene %s assembled from %s\nmtllib materials.mtl\n' % (scene, os.path.basename(scn)))
            w = Writer(f)
            for k, m in enumerate(parse_scn(scn)):
                objs = parsed.get(m.get('slt', '').lower())
                if not objs:
                    missing += 1
                    continue
                xf = placement(vec(m.get('lookvector'), [0, 0, 1]), vec(m.get('upvector'), [0, 1, 0]), vec(m.get('position'), [0, 0, 0]))
                for o in objs:
                    if not o['lods']:
                        continue
                    f.write('o %03d_%s\n' % (k + 1, o['name']))
                    for s in o['lods'][0][1]:
                        if 'pos' in s:
                            w.mesh(s, material(o['mats'][s['mat']] if s['mat'] < len(o['mats']) else 'no_material'), xf)
                placed += 1
    with open(os.path.join(out, 'materials.mtl'), 'w', encoding='latin-1') as f:
        for key, tex in sorted(materials.items()):
            f.write('newmtl m_%s\nKd 1 1 1\n' % re.sub(r'[^A-Za-z0-9_]', '_', key))
            if tex:
                f.write('map_Kd %s\n' % tex)
            f.write('\n')
    print('%s: %d .xng models -> .obj, scene with %d placed models (%d .scn names without .xng), %d errors' % (
        scene, len(parsed), placed, missing, len(errors)))
    for e in errors[:5]:
        print('   ERROR', e)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('folder')
    ap.add_argument('--only', default='')
    a = ap.parse_args()
    only = [s.strip().lower() for s in a.only.split(',') if s.strip()]
    for scene in sorted(os.listdir(a.folder)):
        d = os.path.join(a.folder, scene)
        if os.path.isdir(d) and (not only or scene.lower() in only):
            convert_scene(d, scene)


if __name__ == '__main__':
    main()

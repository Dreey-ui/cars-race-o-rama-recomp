# Modelos .xng do motor do jogo -> .obj (+ .mtl com as texturas .dds), e cenas inteiras montadas pelo .scn completo.
#   python -I tools/xng2obj.py <pasta das cenas soltas> [--so FE,HUB]
# A pasta e a das cenas em arquivos soltos (C\Scenes de origem: <cena>\Geo\*.xng, <cena>\Tex\*.dds e *.xnm,
# <cena>\<cena>.x360.scn com [ModelN] SLT/Position/LookVector/UpVector). Saida em <cena>\_obj\:
#   modelos\<nome>.obj  cada .xng (todos os LODs como grupos; o LOD 0 primeiro)
#   <cena>.obj          a cena: cada [ModelN] do .scn com o LOD 0 do modelo na posicao/orientacao dele
#   materiais.mtl       material -> textura (TextureMap_1 do .xnm)
#
# Formato .xng (big-endian), igual ao "xgs" do .soi das cenas empacotadas mas com os dados no proprio arquivo:
#   'xng\0', u32 versao (5), u32 objetos; por objeto: nome[0x80], matriz 4x4 (floats), limites (7 floats),
#   FFFFFFFF, u32 materiais, materiais x nome[0x40], u8 LODs (+3), por LOD: f32 distancia, u32 submalhas, por submalha:
#   {u32 material, u32 streams, u8 n, u8, u8 (0 = dados aqui, 1 = no stream), u8, u16, n x f32, u16 vertices,
#   u16 indices} seguido dos dados: indices u16 (tira, FFFF reinicia) alinhados a 4, depois um stream por atributo na
#   ordem posicao (0x01, 3 floats), normal (0x02, 3 floats), cor (0x08, ARGB), UV0 (0x04, 2 floats), UV1 (0x10),
#   UV2 (0x4000), tangente (0x20, 3 floats).
import argparse
import math
import os
import re
import struct
import sys

STREAMS = [(0x01, 'pos', 12), (0x02, 'nrm', 12), (0x08, 'cor', 4), (0x04, 'uv0', 8), (0x10, 'uv1', 8), (0x4000, 'uv2', 8),
           (0x20, 'tan', 12)]
KNOWN = sum(b for b, _, _ in STREAMS)


def cstr(b):
    return b.split(b'\0')[0].decode('latin-1')


def parse_xng(d):
    if d[:4] != b'xng\0':
        raise ValueError('nao e .xng')
    ver, count = struct.unpack_from('>II', d, 4)
    p = 12
    objs = []
    for _ in range(count):
        name = cstr(d[p:p + 0x80])
        matrix = struct.unpack_from('>16f', d, p + 0x80)
        p += 0x80 + 0x40 + 0x1C
        if d[p:p + 4] != b'\xff\xff\xff\xff':
            raise ValueError('cabecalho inesperado em %X' % p)
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
                    raise ValueError('streams desconhecidos %X' % flags)
                idx = struct.unpack_from('>%dH' % ni, d, p)
                q = p + ((ni * 2 + 3) & ~3)
                sub = {'mat': mat, 'idx': idx, 'nv': nv}
                for bit, key, size in STREAMS:
                    if flags & bit:
                        if key == 'cor':
                            sub[key] = [d[q + i * 4:q + i * 4 + 4] for i in range(nv)]
                        else:
                            sub[key] = list(struct.iter_unpack('>%df' % (size // 4), d[q:q + nv * size]))
                        q += nv * size
                p = q
                subs.append(sub)
            lods.append((dist, subs))
        objs.append({'name': name, 'matrix': matrix, 'mats': mats, 'lods': lods})
    if p != len(d):
        raise ValueError('sobraram %d bytes' % (len(d) - p))
    return objs


def strip_tris(idx):
    # tiras separadas por FFFF; a paridade (ordem dos vertices) recomeca em cada tira; triangulos degenerados saem
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
        cor = sub.get('cor')
        for i in range(nv):
            x, y, z = pos[i]
            if xf:
                x, y, z = xf(x, y, z, 1.0)
            line = 'v %.6g %.6g %.6g' % (x, y, -z)
            if cor:
                c = cor[i]
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
            # Z invertido: ordem dos vertices invertida
            f.write('f %d/%d/%d %d/%d/%d %d/%d/%d\n' % (a + b, a + b, a + b, d + b, d + b, d + b, c + b, c + b, c + b))
        self.base += nv


def placement(look, up, pos):
    # orientacao do .scn: LookVector = eixo Z do modelo, UpVector = eixo Y, X = Y x Z (mao esquerda do D3D). Os dois
    # vetores NAO sao unitarios: o comprimento e a escala do modelo (ex. 1.755) -- conferido com a captura da GPU
    lx, ly, lz = look
    ux, uy, uz = up
    ln = math.sqrt(lx * lx + ly * ly + lz * lz) or 1
    un = math.sqrt(ux * ux + uy * uy + uz * uz) or 1
    rx, ry, rz = uy * lz - uz * ly, uz * lx - ux * lz, ux * ly - uy * lx
    rn = math.sqrt(rx * rx + ry * ry + rz * rz) or 1
    s = (ln + un) / 2   # escala do eixo X (o .scn so guarda Y e Z)
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
    os.makedirs(os.path.join(out, 'modelos'), exist_ok=True)
    files = {}
    for root, _, names in os.walk(scene_dir):
        if os.path.relpath(root, scene_dir).split(os.sep)[0] == '_obj':
            continue
        for n in names:
            files.setdefault(n.lower(), os.path.join(root, n))
    # materiais: nome do .xnm -> textura -> .dds
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
        except Exception as e:   # noqa: BLE001 - registra e segue
            errors.append('%s: %s' % (n, e))
            continue
        parsed[n[:-4]] = objs
        with open(os.path.join(out, 'modelos', n[:-4] + '.obj'), 'w', encoding='latin-1') as f:
            f.write('# %s (%s)\nmtllib ../materiais.mtl\n' % (os.path.basename(path), scene))
            w = Writer(f)
            for o in objs:
                for li, (dist, subs) in enumerate(o['lods']):
                    f.write('o %s_LOD%d\n' % (o['name'], li))
                    for s in subs:
                        if 'pos' in s:
                            w.mesh(s, material(o['mats'][s['mat']] if s['mat'] < len(o['mats']) else 'sem_material'))
    placed = missing = 0
    scn = files.get(scene.lower() + '.x360.scn')
    if scn:
        with open(os.path.join(out, scene + '.obj'), 'w', encoding='latin-1') as f:
            f.write('# cena %s montada pelo %s\nmtllib materiais.mtl\n' % (scene, os.path.basename(scn)))
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
                            w.mesh(s, material(o['mats'][s['mat']] if s['mat'] < len(o['mats']) else 'sem_material'), xf)
                placed += 1
    with open(os.path.join(out, 'materiais.mtl'), 'w', encoding='latin-1') as f:
        for key, tex in sorted(materials.items()):
            f.write('newmtl m_%s\nKd 1 1 1\n' % re.sub(r'[^A-Za-z0-9_]', '_', key))
            if tex:
                f.write('map_Kd %s\n' % tex)
            f.write('\n')
    print('%s: %d modelos .xng -> .obj, cena com %d modelos posicionados (%d nomes do .scn sem .xng), %d erros' % (
        scene, len(parsed), placed, missing, len(errors)))
    for e in errors[:5]:
        print('   ERRO', e)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('pasta')
    ap.add_argument('--so', default='')
    a = ap.parse_args()
    only = [s.strip().lower() for s in a.so.split(',') if s.strip()]
    for scene in sorted(os.listdir(a.pasta)):
        d = os.path.join(a.pasta, scene)
        if os.path.isdir(d) and (not only or scene.lower() in only):
            convert_scene(d, scene)


if __name__ == '__main__':
    main()

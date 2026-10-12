# Descompila os pacotes das cenas do jogo (C\Scenes\**\*.x360.res + *.x360.str) para arquivos soltos, na mesma
# estrutura de pastas das cenas de origem (como um C\Scenes de desenvolvimento):
#   <saida>\<Cena>\geo\*.xng   modelos (cabecalho "xgs" do .soi + dados do stream; identico ao .xng de origem)
#   <saida>\<Cena>\tex\*.dds   texturas (DDS de PC, sem tiling, todos os mipmaps)    + *.xnm materiais em texto
#   <saida>\<Cena>\col\*.gol   colisoes (cabecalho do .soi + dados do trecho B do stream, compactados como no .gol)
#   <saida>\<Cena>\...         todo o resto que ja era solto no jogo (Eco, Anim, Skies, .scn, .toy, .aud...)
#   <saida>\<Cena>\_pacote\    .soi/.toc originais (indice da cena: posicoes, zonas) e os materiais .bpb que nao converteram
# Uso: python -I tools/scenes_decompile.py [--jogo <pasta>] [--saida <pasta>] [--so FE,HUB] [--comparar <cenas soltas>]
# Le as pastas extraidas por tools/scenes_extract.py (<jogo>\mods\C\Scenes) e monta o stream de cada uma com
# "Cars Race-O-Rama.exe --montar" (mesmo codigo que o jogo usa).
import argparse
import bisect
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
    """Cabecalho xgs em soi[x:] + bloco de dados do stream -> .xng (dados de cada submalha logo depois do registro dela)"""
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
                rec[10] = 0   # dados no proprio arquivo (1 = no stream)
                out += rec
                p += 18 + 4 * nf
                size = ((ni * 2 + 3) & ~3) + nv * vstride(flags)
                out += blob[q:q + size]
                q += size
    return bytes(out), q


def col_to_gol(header, blob):
    """Cabecalho da colisao no .soi (0x58 bytes) + dados do stream -> .gol (vertices float3, nos 32 bytes, triangulos 22)"""
    h = bytearray(header[:0x58])
    struct.pack_into('>I', h, 0x0C, 0)   # 9 = streamado
    nv, nn, nt = struct.unpack_from('>III', h, 0x48)
    out = bytearray(h)
    p = 0
    for i in range(nv):
        out += blob[p:p + 12]
        p += 16
    p = (p + 31) & ~31   # no stream os nos comecam alinhados a 32 bytes
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
    """Tamanho do cabecalho xgs em soi[x:] (mesmo percurso de xgs_to_xng, sem os dados)"""
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
    """Registros do .soi: [(tipo 0 modelo/1 textura/2 colisao, nome, posicao do conteudo, tamanho do conteudo)].
    Cabecalho: +0xC colisoes, +0x10 modelos, +0x18 texturas do stream, +0x1C texturas so no .soi (DDS de PC, logo depois
    das outras), +0x28/+0x30/+0x34 posicao (relativa a 0x3C) do 1o registro de modelos/colisoes/texturas. Registro:
    0x3C de posicionamento antes, u32 indice, nome[0x100], u32, u32, u32 n, n x 0x208 propriedades, conteudo."""
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
    # o .toc as vezes tem espacos que o nome interno do xgs nao tem ("wood catwalk01" / "woodcatwalk01")
    return name.lower().replace(' ', '')


# arquivos ja gravados na cena atual: o mesmo recurso em dois pacotes da mesma pasta (CR_05 e CR_05_Landmark, HUB e
# HUB_Landmark) vira um arquivo so; fica a versao do primeiro pacote (o principal), as vezes com mipmaps um pouco diferentes
WRITTEN = set()


def first_write(dst, stats):
    key = os.path.normcase(os.path.abspath(dst))
    if key in WRITTEN:
        stats['repetido em outro pacote da cena (fica o 1o)'] += 1
        return False
    WRITTEN.add(key)
    return True


def out_path(saida, scene_rel, pack_rel_dir, name, ext):
    # "C\Scenes\<Cena>\geo\X" -> <saida>\<Cena>\geo\X; outros (C\ShadowMap\Tex\X...) -> pasta do pacote\ultimas 2 partes
    parts = name.replace('/', '\\').split('\\')
    if len(parts) > 3 and parts[0].lower() == 'c' and parts[1].lower() == 'scenes':
        rel = os.path.join(*parts[2:])
    else:
        rel = os.path.join(pack_rel_dir, *parts[-2:])
    return os.path.join(saida, rel + ext)


def build_stream(exe, folder, tmpdir):
    out = os.path.join(tmpdir, 'stream.bin')
    subprocess.run([exe, '--montar', folder, out], check=True, capture_output=True)
    return open(out, 'rb').read()


def decompile_pack(game, mods, saida, exe, res_rel, stats, cmp_root):
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
            # stream sem registros (MiniMap): cada peca tem o seu "Section_N" terminando em {posicao, total, total, 0}
            # logo antes do nome
            if toc_pos < 16:
                return 0
            off, tot, da = struct.unpack_from('>3I', toc, toc_pos - 16)
            return off if tot and tot == da and off % 0x800 == 0 and off + tot <= len(raw) else 0
        r = recs[bisect.bisect_right(starts, toc_pos) - 1]
        off, na, tab = r[1], r[4], r[6]
        return off if kind != 2 else off + sum(tab[:na])

    pack_out = os.path.join(saida, pack_rel_dir, '_pacote')
    os.makedirs(pack_out, exist_ok=True)
    for n in soi_n + toc_n:
        shutil.copyfile(os.path.join(res_dir, n), os.path.join(pack_out, n))
    res = resources(toc)
    # modelos: xgs do .soi pelo nome interno
    xgs = {}
    x = soi.find(b'xgs\0')
    while x >= 0:
        if soi[x + 0xE8:x + 0xEC] == b'\xff\xff\xff\xff':
            xgs.setdefault(norm(soi[x + 12:x + 12 + 0x80].split(b'\0')[0].decode('latin-1')), x)
        x = soi.find(b'xgs\0', x + 4)
    # colisoes: cabecalho 00 00 04 D2 'col' precedido do nome do registro no .soi
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
            continue   # mesma coisa em outra zona do HUB
        base = section(toc_pos, f[5])
        blob = raw[base + f[3]:base + f[3] + f[4]]
        if f[5] == 0:
            x = xgs.get(norm(key.rsplit('\\', 1)[-1]))
            if x is None:
                stats['modelo sem cabecalho'] += 1
                continue
            data, used = xgs_to_xng(soi, x, blob)
            dst = out_path(saida, None, pack_rel_dir, name, '.xng')
            stats['modelos'] += 1
            stats['modelo bloco incompleto'] += used != len(blob)
        elif f[5] == 2:
            c = cols.get(norm(key))
            if c is None:
                stats['colisao sem cabecalho'] += 1
                continue
            data, used = col_to_gol(soi[c:c + 0x58], blob)
            dst = out_path(saida, None, pack_rel_dir, name, '.gol')
            stats['colisoes'] += 1
            stats['colisao bloco incompleto'] += used != len(blob)
        elif f[5] == 1 and blob[:4] == b'DDS ':
            # textura que ja esta no stream como DDS de PC inteiro (MiniMap do CR_01); as do Xenos saem abaixo
            data = blob
            dst = out_path(saida, None, pack_rel_dir, name, '.dds')
            stats['texturas (DDS de PC no stream)'] += 1
        else:
            continue
        done.add((key, f[5]))
        if not first_write(dst, stats):
            continue
        os.makedirs(os.path.dirname(dst), exist_ok=True)
        with open(dst, 'wb') as fp:
            fp.write(data)
        compare(cmp_root, dst, saida, data, stats)
    # texturas
    if raw:
        for name, fetch, places in st.scene_textures(res_dir, len(raw), sem_registros=True):
            fmt = st.FORMATS.get(fetch.format)
            pos, size = places[0]
            if not fmt or fetch.dimension not in (1, 3):
                stats['textura nao suportada'] += 1
                continue
            dst = out_path(saida, None, pack_rel_dir, name, '.dds')
            if not first_write(dst, stats):
                continue
            dds, _ = st.export_texture(fetch, fmt, raw[pos:pos + size])
            os.makedirs(os.path.dirname(dst), exist_ok=True)
            with open(dst, 'wb') as fp:
                fp.write(dds)
            stats['texturas'] += 1
            compare(cmp_root, dst, saida, dds, stats, skip_header=True)
    # texturas que so existem no .soi, como DDS de PC (86 no HUB, fora do stream): o .dds sai inteiro
    for kind, name, q, size in soi_records(soi):
        if kind != 1 or size == 0x38 or soi[q + 4:q + 8] != b'DDS ' or (name.lower(), 1) in done:
            continue
        done.add((name.lower(), 1))
        dst = out_path(saida, None, pack_rel_dir, name, '.dds')
        if not first_write(dst, stats):
            continue
        data = soi[q + 4:q + size]
        os.makedirs(os.path.dirname(dst), exist_ok=True)
        with open(dst, 'wb') as fp:
            fp.write(data)
        stats['texturas (DDS de PC no .soi)'] += 1
        compare(cmp_root, dst, saida, data, stats)
    # materiais (.xnm.bpb do .res) -> .xnm em texto
    for line in open(os.path.join(res_dir, '_indice.txt'), encoding='latin-1'):
        n = line.strip().replace(' zlibme', '')
        if line.startswith('#') or not n.lower().endswith('.xnm.bpb'):
            continue
        src = os.path.join(res_dir, *n.split('\\'))
        data = open(src, 'rb').read()
        try:
            txt = bpb_to_ini(data).encode('latin-1')
            dst = out_path(saida, None, pack_rel_dir, n[:-4], '')
            stats['materiais'] += 1
        except Exception:   # noqa: BLE001
            txt = data   # fica .xnm.bpb no caminho do jogo: o jogo le o binario direto da pasta
            dst = out_path(saida, None, pack_rel_dir, n, '')
            stats['material .bpb mantido'] += 1
        os.makedirs(os.path.dirname(dst), exist_ok=True)
        with open(dst, 'wb') as fp:
            fp.write(txt)
    # CRC32 de cada arquivo solto do pacote como foi extraido: o runtime compara e loga "(modificado)" no que mudou
    ext = {0: '.xng', 1: '.dds', 2: '.gol'}
    paths = {out_path(saida, None, pack_rel_dir, name, ext[f[5]]) for _, name, f in res if f[5] in ext}
    paths |= {out_path(saida, None, pack_rel_dir, name, '.dds') for kind, name, q, size in soi_records(soi)
              if kind == 1 and size != 0x38}
    lines = []
    for path in sorted(paths):
        if os.path.exists(path):
            lines.append('%08X %s' % (zlib.crc32(open(path, 'rb').read()) & 0xFFFFFFFF, os.path.relpath(path, saida)))
    stem = soi_n[0][:-4]
    with open(os.path.join(pack_out, stem + '.arquivos.txt'), 'w', encoding='latin-1', newline='\r\n') as fp:
        fp.write('# CRC32 dos arquivos soltos deste pacote como foram extraidos (o runtime avisa no log o que mudou)\n')
        fp.write('\n'.join(lines) + '\n')


def fmt_float(v):
    return '%.1f' % v if v == int(v) and abs(v) < 1e7 else '%.6g' % v


def bpb_to_ini(d):
    """INI binario do motor ("BPB\\0" / "BPBh"): registros {hash, nome|tipo, sufixo, n/valor} + tabelas de nomes e valores"""
    if d[:3] != b'BPB':
        raise ValueError('nao e BPB')
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


def compare(cmp_root, dst, saida, data, stats, skip_header=False):
    if not cmp_root:
        return
    rel = os.path.relpath(dst, saida).lower()
    ref = cmp_root.get(rel) or cmp_root.get(os.path.basename(rel))
    if not ref:
        return
    other = open(ref, 'rb').read()
    same = other[128:] == data[128:] if skip_header else other == data
    stats['iguais a origem (%s)' % os.path.splitext(dst)[1]] += same
    stats['diferentes da origem (%s)' % os.path.splitext(dst)[1]] += not same


def copy_loose(game, saida, scene):
    # tudo que ja e solto no jogo (menos os pacotes)
    src = os.path.join(game, 'C', 'Scenes', scene)
    n = 0
    for root, _, files in os.walk(src):
        for f in files:
            if f.lower().endswith(('.x360.res', '.x360.str')):
                continue
            rel = os.path.relpath(os.path.join(root, f), src)
            dst = os.path.join(saida, scene, rel)
            if not os.path.exists(dst):
                os.makedirs(os.path.dirname(dst), exist_ok=True)
                shutil.copy2(os.path.join(root, f), dst)
                n += 1
    return n


def main():
    import collections
    ap = argparse.ArgumentParser()
    ap.add_argument('--jogo', default=r'C:\Games\Cars Race-O-Rama Recomp')
    ap.add_argument('--saida', default=None)
    ap.add_argument('--so', default='')
    ap.add_argument('--comparar', default=None, help='pasta de cenas soltas de origem para conferir')
    ap.add_argument('--origem', default=None, help=r'raiz com C\Scenes\...\X.x360.res extraidos (scenes_extract.py)')
    a = ap.parse_args()
    saida = a.saida or os.path.join(a.jogo, 'cenas_extraidas')
    # pastas extraidas dos pacotes (scenes_extract.py): saem em mods; depois da troca para arquivos soltos ficam em
    # _mods_pastas_antigas
    mods = a.origem or next((p for p in (os.path.join(a.jogo, '_mods_pastas_antigas'), os.path.join(a.jogo, 'mods'))
                             if os.path.isdir(os.path.join(p, 'C', 'Scenes', 'FE', 'FE.x360.res'))), os.path.join(a.jogo, 'mods'))
    exe = os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', 'build', 'Release', 'CarsRecomp.exe')
    if not os.path.exists(exe):
        exe = os.path.join(a.jogo, 'Cars Race-O-Rama.exe')
    only = [s.strip().lower() for s in a.so.split(',') if s.strip()]
    scenes_root = os.path.join(mods, 'C', 'Scenes')
    cmp_root = None
    if a.comparar:
        cmp_root = {}
        for r, _, ns in os.walk(a.comparar):
            for n in ns:
                full = os.path.join(r, n)
                cmp_root.setdefault(os.path.relpath(full, a.comparar).lower(), full)
                cmp_root.setdefault(n.lower(), full)
    for scene in sorted(os.listdir(scenes_root)):
        if only and scene.lower() not in only:
            continue
        stats = collections.Counter()
        WRITTEN.clear()
        for root, dirs, _ in os.walk(os.path.join(scenes_root, scene)):
            dirs.sort()   # X.x360.res antes de X_Landmark.x360.res
            for d in dirs:
                if d.lower().endswith('.x360.res'):
                    decompile_pack(a.jogo, mods, saida, exe, os.path.relpath(os.path.join(root, d), mods), stats, cmp_root)
        stats['arquivos soltos copiados'] = copy_loose(a.jogo, saida, scene)
        print('%s: %s' % (scene, ', '.join('%s %d' % kv for kv in sorted(stats.items()) if kv[1])))
        sys.stdout.flush()


if __name__ == '__main__':
    main()

# Texturas dos streams das cenas (.x360.str) <-> arquivos .dds de PC (usado por tools/scenes_extract.py, que monta
# a pasta mods\...\X.x360.str\ com write_stream_folder; o runtime faz o caminho inverso em src/scene_assets.cpp).
# O .dds editado tem que ter o mesmo formato, tamanho e numero de mipmaps do original (o espaco no stream e fixo).
#
# Onde fica cada textura: no .soi, registros {nome[0x100], ..., cabecalho D3D do 360 com a fetch constant de 6 dwords
# (assinatura BaseFlush/MipFlush = FFFF0000 FFFF0000 em +0x124)}; no .toc, {nome[0x100], u32 registro de stream,
# u32 indice, u32 indice, u32 posicao no trecho A descomprimido do registro, u32 tamanho, u32 1}. Os mipmaps ficam em
# mip_address (relativo ao inicio da textura); niveis <= 16 texels empacotados num tile (GetPackedMipOffset do Xenia).
import argparse
import bisect
import os
import re
import struct
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import scenes_extract as se   # noqa: E402

# formato Xenos -> (largura do bloco, bytes por bloco, fourcc DDS ou None para RGB, mascaras RGB)
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
    # texture_address.h do Xenia (igual ao TiledOffset2D do runtime); pitch em blocos, ja alinhado a 32
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
    # (nivel, largura, altura, deslocamento do armazenamento, pitch em blocos, x/y em blocos, passo entre faces):
    # GetGuestTextureLayout do Xenia (2D e cubo). Mips: dimensoes de potencia de 2, alinhadas a 32x32 blocos (linear:
    # linha alinhada a 256 bytes), em sequencia a partir de mip_address (cada nivel com as 6 faces no cubo); a partir do
    # nivel empacotado (menor lado <= 16 texels) todos os niveis dividem um armazenamento, cada um num deslocamento.
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
    # posicao (0-3) do byte de cada canal R, G, B, A depois do endian; 4/5 = 0/1
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
        raise ValueError('formato/tamanho/mipmaps diferentes do original')
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
    # nomes de recurso no .toc/.soi: "C\..." ou "c\..." (CT_SM usa minusculas)
    return [m.start() for m in re.finditer(rb'[Cc]\\', buf)]


def scene_textures(res_dir, raw_size, sem_registros=False):
    # [(nome, Fetch, [(posicao no .str cru, tamanho), ...])]: a lista de texturas de cada registro de stream vem logo
    # depois dele no .toc (no HUB, uma por zona; a mesma textura pode estar em varias zonas). sem_registros: stream sem
    # registros (MiniMap, ja cru no disco) -> posicoes diretas no arquivo
    names = os.listdir(res_dir)
    soi_name = [n for n in names if n.lower().endswith('.soi')]
    toc_name = [n for n in names if n.lower().endswith('.toc')]
    if not soi_name or not toc_name:
        return []
    soi = open(os.path.join(res_dir, soi_name[0]), 'rb').read()
    toc = open(os.path.join(res_dir, toc_name[0]), 'rb').read()
    recs = sorted(se.find_records(toc, raw_size), key=lambda r: r[0])
    if not recs and not sem_registros:
        return []
    sections = not recs   # sem registros: posicao de cada peca no "Section_N" dela (abaixo)
    if not recs:
        recs = [(0, 0, raw_size, 0, 1, 0, (raw_size,))]   # um trecho A do tamanho do arquivo
    starts = [r[0] for r in recs]
    places = {}
    for i in name_starts(toc):
        if i + 0x118 <= len(toc):
            name = toc[i:i + 0x100].split(b'\0')[0]
            fields = struct.unpack_from('>6I', toc, i + 0x100)
            if sections and i >= 16:
                # MiniMap: cada peca tem o seu "Section_N" terminando em {posicao, total, total, 0} logo antes
                # do nome; a posicao da textura (fields[3]) e relativa a essa secao
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
    # Pasta do stream (mods\...\X.x360.str\), lida pelo runtime (scene_assets.cpp) no lugar do .str:
    #   <nome da textura>.dds   cada textura do stream (todas as copias no stream recebem o mesmo .dds)
    #   _dados\XXXXXXXX.bin     o resto do stream (geometria e o enchimento dos tiles), na posicao XXXXXXXX
    #   _indice.txt             tamanho do stream, texturas (fetch constant, tamanho, posicoes) e dados
    # O runtime zera o stream, copia os _dados e escreve os blocos de cada .dds (com tiling/endian do Xbox) por cima.
    texs = scene_textures(res_dir, len(raw))
    rest = bytearray(raw)
    lines = ['# Stream da cena montado pelo runtime a partir desta pasta (tools/scenes_extract.py).',
             '# Edite os .dds mantendo formato, tamanho e numero de mipmaps (veja as colunas). Nao edite este indice.',
             'tamanho %X' % len(raw),
             '# textura <fetch constant: 6 dwords> <tamanho> <posicoes no stream> <arquivo>  | formato largura x altura mipmaps']
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
        lines.append('textura %s %X %s %s  | fmt %d %dx%d mips %d%s' % (
            ' '.join('%08X' % v for v in f.raw), size, ','.join('%X' % q for q, _ in places), rel, f.format, f.width,
            f.height, n, ' cubo' if faces == 6 else ''))
        written += 1
    # trechos nao-zero do resto (juntando buracos pequenos)
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
    os.makedirs(os.path.join(folder, '_dados'), exist_ok=True)
    for start, end in runs:
        fn = '_dados\\%08X.bin' % start
        with open(os.path.join(folder, '_dados', '%08X.bin' % start), 'wb') as fp:
            fp.write(rest[start:end])
        lines.append('dados %X %s' % (start, fn))
    with open(os.path.join(folder, '_indice.txt'), 'w', encoding='latin-1', newline='\r\n') as fp:
        fp.write('\n'.join(lines) + '\n')
    return written, len(runs)


def build_from_folder(folder):
    # O mesmo que o runtime faz (scene_assets.cpp): usado para conferir a pasta logo depois de extrair
    size = 0
    texs = []
    datas = []
    for line in open(os.path.join(folder, '_indice.txt'), encoding='latin-1'):
        line = line.rstrip('\r\n')
        if line.startswith('tamanho '):
            size = int(line.split()[1], 16)
        elif line.startswith('textura '):
            parts = line.split('  |')[0].split(' ', 9)
            fetch = [int(x, 16) for x in parts[1:7]]
            texs.append((Fetch(fetch), int(parts[7], 16), [int(x, 16) for x in parts[8].split(',')], parts[9]))
        elif line.startswith('dados '):
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
    print('Use tools/scenes_extract.py (este arquivo e a parte das texturas).')

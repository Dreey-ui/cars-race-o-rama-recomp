# Descompacta os pacotes das cenas (C\Scenes\**\*.x360.res + .x360.str) para pastas em mods, que o runtime le:
#   mods\C\Scenes\X\X.x360.res\   cada arquivo interno do .res, descomprimido (inclusive os ZLIBME) + _indice.txt
#   mods\C\Scenes\X\X.x360.str\   cada textura do stream como .dds + o resto (geometria) em _dados\ + _indice.txt
#                                  (tools/scenes_textures.py); conferido: a pasta remonta o stream byte a byte
# O .toc dentro do .res tem a tabela de tamanhos dos blocos do .str: ela e reescrita para o stream cru.
# O runtime (kernel_io.cpp) monta um .res sem compressao a partir da pasta quando o jogo abre o arquivo e entrega os blocos
# crus do .str sem passar pelo zlib (game_hooks.cpp, uncompress sub_822702F8). Os arquivos originais nao sao alterados.
#
# Uso: python -I tools/scenes_extract.py [--jogo "C:\Games\Cars Race-O-Rama Recomp"] [--mods <pasta>] [--so FE,CR_01]
#
# Formato do .res (v3, leitor em sub_821C3580 / sub_821C1AA0), valores little-endian:
#   +0 versao (2|3), +4 tamanho do cabecalho depois de +8, [v3: +8 flags, +12 0], [flags & 0x40: u32 n + n x u16 tamanho
#   comprimido de cada bloco de 0x6000 (tamanho 0x6000 = bloco guardado sem compressao)], diretorio: u32 quantidade,
#   u32 tamanho do diretorio, entradas {u32 tamanho do nome, nome, u32 posicao, u32 tamanho}; dados logo depois.
#   flags: 0x100 blocos zlib, 0x40 tabela de blocos, 0x200 tamanhos-1, 0x80/0x400 cifra XOR (nao usada nas cenas).
# Formato do .str: registros de stream (um por cena ou um por zona no HUB) com blocos zlib independentes de 128 KB
#   descomprimidos (o ultimo de cada trecho pode ser menor), em posicoes multiplas de 0x800. Cada registro fica no .toc
#   (veja find_records). Os .str do MiniMap nao tem registros: ja sao crus no disco e ficam como estao.
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
        raise ValueError('pacote cifrado (flags %X): nao suportado' % flags)
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
    # Registro de stream no .toc (big-endian): posicao no .str, total, descomprimido A, B, comprimido A, B, blocos A, B,
    # tabela (tamanho de cada bloco no .str). O leitor (sub_820B0398) le cada bloco e manda descomprimir em 128 KB.
    recs = []
    for i in range(4, len(toc) - 28):
        off, tot, da, db, ca, cb, na, nb = struct.unpack_from('>8I', toc, i - 4)
        if not tot or tot != da + db or not (na or nb) or na >= 4096 or nb >= 4096 or off % 0x800 or off + ca + cb > size:
            continue
        # um trecho pode estar vazio (0 blocos e 0 bytes); o leitor so pula a descompressao se os dois estiverem vazios
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
            raise ValueError('registros do .toc nao cobrem o .str (buraco/sobreposicao em %X)' % pos)
        pos = r[1] + sum(r[6])
    if recs and pos != size:
        raise ValueError('registros do .toc terminam em %X, o .str tem %X' % (pos, size))
    return recs


def convert_str(toc, data):
    # Stream cru: cada bloco descomprimido, alinhado a 0x1000 (a camada de arquivo do jogo le direto no buffer), com a
    # posicao/tamanhos/tabela de cada registro reescritos no .toc (o tamanho do .toc nao muda)
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
                # o ultimo bloco de um trecho pode ser ate 0x1000 menor que o declarado (o jogo nao mexe na sobra do destino)
                if not d.eof or not (expected - 0x1000 < len(block) <= expected):
                    raise ValueError('bloco em %X: %X bytes (esperado %X)' % (pos, len(block), expected))
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
        if raw is not None:   # sem registros (MiniMap): o .str ja e cru no disco, fica o original
            files[i] = (files[i][0], new_toc)
            raw_str = raw
    os.makedirs(out_dir, exist_ok=True)
    lines = ['# pacote extraido de %s (versao %d, flags %X). Ordem dos arquivos no .res montado pelo runtime;' % (rel_res, ver, flags),
             '# arquivos novos na pasta entram no fim. "zlibme" = estava comprimido com ZLIBME (o jogo aceita sem).']
    unz = 0
    for name, content in files:
        content, was = unzlibme(content)
        unz += was
        dst = os.path.join(out_dir, *name.replace('/', '\\').split('\\'))
        os.makedirs(os.path.dirname(dst), exist_ok=True)
        with open(dst, 'wb') as f:
            f.write(content)
        lines.append(name + (' zlibme' if was else ''))
    with open(os.path.join(out_dir, '_indice.txt'), 'w', encoding='latin-1', newline='\r\n') as f:
        f.write('\n'.join(lines) + '\n')
    msg = '%s: %d arquivos (%d ZLIBME descomprimidos)' % (rel_res, len(files), unz)
    if raw_str is not None:
        sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
        import scenes_textures as st
        str_dir = os.path.join(mods, rel_res[:-4] + '.str')
        if os.path.isfile(str_dir):
            os.remove(str_dir)   # .str cru da versao anterior desta ferramenta
        n_tex, n_dat = st.write_stream_folder(raw_str, out_dir, str_dir)
        if st.build_from_folder(str_dir) != raw_str:
            raise ValueError('a pasta %s nao remonta o stream igual' % str_dir)
        msg += ' | .str %.1f MB -> pasta com %d .dds + %d blocos de dados (%.1f MB)' % (
            os.path.getsize(src_str) / 1048576.0, n_tex, n_dat, len(raw_str) / 1048576.0)
    print(msg)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--jogo', default=r'C:\Games\Cars Race-O-Rama Recomp')
    ap.add_argument('--mods', default=None)
    ap.add_argument('--so', default='', help='lista de cenas (pastas de C\\Scenes) separadas por virgula')
    a = ap.parse_args()
    mods = a.mods or os.path.join(a.jogo, 'mods')
    only = [s.strip().lower() for s in a.so.split(',') if s.strip()]
    scenes = os.path.join(a.jogo, 'C', 'Scenes')
    for root, _, names in os.walk(scenes):
        for n in sorted(names):
            if not n.lower().endswith('.x360.res'):
                continue
            rel = os.path.relpath(os.path.join(root, n), a.jogo)
            scene = os.path.relpath(root, scenes).split(os.sep)[0].lower()
            if only and scene not in only:
                continue
            try:
                extract(a.jogo, mods, rel)
            except Exception as e:
                print('%s: ERRO %s' % (rel, e))
                sys.exit(1)


if __name__ == '__main__':
    main()

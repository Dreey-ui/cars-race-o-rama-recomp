# Procura jump tables (bctr) que o cars_switch_tables.toml nao cobre.
# Uso: python -I find_missing_switches.py <basefile.exe> <cars_switch_tables.toml> <saida.toml>
# Padrao principal (tabela absoluta):
#   [cmplwi crN, rI, MAX ; bgt default]   (limite, opcional)
#   lis rA, hi ; addi rA, rA, lo           (endereco da tabela)
#   rlwinm rB, rI, 2, 0, 29                (indice * 4)
#   lwzx rX, rA, rB ; mtctr rX ; bctr
import re
import struct
import sys

BASE = 0x82000000
img = open(sys.argv[1], "rb").read()
toml_text = open(sys.argv[2], encoding="utf-8").read()
existing = set(int(x, 16) for x in re.findall(r"base\s*=\s*0x([0-9A-Fa-f]+)", toml_text))

def word(a):
    o = a - BASE
    if o < 0 or o + 4 > len(img):
        return None
    return struct.unpack(">I", img[o:o + 4])[0]

# Faixas de funcao pelo .pdata (0x82074A00, 0x145F8 bytes): BeginAddress + (comprimento em instrucoes nos bits 8..29)
funcs = []
for off in range(0x74A00, 0x74A00 + 0x145F8, 8):
    b, info = struct.unpack(">II", img[off:off + 8])
    if b == 0:
        continue
    length = ((info >> 8) & 0x3FFFFF) * 4
    funcs.append((b, b + length))
funcs.sort()

def func_of(a):
    lo, hi = 0, len(funcs) - 1
    best = None
    while lo <= hi:
        m = (lo + hi) // 2
        if funcs[m][0] <= a:
            best = funcs[m]
            lo = m + 1
        else:
            hi = m - 1
    if best and best[0] <= a < best[1]:
        return best
    return None

TEXT_LO, TEXT_HI = 0x82090000, 0x8252B4B4
out = []
unknown = []
for a in range(TEXT_LO, TEXT_HI, 4):
    if word(a) != 0x4E800420:      # bctr
        continue
    # ja coberto? (base da tabela existente ate 64 bytes antes do bctr)
    if any((a - d) in existing for d in range(0, 68, 4)):
        continue
    mtctr = word(a - 4)
    if mtctr is None or (mtctr & 0xFC1FFFFF) != 0x7C0903A6:
        unknown.append((a, "sem mtctr antes"))
        continue
    rx = (mtctr >> 21) & 31
    # procura lwzx rX, rA, rB para tras
    rA = rB = None
    lwzx_at = None
    for back in range(8, 48, 4):
        w = word(a - back)
        if w is None:
            break
        if (w >> 26) == 31 and ((w >> 1) & 0x3FF) == 23 and ((w >> 21) & 31) == rx:
            rA, rB = (w >> 16) & 31, (w >> 11) & 31
            lwzx_at = a - back
            break
    if lwzx_at is None:
        unknown.append((a, "bctr sem lwzx (chamada indireta normal?)"))
        continue
    # lis/addi da tabela e rlwinm do indice
    hi = lo = None
    index_reg = None
    for back in range(4, 64, 4):
        w = word(lwzx_at - back)
        if w is None:
            break
        op = w >> 26
        rd, ra = (w >> 21) & 31, (w >> 16) & 31
        simm = struct.unpack(">h", struct.pack(">H", w & 0xFFFF))[0]
        if op == 15 and ra == 0 and rd == rA and hi is None:      # lis rA, hi
            hi = (w & 0xFFFF) << 16
        elif op == 14 and rd == rA and ra == rA and lo is None:   # addi rA, rA, lo
            lo = simm
        elif op == 21 and ra == rB and index_reg is None:         # rlwinm rB, rI, 2, 0, 29
            sh, mb, me = (w >> 11) & 31, (w >> 6) & 31, (w >> 1) & 31
            if sh == 2 and mb == 0 and me == 29:
                index_reg = rd   # campo rS (bits 21..25)
    if hi is None or index_reg is None:
        unknown.append((a, "padrao parcial (lis=%s idx=%s)" % (hi is not None, index_reg)))
        continue
    table = (hi + (lo or 0)) & 0xFFFFFFFF
    # Limite: cmplwi crN, rI, MAX antes
    count = None
    for back in range(4, 96, 4):
        w = word(lwzx_at - back)
        if w is None:
            break
        if (w >> 26) == 10 and ((w >> 16) & 31) == index_reg:   # cmpli
            count = (w & 0xFFFF) + 1
            break
    f = func_of(a)
    labels = []
    limit = count if count else 512
    for i in range(limit):
        t = word(table + i * 4)
        if t is None or not (f and f[0] <= t < f[1]) or (t & 3):
            break
        labels.append(t)
    if not labels or (count and len(labels) != count):
        unknown.append((a, "tabela invalida em %08X (%d de %s entradas)" % (table, len(labels), count)))
        continue
    out.append((a, index_reg, table, labels))

with open(sys.argv[3], "w", encoding="utf-8") as fo:
    fo.write("# Gerado por tools/find_missing_switches.py (tabelas absolutas que faltavam)\n")
    for a, r, table, labels in out:
        fo.write("[[switch]]\nbase = 0x%08X\nr = %d\nlabels = [\n" % (a, r))
        for t in labels:
            fo.write("    0x%08X,\n" % t)
        fo.write("]\n\n")

print("bctr sem tabela reconhecidos: %d" % len(out))
for a, r, table, labels in out[:40]:
    print("  %08X  r%d  tabela %08X  %d entradas" % (a, r, table, len(labels)))
print("nao reconhecidos: %d" % len(unknown))
from collections import Counter
print(Counter(k for _, k in unknown).most_common(10))
for a, k in unknown:
    if 'sem lwzx' not in k:
        print("  %08X  %s" % (a, k))

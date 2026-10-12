# Simboliza as linhas "[host-self] / [host-pilha] ... rva=XXXX" do profiler nativo (CARS_HOST_SAMPLE=1)
# Uso: python -I tools/symbolize.py build/CarsRecomp.map run.err.log [secao]
import bisect
import re
import sys

syms = []
line_re = re.compile(r"^([0-9a-fA-F]{8,16})\s+([0-9a-fA-F]{8})\s+\d+\s+(.*)$")
with open(sys.argv[1], encoding="utf-8", errors="replace") as f:
    for line in f:
        m = line_re.match(line)
        if not m:
            continue
        name = m.group(3).strip()
        if ".obj:(" in name or name.startswith("<") or not name:
            continue
        syms.append((int(m.group(1), 16), name))
syms.sort()
addrs = [a for a, _ in syms]

def sym(rva):
    if rva > 0x40000000:
        return "<fora do exe (DLL do sistema/driver)>"
    i = bisect.bisect_right(addrs, rva) - 1
    if i < 0:
        return "?"
    a, n = syms[i]
    n = re.sub(r"^__declspec\([^)]*\)\s*", "", n)
    n = re.sub(r"\(.*", "", n)   # tira a lista de parametros
    return "%s+0x%X" % (n[-90:], rva - a)

want = sys.argv[3] if len(sys.argv) > 3 and sys.argv[3] != "all" else None
blocks = []
cur = None
with open(sys.argv[2], encoding="utf-8", errors="replace") as f:
    for line in f:
        if line.startswith("[host] "):
            cur = []
            blocks.append((line.strip(), cur))
            continue
        m = re.match(r"^\[host-(\w+)\]\s+([0-9.]+)% rva=([0-9A-Fa-f]+)", line)
        if m and cur is not None:
            cur.append((m.group(1), float(m.group(2)), int(m.group(3), 16)))

# usa o ultimo bloco (ou o de indice pedido)
idx = int(sys.argv[4]) if len(sys.argv) > 4 else -1
title, rows = blocks[idx]
print(title)
# agrega por funcao (self)
for kind in ("self", "pilha"):
    if want and kind != want:
        continue
    agg = {}
    for k, pct, rva in rows:
        if k != kind:
            continue
        name = re.sub(r"\+0x[0-9A-F]+$", "", sym(rva))
        agg[name] = agg.get(name, 0.0) + pct
    print("== %s (por funcao)" % kind)
    for name, pct in sorted(agg.items(), key=lambda x: -x[1])[:35]:
        print("%6.2f%%  %s" % (pct, name))

# Tempo dentro de DLLs (driver, runtime C, ntdll), por DLL e por funcao do exe que chamou
dll_rows = []
bi = -1
with open(sys.argv[2], encoding="utf-8", errors="replace") as f:
    for line in f:
        if line.startswith("[host] "):
            bi += 1
            continue
        m = re.match(r"^\[host-dll\]\s+([0-9.]+)% (\S+) <- rva=([0-9A-Fa-f]+)", line)
        if m and bi == (idx if idx >= 0 else len(blocks) + idx):
            dll_rows.append((float(m.group(1)), m.group(2), int(m.group(3), 16)))
by_dll, by_caller = {}, {}
for pct, dll, rva in dll_rows:
    by_dll[dll] = by_dll.get(dll, 0.0) + pct
    key = dll + " <- " + re.sub(r"\+0x[0-9A-F]+$", "", sym(rva))
    by_caller[key] = by_caller.get(key, 0.0) + pct
print("== DLLs")
for k, v in sorted(by_dll.items(), key=lambda x: -x[1]):
    print("%6.2f%%  %s" % (v, k))
print("== DLL <- funcao do exe")
for k, v in sorted(by_caller.items(), key=lambda x: -x[1])[:25]:
    print("%6.2f%%  %s" % (v, k))

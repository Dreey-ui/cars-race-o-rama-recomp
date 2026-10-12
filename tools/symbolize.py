# Symbolizes the "[host-self] / [host-stack] ... rva=XXXX" lines of the native profiler (CARS_HOST_SAMPLE=1)
# Usage: python -I tools/symbolize.py build/CarsRecomp.map run.err.log [section]
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
        return "<outside the exe (system DLL/driver)>"
    i = bisect.bisect_right(addrs, rva) - 1
    if i < 0:
        return "?"
    a, n = syms[i]
    n = re.sub(r"^__declspec\([^)]*\)\s*", "", n)
    n = re.sub(r"\(.*", "", n)   # drop the parameter list
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

# uses the last block (or the requested index)
idx = int(sys.argv[4]) if len(sys.argv) > 4 else -1
title, rows = blocks[idx]
print(title)
# aggregate by function (self)
for kind in ("self", "stack"):
    if want and kind != want:
        continue
    agg = {}
    for k, pct, rva in rows:
        if k != kind:
            continue
        name = re.sub(r"\+0x[0-9A-F]+$", "", sym(rva))
        agg[name] = agg.get(name, 0.0) + pct
    print("== %s (by function)" % kind)
    for name, pct in sorted(agg.items(), key=lambda x: -x[1])[:35]:
        print("%6.2f%%  %s" % (pct, name))

# Time inside DLLs (driver, C runtime, ntdll), per DLL and per exe function that called it
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
print("== DLL <- exe function")
for k, v in sorted(by_caller.items(), key=lambda x: -x[1])[:25]:
    print("%6.2f%%  %s" % (v, k))

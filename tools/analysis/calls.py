#!/usr/bin/env python3
"""List the calls each function in an address range of a PE makes, with imports named
(through incremental-link thunks) and C++ names demangled.
Usage: calls.py image.exe image.asm lo hi [func ...]
  image.asm: objdump -d --no-show-raw-insn -M intel image.exe > image.asm
  func: only print these function start addresses (hex)"""
import bisect, re, struct, subprocess, sys

E, asm, lo, hi = sys.argv[1], sys.argv[2], int(sys.argv[3], 16), int(sys.argv[4], 16)
only = {int(x, 16) for x in sys.argv[5:]}
d = open(E, 'rb').read()
pe = struct.unpack_from('<I', d, 0x3C)[0]; opt = pe + 24; ib = struct.unpack_from('<Q', d, opt + 24)[0]
ns = struct.unpack_from('<H', d, pe + 6)[0]; so = struct.unpack_from('<H', d, pe + 20)[0]
secs = [struct.unpack_from('<IIII', d, opt + so + i * 40 + 8) for i in range(ns)]
off = lambda r: next(r - v + rp for vs, v, rs, rp in secs if v <= r < v + max(vs, rs))

iat = {}
p = off(struct.unpack_from('<I', d, opt + 112 + 8)[0])
while True:
    ilt, _, _, name, first = struct.unpack_from('<IIIII', d, p); p += 20
    if not name: break
    t = off(ilt or first); i = 0
    while True:
        e = struct.unpack_from('<Q', d, t + 8 * i)[0]
        if not e: break
        if not e >> 63:
            iat[ib + first + 8 * i] = d[off(e) + 2:d.index(b'\0', off(e) + 2)].decode()
        i += 1

prva, psz = struct.unpack_from('<II', d, opt + 112 + 8 * 3); po = off(prva)
funcs = sorted(struct.unpack_from('<II', d, po + 12 * i) for i in range(psz // 12))
fs = [ib + f[0] for f in funcs]

# first instruction of every address, to follow thunks (jmp X / jmp [iat])
first_ins = {}
pat = re.compile(r'\s+([0-9a-f]+):\s+(\S+)\s*(.*)')
calls = {}
for line in open(asm):
    m = pat.match(line)
    if not m: continue
    a = int(m.group(1), 16); op, arg = m.group(2), m.group(3)
    first_ins[a] = (op, arg)
    if lo <= a < hi and op in ('call', 'jmp'):
        i = bisect.bisect_right(fs, a) - 1
        calls.setdefault(fs[i], []).append(arg)

def resolve(arg, depth=0):
    mm = re.search(r'# 0x([0-9a-f]+)', arg)
    if mm and int(mm.group(1), 16) in iat: return iat[int(mm.group(1), 16)]
    if re.fullmatch(r'0x[0-9a-f]+', arg.strip()):
        t = int(arg.strip(), 16)
        if depth < 3 and t in first_ins and first_ins[t][0] == 'jmp':
            r = resolve(first_ins[t][1], depth + 1)
            if not r.startswith('sub_'): return r
            return 'sub_%x' % int(r[4:], 16) if r.startswith('sub_') else r
        return 'sub_%x' % t
    if 'ptr' in arg.lower(): return 'virtual ' + arg.split('PTR')[-1].strip()
    return arg.strip()

def dem(names):
    q = [n for n in names if n.startswith('?')]
    if not q: return {}
    out = subprocess.run(['llvm-undname'], input='\n'.join(q), capture_output=True, text=True).stdout.split('\n')
    res = {}; i = 0
    while i + 1 < len(out):
        if out[i].startswith('?'): res[out[i]] = out[i + 1]; i += 3
        else: i += 1
    return res

for f in sorted(calls):
    if only and f not in only: continue
    j = fs.index(f)
    seen = []
    for c in calls[f]:
        r = resolve(c)
        if r not in seen: seen.append(r)
    dm = dem(seen)
    print(f'== {hex(f)} size {funcs[j][1] - funcs[j][0]}')
    for r in seen: print('    ' + dm.get(r, r)[:160])

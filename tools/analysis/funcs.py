#!/usr/bin/env python3
"""Attribute one module's leaf samples to functions using its .pdata table.
Works for unexported code, where syms.py can only name the nearest export.
Usage: funcs.py maps.txt samples.txt tid Module.dll [top]"""
import bisect, collections, struct, sys

maps, samples, tid_want, modname = sys.argv[1:5]
top = int(sys.argv[5]) if len(sys.argv) > 5 else 20

base = path = None
for line in open(maps):
    p = line.split(None, 5)
    if len(p) == 6 and int(p[2], 16) == 0 and p[5].strip().endswith('/' + modname):
        base, path = int(p[0].split('-')[0], 16), p[5].strip()
        break
d = open(path, 'rb').read()
pe = struct.unpack_from('<I', d, 0x3C)[0]; opt = pe + 24
nsec = struct.unpack_from('<H', d, pe + 6)[0]; sopt = struct.unpack_from('<H', d, pe + 20)[0]
secs = [struct.unpack_from('<IIII', d, opt + sopt + i * 40 + 8) for i in range(nsec)]
def off(rva):
    for vs, va, rs, rp in secs:
        if va <= rva < va + max(vs, rs): return rva - va + rp
size = struct.unpack_from('<I', d, opt + 56)[0]
prva, psz = struct.unpack_from('<II', d, opt + 112 + 8 * 3)
po = off(prva)
funcs = sorted(struct.unpack_from('<III', d, po + 12 * i)[:2] for i in range(psz // 12))
starts = [f[0] for f in funcs]

# exports, to name functions when possible
names = {}
erva = struct.unpack_from('<I', d, opt + 112)[0]
if erva:
    e = off(erva)
    nf, nn, af, an, ao = struct.unpack_from('<IIIII', d, e + 20)
    fa = struct.unpack_from(f'<{nf}I', d, off(af))
    for i in range(nn):
        nr = struct.unpack_from('<I', d, off(an) + 4 * i)[0]; o = struct.unpack_from('<H', d, off(ao) + 2 * i)[0]
        names[fa[o]] = d[off(nr):d.index(b'\0', off(nr))].decode(errors='replace')

cnt = collections.Counter(); tot = 0; modtot = 0
tid = None; leaf = None
def flush():
    global tot, modtot
    if tid != tid_want or leaf is None: return
    tot += 1
    r = leaf - base
    if not (0 <= r < size): return
    modtot += 1
    i = bisect.bisect_right(starts, r) - 1
    if i >= 0 and r < funcs[i][1]: cnt[funcs[i]] += 1
    else: cnt[('?', r & ~0xff)] += 1
for line in open(samples):
    if line.startswith('\t'):
        if leaf is None:
            s = line.split()
            if s: leaf = int(s[0], 16)
        continue
    flush()
    s = line.split(); tid = s[0] if s else None
    leaf = int(s[1], 16) if len(s) > 1 else None
flush()
print(f'{modname}: {modtot} of {tot} samples on tid {tid_want} ({100*modtot/max(tot,1):.1f}%)')
for f, n in cnt.most_common(top):
    if f[0] == '?':
        print(f'{100*n/tot:6.2f}%  (no pdata) near {hex(f[1])}')
    else:
        print(f'{100*n/tot:6.2f}%  {hex(f[0])}-{hex(f[1])} ({f[1]-f[0]} B)  {names.get(f[0], "")[:90]}')

#!/usr/bin/env python3
"""Per-frame diff of two main-thread captures by module and by OgreMain export.
Usage: diff.py A.maps A.txt B.maps B.txt tid frames_ratio_B_over_A
Counts are normalised to samples per A-frame (B divided by the ratio)."""
import bisect, collections, struct, sys
def load(maps, samples, tid):
    mods = {}; exp = {}
    for line in open(maps):
        p = line.split(None, 5)
        if len(p) == 6 and int(p[2], 16) == 0 and p[5].strip().lower().endswith(('.dll', '.exe')):
            path = p[5].strip(); n = path.rsplit('/', 1)[-1]
            if n in mods: continue
            d = open(path, 'rb').read(); pe = struct.unpack_from('<I', d, 0x3C)[0]; opt = pe + 24
            lo = int(p[0].split('-')[0], 16); mods[n] = (lo, lo + struct.unpack_from('<I', d, opt + 56)[0])
            if n in ('OgreMain_x64.dll', 'Plugin_ParticleUniverse_x64.dll'):
                ns = struct.unpack_from('<H', d, pe + 6)[0]; so = struct.unpack_from('<H', d, pe + 20)[0]
                secs = [struct.unpack_from('<IIII', d, opt + so + i*40 + 8) for i in range(ns)]
                off = lambda r: next(r - va + rp for vs, va, rs, rp in secs if va <= r < va + max(vs, rs))
                er = struct.unpack_from('<I', d, opt + 112)[0]; e = off(er)
                nf, nn, af, an, ao = struct.unpack_from('<IIIII', d, e + 20)
                fa = struct.unpack_from(f'<{nf}I', d, off(af)); L = []
                for i in range(nn):
                    r = struct.unpack_from('<I', d, off(an) + 4*i)[0]; o = struct.unpack_from('<H', d, off(ao) + 2*i)[0]
                    L.append((fa[o], d[off(r):d.index(b'\0', off(r))].decode(errors='replace')))
                L.sort(); exp[n] = (lo, [a for a, _ in L], [b for _, b in L])
    m = collections.Counter(); f = collections.Counter(); t = None; leaf = None
    def flush():
        if t != tid or leaf is None: return
        for n, (lo, hi) in mods.items():
            if lo <= leaf < hi:
                m[n] += 1
                if n in exp:
                    b, rv, nm = exp[n]; i = bisect.bisect_right(rv, leaf - b) - 1
                    f[(n[:10], nm[i] if i >= 0 else '?')] += 1
                return
        m['[other]'] += 1
    for line in open(samples):
        if line.startswith('\t'):
            if leaf is None:
                s = line.split(); leaf = int(s[0], 16) if s else None
            continue
        flush(); s = line.split(); t = s[0] if s else None; leaf = int(s[1], 16) if len(s) > 1 else None
    flush()
    return m, f
am, af = load(sys.argv[1], sys.argv[2], sys.argv[5]); bm, bf = load(sys.argv[3], sys.argv[4], sys.argv[5])
r = float(sys.argv[6]); ta = sum(am.values()); tb = sum(bm.values()) / r
print(f'per-frame work: A {ta:.0f}  B {tb:.0f}  saved {ta-tb:.0f} ({100*(ta-tb)/ta:.1f}% of A)\n')
print('module                              A      B   saved  %ofSaved')
for n in sorted(set(am) | set(bm), key=lambda n: -(am[n] - bm[n]/r)):
    s = am[n] - bm[n]/r
    if abs(s) > 0.003*ta: print(f'{n[:32]:32} {am[n]:6.0f} {bm[n]/r:6.0f} {s:7.0f}  {100*s/(ta-tb):6.1f}%')
print('\nexported functions (OgreMain/ParticleUniverse)  saved  %ofSaved')
for k in sorted(set(af) | set(bf), key=lambda k: -(af[k] - bf[k]/r))[:22]:
    s = af[k] - bf[k]/r
    print(f'{s:6.0f} {100*s/(ta-tb):6.1f}%  {k[0]} {k[1][:95]}')

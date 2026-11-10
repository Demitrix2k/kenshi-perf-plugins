import bisect, collections, struct, sys, subprocess
maps, samples, tid_want = sys.argv[1], sys.argv[2], sys.argv[3]
targets = {}
for line in open(maps):
    p = line.split(None, 5)
    if len(p) == 6 and int(p[2], 16) == 0 and p[5].strip().endswith(('MyGUIEngine_x64.dll', 'OgreMain_x64.dll', 'RenderSystem_Direct3D11_x64.dll')):
        targets.setdefault(p[5].strip(), int(p[0].split('-')[0], 16))
def exports(path):
    d = open(path, 'rb').read()
    pe = struct.unpack_from('<I', d, 0x3C)[0]; opt = pe + 24
    nsec = struct.unpack_from('<H', d, pe + 6)[0]; sopt = struct.unpack_from('<H', d, pe + 20)[0]
    secs = [struct.unpack_from('<IIII', d, opt + sopt + i*40 + 8) for i in range(nsec)]  # vsize, va, rawsize, rawptr
    def off(rva):
        for vs, va, rs, rp in secs:
            if va <= rva < va + max(vs, rs): return rva - va + rp
    erva = struct.unpack_from('<I', d, opt + 112)[0]; e = off(erva)
    nf, nn, af, an, ao = struct.unpack_from('<IIIII', d, e + 20)
    funcs = struct.unpack_from(f'<{nf}I', d, off(af))
    out = []
    for i in range(nn):
        nrva = struct.unpack_from('<I', d, off(an) + 4*i)[0]; o = struct.unpack_from('<H', d, off(ao) + 2*i)[0]
        n = d[off(nrva):d.index(b'\0', off(nrva))].decode(errors='replace'); out.append((funcs[o], n))
    out.sort(); return out
tabs = {}
for path, base in targets.items():
    ex = exports(path); tabs[path.rsplit('/',1)[-1]] = (base, [r for r, _ in ex], [n for _, n in ex])
# Recompute module ranges from header base + SizeOfImage
def size(path):
    d = open(path,'rb').read(4096); pe = struct.unpack_from('<I', d, 0x3C)[0]; return struct.unpack_from('<I', d, pe+24+56)[0]
ranges = [(b, b + size(p), p.rsplit('/',1)[-1]) for p, b in targets.items()]
cnt = collections.Counter(); tot = 0
tid = leaf = None
def flush():
    global tot
    if tid != tid_want or leaf is None: return
    tot += 1
    for lo, hi, n in ranges:
        if lo <= leaf < hi:
            base, rvas, names = tabs[n]; i = bisect.bisect_right(rvas, leaf - base) - 1
            cnt[(n, names[i] if i >= 0 else '?')] += 1
for line in open(samples):
    s = line.strip()
    if not s: flush(); tid = leaf = None; continue
    if not line.startswith('\t'):
        flush(); b = s.split(); tid = b[0]; leaf = int(b[1], 16) if len(b) > 1 else None
    elif leaf is None: leaf = int(s.split()[0], 16)
flush()
names = [k[1] for k, _ in cnt.most_common(30)]
dem = subprocess.run(['c++filt', '-_'], input='\n'.join(names), capture_output=True, text=True).stdout.split('\n') if False else names
for (mod, n), c in cnt.most_common(30):
    print(f'{100*c/tot:5.2f}%  {mod[:12]:12}  {n[:110]}')

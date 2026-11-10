import sys, collections, struct
maps, samples, tid_want, modname = sys.argv[1:5]
mods = {}
for line in open(maps):
    p = line.split(None, 5)
    if len(p) == 6 and int(p[2], 16) == 0 and p[5].strip().lower().endswith(('.dll', '.exe')):
        path = p[5].strip(); n = path.rsplit('/', 1)[-1]
        if n in mods: continue
        try:
            d = open(path, 'rb').read(4096); pe = struct.unpack_from('<I', d, 0x3C)[0]
            lo = int(p[0].split('-')[0], 16); mods[n] = (lo, lo + struct.unpack_from('<I', d, pe+24+56)[0])
        except Exception: pass
def which(a):
    for n, (lo, hi) in mods.items():
        if lo <= a < hi: return n, a - lo
    return None, a
lo, hi = mods[modname]
buckets, callers = collections.Counter(), collections.Counter(); tot = 0
def flush(chain):
    global tot
    if not chain: return
    n, off = which(chain[0])
    if n != modname: return
    tot += 1; buckets[off & ~0x3f] += 1
    for a in chain[1:]:
        cn, coff = which(a)
        if cn != modname:
            callers[(cn, hex(coff) if cn else '?')] += 1; break
tid, chain = None, []
for line in open(samples):
    if line.startswith('\t'):
        s = line.split()
        if s: chain.append(int(s[0], 16))
        continue
    if tid == tid_want: flush(chain)
    s = line.split(); tid = s[0] if s else None; chain = [int(s[1], 16)] if len(s) > 1 else []
if tid == tid_want: flush(chain)
print(f'{modname} leaf samples on tid {tid_want}: {tot}')
print('hot RVAs (64B buckets):'); [print(f'  {hex(k)}  {v}') for k, v in buckets.most_common(10)]
print('first caller outside module:'); [print(f'  {v:6}  {k}') for k, v in callers.most_common(10)]

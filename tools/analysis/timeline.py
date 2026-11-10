import sys, collections, struct, bisect
maps, samples, tid_want = sys.argv[1:4]
R=[]; seen=set()
for line in open(maps):
    p=line.split(None,5)
    if len(p)<6: continue
    lo,hi=(int(x,16) for x in p[0].split('-')); path=p[5].strip(); n=path.rsplit('/',1)[-1]
    if path.lower().endswith(('.dll','.exe')) and int(p[2],16)==0 and path not in seen:
        try:
            d=open(path,'rb').read(4096); pe=struct.unpack_from('<I',d,0x3C)[0]
            R.append((lo,lo+struct.unpack_from('<I',d,pe+24+56)[0],n)); seen.add(path); continue
        except Exception: pass
    if path and not path.startswith('['): R.append((lo,hi,n))
R.sort(); S=[r[0] for r in R]
def mod(a):
    if a>=0xffff800000000000: return '[kernel]'
    i=bisect.bisect_right(S,a)-1
    return R[i][2] if i>=0 and a<R[i][1] else '[anon]'
rows=[]; tid=t=None; chain=[]
def flush():
    if tid==tid_want and chain: rows.append((t, mod(chain[0])))
for line in open(samples):
    if line.startswith('\t'):
        s=line.split()
        if s: chain.append(int(s[0],16))
        continue
    flush(); s=line.split(); chain=[]
    if len(s)>=2: tid=s[0]; t=float(s[1].rstrip(':'))
flush()
t0=rows[0][0]; sec=collections.defaultdict(collections.Counter)
for t,m in rows: sec[int(t-t0)][m]+=1
keys=['MyGUIEngine_x64.dll','OgreMain_x64.dll','RenderSystem_Direct3D11_x64.dll','d3d11.dll','Kenshi_x64.exe','Plugin_ParticleUniverse_x64.dll','msvcr100.dll','[kernel]']
print('sec total  '+'  '.join(k[:8] for k in keys))
for s in sorted(sec):
    c=sec[s]; print(f'{s:3} {sum(c.values()):5}  '+'  '.join(f'{c[k]:8}' for k in keys))

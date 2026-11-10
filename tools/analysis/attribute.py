#!/usr/bin/env python3
"""Attribute perf samples to modules for a Wine process.
Usage: attribute.py maps.txt samples.txt [main_tid]
PE images have anonymous code sections under Wine, so ranges are rebuilt
from each image's header mapping + SizeOfImage."""
import bisect, collections, struct, sys

maps_path, samples_path = sys.argv[1], sys.argv[2]
main_tid = sys.argv[3] if len(sys.argv) > 3 else None

ranges = []  # (start, end, name)
seen_pe = set()
for line in open(maps_path):
    parts = line.split(None, 5)
    if len(parts) < 6:
        continue
    lo, hi = (int(x, 16) for x in parts[0].split('-'))
    path = parts[5].strip()
    name = path.rsplit('/', 1)[-1]
    if path.lower().endswith(('.dll', '.exe', '.asi')) and int(parts[2], 16) == 0 and path not in seen_pe:
        try:
            with open(path, 'rb') as f:
                hdr = f.read(4096)
            pe = struct.unpack_from('<I', hdr, 0x3C)[0]
            size = struct.unpack_from('<I', hdr, pe + 24 + 56)[0]  # SizeOfImage
            ranges.append((lo, lo + size, name))
            seen_pe.add(path)
            continue
        except Exception:
            pass
    if path and not path.startswith('['):
        ranges.append((lo, hi, name))
ranges.sort()
starts = [r[0] for r in ranges]

def mod(ip):
    if ip >= 0xffff800000000000:
        return '[kernel]'
    i = bisect.bisect_right(starts, ip) - 1
    if i >= 0 and ip < ranges[i][1]:
        return ranges[i][2]
    return '[anon/unknown]'

# perf script with callchains: header line "tid", then indented ips, blank line between samples
by_mod, by_tid, main_mod = collections.Counter(), collections.Counter(), collections.Counter()
tid, leaf, total = None, None, 0
def flush():
    global total
    if tid is None or leaf is None:
        return
    m = mod(leaf)
    by_mod[m] += 1; by_tid[tid] += 1; total += 1
    if tid == main_tid:
        main_mod[m] += 1
for line in open(samples_path):
    s = line.strip()
    if not s:
        flush(); tid, leaf = None, None
        continue
    if not line.startswith('\t') and not line.startswith('    '):
        flush(); bits = s.split(); tid = bits[0]; leaf = int(bits[1], 16) if len(bits) > 1 else None
    elif leaf is None:
        leaf = int(s.split()[0], 16)
flush()

print(f'total samples: {total}')
print('\n-- top threads --')
for t, n in by_tid.most_common(6):
    print(f'{100*n/total:6.2f}%  tid {t}')
print('\n-- all threads, by module --')
for m, n in by_mod.most_common(20):
    print(f'{100*n/total:6.2f}%  {m}')
if main_tid:
    mt = sum(main_mod.values())
    print(f'\n-- tid {main_tid} only ({mt} samples), by module --')
    for m, n in main_mod.most_common(20):
        print(f'{100*n/mt:6.2f}%  {m}')

#!/usr/bin/env python3
"""Count how often code in the running game executes, per frame, with no plugin.

Uses CPU hardware breakpoints through perf (`mem:ADDR:xu`): every execution of the
address is counted exactly, without stopping the game. The Linux equivalent of
x64dbg's non-halting counting breakpoints. One extra counter on Ogre's
Root::_fireFrameEnded gives the frame count, so results come out as calls/frame.

Usage: bpcount.py [-s SECONDS] TARGET [TARGET ...]   (at most 3 targets)
  TARGET = Module.dll:0xRVA  |  Module.dll:ExportName  |  kenshi_x64.exe:0xRVA
  e.g. bpcount.py OgreMain_x64.dll:0xc1620 MyGUIEngine_x64.dll:0x154980

Limits: x86 has 4 debug registers (1 used for the frame counter). Each hit costs a
debug trap (~1-3 us under Wine), so counting something that runs 10,000+ times per
frame slows the game noticeably while it runs; keep runs short and don't measure
FPS at the same time.
"""
import os, re, struct, subprocess, sys

FRAME = ('OgreMain_x64.dll', '?_fireFrameEnded@Root@Ogre@@QEAA_NXZ')


def pid_of_game():
    out = subprocess.run(['pgrep', '-x', 'kenshi_x64.exe'], capture_output=True, text=True).stdout.split()
    if not out:
        sys.exit('kenshi_x64.exe is not running')
    # the RE_Kenshi game process is the one with OgreMain mapped
    for p in out:
        try:
            if 'OgreMain_x64.dll' in open(f'/proc/{p}/maps').read():
                return int(p)
        except OSError:
            pass
    sys.exit('no kenshi_x64.exe process has OgreMain_x64.dll mapped')


def module_base(pid, mod):
    for line in open(f'/proc/{pid}/maps'):
        p = line.split(None, 5)
        if len(p) == 6 and int(p[2], 16) == 0 and p[5].strip().lower().endswith('/' + mod.lower()):
            return int(p[0].split('-')[0], 16), p[5].strip()
    sys.exit(f'{mod} not mapped in pid {pid}')


def export_rva(path, name):
    d = open(path, 'rb').read()
    pe = struct.unpack_from('<I', d, 0x3C)[0]; opt = pe + 24
    ns = struct.unpack_from('<H', d, pe + 6)[0]; so = struct.unpack_from('<H', d, pe + 20)[0]
    secs = [struct.unpack_from('<IIII', d, opt + so + i * 40 + 8) for i in range(ns)]
    off = lambda r: next(r - v + rp for vs, v, rs, rp in secs if v <= r < v + max(vs, rs))
    e = off(struct.unpack_from('<I', d, opt + 112)[0])
    nf, nn, af, an, ao = struct.unpack_from('<IIIII', d, e + 20)
    for i in range(nn):
        nr = struct.unpack_from('<I', d, off(an) + 4 * i)[0]
        if d[off(nr):d.index(b'\0', off(nr))].decode(errors='replace') == name:
            o = struct.unpack_from('<H', d, off(ao) + 2 * i)[0]
            return struct.unpack_from('<I', d, off(af) + 4 * o)[0]
    sys.exit(f'export {name} not found in {path}')


def resolve(pid, target):
    mod, _, what = target.partition(':')
    base, path = module_base(pid, mod)
    rva = int(what, 16) if re.fullmatch(r'0x[0-9a-fA-F]+', what) else export_rva(path, what)
    return base + rva


def main():
    args = sys.argv[1:]
    secs = 10
    if args[:1] == ['-s']:
        secs = float(args[1]); args = args[2:]
    if not 1 <= len(args) <= 3:
        sys.exit(__doc__)
    pid = pid_of_game()
    addrs = [resolve(pid, FRAME[0] + ':' + FRAME[1])] + [resolve(pid, t) for t in args]
    names = ['frames'] + args
    events = ','.join(f'mem:{a:#x}:xu' for a in addrs)
    r = subprocess.run(['perf', 'stat', '-x', ',', '-e', events, '-p', str(pid), '--', 'sleep', str(secs)],
                       capture_output=True, text=True)
    counts = []
    for line in r.stderr.splitlines():
        f = line.split(',')
        if len(f) > 2 and f[2].startswith('mem:'):
            counts.append(int(f[0]) if f[0].isdigit() else None)
    if len(counts) != len(addrs):
        sys.exit('perf failed:\n' + r.stderr)
    frames = counts[0] or 0
    print(f'pid {pid}, {secs:g} s, {frames} frames ({frames / secs:.1f} fps while counting)')
    for n, a, c in zip(names[1:], addrs[1:], counts[1:]):
        per = f'{c / frames:.1f}/frame' if frames and c is not None else '-'
        print(f'  {n:45s} {a:#x}  {c}  {per}')


if __name__ == '__main__':
    main()

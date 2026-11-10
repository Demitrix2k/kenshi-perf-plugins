#!/usr/bin/env python3
"""Symbolize and summarize a StackProbe.log run (real main-thread call stacks).

Usage: stacksym.py StackProbe.log [--focus SUBSTR ...] [--depth N] [--top N]
  Prints: self time by function (leaf), inclusive time by function (on the stack),
  and the hottest caller chains for samples whose leaf matches each --focus substring
  (module or function name; default: msvcr100, ntdll, PhysX, NxCharacter).
Functions are named from each module's exports (demangled with llvm-undname when
available) or as module!sub_RVA, using the .pdata function start.
"""
import bisect, collections, os, re, struct, subprocess, sys

KENSHI = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
# RE_Kenshi's Kenshi_x64.exe (the running exe) must win over Steam's kenshi_x64.exe.
SEARCH = [
    os.path.join(KENSHI, 'RE_Kenshi'), KENSHI,
    os.path.join(os.path.dirname(os.path.dirname(KENSHI)), 'compatdata/233860/pfx/drive_c/windows/system32'),
    os.path.expanduser('~/.local/share/Steam/steamapps/common/Proton - Experimental/files/lib/wine/x86_64-windows'),
]


class Image:
    def __init__(self, path):
        d = open(path, 'rb').read()
        pe = struct.unpack_from('<I', d, 0x3C)[0]; opt = pe + 24
        ns = struct.unpack_from('<H', d, pe + 6)[0]; so = struct.unpack_from('<H', d, pe + 20)[0]
        secs = [struct.unpack_from('<IIII', d, opt + so + i * 40 + 8) for i in range(ns)]
        def off(r):
            for vs, va, rs, rp in secs:
                if va <= r < va + max(vs, rs):
                    return r - va + rp
            return None
        prva, psz = struct.unpack_from('<II', d, opt + 112 + 8 * 3)
        po = off(prva) if prva else None
        self.funcs = sorted(struct.unpack_from('<II', d, po + 12 * i) for i in range(psz // 12)) if po is not None else []
        self.starts = [f[0] for f in self.funcs]
        self.exports = {}
        erva = struct.unpack_from('<I', d, opt + 112)[0]
        if erva and off(erva) is not None:
            e = off(erva)
            nf, nn, af, an, ao = struct.unpack_from('<IIIII', d, e + 20)
            for i in range(nn):
                nr = struct.unpack_from('<I', d, off(an) + 4 * i)[0]
                o = struct.unpack_from('<H', d, off(ao) + 2 * i)[0]
                rva = struct.unpack_from('<I', d, off(af) + 4 * o)[0]
                name = d[off(nr):d.index(b'\0', off(nr))].decode(errors='replace')
                self.exports.setdefault(rva, name)
        self.exp_sorted = sorted(self.exports)

    def func_start(self, rva):
        i = bisect.bisect_right(self.starts, rva) - 1
        if i >= 0 and self.funcs[i][0] <= rva < self.funcs[i][1]:
            return self.funcs[i][0]
        return None


_images, _demangled = {}, {}


def image(mod):
    if mod not in _images:
        _images[mod] = None
        for d in SEARCH:
            try:
                names = {f.lower(): f for f in os.listdir(d)}  # case-insensitive, like Windows
            except OSError:
                continue
            for cand in (names.get(mod.lower()),):
                if not cand:
                    continue
                p = os.path.join(d, cand)
                if os.path.exists(p):
                    try:
                        _images[mod] = Image(p)
                    except Exception:
                        pass
                    break
            if _images[mod]:
                break
    return _images[mod]


def demangle(names):
    todo = [n for n in names if n.startswith('?') and n not in _demangled]
    if todo:
        try:
            out = subprocess.run(['llvm-undname'], input='\n'.join(todo) + '\n', capture_output=True, text=True).stdout
            res = [l for l in out.splitlines() if l.strip()]
            pairs = [(res[i], res[i + 1]) for i in range(0, len(res) - 1, 2)]
            for mangled, plain in pairs:
                plain = re.sub(r'\b(public|private|protected): |\b__cdecl |\b(class|struct) |\bvirtual |\bstatic ', '', plain)
                plain = plain.replace('std::basic_string<char, std::char_traits<char>, std::allocator<char>>', 'String')
                _demangled[mangled] = plain
        except FileNotFoundError:
            pass
    return [_demangled.get(n, n) for n in names]


def func_name(addr):
    mod, _, off = addr.partition('+')
    if not off:
        return addr
    rva = int(off, 16)
    img = image(mod)
    if not img:
        return f'{mod}!{rva:#x}'
    start = img.func_start(rva)
    if start is not None and start in img.exports:
        return f'{mod}!{img.exports[start]}'
    if start is None:  # no unwind data: nearest export
        i = bisect.bisect_right(img.exp_sorted, rva) - 1
        if i >= 0:
            e = img.exp_sorted[i]
            return f'{mod}!{img.exports[e]}+{rva - e:#x}'
        return f'{mod}!{rva:#x}'
    return f'{mod}!sub_{start:x}'


def pretty(name):
    mod, _, fn = name.partition('!')
    if fn.startswith('?'):
        base = fn.split('+')[0]
        fn = demangle([base])[0] + fn[len(base):]
    short = mod.replace('_x64.dll', '').replace('.dll', '').replace('.exe', '')
    return f'{short}!{fn[:110]}'


def main():
    args = sys.argv[1:]
    if not args:
        sys.exit(__doc__)
    path, focus, depth, top = args[0], [], 6, 25
    i = 1
    while i < len(args):
        if args[i] == '--focus': focus.append(args[i + 1]); i += 2
        elif args[i] == '--depth': depth = int(args[i + 1]); i += 2
        elif args[i] == '--top': top = int(args[i + 1]); i += 2
        else: i += 1
    focus = focus or ['msvcr100', 'ntdll', 'PhysX', 'NxCharacter']
    lines = open(path, errors='replace').read().splitlines()
    starts = [k for k, l in enumerate(lines) if l.startswith('chains (')]
    if not starts:
        sys.exit('no completed run in the log')
    k = starts[-1] + 1  # last run
    header = next((l for l in reversed(lines[:k]) if ' run: ' in l), '')
    chains = []
    while k < len(lines) and lines[k] != 'end of run':
        cnt, _, rest = lines[k].partition('|')
        chains.append((int(cnt), [func_name(a) for a in rest.split('>')]))
        k += 1
    total = sum(c for c, _ in chains)
    print(header.strip())
    print(f'{total} samples, {len(chains)} distinct stacks\n')
    selfc, incl = collections.Counter(), collections.Counter()
    for c, fr in chains:
        selfc[fr[0]] += c
        for f in set(fr):
            incl[f] += c
    print('Self time (leaf function):')
    for f, c in selfc.most_common(top):
        print(f'  {100 * c / total:5.1f}%  {pretty(f)}')
    print('\nInclusive time (function anywhere on the stack):')
    for f, c in incl.most_common(top + 15):
        print(f'  {100 * c / total:5.1f}%  {pretty(f)}')
    for fx in focus:
        sel = [(c, fr) for c, fr in chains if fx.lower() in fr[0].lower()]
        n = sum(c for c, _ in sel)
        if not n:
            continue
        print(f'\nLeaf in "{fx}": {100 * n / total:.1f}% of samples; hottest callers (first non-{fx} frames):')
        agg = collections.Counter()
        for c, fr in sel:
            j = 0
            while j < len(fr) and fx.lower() in fr[j].lower():
                j += 1
            agg[tuple(fr[j:j + depth])] += c
        for key, c in agg.most_common(12):
            print(f'  {100 * c / total:5.1f}%  ' + ' < '.join(pretty(f) for f in key))


if __name__ == '__main__':
    main()

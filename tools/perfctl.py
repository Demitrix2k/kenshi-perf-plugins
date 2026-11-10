#!/usr/bin/env python3
"""Control panel for our Kenshi plugins (no in-game hotkeys needed).

The plugins read perfctl.ini in the Kenshi folder 4 times a second and report what
they applied in perfctl-status-<plugin>.txt. This script edits that file.

  perfctl.py                      interactive panel (arrows, space/enter, +/-, q)
  perfctl.py show                 requested vs applied values
  perfctl.py set rqtrim=1 ...     set values
  perfctl.py ab KEY [A B] [--at 10,20] [--label NAME]
                                  scripted A/B: sets KEY=A, waits for you to start a
                                  MangoHud log (Shift+F2), switches to B at +10 s and
                                  back to A at +20 s, then files and analyzes the log
  perfctl.py analyze LOG.csv [--at 10,20]
                                  frame-time stats per segment of a MangoHud log
"""
import csv, glob, os, shutil, sys, time

HERE = os.path.dirname(os.path.abspath(__file__))
KENSHI = os.path.dirname(HERE)
INI = os.path.join(KENSHI, 'perfctl.ini')
MANGO_DIR = os.path.join(HERE, 'mangohud')
MARKS = os.path.join(HERE, 'captures', 'ab-marks.log')

# key, label, min, max (bool = 0..1), plugin status file
CONTROLS = [
    ('quadfix', 'MyGUI Quad Fix', 0, 1, 'quadfix'),
    ('shadow_interval', 'Shadow Throttle interval (1 = off)', 1, 6, 'shadow'),
    ('shadow_correct', 'Shadow matrix correction', 0, 1, 'shadow'),
    ('rqtrim', 'Render Queue Trim', 0, 1, 'rqtrim'),
    ('uiguard', 'MyGUI Layout Guard (0 = count only)', 0, 1, 'uiguard'),
    ('shadercache', 'Shader Const Cache', 0, 1, 'shadercache'),
    ('stackprobe', 'StackProbe: 1 = sample 10 s now', 0, 1, 'stackprobe'),
]
KEYS = {c[0]: c for c in CONTROLS}


def parse_kv(text):
    out = {}
    for line in text.splitlines():
        line = line.split('#', 1)[0].strip()
        if '=' in line:
            k, v = (s.strip() for s in line.split('=', 1))
            try:
                out[k] = int(v)
            except ValueError:
                pass
    return out


def read_ini():
    try:
        return parse_kv(open(INI).read())
    except OSError:
        return {}


def write_ini(values):
    lines = ['# Kenshi plugin controls, read by the plugins 4x/s. Edit with tools/perfctl.py']
    for k, label, *_ in CONTROLS:
        if k in values:
            lines.append(f'{k}={values[k]}')
    for k, v in values.items():
        if k not in KEYS:
            lines.append(f'{k}={v}')
    tmp = INI + '.tmp'
    with open(tmp, 'w', newline='\r\n') as f:
        f.write('\n'.join(lines) + '\n')
    os.replace(tmp, INI)


def read_status():
    st = {}
    for path in glob.glob(os.path.join(KENSHI, 'perfctl-status-*.txt')):
        try:
            st.update(parse_kv(open(path).read()))
        except OSError:
            pass
    return st


def game_running():
    return os.system('pgrep -x kenshi_x64.exe >/dev/null') == 0


def set_values(changes, wait=1.5):
    vals = read_ini()
    vals.update(changes)
    write_ini(vals)
    if not game_running():
        return True
    deadline = time.time() + wait
    while time.time() < deadline:
        st = read_status()
        if all(st.get(k) == v for k, v in changes.items()):
            return True
        time.sleep(0.05)
    return False


def show():
    vals, st = read_ini(), read_status()
    print(f'{"control":38s} {"requested":>9s} {"applied":>8s}')
    for k, label, *_ in CONTROLS:
        print(f'{label:38s} {str(vals.get(k, "-")):>9s} {str(st.get(k, "-")):>8s}   ({k})')
    if not game_running():
        print('(game not running: "applied" is from the last session)')


# ---------------------------------------------------------------- MangoHud logs

def mango_output_dir():
    for conf in glob.glob(os.path.expanduser('~/.config/MangoHud/*.conf')):
        for line in open(conf):
            if line.strip().startswith('output_folder='):
                return os.path.expanduser(line.split('=', 1)[1].strip())
    return os.path.expanduser('~')


def load_log(path):
    rows = list(csv.reader(open(path)))
    hdr = rows[2]
    i_ft, i_el = hdr.index('frametime'), hdr.index('elapsed')
    data = [(int(r[i_el]) / 1e9, float(r[i_ft])) for r in rows[3:] if len(r) > i_el]
    t0 = data[0][0]
    return [(t - t0, f) for t, f in data]


def analyze(path, at, margin=0.7):
    data = load_log(path)
    end = data[-1][0]
    edges = [0.0] + list(at) + [end + 1]
    print(f'{os.path.basename(path)}: {end:.1f} s')
    out = []
    for i in range(len(edges) - 1):
        a = edges[i] + (margin if i else 0.5)
        b = edges[i + 1] - margin
        ft = sorted(f for t, f in data if a <= t < b)
        if not ft:
            continue
        mean = sum(ft) / len(ft)
        med = ft[len(ft) // 2]
        p95 = ft[min(len(ft) - 1, int(len(ft) * 0.95))]
        p99 = ft[min(len(ft) - 1, int(len(ft) * 0.99))]
        out.append((i, mean, med))
        print(f'  segment {i + 1} ({edges[i]:.0f}-{min(edges[i + 1], end):.0f} s): '
              f'mean {mean:.2f} ms, median {med:.2f} ms, p95 {p95:.2f} ms, p99 {p99:.2f} ms, '
              f'max {ft[-1]:.2f} ms, {1000 / mean:.1f} fps')
    return out


def ab(key, a, b, at, label):
    if key not in KEYS:
        sys.exit(f'unknown key {key}; known: {", ".join(KEYS)}')
    if not game_running():
        sys.exit('Kenshi is not running')
    if not set_values({key: a}):
        sys.exit(f'{key}={a} was not applied within 1.5 s (is the plugin loaded? see its log)')
    outdir = mango_output_dir()
    before = set(glob.glob(os.path.join(outdir, '*.csv')))
    print(f'{key}={a} applied. Start the MangoHud log now (Shift+F2) and keep the camera still...')
    while True:
        new = [p for p in glob.glob(os.path.join(outdir, '*.csv'))
               if p not in before and not p.endswith('_summary.csv')]
        if new:
            log = new[0]
            break
        time.sleep(0.05)
    t0 = time.time()
    print(f'log started: {os.path.basename(log)}')
    marks = []
    seq = [(at[0], b), (at[1], a)] if len(at) > 1 else [(at[0], b)]
    for when, val in seq:
        time.sleep(max(0, t0 + when - time.time()))
        ok = set_values({key: val})
        marks.append((time.time() - t0, val, ok))
        print(f'  +{marks[-1][0]:.1f} s: {key}={val}' + ('' if ok else '  (NOT confirmed by plugin!)'))
    print('waiting for the log to finish...')
    summary = log[:-4] + '_summary.csv'
    last = -1
    while not os.path.exists(summary):
        time.sleep(0.5)
        size = os.path.getsize(log)
        if size == last and time.time() - t0 > at[-1] + 15:
            break  # no summary but the log stopped growing
        last = size
    time.sleep(0.5)
    name = f'{label}_' if label else f'ab-{key}_'
    dest = os.path.join(MANGO_DIR, name + os.path.basename(log))
    os.makedirs(MANGO_DIR, exist_ok=True)
    shutil.move(log, dest)  # the MangoHud folder may be on another filesystem
    if os.path.exists(summary):
        shutil.move(summary, os.path.join(MANGO_DIR, name + os.path.basename(summary)))
    os.makedirs(os.path.dirname(MARKS), exist_ok=True)
    with open(MARKS, 'a') as f:
        f.write(f'{time.strftime("%Y-%m-%d %H:%M:%S", time.localtime(t0))} {os.path.basename(dest)} '
                f'{key} start={a} ' + ' '.join(f'+{t:.2f}s={v}{"" if ok else "?"}' for t, v, ok in marks) + '\n')
    print(f'filed as mangohud/{os.path.basename(dest)}')
    segs = analyze(dest, [t for t, _, _ in marks])
    if len(segs) >= 2:
        on_a = [s for s in segs if s[0] % 2 == 0]
        on_b = [s for s in segs if s[0] % 2 == 1]
        ma = sum(s[1] for s in on_a) / len(on_a)
        mb = sum(s[1] for s in on_b) / len(on_b)
        print(f'  {key}={a}: mean {ma:.2f} ms   {key}={b}: mean {mb:.2f} ms   difference {mb - ma:+.2f} ms')


# ---------------------------------------------------------------- interactive panel

def panel():
    import curses

    def run(scr):
        curses.curs_set(0)
        scr.timeout(300)
        sel = 0
        msg = ''
        while True:
            vals, st = read_ini(), read_status()
            running = game_running()
            scr.erase()
            scr.addstr(0, 0, 'Kenshi plugin controls   (arrows: select/change, space: toggle, q: quit)')
            scr.addstr(1, 0, f'game {"running" if running else "not running"}; file {INI}')
            scr.addstr(3, 0, f'  {"control":38s} {"requested":>9s} {"applied":>8s}')
            for i, (k, label, lo, hi, _) in enumerate(CONTROLS):
                req, app = vals.get(k), st.get(k)
                mark = '' if req is None or app is None or req == app else '  (pending)'
                line = f'{">" if i == sel else " "} {label:38s} {str(req if req is not None else "-"):>9s} ' \
                       f'{str(app if app is not None else "-"):>8s}{mark}'
                scr.addstr(4 + i, 0, line, curses.A_REVERSE if i == sel else 0)
            scr.addstr(5 + len(CONTROLS), 0, 'A/B runs: python3 tools/perfctl.py ab <key>')
            if msg:
                scr.addstr(6 + len(CONTROLS), 0, msg)
            scr.refresh()
            ch = scr.getch()
            if ch == ord('q'):
                return
            k, label, lo, hi, _ = CONTROLS[sel]
            cur = vals.get(k, st.get(k, lo))
            new = None
            if ch == curses.KEY_UP:
                sel = (sel - 1) % len(CONTROLS)
            elif ch == curses.KEY_DOWN:
                sel = (sel + 1) % len(CONTROLS)
            elif ch in (ord(' '), 10, 13) and hi == 1:
                new = 1 - cur
            elif ch in (curses.KEY_RIGHT, ord('+')):
                new = min(hi, cur + 1)
            elif ch in (curses.KEY_LEFT, ord('-')):
                new = max(lo, cur - 1)
            if new is not None and new != cur:
                v = read_ini()
                v[k] = new
                write_ini(v)
                msg = time.strftime('%H:%M:%S') + f' set {k}={new}'

    curses.wrapper(run)


def main():
    args = sys.argv[1:]
    if not args:
        return panel()
    cmd = args[0]
    if cmd == 'show':
        show()
    elif cmd == 'set':
        ch = {}
        for a in args[1:]:
            k, v = a.split('=', 1)
            if k not in KEYS:
                sys.exit(f'unknown key {k}; known: {", ".join(KEYS)}')
            ch[k] = int(v)
        ok = set_values(ch)
        show()
        if not ok:
            print('warning: not all values were confirmed by the plugins within 1.5 s')
    elif cmd == 'ab':
        at, label, rest = [10.0, 20.0], None, []
        i = 1
        while i < len(args):
            if args[i] == '--at':
                at = [float(x) for x in args[i + 1].split(',')]; i += 2
            elif args[i] == '--label':
                label = args[i + 1]; i += 2
            else:
                rest.append(args[i]); i += 1
        key = rest[0]
        lo, hi = KEYS[key][2], KEYS[key][3]
        cur = read_ini().get(key, read_status().get(key, hi))
        if len(rest) >= 3:
            a, b = int(rest[1]), int(rest[2])
        elif hi == 1:
            a, b = cur, 1 - cur
        else:
            a, b = cur, (1 if cur > 1 else 3)
        ab(key, a, b, at, label)
    elif cmd == 'analyze':
        at = [10.0, 20.0]
        if '--at' in args:
            at = [float(x) for x in args[args.index('--at') + 1].split(',')]
        analyze(args[1], at)
    else:
        sys.exit(__doc__)


if __name__ == '__main__':
    main()

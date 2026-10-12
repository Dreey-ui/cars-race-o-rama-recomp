# Summary of a session log (logs\ folder next to the exe, created every time the game starts).
# Usage: python -I tools/session.py                -> last session in C:\Games\Cars Race-O-Rama Recomp\logs
#        python -I tools/session.py --list         -> available sessions
#        python -I tools/session.py -n 2           -> second-to-last session
#        python -I tools/session.py <folder>       -> a logs folder or a single session folder
#        python -I tools/session.py --mark 3       -> everything that happened in the 20 s before mark 3
# Also reads logs from older builds (jogo.log, Portuguese tags).
import os
import re
import sys

DEFAULT_LOGS = r'C:\Games\Cars Race-O-Rama Recomp\logs'
SESSION_RE = re.compile(r'^\d{4}-\d\d-\d\d_\d\d-\d\d-\d\d(_\d+)?$')
LINE_RE = re.compile(r'^(\d\d):(\d\d):(\d\d)\.(\d{3}) (.*)$')
GPU_RE = re.compile(r'^\[gpu\] ([0-9.]+) fps \((?:worst|pior) frame (\d+) ms\) \| draws (\d+)')
MARK_RE = re.compile(r'^\[(?:MARK|MARCA) (\d+)\] =====')
LOG_NAMES = ('game.log', 'jogo.log')
ROUTINE = ('[gpu] ', '[cp] ', '[audio] ', '[file] ', '[arquivo] ', '[input] ', '[shaders] guest memory scan', '[shaders] varredura',
           '[monitor] ', '[stack] ', '[pilha] ', '[boot] ')
PROBLEMS = [
    ('crash', lambda t: t.startswith('[CRASH]')),
    ('picture stopped (hang?)', lambda t: t.startswith(('[hang?] no new frame', '[travou?] nenhum'))),
    ('Lua script error', lambda t: t.startswith('[lua]')),
    ('unimplemented kernel import', lambda t: t.startswith(('[unimplemented import]', '[import nao implementado]'))),
    ('unmapped guest page (auto-map)', lambda t: t.startswith('[auto-map]')),
    ('frontbuffer without content', lambda t: 'NO CONTENT' in t or 'SEM CONTEUDO' in t),
    ('host GPU problem', lambda t: t.startswith('[gpu]') and any(w in t for w in ('removed', 'waiting for the GPU', 'removido', 'esperando a GPU'))),
    ('shader/texture failure', lambda t: 'FAILED' in t or 'FALHOU' in t or t.startswith('[shaders]') and ('fail' in t.lower() or 'error' in t.lower())),
    ('VB/IB cache restarted', lambda t: 'VB/IB cache full' in t or 'cache de VB/IB cheio' in t),
    ('loose scene error', lambda t: 'ERROR loose scene' in t or 'ERRO cena solta' in t),
]


def log_file(path):
    for n in LOG_NAMES:
        p = os.path.join(path, n)
        if os.path.exists(p):
            return p
    return None


def sessions(logs):
    return sorted(d for d in os.listdir(logs) if SESSION_RE.match(d) and os.path.isdir(os.path.join(logs, d)))


def load(path):
    rows = []
    with open(log_file(path), encoding='cp1252', errors='replace') as f:
        for raw in f:
            m = LINE_RE.match(raw.rstrip('\r\n'))
            if m:
                h, mi, s, ms, text = m.groups()
                rows.append((int(h) * 3600 + int(mi) * 60 + int(s) + int(ms) / 1000.0, raw[:12], text))
            elif rows:
                rows.append((rows[-1][0], rows[-1][1], raw.rstrip('\r\n')))
    # past midnight
    day = 0.0
    out = []
    for t, ts, text in rows:
        if out and t + day < out[-1][0] - 3600:
            day += 86400
        out.append((t + day, ts, text))
    return out


def fmt_dur(s):
    s = int(s)
    return '%02d:%02d:%02d' % (s // 3600, s // 60 % 60, s % 60)


def is_session_line(text):
    return text.startswith(('[session]', '[sessao]'))


def is_end_line(text):
    return text.startswith(('[session] end', '[sessao] fim'))


def is_file_line(text):
    return text.startswith(('[file]', '[arquivo]'))


def summary(path, only_mark=None):
    rows = load(path)
    if not rows:
        print('empty log')
        return
    print('== session %s ==' % os.path.basename(path))
    for _, ts, text in rows:
        if is_session_line(text) and not is_end_line(text) and 'F12 in game' not in text and 'F12 no jogo' not in text:
            print('  ' + text[text.index(']') + 2:])
    start, end = rows[0][0], rows[-1][0]
    ends = [(ts, text) for _, ts, text in rows if is_end_line(text)]
    print('duration: %s (%s to %s)' % (fmt_dur(end - start), rows[0][1], rows[-1][1]))
    if ends:
        print('ended: %s at %s' % (ends[-1][1].split(' ', 2)[2], ends[-1][0]))
    else:
        print('ended: WITHOUT the end line -> process killed (task manager / hung and was closed) or still running')

    gpu = []
    for t, ts, text in rows:
        m = GPU_RE.match(text)
        if m:
            gpu.append((t, ts, float(m.group(1)), int(m.group(2)), int(m.group(3))))
    if len(gpu) > 15:
        play = gpu[10:]
        avg = sum(g[2] for g in play) / len(play)
        low = [g for g in play if g[2] < 50]
        print('fps: average %.1f | %d of %d seconds below 50 fps' % (avg, len(low), len(play)))
        worst = sorted(play, key=lambda g: -g[3])[:5]
        print('worst frames: ' + ', '.join('%d ms at %s (%.0f fps, %d draws/s)' % (g[3], g[1], g[2], g[4]) for g in worst))

    print('\n-- problems --')
    found = False
    for name, test in PROBLEMS:
        hits = [(ts, text) for _, ts, text in rows if test(text)]
        if hits:
            found = True
            print('%s: %d (first at %s: %s)' % (name, len(hits), hits[0][0], hits[0][1][:160]))
    missing = set()
    for _, ts, text in rows:
        if is_file_line(text) and ('(missing)' in text or '(nao existe)' in text) and '.bpb (' not in text and 'XBR' not in text:
            missing.add(text[text.index(']') + 2:].replace(' (missing)', '').replace(' (nao existe)', ''))
    if missing:
        found = True
        print('files the game looked for that do not exist: %d (e.g.: %s)' % (len(missing), ', '.join(sorted(missing)[:5])))
    if not found:
        print('none')

    marks = [(t, ts, text) for t, ts, text in rows if MARK_RE.match(text)]
    print('\n-- marks (F12) --' if marks else '\n-- no marks (F12) --')
    for t, ts, text in marks:
        n = int(MARK_RE.match(text).group(1))
        if only_mark is not None and n != only_mark:
            continue
        folder = [d for d in os.listdir(path) if d.startswith(('mark_%02d_' % n, 'marca_%02d_' % n))]
        print('\n* mark %d at %s  (folder %s)' % (n, ts, folder[0] if folder else '?'))
        window = 20 if only_mark is not None else 10
        before = [(tt, s, x) for tt, s, x in rows if t - window <= tt <= t + 1]
        g = [x for x in before if GPU_RE.match(x[2])]
        if g:
            print('  fps before: ' + ' | '.join(GPU_RE.match(x[2]).group(1) for x in g[-6:]))
        files = []
        for x in reversed([x for x in rows if x[0] <= t and is_file_line(x[2])]):
            if all(x[2] != f[2] for f in files):
                files.insert(0, x)
            if len(files) == 6:
                break
        if files:
            print('  last files opened: ' + ', '.join('%s %s' % (s[:8], x[x.index(']') + 2:]) for _, s, x in files))
        inputs = [x for x in before if x[2].startswith('[input]')]
        if inputs:
            print('  input in the %d s before: ' % window + ' '.join('%s%s' % (s[:8], x[x.index(':') + 1:]) for _, s, x in inputs[-25:]))
        events = [x for x in before if not x[2].startswith(ROUTINE) and not MARK_RE.match(x[2]) and not x[2].startswith(('[MARK', '[MARCA'))]
        for _, s, x in events[-15:]:
            print('  %s %s' % (s, x[:200]))
        if only_mark is not None:
            for _, s, x in before:
                if not GPU_RE.match(x):
                    print('    | %s %s' % (s, x[:220]))


def main():
    args = sys.argv[1:]
    logs = DEFAULT_LOGS
    n = 1
    only_mark = None
    i = 0
    listing = False
    while i < len(args):
        a = args[i]
        if a == '--list':
            listing = True
        elif a == '-n':
            n = int(args[i + 1])
            i += 1
        elif a == '--mark':
            only_mark = int(args[i + 1])
            i += 1
        else:
            logs = a
        i += 1
    if log_file(logs):
        summary(logs, only_mark)
        return
    found = sessions(logs)
    if not found:
        print('no sessions in %s' % logs)
        return
    if listing:
        for d in found:
            p = os.path.join(logs, d)
            lf = log_file(p)
            size = os.path.getsize(lf) if lf else 0
            marks = len([x for x in os.listdir(p) if x.startswith(('mark_', 'marca_'))])
            print('%s  log %7.1f KB  marks %d' % (d, size / 1024.0, marks))
        return
    summary(os.path.join(logs, found[-n]), only_mark)


if __name__ == '__main__':
    main()

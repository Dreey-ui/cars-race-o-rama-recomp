# Resumo de uma sessao do log (pasta logs\ ao lado do exe, criada a cada vez que o jogo abre).
# Uso: python -I tools/sessao.py                -> ultima sessao em C:\Games\Cars Race-O-Rama Recomp\logs
#      python -I tools/sessao.py --lista        -> sessoes disponiveis
#      python -I tools/sessao.py -n 2           -> penultima sessao
#      python -I tools/sessao.py <pasta>        -> pasta de logs ou de uma sessao
#      python -I tools/sessao.py --marca 3      -> tudo que aconteceu nos 20 s antes da marca 3
import os
import re
import sys

DEFAULT_LOGS = r'C:\Games\Cars Race-O-Rama Recomp\logs'
SESSION_RE = re.compile(r'^\d{4}-\d\d-\d\d_\d\d-\d\d-\d\d(_\d+)?$')
LINE_RE = re.compile(r'^(\d\d):(\d\d):(\d\d)\.(\d{3}) (.*)$')
GPU_RE = re.compile(r'^\[gpu\] ([0-9.]+) fps \(pior frame (\d+) ms\) \| draws (\d+)')
ROUTINE = ('[gpu] ', '[cp] ', '[audio] ', '[arquivo] ', '[input] ', '[shaders] varredura', '[monitor] ', '[pilha] ', '[boot] ')
PROBLEMS = [
    ('crash', lambda t: t.startswith('[CRASH]')),
    ('imagem parada (travou?)', lambda t: t.startswith('[travou?] nenhum')),
    ('erro em script Lua', lambda t: t.startswith('[lua]')),
    ('import do kernel nao implementado', lambda t: t.startswith('[import nao implementado]')),
    ('pagina guest nao mapeada (auto-map)', lambda t: t.startswith('[auto-map]')),
    ('frontbuffer sem conteudo', lambda t: 'SEM CONTEUDO' in t),
    ('GPU do host com problema', lambda t: t.startswith('[gpu]') and ('removido' in t or 'esperando a GPU' in t)),
    ('shader/textura com falha', lambda t: 'FALHOU' in t or 'falha' in t.lower() and t.startswith('[shaders]')),
    ('cache de VB/IB recomecou', lambda t: 'cache de VB/IB cheio' in t),
]


def sessions(logs):
    return sorted(d for d in os.listdir(logs) if SESSION_RE.match(d) and os.path.isdir(os.path.join(logs, d)))


def load(path):
    rows = []
    with open(os.path.join(path, 'jogo.log'), encoding='cp1252', errors='replace') as f:
        for raw in f:
            m = LINE_RE.match(raw.rstrip('\r\n'))
            if m:
                h, mi, s, ms, text = m.groups()
                rows.append((int(h) * 3600 + int(mi) * 60 + int(s) + int(ms) / 1000.0, raw[:12], text))
            elif rows:
                rows.append((rows[-1][0], rows[-1][1], raw.rstrip('\r\n')))
    # passou da meia-noite
    day = 0.0
    out = []
    for i, (t, ts, text) in enumerate(rows):
        if out and t + day < out[-1][0] - 3600:
            day += 86400
        out.append((t + day, ts, text))
    return out


def fmt_dur(s):
    s = int(s)
    return '%02d:%02d:%02d' % (s // 3600, s // 60 % 60, s % 60)


def summary(path, only_mark=None):
    rows = load(path)
    if not rows:
        print('jogo.log vazio')
        return
    print('== sessao %s ==' % os.path.basename(path))
    for _, ts, text in rows:
        if text.startswith('[sessao]') and 'fim (' not in text and 'F12 no jogo' not in text:
            print('  ' + text[9:])
    start, end = rows[0][0], rows[-1][0]
    ends = [(ts, text) for _, ts, text in rows if text.startswith('[sessao] fim')]
    print('duracao: %s (%s ate %s)' % (fmt_dur(end - start), rows[0][1], rows[-1][1]))
    if ends:
        print('terminou: %s as %s' % (ends[-1][1][15:], ends[-1][0]))
    else:
        print('terminou: SEM a linha de fim -> processo morto (gerenciador de tarefas / travou e foi fechado) ou ainda aberto')

    gpu = []
    for t, ts, text in rows:
        m = GPU_RE.match(text)
        if m:
            gpu.append((t, ts, float(m.group(1)), int(m.group(2)), int(m.group(3))))
    if len(gpu) > 15:
        play = gpu[10:]
        avg = sum(g[2] for g in play) / len(play)
        low = [g for g in play if g[2] < 50]
        print('fps: media %.1f | %d de %d segundos abaixo de 50 fps' % (avg, len(low), len(play)))
        worst = sorted(play, key=lambda g: -g[3])[:5]
        print('piores frames: ' + ', '.join('%d ms as %s (%.0f fps, %d draws/s)' % (g[3], g[1], g[2], g[4]) for g in worst))

    print('\n-- problemas --')
    found = False
    for name, test in PROBLEMS:
        hits = [(ts, text) for _, ts, text in rows if test(text)]
        if hits:
            found = True
            print('%s: %d (primeira as %s: %s)' % (name, len(hits), hits[0][0], hits[0][1][:160]))
    missing = sorted({text[10:].replace(' (nao existe)', '') for _, ts, text in rows
                      if text.startswith('[arquivo]') and '(nao existe)' in text and not text.endswith('.bpb (nao existe)')
                      and 'XBR' not in text})
    if missing:
        found = True
        print('arquivos que o jogo procurou e nao existem: %d (ex.: %s)' % (len(missing), ', '.join(missing[:5])))
    if not found:
        print('nenhum')

    marks = [(t, ts, text) for t, ts, text in rows if re.match(r'^\[MARCA \d+\] =====', text)]
    print('\n-- marcas (F12) --' if marks else '\n-- nenhuma marca (F12) --')
    for t, ts, text in marks:
        n = int(re.match(r'^\[MARCA (\d+)\]', text).group(1))
        if only_mark is not None and n != only_mark:
            continue
        folder = [d for d in os.listdir(path) if d.startswith('marca_%02d_' % n)]
        print('\n* marca %d as %s  (pasta %s)' % (n, ts, folder[0] if folder else '?'))
        window = 20 if only_mark is not None else 10
        before = [(tt, s, x) for tt, s, x in rows if t - window <= tt <= t + 1]
        g = [x for x in before if GPU_RE.match(x[2])]
        if g:
            print('  fps antes: ' + ' | '.join(GPU_RE.match(x[2]).group(1) for x in g[-6:]))
        files = []
        for x in reversed([x for x in rows if x[0] <= t and x[2].startswith('[arquivo]')]):
            if all(x[2] != f[2] for f in files):
                files.insert(0, x)
            if len(files) == 6:
                break
        if files:
            print('  ultimos arquivos abertos: ' + ', '.join('%s %s' % (s[:8], x[10:]) for _, s, x in files))
        inputs = [x for x in before if x[2].startswith('[input]')]
        if inputs:
            print('  entradas nos %d s antes: ' % window + ' '.join('%s%s' % (s[:8], x[x.index(':') + 1:]) for _, s, x in inputs[-25:]))
        events = [x for x in before if not x[2].startswith(ROUTINE) and not x[2].startswith('[MARCA')]
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
        if a == '--lista':
            listing = True
        elif a == '-n':
            n = int(args[i + 1])
            i += 1
        elif a == '--marca':
            only_mark = int(args[i + 1])
            i += 1
        else:
            logs = a
        i += 1
    if os.path.exists(os.path.join(logs, 'jogo.log')):
        summary(logs, only_mark)
        return
    found = sessions(logs)
    if not found:
        print('nenhuma sessao em %s' % logs)
        return
    if listing:
        for d in found:
            p = os.path.join(logs, d)
            size = os.path.getsize(os.path.join(p, 'jogo.log')) if os.path.exists(os.path.join(p, 'jogo.log')) else 0
            marks = len([x for x in os.listdir(p) if x.startswith('marca_')])
            print('%s  log %7.1f KB  marcas %d' % (d, size / 1024.0, marks))
        return
    summary(os.path.join(logs, found[-n]), only_mark)


if __name__ == '__main__':
    main()

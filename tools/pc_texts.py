# Game texts without "Xbox 360": console -> computer (in each language), controller without the brand.
# Reads <game>\<L>\Loc\<L>_L_*.bin (u32 byte size of the text block, BOM FF FE, NUL-terminated UTF-16LE strings,
# then each string's attribute bytes, kept as they are) and writes the changed version to <game>\mods\<L>\Loc\
# (the original stays untouched).
# Usage: python -I tools/pc_texts.py [--game "<game folder>"] [--undo]
import argparse
import os
import struct
import sys

FILES = ('L_TRC_X360', 'L_CarsFrontEndUI')
# (from, to) per language; only applied to strings that mention Xbox or the console (order: longer phrases first)
RULES = {
    'E': [('your Xbox 360 console', 'your computer'), ('the Xbox 360 controller', 'the controller'),
          ('XBOX 360 CONTROLLER SETTINGS', 'CONTROLLER SETTINGS')],
    'F': [('votre console Xbox 360', 'votre ordinateur'), ('la console Xbox 360', "l'ordinateur"),
          ('la manette Xbox 360', 'la manette'), ('à la console', "à l'ordinateur"),
          ('COMMANDES MANETTE XBOX 360', 'COMMANDES MANETTE'),
          ('votre console', 'votre ordinateur')],
    'G': [('Ihre Xbox 360 Konsole', 'Ihren Computer'), ('die Xbox 360 Konsole', 'den Computer'),
          ('den Xbox 360 Controller', 'den Controller'), ('mit der Konsole', 'mit dem Computer'),
          ('XBOX 360 CONTROLLER-EINSTELLUNGEN', 'CONTROLLER-EINSTELLUNGEN'),
          ('Ihre Konsole', 'Ihren Computer')],
    'I': [('la console Xbox 360', 'il computer'), ('Non spegnere Xbox 360', 'Non spegnere il computer'),
          ('il controller Xbox 360', 'il controller'), ('alla console', 'al computer'),
          ('IMPOSTAZIONI CONTROLLER XBOX 360', 'IMPOSTAZIONI CONTROLLER'),
          ('la console', 'il computer')],
    'S': [('tu consola Xbox 360', 'tu ordenador'), ('la consola Xbox 360', 'el ordenador'),
          ('el mando Xbox 360', 'el mando'), ('a la consola', 'al ordenador'),
          ('CONFIGURACIÓN DEL MANDO PARA XBOX 360', 'CONFIGURACIÓN DEL MANDO'),
          ('tu consola', 'tu ordenador')],
}


def convert(data, rules):
    n = struct.unpack_from('<I', data, 0)[0]
    if data[4:6] != b'\xff\xfe' or 4 + n > len(data):
        raise ValueError('unexpected format')
    texts = data[6:4 + n].decode('utf-16-le').split('\0')
    changed = []
    for k, t in enumerate(texts):
        if not any(w in t.lower() for w in ('xbox', 'consol', 'konsol')):
            continue
        new = t
        for a, b in rules:
            new = new.replace(a, b)
        if any(w in new.lower() for w in ('xbox', 'consol', 'konsol')):
            raise ValueError('string %d still mentions Xbox/console: %r' % (k, new))
        texts[k] = new
        changed.append((t, new))
    body = b'\xff\xfe' + '\0'.join(texts).encode('utf-16-le')
    return struct.pack('<I', len(body)) + body + data[4 + n:], changed


def main():
    sys.stdout.reconfigure(encoding='utf-8')
    ap = argparse.ArgumentParser()
    ap.add_argument('--game', default=r'C:\Games\Cars Race-O-Rama Recomp')
    ap.add_argument('--undo', action='store_true', help='deletes the changed texts from mods')
    a = ap.parse_args()
    for lang, rules in RULES.items():
        for name in FILES:
            rel = os.path.join(lang, 'Loc', '%s_%s.bin' % (lang, name))
            src = os.path.join(a.game, rel)
            dst = os.path.join(a.game, 'mods', rel)
            if a.undo:
                if os.path.exists(dst):
                    os.remove(dst)
                    print('removed', dst)
                continue
            out, changed = convert(open(src, 'rb').read(), rules)
            os.makedirs(os.path.dirname(dst), exist_ok=True)
            with open(dst, 'wb') as f:
                f.write(out)
            print('%s: %d string(s)' % (rel, len(changed)))
            for old, new in changed:
                print('   %s\n -> %s' % (old.replace('\r\n', ' '), new.replace('\r\n', ' ')))


if __name__ == '__main__':
    main()

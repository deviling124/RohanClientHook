#!/usr/bin/env python3
"""Costume Effect item data: the ticket, and for every costume in costume_effect.yml one new costume item per effect with
that item's aura row. The YAML is the file the server uses. The game is only read.

  python tools/make_costume_effect_items.py <game folder> [--data <folder>] [--yml <file>]

  --data  folder with the ORIGINAL itemcostume.bin, itemetc.bin and gameeffect.bin (default: <game folder>/data).
          Use it when the game folder already has Costume Effect items: the tool never edits an existing record.
  --yml   default: ../RohanMapServ_Hook/costume_effect.yml

Writes bin/CostumeEffect/data/. Needs effect_save.py from the sibling server repo."""
import os
import struct
import sys

root = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(root, '..', 'RohanMapServ_Hook', 'tools', 'mapeditor'))
import effect_save as es  # noqa: E402

COSTUME_SIZE, ETC_SIZE = 724, 680
# Offsets inside a record (BinEditor layouts). Both start: Disable, ID@4, Name@8[50], KoreanName@58[50].
NAME, KOREAN, ETC_TYPE, ETC_MAXSTACK, ETC_EXPLAIN = 8, 58, 108, 128, None
MATERIAL_TYPE = 32                                   # the most common plain itemetc type


def read_config(path):
    """The small part of YAML costume_effect.yml uses. Anything else is an error, never skipped."""
    ticket, effects, costumes = None, [], []
    section, results = None, False
    with open(path, encoding='utf-8-sig') as f:
        for number, line in enumerate(f, 1):
            line = line.split('#', 1)[0].rstrip()
            if not line.strip():
                continue

            def bad(why='not understood'):
                raise SystemExit('%s line %d: %s' % (os.path.basename(path), number, why))

            indent = len(line) - len(line.lstrip(' '))
            word = line.strip()
            item = word.startswith('- ')
            key, colon, value = word[2 if item else 0:].partition(':')
            key, value = key.strip(), value.strip().strip('"\'')
            if not colon:
                bad()
            if indent == 0 and not item:
                section, results = key, False
                if key == 'Ticket' and value.isdigit():
                    ticket = int(value)
                elif key not in ('Effects', 'Costumes') or value:
                    bad()
            elif section == 'Effects':
                if item:
                    effects.append({})
                if not effects or key not in ('Key', 'Name', 'Aura') or key in effects[-1] or not value:
                    bad()
                effects[-1][key] = value
            elif section == 'Costumes':
                if item:
                    costumes.append({'Results': []})
                    results = False
                if not costumes:
                    bad()
                if key == 'Base' and value.isdigit() and 'Base' not in costumes[-1]:
                    costumes[-1]['Base'], results = int(value), False
                elif key == 'Results' and not value:
                    results = True
                elif results and value.isdigit():
                    costumes[-1]['Results'].append((key, int(value)))
                else:
                    bad()
            else:
                bad()

    keys = [e.get('Key') for e in effects]
    if ticket is None or not effects or not costumes:
        raise SystemExit('the file needs Ticket, Effects and Costumes')
    if len(set(keys)) != len(keys) or any(set(e) != {'Key', 'Name', 'Aura'} for e in effects):
        raise SystemExit('every effect needs its own Key, a Name and an Aura')
    ids = [ticket]
    for costume in costumes:
        if 'Base' not in costume or not 1 <= len(costume['Results']) <= 5:
            raise SystemExit('every costume needs a Base and 1 to 5 Results')
        names = [k for k, _ in costume['Results']]
        if len(set(names)) != len(names) or any(k not in keys for k in names):
            raise SystemExit('costume %d: unknown or repeated effect key' % costume['Base'])
        ids += [costume['Base']] + [i for _, i in costume['Results']]
        if any(not 0x430000 <= i <= 0x43ffff for i in ids[1:]):
            raise SystemExit('costume %d: an id is outside the costume block' % costume['Base'])
    if len(set(ids)) != len(ids):
        raise SystemExit('an item id is used twice')
    return ticket, effects, costumes


def records(data, size):
    if len(data) % size:
        raise SystemExit('a table is not a whole number of %d byte records' % size)
    return {struct.unpack_from('<i', data, a + 4)[0]: data[a:a + size] for a in range(0, len(data), size)}


def with_text(record, at, length, text):
    raw = text.encode('ascii')
    if len(raw) >= length:
        raise SystemExit('too long: ' + text)
    record[at:at + length] = raw.ljust(length, b'\0')


def copy_of(template, item, name):
    record = bytearray(template)
    struct.pack_into('<ii', record, 0, 0, item)
    with_text(record, NAME, 50, name)
    with_text(record, KOREAN, 50, name)
    return record


def main(game, data=None, yml=None):
    ticket, effects, wanted = read_config(yml or os.path.join(root, '..', 'RohanMapServ_Hook', 'costume_effect.yml'))
    folder_in = data or os.path.join(game, 'data')
    read = lambda name: es.read(os.path.join(folder_in, name))
    costume, etc, aura = read('itemcostume.bin'), read('itemetc.bin'), read('gameeffect.bin')
    costumes, etcs = records(costume, COSTUME_SIZE), records(etc, ETC_SIZE)
    taken = set(costumes) | set(etcs)
    new = [ticket] + [i for c in wanted for _, i in c['Results']]
    missing = [c['Base'] for c in wanted if c['Base'] not in costumes]
    if missing:
        raise SystemExit('not in itemcostume.bin: ' + ', '.join(map(str, missing)))
    used = [i for i in new if i in taken]
    if used:
        raise SystemExit('item ids already used (choose free ids, and build from the ORIGINAL data files, see --data): ' +
                         ', '.join(map(str, used)))
    known = {n.lower(): n for n, _ in es.viewer.parse_ebf(es.read(os.path.join(game, es.EBF[0])))}
    missing = [e['Aura'] for e in effects if e['Aura'].lower() not in known]
    if missing:
        raise SystemExit('the game has no effect called ' + ', '.join(missing))
    material = next(r for _, r in sorted(etcs.items())
                    if struct.unpack_from('<i', r, ETC_TYPE)[0] == MATERIAL_TYPE and struct.unpack_from('<i', r)[0] == 0
                    and struct.unpack_from('<i', r, ETC_MAXSTACK)[0] > 1)
    number = {e['Key']: (n, e) for n, e in enumerate(effects, 1)}

    out_etc, out_costume, out_aura = bytearray(etc), bytearray(costume), aura
    out_etc += copy_of(material, ticket, 'Costume Effect Ticket')
    made = [('Costume Effect Ticket', ticket, '')]
    for c in wanted:
        base = c['Base']
        base_name = costumes[base][NAME:NAME + 50].split(b'\0')[0].decode('latin-1')
        for key_name, result in c['Results']:
            n, effect = number[key_name]
            suffix = ' +E%d' % n
            name = base_name.encode('ascii', 'replace').decode('ascii')[:49 - len(suffix)] + suffix
            out_costume += copy_of(costumes[base], result, name)
            key = result - 0x430000
            if any(struct.unpack_from('<i', out_aura, a + 4)[0] == key and
                   es.viewer.text(out_aura[a:a + es.ROW], 0x6c, 50) == es.COSTUME_TYPE
                   for a in range(0, len(out_aura), es.ROW)):
                raise SystemExit('gameeffect.bin already has an aura row for costume %d' % result)
            out_aura, _ = es.with_costume_row(out_aura, key, known[effect['Aura'].lower()])
            made.append((name, result, effect['Aura']))

    assert bytes(out_etc[:len(etc)]) == etc and bytes(out_costume[:len(costume)]) == costume and out_aura[:len(aura)] == aura
    folder = os.path.join(root, 'bin', 'CostumeEffect', 'data')
    os.makedirs(folder, exist_ok=True)
    for name, out, size in (('itemetc.bin', out_etc, ETC_SIZE), ('itemcostume.bin', out_costume, COSTUME_SIZE),
                            ('gameeffect.bin', out_aura, es.ROW)):
        with open(os.path.join(folder, name), 'wb') as f:
            f.write(out)
        print('%s: %d -> %d records' % (name, len(read(name)) // size, len(out) // size))
    for name, item, effect in made:
        print('%d  %s  %s' % (item, name, effect))


if __name__ == '__main__':
    args, options = [], {}
    words = iter(sys.argv[1:])
    for word in words:
        if word in ('--data', '--yml'):
            options[word[2:]] = next(words)
        else:
            args.append(word)
    if len(args) != 1:
        raise SystemExit(__doc__)
    main(args[0], **options)

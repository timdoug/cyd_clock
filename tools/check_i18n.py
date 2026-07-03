#!/usr/bin/env python3
# Portable i18n sanity checks - no macOS or Swift required. Verifies that
# every character used by main/i18n_data.inc has a glyph in main/fonts.c
# (otherwise it renders as '?' on the device until tools/gen_fonts.swift is
# rerun on macOS), that no string table has duplicate ids, and that English
# defines every id in the str_id_t enum.
import re
import sys
import unicodedata
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
CJK_SCOPES = ("zh_hant", "ja", "zh", "ko", "ka", "hy")  # longest first so zh_hant wins over zh
SHAPED_SCOPES = ("ar", "fa", "he", "hi", "bn", "th", "ur", "mr", "ta", "te", "pa", "gu")  # pre-shaped by the generator


def parse_enum_ids(i18n_h):
    m = re.search(r'typedef enum \{(.*?)\} str_id_t;', i18n_h, re.S)
    return [x for x in re.findall(r'(STR_\w+)', m.group(1)) if x != 'STR_COUNT']


def literal_chars(line):
    # contents of every "..." literal on the line, comments stripped
    line = re.sub(r'//.*', '', line)
    out = []
    for lit in re.findall(r'"((?:\\.|[^"\\])*)"', line):
        out.append(re.sub(r'\\(.)', r'\1', lit))
    return ''.join(out)


def scope_of(symbol):
    for scope in CJK_SCOPES:
        if symbol.endswith('_' + scope):
            return scope
    return ''


def main():
    inc = (ROOT / 'main' / 'i18n_data.inc').read_text(encoding='utf-8')
    fonts = (ROOT / 'main' / 'fonts.c').read_text(encoding='utf-8')
    i18n_h = (ROOT / 'main' / 'i18n.h').read_text(encoding='utf-8')
    errors = 0

    # glyph coverage tables from fonts.c
    coverage = {}
    for scope, table in [('', 'font_base_ext_cp')] + [(s, f'{s}_glyphs_cp') for s in CJK_SCOPES]:
        m = re.search(r'uint16_t ' + table + r'\[\] = \{(.*?)\};', fonts, re.S)
        # a font not generated yet has no coverage at all
        coverage[scope] = ({int(x, 16) for x in re.findall(r'0x([0-9A-F]{4})', m.group(1))}
                           if m else set())

    # walk the data file: bucket string chars by declaration scope,
    # collect ids per lang table for dup/completeness checks
    scope = ''
    in_block = False
    in_shaped = False
    table_ids = {}
    cur_table = None
    used = {s: set() for s in coverage}
    for lineno, line in enumerate(inc.split('\n'), 1):
        # shaped-source sections are generator input: their characters are
        # rasterized to cluster glyphs, never rendered from codepoints, so
        # coverage does not apply (ids are still collected below)
        if line.startswith('// >>> shaped-source'):
            in_shaped = True
        if line.startswith('// <<< shaped-source'):
            in_shaped = False
        if not in_block:
            scope = ''
            cur_table = None
            m = re.search(r'(lang|weekdays|months|lang_name|date_year|date_day)_[a-z_]+', line)
            if m:
                scope = scope_of(m.group(0))
                if m.group(0).startswith('lang_') and '[STR_COUNT]' in line:
                    cur_table = m.group(0)
                    table_ids[cur_table] = []
            if '= {' in line:
                in_block = True
        if cur_table:
            sid = re.search(r'\[(STR_\w+)\]', line)
            if sid and sid.group(1) != 'STR_COUNT':
                table_ids[cur_table].append(sid.group(1))
        if not in_shaped:
            for ch in unicodedata.normalize('NFC', literal_chars(line)):
                if ord(ch) > 0x7E:
                    used[scope].add((ch, lineno))
        if in_block and line.startswith('};'):
            in_block = False
            cur_table = None

    # coverage: CJK scopes may also fall back to the base table at runtime
    for scope, chars in used.items():
        for ch, lineno in sorted(chars, key=lambda t: ord(t[0])):
            cp = ord(ch)
            ok = cp in coverage[scope] or (scope and cp in coverage[''])
            if not ok:
                target = scope or 'base'
                print(f"i18n_data.inc:{lineno}: U+{cp:04X} ({ch}) has no glyph in the "
                      f"{target} font - rerun tools/gen_fonts.swift on macOS")
                errors += 1

    # duplicate ids
    for table, ids in table_ids.items():
        dups = {x for x in ids if ids.count(x) > 1}
        if dups:
            print(f"{table}: duplicate ids {sorted(dups)} (last initializer silently wins)")
            errors += 1

    # English completeness
    enum_ids = parse_enum_ids(i18n_h)
    missing = [x for x in enum_ids if x not in table_ids.get('lang_en', [])]
    if missing:
        print(f"lang_en is missing {missing} - English is the fallback and must be complete")
        errors += 1
    unknown = [x for ids in table_ids.values() for x in ids if x not in enum_ids]
    if unknown:
        print(f"unknown string ids: {sorted(set(unknown))}")
        errors += 1

    # stale shaped tables: fonts.c records a hash of each shaped-source
    # section; a mismatch means the raw text was edited without a macOS
    # regeneration pass
    import hashlib
    for scope in SHAPED_SCOPES:
        a = inc.find(f'// >>> shaped-source {scope}\n')
        b = inc.find(f'// <<< shaped-source {scope}')
        if a < 0 or b < 0:
            print(f"shaped-source markers for {scope} missing from i18n_data.inc")
            errors += 1
            continue
        section = inc[a + len(f'// >>> shaped-source {scope}\n'):b]
        want = hashlib.sha256(section.encode()).hexdigest()[:16]
        m = re.search(r'// shaped-source-hash ' + scope + r' ([0-9a-f]+)', fonts)
        if not m:
            print(f"{scope}: no shaped tables in fonts.c - run tools/gen_fonts.swift on macOS")
            errors += 1
        elif m.group(1) != want:
            print(f"{scope}: shaped tables in fonts.c are STALE - the source in "
                  f"i18n_data.inc changed; rerun tools/gen_fonts.swift on macOS")
            errors += 1

    if errors:
        print(f"{errors} problem(s)")
        return 1
    langs = len(table_ids)
    glyphs = sum(len(v) for v in coverage.values())
    print(f"OK: {langs} languages, all characters covered ({glyphs} glyphs across {len(coverage)} fonts)")
    return 0


if __name__ == '__main__':
    sys.exit(main())

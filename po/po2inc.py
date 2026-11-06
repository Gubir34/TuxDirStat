#!/usr/bin/env python3
"""Compiles po/*.po into a C++ table included by src/core/i18n.cpp.

Usage: po2inc.py OUTPUT.inc file1.po [file2.po ...]

Untranslated and fuzzy entries are left out (the English text is shown). The header
fields X-Language-Name, X-Decimal-Point, X-Thousands-Separator and X-Date-Format set
the language's name and number/date formats.
"""
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import pofile  # noqa: E402


def char_lit(c, default):
    c = c or default
    return "'\\''" if c == "'" else "'" + c[0] + "'"


def main():
    out, pos = sys.argv[1], sorted(sys.argv[2:], key=os.path.basename)
    lines = ["// Generated from po/*.po by po/po2inc.py. Do not edit.", ""]
    cats = []
    for path in pos:
        code = os.path.splitext(os.path.basename(path))[0]
        header, body = pofile.read(path)
        ident = "".join(ch if ch.isalnum() else "_" for ch in code)
        pairs = [(i, s) for i, s, fz in body if s and not fz]
        lines.append(f"static const Pair CAT_{ident}[] = {{")
        for i, s in pairs:
            lines.append(f"    {{{pofile.quote(i)}, {pofile.quote(s)}}},")
        if not pairs:
            lines.append('    {"", ""},')
        lines.append("};")
        cats.append(
            f"    {{{pofile.quote(code)}, {pofile.quote(header.get('X-Language-Name', code))}, "
            f"{char_lit(header.get('X-Decimal-Point'), '.')}, {char_lit(header.get('X-Thousands-Separator'), ',')}, "
            f"{pofile.quote(header.get('X-Date-Format', '%Y-%m-%d %H:%M'))}, CAT_{ident}, {len(pairs)}}},"
        )
    lines.append("")
    lines.append("static const Catalog CATALOGS[] = {")
    lines.extend(cats or ['    {"en", "English", \'.\', \',\', "%Y-%m-%d %H:%M", nullptr, 0},'])
    lines.append("};")
    text = "\n".join(lines) + "\n"
    try:
        if open(out, encoding="utf-8").read() == text:
            return 0
    except OSError:
        pass
    with open(out, "w", encoding="utf-8") as f:
        f.write(text)
    return 0


sys.exit(main())

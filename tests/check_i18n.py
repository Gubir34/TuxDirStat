#!/usr/bin/env python3
"""Checks that the translatable strings in the sources are in the translations (po/*.po).

Found in the sources: T("…") / TF("…") / QT / QTF / N_ (adjacent literals are joined),
add("…") / addNode("…") suggestion names in tools.cpp, age bucket labels and Tcl [T "…"].

Turkish (po/tr.po) must be complete; other languages only get a summary line.

Usage:
  tests/check_i18n.py               check (exit code 1 when Turkish is incomplete)
  tests/check_i18n.py --update-pot  rewrite po/tuxdirstat.pot (template for new languages)
  tests/check_i18n.py --merge       add missing strings to every po/*.po (empty msgstr)
"""
import os
import re
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(ROOT, "po"))
import pofile  # noqa: E402

LIT = r'"(?:[^"\\\n]|\\.)*"'
REQUIRED = {"tr"}


def unescape(lit):
    body = lit[1:-1]
    return bytes(body, "utf-8").decode("unicode_escape").encode("latin-1").decode("utf-8")


def join_literals(text):
    return "".join(unescape(m) for m in re.findall(LIT, text))


def used_keys():
    """Returns {string: first "file:line" where it is used}."""
    keys = {}

    def add(k, path, src, pos):
        if k and k not in keys:
            keys[k] = "%s:%d" % (os.path.relpath(path, ROOT), src.count("\n", 0, pos) + 1)

    for dirpath, _, files in sorted(os.walk(os.path.join(ROOT, "src"))):
        for f in sorted(files):
            if not f.endswith((".cpp", ".h")):
                continue
            path = os.path.join(dirpath, f)
            src = open(path, encoding="utf-8").read()
            src = re.sub(r"(?m)^\s*//.*$", lambda m: " " * len(m.group(0)), src)
            for m in re.finditer(r'\b(?:T|TF|QT|QTF|N_)\(\s*((?:' + LIT + r'\s*)+)', src):
                add(join_literals(m.group(1)), path, src, m.start())
            for m in re.finditer(r'\b(?:add|addNode)\(\s*(' + LIT + r')', src):
                add(unescape(m.group(1)), path, src, m.start())
            for m in re.finditer(r'\{\s*[0-9A-Z_* ]*DAY[^,]*,\s*(' + LIT + r')', src):
                add(unescape(m.group(1)), path, src, m.start())
            for m in re.finditer(r'\{INT64_MAX,\s*(' + LIT + r')', src):
                add(unescape(m.group(1)), path, src, m.start())
            for m in re.finditer(r'\? "(Unused (?:Docker|Podman) data)"\s*:\s*"(Unused (?:Docker|Podman) data)"', src):
                for g in m.groups():
                    add(g, path, src, m.start())
            for m in re.finditer(r'\[T "((?:[^"\\]|\\.)*)"\]', src):
                add(m.group(1).replace('\\"', '"'), path, src, m.start())
    return keys


def po_files():
    d = os.path.join(ROOT, "po")
    return sorted(os.path.join(d, f) for f in os.listdir(d) if f.endswith(".po"))


def entry(msgid, msgstr, ref=None):
    out = []
    if ref:
        out.append("#: " + ref)
    if "%1" in msgid:
        out.append("#, c-format")
    out.append("msgid " + pofile.quote(msgid))
    out.append("msgstr " + pofile.quote(msgstr))
    return "\n".join(out) + "\n"


def update_pot(used):
    header = (
        '# Template for TuxDirStat translations. Copy to po/<language>.po and fill in msgstr.\n'
        '# %1 … %9 are placeholders; keep them in the translation.\n'
        'msgid ""\nmsgstr ""\n'
        '"Project-Id-Version: TuxDirStat\\n"\n'
        '"Language: \\n"\n'
        '"MIME-Version: 1.0\\n"\n'
        '"Content-Type: text/plain; charset=UTF-8\\n"\n'
        '"Content-Transfer-Encoding: 8bit\\n"\n'
        '"X-Language-Name: \\n"\n'
        '"X-Decimal-Point: .\\n"\n'
        '"X-Thousands-Separator: ,\\n"\n'
        '"X-Date-Format: %Y-%m-%d %H:%M\\n"\n'
    )
    body = [entry(k, "", used[k]).replace("#, c-format\n", "") for k in sorted(used, key=lambda k: used[k])]
    with open(os.path.join(ROOT, "po", "tuxdirstat.pot"), "w", encoding="utf-8") as f:
        f.write(header + "\n" + "\n".join(body))


def merge(used):
    for path in po_files():
        _, body = pofile.read(path)
        have = {i for i, _, _ in body}
        missing = [k for k in sorted(used, key=lambda k: used[k]) if k not in have]
        if missing:
            with open(path, "a", encoding="utf-8") as f:
                for k in missing:
                    f.write("\n" + entry(k, "", used[k]).replace("#, c-format\n", ""))
        print("%s: %d added" % (os.path.basename(path), len(missing)))


def main():
    used = used_keys()
    if "--update-pot" in sys.argv:
        update_pot(used)
        return 0
    if "--merge" in sys.argv:
        merge(used)
        return 0
    failed = False
    for path in po_files():
        code = os.path.splitext(os.path.basename(path))[0]
        _, body = pofile.read(path)
        ids = [i for i, _, _ in body]
        done = {i for i, s, fz in body if s and not fz}
        dup = sorted({i for i in ids if ids.count(i) > 1})
        missing = sorted(k for k in used if k not in done)
        unused = sorted(set(ids) - set(used))
        bad = [i for i, s, fz in body if s and not fz and
               sorted(re.findall(r"%[1-9]", i)) != sorted(re.findall(r"%[1-9]", s))]
        if code in REQUIRED:
            for k in dup:
                print("%s duplicate: %r" % (code, k))
            for k in missing:
                print("%s missing: %r  (%s)" % (code, k, used[k]))
            for k in unused:
                print("%s unused: %r" % (code, k))
            failed |= bool(missing or dup)
        for k in bad:
            print("%s placeholder mismatch: %r" % (code, k))
        failed |= bool(bad)
        print("%s: %d strings, %d translated, %d missing, %d unused" % (code, len(used), len(used) - len(missing), len(missing), len(unused)))
    return 1 if failed else 0


sys.exit(main())

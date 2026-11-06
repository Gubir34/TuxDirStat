#!/usr/bin/env python3
"""Kaynaktaki çevrilecek metinlerin Türkçe tabloda (src/core/i18n_tr.inc) olduğunu denetler.

Bulunanlar: T("…") / TF("…") (yan yana yazılmış dizgiler birleştirilir), tools.cpp'deki
add("…") / addNode("…") öneri adları, yaş kovası etiketleri ve Tcl betiğindeki [T "…"].

Kullanım: tests/check_i18n.py [--missing]   (--missing: eksikleri tablo satırı olarak yazar)
"""
import os
import re
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
LIT = r'"(?:[^"\\\n]|\\.)*"'


def unescape(lit):
    body = lit[1:-1]
    return bytes(body, "utf-8").decode("unicode_escape").encode("latin-1").decode("utf-8")


def join_literals(text):
    return "".join(unescape(m) for m in re.findall(LIT, text))


def used_keys():
    keys = set()
    for dirpath, _, files in os.walk(os.path.join(ROOT, "src")):
        for f in files:
            if not f.endswith((".cpp", ".h")):
                continue
            src = open(os.path.join(dirpath, f), encoding="utf-8").read()
            src = re.sub(r"(?m)^\s*//.*$", "", src)
            for m in re.finditer(r'\b(?:T|TF|QT|QTF|N_)\(\s*((?:' + LIT + r'\s*)+)', src):
                keys.add(join_literals(m.group(1)))
            for m in re.finditer(r'\b(?:add|addNode)\(\s*(' + LIT + r')', src):
                keys.add(unescape(m.group(1)))
            for m in re.finditer(r'\{\s*[0-9A-Z_* ]*DAY[^,]*,\s*(' + LIT + r')', src):
                keys.add(unescape(m.group(1)))
            for m in re.finditer(r'\{INT64_MAX,\s*(' + LIT + r')', src):
                keys.add(unescape(m.group(1)))
            for m in re.finditer(r'\? "(Unused (?:Docker|Podman) data)"\s*:\s*"(Unused (?:Docker|Podman) data)"', src):
                keys.update(m.groups())
            # Tcl: [T "…"]
            for m in re.finditer(r'\[T "((?:[^"\\]|\\.)*)"\]', src):
                keys.add(m.group(1).replace('\\"', '"'))
    return keys


def table_keys():
    path = os.path.join(ROOT, "src", "core", "i18n_tr.inc")
    keys = []
    text = re.sub(r"(?m)^\s*//.*$", "", open(path, encoding="utf-8").read())
    for m in re.finditer(r'\{\s*((?:' + LIT + r'\s*)+),\s*((?:' + LIT + r'\s*)+)\}', text):
        keys.append(join_literals(m.group(1)))
    return keys


def main():
    used = used_keys()
    table = table_keys()
    dup = {k for k in table if table.count(k) > 1}
    missing = sorted(used - set(table))
    unused = sorted(set(table) - used)
    if "--missing" in sys.argv:
        for k in missing:
            print('{"%s", ""},' % k.replace("\\", "\\\\").replace('"', '\\"').replace("\n", "\\n"))
        return 0
    for k in sorted(dup):
        print("duplicate:", repr(k))
    for k in missing:
        print("missing:", repr(k))
    for k in unused:
        print("unused:", repr(k))
    print(f"{len(used)} strings, {len(missing)} missing, {len(unused)} unused, {len(dup)} duplicate")
    return 1 if missing or dup else 0


sys.exit(main())

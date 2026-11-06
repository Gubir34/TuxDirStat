"""Minimal reader for gettext .po files, shared by po2inc.py and tests/check_i18n.py."""
import re

_ESC = {"n": "\n", "t": "\t", "r": "\r", '"': '"', "\\": "\\", "a": "\a", "b": "\b", "f": "\f", "v": "\v"}


def unquote(lit):
    """Decodes one C-style quoted string from a .po file."""
    body = lit.strip()[1:-1]
    return re.sub(r'\\(.)', lambda m: _ESC.get(m.group(1), m.group(1)), body)


def read(path):
    """Returns (header dict, list of (msgid, msgstr, fuzzy)) for a .po file."""
    entries = []
    cur = {}
    field = None
    fuzzy = False

    def flush():
        nonlocal cur, fuzzy
        if "msgid" in cur:
            entries.append((cur["msgid"], cur.get("msgstr", ""), fuzzy))
        cur = {}
        fuzzy = False

    with open(path, encoding="utf-8") as f:
        for line in f:
            line = line.strip()
            if not line:
                flush()
                field = None
                continue
            if line.startswith("#,") and "fuzzy" in line:
                fuzzy = True
                continue
            if line.startswith("#"):
                continue
            m = re.match(r'(msgid|msgstr)\s+(".*")$', line)
            if m:
                if m.group(1) == "msgid" and "msgid" in cur:
                    flush()
                field = m.group(1)
                cur[field] = unquote(m.group(2))
            elif line.startswith('"') and field:
                cur[field] += unquote(line)
    flush()
    header = {}
    body = []
    for msgid, msgstr, fz in entries:
        if msgid == "":
            for hl in msgstr.split("\n"):
                if ":" in hl:
                    k, v = hl.split(":", 1)
                    header[k.strip()] = v.strip()
        else:
            body.append((msgid, msgstr, fz))
    return header, body


def quote(s):
    """C / .po string literal."""
    return '"' + s.replace("\\", "\\\\").replace('"', '\\"').replace("\n", "\\n").replace("\t", "\\t") + '"'

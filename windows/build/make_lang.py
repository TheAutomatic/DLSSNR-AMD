#!/usr/bin/env python3
"""The menus' English language file, from the code (windows/src/ui/nr_lang.hpp reads it):
    make_lang.py <out English.txt> <source files...> [--producers <files...>]

Every text the menus translate: the string literals inside tr(...) and NR_N(...), the texts given to the helpers that
translate what they are given (help, tooltip, section, text_dim_wrapped, ...), and the key tables' groups, labels,
help texts and combo items (*_keys.hpp: {"Name", kKind, ..., "group", "label", "help", "items"}). Adjacent literals
are joined as the compiler joins them. Each text once, in the order found; msgstr is the English text itself, so a copy
of the file is a complete language to translate in place.

The files after --producers make the texts the menus show as they come (nr_lang.hpp tr_text: a layer's note, HUD FIX's
state, a status line, the engine data): from them only NR_N(...), the notes given to layer_note / note_layer /
note_all / hudfix_now / unavailable, the literals assigned (= or +=) to `status`, `why`, `refused`, `hdr_why` and
`aco_why`, and - in nr_engine_panel.hpp - every literal (its lines' state mark left out).
"""
import re
import sys
from pathlib import Path

# function -> the arguments (0-based) whose literals are texts; None: every literal in every argument
CALLS = {
    "tr": None, "NR_N": None, "tr_items": None,
    "help": [0], "tooltip": [0], "section": [0], "text_dim_wrapped": [0],
    # the menus' own helpers and lambdas that translate their label / help text (nr_control_pages.hpp)
    "ini_flag": [1, 5], "ini_hotkey": [1, 3], "slider": [0, 5], "combo": [0, 5], "row": [0, 1],
}
# --producers: the calls whose arguments are texts shown as they come (member calls too: net_switch.unavailable(...))
PRODUCER_CALLS = {"NR_N": None, "layer_note": [3], "note_layer": [3], "note_all": [0], "hudfix_now": [0], "unavailable": [1]}
# and the literals assigned (= or +=) to these names: a session's status, HUD FIX's reasons
ASSIGNED = {"status", "why", "refused", "hdr_why", "aco_why"}   # (aco_why: ACO Mode off, the Windows version)
ESC = {"n": "\n", "t": "\t", "0": "\0", "\\": "\\", '"': '"', "'": "'", "r": "\r"}


def tokens(src):
    """(kind, text, pos): kind 's' a string literal (its value), 'i' an identifier, 'p' punctuation; comments and
    preprocessor lines skipped."""
    i, n = 0, len(src)
    while i < n:
        c = src[i]
        if c in " \t\r\n":
            i += 1
        elif src.startswith("//", i):
            i = src.find("\n", i)
            i = n if i < 0 else i
        elif src.startswith("/*", i):
            i = src.find("*/", i) + 2
        elif c == "#" and (i == 0 or src[src.rfind("\n", 0, i) + 1:i].strip() == ""):
            while True:   # a preprocessor line, with its continuations
                j = src.find("\n", i)
                if j < 0:
                    i = n
                    break
                if src[j - 1] != "\\":
                    i = j
                    break
                i = j + 1
        elif c == '"' or (c in "uUL" and src[i + 1:i + 2] == '"') or (src.startswith("u8", i) and src[i + 2:i + 3] == '"'):
            if src.startswith('R"', i):
                raise SystemExit("raw string literals are not read")
            i = src.index('"', i) + 1
            out = []
            while src[i] != '"':
                if src[i] == "\\":
                    e = src[i + 1]
                    if e == "x":
                        m = re.match(r"[0-9a-fA-F]+", src[i + 2:])
                        out.append(chr(int(m.group(0), 16)))
                        i += 2 + len(m.group(0))
                        continue
                    out.append(ESC.get(e, e))
                    i += 2
                else:
                    out.append(src[i])
                    i += 1
            yield "s", "".join(out), i
            i += 1
        elif c == "'":
            j = i + 1
            while src[j] != "'":
                j += 2 if src[j] == "\\" else 1
            i = j + 1
        elif c.isalpha() or c == "_":
            m = re.match(r"[A-Za-z_][A-Za-z0-9_]*", src[i:])
            yield "i", m.group(0), i
            i += len(m.group(0))
        else:
            yield "p", c, i
            i += 1


def joined(toks):
    """Adjacent string literals as one."""
    out = []
    for t in toks:
        if t[0] == "s" and out and out[-1][0] == "s":
            out[-1] = ("s", out[-1][1] + t[1], t[2])
        else:
            out.append(t)
    return out


def texts(path, producer=False):
    toks = joined(list(tokens(Path(path).read_text(encoding="utf-8"))))
    found = []
    keys_file = path.endswith("_keys.hpp") and not producer
    calls = PRODUCER_CALLS if producer else CALLS
    if producer and path.endswith("nr_engine_panel.hpp"):   # every literal, without the state mark
        return [re.sub(r"^[-+~=!](?=[A-Z])", "", t[1]) for t in toks
                if t[0] == "s" and not re.fullmatch(r"[A-Z0-9_]+", t[1])]   # (not the formats' names)
    for k, (kind, text, _) in enumerate(toks):
        # status = "..." (a producer's): the literals up to the ';'
        op = 1 if k + 1 < len(toks) and toks[k + 1][1] == "=" else 2 if k + 2 < len(toks) and toks[k + 1][1] == "+" and toks[k + 2][1] == "=" else 0
        if producer and kind == "i" and text in ASSIGNED and op and k + op + 1 < len(toks) and \
                toks[k + op][1] == "=" and toks[k + op + 1][1] != "=" and not (k > 0 and toks[k - 1][1] == "="):
            j = k + op + 1
            while j < len(toks) and toks[j][1] != ";":
                if toks[j][0] == "s":
                    found.append(toks[j][1])
                j += 1
        # name( ... ): the arguments at top level
        member = k > 0 and (toks[k - 1][1] == "." or (k > 1 and toks[k - 1][1] == ">" and toks[k - 2][1] == "-"))
        if kind == "i" and text in calls and k + 1 < len(toks) and toks[k + 1][1] == "(" and \
                (not member or (producer and text == "unavailable")):
            depth, arg, j = 0, 0, k + 1
            want = calls[text]
            while j < len(toks):
                t = toks[j]
                if t[1] in "([{" and t[0] == "p":
                    depth += 1
                elif t[1] in ")]}" and t[0] == "p":
                    depth -= 1
                    if depth == 0:
                        break
                elif t[1] == "," and t[0] == "p" and depth == 1:
                    arg += 1
                elif t[0] == "s" and (want is None or arg in want):
                    found.append(t[1])
                j += 1
        # a key table's entry: {"Name", kKind, decimals, lo, hi, def, "group", "label", "help", items}
        if keys_file and kind == "p" and text == "{" and k + 2 < len(toks) and toks[k + 1][0] == "s" and \
                toks[k + 2][1] == "," and toks[k + 3][0] == "i" and toks[k + 3][1].startswith("k"):
            fields, j, depth, cur = [], k + 1, 1, []
            while j < len(toks) and depth > 0:
                t = toks[j]
                if t[0] == "p" and t[1] in "({":
                    depth += 1
                elif t[0] == "p" and t[1] in ")}":
                    depth -= 1
                    if depth == 0:
                        fields.append(cur)
                        break
                if depth == 1 and t[0] == "p" and t[1] == ",":
                    fields.append(cur)
                    cur = []
                else:
                    cur.append(t)
                j += 1
            for f in fields[6:9]:
                found += [t[1] for t in f if t[0] == "s"]
            if len(fields) > 9:
                for t in fields[9]:
                    if t[0] == "s":
                        found += [x for x in t[1].split("\0") if x]
    return found


def quote(s):
    return '"' + s.replace("\\", "\\\\").replace('"', '\\"').replace("\n", "\\n").replace("\t", "\\t") + '"'


def main():
    out, args = sys.argv[1], sys.argv[2:]
    cut = args.index("--producers") if "--producers" in args else len(args)
    sources = [(a, False) for a in args[:cut]] + [(a, True) for a in args[cut + 1:]]
    seen, lines = set(), []
    lines += [
        "# DLSSNR-AMD: the menus' texts in English (NR Control and the ReShade Add-ons page).",
        "# This file is made from the program at every build and replaced by every install: do not translate it",
        "# here. Copy it to a file of your own in this folder (README.txt says how) and translate that one.",
        "",
        'msgid "@language"',
        'msgstr "English"',
        "",
        "# A font file for the letters of your language (in this folder, or a full path); empty: NR Control's own,",
        "# which has Latin, Greek and Cyrillic letters only.",
        'msgid "@font"',
        'msgstr ""',
    ]
    for src, producer in sources:
        # (a text of conversions and signs only, "%s" or "%u x %u", has nothing to translate)
        new = [t for t in texts(src, producer) if re.search(r"[A-Za-z]{2}", re.sub(r"%[-+#0-9.*hljztL]*[diouxXeEfgGaAcspn]", "", t))
               and not t.startswith("##")]
        new = [t.split("##")[0] if "##" in t else t for t in new]
        new = [t for t in new if t and t not in seen and not seen.add(t)]
        if not new:
            continue
        lines += ["", "# ---- " + Path(src).name + (" (shown as it comes: notes, states, status lines)" if producer else "")]
        for t in new:
            lines += ["", "msgid " + quote(t), "msgstr " + quote(t)]
    Path(out).write_text("\n".join(lines) + "\n", encoding="utf-8", newline="\r\n")
    print(f"{out}: {len(seen)} texts from {len(sources)} files")


if __name__ == "__main__":
    main()

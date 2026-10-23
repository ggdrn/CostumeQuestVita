#!/usr/bin/env python3
"""
Turns a Costume Quest PC translation (xdelta patches for the Steam packs)
into string tables the Vita port loads from translations/<name>/.

    translation_from_pc.py <pc_translation_dir> <CostumeQuestPowerVR.apk> <out_dir>

The PC translation is a set of xdelta3 (VCDIFF) patches for Win/Packs/
RgB_Stuff.~p and DLC1_Stuff.~p. Without the PC game they can't be applied,
but every string table they rewrite is new data inside the patch: decoding
against an all-zero source leaves those tables intact (bytes copied from the
original come out as zeros). The tables use the same text format as the
Android build ("StringTable{LineCodeData={KEY=LineCodeData{Text=...}}}").

For each of the game's tables (main game, DLC) the one whose text matches
none of the APK's languages is the translation. Keys it lacks are taken
from the APK's English table; tables the translation doesn't cover
(cq1patch1, cqmobile, cqpc) are written in English, so the game shows no
leftover text of the language the translation replaces on the Vita.

Needs xdelta3 (macOS: brew install xdelta). The output holds the
translation's text and the game's English text: keep it with your game
files, don't redistribute it.
"""

import io
import os
import re
import shutil
import struct
import subprocess
import sys
import tempfile
import zipfile
import zlib

PATCHES = ["patch_p.xdelta", "patch_dlc_p.xdelta"]
TABLES = ["costumequest", "costumequestdlc1"]          # translated by the PC patch
ENGLISH_ONLY = ["cq1patch1", "cqmobile", "cqpc"]       # not in the PC translation
APK_LANGS = ["enus", "engb", "frfr", "dede", "eses", "itit", "leet",
             "usenglish", "ukenglish", "french", "german", "spanish", "italian"]
DATA_ENTRY = "assets/ant.mp3"
TABLE_PATH = "OGLES/Munged/stringtable/%s_%s.STbl~c"


def fail(msg):
    sys.exit("error: " + msg)


# --- string tables -----------------------------------------------------------

def parse_entries(text):
    """(header, [(key, raw 'KEY=LineCodeData{...};')], tail) of a StringTable."""
    head = text.find(b"LineCodeData={")
    if not text.startswith(b"StringTable{") or head < 0:
        raise ValueError("not a StringTable")
    pos = head + len(b"LineCodeData={")
    entries = []
    while True:
        m = re.compile(rb"([A-Za-z0-9_]+)=LineCodeData\{").match(text, pos)
        if not m:
            break
        i = m.end()
        in_str = False
        while i < len(text):
            ch = text[i:i + 1]
            if in_str:
                if ch == b"\\":
                    i += 1
                elif ch == b'"':
                    in_str = False
            elif ch == b'"':
                in_str = True
            elif ch == b"}":
                break
            i += 1
        if text[i:i + 2] != b"};":
            raise ValueError("bad entry at %d" % pos)
        entries.append((m.group(1).decode(), text[pos:i + 2]))
        pos = i + 2
    return text[:head + len(b"LineCodeData={")], entries, text[pos:]


def texts(entries):
    out = {}
    for key, raw in entries:
        m = re.search(rb'Text="((?:[^"\\]|\\.)*)"', raw)
        out[key] = m.group(1) if m else b""
    return out


def load_apk_tables(apk_path):
    """{(table, lang): text} from the game data inside the APK."""
    with zipfile.ZipFile(apk_path) as apk:
        data = zipfile.ZipFile(io.BytesIO(apk.read(DATA_ENTRY)))
    tables = {}
    for name in TABLES + ENGLISH_ONLY:
        for lang in APK_LANGS:
            try:
                d = data.read(TABLE_PATH % (name, lang))
            except KeyError:
                continue
            flags = struct.unpack(">H", d[0x16:0x18])[0]
            body = zlib.decompress(d[0x40:]) if flags == 0x0102 else d[0x40:]
            tables[(name, lang)] = body.rstrip(b"\0")
    return tables


# --- patches -----------------------------------------------------------------

def decode_patch(patch, workdir):
    """Target of an xdelta patch decoded against an all-zero source."""
    zero = os.path.join(workdir, "zero")
    with open(zero, "wb") as f:
        f.truncate(1 << 30)  # sparse: covers any copy window
    out = os.path.join(workdir, os.path.basename(patch) + ".out")
    subprocess.run(["xdelta3", "-d", "-n", "-f", "-s", zero, patch, out], check=True,
                   stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    with open(out, "rb") as f:
        return f.read()


def find_string_tables(data):
    """Every StringTable zlib stream in the decoded data, as text."""
    regions = []
    for m in re.finditer(rb"[^\x00]+", data):
        if regions and m.start() - regions[-1][1] < 64:
            regions[-1][1] = m.end()
        else:
            regions.append([m.start(), m.end()])
    # Bytes equal to the original (the zlib header, sometimes the first
    # deflate byte) were copied from the zero source: try them back
    prefixes = [b""] + [b"\x78\xda"] + [b"\x78\xda" + bytes([x]) for x in range(256)]
    found = []
    for a, b in regions:
        if b - a < 200:
            continue
        seg = data[a:]
        for pre in prefixes:
            try:
                z = zlib.decompressobj(15 if pre else -15)
                head = z.decompress(pre + seg[:4096], 64)
            except zlib.error:
                continue
            if b"StringTable{" not in head[:16]:
                continue
            z = zlib.decompressobj(15 if pre else -15)
            text = b""
            stream = pre + seg[:4 << 20]
            for k in range(0, len(stream), 65536):
                try:
                    text += z.decompress(stream[k:k + 65536])
                except zlib.error:
                    break  # checksum bytes copied from the source: the text is complete anyway
                if z.eof:
                    break
            # PC tables start with their length
            if len(text) > 4 and struct.unpack("<I", text[:4])[0] == len(text) - 4:
                text = text[4:]
            if text.startswith(b"StringTable{") and text.rstrip(b"\0").endswith(b"};}"):
                found.append(text.rstrip(b"\0"))
            break
    return found


# --- main --------------------------------------------------------------------

def main():
    if len(sys.argv) != 4:
        sys.exit(__doc__)
    pc_dir, apk_path, out_dir = sys.argv[1:]
    if not shutil.which("xdelta3"):
        fail("xdelta3 not found (macOS: brew install xdelta)")
    patch_dir = os.path.join(pc_dir, "dados") if os.path.isdir(os.path.join(pc_dir, "dados")) else pc_dir

    apk_tables = load_apk_tables(apk_path)
    candidates = []
    with tempfile.TemporaryDirectory() as work:
        for p in PATCHES:
            path = os.path.join(patch_dir, p)
            if os.path.exists(path):
                print("decoding", p)
                candidates += find_string_tables(decode_patch(path, work))
    print("string tables in the patches:", len(candidates))

    os.makedirs(out_dir, exist_ok=True)
    for name in TABLES:
        english = parse_entries(apk_tables[(name, "enus")])
        en_keys = {k for k, _ in english[1]}
        known = [texts(parse_entries(t)[1]) for (n, _), t in apk_tables.items() if n == name]
        best = None
        for cand in candidates:
            try:
                head, entries, tail = parse_entries(cand)
            except ValueError:
                continue
            tx = texts(entries)
            if len(en_keys & set(tx)) < 0.9 * len(en_keys):
                continue  # another table
            # A language the APK already has? (same text on the sampled keys)
            sample = [k for k in sorted(en_keys & set(tx)) if len(tx[k]) > 12][:40]
            if any(all(lang.get(k) == tx[k] for k in sample) for lang in known):
                continue
            if not best or len(entries) > len(best[1]):
                best = (head, entries, tail)
        if not best:
            fail("no translated %s table in the patches" % name)
        head, entries, tail = best
        have = {k for k, _ in entries}
        missing = [(k, raw) for k, raw in english[1] if k not in have]
        text = head + b"".join(raw for _, raw in entries + missing) + tail
        with open(os.path.join(out_dir, name + ".STbl"), "wb") as f:
            f.write(text)
        print("%s.STbl: %d entries translated, %d from English" % (name, len(entries), len(missing)))

    for name in ENGLISH_ONLY:
        if (name, "enus") in apk_tables:
            with open(os.path.join(out_dir, name + ".STbl"), "wb") as f:
                f.write(apk_tables[(name, "enus")])
            print("%s.STbl: English (not in the translation)" % name)

    with open(os.path.join(out_dir, "translation.txt"), "w") as f:
        f.write("Built by translation_from_pc.py from %s\n" % os.path.basename(os.path.abspath(pc_dir)))
    print("done:", out_dir)


if __name__ == "__main__":
    main()

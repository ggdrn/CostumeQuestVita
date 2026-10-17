#!/usr/bin/env python3
"""
Builds the LiveArea images from the user's own Costume Quest APK.

The game's artwork is not part of this repository: it is read from the APK at
build time and written to an output folder that git ignores. The resulting
VPK is for personal use only.

    livearea_from_apk.py <game.apk> <out_dir>

Writes icon0.png (128x128), pic0.png (960x544), bg0.png (840x500) and
startup.png (280x158) as 8-bit palette PNGs, as the LiveArea requires.

Sources inside the APK:
  res/drawable-xxhdpi-v4/ic_launcher.png   launcher icon
  assets/ant.mp3 (a zip with the game data):
    Data/PS3/ICON0.PNG                      "Costume Quest" logo
    OGLES/Munged/ui/flash/frontend/whole_bg.Txtr~c          menu background
    OGLES/Munged/ui/textures/loadingtextures/*.Txtr~c       loading screens
"""

import io
import os
import struct
import sys
import zipfile
import zlib

from PIL import Image

ICON = "res/drawable-xxhdpi-v4/ic_launcher.png"
DATA = "assets/ant.mp3"
LOGO = "Data/PS3/ICON0.PNG"
MENU_BG = "OGLES/Munged/ui/flash/frontend/whole_bg.Txtr~c"
LOADING = "OGLES/Munged/ui/textures/loadingtextures/cq_suburbs_mobile.Txtr~c"


def decode_texture(blob):
    """Munged texture: 'dfcf' header, zlib stream at 0x40 holding a DDS
    with uncompressed 32-bit pixels (B, G, R, A in memory)."""
    if blob[:4] != b"dfcf":
        raise ValueError("not a dfcf texture")
    raw = zlib.decompress(blob[0x40:])
    dds = raw.find(b"DDS ")
    if dds < 0:
        raise ValueError("no DDS header")
    height, width = struct.unpack_from("<II", raw, dds + 12)
    flags, fourcc, bits = struct.unpack_from("<III", raw, dds + 80)
    if fourcc != 0 or bits != 32:
        raise ValueError("unsupported DDS format (fourcc %08x, %d bpp)" % (fourcc, bits))
    pixels = raw[dds + 128: dds + 128 + width * height * 4]
    img = Image.frombuffer("RGBA", (width, height), pixels, "raw", "BGRA", 0, 1)
    return img.convert("RGB")


def cover(img, size):
    """Scale to fill size, cropping the overflow evenly."""
    w, h = size
    scale = max(w / img.width, h / img.height)
    img = img.resize((round(img.width * scale), round(img.height * scale)), Image.LANCZOS)
    left = (img.width - w) // 2
    top = (img.height - h) // 2
    return img.crop((left, top, left + w, top + h))


def contain(img, size):
    """Scale to fit inside size, keeping the aspect ratio."""
    scale = min(size[0] / img.width, size[1] / img.height)
    return img.resize((round(img.width * scale), round(img.height * scale)), Image.LANCZOS)


def save_p8(img, path):
    img.convert("RGB").quantize(256, method=Image.MEDIANCUT, dither=Image.FLOYDSTEINBERG).save(path, optimize=True)


def main():
    if len(sys.argv) != 3:
        sys.exit(__doc__)
    apk_path, out = sys.argv[1], sys.argv[2]
    os.makedirs(out, exist_ok=True)

    with zipfile.ZipFile(apk_path) as apk:
        icon = Image.open(io.BytesIO(apk.read(ICON))).convert("RGBA")
        with zipfile.ZipFile(apk.open(DATA)) as data:
            logo = Image.open(io.BytesIO(data.read(LOGO))).convert("RGBA")
            menu_bg = decode_texture(data.read(MENU_BG))
            loading = decode_texture(data.read(LOADING))

    # icon0: launcher icon
    save_p8(icon.resize((128, 128), Image.LANCZOS), os.path.join(out, "icon0.png"))

    # pic0: shown while the app boots
    save_p8(cover(loading, (960, 544)), os.path.join(out, "pic0.png"))

    # bg0: LiveArea background
    save_p8(cover(menu_bg, (840, 500)), os.path.join(out, "bg0.png"))

    # startup: the logo over the menu background
    gate = cover(menu_bg, (280, 158)).convert("RGBA")
    mark = contain(logo, (264, 146))
    gate.alpha_composite(mark, ((280 - mark.width) // 2, (158 - mark.height) // 2))
    save_p8(gate, os.path.join(out, "startup.png"))

    print("LiveArea images from %s -> %s" % (apk_path, out))


if __name__ == "__main__":
    main()

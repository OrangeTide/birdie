#!/usr/bin/env python3
"""Extract OPEN LOOK pushpin glyphs from BDF fonts and write PNGs.

Reads composite glyphs at encodings 19 (out), 20 (in), 21 (default)
from each BDF file and writes white-on-transparent PNGs.

Usage: python3 scripts/bdf2pushpin.py <bdf_dir> <output_dir>
"""

import os
import sys
from PIL import Image


def parse_bdf_glyphs(path, encodings):
    """Extract glyphs by encoding number from a BDF file."""
    glyphs = {}
    with open(path) as f:
        lines = f.readlines()

    i = 0
    while i < len(lines):
        line = lines[i].strip()
        if line.startswith("STARTCHAR"):
            name = line.split(None, 1)[1]
            enc = None
            bbw = bbh = 0
            bitmap = []
            i += 1
            while i < len(lines) and not lines[i].strip().startswith("ENDCHAR"):
                l = lines[i].strip()
                if l.startswith("ENCODING"):
                    enc = int(l.split()[1])
                elif l.startswith("BBX"):
                    parts = l.split()
                    bbw, bbh = int(parts[1]), int(parts[2])
                elif l.startswith("BITMAP"):
                    i += 1
                    while i < len(lines) and not lines[i].strip().startswith("ENDCHAR"):
                        bitmap.append(lines[i].strip())
                        i += 1
                    continue
                i += 1
            if enc in encodings:
                glyphs[enc] = (name, bbw, bbh, bitmap)
        i += 1
    return glyphs


def bitmap_to_image(bbw, bbh, bitmap_hex):
    """Convert BDF hex bitmap to a PIL Image (white on transparent)."""
    img = Image.new("RGBA", (bbw, bbh), (0, 0, 0, 0))
    for row, hexstr in enumerate(bitmap_hex):
        if row >= bbh:
            break
        val = int(hexstr, 16)
        # BDF pads each row to byte boundary; total bits = len(hexstr)*4
        total_bits = len(hexstr) * 4
        for col in range(bbw):
            bit = total_bits - 1 - col
            if val & (1 << bit):
                img.putpixel((col, row), (255, 255, 255, 255))
    return img


STATE_NAMES = {19: "out", 20: "in", 21: "default"}


def main():
    if len(sys.argv) < 3:
        print(f"Usage: {sys.argv[0]} <bdf_dir> <output_dir>")
        sys.exit(1)

    bdf_dir = sys.argv[1]
    out_dir = sys.argv[2]
    os.makedirs(out_dir, exist_ok=True)

    bdf_files = sorted(
        f for f in os.listdir(bdf_dir) if f.startswith("olgl") and f.endswith(".bdf")
    )

    for bdf_name in bdf_files:
        size_tag = bdf_name.replace("olgl", "").replace(".bdf", "")
        path = os.path.join(bdf_dir, bdf_name)
        glyphs = parse_bdf_glyphs(path, {19, 20, 21})

        for enc, (name, bbw, bbh, bitmap) in sorted(glyphs.items()):
            state = STATE_NAMES[enc]
            img = bitmap_to_image(bbw, bbh, bitmap)
            out_path = os.path.join(out_dir, f"pushpin-{state}-{size_tag}.png")
            img.save(out_path)
            print(f"  {out_path}  ({bbw}x{bbh})  [{name}]")


if __name__ == "__main__":
    main()

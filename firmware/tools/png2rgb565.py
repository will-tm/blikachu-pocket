#!/usr/bin/env python3
"""Convert PNG assets to RGB565 C headers for the ST7789V display.

Usage: png2rgb565.py <input.png> <output.h> [max_size]

Pixels with alpha < 128 become black. Output is count/value RLE of
big-endian RGB565. Optional max_size rescales the longest edge.
"""

import sys
from pathlib import Path

from PIL import Image
from gif2frames import encode_rle


def convert(src: Path, dst: Path, max_size: int | None = None):
    img = Image.open(src).convert("RGBA")
    if max_size:
        w, h = img.size
        scale = max_size / max(w, h)
        if scale < 1.0:
            img = img.resize((int(w * scale), int(h * scale)), Image.LANCZOS)
    w, h = img.size
    name = src.stem

    pixels = []
    for y in range(h):
        for x in range(w):
            r, g, b, a = img.getpixel((x, y))
            if a < 128:
                r, g, b = 0, 0, 0
            rgb565 = ((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3)
            # Byte-swap: the ST7789V receives bytes MSB-first, so the
            # uint16_t in memory must be big-endian (same convention as
            # the RGB565() macro in gfx.h).
            pixels.append(((rgb565 & 0xFF) << 8) | (rgb565 >> 8))

    words = encode_rle(pixels)
    guard = f"ASSET_{name.upper()}_H"
    with open(dst, "w") as f:
        f.write(f"#ifndef {guard}\n#define {guard}\n\n")
        f.write("#include <stdint.h>\n\n")
        f.write(f"#define {name.upper()}_ICON_W {w}\n")
        f.write(f"#define {name.upper()}_ICON_H {h}\n\n")
        f.write(
            f"static const uint16_t {name}_icon_rle[{len(words)}] = {{\n"
        )
        for i in range(0, len(words), 8):
            vals = words[i : i + 8]
            f.write("\t" + ", ".join(f"0x{v:04X}" for v in vals) + ",\n")
        f.write("};\n\n#endif\n")


if __name__ == "__main__":
    src = Path(sys.argv[1])
    dst = Path(sys.argv[2])
    max_size = int(sys.argv[3]) if len(sys.argv) > 3 else None
    convert(src, dst, max_size)

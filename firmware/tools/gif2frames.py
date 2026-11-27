#!/usr/bin/env python3
"""Convert GIF frames to lossless count/value RLE of wire-order RGB565 pixels."""

import sys
from pathlib import Path

from PIL import Image


def encode_rle(pixels):
    """Return uint16 count/value pairs; runs never exceed UINT16_MAX."""
    words = []
    count = 0
    last = None
    for pixel in pixels:
        if pixel == last and count < 65535:
            count += 1
        else:
            if count:
                words.extend((count, last))
            last, count = pixel, 1
    if count:
        words.extend((count, last))
    return words


def frame_pixels(frame):
    for r, g, b in frame.getdata():
        value = ((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3)
        yield ((value & 0xFF) << 8) | (value >> 8)


def convert(src: Path, dst_c: Path, dst_h: Path):
    with Image.open(src) as img:
        name = src.stem
        words, delays, offsets = [], [], [0]
        canvas = Image.new("RGBA", img.size, (0, 0, 0, 255))
        while True:
            canvas.alpha_composite(img.convert("RGBA"))
            words.extend(encode_rle(frame_pixels(canvas.convert("RGB"))))
            offsets.append(len(words))
            delays.append(max(20, int(img.info.get("duration", 100))))
            try:
                img.seek(img.tell() + 1)
            except EOFError:
                break
        w, h = img.size
    n = len(delays)

    with dst_h.open("w") as f:
        guard = f"ASSET_{name.upper()}_ANIM_H"
        f.write(f"#ifndef {guard}\n#define {guard}\n\n#include <stdint.h>\n\n")
        f.write(f"#define {name.upper()}_ANIM_W {w}\n")
        f.write(f"#define {name.upper()}_ANIM_H {h}\n")
        f.write(f"#define {name.upper()}_ANIM_FRAMES {n}\n\n")
        f.write(f"extern const uint16_t {name}_anim_rle[{len(words)}];\n")
        f.write(f"extern const uint32_t {name}_anim_offsets[{n + 1}];\n")
        f.write(f"extern const uint16_t {name}_anim_delay_ms[{n}];\n\n#endif\n")

    with dst_c.open("w") as f:
        f.write(f'#include "{dst_h.name}"\n\n')
        f.write(f"const uint16_t {name}_anim_delay_ms[{n}] = {{")
        f.write(", ".join(str(d) for d in delays))
        f.write("};\n\n")
        f.write(f"const uint32_t {name}_anim_offsets[{n + 1}] = {{")
        f.write(", ".join(str(o) for o in offsets))
        f.write("};\n\n")
        f.write(f"const uint16_t {name}_anim_rle[{len(words)}] = {{\n")
        for i in range(0, len(words), 12):
            f.write(",".join(f"0x{v:04X}" for v in words[i:i + 12]) + ",\n")
        f.write("};\n")


if __name__ == "__main__":
    convert(Path(sys.argv[1]), Path(sys.argv[2]), Path(sys.argv[3]))

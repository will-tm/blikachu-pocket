#!/usr/bin/env python3
"""Build the BL61x whole-flash image (BFNP boot header + app) offline.

Usage: bflb_whole_img.py <zephyr.bin> <out.bin>

Runs `bflb-mcu-tool-uart --build` (no device needed) and copies the
whole_img.bin it leaves in its package directory. The result is what
the BootROM boots from flash offset 0: the 8 KiB header carries the
app length (0x84) and SHA-256 (0x88), so an app-only image can't be
installed on its own.

SPDX-License-Identifier: Apache-2.0
"""

import os
import shutil
import subprocess
import sys

import bflb_mcu_tool
from bflb_flash_support import configure_flash


def main():
    if len(sys.argv) != 3:
        sys.exit(__doc__)
    app, out = sys.argv[1], sys.argv[2]
    configure_flash()
    whole = os.path.join(os.path.dirname(bflb_mcu_tool.__file__),
                         "chips", "bl616", "img_create_mcu", "whole_img.bin")
    if os.path.exists(whole):
        os.remove(whole)
    # Prefer the tool installed next to this interpreter (the venv).
    tool = os.path.join(os.path.dirname(sys.executable), "bflb-mcu-tool-uart")
    if not os.path.exists(tool):
        tool = "bflb-mcu-tool-uart"
    subprocess.run([tool, "--chipname", "bl616", "--build",
                    "--firmware", app],
                   check=True, stdout=subprocess.DEVNULL)
    with open(whole, "rb") as f:
        if f.read(4) != b"BFNP":
            sys.exit(f"{whole}: no BFNP boot header")
    shutil.copyfile(whole, out)
    print(f"DFU image: {out} ({os.path.getsize(out)} bytes)")


if __name__ == "__main__":
    main()

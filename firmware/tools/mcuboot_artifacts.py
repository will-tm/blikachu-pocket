#!/usr/bin/env python3
"""Package bootloader, signed application and UF2 without hashing mutable slots into BFNP."""
import hashlib
import struct
import subprocess
import sys
from pathlib import Path
from elftools.elf.elffile import ELFFile

ROOT = Path(__file__).resolve().parent.parent
BUILD = ROOT / "build"
SLOT0 = 0x80000
SLOT1 = 0x43F000
FW_END = 0x800000
FAMILY = 0xF3478D12
XIP = 0xA0000000
KEY = ROOT / ".keys/mcuboot.pem"


def check_uf2(path, image):
    data = path.read_bytes()
    blocks = (len(image) + 255) // 256
    if len(data) != blocks * 512:
        raise ValueError("UF2 block count mismatch")
    for i in range(blocks):
        b = data[i * 512:(i + 1) * 512]
        expected = (0x0A324655, 0x9E5D5157, 0x2000,
                    XIP + SLOT0 + i * 256, 256, i, blocks, FAMILY)
        payload = image[i * 256:(i + 1) * 256]
        if (struct.unpack_from("<8I", b) != expected or
                struct.unpack_from("<I", b, 508)[0] != 0x0AB16F30 or
                b[32:32 + len(payload)] != payload):
            raise ValueError(f"Invalid UF2 block {i}")


def check_symbols(path, required):
    with path.open("rb") as stream:
        elf = ELFFile(stream)
        names = {symbol.name for symbol in elf.get_section_by_name(".symtab").iter_symbols()}
    if missing := required - names:
        raise ValueError(f"{path}: recovery symbols not linked: {sorted(missing)}")


def main():
    boot = BUILD / "mcuboot/zephyr/zephyr.bin"
    app = BUILD / "firmware/zephyr/zephyr.signed.bin"
    bfnp = BUILD / "mcuboot/zephyr/bootloader.bfnp.bin"
    check_symbols(boot.with_suffix(".elf"), {
        "blikachu_uf2_init", "blikachu_uf2_complete", "blikachu_dfu_requested",
        "dfu_screen_message", "dfu_screen_progress", "sys_arch_reboot",
    })
    check_symbols(app.with_name("zephyr.elf"), {"cmd_dfu", "boot_write_img_confirmed"})
    subprocess.run([sys.executable, str(ROOT / "tools/bflb_whole_img.py"), str(boot), str(bfnp)], cwd=BUILD, check=True)
    blob, image = bfnp.read_bytes(), app.read_bytes()
    length = struct.unpack_from("<I", blob, 0x84)[0]
    if blob[:4] != b"BFNP" or length != len(blob) - 0x2000 or len(blob) > 0x7F000:
        raise ValueError("Invalid BL618 bootloader bounds")
    if hashlib.sha256(blob[0x2000:]).digest() != blob[0x88:0xA8]:
        raise ValueError("BootROM image hash mismatch")
    if len(image) > min(SLOT1 - SLOT0 - 0x2000, 3584 * 1024):
        raise ValueError("Signed application does not fit update slots/staging buffer")
    subprocess.run([sys.executable, str(ROOT / "deps/bootloader/mcuboot/scripts/imgtool.py"),
                    "verify", "-k", str(KEY), str(app)], check=True)
    factory = bytearray(b"\xff" * FW_END)
    factory[:len(blob)] = blob
    factory[SLOT0:SLOT0 + len(image)] = image
    path = BUILD / "factory.bin"
    path.write_bytes(factory)
    uf2 = BUILD / "firmware.uf2"
    subprocess.run([sys.executable, str(ROOT / "deps/zephyr/scripts/build/uf2conv.py"), "--convert",
                    "-b", hex(XIP + SLOT0), "-f", hex(FAMILY), "-o", str(uf2), str(app)], check=True)
    check_uf2(uf2, image)
    print(f"Factory: {path} ({len(factory)} bytes, ends at storage boundary)")
    print(f"Update: {uf2}")


if __name__ == "__main__":
    main()

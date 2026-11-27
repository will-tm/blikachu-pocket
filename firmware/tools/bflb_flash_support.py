"""Register the production W25Q512JV profile with the vendor tools.

The vendor tool stores generated images and flash profiles in its package
workspace. Install our profile there so both offline image generation and
BootROM programming select the same three-byte boot configuration. Firmware
enables four-byte addressing after startup for the full mass-storage range.
"""

import csv
import re
import shutil
from pathlib import Path

import bflb_mcu_tool


def configure_flash():
    package = Path(bflb_mcu_tool.__file__).resolve().parent
    profiles = package / "utils" / "flash" / "bl616"
    name = "W25Q512JV_ef4020.conf"
    shutil.copyfile(Path(__file__).parent / "flash" / name, profiles / name)

    table = profiles / "flashcfg_list.csv"
    with table.open(encoding="utf-8-sig", newline="") as stream:
        reader = csv.DictReader(stream)
        fields = reader.fieldnames
        rows = [row for row in reader if row["flashJedecID"].lower() != "ef4020"]
    rows.append({"flashJedecID": "ef4020", "configFile": name, "flashSize": "64"})
    with table.open("w", newline="", encoding="utf-8") as stream:
        writer = csv.DictWriter(stream, fieldnames=fields)
        writer.writeheader()
        writer.writerows(rows)

    # Both the initial image and the post-JEDEC regenerated image need
    # this profile; fixing only the reported capacity is insufficient.
    loader = package / "chips" / "bl616" / "eflash_loader"
    for suffix in ("conf", "ini"):
        path = loader / ("eflash_loader_cfg." + suffix)
        if path.exists():
            text = path.read_text()
            text = re.sub(r"(?m)^flash_id\s*=.*$", "flash_id = ef4020", text)
            # Verify actual programmed bytes through the serial command
            # path as well as hashing; do not rely solely on loader XIP.
            text = re.sub(r"(?m)^verify\s*=.*$", "verify = 1", text)
            path.write_text(text)

    # A generic mfg_id=0xff makes update_flash_cfg() skip the selected
    # profile and force I/O mode 0x11. Select Winbond explicitly instead.
    for relative in ("img_create_mcu/efuse_bootheader_cfg.ini",
                     "efuse_bootheader/efuse_bootheader_cfg.conf"):
        path = package / "chips" / "bl616" / relative
        if path.exists():
            text = re.sub(r"(?m)^mfg_id\s*=.*$", "mfg_id = 0xef", path.read_text())
            path.write_text(text)

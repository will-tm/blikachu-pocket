# BLikachu Pocket firmware

BL618 handheld firmware on **Zephyr v4.5.0**, with GB/GBC, NES and SNES
emulation, ST7789V display, buttons, buzzer, save states, WiFi ROM management
and USB firmware updates.

## Build

Requires Python 3, CMake, Ninja, Zephyr SDK 1.0.1 with the RISC-V toolchain,
and `dfu-util` for USB updates. On macOS:

```bash
brew install cmake ninja dtc dfu-util
./build.sh
```

`build.sh` creates `.venv`, fetches the pinned west workspace and vendor blobs,
applies project patches and builds `blikachu_pocket/bl618m05q2i` into `build/`.
Use `./build.sh clean` for a pristine rebuild.

Outputs in `build/zephyr/`:

- `zephyr.dfu.bin`: whole-flash image for USB DFU and WiFi OTA.
- `zephyr.bin`: application binary for the BootROM flash tool.
- `zephyr.elf`: debugging symbols.

## Libretro development

Gambatte (GB/GBC), FCEUmm (NES) and Snes9x2005 (SNES) are pinned in
`west.yml`. One firmware selects the appropriate core from the ROM header
and extension. Only one core is active at a time.

`lrprobe <path> [frames]` runs a headless core diagnostic, including a
save-state round trip and heap/stack measurements. `perfclock` reports the configured clock tree and measured CPU frequency.
The board uses a 480 MHz AU PLL CPU overclock, while BCLK stays at
80 MHz and the SPI peripheral source stays at 160 MHz. `emu info` reports
emulated frames and completed LCD pushes separately, including unchanged
frames and strips transferred. Core-event checkpoints let concurrent
SPI DMA make progress without changing emulated cycles. Two uncached WRAM
buffers alternate up to 32 rows per DMA transfer; exact native pixel
comparisons omit unchanged strips and letterbox rows from the queue.
`lcdbench` and `lrdmaprobe` measure DMA independently and during gameplay.
Full-speed emulation and presentation are not established for every
system or scene.

```bash
.venv/bin/west build -d build -t libretro_cores
cc -std=c11 -Wall -Wextra -Werror -fsanitize=address,undefined -Isrc \
  tests/libretro_content.c src/libretro/content.c -o build/libretro_content
build/libretro_content
.venv/bin/python -I tests/snes_readonly.py
```

The SNES port accepts bounded, normalized ordinary LoROM/HiROM cartridges,
keeps ROM bytes read-only, and rejects unaudited enhancement-chip and
transformation-dependent images. Core licenses remain in their dependency
repositories; Snes9x includes non-commercial distribution restrictions.

PSRAM is partitioned into 512 KiB for firmware buffers and a 3.5 MiB
exclusive core/update workspace at `0xa8080000`. Core writable sections
have linker overlays; the frontend must initialize only the selected core
while holding the workspace claim. The linker usage report does not include
runtime heap use or raw update-staging accesses to this reserved region.
The DFU/upload payload limit is 3.5 MiB.

## Firmware updates

```bash
./build.sh flash
```

With the firmware running, this uses USB DFU, without button presses:

```bash
dfu-util -d 2e8a:000a -D build/zephyr/zephyr.dfu.bin
```

Keep power connected until the board finishes writing flash and reboots.
**`Download done` means the transfer has finished, not the installation.**
An active game is stopped and unloaded before staging. The image stages in
the shared 3.5 MiB PSRAM upload buffer, is checked against
its boot-header CRC and application SHA-256, and is written and read back
before rebooting.
Images larger than 3.5 MiB are rejected.
Older firmware that stages only 3 MiB cannot receive a larger image directly;
use a compatible staging firmware or the BootROM recovery path.

Installation is in-place, not A/B: loss of power during writing may require
BootROM recovery. The checksum detects corruption; it is not an authenticity
signature.

### BootROM recovery / first flash

Hold BOOT while resetting, then release BOOT. The native USB port identifies
as `Bouffalo CDC DEMO`. Find its current serial port (names change with USB
port/cabling), then:

```bash
FLASH_PORT=/dev/cu.usbmodemXXXXX ./build.sh flash
```

The project flasher verifies the programmed bytes and SHA-256, then
automatically resets the board. BootROM programming uses 3-byte addressing
within the lower 8 MiB; firmware enables 4-byte access to the full chip.

## Storage and ROMs

W25Q512JV flash: 64 MiB total, divided into 8 MiB firmware, 8 MiB LittleFS
for settings, saves, OTA files and the ROM catalogue, and 48 MiB raw NOR
for contiguous XIP ROMs. LittleFS uses copy-on-write metadata and dynamic
wear leveling. Uploads allocate a new extent, verify its CRC and atomically
publish the catalogue entry; a replacement leaves the old ROM intact until
publication succeeds. ROM access never needs a defragmentation pass.

Upload your own `.gb`, `.gbc`, `.nes`, `.sfc` or `.smc` files through the
WebUI. USB exposes CDC shell and DFU, not mass storage. Settings and saves
use atomic file replacement and are retained across firmware updates.
The settings file is `/NAND:/.blikachu.cfg`. Initialize a new LittleFS
volume explicitly with `umount`, `mkfs`, `mount`.

The WebUI on the board's WiFi address provides ROM upload/delete, remote play
and WiFi OTA. Provision a network through the shell:

```text
wifi_cred add <ssid> <passphrase>
wifi_cred list
wifi_cred connect
```

## Shell

USB CDC provides the `blikachu$` shell. UART0/J3 provides logs at
115200 baud; it does not accept shell commands.

```text
resetinfo
kernel uptime
imu
emu list
emu run <name-or-index>
emu info
emu apu
emu stop
emu save
emu restore
bright 7
emu vol 6
fs ls /NAND:
disk
```

`emu run` cold-boots a ROM and loads its cartridge battery RAM. Full states
are restored only on request while a core is loaded. Save data lives under
`/NAND:/.saves/` in core/ROM-specific `.lrstate`, `.lrsram` and `.lrrtc`
files. Existing Peanut-GB save states are not modified or imported.
The cartridge clock is bounded if the system clock restarts or moves
backward. Accurate RTC advancement while powered off requires a persistent
wall clock; the default board clock starts at the Unix epoch on boot.

Hotkeys:

- SELECT+START: quit to menu.
- SELECT+B / SELECT+A: save / restore state.
- SELECT+UP/DOWN: volume; SELECT+LEFT/RIGHT: brightness.
- Long START: request power-off.

SNES uses physical A/B for B/Y and SELECT+A/B for A/X. SELECT+LEFT/RIGHT
provides L/R. A SELECT tap provides Select; SELECT+START and SELECT+UP/DOWN
retain quit and volume. Use the shell for SNES save/restore and brightness.
Remote play exposes all twelve SNES buttons.

`mkfs` and `wipe` destroy filesystem data; don't run them as routine bring-up
steps. Firmware updates do not require formatting.

See `CLAUDE.md` for board pin assignments, scheduling constraints and patch
maintenance.

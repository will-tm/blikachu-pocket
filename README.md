# BLikachu Pocket

Custom BL618-based replacement board and firmware for the Nintendo Pokémon
Pikachu (MPG-001) shell.

![BLikachu Pocket PCB](hardware/pcb-3d.png)

## Hardware

- Bouffalo Lab BL618 RISC-V SoC with 4 MiB PSRAM.
- W25Q512JV 64 MiB QSPI flash, including a 56 MiB USB mass-storage volume.
- ST7789V SPI colour LCD, D-pad and four buttons, PWM buzzer.
- Li-ion charging, battery measurement and GPIO-controlled power latch.
- Wi-Fi and high-speed USB.

Open [the KiCad project](hardware/blikachu-pocket.kicad_pro) in KiCad 10.

## Firmware

Zephyr v4.5.0 with GB/GBC, NES and SNES emulation, save states, ROM management,
USB DFU, Wi-Fi OTA and remote play.

```sh
cd firmware
./build.sh
./build.sh flash
```

See [firmware documentation](firmware/README.md) for dependencies, flashing,
recovery and development.

GitHub Actions builds the BL618 firmware and renders the PCB in 3D on hardware
changes. The render workflow updates the image above on pushes to main and
uploads a preview artifact on pull requests.

This is an independent replacement hardware/firmware project, not affiliated
with or endorsed by Nintendo.

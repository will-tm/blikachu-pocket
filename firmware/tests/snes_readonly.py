#!/usr/bin/env python3
"""Build the patched core under ASan; optionally exercise a read-only ROM file."""
import os
from pathlib import Path
import subprocess
import sys

root = Path(__file__).resolve().parents[1]
core = root / 'deps/libretro/snes9x2005'
build = root / 'build/snes-host'
build.mkdir(parents=True, exist_ok=True)
cc = os.environ.get('CC', 'cc')
flags = ['-std=gnu99', '-O1', '-g', '-fsanitize=address',
         '-fno-omit-frame-pointer', '-DLOAD_FROM_MEMORY', '-DNO_SPEEDHACKS',
         '-DSNES_READONLY_ROM', '-DRIGHTSHIFT_IS_SAR', '-DINLINE=inline',
         '-DPPU=lr_snes9x2005_PPU', '-DHAVE_STRINGS_H',
         '-I' + str(core), '-I' + str(core / 'source'),
         '-I' + str(core / 'libretro-common/include'),
         '-I' + str(root / 'src/libretro')]
# Match the upstream non-Blargg source list, excluding textually included DSP files.
sources = [core / 'source' / (name + '.c') for name in
           ('c4 c4emu cheats2 cheats clip cpu cpuexec cpuops data dma dsp1 '
            'fxemu fxinst gfx getset globals memmap obc1 ppu sa1 sa1cpu '
            'sdd1 sdd1emu seta010 seta011 seta018 seta spc7110 spc7110dec '
            'srtc tile apu soundux spc700').split()]
sources += [core / 'libretro.c', root / 'src/libretro/snes_readonly.c']
objects = []
for src in sources:
    obj = build / (src.stem + '.o')
    cmd = [cc, *flags, '-Dmalloc=snes_test_malloc', '-Dcalloc=snes_test_calloc',
           '-Dfree=snes_test_free', '-c', str(src), '-o', str(obj)]
    subprocess.run(cmd, check=True, cwd=root)
    objects.append(str(obj))
exe = build / 'snes_readonly'
subprocess.run([cc, *flags, str(root / 'tests/snes_readonly.c'), *objects,
                '-lm', '-o', str(exe)], check=True, cwd=root)
subprocess.run([str(exe), *sys.argv[1:]], check=True, cwd=root)
# Bank-end instruction fetch is a mandatory XIP safety regression.
if sys.argv[1:] != ['--boundary']:
    subprocess.run([str(exe), '--boundary'], check=True, cwd=root)

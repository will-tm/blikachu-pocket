#include <assert.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>
#include "libretro.h"
#include "snes9x.h"
#include "memmap.h"
#include "cheats.h"
#include "snes_readonly.h"
#include "cpuexec.h"
#include "cpufetch.h"

static size_t allocations, fail_at, live, largest, live_bytes, peak_bytes;
static struct { void *pointer; size_t size; } tracked[128];
void *snes_test_malloc(size_t size)
{
    if (++allocations == fail_at)
        return NULL;
    void *p = malloc(size);
    if (p) {
        size_t slot = 0;
        while (slot < 128 && tracked[slot].pointer) slot++;
        assert(slot < 128);
        tracked[slot].pointer = p;
        tracked[slot].size = size;
        live++;
        live_bytes += size;
        if (live_bytes > peak_bytes) peak_bytes = live_bytes;
    }
    if (size > largest) largest = size;
    return p;
}
void *snes_test_calloc(size_t count, size_t size)
{
    void *p = snes_test_malloc(count * size);
    if (p) memset(p, 0, count * size);
    return p;
}
void snes_test_free(void *p)
{
    if (p) {
        size_t slot = 0;
        while (slot < 128 && tracked[slot].pointer != p) slot++;
        assert(slot < 128 && live && live_bytes >= tracked[slot].size);
        live--;
        live_bytes -= tracked[slot].size;
        tracked[slot].pointer = NULL;
    }
    free(p);
}
static bool environment(unsigned cmd, void *data)
{
    (void)cmd; (void)data;
    return false;
}
static void video(const void *data, unsigned width, unsigned height, size_t pitch)
{
    (void)data; (void)width; (void)pitch;
    assert(height <= 478);
}
static size_t audio(const int16_t *data, size_t frames)
{
    (void)data;
    return frames;
}
static void poll(void) {}
static int16_t input(unsigned port, unsigned device, unsigned index, unsigned id)
{
    (void)port; (void)device; (void)index; (void)id;
    return 0;
}
static void header(uint8_t *rom, size_t size, bool hi)
{
    uint8_t *h = rom + (hi ? 0xffc0 : 0x7fc0);
    memset(h, 0, 64);
    memcpy(h, "READONLY TEST        ", 21);
    h[0x15] = hi ? 0x21 : 0x20;
    h[0x16] = 2;
    unsigned exponent = 5;
    while (((size_t)1024 << exponent) < size) exponent++;
    h[0x17] = exponent;
    h[0x18] = 7;
    h[0x19] = 1;
    h[0x1c] = 0xcb; h[0x1d] = 0xed;
    h[0x1e] = 0x34; h[0x1f] = 0x12;
    h[0x3d] = 0x80;
    /* SEI; BRA -2: bounded synthetic CPU execution. */
    size_t reset = hi ? 0x8000 : 0;
    rom[reset] = 0x78; rom[reset + 1] = 0x80; rom[reset + 2] = 0xfe;
}
static void maps(const uint8_t *rom, size_t size, bool hi)
{
    for (unsigned b = 0; b < MEMMAP_NUM_BLOCKS; b++) {
        if (!Memory.BlockIsROM[b]) continue;
        size_t addr = (b & 15) << 12;
        size_t bank = b >> 4;
        size_t logical = hi ? ((bank & 63) << 16) + addr :
                             ((bank & 127) << 15) + (addr & 0x7fff);
        uintptr_t first = (uintptr_t)Memory.Map[b] + addr;
        assert(first >= (uintptr_t)rom);
        assert(first + 4096 <= (uintptr_t)rom + size);
        assert(first == (uintptr_t)rom + snes_readonly_mirror(size, logical));
        assert(Memory.WriteMap[b] == (uint8_t *)MAP_NONE);
        volatile uint8_t byte = *(uint8_t *)(first + 4095);
        (void)byte;
    }
}
static void reject(uint8_t *rom, size_t size)
{
    struct retro_game_info game = {NULL, rom, size, NULL};
    uint8_t *before = Memory.ROM;
    uint32_t calculated = Memory.CalculatedSize;
    uint32_t flags = CPU.Flags;
    assert(!LoadROM(&game));
    assert(!retro_load_game(&game));
    assert(CPU.Flags == flags);
    assert(Memory.ROM == before && Memory.CalculatedSize == calculated);
}
static void exercise(uint8_t *rom, size_t size, bool hi, unsigned frames)
{
    struct retro_game_info game = {NULL, rom, size, NULL};
    assert(mprotect(rom, size, PROT_READ) == 0);
    assert(retro_load_game(&game));
    assert(Memory.ROM == rom && Memory.FillRAM != rom);
    assert(Memory.CalculatedSize == size);
    maps(rom, size, hi);
    assert(Memory.BSRAM == NULL);
    Memory.FillRAM[0x7fff] = 0x55;
    S9xSetByte(0xff, 0x008000);
    S9xSetWord(0xffff, 0x008001);
    Memory.SRAM[0x1ffff] = 0x5a;
    retro_cheat_set(0, true, "008000FF");
    S9xAddCheat(true, true, 0x008000, 0xff);
    S9xApplyCheat(0); S9xRemoveCheat(0);
    retro_cheat_reset();
    S9xDeinterleaveMode2();
    for (unsigned i = 0; i < frames; i++) retro_run();
    size_t bytes = retro_serialize_size();
    void *state = malloc(bytes);
    assert(state && retro_serialize(state, bytes));
    assert(retro_unserialize(state, bytes));
    assert(Memory.SRAM[0x1ffff] == 0x5a);
    free(state);
    retro_unload_game();
    assert(mprotect(rom, size, PROT_READ | PROT_WRITE) == 0);
}
static void operand_boundaries(size_t page)
{
    const size_t size = 0x10000;
    uint8_t *base = mmap(NULL, size + 2 * page, PROT_NONE,
                        MAP_ANON | MAP_PRIVATE, -1, 0);
    assert(base != MAP_FAILED);
    uint8_t *rom = base + page;
    for (unsigned test = 0; test < 5; test++) {
        assert(mprotect(rom, size, PROT_READ | PROT_WRITE) == 0);
        memset(rom, 0, size);
        header(rom, size, false);
        uint16_t pc;
        if (test == 0) {
            pc = 0xffff; rom[pc] = 0xea; /* NOP */
        } else if (test == 1) {
            pc = 0xffff; rom[pc] = 0xa9; /* LDA #8 */
        } else if (test == 2 || test == 3) {
            pc = 0xfffe; rom[pc] = test == 2 ? 0xa9 : 0x69;
            rom[0xffff] = 0x34; /* LDA/ADC #16, high byte wraps to RAM. */
        } else {
            pc = 0xfffd; rom[pc] = 0x5c; /* JML $7e1234 */
            rom[0xfffe] = 0x34; rom[0xffff] = 0x12;
        }
        assert(mprotect(rom, size, PROT_READ) == 0);
        struct retro_game_info game = {NULL, rom, size, NULL};
        assert(retro_load_game(&game));
        Memory.RAM[0] = test == 4 ? 0x7e : test == 1 ? 0x5a : 0x12;
        ICPU.Registers.PB = 1;
        ICPU.ShiftedPB = 0x010000;
        ICPU.Registers.A.W = 1;
        ICPU._Carry = 0;
        ClearDecimal();
        S9xSetPCBase(ICPU.ShiftedPB | pc);
        S9xPCPrepare();
        uint8_t opcode = S9xPCReadNext();
        SOpcodes *table = test == 1 ? S9xOpcodesM1X1 : S9xOpcodesM0X0;
        table[opcode].S9xOpcode();
        S9xPCPrepare();
        uint16_t next = (uint16_t)((uintptr_t)CPU.PC - (uintptr_t)CPU.PCBase);
        if (test == 0) assert(next == 0);
        if (test == 1) assert(next == 1 && ICPU.Registers.AL == 0x5a);
        if (test == 2) assert(next == 1 && ICPU.Registers.A.W == 0x1234);
        if (test == 3) assert(next == 1 && ICPU.Registers.A.W == 0x1235);
        if (test == 4) assert(next == 0x1234 && ICPU.ShiftedPB == 0x7e0000);
        retro_unload_game();
    }
    munmap(base, size + 2 * page);
    puts("opcode, 8/16-bit operands, ADC and long-jump bank wraps passed");
}

int main(int argc, char **argv)
{
    retro_set_environment(environment);
    retro_set_video_refresh(video);
    retro_set_audio_sample_batch(audio);
    retro_set_input_poll(poll);
    retro_set_input_state(input);
    /* Every initialization allocation must fail cleanly and permit retry. */
    uint8_t *valid = calloc(0x10000, 1);
    assert(valid);
    header(valid, 0x10000, false);
    struct retro_game_info valid_game = {NULL, valid, 0x10000, NULL};
    allocations = 0;
    retro_init();
    size_t init_allocations = allocations;
    retro_deinit();
    assert(live == 0);
    for (size_t n = 1; n <= init_allocations; n++) {
        allocations = 0; fail_at = n;
        retro_init();
        assert(!retro_load_game(&valid_game));
        retro_deinit();
        assert(live == 0);
    }
    fail_at = 0; allocations = 0;
    retro_init();
    fail_at = allocations + 1; /* Audio buffer allocation during load. */
    assert(!retro_load_game(&valid_game));
    fail_at = 0;
    retro_unload_game();
    free(valid);
    const size_t page = (size_t)sysconf(_SC_PAGESIZE);
    const size_t sizes[] = {0x8000, 0x10000, 0x180000, 0x280000, 0x300000, 0x400000};
    assert(snes_readonly_mirror(0x280000, 0x280000) == 0x200000);
    assert(snes_readonly_mirror(0x280000, 0x3fffff) == 0x27ffff);
    for (unsigned hi = 0; hi < 2; hi++) for (size_t i = hi; i < sizeof(sizes)/sizeof(sizes[0]); i++) {
        size_t size = sizes[i];
        /* Inaccessible pages catch both prefix and suffix loader reads. */
        uint8_t *base = mmap(NULL, size + 2 * page, PROT_NONE, MAP_ANON | MAP_PRIVATE, -1, 0);
        assert(base != MAP_FAILED);
        uint8_t *rom = base + page;
        assert(mprotect(rom, size, PROT_READ | PROT_WRITE) == 0);
        header(rom, size, hi);
        exercise(rom, size, hi, 2);
        uint8_t *h = rom + (hi ? 0xffc0 : 0x7fc0);
        h[0x15] |= 0x10;
        exercise(rom, size, hi, 1);
        header(rom, size, hi);
        Settings.ForceSuperFX = true; reject(rom, size); Settings.ForceSuperFX = false;
        Settings.ForceInterleaved = true; reject(rom, size); Settings.ForceInterleaved = false;
        if (size >= 0x10000) {
            header(rom, size, !hi);
            reject(rom, size); /* Both header locations valid: ambiguous. */
            uint8_t *other = rom + (hi ? 0x7fc0 : 0xffc0);
            memset(other, 0, 64);
            memcpy(other, "BS TEST", 7);
            other[0x10] = 1; other[0x18] = 0x20; other[0x1a] = 0x33;
            reject(rom, size); /* BS header in the unselected location. */
            memset(other, 0, 64);
        }
        for (unsigned type = 3; type < 256; type++) {
            h[0x16] = type; reject(rom, size);
        }
        h[0x16] = 2;
        h[0x18] = 8; reject(rom, size); h[0x18] = 7;
        h[0x17] = 255; reject(rom, size); header(rom, size, hi);
        h[0x15] = 0x25; reject(rom, size); header(rom, size, hi);
        h[0x3d] = 0; reject(rom, size); header(rom, size, hi);
        memcpy(h, "ADD-ON BASE CASSETE", 19); reject(rom, size); header(rom, size, hi);
        memcpy(h, "MEY Fun with Numbers", 20); reject(rom, size); header(rom, size, hi);
        memcpy(rom, "BANDAI SFC-ADX", 14); reject(rom, size); header(rom, size, hi);
        reject(rom, size - 1);
        reject(rom, 0x400200);
        munmap(base, size + 2 * page);
    }
    /* Even a one-byte pointer is safe when its length is rejected. */
    uint8_t byte = 0;
    for (size_t size = 0; size < 0x8000; size++) reject(&byte, size);
    reject(NULL, 0x8000);
    if (argc > 1 && !strcmp(argv[1], "--boundary")) {
        /* Regression: an accepted ROM must not fetch beyond its XIP extent. */
        size_t size = 0x8000;
        uint8_t *base = mmap(NULL, size + 2 * page, PROT_NONE,
                            MAP_ANON | MAP_PRIVATE, -1, 0);
        assert(base != MAP_FAILED);
        uint8_t *rom = base + page;
        assert(mprotect(rom, size, PROT_READ | PROT_WRITE) == 0);
        header(rom, size, false);
        rom[0x7ffc] = 0xff; rom[0x7ffd] = 0xff;
        rom[0x7fff] = 0xea; /* NOP at bank end; next PC must wrap. */
        assert(mprotect(rom, size, PROT_READ) == 0);
        struct retro_game_info game = {NULL, rom, size, NULL};
        assert(retro_load_game(&game));
        retro_run();
        retro_unload_game();
        munmap(base, size + 2 * page);
    } else if (argc > 1) {
        int fd = open(argv[1], O_RDONLY);
        struct stat st;
        assert(fd >= 0 && fstat(fd, &st) == 0);
        size_t size = (size_t)st.st_size;
        uint8_t *rom = mmap(NULL, size, PROT_READ, MAP_PRIVATE, fd, 0);
        bool hi;
        assert(rom != MAP_FAILED && snes_readonly_validate(rom, size, &hi));
        struct retro_game_info game = {NULL, rom, size, NULL};
        assert(retro_load_game(&game));
        maps(rom, size, hi);
        for (unsigned i = 0; i < 120; i++) retro_run();
        retro_unload_game();
        munmap(rom, size); close(fd);
        printf("read-only cartridge: %zu bytes, 120 frames\n", size);
    }
    operand_boundaries(page);
    retro_deinit();
    assert(live == 0);
    assert(largest < 0x100000);
    assert(live_bytes == 0);
    printf("SNES read-only safety tests passed: %zu init allocation failures; largest allocation %zu bytes; peak heap %zu bytes\n", init_allocations, largest, peak_bytes);
    return 0;
}

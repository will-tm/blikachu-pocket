#!/bin/bash
# Build script for blikachu-pocket (BL618)
# Handles complete setup on first run - just clone and run ./build.sh
#
# Usage:
#   ./build.sh [options]
#
# Options:
#   clean/pristine  - Clean build
#   mcuboot         - Build MCUboot app, UF2 and ISP factory image
#   flash           - Update over USB DFU; FLASH_PORT selects BootROM ISP

set -e

PRISTINE=""
DO_FLASH=""
MCUBOOT=""

for arg in "$@"; do
    case "$arg" in
        pristine|clean)
            PRISTINE="--pristine"
            ;;
        mcuboot)
            MCUBOOT="1"
            ;;
        flash)
            DO_FLASH="1"
            ;;
    esac
done

# Production hardware: BL618 with 64 MiB external flash.
BOARD="blikachu_pocket/bl618m05q2i"

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
cd "$SCRIPT_DIR"

# Drop any inherited venv so our own .venv wins.
unset VIRTUAL_ENV

# === Setup: Python virtual environment ===
if [ ! -d ".venv" ]; then
    echo "Creating Python virtual environment..."
    python3 -m venv .venv
fi

source .venv/bin/activate

# === Setup: Install Python dependencies ===
if ! command -v west &> /dev/null || ! command -v bflb-mcu-tool-uart &> /dev/null; then
    echo "Installing Python dependencies..."
    pip install -r requirements.txt
fi

# Project flasher wrapper adds W25Q512JV capacity decoding.
export PATH="${SCRIPT_DIR}/tools:${PATH}"

# === Setup: Initialize west workspace ===
if [ ! -f ".west/config" ]; then
    echo "Initializing west workspace..."
    mkdir -p .west
    cat > .west/config << 'EOF'
[manifest]
path = .
file = west.yml

[zephyr]
base = deps/zephyr
EOF
fi

# === Setup: Download Zephyr if needed ===
if [ ! -f "deps/zephyr/west.yml" ]; then
    echo "Downloading Zephyr (this takes a while)..."
    west config manifest.project-filter -- "-hal_.*,-nrf_hw_models,+hal_bouffalolab,+hal_st"
    west update
fi

# The LSM6DSV16X driver uses the ST MEMS HAL.
if [ "$(west list -f '{active}' hal_st)" = "inactive" ]; then
    west config manifest.project-filter -- "$(west config manifest.project-filter),+hal_st"
fi
if [ ! -d "deps/modules/hal/st/.git" ]; then
    west update --narrow hal_st
fi

# === Pinned emulator dependencies ===
for core in common gambatte fceumm snes9x2005; do
    if [ ! -d "deps/libretro/$core/.git" ]; then
        west update --narrow libretro-common libretro-gambatte libretro-fceumm libretro-snes9x2005
        break
    fi
done

# === Apply project patches to deps/zephyr (idempotent) ===
if [ -d patches ] && [ -d deps/zephyr/.git ]; then
    for p in patches/*.patch; do
        [ -f "$p" ] || continue
        if git -C deps/zephyr apply --reverse --check "$SCRIPT_DIR/$p" 2>/dev/null; then
            : # already applied
        elif git -C deps/zephyr apply --check "$SCRIPT_DIR/$p" 2>/dev/null; then
            echo "Applying patch: $p"
            git -C deps/zephyr apply "$SCRIPT_DIR/$p"
        else
            echo "ERROR: $p does not apply cleanly to deps/zephyr"
            exit 1
        fi
    done
fi

# === MCUboot integration ===
if [ -n "$MCUBOOT" ]; then
    if [ ! -d deps/bootloader/mcuboot/.git ]; then
        west update --narrow mcuboot
    fi
    for p in patches/mcuboot/*.patch; do
        if ! git -C deps/bootloader/mcuboot apply --reverse --check "$SCRIPT_DIR/$p" 2>/dev/null; then
            git -C deps/bootloader/mcuboot apply "$SCRIPT_DIR/$p"
        fi
    done
fi

# === Hostap patches ===
for p in patches/hostap/*.patch; do
    [ -f "$p" ] || continue
    if git -C deps/modules/lib/hostap apply --reverse --check "$SCRIPT_DIR/$p" 2>/dev/null; then
        : # already applied
    elif git -C deps/modules/lib/hostap apply --check "$SCRIPT_DIR/$p" 2>/dev/null; then
        echo "Applying patch: $p"
        git -C deps/modules/lib/hostap apply "$SCRIPT_DIR/$p"
    else
        echo "ERROR: $p does not apply cleanly to deps/modules/lib/hostap"
        exit 1
    fi
done

# === Project-owned libretro patches ===
for core in common gambatte fceumm snes9x2005; do
    for p in "patches/libretro/$core/"*.patch; do
        [ -f "$p" ] || continue
        if git -C "deps/libretro/$core" apply --reverse --check "$SCRIPT_DIR/$p" 2>/dev/null; then
            : # already applied
        elif git -C "deps/libretro/$core" apply --check "$SCRIPT_DIR/$p" 2>/dev/null; then
            echo "Applying patch: $p"
            git -C "deps/libretro/$core" apply "$SCRIPT_DIR/$p"
        else
            echo "ERROR: $p does not apply cleanly to deps/libretro/$core"
            exit 1
        fi
    done
done

# === Vendor blobs (BL618 WiFi/BLE) — fetch once, idempotent ===
if [ -d "deps/modules/hal/bouffalolab" ] && \
   [ ! -f "deps/modules/hal/bouffalolab/zephyr/blobs/lib/bl61x/libwl80211_bl616.a" ]; then
    echo "Fetching Bouffalo Lab vendor blobs..."
    west blobs fetch hal_bouffalolab
fi

# === Install Zephyr Python deps ===
if [ ! -f "deps/.python_deps_installed" ]; then
    echo "Installing Zephyr Python requirements..."
    pip install -r deps/zephyr/scripts/requirements.txt
    touch deps/.python_deps_installed
fi

# === SDK detection ===
# Reject a pre-set ZEPHYR_SDK_INSTALL_DIR that doesn't actually look like a
# Zephyr SDK root (e.g. an empty ~/.zephyr_ide/toolchains stub) — otherwise
# CMake fails later resolving the SDK. Marker: cmake/Zephyr-sdkConfig.cmake
# (1.0.x layout; the old cmake/zephyr/generic.cmake path no longer exists).
if [ -n "$ZEPHYR_SDK_INSTALL_DIR" ] && [ ! -f "$ZEPHYR_SDK_INSTALL_DIR/cmake/Zephyr-sdkConfig.cmake" ]; then
    echo "WARN: ZEPHYR_SDK_INSTALL_DIR=$ZEPHYR_SDK_INSTALL_DIR is not a Zephyr SDK; ignoring."
    unset ZEPHYR_SDK_INSTALL_DIR
fi

# Honor user-set ZEPHYR_SDK_INSTALL_DIR; otherwise pick the first SDK in a
# standard install location that contains a host-compatible toolchain for the
# RISC-V target. SDKs in non-standard
# locations are opt-in: export ZEPHYR_SDK_INSTALL_DIR to point at one.
# Any SDK >= 0.17.4 groks the BL61x T-Head ISA extensions.
NEED_GCC='riscv64-zephyr-elf-gcc'
if [ -z "$ZEPHYR_SDK_INSTALL_DIR" ]; then
    HOST_ARCH="$(uname -m)"
    for candidate in \
        "$HOME/zephyr-sdk" \
        "$HOME/zephyr-sdk-"* \
        "/opt/zephyr-sdk" \
        "/opt/zephyr-sdk-"*; do
        [ -d "$candidate" ] || continue
        [ -f "$candidate/cmake/Zephyr-sdkConfig.cmake" ] || continue
        # Look for the needed cross-gcc anywhere under the SDK and verify
        # its Mach-O architecture matches the host (handles SDKs that ship
        # only arm64 or only x86_64 binaries).
        gcc_bin="$(find "$candidate" -maxdepth 5 -name "$NEED_GCC" -type f 2>/dev/null | head -1)"
        [ -n "$gcc_bin" ] || continue
        file "$gcc_bin" 2>/dev/null | grep -q "$HOST_ARCH" || continue
        # BL61x needs binutils that grok the unversioned T-Head
        # extension names (SDK >= 0.17.4).
        echo 'int main(void){return 0;}' | \
            "$gcc_bin" -march=rv32imafc_zicsr_zifencei_xtheadba_xtheadbb_xtheadbs_xtheadcmo \
            -mabi=ilp32f -x c -c - -o /dev/null 2>/dev/null || continue
        export ZEPHYR_SDK_INSTALL_DIR="$candidate"
        break
    done
fi

if [ -n "$ZEPHYR_SDK_INSTALL_DIR" ]; then
    echo "Using SDK: $ZEPHYR_SDK_INSTALL_DIR"
else
    echo "WARN: ZEPHYR_SDK_INSTALL_DIR not set; relying on Zephyr auto-discovery"
fi

# === Build ===
echo "========================================"
echo "Building blikachu-pocket"
echo "Board: ${BOARD}"
echo "========================================"

# Auto-pristine if the existing build dir was configured for a different board.
if [ -z "$PRISTINE" ] && [ -f "build/CMakeCache.txt" ]; then
    CACHED_BOARD="$(grep -E '^CACHED_BOARD:' build/CMakeCache.txt | cut -d= -f2)"
    if [ -n "$CACHED_BOARD" ] && [ "$CACHED_BOARD" != "$BOARD" ]; then
        echo "Board changed ($CACHED_BOARD -> $BOARD); forcing pristine build."
        PRISTINE="--pristine"
    fi
fi

# Pin Zephyr to the in-tree copy. Without this, CMake's user package
# registry (~/.cmake/packages/Zephyr) can resolve to another workspace's
# Zephyr that happens to be registered.
export ZEPHYR_BASE="${SCRIPT_DIR}/deps/zephyr"

# Disable CMake's user package registry so a system-registered Zephyr SDK
# (e.g. ~/.cmake/packages/Zephyr-sdk/...) can't override our chosen one.
export CMAKE_FIND_USE_PACKAGE_REGISTRY=OFF

WEST_EXTRA_CMAKE=()
if [ -n "$ZEPHYR_SDK_INSTALL_DIR" ]; then
    WEST_EXTRA_CMAKE+=(-DZEPHYR_SDK_INSTALL_DIR="$ZEPHYR_SDK_INSTALL_DIR")
fi

if [ -n "$MCUBOOT" ]; then
    [ -f build/domains.yaml ] || PRISTINE="--pristine=always"
    west build -b "${BOARD}" -d build "${SCRIPT_DIR}" ${PRISTINE} --sysbuild \
        -- -DBOARD_ROOT="$SCRIPT_DIR" '-DSB_CONFIG_BOOT_SIGNATURE_KEY_FILE=""' "${WEST_EXTRA_CMAKE[@]}"
else
    [ ! -f build/domains.yaml ] || PRISTINE="--pristine=always"
    west build -b "${BOARD}" -d build "${SCRIPT_DIR}" ${PRISTINE} --no-sysbuild \
        -- "${WEST_EXTRA_CMAKE[@]}"
fi

echo "========================================"
echo "Build complete!"
echo "========================================"

# === Flash ===
if [ -n "$MCUBOOT" ]; then
    python tools/mcuboot_artifacts.py
    if [ -n "$DO_FLASH" ]; then
        if [ -z "${FLASH_PORT:-}" ]; then
            echo "ERROR: factory installation requires BootROM ISP (set FLASH_PORT)."
            exit 1
        fi
        bflb-mcu-tool-uart --raw-factory --chipname bl616 --port "$FLASH_PORT" --baudrate 2000000 --addr 0x0 --firmware "$SCRIPT_DIR/build/factory.bin"
    fi
    exit 0
fi
if [ -n "$DO_FLASH" ]; then
    echo ""
    echo "Flashing..."

    if [ -n "${FLASH_PORT:-}" ]; then
        west flash -d build --dev-id "$FLASH_PORT" --baudrate 2000000
        echo "Image read-back verified. The flash tool automatically resets the board."
    elif command -v dfu-util >/dev/null && dfu-util -l 2>/dev/null | grep -q 'Found DFU: \[2e8a:000a\]'; then
        dfu-util -d 2e8a:000a -D "${SCRIPT_DIR}/build/zephyr/zephyr.dfu.bin"
        echo "Download complete; keep power connected during flash installation and reboot."
    else
        echo "ERROR: no DFU device found. For BootROM ISP, hold BOOT while resetting, then set FLASH_PORT to its serial port."
        exit 1
    fi
else
    echo ""
    echo "Output files in ${SCRIPT_DIR}/build/zephyr/:"
    echo "  zephyr.dfu.bin - USB DFU / WiFi OTA whole-flash image"
    echo "  zephyr.elf  - For debugging symbols"
    echo ""
    echo "To flash: ./build.sh flash"
fi

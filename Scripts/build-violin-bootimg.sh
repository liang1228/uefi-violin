#!/usr/bin/env bash
set -euo pipefail

# Build VIOLIN_UEFI.fd (CLANGPDB) + BootShim + Android v4 boot.img in WSL2.
# Usage: bash Scripts/build-violin-bootimg.sh [--qemu] [--out /path/to/out.img]

QEMU_TEST=0
OUT_IMG=""
OUT_KERNEL=""

while [[ $# -gt 0 ]]; do
  case "$1" in
    --qemu) QEMU_TEST=1; shift ;;
    --out) OUT_IMG="$2"; shift 2 ;;
    --out-kernel) OUT_KERNEL="$2"; shift 2 ;;
    *) echo "Unknown arg: $1" >&2; exit 2 ;;
  esac
done

UEFI_ROOT="$(cd "$(dirname "$0")/.." && pwd)"
UNIXNU_ROOT="$(cd "$UEFI_ROOT/../UniXNU" && pwd)"
EDK2_ROOT="${UNIXNU_EDK2:-/opt/unixnu-edk2/Mu_Basecore}"
STAGE0_MKBOOT="$(cd "$UEFI_ROOT/../../outputs/stage0-probe-20261124" && pwd)/mkbootv4.py"

BUILD_WORK="/tmp/violin-uefi-build-q${QEMU_TEST}"
CONF_DIR="${BUILD_WORK}/Conf"
mkdir -p "$CONF_DIR"
for f in target tools_def build_rule; do
  if [[ ! -f "$CONF_DIR/${f}.txt" ]]; then
    cp "$EDK2_ROOT/BaseTools/Conf/${f}.template" "$CONF_DIR/${f}.txt"
  fi
done

export WORKSPACE="$BUILD_WORK"
export EDK_TOOLS_PATH="$EDK2_ROOT/BaseTools"
export CONF_PATH="$CONF_DIR"
export PACKAGES_PATH="${UEFI_ROOT}:${UNIXNU_ROOT}/package:${UNIXNU_ROOT}/drivers/DarwinBoot:${EDK2_ROOT}"
export PYTHON_COMMAND=python3
export CLANG_BIN=/usr/bin/
export PATH="${EDK_TOOLS_PATH}/BinWrappers/PosixLike:${EDK_TOOLS_PATH}/Source/C/bin:${PATH}"

build \
  -p Platforms/Xiaomi/ViolinPkg/ViolinPkg.dsc \
  -a AARCH64 \
  -b DEBUG \
  -t CLANGPDB \
  -n "${JOBS:-8}" \
  -D FD_BASE=0x100000000 \
  -D FD_SIZE=0x00400000 \
  -D FD_BLOCKS=0x400 \
  -D QEMU_TEST="${QEMU_TEST}"

FD_FILE="${BUILD_WORK}/Build/ViolinPkg/DEBUG_CLANGPDB/FV/VIOLIN_UEFI.fd"
ls -lh "$FD_FILE"
FD_BYTES=$(stat -c %s "$FD_FILE")
if [[ "$FD_BYTES" -ne 4194304 ]]; then
  echo "ERROR: expected VIOLIN_UEFI.fd == 4194304 B, got $FD_BYTES" >&2
  exit 1
fi

# Verify GenFv patched AArch64 branch instruction at FD+0x00
echo "=== VIOLIN_UEFI.fd first 16 bytes (check AArch64 reset branch) ==="
od -Ax -tx1 -N 16 "$FD_FILE"

# Assemble BootShim (padded to exact 4096 bytes) + concatenate FD
SHIM_OBJ="${BUILD_WORK}/BootShim.o"
SHIM_ELF="${BUILD_WORK}/BootShim.elf"
SHIM_BIN="${BUILD_WORK}/BootShim.bin"
SHIM_PAD="${BUILD_WORK}/BootShim.4k.bin"
KERNEL_BIN="${OUT_KERNEL:-${BUILD_WORK}/violin-uefi-kernel.bin}"

aarch64-linux-gnu-gcc -nostdlib -nostartfiles -ffreestanding -march=armv8-a \
  -DFD_BASE=0x100000000 -DFD_SIZE=0x400000 \
  -c "$UEFI_ROOT/BootShim/BootShim.S" -o "$SHIM_OBJ"
aarch64-linux-gnu-ld -T "$UEFI_ROOT/BootShim/BootShim.ld" -nostdlib "$SHIM_OBJ" -o "$SHIM_ELF"
aarch64-linux-gnu-objcopy -O binary "$SHIM_ELF" "$SHIM_BIN"

python3 - "$SHIM_BIN" "$FD_FILE" "$KERNEL_BIN" << 'PY'
import struct, sys
from pathlib import Path

shim = Path(sys.argv[1]).read_bytes()
fd = Path(sys.argv[2]).read_bytes()
out = Path(sys.argv[3])
assert len(shim) <= 4096, f"BootShim too large: {len(shim)}"
shim_padded = shim + b"\x00" * (4096 - len(shim))
img = shim_padded + fd
assert img[0:2] == b"MZ" and img[56:60] == b"ARMd"
out.write_bytes(img)
print(f"Combined BootShim(4096) + FD({len(fd)}) -> {out} ({len(img)} B)")
PY

if [[ -n "$OUT_IMG" ]]; then
  python3 "$STAGE0_MKBOOT" --kernel "$KERNEL_BIN" --output "$OUT_IMG"
fi

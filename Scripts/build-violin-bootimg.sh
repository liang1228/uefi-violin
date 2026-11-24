#!/usr/bin/env bash
set -euo pipefail

# Build VIOLIN_UEFI.fd (CLANGPDB) + BootShim + optional VNPDISK1 FAT payload + Android v4 boot.img in WSL2.
# Usage: bash Scripts/build-violin-bootimg.sh [--qemu] [--with-stage2-payload] [--payload-disk path] [--timeout N] [--out /path/to/out.img] [--out-kernel /path/to/kernel.bin]

QEMU_TEST=0
WITH_STAGE2_PAYLOAD=0
SKIP_UEFI_BUILD=0
PAYLOAD_DISK=""
STAGE2_TIMEOUT=3
OUT_IMG=""
OUT_KERNEL=""

while [[ $# -gt 0 ]]; do
  case "$1" in
    --qemu) QEMU_TEST=1; shift ;;
    --with-stage2-payload) WITH_STAGE2_PAYLOAD=1; shift ;;
    --skip-uefi-build) SKIP_UEFI_BUILD=1; shift ;;
    --payload-disk) PAYLOAD_DISK="$2"; shift 2 ;;
    --timeout) STAGE2_TIMEOUT="$2"; shift 2 ;;
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

if [[ "$SKIP_UEFI_BUILD" -eq 0 ]]; then
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
fi

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

if [[ "$WITH_STAGE2_PAYLOAD" -eq 1 && -z "$PAYLOAD_DISK" ]]; then
  PAYLOAD_DISK="${BUILD_WORK}/violin-stage2-payload.fat"
  STUB_ARGS=(--out "$PAYLOAD_DISK" --timeout "$STAGE2_TIMEOUT")
  if [[ "$QEMU_TEST" -eq 1 ]]; then
    STUB_ARGS+=(--qemu)
  fi
  python3 "$UEFI_ROOT/Tools/Stage2Stub/build-stage2-payload.py" "${STUB_ARGS[@]}"
fi

python3 - "$SHIM_BIN" "$FD_FILE" "$KERNEL_BIN" "$PAYLOAD_DISK" << 'PY'
import struct, sys
from pathlib import Path

shim = Path(sys.argv[1]).read_bytes()
fd = Path(sys.argv[2]).read_bytes()
out = Path(sys.argv[3])
payload_arg = sys.argv[4]

assert len(shim) <= 4096, f"BootShim too large: {len(shim)}"
shim_padded = bytearray(shim + b"\x00" * (4096 - len(shim)))
img = bytearray(shim_padded) + bytearray(fd)

if payload_arg:
    payload = Path(payload_arg).read_bytes()
    assert len(payload) > 0 and (len(payload) % 512) == 0, f"Payload size must be non-zero 512B multiple: {len(payload)}"
    trailer = struct.pack("<8sQQQ", b"VNPDISK1", len(payload), 0, 0)
    img += trailer + payload
    # Patch ARM64 Image header image_size (+0x10) so bootloader / QEMU never overlaps DTB with payload
    aligned_total = ((len(img) + 0xFFFF) // 0x10000) * 0x10000
    struct.pack_into("<Q", img, 0x10, aligned_total)
    print(f"Attached VNPDISK1 payload ({len(payload)} B), patched ARM64 image_size=0x{aligned_total:x}")

assert img[0:2] == b"MZ" and img[56:60] == b"ARMd"
out.write_bytes(bytes(img))
print(f"Combined BootShim(4096) + FD({len(fd)}) + Payload -> {out} ({len(img)} B)")
PY

if [[ -n "$OUT_IMG" ]]; then
  python3 "$STAGE0_MKBOOT" --kernel "$KERNEL_BIN" --output "$OUT_IMG"
fi

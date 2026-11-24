#!/usr/bin/env python3
"""Build Stage-3 FAT32 payload disk for ViolinPayloadDiskDxe (VNPDISK1).

Packages a real arm64e XNU Mach-O kernel, Format-2 generated violin.adt, and
EFI/UniXNU/unixnu.toml into a superfloppy FAT32 volume sized to fit inside
violin's 96 MiB boot_b partition alongside BootShim + 4 MiB VIOLIN_UEFI.fd.

SPDX-License-Identifier: BSD-2-Clause-Patent
"""

import argparse
import hashlib
import math
import shutil
import struct
import subprocess
import tempfile
from pathlib import Path

MH_MAGIC_64 = 0xFEEDFACF
CPU_TYPE_ARM64 = 0x0100000C
CPU_SUBTYPE_ARM64E = 2
MH_EXECUTE = 2
MH_FILESET = 12


def verify_kernel_macho(path: Path) -> tuple[int, int, int]:
    raw = path.read_bytes()
    if len(raw) < 32:
        raise SystemExit(f"Kernel file too small: {path} ({len(raw)} B)")
    magic, cputype, cpusubtype, filetype = struct.unpack_from("<4I", raw, 0)
    if magic != MH_MAGIC_64 or cputype != CPU_TYPE_ARM64:
        raise SystemExit(f"Not an ARM64 64-bit Mach-O: magic=0x{magic:x} cpu=0x{cputype:x}")
    if (cpusubtype & 0x00FFFFFF) != CPU_SUBTYPE_ARM64E:
        raise SystemExit(f"Expected arm64e subtype (2), got 0x{cpusubtype:x}")
    if filetype not in (MH_EXECUTE, MH_FILESET):
        raise SystemExit(f"Unexpected Mach-O filetype {filetype} (expected MH_EXECUTE=2 or MH_FILESET=12)")
    return len(raw), cpusubtype, filetype


def build_unixnu_toml(timeout: int, title: str, args_line: str) -> str:
    return (
        "version = 1\n\n"
        "[boot]\n"
        f"timeout = {timeout}\n"
        "default = \"violin-xnu\"\n"
        "rotate = 0\n\n"
        "[ui]\n"
        "title = \"UniXNU (Xiaomi Pad 7S Pro - violin)\"\n\n"
        "[[entry]]\n"
        "id = \"violin-xnu\"\n"
        f"title = \"{title}\"\n"
        "type = \"xnu\"\n"
        "path = \"Payloads/kernel.macho\"\n"
        f"args = \"{args_line}\"\n\n"
        "[[entry]]\n"
        "id = \"reset\"\n"
        "title = \"Restart (Return to Android)\"\n"
        "type = \"action\"\n"
        "action = \"reset\"\n\n"
        "[[entry]]\n"
        "id = \"shutdown\"\n"
        "title = \"Power off\"\n"
        "type = \"action\"\n"
        "action = \"shutdown\"\n"
    )


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--kernel", type=Path, required=True, help="Real arm64e XNU Mach-O kernel")
    parser.add_argument("--adt", type=Path, required=True, help="Compiled Apple Device Tree (.adt)")
    parser.add_argument("--trustcache", type=Path, default=None, help="Optional DarwinBoot TrustCache bundle (.trustcache)")
    parser.add_argument("--out", type=Path, required=True, help="Output FAT32 payload disk image")
    parser.add_argument("--args", type=str, required=True, help="XNU kernel command line")
    parser.add_argument("--title", type=str, default="Violin Stage-3 arm64e XNU", help="Boot menu entry title")
    parser.add_argument("--timeout", type=int, default=3, help="UniXNU menu timeout in seconds")
    parser.add_argument("--size-mb", type=int, default=0, help="FAT32 volume size in MiB (0 = auto, min 40, max 240 with --lz4 or 84 raw)")
    parser.add_argument("--lz4", action="store_true", help="Compress FAT32 image with LZ4 frame for VNPDISK2 (writes <out>.lz4)")
    args = parser.parse_args()

    for tool_name in ("mkfs.vfat", "mmd", "mcopy"):
        if not shutil.which(tool_name):
            raise SystemExit(f"Missing required tool on PATH: {tool_name}")

    ksize, subtype, ftype = verify_kernel_macho(args.kernel)
    adt_bytes = args.adt.read_bytes()
    if len(adt_bytes) < 64:
        raise SystemExit(f"ADT too small: {args.adt} ({len(adt_bytes)} B)")
    tc_bytes = args.trustcache.read_bytes() if args.trustcache else b""

    if args.size_mb > 0:
        size_mb = args.size_mb
    else:
        needed_mb = math.ceil((ksize + len(adt_bytes) + len(tc_bytes) + 6 * 1024 * 1024) / (4 * 1024 * 1024)) * 4
        size_mb = max(40, needed_mb)
    max_mb = 240 if args.lz4 else 84
    if size_mb > max_mb:
        raise SystemExit(f"Payload volume {size_mb} MiB exceeds {max_mb} MiB budget")

    args.out.parent.mkdir(parents=True, exist_ok=True)
    if args.out.exists():
        args.out.unlink()

    with tempfile.TemporaryDirectory() as tmp:
        toml_path = Path(tmp) / "unixnu.toml"
        toml_path.write_text(build_unixnu_toml(args.timeout, args.title, args.args), encoding="ascii")

        subprocess.run(
            ["mkfs.vfat", "-F", "32", "-n", "VIOLINBOOT", "-C", str(args.out), str(size_mb * 1024)],
            check=True,
            stdout=subprocess.DEVNULL,
        )
        subprocess.run(["mmd", "-i", str(args.out), "::/EFI", "::/EFI/UniXNU", "::/EFI/UniXNU/Payloads"], check=True)
        subprocess.run(["mcopy", "-i", str(args.out), str(toml_path), "::/EFI/UniXNU/unixnu.toml"], check=True)
        subprocess.run(["mcopy", "-i", str(args.out), str(args.kernel), "::/EFI/UniXNU/Payloads/kernel.macho"], check=True)
        subprocess.run(["mcopy", "-i", str(args.out), str(args.adt), "::/EFI/UniXNU/Payloads/kernel.macho.adt"], check=True)
        if args.trustcache:
            subprocess.run(["mcopy", "-i", str(args.out), str(args.trustcache), "::/EFI/UniXNU/Payloads/kernel.macho.trustcache"], check=True)

    khash = hashlib.sha256(args.kernel.read_bytes()).hexdigest()[:16]
    ahash = hashlib.sha256(adt_bytes).hexdigest()[:16]
    print(
        f"Built FAT32 Stage-3/4 payload -> {args.out} ({size_mb} MiB = {args.out.stat().st_size} B) "
        f"[kernel={ksize} B subtype=0x{subtype:x} type={ftype} sha256={khash}..; adt={len(adt_bytes)} B sha256={ahash}..; tc={len(tc_bytes)} B]"
    )
    if args.lz4:
        if not shutil.which("lz4"):
            raise SystemExit("Missing required tool on PATH: lz4")
        lz4_out = Path(str(args.out) + ".lz4")
        subprocess.run(["lz4", "-z", "-9", "--content-size", "--no-frame-crc", "-f", str(args.out), str(lz4_out)], check=True)
        lz4_bytes = lz4_out.stat().st_size
        if lz4_bytes > 84 * 1024 * 1024:
            raise SystemExit(f"Compressed LZ4 payload {lz4_bytes} B exceeds 84 MiB BootShim/boot_b budget")
        print(f"Compressed VNPDISK2 LZ4 -> {lz4_out} ({lz4_bytes} B = {lz4_bytes / (1024 * 1024):.2f} MiB)")


if __name__ == "__main__":
    main()

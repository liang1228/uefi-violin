#!/usr/bin/env python3
"""Build Stage-2 arm64e Mach-O verification stub, ADT, TrustCache, RAMDisk,
unixnu.toml, and a 4 MiB FAT16 superfloppy volume for ViolinPayloadDiskDxe.

Freely redistributable synthetic DarwinBoot ABI payload; no Apple binaries.
SPDX-License-Identifier: BSD-2-Clause-Patent
"""

import argparse
import shutil
import struct
import subprocess
import tempfile
from pathlib import Path

BASE_VA = 0xFFFFFFF007004000
CPU_TYPE_ARM64 = 0x0100000C
CPU_SUBTYPE_ARM64E_PTR_AUTH = 0x80000002  # DarwinBoot accepts arm64e capability bits
MH_MAGIC_64 = 0xFEEDFACF
MH_EXECUTE = 2


def adt_node(name: str, properties=(), children=()):
    props = [("name", name.encode("ascii") + b"\x00"), *properties]
    data = struct.pack("<II", len(props), len(children))
    for key, value in props:
        data += struct.pack("<32sI", key.encode("ascii"), len(value)) + value + bytes(-len(value) % 4)
    return data + b"".join(children)


def build_macho(stub_src: Path, work_dir: Path, qemu_stub: bool) -> bytes:
    obj = work_dir / "stub.o"
    elf = work_dir / "stub.elf"
    raw = work_dir / "stub.bin"

    subprocess.run(
        [
            "clang",
            "--target=aarch64-none-elf",
            "-march=armv8.3-a",
            "-ffreestanding",
            "-nostdlib",
            f"-DQEMU_STUB={1 if qemu_stub else 0}",
            "-c",
            str(stub_src),
            "-o",
            str(obj),
        ],
        check=True,
    )
    subprocess.run(
        [
            "ld.lld",
            "--omagic",
            f"-Ttext={BASE_VA + 0x1000:#x}",
            "-e",
            "_start",
            str(obj),
            "-o",
            str(elf),
        ],
        check=True,
    )
    subprocess.run(
        ["llvm-objcopy", "-O", "binary", str(elf), str(raw)],
        check=True,
    )
    code = raw.read_bytes()
    if len(code) > 0x2000:
        raise SystemExit(f"Stage-2 stub code too large ({len(code)} > 0x2000)")

    text_file_size = 0x3000  # 0x1000 header + up to 0x2000 code/rodata

    def seg64(name: bytes, addr: int, vmsz: int, foff: int, fsz: int, prot: int) -> bytes:
        return struct.pack(
            "<II16sQQQQiiII",
            0x19,  # LC_SEGMENT_64
            72,
            name,
            addr,
            vmsz,
            foff,
            fsz,
            prot,
            prot,
            0,
            0,
        )

    thread_state = bytearray(272)
    struct.pack_into("<Q", thread_state, 256, BASE_VA + 0x1000)  # PC = _start
    lc_unixthread = struct.pack("<IIII", 5, 288, 6, 68) + bytes(thread_state)

    commands = (
        seg64(b"__TEXT", BASE_VA, text_file_size, 0, text_file_size, 5)
        + seg64(b"__DATA", BASE_VA + 0x4000, 0x1000, 0, 0, 3)
        + lc_unixthread
    )
    image = bytearray(text_file_size)
    image[:32] = struct.pack(
        "<8I",
        MH_MAGIC_64,
        CPU_TYPE_ARM64,
        CPU_SUBTYPE_ARM64E_PTR_AUTH,
        MH_EXECUTE,
        3,
        len(commands),
        0,
        0,
    )
    image[32 : 32 + len(commands)] = commands
    image[0x1000 : 0x1000 + len(code)] = code
    return bytes(image)


def build_adt() -> bytes:
    return adt_node(
        "device-tree",
        children=[
            adt_node(
                "chosen",
                children=[
                    adt_node(
                        "memory-map",
                        properties=[
                            ("RAMDisk", bytes(16)),
                            ("TrustCache", bytes(16)),
                        ],
                    )
                ],
            )
        ],
    )


def build_trustcache() -> bytes:
    module = struct.pack("<I16sI", 2, bytes(range(16)), 1) + bytes([0x11]) * 20 + bytes([2, 0, 0, 0])
    return struct.pack("<2I", 1, 8) + module


def build_ramdisk() -> bytes:
    return b"DRAMDISK" + bytes(4088) + b"\xA5"


def build_unixnu_toml(timeout: int) -> str:
    return (
        "version = 1\n\n"
        "[boot]\n"
        f"timeout = {timeout}\n"
        "default = \"violin-xnu-probe\"\n"
        "rotate = 0\n\n"
        "[ui]\n"
        "title = \"UniXNU (Xiaomi Pad 7S Pro - violin)\"\n\n"
        "[[entry]]\n"
        "id = \"violin-xnu-probe\"\n"
        "title = \"Violin Stage-2 arm64e Mach-O Probe\"\n"
        "type = \"xnu\"\n"
        "path = \"violin_xnu_stub.macho\"\n"
        "initrd = \"ramdisk.bin\"\n"
        "args = \"violin.stage2=1 debug=0x8 serial=0\"\n\n"
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
    parser.add_argument("--out", type=Path, required=True, help="Output FAT16 payload disk image")
    parser.add_argument("--qemu", action="store_true", help="Build QEMU stub (PL011 + PSCI SYSTEM_OFF)")
    parser.add_argument("--timeout", type=int, default=3, help="UniXNU menu timeout in seconds")
    parser.add_argument("--size-kb", type=int, default=4096, help="FAT16 image size in KiB (default 4096 = 4 MiB)")
    args = parser.parse_args()

    for tool in ("clang", "ld.lld", "llvm-objcopy", "mkfs.vfat", "mmd", "mcopy"):
        if not shutil.which(tool):
            raise SystemExit(f"Missing required tool on PATH: {tool}")

    stub_src = Path(__file__).with_name("violin_xnu_stub.S")
    args.out.parent.mkdir(parents=True, exist_ok=True)

    with tempfile.TemporaryDirectory() as tmp:
        work = Path(tmp)
        tree = work / "EFI" / "UniXNU"
        tree.mkdir(parents=True)

        macho = build_macho(stub_src, work, args.qemu)
        adt = build_adt()
        tc = build_trustcache()
        rd = build_ramdisk()
        toml = build_unixnu_toml(args.timeout)

        (tree / "violin_xnu_stub.macho").write_bytes(macho)
        (tree / "violin_xnu_stub.macho.adt").write_bytes(adt)
        (tree / "violin_xnu_stub.macho.trustcache").write_bytes(tc)
        (tree / "ramdisk.bin").write_bytes(rd)
        (tree / "unixnu.toml").write_text(toml, encoding="ascii")

        fat_img = args.out
        if fat_img.exists():
            fat_img.unlink()
        subprocess.run(
            ["mkfs.vfat", "-F", "16", "-s", "1", "-n", "VIOLINBOOT", "-C", str(fat_img), str(args.size_kb)],
            check=True,
            stdout=subprocess.DEVNULL,
        )
        subprocess.run(["mmd", "-i", str(fat_img), "::/EFI", "::/EFI/UniXNU"], check=True)
        for p in sorted(tree.iterdir()):
            subprocess.run(["mcopy", "-i", str(fat_img), str(p), f"::/EFI/UniXNU/{p.name}"], check=True)

        print(
            f"Built FAT16 Stage-2 payload -> {fat_img} ({fat_img.stat().st_size} B) "
            f"[macho={len(macho)} B arm64e, adt={len(adt)} B, tc={len(tc)} B, rd={len(rd)} B, qemu={args.qemu}]"
        )


if __name__ == "__main__":
    main()

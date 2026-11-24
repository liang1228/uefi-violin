# uefi-violin

Minimal self-contained AArch64 EDK II UEFI firmware (`ViolinPkg`) for Xiaomi Pad 7S Pro (`violin` / O81A / XRING O1), built on `Mu_Basecore` + `UniXNUPkg` + `DarwinBootPkg`.

- Target handoff verified in Stage-0/1A: `EDK2-XRING` EL1 (`CurrentEL=0x4`), `LOAD_PC=0x200000`, `X0=0x8200000` FDT, MMU/caches off.
- UEFI DRAM window: Bank 1 `0x100000000..0x180000000` (2 GiB), `FD_BASE=0x100000000`, `FD_SIZE=0x400000` (4 MiB).
- Persistent RAM logging: `0xA0040000` (`console-ramoops-0`, 512 KiB) & `0xA00C0000` (`pmsg-ramoops-0`, 256 KiB), `"DBGC"` header; physical UART3 (`0xEC118000`) disabled to avoid external aborts.
- Live DPU GOP framebuffer: dynamically read from `0xE7000000` (`SCENE0_RCH_EN` / `MIXER0` / `RCH9`, fallback `0x29BFF000`, `3200x2136`, `BGRX` 32bpp).
- Physical button input (`ViolinPmicKeyDxe`): polls XRING O1 non-secure SPMI Channel 1 (`0xE00011B0` / `0xE00011A0`) -> PMIC SID `0xF` `TOP_REG_PCTRL_STATUS3` (`0x0E04`, active-LOW Vol+/Vol-/Power) at 50 Hz, publishing `SimpleTextIn` + `SimpleTextInEx` + `ConsoleInDevice` + `DevicePath` (QEMU builds poll PL011 RX).
- Embedded FAT payload disk (`BootShim` `"VNPDISK1"` trailer + `ViolinPayloadDiskDxe`): relocates an optional FAT16/FAT32 volume appended behind `VIOLIN_UEFI.fd` inside `boot_b` to `0x100800000` (`EfiBootServicesData`) and publishes a read-only 512 B `BlockIo` device mounted automatically by `PartitionDxe` + `EnhancedFatDxe` for `\EFI\UniXNU\unixnu.toml` and `DarwinBoot` payloads.
- Stage-2 ABI verification payload (`Tools/Stage2Stub/`): builds an `arm64e` (`0x80000002`) `MH_EXECUTE` Mach-O stub + `.adt` + `.trustcache` + `ramdisk.bin` + `unixnu.toml` into a 4 MiB FAT16 image (`Scripts/build-violin-bootimg.sh --with-stage2-payload`).

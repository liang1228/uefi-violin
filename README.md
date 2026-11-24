# uefi-violin

Minimal self-contained AArch64 EDK II UEFI firmware (`ViolinPkg`) for Xiaomi Pad 7S Pro (`violin` / O81A / XRING O1), built on `Mu_Basecore` + `UniXNUPkg` + `DarwinBootPkg`.

- Target handoff verified in Stage-0/1A: `EDK2-XRING` EL1 (`CurrentEL=0x4`), `LOAD_PC=0x200000`, `X0=0x8200000` FDT, MMU/caches off.
- UEFI DRAM window: Bank 1 `0x100000000..0x180000000` (2 GiB), `FD_BASE=0x100000000`, `FD_SIZE=0x400000` (4 MiB).
- Persistent RAM logging: `0xA0040000` (`console-ramoops-0`, 512 KiB) & `0xA00C0000` (`pmsg-ramoops-0`, 256 KiB), `"DBGC"` header; physical UART3 (`0xEC118000`) disabled to avoid external aborts.
- Live DPU GOP framebuffer: dynamically read from `0xE7000000` (`SCENE0_RCH_EN` / `MIXER0` / `RCH9`, fallback `0x29BFF000`, `3200x2136`, `BGRX` 32bpp).

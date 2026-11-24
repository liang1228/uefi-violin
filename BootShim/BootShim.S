/*
 * BootShim for Xiaomi Pad 7S Pro (violin / O81A / XRING O1).
 *
 * SPDX-License-Identifier: BSD-2-Clause-Patent
 *
 * Entered at EL1 (LOAD_PC=0x200000 on violin, 0x40200000 on QEMU virt) with:
 *   x0 = physical FDT address (0x08200000 on violin)
 *   SCTLR_EL1.M = 0 (MMU off)
 *
 * Actions:
 *   1. Save x0 (FDT PA) and entry PC into fixed handoff page at HANDOFF_BASE (0x100480000).
 *   2. Initialize Linux persistent_ram "DBGC" headers at 0xA0040000 (console) & 0xA00C0000 (pmsg)
 *      and write an early BootShim banner.
 *   3. Relocate appended VIOLIN_UEFI.fd (FD_SIZE = 0x400000) to FD_BASE (0x100000000).
 *   4. Clean D-cache + invalidate I-cache and jump to FD_BASE (PeilessSec _ModuleEntryPoint).
 */

.arch armv8-a

#ifndef FD_BASE
#define FD_BASE 0x100000000
#endif

#ifndef FD_SIZE
#define FD_SIZE 0x400000
#endif

#define HANDOFF_BASE         0x100480000
#define HANDOFF_MAGIC        0x56494F484F423031  /* "VIOHOB01" */
#define RAMOOPS_CONSOLE_BASE 0xA0040000
#define RAMOOPS_PMSG_BASE    0xA00C0000
#define PERSISTENT_RAM_SIG   0x43474244          /* "DBGC" */
#define O1_RAM_LOG_BASE      0x100400000

.section .text.head, "ax"
.global _shim_base
_shim_base:
    .word 0x91005a4d                /* +0x00: "MZ" (`add x13, x18, #0x16`), preserves x0..x3 */
    b     _start                    /* +0x04: jump to BootShim entry */
    .quad 0x0                       /* +0x08: text_offset = 0 */
    .quad (FD_SIZE + 0x10000)       /* +0x10: image_size */
    .quad 0xa                       /* +0x18: flags = LE, 4K, anywhere */
    .quad 0x0                       /* +0x20 */
    .quad 0x0                       /* +0x28 */
    .quad 0x0                       /* +0x30 */
    .word 0x644d5241                /* +0x38: "ARM\x64" */
    .word 0x0                       /* +0x3c */

.section .text, "ax"
_start:
    mov   x20, x0                   /* x20 = incoming FDT phys addr */
    adr   x21, _shim_base           /* x21 = actual load PC */
    msr   daifset, #0xf
    msr   spsel, #1
    isb

    /* 1. Save handoff record at HANDOFF_BASE (Bank 1 reserved page) */
    ldr   x0, =HANDOFF_BASE
    ldr   x1, =HANDOFF_MAGIC
    str   x1, [x0, #0]
    str   x20, [x0, #8]             /* FdtBase */
    str   x21, [x0, #16]            /* LoadPc */
    mrs   x2, CurrentEL
    str   x2, [x0, #24]             /* CurrentEL */
    str   xzr, [x0, #32]
    dc    civac, x0
    dsb   sy

    /* Clear O1 primary RAM log header so O1SerialPortLib initializes cleanly */
    ldr   x0, =O1_RAM_LOG_BASE
    str   xzr, [x0, #0]
    str   xzr, [x0, #8]
    str   xzr, [x0, #16]
    str   xzr, [x0, #24]
    dc    civac, x0
    dsb   sy

    /* 2. Initialize persistent_ram "DBGC" headers + early BootShim line in both rings */
    ldr   x0, =RAMOOPS_CONSOLE_BASE
    bl    init_ramoops_ring
    ldr   x0, =RAMOOPS_PMSG_BASE
    bl    init_ramoops_ring

    /* 3. Relocate appended UEFI FD (at _fd_payload) -> FD_BASE (8-byte aligned) */
    adr   x1, _fd_payload
    ldr   x2, =FD_BASE
    ldr   x3, =FD_SIZE
    cmp   x1, x2
    b.eq  .Lcopy_done
    mov   x4, #0
.Lcopy_loop:
    cmp   x4, x3
    b.hs  .Lcopy_done
    ldr   x5, [x1, x4]
    str   x5, [x2, x4]
    add   x4, x4, #8
    b     .Lcopy_loop
.Lcopy_done:
    dsb   sy

    /* Clean D-cache across relocated FD and invalidate I-cache */
    ldr   x0, =FD_BASE
    ldr   x1, =FD_SIZE
    add   x1, x0, x1
.Lflush_fd:
    cmp   x0, x1
    b.hs  .Lflush_done
    dc    cvau, x0
    add   x0, x0, #64
    b     .Lflush_fd
.Lflush_done:
    dsb   ish
    ic    iallu
    dsb   sy
    isb

    /* 4. Jump to PeilessSec _ModuleEntryPoint at FD_BASE with x0..x3 = 0 */
    mov   x0, #0
    mov   x1, #0
    mov   x2, #0
    mov   x3, #0
    ldr   x4, =FD_BASE
    br    x4

/* init_ramoops_ring: x0 = ring base (0xA0040000 or 0xA00C0000) */
init_ramoops_ring:
    adr   x1, s_shim_msg
    add   x2, x0, #12
    mov   x3, #0
1:
    ldrb  w4, [x1, x3]
    cbz   w4, 2f
    strb  w4, [x2, x3]
    add   x3, x3, #1
    b     1b
2:
    dmb   sy
    ldr   w4, =PERSISTENT_RAM_SIG
    str   w4, [x0, #0]
    str   w3, [x0, #4]
    str   w3, [x0, #8]
    dsb   sy
    add   x5, x0, #256
    mov   x6, x0
3:
    cmp   x6, x5
    b.hs  4f
    dc    civac, x6
    add   x6, x6, #64
    b     3b
4:
    dsb   sy
    isb
    ret

.section .rodata, "a"
s_shim_msg:
    .asciz "[VIOLIN_BOOTSHIM] Relocating UEFI FD -> 0x100000000 (PeilessSec)\n"

.extern _fd_payload

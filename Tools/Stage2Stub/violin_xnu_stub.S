/*
 * Stage-2 arm64e Mach-O verification kernel for Xiaomi Pad 7S Pro (violin).
 *
 * Entered by DarwinBoot (DarwinEnter) at EL1 with MMU/caches OFF and:
 *   x0 = physical address of DARWIN_BOOT_ARGS (Darwin 23 arm64 ABI)
 *
 * Verifies:
 *   bit 0: CurrentEL == EL1 (0x4)
 *   bit 1: SCTLR_EL1 (M=0, C=0, I=0)
 *   bit 2: boot_args Revision==2 & Version==2 (0x00020002)
 *   bit 3: VirtualBase == 0xfffffff007004000 & PhysicalBase 2 MiB-aligned & entry offset 0x1000
 *   bit 4: MemorySize == MemorySizeActual == 0x10000000 & TopOfKernelData in bounds
 *   bit 5: Video GOP handoff valid (base!=0, display==1, row>=w*4, w>0, h>0, depth==32)
 *   bit 6: ADT header valid + RAMDisk (0x2000 B, "DRAMDISK", 0xA5) + TrustCache (56 B, v1)
 *   bit 7: __DATA BSS (4 KiB at PA+0x4000) zero-filled
 *   bit 8: CommandLine starts with "violin.stage2=1"
 *   bit 9: EL1 arm64e PAC (PACIA + AUTIA round-trip with EnIA=1)
 *
 * Appends decoded report to existing persistent_ram ("DBGC") ramoops rings at
 * 0xA0040000 & 0xA00C0000 (plus QEMU PL011 when QEMU_STUB=1), paints a cyan
 * PASS (or red FAIL) stripe right below DarwinBoot's top green bar, and cleanly
 * invokes PSCI SYSTEM_OFF (QEMU) or PSCI SYSTEM_RESET (violin, after 2s).
 *
 * SPDX-License-Identifier: BSD-2-Clause-Patent
 */

.arch armv8.3-a

#ifndef QEMU_STUB
#define QEMU_STUB 0
#endif

.equ EXPECTED_VBASE_LO,    0x07004000
.equ EXPECTED_VBASE_HI,    0xfffffff0
.equ EXPECTED_WINDOW,      0x10000000
.equ RAMOOPS_CONSOLE_BASE, 0xA0040000
.equ RAMOOPS_CONSOLE_CAP,  (0x80000 - 12)
.equ RAMOOPS_PMSG_BASE,    0xA00C0000
.equ RAMOOPS_PMSG_CAP,     (0x40000 - 12)
.equ PERSISTENT_RAM_SIG,   0x43474244
.equ QEMU_PL011_BASE,      0x09000000
.equ ALL_CHECKS_MASK,      0x3FF

.section .text, "ax"
.global _start
_start:
    msr   daifset, #0xf
    msr   spsel, #1
    isb

    mov   x19, x0                   /* x19 = DARWIN_BOOT_ARGS PA */
    mov   x20, #0                   /* x20 = bitmask of passed checks (0..9) */

    /* Set up temporary stack inside our zero-filled __DATA page (PA + 0x4FF0) */
    ldr   x21, [x19, #0x10]         /* x21 = PhysicalBase */
    /* Wait: verify __DATA zero-fill BEFORE touching stack inside __DATA!
     * Actually check __DATA first using pure registers, then set SP = PA + 0x4FF0! */

    /* Check 7: __DATA BSS (PA + 0x4000, 4096 B) is zero-filled */
    add   x1, x21, #0x4000
    mov   x2, #0x200                /* 512 * 8 = 4096 B */
    mov   x3, #0
.Lbss_loop:
    ldr   x4, [x1], #8
    orr   x3, x3, x4
    subs  x2, x2, #1
    b.ne  .Lbss_loop
    cbnz  x3, .Lbss_done
    orr   x20, x20, #(1 << 7)
.Lbss_done:

    /* Now safe to place stack at top of __DATA page (PA + 0x4FE0) */
    add   x1, x21, #0x4000
    add   x1, x1, #0xfe0
    mov   sp, x1

    /* Check 0: CurrentEL == EL1 (0x4) */
    mrs   x22, CurrentEL
    cmp   x22, #4
    b.ne  .Lchk1
    orr   x20, x20, #(1 << 0)

.Lchk1:
    /* Check 1: SCTLR_EL1 M=0, C=0, I=0 */
    mrs   x23, sctlr_el1
    mov   x1, #0x1005
    tst   x23, x1
    b.ne  .Lchk2
    orr   x20, x20, #(1 << 1)

.Lchk2:
    /* Check 2: boot_args Revision==2, Version==2 */
    ldr   w1, [x19, #0x00]
    mov   w2, #0x0002
    movk  w2, #0x0002, lsl #16
    cmp   w1, w2
    b.ne  .Lchk3
    orr   x20, x20, #(1 << 2)

.Lchk3:
    /* Check 3: VirtualBase == 0xfffffff007004000, 2 MiB align, _start - PA == 0x1000 */
    ldr   x24, [x19, #0x08]         /* x24 = VirtualBase */
    ldr   x1, =0xfffffff007004000
    cmp   x24, x1
    b.ne  .Lchk4
    eor   x2, x24, x21
    tst   x2, #0x1fffff
    b.ne  .Lchk4
    adr   x2, _start
    sub   x2, x2, x21
    cmp   x2, #0x1000
    b.ne  .Lchk4
    orr   x20, x20, #(1 << 3)

.Lchk4:
    /* Check 4: MemorySize == MemorySizeActual == 0x10000000 & TopOfKernelData valid */
    ldr   x25, [x19, #0x18]         /* x25 = MemorySize */
    ldr   x1, =EXPECTED_WINDOW
    cmp   x25, x1
    b.ne  .Lchk5
    ldr   x2, [x19, #0x478]         /* MemorySizeActual */
    cmp   x2, x1
    b.ne  .Lchk5
    ldr   x26, [x19, #0x20]         /* x26 = TopOfKernelData */
    add   x3, x21, x25
    cmp   x26, x3
    b.hi  .Lchk5
    cmp   x26, x19
    b.ls  .Lchk5
    orr   x20, x20, #(1 << 4)

.Lchk5:
    /* Check 5: Video GOP handoff (base, display==1, row>=w*4, w>0, h>0, depth==32) */
    ldp   x1, x2, [x19, #0x28]      /* base, display */
    cbz   x1, .Lchk6
    cmp   x2, #1
    b.ne  .Lchk6
    ldp   x3, x4, [x19, #0x38]      /* rowBytes, width */
    cbz   x4, .Lchk6
    cmp   x3, x4, lsl #2
    b.lo  .Lchk6
    ldp   x5, x6, [x19, #0x48]      /* height, depth */
    cbz   x5, .Lchk6
    cmp   x6, #32
    b.ne  .Lchk6
    orr   x20, x20, #(1 << 5)

.Lchk6:
    /* Check 6: ADT + RAMDisk + TrustCache */
    ldr   x6, [x19, #0x60]          /* DeviceTree VA */
    sub   x6, x6, x24
    add   x6, x6, x21               /* x6 = DeviceTree PA */
    mov   x27, x6                   /* save DT PA in x27 */
    ldr   w7, [x6, #0]
    cmp   w7, #1
    b.ne  .Lchk8
    ldr   w7, [x6, #4]
    cmp   w7, #1
    b.ne  .Lchk8
    ldr   w7, [x6, #40]
    cmp   w7, #12
    b.ne  .Lchk8

    /* Patched RAMDisk at ADT + 200 */
    add   x14, x6, #200
    ldp   w9, w15, [x14]
    orr   x9, x9, x15, lsl #32      /* x9 = RAMDisk PA */
    ldp   w10, w15, [x14, #8]
    orr   x10, x10, x15, lsl #32    /* x10 = RAMDisk size */
    cmp   x10, #0x2000
    b.ne  .Lchk8
    tst   x9, #0xfff
    b.ne  .Lchk8
    cmp   x9, x21
    b.ls  .Lchk8
    add   x11, x9, x10
    ldr   x12, [x19, #0x20]
    cmp   x11, x12
    b.hi  .Lchk8
    ldr   x11, [x9]
    ldr   x12, =0x4b5349444d415244  /* "DRAMDISK" */
    cmp   x11, x12
    b.ne  .Lchk8
    add   x14, x9, #0x1000
    ldrb  w11, [x14]
    cmp   w11, #0xa5
    b.ne  .Lchk8

    /* Patched TrustCache at ADT + 252 */
    add   x14, x6, #252
    ldp   w12, w15, [x14]
    orr   x12, x12, x15, lsl #32    /* x12 = TrustCache PA */
    ldp   w13, w15, [x14, #8]
    orr   x13, x13, x15, lsl #32    /* x13 = TrustCache size */
    cmp   x13, #56
    b.ne  .Lchk8
    tst   x12, #0xfff
    b.ne  .Lchk8
    ldp   w13, w14, [x12]
    cmp   w13, #1
    b.ne  .Lchk8
    cmp   w14, #8
    b.ne  .Lchk8
    ldrb  w13, [x12, #32]
    cmp   w13, #0x11
    b.ne  .Lchk8
    orr   x20, x20, #(1 << 6)

.Lchk8:
    /* Check 8: CommandLine starts with "violin.stage2=1" */
    adr   x1, s_expected_cmd
    add   x2, x19, #0x6c
.Lcmd_cmp:
    ldrb  w3, [x1], #1
    cbz   w3, .Lcmd_ok
    ldrb  w4, [x2], #1
    cmp   w3, w4
    b.ne  .Lchk9
    b     .Lcmd_cmp
.Lcmd_ok:
    orr   x20, x20, #(1 << 8)

.Lchk9:
    /* Check 9: arm64e EL1 PAC round-trip (enable EnIA temporarily, PACIA + AUTIA) */
    ldr   x1, =0x1122334455667788
    ldr   x2, =0x99aabbccddeeff00
    msr   S3_0_C2_C1_0, x1          /* APIAKeyLo_EL1 */
    msr   S3_0_C2_C1_1, x2          /* APIAKeyHi_EL1 */
    mrs   x3, sctlr_el1
    orr   x4, x3, #(1 << 31)        /* EnIA = 1 */
    msr   sctlr_el1, x4
    isb
    ldr   x5, =0x0000000100123456   /* canonical test pointer */
    mov   x6, x5
    mov   x7, #0x1234
    pacia x6, x7
    cmp   x6, x5                    /* PAC bits must modify upper address bits */
    b.eq  .Lpac_restore
    autia x6, x7
    cmp   x6, x5                    /* AUTIA must recover exact original pointer */
    b.ne  .Lpac_restore
    orr   x20, x20, #(1 << 9)
.Lpac_restore:
    msr   sctlr_el1, x3
    isb

    /* ---- Paint status stripe right below DarwinBoot's top 1/16 green bar ---- */
    ldp   x1, x2, [x19, #0x28]      /* x1 = FB base, x2 = display */
    cbz   x1, .Llog_report
    ldp   x3, x4, [x19, #0x38]      /* x3 = rowBytes, x4 = width */
    ldr   x5, [x19, #0x48]          /* x5 = height */
    lsr   x6, x5, #4                /* y0 = height / 16 */
    add   x7, x6, #24               /* 24-pixel tall confirmation bar */
    cmp   x7, x5
    csel  x7, x5, x7, hi
    /* Cyan (0xFF00E5FF) if all 10 checks pass, else Red (0xFFFF2020) */
    cmp   x20, #ALL_CHECKS_MASK
    ldr   w8, =0xFF00E5FF
    ldr   w9, =0xFFFF2020
    csel  w8, w8, w9, eq
.Lfb_row:
    cmp   x6, x7
    b.hs  .Lfb_done
    mul   x10, x6, x3
    add   x10, x1, x10
    mov   x11, #0
.Lfb_col:
    cmp   x11, x4
    b.hs  .Lfb_next
    str   w8, [x10, x11, lsl #2]
    add   x11, x11, #1
    b     .Lfb_col
.Lfb_next:
    add   x6, x6, #1
    b     .Lfb_row
.Lfb_done:
    dsb   sy

    /* ---- Emit structured post-ExitBootServices report into ramoops & QEMU PL011 ---- */
.Llog_report:
    adr   x0, s_hdr
    bl    emit_str

    adr   x0, k_checks
    mov   x1, x20
    bl    emit_kv64

    adr   x0, k_el
    mov   x1, x22
    bl    emit_kv64

    adr   x0, k_sctlr
    mov   x1, x23
    bl    emit_kv64

    adr   x0, k_va
    mov   x1, x24
    bl    emit_kv64

    adr   x0, k_pa
    mov   x1, x21
    bl    emit_kv64

    adr   x0, k_memsz
    mov   x1, x25
    bl    emit_kv64

    adr   x0, k_top
    mov   x1, x26
    bl    emit_kv64

    adr   x0, k_fb_base
    ldr   x1, [x19, #0x28]
    bl    emit_kv64

    adr   x0, k_fb_row
    ldr   x1, [x19, #0x38]
    bl    emit_kv64

    adr   x0, k_fb_w
    ldr   x1, [x19, #0x40]
    bl    emit_kv64

    adr   x0, k_fb_h
    ldr   x1, [x19, #0x48]
    bl    emit_kv64

    adr   x0, k_dt_pa
    mov   x1, x27
    bl    emit_kv64

    adr   x0, k_cmd
    bl    emit_str
    add   x0, x19, #0x6c
    bl    emit_str
    adr   x0, s_nl
    bl    emit_str

    cmp   x20, #ALL_CHECKS_MASK
    b.ne  .Lemit_fail
    adr   x0, s_pass
    bl    emit_str
    b     .Lexit_action
.Lemit_fail:
    adr   x0, s_fail
    bl    emit_str

.Lexit_action:
    dsb   sy
    isb
#if QEMU_STUB
    ldr   w0, =0x84000008           /* PSCI SYSTEM_OFF on QEMU virt */
    hvc   #0
#else
    /* Pause ~2.0 seconds so green + cyan bars are visible on violin display */
    mrs   x1, cntfrq_el0
    cbz   x1, .Ldo_reset
    lsl   x1, x1, #1                /* 2 seconds */
    mrs   x2, cntpct_el0
    add   x2, x2, x1
.Lwait_vis:
    mrs   x3, cntpct_el0
    cmp   x3, x2
    b.lo  .Lwait_vis
.Ldo_reset:
    ldr   w0, =0x84000009           /* PSCI SYSTEM_RESET -> auto-return to slot a */
    smc   #0
#endif
.Lhalt:
    wfe
    b     .Lhalt

/* ---- Output helpers: append single byte (w0) to both ramoops rings + optional PL011 ---- */

ring_append_byte:
    /* x1 = ring base, x2 = capacity, w0 = byte */
    ldr   w3, [x1, #0]
    ldr   w4, =PERSISTENT_RAM_SIG
    cmp   w3, w4
    b.ne  .Lring_ret
    ldr   w5, [x1, #4]              /* start (write offset) */
    ldr   w6, [x1, #8]              /* size */
    cmp   w5, w2
    b.hs  .Lring_ret
    add   x7, x1, #12
    strb  w0, [x7, x5]
    add   w5, w5, #1
    cmp   w5, w2
    csel  w5, wzr, w5, hs
    cmp   w6, w2
    add   w8, w6, #1
    csel  w6, w8, w2, lo
    dmb   sy
    str   w5, [x1, #4]
    str   w6, [x1, #8]
    dsb   sy
    /* Clean cache lines of both header and written byte */
    dc    civac, x1
    add   x7, x7, x5
    bic   x7, x7, #63
    dc    civac, x7
    dsb   sy
.Lring_ret:
    ret

emit_byte:
    stp   x29, x30, [sp, #-32]!
    str   x19, [sp, #16]
    and   w19, w0, #0xff
#if QEMU_STUB
    ldr   x1, =QEMU_PL011_BASE
1:
    ldr   w2, [x1, #0x18]
    tbnz  w2, #5, 1b
    str   w19, [x1, #0x00]
#endif
    mov   w0, w19
    ldr   x1, =RAMOOPS_CONSOLE_BASE
    ldr   w2, =RAMOOPS_CONSOLE_CAP
    bl    ring_append_byte

    mov   w0, w19
    ldr   x1, =RAMOOPS_PMSG_BASE
    ldr   w2, =RAMOOPS_PMSG_CAP
    bl    ring_append_byte

    ldr   x19, [sp, #16]
    ldp   x29, x30, [sp], #32
    ret

emit_str:
    stp   x29, x30, [sp, #-32]!
    str   x19, [sp, #16]
    mov   x19, x0
1:
    ldrb  w0, [x19], #1
    cbz   w0, 2f
    bl    emit_byte
    b     1b
2:
    ldr   x19, [sp, #16]
    ldp   x29, x30, [sp], #32
    ret

emit_hex64:
    stp   x29, x30, [sp, #-32]!
    stp   x19, x20, [sp, #16]
    mov   x19, x0
    mov   x20, #60
1:
    lsr   x0, x19, x20
    and   w0, w0, #0xf
    cmp   w0, #10
    b.lo  2f
    add   w0, w0, #('a' - 10)
    b     3f
2:
    add   w0, w0, #'0'
3:
    bl    emit_byte
    cbz   x20, 4f
    sub   x20, x20, #4
    b     1b
4:
    ldp   x19, x20, [sp, #16]
    ldp   x29, x30, [sp], #32
    ret

emit_kv64:
    stp   x29, x30, [sp, #-32]!
    str   x19, [sp, #16]
    mov   x19, x1
    bl    emit_str
    mov   w0, #'0'
    bl    emit_byte
    mov   w0, #'x'
    bl    emit_byte
    mov   x0, x19
    bl    emit_hex64
    mov   w0, #'\n'
    bl    emit_byte
    ldr   x19, [sp, #16]
    ldp   x29, x30, [sp], #32
    ret

.section .rodata, "a"
s_expected_cmd: .asciz "violin.stage2=1"
s_hdr:          .asciz "\n=== [VIOLIN_XNU_STUB] POST-EXITBOOTSERVICES ARM64E MACH-O ENTRY ===\n"
k_checks:       .asciz "STUB_CHECKS_MASK="
k_el:           .asciz "STUB_CURRENTEL="
k_sctlr:        .asciz "STUB_SCTLR_EL1="
k_va:           .asciz "STUB_VIRT_BASE="
k_pa:           .asciz "STUB_PHYS_BASE="
k_memsz:        .asciz "STUB_MEM_SIZE="
k_top:          .asciz "STUB_TOP_OF_KERNEL_DATA="
k_fb_base:      .asciz "STUB_VIDEO_BASE="
k_fb_row:       .asciz "STUB_VIDEO_ROWBYTES="
k_fb_w:         .asciz "STUB_VIDEO_WIDTH="
k_fb_h:         .asciz "STUB_VIDEO_HEIGHT="
k_dt_pa:        .asciz "STUB_ADT_PA="
k_cmd:          .asciz "STUB_CMDLINE="
s_nl:           .asciz "\n"
s_pass:         .asciz "[VIOLIN_XNU_STUB] RESULT=PASS (all 10 checks incl. arm64e PAC + GOP + ADT + RAMDisk + TrustCache)\n"
s_fail:         .asciz "[VIOLIN_XNU_STUB] RESULT=FAIL\n"

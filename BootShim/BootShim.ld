ENTRY(_shim_base)
SECTIONS
{
    . = 0x0;
    .text : {
        KEEP(*(.text.head))
        *(.text*)
    }
    .rodata : ALIGN(16) {
        *(.rodata*)
    }
    . = ALIGN(4096);
    _fd_payload = .;
    /DISCARD/ : {
        *(.comment*)
        *(.note*)
        *(.eh_frame*)
    }
}

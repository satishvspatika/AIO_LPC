    .syntax unified
    .cpu cortex-m3
    .thumb

    .section .vectors, "a"
    .word 0x10008000        /* Initial SP */
    .word Reset_Stub + 1   /* Reset handler (Thumb) */
    .word 0, 0, 0, 0, 0     /* Reserved */
    .word 0                 /* Checksum placeholder (offset 0x1C) */

    .section .text
    .align 4
    .thumb_func
    .global Reset_Stub
Reset_Stub:
    ldr r0, =0xE000ED08    /* SCB->VTOR */
    ldr r1, =0x00002000    /* Bootloader base */
    str r1, [r0]
    ldr r0, [r1]           /* Bootloader initial SP */
    msr msp, r0
    ldr r0, [r1, #4]       /* Bootloader reset */
    bx  r0


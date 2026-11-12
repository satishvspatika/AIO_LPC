/* Copied from TEST_JIG/migrate (MIG-1.4, proven on old unit 2114) for ctrl_isp.c (v6.9.33). */
/* The LPC845's sector 0 while M1 rewrites the rest of its flash (TEST_JIG.md section 13).
 *
 * If the power is cut mid-way, the LPC845 starts this at the next power-up: it drives the rail lines
 * (LPC1768 and LCD on, as the CTRL firmware does), then restarts the ROM loader on the USART. The
 * LPC1768 powers up, its migration image finds the ROM loader already answering and carries on.
 * Without it, a half-written LPC845 would sit in the ROM loader with its pull-ups switching the
 * LPC1768 off (measured 2026-11-11), and the unit could only be fixed with a probe or the jig.
 *
 * A normal vector table: the boot ROM checks that words 0-7 sum to zero (UM11029 5.3.4), build.sh
 * fills word 7. Addresses from LPC845/device/LPC845.h; IAP 57 = Reinvoke ISP, 1 = USART (5.6.8). */
    .syntax unified
    .cpu cortex-m0plus
    .thumb
    .section .text
    .global rescue_vectors
rescue_vectors:
    .word 0x10001000            /* SP: above the ROM loader's 2 KB and the IAP tables */
    .word rescue_start + 1
    .word hang + 1              /* NMI */
    .word hang + 1              /* HardFault */
    .word 0, 0, 0
    .word 0                     /* Checksum: build.sh */
    .thumb_func
rescue_start:
    ldr  r0, =0x40048080        /* SYSCON SYSAHBCLKCTRL0: GPIO0 and SWM clocks on (off at reset) */
    ldr  r1, [r0]
    movs r2, #0xC0
    orrs r1, r2
    str  r1, [r0]
    ldr  r0, =0xA0002280        /* GPIO CLR0 */
    ldr  r1, =0x18000000        /* PIO0_27 (LPC1768), PIO0_28 (LCD): low = rail on */
    str  r1, [r0]
    ldr  r0, =0xA0002380        /* GPIO DIRSET0 */
    str  r1, [r0]
    ldr  r0, =0x10000F00        /* IAP command table */
    movs r2, #57                /* Reinvoke ISP */
    str  r2, [r0]
    movs r2, #1                 /* USART */
    str  r2, [r0, #4]
    adds r1, r0, #0
    adds r1, #64
    ldr  r3, =0x0F001FF1
    blx  r3
    .thumb_func
hang:
    b    hang
    .ltorg

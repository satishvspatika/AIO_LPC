/* Copied from TEST_JIG/migrate (MIG-1.4, proven on old unit 2114) for ctrl_isp.c (v6.9.33). */
/* Runs on the LPC845, in RAM at 0x10000800, started by the ROM loader's "G" command.
 *
 * Why: on old units the power board switches the LPC1768 off while the LPC845's rail lines float
 * (reset or ROM loader). Measured 2026-11-11 on unit 2114: the LPC1768 survives 300 ms of that, not
 * 3 s. So the LPC1768 enters the ROM loader quickly, loads this, and this drives the rail lines
 * again; then it restarts the ROM loader with the lines left as set (UM11029 5.6.8: Reinvoke ISP
 * changes the clock and the ISP interface's pins only).
 *
 * Rail lines (active low, as the CTRL firmware drives them): PIO0_27 LPC1768, PIO0_28 LCD = low (on);
 * PIO0_26 modem left an input (its pull-up keeps the modem off). Addresses from LPC845/device/LPC845.h.
 * Every byte of this is sent on hold-up power, so it is as short as it can be (52 bytes). */
    .syntax unified
    .cpu cortex-m0plus
    .thumb
    .section .text
    .global stub_start
    .thumb_func
stub_start:
    /* As short as possible: every byte is sent while the LPC1768 runs on hold-up (about 45 ms once
     * the ROM loader starts, and its serial link garbles before that). In the ROM loader the GPIO
     * and SWM clocks are already on and SPI0 is on no pin (read with the probe, 2026-11-11), so: */
    ldr  r0, =0xA0002280        /* GPIO CLR0 */
    ldr  r1, =0x18000000        /* PIO0_27 (LPC1768), PIO0_28 (LCD): low = rail on */
    str  r1, [r0]
    ldr  r0, =0xA0002380        /* GPIO DIRSET0: now outputs (PIO0_26, modem, stays an input: off) */
    str  r1, [r0]
    ldr  r0, =0x10000F00        /* IAP command table (above the ROM loader's 2 KB) */
    movs r2, #57                /* Reinvoke ISP */
    str  r2, [r0]
    movs r2, #1                 /* 1 = USART ISP */
    str  r2, [r0, #4]
    adds r1, r0, #0
    adds r1, #64                /* result table 0x10000F40 */
    ldr  r3, =0x0F001FF1        /* IAP entry */
    blx  r3
hang:
    b    hang                   /* Not reached if the ROM loader restarted */
    .ltorg

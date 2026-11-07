/* The LPC845 bootloader. It uses no interrupts or UART, runs at the reset clock, and no firmware
 * update ever changes it. The running firmware receives new images, which keeps it counting rain;
 * this only installs them. See LPC1768/boot/boot_core.c for what it decides.
 *
 * The rail pins stay at their reset state in here, which switches the LPC1768, modem and LCD off.
 * The firmware switches them back on as it starts. */
#include "LPC845.h"
#include "boot_core.h"

/* About 5 minutes, and anywhere from 3.5 to 8 with this oscillator's spread: 0.6 MHz, divided by 64
 * then by the watchdog's own 4. It can't be stopped once started. */
#define WDT_FREQSEL     1U
#define WDT_DIVSEL      31U
#define WDT_TICKS       703125UL

/* Normal firmware feeds this all the time, so a hang restarts the chip. Firmware on trial doesn't
 * feed it until the LPC1768 confirms it, so one that hangs or can't talk runs out of tries. */
static void start_watchdog(void)
{
    SYSCON->SYSAHBCLKCTRL0 |= SYSCON_SYSAHBCLKCTRL0_WWDT_MASK;
    SYSCON->PDRUNCFG &= ~SYSCON_PDRUNCFG_WDTOSC_PD_MASK;
    SYSCON->WDTOSCCTRL = SYSCON_WDTOSCCTRL_FREQSEL(WDT_FREQSEL) | SYSCON_WDTOSCCTRL_DIVSEL(WDT_DIVSEL);
    for (volatile uint32_t i = 0; i < 2000UL; i++) {
    }
    WWDT->TC = WDT_TICKS;
    WWDT->MOD = WWDT_MOD_WDEN_MASK | WWDT_MOD_WDRESET_MASK;
    WWDT->FEED = 0xAA;
    WWDT->FEED = 0x55;
}

static void delay_seconds(uint32_t seconds)
{
    for (volatile uint32_t i = 0; i < seconds * 1000000UL; i++) {
    }
}

static void __attribute__((noreturn)) jump_to_app(void)
{
    const uint32_t *vectors = (const uint32_t *)FW_SLOT_A_BASE;
    SCB->VTOR = FW_SLOT_A_BASE;
    __asm volatile(
        "msr msp, %0   \n"
        "bx  %1        \n"
        : : "r"(vectors[0]), "r"(vectors[1]));
    for (;;) {
    }
}

int main(void)
{
    switch (fw_boot_decide()) {
    case FW_BOOT_RUN_TRIAL:
    case FW_BOOT_RUN:
        start_watchdog();
        jump_to_app();
    case FW_BOOT_RETRY:
        /* Carries on from the journal */
        delay_seconds(2);
        break;
    default:
        /* Nothing valid anywhere: only MCU-Link can fix this, but keep trying */
        delay_seconds(60);
        break;
    }
    NVIC_SystemReset();
    for (;;) {
    }
}

/* ---- Startup, with no C library ---- */
extern uint32_t __data_load, __data_start, __data_end, __bss_start, __bss_end, __stack_top;

void *memcpy(void *dst, const void *src, unsigned int n)
{
    uint8_t *d = dst;
    const uint8_t *s = src;
    while (n--) {
        *d++ = *s++;
    }
    return dst;
}

void *memset(void *dst, int c, unsigned int n)
{
    uint8_t *d = dst;
    while (n--) {
        *d++ = (uint8_t)c;
    }
    return dst;
}

void Reset_Handler(void)
{
    uint32_t *src = &__data_load;
    for (uint32_t *d = &__data_start; d < &__data_end;) {
        *d++ = *src++;
    }
    for (uint32_t *b = &__bss_start; b < &__bss_end;) {
        *b++ = 0;
    }
    main();
    for (;;) {
    }
}

static void Fault_Handler(void)
{
    NVIC_SystemReset();
}

/* Only what the core can raise here. Entry 7 is the checksum the chip's boot ROM wants, which the
 * build fills in. */
__attribute__((section(".vectors"), used))
void (* const g_boot_vectors[8])(void) = {
    (void (*)(void))&__stack_top,
    Reset_Handler,
    Fault_Handler, /* NMI */
    Fault_Handler, /* HardFault */
};

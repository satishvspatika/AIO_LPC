/* The LPC1768 bootloader. It runs at the reset clock (4 MHz internal), uses no interrupts, and no
 * firmware update ever changes it. See boot_core.c for what it decides. */
#include "LPC17xx.h"
#include "boot_core.h"

#define IAP_LOCATION    0x1FFF1FF1UL
#define BOOT_CCLK_KHZ   4000UL
#define TRIAL_WDT_TICKS 300000000UL /* 5 minutes: WDT runs from the 4 MHz IRC, divided by 4 */

typedef void (*iap_entry_t)(unsigned long[], unsigned long[]);
static const iap_entry_t iap_call = (iap_entry_t)IAP_LOCATION;
static unsigned long s_cmd[5];
static unsigned long s_res[5];

const uint8_t *fw_hal_ptr(uint32_t addr)
{
    return (const uint8_t *)addr;
}

static uint32_t sector_of(uint32_t addr)
{
    return (addr < 0x10000UL) ? (addr / 0x1000UL) : (16UL + (addr - 0x10000UL) / 0x8000UL);
}

static bool iap_prepare(uint32_t sector)
{
    s_cmd[0] = 50;
    s_cmd[1] = sector;
    s_cmd[2] = sector;
    iap_call(s_cmd, s_res);
    return s_res[0] == 0;
}

bool fw_hal_erase(uint32_t sector)
{
    if (!iap_prepare(sector)) {
        return false;
    }
    s_cmd[0] = 52;
    s_cmd[1] = sector;
    s_cmd[2] = sector;
    s_cmd[3] = BOOT_CCLK_KHZ;
    iap_call(s_cmd, s_res);
    return s_res[0] == 0;
}

bool fw_hal_program(uint32_t addr, const void *src, uint32_t len)
{
    if (!iap_prepare(sector_of(addr))) {
        return false;
    }
    s_cmd[0] = 51;
    s_cmd[1] = addr;
    s_cmd[2] = (unsigned long)src;
    s_cmd[3] = len;
    s_cmd[4] = BOOT_CCLK_KHZ;
    iap_call(s_cmd, s_res);
    return s_res[0] == 0;
}

/* A new image that hangs instead of crashing still gets restarted. The application takes over
 * feeding this once it has confirmed itself. */
static void start_trial_watchdog(void)
{
    LPC_WDT->WDCLKSEL = 0;
    LPC_WDT->WDTC = TRIAL_WDT_TICKS;
    LPC_WDT->WDMOD = 3; /* Enable, reset on timeout */
    LPC_WDT->WDFEED = 0xAA;
    LPC_WDT->WDFEED = 0x55;
}

static void delay_seconds(uint32_t seconds)
{
    for (volatile uint32_t i = 0; i < seconds * 400000UL; i++) {
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
        start_trial_watchdog();
        jump_to_app();
    case FW_BOOT_RUN:
        jump_to_app();
    case FW_BOOT_RETRY:
        /* Carries on from the journal */
        delay_seconds(2);
        break;
    default:
        /* Nothing valid anywhere: the LPC845's hang timer ends this, and it tries again */
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

__attribute__((section(".vectors"), used))
void (* const g_boot_vectors[])(void) = {
    (void (*)(void))&__stack_top,
    Reset_Handler,
    Fault_Handler, /* NMI */
    Fault_Handler, /* HardFault */
    Fault_Handler, /* MemManage */
    Fault_Handler, /* BusFault */
    Fault_Handler, /* UsageFault */
};

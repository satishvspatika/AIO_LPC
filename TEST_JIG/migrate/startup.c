/* Start-up for the migration image. No RTOS: one loop, a 1 ms SysTick for timeouts.
 * The old SD bootloader has already set VTOR to 0x18000 and run the PLL set-up; SystemInit runs
 * again here, as it did for the old application it replaces. */
#include <stdint.h>
#include "LPC17xx.h"
#include "system_LPC17xx.h"

extern uint32_t __stack_top, __data_load, __data_start, __data_end, __bss_start, __bss_end;
extern const char g_fw_image_info[];
int main(void);
void SysTick_Handler(void);

void Reset_Handler(void);
static void Fault_Handler(void)
{
    for (;;) {
    }
}

__attribute__((section(".isr_vector"), used))
void (* const g_vectors[])(void) = {
    (void (*)(void))&__stack_top,
    Reset_Handler,
    Fault_Handler,                          /* NMI */
    Fault_Handler,                          /* HardFault */
    Fault_Handler,                          /* MemManage */
    Fault_Handler,                          /* BusFault */
    Fault_Handler,                          /* UsageFault */
    0,
    (void (*)(void))g_fw_image_info,        /* Vector 8: the image info, as in the production image */
    0, 0,
    Fault_Handler,                          /* SVC */
    Fault_Handler,                          /* DebugMon */
    0,
    Fault_Handler,                          /* PendSV */
    SysTick_Handler,
    /* No peripheral interrupts are enabled */
};

void Reset_Handler(void)
{
    __disable_irq();
    SCB->VTOR = 0x18000;
    uint32_t *src = &__data_load;
    for (uint32_t *dst = &__data_start; dst < &__data_end;) {
        *dst++ = *src++;
    }
    for (uint32_t *dst = &__bss_start; dst < &__bss_end;) {
        *dst++ = 0;
    }
    /* The old bootloader left PLL0 connected; the user manual forbids reconfiguring it like that.
     * Disconnect, then disable, each with its feed, then set the clocks up afresh. */
    if (LPC_SC->PLL0STAT & (1UL << 25)) {
        LPC_SC->PLL0CON = 0x01;
        LPC_SC->PLL0FEED = 0xAA;
        LPC_SC->PLL0FEED = 0x55;
    }
    LPC_SC->PLL0CON = 0x00;
    LPC_SC->PLL0FEED = 0xAA;
    LPC_SC->PLL0FEED = 0x55;
    SystemInit();
    SystemCoreClockUpdate();
    __enable_irq();
    main();
    for (;;) {
    }
}

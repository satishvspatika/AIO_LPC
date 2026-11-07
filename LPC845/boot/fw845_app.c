#include "fw845_app.h"
#include "fw845_recv.h"
#include "fw_layout.h"
#include "LPC845.h"

/* Straight after the vector table. scripts/fw_image.py fills in length and crc32 after linking, and
 * points reserved vector 8 here. */
__attribute__((used, section(".image_info")))
const fw_image_info_t g_fw845_image_info = {
    .magic = FW_IMAGE_MAGIC,
    .board = FW_BOARD_ID,
    .length = 0,
    .crc32 = 0,
    .profile = 0,
    .version = FW845_VERSION
};

#define FW845_WDT_RUN_TICKS 132300UL   /* 60 s at the measured 2205 ticks/s */

void fw845_app_init(void)
{
    /* Read from flash: the build stamps this after compiling, so the compiler's copy is wrong */
    fw845_recv_init(g_fw845_image_info.version, *(const volatile uint32_t *)&g_fw845_image_info.crc32);
}

void fw845_app_feed_watchdog(void)
{
    if (!fw845_on_trial()) {
        __disable_irq();
        /* The bootloader's long timeout is for a firmware trial. Once running normally, a hung main
         * loop restarts in about a minute, as the rain count stops being served until then. */
        WWDT->TC = FW845_WDT_RUN_TICKS;
        WWDT->FEED = 0xAA;
        WWDT->FEED = 0x55;
        __enable_irq();
    }
}

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

void fw845_app_init(void)
{
    /* Read from flash: the build stamps this after compiling, so the compiler's copy is wrong */
    fw845_recv_init(g_fw845_image_info.version, *(const volatile uint32_t *)&g_fw845_image_info.crc32);
}

void fw845_app_feed_watchdog(void)
{
    if (!fw845_on_trial()) {
        __disable_irq();
        WWDT->FEED = 0xAA;
        WWDT->FEED = 0x55;
        __enable_irq();
    }
}

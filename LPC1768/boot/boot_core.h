#ifndef BOOT_CORE_H
#define BOOT_CORE_H

#include "fw_layout.h"

typedef enum {
    FW_BOOT_RUN = 0,    /* Slot A is good: start it */
    FW_BOOT_RUN_TRIAL,  /* Slot A is on trial: start it with the watchdog running */
    FW_BOOT_RETRY,      /* A flash operation failed: restart and carry on from the journal */
    FW_BOOT_NO_IMAGE    /* Nothing valid anywhere */
} fw_boot_result_t;

#define FW_TRIAL_MAX_BOOTS 3

fw_boot_result_t fw_boot_decide(void);

/* Is there a complete, undamaged image at this slot? */
bool fw_image_valid(uint32_t slot_base);

#endif /* BOOT_CORE_H */

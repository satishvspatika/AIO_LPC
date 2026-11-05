/* The update journal: a ring of records in flash, where the newest valid one is the state.
 * Nothing ever erases the current state: only a sector the ring is moving into gets erased. A
 * record torn by a power cut fails its CRC, so the state is still the one before it. */
#ifndef FW_JOURNAL_H
#define FW_JOURNAL_H

#include "fw_layout.h"

typedef enum {
    FW_J_NONE = 0,      /* No record yet */
    FW_J_PENDING,       /* Application: a new image is staged in slot B */
    FW_J_REJECTED,      /* Bootloader: the staged image failed its check, slot A unchanged */
    FW_J_SWAP_START,    /* op says why. From here the swap must be finished */
    FW_J_SWAP_STEP,     /* arg = step just finished */
    FW_J_SWAPPED,       /* New image in slot A, on trial */
    FW_J_TRIAL_BOOT,    /* arg = attempts so far */
    FW_J_CONFIRMED,     /* Application: the new image proved itself */
    FW_J_ROLLED_BACK,   /* Failed its trial, old image back in slot A */
    FW_J_RECOVERED,     /* Slot A was damaged, slot B's image now in slot A */
    FW_J_REVERT_REQ     /* Application: operator asked for the previous firmware */
} fw_j_type_t;

typedef enum {
    FW_OP_NONE = 0,
    FW_OP_INSTALL,
    FW_OP_ROLLBACK,
    FW_OP_RECOVER
} fw_op_t;

typedef struct {
    uint32_t magic;
    uint32_t seq;
    uint8_t type;
    uint8_t op;
    uint8_t arg;
    uint8_t reserved;
    uint32_t crc;
} fw_record_t;

/* The newest valid record. False if there isn't one (rec is then FW_J_NONE). */
bool fw_journal_read(fw_record_t *rec);
bool fw_journal_append(fw_j_type_t type, fw_op_t op, uint8_t arg);

#endif /* FW_JOURNAL_H */

/* Firmware over the air: brings the firmware file to the SD card, then the SD-card update installs it.
 * See ota.c. */
#ifndef OTA_H
#define OTA_H

/* After a slot whose record was delivered, with the modem still held and on the network. slot is the
 * slot of the day (0 = 00:00). Usually returns at once: it acts on the unit's daily slot, while a
 * download is unfinished, after an install, or on every slot if the card holds OTA.NOW. Asks the
 * server and downloads; never restarts the unit. */
void ota_slot_step(int slot);

/* Once the slot is recorded as done (SLOTS.CSV, LASTSLOT.DAT) and before the unit may sleep: installs
 * a file that ota_slot_step found whole. Does not return if it installs (the unit restarts); a restart
 * any earlier would have the slot taken for unfinished and sent again. */
void ota_install_step(void);

/* After a slot whose record was not delivered: if it was the unit's check slot, the check (and its
 * health report) is asked at the next delivered slot that day instead */
void ota_slot_not_sent(int slot, const char *time14);

/* v6.9.64: at the start of every slot: new firmware that delivers no record for 2 h goes back */
void ota_slot_begin(void);

/* v6.9.66: SEND STATUS on the keypad also reaches the office page (the unit's check with k=1 and what it
 * sees now). The modem must be on, registered and held by the caller. net: 2 or 4 (G); carrier: the
 * operator's name. Returns true when the office server answered. Never starts a download. */
#include "intermcu_protocol.h"
#include <stdbool.h>
bool ota_keypad_status(const telemetry_data_t *telem, int csq_dbm, int net, const char *carrier);

/* 1 the office answered the last SEND STATUS, 0 it did not, -1 not asked; reading it resets it to -1 */
int ota_keypad_result(void);

#endif /* OTA_H */

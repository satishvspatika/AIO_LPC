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

#endif /* OTA_H */

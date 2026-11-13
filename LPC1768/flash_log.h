#ifndef FLASH_LOG_H
#define FLASH_LOG_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

/* Circular log of 15-minute records in the LPC1768's own flash (sectors 25-29, 160 KB: 640
 * records, at least 512 after a sector is erased), so records survive a missing or failed SD card. Each page is written once and the
 * oldest sector is erased only when the log wraps, about every 8 days per sector. */

#define FLASH_LOG_RECORD_SIZE   72
#define FLASH_LOG_PAYLOAD_SIZE  168

typedef struct __attribute__((packed)) {
    uint32_t magic;
    uint32_t seq;       /* Increases with every page written */
    int32_t slot_id;    /* Record: its slot. Pending/sent marker: seq of the record it refers to */
    uint8_t type;
    uint8_t reserved;
    uint16_t crc;
    char record[FLASH_LOG_RECORD_SIZE];   /* SD card record text */
    char payload[FLASH_LOG_PAYLOAD_SIZE]; /* HTTP payload, for sending again */
} flash_log_page_t;

/* Finds where the log left off. Call once at boot. */
void flash_log_init(void);

/* Returns the record's seq, or 0 if it could not be written */
uint32_t flash_log_append(int32_t slot_id, const char *record, const char *payload);

/* Oldest record newer than after_seq, or NULL if there are none */
const flash_log_page_t *flash_log_next_record(uint32_t after_seq);
const flash_log_page_t *flash_log_newest_record(void);
/* The newest record with a payload: records kept while sending was stopped have none */
const flash_log_page_t *flash_log_newest_with_payload(void);
uint32_t flash_log_newest_seq(void);
int32_t flash_log_newest_slot(void);
uint32_t flash_log_record_count(void);

/* Records to send again when the SD card can't hold them in its backlog */
bool flash_log_mark_pending(uint32_t record_seq);
bool flash_log_mark_sent(uint32_t record_seq);
bool flash_log_next_pending(uint32_t *out_seq, char *out_payload, size_t size);
uint32_t flash_log_pending_count(void);

/* Whether the server got this slot's record. Survives power off, with no SD card. */
void flash_log_note_delivery(uint32_t record_seq, bool delivered);
/* Newest record the server got: what LAST LOGGED and the status SMS report. NULL if none. */
const flash_log_page_t *flash_log_last_delivered(void);

/* An outage ended and nothing has yet recorded that its records are all resent */
bool flash_log_outage_unsettled(void);
/* Everything from past outages has been delivered */
void flash_log_note_drained(void);
/* The card holding the queue has failed: queue those outages' records here. Returns how many. */
uint32_t flash_log_adopt_undelivered(void);
/* The server refused a queued record. True once it has had its chances and is set aside. */
bool flash_log_note_rejected(uint32_t record_seq, int strikes_allowed);
/* How many of the newest records, up to limit, were made with no network */
int flash_log_no_network_run(int limit);

/* Erase Data: wipes the whole log */
void flash_log_erase_all(void);

#endif /* FLASH_LOG_H */

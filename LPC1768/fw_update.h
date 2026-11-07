/* Firmware file handling: checks fw.hex completely before anything is erased, then copies it to
 * the staging slot and proves the copy. The bootloader does the rest. Also the base for OTA, which
 * only needs a different source for the file. */
#ifndef FW_UPDATE_H
#define FW_UPDATE_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

typedef enum {
    FW_OK = 0,
    FW_E_NOT_FOUND,
    FW_E_READ,          /* The SD card stopped answering */
    FW_E_BAD_LINE,      /* Bad format or line checksum: a damaged file */
    FW_E_BAD_ADDRESS,   /* Data outside the firmware slot: a full flash image, or another chip's */
    FW_E_GAP,           /* Lines missing or out of order */
    FW_E_NO_END,        /* The file stops early */
    FW_E_NOT_AN_IMAGE,  /* No valid image block or start address */
    FW_E_WRONG_BOARD,
    FW_E_WRONG_PROFILE, /* For another station type */
    FW_E_BAD_CRC,
    FW_E_FLASH,         /* Staging could not be written or read back */
    FW_E_JOURNAL,
    FW_E_LINK           /* The LPC845 stopped answering */
} fw_result_t;

typedef struct {
    uint32_t length;
    uint32_t crc32;
    uint32_t profile;
    char version[17];
} fw_file_info_t;

typedef void (*fw_progress_fn)(const char *stage, unsigned percent);

/* Reads the whole file and writes nothing. FW_E_WRONG_PROFILE is a good file for another station
 * type: info is filled in, and installing it is the operator's decision. */
fw_result_t fw_update_check(const char *name, uint32_t expected_profile, fw_file_info_t *info, fw_progress_fn progress);

/* Check first. Erases staging, copies the file, proves it, and only then marks it ready to install. */
fw_result_t fw_update_stage(const char *name, const fw_file_info_t *checked, fw_progress_fn progress);

/* Is there a complete image in staging to go back to? */
bool fw_update_slot_b_info(fw_file_info_t *info);

const char *fw_update_error_text(fw_result_t r);

/* The same checks for sensor.hex, which is for the LPC845's flash map instead */
fw_result_t fw_ctrl_check(const char *name, fw_file_info_t *info, fw_progress_fn progress);

/* Reads the file again, handing the image over in order, in pieces of chunk_bytes (at most 256) */
typedef bool (*fw_chunk_fn)(uint32_t offset, const uint8_t *data, uint32_t len);
fw_result_t fw_ctrl_stream(const char *name, const fw_file_info_t *checked, fw_chunk_fn sink, uint32_t chunk_bytes, fw_progress_fn progress);

/* File access, supplied by the application or the tests. Read returns bytes read, or -1. */
bool fw_file_open(const char *name, uint32_t *out_size);
int fw_file_read(void *buf, uint32_t len);
void fw_file_close(void);

#endif /* FW_UPDATE_H */

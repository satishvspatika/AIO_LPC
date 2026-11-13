#ifndef SMS_CMD_H
#define SMS_CMD_H
/* SMS commands (v6.9.41). Pure text handling, no modem or flash: host-tested (tests/sms_cmd_test.c).
 *
 *   VARSHA <CMD> [value]      as the ESP32 fleet (VARSHA GET_STATUS, VARSHA SET_STN_ID 001903)
 *   VARSHA <CMD>=<value>
 *   #<CMD>[=value]            the LPC units' older form (#STATUS, #GPS, #TIME=20261113104500)
 *
 * GET_ commands are answered to any sender. SET_ and number-list commands only from office numbers. */
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef enum {
    SMS_CMD_NONE = 0,       /* No command in the message: ignored */
    SMS_CMD_UNKNOWN,        /* Keyword or # present, command not known: answered with the list */
    SMS_CMD_GET_STATUS,
    SMS_CMD_GET_GPS,
    SMS_CMD_GET_CONFIG,
    SMS_CMD_GET_NUM,
    SMS_CMD_SET_TIME,
    SMS_CMD_SET_STN_ID,
    SMS_CMD_SET_SERVER_2,
    SMS_CMD_SET_SERVER_MODE,
    SMS_CMD_ADD_NUM,
    SMS_CMD_DEL_NUM,
    SMS_CMD_STOP,           /* Stop all HTTP sends (junk values): records kept on the card */
    SMS_CMD_RESUME,
    SMS_CMD_ERASE_DATA      /* ERASE_DATA <station ID>: the ID guards against a message for another unit */
} sms_cmd_t;

/* Finds the command in a message body. arg gets the value (trimmed, may be empty). */
sms_cmd_t sms_cmd_parse(const char *body, char *arg, size_t arg_size);

/* Commands that change something: only from office numbers */
bool sms_cmd_needs_office(sms_cmd_t cmd);

/* The last 10 digits of a phone number ("+919980945474" -> "9980945474"). False if fewer than 10. */
bool sms_number_last10(const char *number, char out[11]);

/* Both numbers have the same last 10 digits */
bool sms_number_same(const char *a, const char *b);

/* "20261113104500" (also with - : space T separators): out gets 14 digits. Range checked here; the
 * caller still checks it is a believable time. */
bool sms_parse_time(const char *arg, char out[15]);

/* Station ID of 1 to digits digits, not all zeros; out gets it zero-padded to digits */
bool sms_parse_station_id(const char *arg, int digits, char *out, size_t out_size);

/* "host=rtdas.x.net;port=80;path=/a/b;key=abc" (any order, port 1-65535, host and path needed).
 * "OFF" or "NONE": clears server 2 (host empty). */
bool sms_parse_server2(const char *arg, char *host, size_t host_size, uint16_t *port,
                       char *path, size_t path_size, char *key, size_t key_size, bool *off);

/* A payload with its key replaced: "&key=..." (form) or "\"ApiKey\":\"...\"" (JSON). False (out
 * unchanged) if the payload has no key or out is too small. */
bool sms_payload_with_key(const char *payload, const char *key, char *out, size_t out_size);

/* Minutes from an SMS centre time stamp ("26/11/13,12:16:17+22", local time as sent by the network)
 * to the unit's time (YYYYMMDDhhmmss). False if either can't be read. Negative: the SMS is "newer". */
bool sms_age_minutes(const char *sms_ts, const char *now14, long *out_minutes);

/* A body sent as UCS2 shows as hex in text mode ("0056004100520053...": VARS...). Decoded in place to
 * plain text when every character is plain ASCII; anything else is left as it is. True if decoded. */
bool sms_decode_ucs2(char *body);

#endif /* SMS_CMD_H */

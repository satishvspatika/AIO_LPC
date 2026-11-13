#ifndef SIM7600_DRIVER_H
#define SIM7600_DRIVER_H

#include <stdbool.h>
#include <stdint.h>
#include "telemetry_formatter.h"

typedef struct {
    int csq_val;
    int csq_dbm;
    char ip_addr[20];
    char iccid[24];
    char carrier[16];
    bool is_registered;
    bool is_pdp_active;
} modem_status_t;

void sim7600_init(void);
bool sim7600_power_on(void);
/* After a slot: lets the radio mode learning (net_policy.c) know whether the mode it used delivered */
void sim7600_note_slot_delivery(bool delivered, bool network_fault);

/* The last power-on found no usable SIM */
bool sim7600_sim_missing(void);
/* The modem answered AT since it was last powered on (GPS works without a network) */
bool sim7600_modem_answered(void);
/* Server time sets the clock (default on). Off for server 2's copy in mode 2. */
void sim7600_set_server_clock_sync(bool on);

/* The last post went out with no answer: don't repeat it until the next slot */
bool sim7600_post_unanswered(void);

/* The server answered the last post and said no */
bool sim7600_post_rejected(void);

/* One send by one route: 0 the server's name, 1 its IP, 2 the modem's HTTP commands */
bool sim7600_http_post_once(const http_endpoint_t *endpoint, const char *payload, int route);
const char *sim7600_route_name(int route);

/* Asks the modem whether it's still on the network */
bool sim7600_still_registered(void);
void sim7600_power_off(void);

/* Modem concurrency lock */
bool sim7600_acquire(uint32_t timeout_ms);
void sim7600_release(void);
bool sim7600_is_busy(void);

/* Background supervisor step: boots modem on startup, polls CSQ/carrier/time/SMS */
void sim7600_worker_step(void);

/* Transmit HTTP Telemetry via Layer-4 TCP Socket */
bool sim7600_http_post(const http_endpoint_t *endpoint, const char *payload, char *date_hdr_out, size_t date_hdr_size);
bool sim7600_net_diagnostics(const http_endpoint_t *endpoint, const char *payload); /* TEMPORARY: TEST SEND only */

/* Query live modem status */
typedef enum {
    GPS_NO_FIX = 0,     /* Searched for the full time: no sky view or no antenna */
    GPS_NOT_AVAILABLE,  /* The modem refused the GPS commands */
    GPS_NEW_POSITION,   /* First position, saved */
    GPS_MOVED,          /* Moved beyond the limit, saved */
    GPS_SAME_POSITION   /* Within the limit, nothing written */
} gps_result_t;

/* One GPS try: the modem must be on and held by the caller. Always turns GPS off again. */
gps_result_t sim7600_gps_update(const char *today);

void sim7600_get_status(modem_status_t *out_status);

/* Sub-step progress hook & cancellation for UI responsiveness */
typedef void (*sim7600_progress_cb_t)(const char *step_text);
void sim7600_set_progress_callback(sim7600_progress_cb_t cb);
void sim7600_request_cancel(void);
void sim7600_clear_cancel(void);
bool sim7600_is_cancel_requested(void);

/* SMS Engine */
void sanitize_phone_number(char *dest, const char *src, size_t dest_size);
bool sim7600_send_sms(const char *dest_phone, const char *message);
/* GPS SMS text in the ESP32 format (STAT_AD-F from the keypad, STAT_AD-C by SMS), saved position.
 * The modem must be on; send it with sim7600_send_sms(). Static buffer: one caller at a time. */
const char *sim7600_build_gps_sms(char kind, const char *present_time);
bool sim7600_transmit_status_sms(const telemetry_data_t *telem, int csq_dbm);
void sim7600_check_incoming_sms(const telemetry_data_t *telemetry, bool in_slot);

#endif /* SIM7600_DRIVER_H */


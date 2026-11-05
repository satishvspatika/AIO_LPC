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
bool sim7600_transmit_status_sms(const telemetry_data_t *telem, int csq_dbm);
void sim7600_check_incoming_sms(const telemetry_data_t *telemetry);

#endif /* SIM7600_DRIVER_H */


#ifndef USER_CONFIG_H
#define USER_CONFIG_H

#include <stdint.h>
#include <stdbool.h>

/* =========================================================================
 *  UNIFIED AIO TELEMETRY STATION USER CONFIGURATION
 *  Platform: NXP LPC1768 Master + NXP LPC845 Sentry + SIMCom SIM7600 4G LTE
 * ========================================================================= */

/* System Types */
#define SYSTEM_TRG      0   /* Telemetric Rain Gauge (Rainfall only) */
#define SYSTEM_TWS      1   /* Telemetric Weather Station (Full AWS) */
#define SYSTEM_TWSRF    2   /* Telemetric Weather Station + Rain Addon */
#define SYSTEM_TWSRP    3   /* Telemetric Weather Station + Rain + Pressure */

/* Station Configuration Profiles */
#define CFG_KSNDMC_TRG  0   /* Karnataka TRG (URL-encoded slot cumulative, port 80) */
#define CFG_BIHAR_TRG   1   /* Bihar Govt RTDAS (JSON incremental + cumulative, port 8085) */
#define CFG_KSNDMC_TWS  2   /* Karnataka AWS (URL-encoded full AWS suite, port 80) */

/* Active Configuration Selection */
#ifndef ACTIVE_SYSTEM
#define ACTIVE_SYSTEM   SYSTEM_TWS
#endif

#ifndef ACTIVE_CONFIG
#define ACTIVE_CONFIG   CFG_KSNDMC_TWS
#endif

/* Firmware Version Identifier */
#define FIRMWARE_VERSION    "AIO_6.0.8"
#define LCD_VERSION_STRING  "v6.0.8"
#define STATION_ID_DEFAULT  "001922"

/* Status SMS Destination & Network Identifiers */
#if (ACTIVE_CONFIG == CFG_BIHAR_TRG)
#define STATUS_SMS_DEST_NUMBER    "9741391102"
#define STATUS_SMS_NETWORK_NAME   "DES-BIH"
#else
#define STATUS_SMS_DEST_NUMBER    "9980945474"
#define STATUS_SMS_NETWORK_NAME   "KSNDMC"
#endif

/* Debug Dump Engine Settings */
#define SDC_DEBUG_DUMP_DEFAULT  1       /* 1 = verbose debug dump enabled by default */
#define DEBUG_LOG_FILENAME      "/debuglog.txt"
#define HTTP_LOG_FILENAME       "/httplog.txt"
#define DEBUG_TRIGGER_FILENAME  "/DEBUG.TXT"

/* Timing & Safety Watchdog Limits (in milliseconds) */
#define WDT_PERIODIC_LIMIT_MS   (11 * 60 * 1000)  /* 11-min hard power cutoff */
#define WDT_ACTIVITY_LIMIT_MS   (5 * 60 * 1000)   /* 5-min inactivity hold */
#define UI_INACTIVITY_SLEEP_MS  (60 * 1000)       /* 60s screen blanking */
#define SLOT_HOLD_GUARD_SEC     62                /* 62s minimum slot boundary guard */

/* Rainfall Parameters */
#define RF_BUCKET_RESOLUTION   0.25f   /* mm per bucket tip (0.25 mm standard) */

#endif /* USER_CONFIG_H */


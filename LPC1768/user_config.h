#ifndef USER_CONFIG_H
#define USER_CONFIG_H

#include <stdint.h>
#include <stdbool.h>

/* =========================================================================
 *  UNIFIED AIO TELEMETRY STATION USER CONFIGURATION
 *  Platform: NXP LPC1768 Master + NXP LPC845 Sentry + SIMCom SIM7600 4G LTE
 * ========================================================================= */

/* Station Unit Profiles */
#define KSNDMC_TRG      0   /* Karnataka TRG (Rainfall only, cumulative mm, port 80) */
#define BIHAR_TRG       1   /* Bihar Govt RTDAS (Rainfall only, incremental + cumulative mm, port 8085) */
#define KSNDMC_TWS      2   /* Karnataka AWS (Full Weather Station suite, port 80) */

/* Active Station Selection: Choose one of KSNDMC_TRG, BIHAR_TRG, KSNDMC_TWS */
#ifndef UNIT_CFG
#define UNIT_CFG        KSNDMC_TWS
#endif

/* Firmware Version Identifier (visible on LCD line 2 & Status SMS) */
#define FIRMWARE_VERSION    "v6.2.9"
#define STATION_ID_DEFAULT  "001061"

/* Status SMS Destination & Network Identifiers */
#if (UNIT_CFG == BIHAR_TRG)
#define STATUS_SMS_DEST_NUMBER    "9741391102"
#define STATUS_SMS_NETWORK_NAME   "DES-BIH"
#else
#define STATUS_SMS_DEST_NUMBER    "9980945474"
#define STATUS_SMS_NETWORK_NAME   "KSNDMC"
#endif

/* Debug Dump Engine Settings */
#define SDC_DEBUG_DUMP_DEFAULT  1       /* 1 = verbose debug dump enabled by default */
#define DEBUG_LOG_FILENAME      "DEBUG.TXT"
#define HTTP_LOG_FILENAME       "HTTPLOG.TXT"
#define SLOT_LOG_FILENAME       "SLOTS.CSV"
#define LAST_LOGGED_FILENAME    "LASTLOG.DAT"
#define RAT_MODE_FILENAME       "RATMODE.DAT"
#define LAST_SLOT_FILENAME      "LASTSLOT.DAT"
#define FLASH_SYNC_FILENAME     "FLASHSYN.DAT"
#define SD_CHECK_FILENAME       "SDCHECK.DAT"
#define DEBUG_TRIGGER_FILENAME  "DEBUG.TXT"

/* Timing & Safety Watchdog Limits (in milliseconds) */
#define WDT_PERIODIC_LIMIT_MS   (11 * 60 * 1000)  /* 11-min hard power cutoff */
#define WDT_ACTIVITY_LIMIT_MS   (5 * 60 * 1000)   /* 5-min inactivity hold */
#define UI_INACTIVITY_SLEEP_MS  (60 * 1000)       /* 60s screen blanking */
#define SLOT_HOLD_GUARD_SEC     62                /* 62s minimum slot boundary guard */

/* Rainfall Parameters */
#define RF_BUCKET_RESOLUTION   0.50f   /* mm per bucket tip */

/* Sensor Scaling (same as the ESP32): the LPC845 sends raw counts */
#define WS_CALIBRATION_FACTOR  0.45f   /* Metres per anemometer revolution */
#define WIND_TEETH_COUNT       2.0f    /* Pulses per revolution */
#define WIND_DIR_ADC_MIN       140
#define WIND_DIR_ADC_MAX       3950

#endif /* USER_CONFIG_H */


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
#define UNIT_CFG        BIHAR_TRG
#endif

/* Firmware Version Identifier (visible on LCD line 2 & Status SMS) */
#define FIRMWARE_VERSION "v6.9.42"

/* THE production switch. 1 = field build:
 *   - MENU > KEY TEST hidden (and TEST SEND can't be switched on)
 *   - debug log on the card off, unless the card holds DEBUGLOG.ON (see SD_DEBUG_LOG_DEFAULT)
 *   - no NXP driver parameter checks: build.sh reads this line and leaves out -DDEBUG
 *     (with it, a bad driver parameter stops the unit in check_failed() until the watchdog)
 * 0 = bench build: KEY TEST in the menu, debug log always on, driver parameter checks on.
 * Keep the "#define PRODUCTION_BUILD" line as it is: build.sh greps it. */
#define PRODUCTION_BUILD    1

/* TESTING: debug log on by default in a production build, on every unit and card, until testing
 * is signed off (user, 2026-11-12). Set to 0 for the production release: the log then needs
 * DEBUGLOG.ON on the card. build.sh greps this line too and says which way it is. */
#define TESTING_DEBUG_LOG   1

/* TEMPORARY test-station tool: MENU > TEST SEND posts dummy records. */
#define ENABLE_TEST_SEND    0
#if PRODUCTION_BUILD && ENABLE_TEST_SEND
#error "TEST SEND is a bench tool: not in a production build"
#endif
/* MENU > KEY TEST (shows each key's code). Bench builds with TEST_FAULTS need it: the fault
 * tests run from that screen. */
#if !PRODUCTION_BUILD || defined(TEST_FAULTS)
#define ENABLE_KEY_TEST     1
#else
#define ENABLE_KEY_TEST     0
#endif
#if (UNIT_CFG == KSNDMC_TRG)
#define TEST_STATION_ID     "1001"
#elif (UNIT_CFG == BIHAR_TRG)
#define TEST_STATION_ID     "1003"
#else
#define TEST_STATION_ID     "1002"
#endif
#define STATION_ID_DEFAULT  "001061"
/* Station ID length on the server, the LCD and in file names: Bihar uses 8 digits, KSNDMC 6.
 * Shorter stored IDs are zero-padded to this; flash holds up to STATION_ID_MAX digits. */
#if (UNIT_CFG == BIHAR_TRG)
#define STATION_ID_DIGITS   8
#else
#define STATION_ID_DIGITS   6
#endif
#define STATION_ID_MAX      8

/* Status SMS Destination & Network Identifiers */
#if (UNIT_CFG == BIHAR_TRG)
/* TEMPORARY for the SMS test: the ESP32's Bihar number is 9741391102. This one last worked from this SIM. */
#define STATUS_SMS_DEST_NUMBER    "9980945474"
#define STATUS_SMS_NETWORK_NAME   "DES-BIH"
#else
#define STATUS_SMS_DEST_NUMBER    "9980945474"
#define STATUS_SMS_NETWORK_NAME   "KSNDMC"
#endif

/* SMS commands (v6.9.41): VARSHA <CMD> [value], or the older #CMD=value. GET_ commands are answered to
 * any sender; SET_ commands only from these office numbers (last 10 digits). Up to 5, until changed by
 * SMS (ADD_NUM / DEL_NUM from a listed number): the list then lives in flash page 0x1100. */
/* DMC Airtel, DMC BSNL, Spatika Airtel, Satish Jio (user, 2026-11-13). No +91: matched on the last 10 digits */
#define SMS_OFFICE_NUMBERS        "9980945474", "9449020611", "9741391102", "9108052058"

/* Debug log on the card (LOG/YYMMDD.TXT, HTTPLOG.TXT): about 1 MB a day of card writes.
 * Production: off unless the card holds DEBUGLOG.ON at its root (checked at every boot, so every slot):
 * a unit in the field can be switched on and off with the card alone. Errors are kept either way
 * (flash error log, copied to ERRORS.TXT), and SLOTS.CSV, DATA/, UPDATE.TXT are always written. */
#define SD_DEBUG_LOG_DEFAULT    (!PRODUCTION_BUILD || TESTING_DEBUG_LOG)
#define DEBUG_LOG_ON_FILENAME   "DEBUGLOG.ON"
#define DEBUG_LOG_FILENAME      "DEBUG.TXT"
#define HTTP_LOG_FILENAME       "HTTPLOG.TXT"
#define SLOT_LOG_FILENAME       "SLOTS.CSV"
#define LAST_LOGGED_FILENAME    "LASTLOG.DAT"
#define REJECTED_FILENAME       "REJECTED.TXT"
#define ERRORS_FILENAME         "ERRORS.TXT"    /* Copy of the flash error log, at most 64 KB */
#define ERRORS_OLD_FILENAME     "ERRORS.OLD"    /* The previous 64 KB */
#define ERRORS_SYNC_FILENAME    "ERRSYNC.DAT"
/* Automatic GPS searches in slots (01:00, 07:00, 13:00, 19:00 with no position). Off from v6.9.20: the GPS
 * has never got a fix on these boards, and each search kept the modem on 90 s more (about 6 minutes a
 * day). Searching from the GPS menu still works. Set to 1 once the GPS gets fixes. */
#define GPS_AUTO_SEARCH         0

#define RAT_MODE_FILENAME       "RATMODE.DAT"
#define LAST_SLOT_FILENAME      "LASTSLOT.DAT"
#define FLASH_SYNC_FILENAME     "FLASHSYN.DAT"
#define SD_CHECK_FILENAME       "SDCHECK.DAT"
#define POWER_OFF_MARK_FILENAME "PWROFF.DAT"  /* Written last before power-off: see storage_power_off_mark() */
#define FW_FILE_KSNDMC_TRG      "fw_ksn_trg.hex"
#define FW_FILE_BIHAR_TRG       "fw_bihar.hex"
#define FW_FILE_KSNDMC_TWS      "fw_ksn_tws.hex"
#define FW_CTRL_FILENAME        "ctrl.hex"
#define FW_HISTORY_FILENAME     "UPDATE.TXT"
#define DEBUG_TRIGGER_FILENAME  "DEBUG.TXT"

/* Timing & Safety Watchdog Limits (in milliseconds) */
#define WDT_PERIODIC_LIMIT_MS   (11 * 60 * 1000)  /* 11-min hard power cutoff */
#define WDT_ACTIVITY_LIMIT_MS   (5 * 60 * 1000)   /* 5-min inactivity hold */
#define UI_INACTIVITY_SLEEP_MS  (60 * 1000)       /* 60s screen blanking */
#define SLOT_HOLD_GUARD_SEC     62                /* 62s minimum slot boundary guard */

/* GPS position (SIM7600 AT+CGPS). It only runs while the modem is already on for a slot, after
 * the record has gone out. */
#define GPS_MOVE_LIMIT_M        50      /* Save a new position only if it moved further than this */
#define GPS_FIX_TIMEOUT_SEC     90      /* Longest try: with no antenna, this is all it costs */
#define GPS_MIN_BATTERY_V       11.8f   /* Scheduled tries are skipped below this */
/* Wait after the last SD write before asking the CTRL board to cut power (v6.9.29) */
#define SD_SETTLE_BEFORE_POWER_OFF_MS 1000
#define BATTERY_SENSE_FAULT_V   2.0f    /* A reading below this is a broken sense line, not a battery */

/* Rainfall Parameters */
#define RF_BUCKET_RESOLUTION   0.50f   /* mm per bucket tip */

/* Sensor scaling: the LPC845 sends raw counts. As the LPC TWS5 firmware (TWS5_1_0_5, sensor.c) that
 * ran these sensors on this board: speed = pulses per second / 4, direction = ADC x 360 / 4095.
 * Up to v6.9.27 these were the ESP32's (0.45 m per revolution, 2 pulses, ADC 140-3950), which read
 * speed 10% low and direction offset. */
#define WS_CALIBRATION_FACTOR  0.5f    /* With 2 pulses per revolution: 0.25 m/s per pulse a second */
#define WIND_TEETH_COUNT       2.0f
#define WIND_DIR_ADC_MIN       0
#define WIND_DIR_ADC_MAX       4095

#endif /* USER_CONFIG_H */


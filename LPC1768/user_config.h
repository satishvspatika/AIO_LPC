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
#define FIRMWARE_VERSION "v6.9.55"

/* THE production switch. 1 = field build:
 *   - MENU > KEY TEST hidden (and TEST SEND can't be switched on)
 *   - debug log on the card off, unless the card holds DEBUGLOG.ON (see SD_DEBUG_LOG_DEFAULT)
 *   - no NXP driver parameter checks: build.sh reads this line and leaves out -DDEBUG
 *     (with it, a bad driver parameter stops the unit in check_failed() until the watchdog)
 * 0 = bench build: KEY TEST in the menu, debug log always on, driver parameter checks on.
 * The only build switch (v6.9.55: TESTING_DEBUG_LOG removed). Test units run the release build
 * with DEBUGLOG.ON on their cards, so what they test is exactly what ships.
 * Keep the "#define PRODUCTION_BUILD" line as it is: build.sh greps it. */
#define PRODUCTION_BUILD    1

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
/* DMC Airtel, DMC BSNL, Spatika Airtel, Satish Jio (user, 2026-11-13), Satish Airtel (user, 2026-11-14, v6.9.55).
 * No +91: matched on the last 10 digits. At most REMOTE_MAX_NUMBERS (5): the list is now full. */
#define SMS_OFFICE_NUMBERS        "9980945474", "9449020611", "9741391102", "9108052058", "9980052058"

/* SMS listening window (v6.9.44; v6.9.45 automatic radio mode first, as 4G-only gets no SMS on BSNL):
 * on listening slots the modem stays on until this long after it registered for SMS, checking every 5 s, so commands sent while it was off can arrive (the network delivers
 * them 10-30 s after registration). About 20 s more modem time per
 * listening slot (no check at all on other slots). v6.9.55: every slot listens again (user, 2026-11-14);
 * with SMS_LISTEN_ALL_SLOTS 0 only the SMS_LISTEN_SLOTS slots do (build 8: 4 a day, see below). */
/* v6.9.55: 10 s (was 25): on 001903, 14 Nov, every SMS that came was on the SIM at the first check
 * after registering (08:31, 08:46, 09:16, 09:31); the rest of the wait caught nothing (user, 2026-11-14) */
/* Build 14: 20 s. Only 4 slots a day listen now (it was all 96 when 10 s was chosen), so 10 s more
 * costs 40 s of modem time a day. After a fresh join the first SMS came up to 10 s later (14 Nov
 * 15:01 9 s, 16:16 10 s): 10 s left no margin, and one that comes just after power-off is a failed
 * delivery for its SMS centre, which then waits on its own retry timer. */
#define SMS_LISTEN_SEC            20
/* Each SMS taken keeps listening this much longer, to SMS_LISTEN_MAX_SEC in all (v6.9.47) */
/* Build 14: 60 s (was 8, before that 20). An SMS centre hands over the next waiting SMS 30-50 s after
 * the unit's reply to the one before (Airtel, 14 Nov 10:37-10:39 with the LCD on: 31 s and 46 s). With
 * 8 s the unit switched off 14 s after the last one (14 Nov 16:16, 15 Nov 11:01), the rest of the queue
 * met a unit that was off, and came a slot or a day later. Costs nothing on a slot with no SMS. */
#define SMS_LISTEN_MORE_SEC       60   /* Build 20: only for a command; any other SMS adds nothing */
#define SMS_LISTEN_MAX_SEC        240   /* Build 14: was 90. Inside the slot's 300 s; the window also ends with the slot's time */
/* A GET_ command already answered to the same sender this long ago or less isn't answered again (build 9) */
#define SMS_GET_REPEAT_MIN        10
/* No window with less of the slot's time left than this (the SIM is still read once). Setting up
 * the window (fresh registration) can take about 90 s at worst (review, v6.9.52) */
#define SMS_LISTEN_MIN_BUDGET_SEC 120
/* Build 16: longest wait for voice/SMS registration after the window's leave-and-join. An Airtel IoT
 * SIM was still searching after 20 s (1922, 16 Nov 16:02); BSNL takes about 4 s. */
#define SMS_REJOIN_WAIT_SEC       60
/* Build 18: the signal is asked again, 1 s apart, up to this many times while the modem answers 99
 * ("not known yet", just after it changes between 2G and 4G). Was 3. */
#define CSQ_RETRY_MAX             10
/* Build 17: 1 = the listening window leaves the network and joins again on an Airtel IoT SIM too
 * (as on every other SIM). 0 while the slow searches on 1922 after such a window are not explained. */
#define SMS_REJOIN_AIRTEL_IOT     0
/* v6.9.55: an SMS centre needs the modem's acknowledgement of each SMS it delivers. Replies wait
 * until none has arrived for SMS_QUIET_MS (at most SMS_QUIET_MAX_MS), and so does power-off */
#define SMS_QUIET_MS              4000
#define SMS_QUIET_MAX_MS          15000
/* Messages already answered: a repeat delivery from the network is not answered again (v6.9.55) */
#define SMS_SEEN_FILENAME         "SMSSEEN.DAT"
/* After the first 60 s with no +CMGS, the SMS send waits this much more before calling it unknown
 * (v6.9.55: BSNL on LTE confirmed after 102 s; an unknown send is never repeated) */
#define SMS_CMGS_MORE_MS          120000
/* A command sent longer ago than this is deleted unrun (old messages on field SIMs, v6.9.46) */
#define SMS_MAX_AGE_MIN           (48 * 60)   /* v6.9.50: 24 h, with 2 listening slots a day, could drop a command unrun */
/* SET_TIME older than this is refused: it would set an old time (v6.9.51) */
#define SMS_SET_TIME_MAX_AGE_MIN  15
/* v6.9.55 build 8 (user, 2026-11-14): 11:00, 11:15, 16:00, 16:15 only. Each listening slot leaves the
 * network and joins again, which makes Jio/Airtel deliver what they hold; the second slot of each pair
 * catches what the first didn't (Airtel hands its queue over in order). Commands wait at most ~19 h
 * (16:15 to 11:00). All 96 slots cost ~26 min/day of modem time; these 4 about 1 min. */
/* Build 18 (user, 2026-11-16): every slot listens again, so a command waits 15 min at most. Costs
 * about 20 s of modem time a slot (~30 min a day) when no SMS comes. */
/* Build 20 (user, 2026-11-17): back to 4 slots a day, now 11:00, 12:00, 16:00 and 17:00. 1922 overnight
 * on build 19 (BSNL, 38 slots): the window was 23-29 s of the 70 s each slot was on, and the automatic
 * mode it leaves the modem in was followed by "no network service" at the next slot's data in 29 of
 * them. A command waits 18 h at most (17:00 to 11:00). */
#define SMS_LISTEN_ALL_SLOTS      0
/* Build 19: with every slot listening, the leave-and-join only in one slot of this many (4 = the
 * hh:00 slot). The other slots listen without it. */
#define SMS_REJOIN_EVERY_SLOTS    4
#define SMS_LISTEN_SLOTS          1100, 1200, 1600, 1700

/* Debug log on the card (LOG/YYMMDD.TXT, HTTPLOG.TXT): about 1 MB a day of card writes.
 * Production: off unless the card holds DEBUGLOG.ON at its root (checked at every boot, so every slot):
 * a unit in the field can be switched on and off with the card alone. Errors are kept either way
 * (flash error log, copied to ERRORS.TXT), and SLOTS.CSV, DATA/, UPDATE.TXT are always written. */
#define SD_DEBUG_LOG_DEFAULT    (!PRODUCTION_BUILD)
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
/* Build 21: the APN that last delivered a record on this SIM (SIM number + APN), tried first from
 * then on. As the ESP32's apn_config.txt. */
#define APN_SAVED_FILENAME      "APN.DAT"
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
/* v6.9.55 build 12: how long after a slot boundary the CTRL (v1.15+) powers this board and the modem.
 * Set here, sent to the CTRL at every boot and checked against its status frames, so it is tuned
 * with a main firmware update only. The record is closed at the boundary, so no value sent depends on
 * it. The modem needs 16-18 s to answer and 20-27 s to register, and the server takes records from
 * +62 s: 40 s cuts the idle hold from 26 s to about 16 s. The CTRL accepts 10-55 s. */
#define SLOT_WAKE_DELAY_SEC     40

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
/* Highest believable 15-min average (user, 2026-11-13). Above it the record says 0.0 and ERRORS.TXT
 * keeps the value read: 001903 read 55.6 and 53.0 with a mains supply connected (pulse noise). */
#define WS_AVG_MAX_MPS         30.0f
#define WIND_DIR_ADC_MIN       0
#define WIND_DIR_ADC_MAX       4095

#endif /* USER_CONFIG_H */


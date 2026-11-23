# AIO Data Logger: Release v6.9.66

**Release date:** 23 November 2026

**Type:** Production build (the debug log is off unless the card holds `DEBUGLOG.ON`)

**For:** Test team, bench and field test units. Replaces v6.9.65. **Not yet run on a unit.**

| Part | Version | File | Folders |
|---|---|---|---|
| Main firmware, KSNDMC rain gauge (TRG) | v6.9.66 | `fw_ksn_trg.hex` | OTA, SD_CARD |
| Main firmware, Bihar rain gauge (TRG) | v6.9.66 | `fw_bihar.hex` | OTA, SD_CARD |
| Main firmware, KSNDMC weather station (TWS) | v6.9.66 | `fw_ksn_tws.hex` | OTA, SD_CARD |
| CTRL firmware (all unit types) | v1.17 (same as v6.9.63) | `ctrl.hex` | OTA, SD_CARD |
| Old-unit conversion image | MIG-1.10 (same as v6.9.63) | `fw.hex` | SD_CARD only |
| Factory images (bootloader + firmware, MCU-Link) | v6.9.66 / CTRL v1.17 | `MAIN_<type>_v6.9.66_FULL.hex`, `CTRL_v1.17_FULL.hex` | FACTORY_MCU-LINK |

## Folders

| Folder | Used for |
|---|---|
| `OTA/` | Upload to the office page. The three main files plus `ctrl.hex`. |
| `SD_CARD/` | Copy to the root of a unit's card for a keypad update, or with `fw.hex` to convert an old unit. |
| `FACTORY_MCU-LINK/` | A blank or bricked board, flashed with MCU-Link. |

---

## 1. What is new since v6.9.65

### SEND STATUS also reaches the office page
- **SEND STATUS on the keypad now also tells the office page**, right after the status SMS: firmware version,
  battery, signal, network (2G/4G), operator, records waiting and SD card. The station page shows it as
  "Last SEND STATUS on the unit", with an event in the station's history.
- **The screen says whether each part got through:** `SMS+OFFICE OK`, `SMS OK,OFFICE NO`, `SMS NO,OFFICE OK` or
  `SMS+OFFICE FAIL` (shown for 5 s). A technician on site knows at once whether the unit reaches the office
  server, instead of waiting for its next twice-a-day check.
- It is kept apart from the daily health report (it does not replace that card) and never starts a firmware
  download: downloads still happen only in the unit's own slots.
- A first SEND STATUS from a unit the office has not seen before adds the station to the page.

### More flash room
- The firmware is now built with link-time optimisation: the same code, about 4.7 KB smaller. Free space in the
  128 KB firmware slot is now about 6.5 KB on every unit type (Bihar had 2.6 KB in v6.9.65), so later changes fit.
- Checked: every task's worst-case stack use is within 16 bytes of v6.9.65 (static analysis of the linked image),
  and all host tests pass.

## 2. Needs the office page update
The office page shows the new SEND STATUS report only after its update (AIO_LPC `ota_server/lpc_ota.py`,
`./deploy_now.sh` on the server). Before that, a SEND STATUS still reaches the office but only updates the
station's "Last check".

## 3. What to test
1. SEND STATUS on a unit with a working SIM: the SMS arrives, the screen shows `SMS+OFFICE OK`, and the station
   page shows "Last SEND STATUS on the unit" with the right battery and signal.
2. SEND STATUS with the antenna off: the screen shows the failure, the unit carries on normally.
3. A normal day: slots, backlog, SMS commands, the 12:30 check and health report, and an OTA update to and from
   v6.9.66 (the LTO build changes every function's code, so a full day's run on the bench is needed before field use).

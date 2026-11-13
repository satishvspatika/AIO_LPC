# AIO Data Logger: Release v6.9.54

**Release date:** 13 November 2026

**Type:** Production build, first cut for the test team's weekend run (the debug log is off unless the card holds `DEBUGLOG.ON`)

**For:** Test team, bench and field test units

| Part | Version | File on the SD card |
|---|---|---|
| Main firmware, KSNDMC rain gauge (TRG) | v6.9.54 | `fw_ksn_trg.hex` |
| Main firmware, Bihar rain gauge (TRG) | v6.9.54 | `fw_bihar.hex` |
| Main firmware, KSNDMC weather station (TWS) | v6.9.54 | `fw_ksn_tws.hex` |
| CTRL firmware (all unit types) | v1.14 | `ctrl.hex` |
| Old-unit conversion image | MIG-1.9 | `fw.hex` |

---

## 1. What is new since v6.9.38

### SMS commands (new)
- Commands are sent as `VARSHA <COMMAND> [value]` (the ESP32 fleet's format; the older `#CMD=value` also works).
- **Anyone:** `GET_STATUS`, `GET_GPS`, `GET_CONFIG`.
- **Office numbers only:** `SET_TIME`, `SET_STN_ID`, `SET_SERVER_2`, `SET_SERVER_MODE`, `ADD_NUM`, `DEL_NUM`,
  `GET_NUM`, `STOP`, `RESUME`, `ERASE_DATA <station ID>`.
- `STOP` stops all sending (for a unit sending bad values). Records are still kept on the card and in flash;
  nothing from the stopped time is ever sent. `RESUME` starts sending again from the next slot.
- `ERASE_DATA <station ID>` erases records, backlog and the rain total. The ID must be this unit's.
- **When commands are answered:**
  - With the LCD on (someone at the unit): within seconds.
  - Unattended: only at the **11:00 and 17:00 slots**, when the modem listens for about 25 s (longer if more
    messages are coming in). All other slots do not check for SMS, so they use no extra modem time.
  - A command older than 48 hours is deleted without being run. `SET_TIME` older than 15 minutes (by the network's time) is refused.
- **Known limit:** an SMS sent to an unattended unit from another operator (seen from Jio to BSNL) may not
  reach the unit in its listening window, because the sender's network retries on its own schedule. From the
  same operator as the unit's SIM (BSNL to BSNL) it worked.

### Weather station (TWS): wind speed with the cups still is now 0.0
- **CTRL v1.14 counts wind pulses only when the input holds its new level for about 2 ms.** Up to v1.13 every
  rising edge was counted, however short. On bench unit 1903 with a mains supply connected and the cups still,
  electrical spikes were counted as about 200,000 pulses a slot and read as 53 to 85 m/s. Real pulses (up to
  30 m/s and beyond) last several milliseconds and are all still counted.
- As a last safety net, the main firmware treats a 15-minute average above 30 m/s as noise: it is sent as 0.0
  and ERRORS.TXT keeps the value that was read.
- **The LCD's wind speed is now the last 3 seconds**, so it follows the cups and shows 0.0 about 3 s after they
  stop. It used to show the average since the start of the 15-minute slot, which fell slowly (0.2, then 0.1 for
  minutes). Records still carry the 15-minute average.
- After a CTRL restart, the slot uses the CTRL's live readings instead of zeros (CTRL v1.13 reads the sensor at
  start-up).

### Slot records
- **A slot is always named by the main board's clock.** On unit 1903 (failed RTC) a 15:30 slot was stored and
  sent as a second 15:15. If the CTRL's record is for another slot, the slot keeps the right name, its rain is
  counted in a later slot (the day total is never lost or doubled) and ERRORS.TXT says so.
- Settings (station ID, office numbers, server 2, STOP) can no longer be lost when two changes happen at once.
- The unit no longer powers off, or restarts, under a slot that has just started: the power save waits for it.
- A full SD card can no longer make the backlog file unreadable.

### Other
- Solar voltage removed from the LCD. MENU has one SEND GPS SMS item; SET DATE/TIME comes before SET STATION ID.
- Error log: the same error is written once a day, also for lines of different lengths.
- Production build: the debug log (LOG folder, HTTPLOG.TXT) is written only when the card holds an empty file
  named `DEBUGLOG.ON` at its root.

---

## 2. How to install

### Units already on the new firmware (v6.9.x)
1. Copy everything from the `SD_CARD` folder in the zip to the root of the unit's SD card (replace the old files).
   Keep the card's other files and folders.
2. **For test units, also create an empty file named `DEBUGLOG.ON`** at the root of the card, so the unit keeps
   the debug log we need to check the run.
3. **MENU > FIRMWARE UPDATE > CTRL F/W**, wait for `CTRL UPDATE OK` (about 1 minute). The LCD then shows CTRL v1.14.
4. **MENU > FIRMWARE UPDATE > MAIN F/W.** The unit restarts into v6.9.54 and confirms it after about 30 seconds.
   `UPDATE.TXT` on the card shows `UPDATE OK, now running v6.9.54`.

### Old units (original firmware with the old SD-card bootloader)
1. Copy the `SD_CARD` files to the root of a card (and `DEBUGLOG.ON` for test units).
2. With the unit **off**, fit caps on **JP1 and JP2** on the CTRL board, insert the card, power on.
3. The LCD shows `CONVERTING UNIT  MIG-1.9`. When it shows `REMOVE JP1+JP2`, take both caps off. The unit
   finishes and starts on v6.9.54.
4. Check the station ID and unit type on the LCD; set them from the keypad if asked.

---

## 3. What to test over the weekend

| # | Test | Expected |
|---|---|---|
| 1 | Update (or convert) each unit type | `UPDATE OK`; LCD shows v6.9.54 and CTRL v1.14 |
| 2 | Normal running, card in all the time | Every slot in SLOTS.CSV `SENT` (or `BACKLOG` then sent), no gaps, no slot name twice |
| 3 | TRG: 08:45 rain-day change | Day total restarts; no drop within a day |
| 4 | SMS with the LCD on: `VARSHA GET_STATUS` | Reply within seconds |
| 5 | SMS unattended: send `VARSHA GET_STATUS` a few minutes before 11:00 or 17:00, from the same operator as the unit's SIM and from another | Reply at about hh:01 (same operator). Note which arrived |
| 6 | Other slots | No `[SMS]` lines in the LOG for slots other than 11:00 and 17:00 |
| 7 | `VARSHA STOP`, wait 2 slots, `VARSHA RESUME` (office number, LCD on) | Nothing sent while stopped, records on the card; sending restarts; no old records sent after RESUME |
| 8 | Power cut for 1 minute during the day | Correct time after power-on; next slot sent |
| 9 | TWS: cups still, on battery and on a mains supply | Live wind speed on the LCD 0.0; records 0.0; no wind-speed lines in ERRORS.TXT |
| 10 | TWS: turn the cups slowly by hand, then stop them; then spin them fast | LCD follows within a few seconds and shows 0.0 about 3 s after they stop; a fast spin shows a high value |

Please send back the whole SD card contents (or at least `SLOTS.CSV`, `ERRORS.TXT`, `UPDATE.TXT`,
`HTTPLOG.TXT` and the `LOG` folder) with the station ID and what was done when.

---

## 4. Known limits in this release

- Unattended SMS from another operator's number may arrive late or not within a listening window (see 1).
- A unit with a failed RTC drifts about 6 s a slot between server corrections; the server corrects it.
- The reply to an unknown SMS command does not list STOP, RESUME and ERASE_DATA (it would not fit one SMS).

---

## 5. Checks done before release

- All three unit types built from the same source as production builds.
- Host test suite: all 13 sets pass (firmware update power-cut simulations, CTRL update, CTRL repair, frame
  compatibility, station ID, SMS command parsing).
- Code review of the changes since v6.9.38 and of the whole main, CTRL and conversion firmware: no path found
  that can leave a unit unable to start; the issues found were fixed in v6.9.51 to v6.9.54, and each set of fixes was reviewed again.
- Bench unit 1903 (TWS, failed RTC) ran v6.9.43 to v6.9.53 on 13 Nov, all slots sent; CTRL v1.14 installed
  from the SD card (`CTRL UPDATE OK`). **v6.9.54 itself runs first on 1903 alongside the test team's weekend run.**

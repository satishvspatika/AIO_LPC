# AIO Data Logger: Release v6.9.63

**Release date:** 20 November 2026

**Type:** Production build (the debug log is off unless the card holds `DEBUGLOG.ON`)

**For:** Test team, bench and field test units

| Part | Version | File on the SD card |
|---|---|---|
| Main firmware, KSNDMC rain gauge (TRG) | v6.9.63 | `fw_ksn_trg.hex` |
| Main firmware, Bihar rain gauge (TRG) | v6.9.63 | `fw_bihar.hex` |
| Main firmware, KSNDMC weather station (TWS) | v6.9.63 | `fw_ksn_tws.hex` |
| CTRL firmware (all unit types) | v1.17 | `ctrl.hex` |
| Old-unit conversion image | MIG-1.10 | `fw.hex` |

---

## 1. What is new since v6.9.54

### Firmware over the air (OTA), new
- Each unit checks the office OTA server twice a day: at **00:30 and 12:30, plus 15 minutes times (station ID mod 8)**
  (for example station 1922 checks at 01:00 and 13:00).
- When the office has set a new firmware for the station, the unit downloads it to the SD card in small
  checked pieces (a broken download carries on where it stopped), then installs it with the same SD-card
  update used from the keypad, after the slot's record is sent. About 2 minutes on 4G, about 5 minutes on 2G.
- **The CTRL firmware can now also be sent over the air.** It is sent only when no main firmware update is
  waiting, and installed only when there was no rain in this slot or the one before.
- A failed check is tried again at the next 3 slots that day. Below **11.0 V** only the download waits; the
  check and the report still go.
- A rolled-back firmware is never taken again. The slot running when the new firmware starts is not sent twice,
  and the unit powers off as soon as the new firmware is confirmed.
- `VARSHA SET_OTA_SERVER host [port]` / `VARSHA SET_OTA_SERVER OFF` (office numbers) change or stop it.
- Bench units: an empty file `OTA.NOW` at the root of the card makes the unit check at every slot.

### Daily health report, new
- With the 12:30 check the unit sends a short report on the last whole rain day (08:45 to 08:30): records sent
  of 96 and how many by backlog, day and night battery averages, lowest battery, errors logged and the newest one,
  firmware and CTRL versions. It is shown on the office page.

### Shorter time on per slot (battery)
- The unit wakes 40 s after the slot boundary and is on about **27 s per slot** (was about 70 s on 1922).
- The APN that last worked on the SIM is remembered (`APN.DAT`) and tried first; modem settings are written only
  when they differ.
- The main chip sleeps when it has nothing to do.
- An unattended slot that is still on after 13 minutes is powered off.

### SMS commands
- Unattended units listen for SMS at the **11:00, 12:00, 16:00 and 17:00 slots** (was 11:00 and 17:00), with a fresh
  network join. With the LCD on, commands are answered within seconds, as before.
- The modem's radio mode is set back to 4G-only (or 2G-only) before power-off after a listening slot.
- A repeated SMS from the network is not answered twice. A fifth office number can be stored.
- A status or GPS SMS for which the network gave no confirmation now shows **`NOT CONFIRMED / MAY HAVE SENT`**
  on the LCD (it usually has arrived; please do not send it again at once).

### Keypad and LCD
- **POWER SAVE while the unit is busy** (GPS search, SMS, slot, update): the LCD says what is busy, a GPS search or
  status SMS is stopped, and the unit powers off when done. (Before, pressing EN on POWER SAVE during SEND GPS did nothing.)
- The keypad task is now watched by the watchdog: a stuck keypad restarts the unit instead of hanging it.
- The keypad SD-card update needs **11.0 V** (was 11.8 V).

### Network and clock
- 2G-only areas: the modem's old 4G registration is no longer taken as a network.
- First data connection wait 12 s (was 25 s); a second try after 5 s.
- The clock is corrected from the server when it is **20 s** off (was 90 s).

### CTRL v1.17 (all unit types)
- The slot wake delay is set by the main board (v1.15/v1.16); clock (RTC) reads are checked; the charge
  controller's frame is checked before use; the CTRL sleeps between its 1 ms ticks.
- v1.17: the CTRL's watchdog is fed only while its 1 ms tick is still counting, so a stuck CTRL restarts itself.

### Old-unit conversion MIG-1.10
- Same conversion as MIG-1.9; it now puts CTRL v1.17 on the converted unit (MIG-1.9 put v1.14).

---

## 2. How to install

### Units already on the new firmware (v6.9.x)
1. Copy everything from the `SD_CARD` folder in the zip to the root of the unit's SD card (replace the old files).
   Keep the card's other files and folders.
2. **For test units, also create an empty file named `DEBUGLOG.ON`** at the root of the card, so the unit keeps
   the debug log we need to check the run.
3. **MENU > FIRMWARE UPDATE > CTRL F/W**, wait for `CTRL UPDATE OK` (about 1 minute). The LCD then shows CTRL v1.17.
4. **MENU > FIRMWARE UPDATE > MAIN F/W.** The unit restarts into v6.9.63 and confirms it after about 30 seconds.
   `UPDATE.TXT` on the card shows `UPDATE OK, now running v6.9.63`.

Units already on v6.9.55 or later can instead be updated over the air from the office page (main firmware first,
then CTRL).

### Old units (original firmware with the old SD-card bootloader)
1. Copy the `SD_CARD` files to the root of a card (and `DEBUGLOG.ON` for test units).
2. With the unit **off**, fit caps on **JP1 and JP2** on the CTRL board, insert the card, power on.
3. The LCD shows `CONVERTING UNIT  MIG-1.10`. When it shows `REMOVE JP1+JP2`, take both caps off. The unit
   finishes and starts on v6.9.63.
4. Check the station ID and unit type on the LCD; set them from the keypad if asked.

---

## 3. What to test

| # | Test | Expected |
|---|---|---|
| 1 | Update (or convert) each unit type | `UPDATE OK`; LCD shows v6.9.63 and CTRL v1.17 |
| 2 | Normal running, card in all the time | Every slot in SLOTS.CSV `SENT` (or `BACKLOG` then sent), no gaps, no slot name twice |
| 3 | TRG: 08:45 rain-day change | Day total restarts; no drop within a day |
| 4 | OTA: office sets a new firmware for the station | Downloaded at the station's check time, `UPDATE OK` in UPDATE.TXT, new version on the office page |
| 5 | OTA: office sets a CTRL firmware for the station (no rain) | CTRL updated at a check slot; office page shows the new CTRL version |
| 6 | Day after install | Health report for the day on the office page |
| 7 | SMS unattended: `VARSHA GET_STATUS` a few minutes before 11:00, 12:00, 16:00 or 17:00 | Reply at about that slot |
| 8 | MENU > SEND GPS, then POWER SAVE + EN while it searches | LCD names the busy job, search stops, unit powers off |
| 9 | 2G-only area (or SIM forced to 2G) | Slots sent; no long waits in the LOG |
| 10 | Power cut for 1 minute during the day | Correct time after power-on; next slot sent |
| 11 | TWS: cups still / turned by hand | LCD and records 0.0 when still; LCD follows within a few seconds |

Please send back the whole SD card contents (or at least `SLOTS.CSV`, `ERRORS.TXT`, `UPDATE.TXT`,
`HTTPLOG.TXT` and the `LOG` folder) with the station ID and what was done when.

---

## 4. Known limits in this release

- Unattended SMS from another operator's number may arrive late or not within a listening window.
- A unit with a slow or failed clock chip drifts a few seconds a slot between server corrections; the server corrects it.
- OTA needs the office server to be reachable from the SIM (Airtel M2M SIMs need the server approved by Airtel).
- If the unit loses the CTRL's "update done" reply after a good CTRL send, the next check may report that CTRL
  update as failed although the CTRL took it; the office page then shows a failure that can be ignored.

---

## 5. Checks done before release

- All three unit types built from the same source as production builds.
- Host test suite: all 17 sets pass (firmware update power-cut simulations, CTRL update, CTRL repair, OTA download
  and install including CTRL by OTA, health report, clock check, frame compatibility, station ID, SMS commands).
- Design review of the whole main and CTRL firmware on 18 Nov; the fixes are in this release and were reviewed again.
- Bench unit 1922 (KSNDMC TRG): OTA updates v6.9.55 to v6.9.61 passed on 4G and 2G (17-18 Nov), 45 of 46 slots
  sent first try overnight. **v6.9.63 and CTRL v1.17 (and CTRL by OTA) run on units for the first time with this release.**

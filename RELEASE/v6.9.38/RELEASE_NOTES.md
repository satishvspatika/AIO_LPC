# AIO Data Logger: Release v6.9.38

**Release date:** 13 November 2026

**Type:** Testing release (the debug log on the SD card is on by default)

**For:** Test team, bench and field test units

| Part | Version | File on the SD card |
|---|---|---|
| Main firmware, KSNDMC rain gauge (TRG) | v6.9.38 | `fw_ksn_trg.hex` |
| Main firmware, Bihar rain gauge (TRG) | v6.9.38 | `fw_bihar.hex` |
| Main firmware, KSNDMC weather station (TWS) | v6.9.38 | `fw_ksn_tws.hex` |
| CTRL firmware (all unit types) | v1.12 | `ctrl.hex` |
| Old-unit conversion image | MIG-1.7 | `fw.hex` |

---

## 1. What is new in this release

### Weather station (TWS): no more zero readings sent to the server
- **A slot with temperature or humidity at 0 is no longer sent or stored.** A 0 always means the reading is
  missing (sensor, cable, or a CTRL that has just restarted), never a real value. SLOTS.CSV shows the slot as
  `SKIPPED,NO_TEMP_HUMID` and ERRORS.TXT says why.
- The same rule applies to slots filled in later from the CTRL's stored samples, and to records still waiting
  in the backlog from older firmware. Those are moved to `REJECTED.TXT` instead of being sent.
- **A slot that runs a second time (after a restart) resends exactly what was stored the first time,** never
  new readings. Before this fix, a repeat send could put zeros over a good record on the server
  (seen on unit 1903, 12 Nov 16:34).

### Clock: a unit with a failed clock chip (RTC) no longer jumps back in time
- After a power cut, a CTRL with a failed RTC used to report the time the RTC had frozen at (43 minutes back on
  unit 1903). **CTRL v1.12 gives no time after power-on until it sees the RTC actually ticking.** If the RTC
  is dead, the unit takes the time from the network or GPS instead.
- If the RTC had stopped while the unit was off (backup cell flat), its old time is not trusted either: the unit
  waits for the network or GPS time.
- On a unit whose CTRL is keeping the time itself (RTC failed, CTRL v1.11 and later), the server time now
  corrects it once it is 15 s off instead of 90 s, so slots stay on time to within about 15 s.

### Error log (ERRORS.TXT)
- The same error is written **at most once a day.** A unit with a failed RTC used to write the same line every
  slot (96 a day), which pushed real faults out of the 48-entry error memory within about 12 hours.

### Old-unit conversion
- **MIG-1.7** is MIG-1.6 with CTRL v1.12 inside. Nothing else changed in the conversion steps.

---

## 2. How to install

### Units already on the new firmware (v6.9.x)
1. Copy everything from the `SD_CARD` folder in the zip to the root of the unit's SD card (replace the old files).
   Keep the card's other files and folders.
2. Insert the card and power the unit on. On the LCD: **MENU > FIRMWARE UPDATE > CTRL F/W**.
   Wait for `CTRL UPDATE OK` (about 1 minute). The LCD then shows **v1.12** for CTRL.
3. **MENU > FIRMWARE UPDATE > MAIN F/W.** The unit restarts into v6.9.38 and confirms it after about
   30 seconds. `UPDATE.TXT` on the card shows `UPDATE OK, now running v6.9.38`.

The two updates can be done in either order; both orders work with the other part's old version.

### Old units (original firmware with the old SD-card bootloader)
Same steps as MIG-1.6:
1. Copy the `SD_CARD` files to the root of a card. The unit chooses the `fw_*.hex` for its own type.
2. With the unit **off**, fit caps on **JP1 and JP2** on the CTRL board, insert the card, power on.
3. The LCD shows `CONVERTING UNIT  MIG-1.7` and works through the steps. When it shows
   `REMOVE JP1+JP2`, take both caps off. The unit finishes and starts on v6.9.38.
4. Check the station ID and unit type on the LCD; set them from the keypad if asked.

---

## 3. What to test

| # | Test | Expected |
|---|---|---|
| 1 | Update CTRL then MAIN as above, on each unit type | `UPDATE OK` for both; LCD shows v6.9.38 / CTRL v1.12 |
| 2 | Normal running for 24 hours | Every slot in SLOTS.CSV `SENT`, no gaps; 08:45 rain-day change on TRG |
| 3 | **Power cut**: remove power for 1 minute during the day, then restore | Correct time on the LCD after power-on (never an old time); next slot sent normally |
| 4 | TWS only: unplug the temperature/humidity sensor for one slot, then plug it back | That slot `SKIPPED,NO_TEMP_HUMID`, **nothing sent with 000.0**; next slot normal |
| 5 | TWS only: restart the unit (power cycle) within a minute after a slot was sent | No second send of that slot with different values on the server |
| 6 | Unit with a failed RTC (LCD shows RTC BAD), if available | Time stays right; HTTPLOG/LOG show corrections at about 15 s; one RTC line a day in ERRORS.TXT |
| 7 | Old unit conversion with MIG-1.7 | Conversion completes; CTRL v1.12 on the LCD |

Please send back the whole SD card contents (or at least `SLOTS.CSV`, `ERRORS.TXT`, `UPDATE.TXT`,
`HTTPLOG.TXT` and the `LOG` folder) with the station ID and what was done when.

---

## 4. Known limits in this release

- **A unit with a failed RTC that loses power while it has no network** records no slots until the network
  returns (it has no trustworthy time). Older versions recorded them under a wrong time. A GPS-time fallback
  is planned. The real fix for such a unit is to repair its RTC.
- **TWS:** the first slot right after the CTRL restarts is skipped (the CTRL has not yet read the sensor).
  A fix is planned.
- This is a **testing build**: the debug log (LOG folder, HTTPLOG.TXT) is written to the SD card on every
  unit. Production builds will switch it off.

---

## 5. Checks done before release

- All three unit types and the CTRL built from the same source; images are 121.9 to 122.6 KB of 128 KB.
- Host test suite: all pass (firmware update power-cut simulations: 4,619 and 4,914 scenarios with no image
  lost; CTRL update; CTRL repair by ROM loader; frame compatibility between main and CTRL; station ID;
  network policy).
- Worst-case stack use checked for every task on all three types: all within limits.
- The new zero-reading and once-a-day error checks were tested on the bench PC with real lines from unit 1903.
- Bench unit 1903 (TWS) ran v6.9.37 overnight 12-13 Nov: 71 of 71 slots sent first try. **v6.9.38 itself
  has not yet run on a unit:** the test team's run is its first field test.

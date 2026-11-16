# AIO Data Logger: Release v6.9.55 (in progress)

**Status:** Work in progress, weekend of 14-15 November 2026. v6.9.54 stays the test team's version until this is released.

**Type:** Production build

| Part | Version | File on the SD card |
|---|---|---|
| Main firmware, KSNDMC rain gauge (TRG) | v6.9.55 | `fw_ksn_trg.hex` |
| Main firmware, Bihar rain gauge (TRG) | v6.9.55 | `fw_bihar.hex` |
| Main firmware, KSNDMC weather station (TWS) | v6.9.55 | `fw_ksn_tws.hex` |
| CTRL firmware (all unit types) | v1.16 | `ctrl.hex` |
| Old-unit conversion image | MIG-1.9 (unchanged) | `fw.hex` |

---

## 1. What is new since v6.9.54

### Shorter awake time per slot (build 12/13 + CTRL v1.16)
- **Before:** the CTRL powered the unit 30 s after each slot boundary. The unit then waited about 26 s with
  nothing to do, because the server takes a record only from 62 s after its boundary. That wait was about
  80 % of the 39 s the unit was awake (001903, 15-16 Nov, 92 slots).
- **Now:** the CTRL powers the unit 40 s after the boundary. The modem still has 22 s to start before the
  send, and the unit waits for it to register when it needs longer. Expected: about 29 s awake per slot
  instead of 39 s, for both the main chip and the modem. Sends happen between +62 s and about +67 s.
- **Data is not affected:** the CTRL closes the slot's readings at the boundary, before it powers the unit.
- The unit no longer presses the modem's power key while the modem is still starting up by itself. It now
  waits up to 30 s from power-on first.
- 2 s fixed wait removed from each modem start when the modem already answers.
- The log now says when the modem answered and registered (`Modem answered Ns after its power came on`,
  `Registered Ns after the modem's power came on`). These are the figures for checking the 40 s.
- The hold message no longer says "for sensors": `holding Ns (server takes records from +62s)`.
- **The CTRL must be updated to v1.16 as well** (v1.15 was a bench build of 16 Nov, never released). With the old CTRL (30 s wake) the unit behaves as in
  v6.9.54, just without the 2 s wait. CTRL v1.16 with an older main firmware also behaves as before (30 s).
- **Future changes to the wake time need no CTRL update.** The main firmware tells the CTRL the delay
  (40 s now) and the CTRL reports the one in force every second. After a CTRL restart it starts at 30 s
  and the main firmware sets it again at the end of that slot. The log shows
  `CTRL slot wake is 30s: set to 40s at power-off` when that happens.
- CTRL v1.16 refuses a damaged set-time command instead of setting a wrong time.

### Lower power while waiting (build 13 + CTRL v1.16)
- **Main chip:** it used to run at full speed for the whole time it was on, most of which is waiting (the
  hold to +62 s, the modem's answers). It now sleeps between events and wakes on any interrupt, at the
  latest 1 ms later. Nothing is slower.
- **CTRL:** the one chip that is on all day did the same, all day. It now sleeps between its 1 ms ticks.
  Rain and wind counting, the DSP key and all timing run in those ticks, as before.
- Neither changes what is measured or sent. The saving has not been measured yet: compare the unit's
  standby current (asleep) and its current during a slot with v6.9.54 + CTRL v1.14.

### CTRL v1.16: clock reads and charge-controller input checked
- A clock-chip read is now used only if it is a real date and time and follows the time the CTRL already
  holds by 0-3 s. Anything else (clock just set, first read after a start) needs a second read that agrees.
  Before, any time with today's date was taken, so one damaged read could show a slot boundary that wasn't
  there and close a slot under a wrong time.
- The charge-controller serial input is used only when it is a whole, correctly formed message. Its
  battery value replaces the CTRL's own reading in the record, so noise on that input could have been
  sent as the battery voltage.

### Fixes from the build 12 review
- SMS reply wait: a leftover reply from the modem could be read as "network refused", and the reply sent
  twice. It is now cleared before the wait.
- "Already answered" list for repeated SMS: with no card it was forgotten at each check during one
  session. It is now kept for the whole session.

### SMS from other operators: what to expect (radio handling unchanged from v6.9.54)
- Commands from a **BSNL** phone (the unit's own operator) are answered in the next slot, within 15 minutes.
- Commands from **Jio or Airtel** are answered in the first slot that falls on one of that operator's retries,
  typically **10-60 minutes** after sending (measured on 001903, 13-14 Nov: Jio 20-57 minutes, Airtel 12 and
  17 minutes).
  The other operator's SMS centre retries on its own timer and is not told when the unit comes back, so the unit
  can't make it come sooner while its modem is switched off between slots.
- Tried and dropped during this work (builds 1-3 of 14 Nov): keeping the modem's own registration from power-on
  and leaving the radio in automatic mode. It did not bring other operators' SMS sooner, and on 14 Nov 09:00 the
  slot's data failed in that mode. The radio handling is back to v6.9.54's (4G first for data, then automatic mode
  for the SMS window).

### SMS listening: 11:00, 11:15, 16:00 and 16:15
- Four slots a day listen for SMS (v6.9.54: 11:00 and 17:00). Each one leaves the network and joins again,
  which is what makes Jio and Airtel deliver the commands they are holding. The second slot of each pair catches
  what the first didn't: Airtel hands over its queue oldest first.
- A command sent after 16:15 is answered at 11:00 the next day.
- `SET_TIME` is refused if it is more than 15 minutes old, so send it just before one of these slots.
- **Build 14:** the window is 20 s after registering (was 10 s), and stays open 60 s after each SMS taken
  (was 8 s), up to 4 minutes. An SMS centre hands over the next waiting SMS 30-50 s after the unit's reply
  to the one before (Airtel, 14 Nov 10:37-10:39). With 8 s the unit switched off 14 s after the last SMS,
  so the rest of the queue met a unit that was off and came a slot or a day later (14 Nov 16:16: two sent
  at 16:11 and 16:12 came at 11:01 the next day). A slot with no SMS is 10 s longer than before; only a
  slot that takes one stays on for the extra minute.
- **Build 14:** an SMS that arrives while the last replies go out is read before power-off, not left on
  the SIM for the next listening slot.
- **Build 14:** a message whose text contains the word `ERROR` could stop the SIM's message list from
  being read. That message then stayed on the SIM and blocked every command after it. The list is now
  read to its own end line.
- Listening in all 96 slots (tried in builds 4-7) costs about 26 minutes of modem time a day; these 4 cost
  about 1-2 minutes.

### Radio mode command refused
- On 14 Nov at 15:01 the modem answered `ERROR` to the 4G-only and 2G commands (`AT+CNMP`). The slot went out
  on 3G, and the unit learned "2G first" from it. The command is now tried 3 times; if it is still refused, that
  slot doesn't change the learned radio mode (log: `Radio mode command (AT+CNMP) refused 3 times`).

### SMS replies are no longer sent twice
- **Problem in v6.9.54 (and the first v6.9.55 build):** one `GET_GPS` got 4 replies. The unit waited 60 s for
  the network to confirm its reply; on BSNL 4G the confirmation took 102 s. The unit took that as a failure,
  restarted the modem and sent the reply again (14 Nov 08:22-08:24). The same happened on 13 Nov at 16:35.
  The network may also deliver extra copies itself when it is that slow; the unit can't stop those.
- **Now:** the unit waits up to 3 minutes for the confirmation. A reply that has gone to the network is never sent
  again, even if no confirmation comes; it is only retried when the network clearly refused it or it never
  left the unit. A missing reply can be asked for again; a repeated one can't be taken back.

### Power save no longer floods the log while the modem is busy
- With the LCD on, if power save came while the modem was still sending, the unit retried every 50 ms and wrote
  "Entering sleep" 340 times in 18 s, so 56 log lines were lost. It now waits quietly until the modem is free.

### Radio-mode message corrected
- After a slot sent on 2G because no 4G was found, the log said "4G registers but can't send here". It now says
  "Sent on 2G (no 4G this time)".

### Airtel (and other) commands no longer stall or get answered twice
- **Found on 14 Nov:** Airtel delivered some commands twice, the same message up to 66 minutes apart. Each first
  delivery came while the unit was leaving the network for its SMS window, or sending a reply SMS. The modem's
  acknowledgement then didn't reach Airtel, so Airtel took the delivery as failed. It then waited on its own slow
  retry timer and held every newer command behind it. That was the 1-hour stall. A new SMS from the phone made
  Airtel try again at once, which is why one command seemed to "release" the old ones.
- **Now:**
  - The SMS window waits until no SMS has arrived for 4 s before leaving and re-joining the network. If an SMS
    came in at the start of the window, it stays on the network instead.
  - (Build 6 tried not re-joining at all. Jio/Airtel SMS then took 1-1.5 hours: the fresh join is what makes the
    network tell the other operator's SMS centre the unit is back. Build 7 re-joins again, with the wait above.)
  - Replies wait until no SMS has arrived for 4 s. Messages arriving together are all read first.
  - Power-off waits a few seconds after an SMS arrives, so its acknowledgement gets out.
  - A message the network delivers again (same sender, network time stamp and text as one of the last 10 handled,
    kept in `SMSSEEN.DAT` on the card) is not answered again.

### Fifth office number
- 9980052058 (Airtel) added to the built-in office numbers; the list (5 at most) is now full. This applies to
  units whose SMS settings were never saved. A unit that has had any of `ADD_NUM`, `DEL_NUM`, `SET_SERVER_2`,
  `SET_SERVER_MODE`, `STOP` or `RESUME` keeps its own list: add the number there with `VARSHA ADD_NUM 9980052058` from an office number.

### One build switch
- `TESTING_DEBUG_LOG` is gone. `PRODUCTION_BUILD 1` is the only switch: the debug log is off unless the card
  holds `DEBUGLOG.ON`. Test units run the same release files with an empty `DEBUGLOG.ON` on their cards, so what
  is tested is exactly what ships. `make_release.sh` refuses a bench build (`PRODUCTION_BUILD 0`).

---

## 2. What to test

1. **BSNL phone, unit asleep:** send `VARSHA GET_STATUS` before 11:00 or 16:00: one reply in that slot.
2. **Jio or Airtel phone, unit asleep:** one reply at 11:00/11:15 or 16:00/16:15, never two.
   Build 14: send 3 commands a minute apart from one Jio or Airtel phone before 11:00: all 3 should be
   answered in the 11:00 slot (the log shows each `In #` up to about 50 s after the reply before it).
3. Slots still send on 4G first try (SLOTS.CSV `SENT`, attempt 1).
4. A slow network confirmation of a reply (log `No confirmation in 60 s`): still one reply only.
5. Several commands from Airtel while the unit is asleep: all answered, once each, without needing a new SMS to
   "release" them (log: `leaving the network and joining again` comes after the quiet wait, or `an SMS is arriving: staying on the network`; any repeat shows `Sent again by the network`).
6. **CTRL v1.16 wake:** the first slot after the CTRL update still wakes at hh:mm:30 and logs `CTRL slot wake
   is 30s: set to 40s at power-off`; from the next slot the log shows `Boot type: SCHEDULED` at about hh:mm:40 and `holding` about 16 s. Check
   `Registered Ns` stays at or below about 27 s, SLOTS.CSV still `SENT` first try, and the time from boot to
   `$f7 sent` (+Ns) is about 29 s. Watch a BSNL TRG unit (1922 or 1923) too: report any rise in
   `no network service`.
7. **Lower power (build 13 + CTRL v1.16):** measure the current asleep and during a slot against a unit on
   v6.9.54 + CTRL v1.14. Check rain tips (tip the bucket by hand: each tip counted once), the DSP key wake,
   wind speed on a TWS unit, and that slots, the keypad and the LCD behave as before.
8. **Clock checks:** set the time from the menu and by SMS (`SET_TIME`): the new time holds. A power
   switch off/on: the unit comes back with the right time as before.

---

## 3. Known limits
- Commands are answered only at 11:00, 11:15, 16:00 and 16:15 (or at once with the LCD on).
- `=== BOOT` and `Free heap` lines are missing from the debug log on most scheduled boots (cosmetic, since v6.9.37).

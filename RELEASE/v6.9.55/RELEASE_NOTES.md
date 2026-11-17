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

### Signal read again when the modem has no reading yet (build 15)
- On 16 Nov at 09:46 (Airtel SIM) the record went out with signal -113. The unit asked for the signal in the
  second the modem moved from 2G to 4G, and the modem answered "not measured yet". The send itself was fine.
- The unit now asks again, 1 s apart, up to 3 more times. A slot with a normal first reading is not slower.

### APN handling: three things taken from the ESP32 firmware (build 21)
- **The APN that last delivered on a SIM is remembered** (`APN.DAT` on the card: SIM number and APN) and
  tried first from then on. Before, a SIM the built-in rules placed wrongly, such as an Airtel M2M SIM whose
  number does not start with 899116, 899110 or 899145, paid a radio restart to reach the right APN in every
  slot; now once. Log: `APN ... remembered for this SIM`, then `APN ... delivered last on this SIM: tried
  first`. `airtelgprs.com` is never remembered for a known Airtel M2M SIM. With no card, or after a SIM
  change, the rules decide as before.
- **Modem settings are written only when they differ.** The APN and the unused side channels were written
  to the modem's own memory again, unchanged, twice in every slot. The unit now reads the list once and
  writes only what is wrong (normally nothing). About 1-2 s shorter per slot and less wear on the modem.
- **The second data-connection try comes 5 s after a refusal** (it was 2-5 s). On 1922 overnight (BSNL 4G):
  5 s after `no network service` it worked 14 times of 14; 2 s after, it failed 15 times of 15 and the
  radio restart that follows cost 22 s.
- Not yet run on a unit. The host tests do not cover this code.

### Less time on per slot: SMS at 4 slots a day, radio mode set back before power-off (build 20)
- Measured on 1922 with a BSNL SIM, 38 slots overnight on build 19 (16 Nov 21:30 to 17 Nov 06:45): all
  sent first try, but the unit and modem were on about 70 s per slot (about 111 minutes a day): 23 s
  waiting to start, 20 s sending, 26 s SMS listening.
- **SMS listening is back to 4 slots a day: 11:00, 12:00, 16:00 and 17:00** (it was every slot in builds
  18 and 19). Each of the four leaves and joins the network, as before. A command waits 18 hours at most,
  or is answered at once with the LCD on. Build 19's "once an hour" note below no longer applies.
- **The modem is switched off in the radio mode the next slot starts with.** Listening leaves it in
  automatic mode and the modem remembers that. In 29 of the 38 slots the next slot showed 4G registered
  but data refused (`no network service`), costing 13-25 s each. The log shows
  `Radio mode set back to 4G-only for the next power-on`. Whether this removes the refusals is not yet
  proven on a unit: the 40 s wake could also be the cause.
- **Only a command keeps the listening window open longer.** On 17 Nov at 05:31 a promotional SMS kept the
  unit on 60 s more. Other SMS are still read and deleted.
- Checked, no change needed (1922, 16 Nov): the Airtel M2M SIM (ICCID 899145...) got `airteliot.com`; after
  the swap to BSNL the 19:30 slot logged `SIM operator changed (40445 to 40471)`, searched 4G for 16 s, went
  to 2G and sent first try.

### 7-8 s shorter slot: faster check of the records kept in flash (build 19)
- After every send the unit looks in its flash record store for records still to send. Each old "to send"
  mark made it read the whole store twice, working out every page's check sum. With the 72 marks left from
  16 Nov on unit 1922 this took 7-8 s in every slot, with the modem on, and would have stayed until the store
  wrapped around.
- A page's type and number are now compared first and the check sum is worked out only for a page that
  matches. The result is the same. Not yet timed on a unit.

### Listening leaves and joins the network once an hour only (build 19)
- Measured on 1922 with a BSNL SIM (16 Nov, 19:30-21:00): every-slot listening added about 44 s to each slot
  (13-20 s to leave and join again, 20 s listening), about 70 minutes of modem time a day, not the 30 minutes
  estimated in build 18.
- Now only the hh:00 slot leaves and joins again (`SMS_REJOIN_EVERY_SLOTS 4`); the other three slots listen
  without it. SMS from the SIM's own operator come in any slot; one from another operator may wait for the
  next hh:00 slot.
- SMS commands on a BSNL SIM were proven on 1922 on 16 Nov (two `VARSHA GET_STATUS`, both answered).

### SMS listening in every slot again (build 18)
- Every 15-minute slot now listens for SMS commands (it was 11:00, 11:15, 16:00 and 16:15 only), so a command
  waits 15 minutes at most (`SMS_LISTEN_ALL_SLOTS 1`). Where these notes say "4 listening slots", this replaces it.
- Cost: about 20 s more modem time in each slot with no SMS, about 30 minutes a day, from the battery.
- Not changed, and not a firmware fault as far as the logs show: unit 1922's Airtel IoT SIM got no SMS at all
  on 16 Nov, and its two status SMS were accepted by Airtel (`+CMGS`) but did not reach the phone. That points
  to the SIM's SMS service; to be checked with a BSNL SIM in the same unit and with Airtel.

### Signal -113 after a change from 2G to 4G, longer wait (build 18)
- Build 15 asked for the signal 3 more times. On 16 Nov at 17:16 (unit 1922) the modem answered "not known
  yet" all 4 times in 3 s and the 17:15 record went out with -113.
- The unit now asks up to 10 more times, 1 s apart (`CSQ_RETRY_MAX`). Only a slot where the modem has no
  reading waits; a normal slot is not slower.

### "CTRL" in the log and error files (build 18)
- Lines in `ERRORS.TXT` and the debug log that named the control board by its chip number now say CTRL.
  The link line's `845reset=` is now `reset=`. Wording only.

### "UPDATE STOPPED / NO MEMORY" in the first seconds after switch-on (build 17)
- An update is two steps (check the file, then install) and each needs the same block of memory. In the
  first 15 seconds after switch-on the memory of the first step was not yet handed back, so EN at the `FOUND`
  prompt gave `UPDATE STOPPED / NO MEMORY` (unit 1922, 16 Nov, three times, each 10-11 s after switch-on).
- The unit now shows `PLEASE WAIT...` and starts the step by itself as soon as the memory is back (up to 30 s).
- A unit still on an older build has the fault: wait 30 s after switch-on before starting the update.

### No leave-and-join in the SMS listening window on an Airtel IoT SIM (build 17)
- On unit 1922 (Airtel IoT SIM, 13-digit number) the 16:02 listening window left the network and had not
  joined again when the unit switched off. The next three switch-ons took 65-90 s to find the network and two
  ended on 2G, where the 16:30 record could not be sent. Before that window the SIM joined 4G in about 15 s.
- It is not proven that the window caused this. Until it is, a unit with an Airtel IoT SIM listens without
  leaving the network (`SMS_REJOIN_AIRTEL_IOT 0`). Other SIMs are unchanged.

### SMS listening waits longer to join the network again (build 16)
- On 16 Nov at 16:02 (unit 1922, Airtel IoT SIM) the listening window left the network and joined again as
  designed, but the modem was still searching 20 s later. The unit gave up, and a `VARSHA GET_STATUS` sent to
  it was not delivered. On BSNL the same step takes about 4 s.
- The unit now waits up to 60 s for the network. The log shows how long it took: `registered for SMS Ns
  after joining again`. A unit that joins quickly is not slower.

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

0. **Build 20, any BSNL unit, a few hours of slots:** in the log, `no network service` should be rare
   in the slot after a non-listening slot, and boot to `$f7 sent` about 30-45 s (was about 70 s).
   SLOTS.CSV still `SENT`, attempt 1.
0b. **Build 21:** after the first sent slot the card holds `APN.DAT` with the SIM number and APN. After a
   SIM change to another operator the first slot still sends and `APN.DAT` shows the new SIM. In the log,
   a slot with `no network service` should show the second `AT+CGACT=1,1` about 5 s later answering `OK`,
   with no `Fresh attach`.
1. **BSNL phone, unit asleep:** send `VARSHA GET_STATUS` before 11:00 or 16:00: one reply in that slot.
2. **Jio or Airtel phone, unit asleep:** one reply at 11:00/12:00 or 16:00/17:00, never two.
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
- Commands are answered only at 11:00, 12:00, 16:00 and 17:00 (or at once with the LCD on).
- `=== BOOT` and `Free heap` lines are missing from the debug log on most scheduled boots (cosmetic, since v6.9.37).

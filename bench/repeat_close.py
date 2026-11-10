"""Tests 845-v1.8's guard against closing a slot twice, as a clock set back across a boundary would.

    python3 bench/repeat_close.py

Waits for the next :00/:15/:30/:45 boundary to close, then, 40 s into that minute, sets the LPC845's
last_rec_min back one minute. The main loop then sees the minute change to the boundary minute again,
exactly as after the LPC1768 sets the clock back from hh:16 to hh:15. 845-v1.8 must count it in
repeat_closes and leave the slot alone (no second store_data, no second wake); 845-v1.7 and earlier
would close it again.

Addresses come from LPC845/LPC845_TWS.axf, so the unit must run that build. Attaching does not reset
the LPC845.
"""
import os, subprocess, sys, time, datetime
from pyocd.core.helpers import ConnectHelper

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
AXF = os.path.join(ROOT, 'LPC845', 'LPC845_TWS.axf')
NM = os.path.join(ROOT, 'tools', 'arm-toolchain', 'bin', 'arm-none-eabi-nm')


def symbols():
    out = subprocess.run([NM, AXF], capture_output=True, text=True, check=True).stdout
    found = {}
    for line in out.splitlines():
        parts = line.split()
        if len(parts) == 3:
            name = parts[2].split('.')[0]
            found.setdefault(name, int(parts[0], 16))
    return found


def now():
    return datetime.datetime.now().strftime('%H:%M:%S')


sym = symbols()
need = ['Present_Date_Time', 'last_rec_min', 'repeat_closes', 'late_wakes', 'late_wake_pending', 'rain_day_tips']
missing = [n for n in need if n not in sym]
if missing:
    print('not in the axf (needs 845-v1.8):', missing); sys.exit(2)

# Attach only for the test itself: a long session has dropped the MCU-Link off USB. This Mac's clock
# is within seconds of the LPC845's, so attach 30 s into the next boundary minute.
while True:
    d = datetime.datetime.now()
    if d.minute % 15 == 0 and 30 <= d.second < 45:
        break
    time.sleep(1)

with ConnectHelper.session_with_chosen_probe(target_override='lpc845', connect_mode='attach',
                                             frequency=1000000,
                                             options={'resume_on_disconnect': True}) as s:
    t = s.board.target
    clock = sym['Present_Date_Time']

    def hms():
        return t.read8(clock + 4), t.read8(clock + 5), t.read8(clock + 6)

    h, m, sec = hms()
    print(now(), f'LPC845 clock {h:02d}:{m:02d}:{sec:02d}', flush=True)
    # Wait for a boundary minute, 40 s in: the late wake (+30 s) is done, the slot closed long ago
    while True:
        h, m, sec = hms()
        if m % 15 == 0 and 40 <= sec < 50:
            break
        time.sleep(0.5)
    before = {n: t.read32(sym[n]) for n in ('repeat_closes', 'late_wakes', 'rain_day_tips')}
    lrm = t.read32(sym['last_rec_min'])
    print(now(), f'at {h:02d}:{m:02d}:{sec:02d}: last_rec_min={lrm} before={before}', flush=True)
    if lrm != m:
        print('last_rec_min does not match the clock minute: not testing'); sys.exit(3)

    t.write32(sym['last_rec_min'], (m - 1) % 60)
    time.sleep(2)
    after = {n: t.read32(sym[n]) for n in ('repeat_closes', 'late_wakes', 'rain_day_tips')}
    lrm = t.read32(sym['last_rec_min'])
    pending = t.read8(sym['late_wake_pending'])
    print(now(), f'after: last_rec_min={lrm} after={after} late_wake_pending={pending}', flush=True)

    ok = (after['repeat_closes'] == before['repeat_closes'] + 1 and after['late_wakes'] == before['late_wakes']
          and after['rain_day_tips'] == before['rain_day_tips'] and lrm == m and pending == 0)
    print('PASS: repeat boundary counted, slot not closed again' if ok else 'FAIL')
    sys.exit(0 if ok else 1)

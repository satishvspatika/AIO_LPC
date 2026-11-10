"""Cuts the LPC1768 and modem rails part way through the next slot, as a power cut would.

    python3 bench/cut845.py SECONDS

Waits for the slot to power the LPC1768 (P27 low), waits SECONDS more, then sets P26 (modem) and
P27 (LPC1768) high: both rails off. The LPC845 reads the pins, so it sees the unit as asleep and
wakes it at the next slot as usual. With 845-v1.7 the rail comes on at hh:mm:30 and the record is
stored and sent from about 35 s later, so 37 cuts during the send.

Read-only until the cut. Attaching does not reset the LPC845.
"""
import sys, time, datetime
from pyocd.core.helpers import ConnectHelper

PIN, SET = 0xA0002100, 0xA0002200
MODEM, LPC1768 = 26, 27


def now():
    return datetime.datetime.now().strftime('%H:%M:%S')


delay = float(sys.argv[1])
with ConnectHelper.session_with_chosen_probe(target_override='lpc845', connect_mode='attach',
                                             frequency=1000000,
                                             options={'resume_on_disconnect': True}) as s:
    t = s.board.target
    if not (t.read32(PIN) >> LPC1768 & 1):
        print(now(), 'LPC1768 already on: wait for it to sleep, then run again'); sys.exit(3)
    print(now(), 'waiting for the slot to power the LPC1768', flush=True)
    while t.read32(PIN) >> LPC1768 & 1:
        time.sleep(0.05)
    on = time.time()
    print(now(), f'rail on; cutting in {delay:.1f}s', flush=True)
    while time.time() - on < delay:
        if t.read32(PIN) >> LPC1768 & 1:
            print(now(), f'rail went off by itself after {time.time() - on:.1f}s: no cut made'); sys.exit(4)
        time.sleep(0.05)
    t.write32(SET, (1 << MODEM) | (1 << LPC1768))
    print(now(), f'CUT at {time.time() - on:.1f}s after rail on', flush=True)
    time.sleep(2)
    p = t.read32(PIN)
    print(now(), 'rails now', format((p >> 26) & 7, '03b'), '(111 = all off)')

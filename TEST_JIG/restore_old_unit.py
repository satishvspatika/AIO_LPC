"""Puts an old unit back to its old firmware from a probe dump, so the SD + JP1/JP2 conversion can be
practised again on the same unit (TEST_JIG.md 13). MCU-Link on J4 for lpc1768, on J2 for lpc845.

    python3 restore_old_unit.py lpc1768 old_units/old_2114_lpc1768.bin
    python3 restore_old_unit.py lpc845  old_units/old_2114_lpc845.bin

Start it first, then press DSP: it waits up to 4 minutes for the chip to power up and grabs it at once.
The old CTRL keeps the unit on for only about 90 s after DSP, so it writes only the used part of the dump
(after a chip erase, the rest is blank anyway) at 4 MHz: about 3 s. Then reads back and compares.

Only on a unit that is really meant to go back to old firmware: never on a unit in the field.
The dumps hold the server login: they stay in TEST_JIG/old_units/ (git-ignored)."""
import os, sys, tempfile, time
from pyocd.core.helpers import ConnectHelper
from pyocd.flash.file_programmer import FileProgrammer

if len(sys.argv) != 3 or sys.argv[1] not in ('lpc1768', 'lpc845'):
    sys.exit(__doc__)
chip, path = sys.argv[1], sys.argv[2]
full = open(path, 'rb').read()
used = len(full.rstrip(b'\xff'))
used = (used + 0xFFF) & ~0xFFF
data = full[:used]
tmp = tempfile.NamedTemporaryFile(suffix='.bin', delete=False)
tmp.write(data)
tmp.close()

try:
    t0 = time.time()
    print('waiting for the %s: press DSP now' % chip, flush=True)
    while True:
        try:
            s = ConnectHelper.session_with_chosen_probe(target_override=chip, options={'frequency': 4000000, 'connect_mode': 'attach'})
            s.open()
            s.target.halt()
            break
        except Exception:
            if time.time() - t0 > 240:
                sys.exit('no answer within 4 minutes: check the MCU-Link header (J4 main chip, J2 CTRL chip) and press DSP')
            time.sleep(0.3)
    t1 = time.time()
    t = s.target
    print('caught, writing %d KB' % (used // 1024), flush=True)
    FileProgrammer(s, chip_erase='chip', trust_crc=False).program(tmp.name, file_format='bin', base_address=0)
    back = bytes(t.read_memory_block8(0, used))
    diff = sum(1 for a, b in zip(data, back) if a != b)
    print('done in %.0f s: %d bytes differ' % (time.time() - t1, diff), flush=True)
    if diff == 0:
        t.reset()
        print('PASS: old firmware back and running')
    else:
        print('FAIL: run it again')
    s.close()
finally:
    os.unlink(tmp.name)

"""Read-only dump of an old unit's chip over SWD (attach, no reset, no halt).
usage: python3 old_read.py lpc1768|lpc845 OUTFILE
"""
import sys, time, logging, re, struct
from pyocd.core.helpers import ConnectHelper
logging.disable(logging.CRITICAL)
chip, out = sys.argv[1], sys.argv[2]
size = 0x80000 if chip == 'lpc1768' else 0x10000
with ConnectHelper.session_with_chosen_probe(target_override=chip, options={
        'frequency': 1000000, 'connect_mode': 'attach', 'resume_on_disconnect': True}) as s:
    t = s.target
    print(time.strftime('%H:%M:%S'), 'state', t.get_state().name)
    data = bytearray()
    step = 0x1000
    for a in range(0, size, step):
        data += bytes(t.read_memory_block8(a, step))
        if a % 0x10000 == 0:
            print('  read', hex(a)); sys.stdout.flush()
open(out, 'wb').write(data)
print('saved', out, len(data), 'bytes')
w = lambda a: struct.unpack_from('<I', data, a)[0]
print('0x0: SP', hex(w(0)), 'reset', hex(w(4)))
print('CRP word 0x2FC:', hex(w(0x2FC)))
for a in range(0, size, 0x1000 if chip == 'lpc1768' and False else 0x1000):
    pass
used = [hex(a) for a in range(0, size, 0x1000) if data[a:a + 0x1000] != b'\xff' * 0x1000]
print('non-blank 4K blocks:', len(used), used[:80])
for m in re.finditer(rb'[ -~]{6,}', data):
    s_ = m.group().decode()
    if re.search(r'(?i)boot|ver|fw\.hex|v\d', s_):
        print(hex(m.start()), s_[:70])

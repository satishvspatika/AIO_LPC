"""RTC-lost recovery test for LPC845 845-v1.5 (see docs/SYSTEM_DESIGN.md, RTC watch).

  python3 bench/rtc_lost845.py status          one snapshot
  python3 bench/rtc_lost845.py halt [MIN]      stop the DS1338 at 2000-01-01 00:MIN:00 with the halt bit
                                               set (as a lost coin cell leaves it), then watch it recover
  python3 bench/rtc_lost845.py safety          make the clock invalid but running, then bring the
                                               safety-net wake forward to within a second, and watch
  python3 bench/rtc_lost845.py jam [SECONDS]    switch I2C0 off (as the jam seen on 2026-11-07 leaves it
                                               unusable) and check the time never goes to zeros
  python3 bench/rtc_lost845.py watch SECONDS   print changes only

Writes to the RTC by running the firmware's own Set_Time() on the halted core, then restores every
register. Probe on J2 (plugging it in resets the LPC845). Attaching does not reset it.
"""
import os, sys, time, subprocess, logging
from pyocd.core.helpers import ConnectHelper
logging.disable(logging.CRITICAL)

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), '..'))
AXF = ROOT + '/LPC845/LPC845_TWS.axf'
NM = ROOT + '/tools/arm-toolchain/bin/arm-none-eabi-nm'
sym = {}
for line in subprocess.run([NM, AXF], capture_output=True, text=True).stdout.splitlines():
    p = line.split()
    if len(p) == 3:
        sym[p[2]] = int(p[0], 16)
GPIO_PIN0 = 0xA0002100
REGS = ['r0', 'r1', 'r2', 'r3', 'r4', 'r5', 'r6', 'r7', 'r8', 'r9', 'r10', 'r11', 'r12',
        'sp', 'lr', 'pc', 'xpsr']


def snap(t):
    d = bytes(t.read_memory_block8(sym['Present_Date_Time'], 7))
    rails = (t.read32(GPIO_PIN0) >> 26) & 7
    return ('%02d%02d-%02d-%02d %02d:%02d:%02d' % tuple(d),
            'rails=%s' % format(rails, '03b'),
            'wake=%s' % chr(t.read8(sym['wake_reason']) or 32),
            'kicks=%d' % t.read32(sym['rtc_kicks']),
            'i2c_resets=%d' % t.read32(sym['rtc_i2c_resets']) if 'rtc_i2c_resets' in sym else 'i2c_resets=-',
            'safety_wakes=%d' % t.read32(sym['bad_clock_wakes']),
            'day_tips=%d' % t.read32(sym['rain_day_tips']),
            'kept_day_tips=%d' % t.read32(sym['s_rain_keep'] + 8))


def show(t, prev=None):
    s = snap(t)
    key = s[1:]
    if prev is None or key != prev or s[0][-2:] == '00':
        print(time.strftime('%H:%M:%S'), ' '.join(s), flush=True)
    return key


def call_set_time(t, date):
    """date = (century, year, month, date, hour, minute, sec) as stored in Set_Date_Time;
    sec 80 sets the halt bit, since Set_Time writes it as BCD 0x80."""
    for _ in range(50):
        t.halt()
        if (t.read_core_register('xpsr') & 0x3F) == 0:     # thread mode, not inside an interrupt
            break
        t.resume(); time.sleep(0.003)
    else:
        raise SystemExit('could not halt in thread mode')
    saved = {r: t.read_core_register(r) for r in REGS}
    t.write_memory_block8(sym['Set_Date_Time'], list(date))
    ret = saved['pc']
    t.set_breakpoint(ret)
    t.write_core_register('lr', ret | 1)
    t.write_core_register('pc', sym['Set_Time'] & ~1)
    t.resume()
    for _ in range(200):
        time.sleep(0.01)
        if t.get_state().name == 'HALTED':
            break
    t.remove_breakpoint(ret)
    ok = t.read_core_register('pc') == ret
    for r in REGS:
        t.write_core_register(r, saved[r])
    t.resume()
    if not ok:
        raise SystemExit('Set_Time did not return as expected; registers restored')


def wait_asleep(t, secs=300):
    """The test means most with the unit asleep (rails 111), as in the field"""
    end = time.time() + secs
    while ((t.read32(GPIO_PIN0) >> 26) & 7) != 7:
        if time.time() > end:
            raise SystemExit('unit did not go to sleep')
        time.sleep(1)
    print(time.strftime('%H:%M:%S'), 'unit asleep', flush=True)
    time.sleep(3)


def watch(t, secs):
    prev = None
    end = time.time() + secs
    while time.time() < end:
        prev = show(t, prev)
        time.sleep(0.5)


mode = sys.argv[1] if len(sys.argv) > 1 else 'status'
with ConnectHelper.session_with_chosen_probe(
        target_override='lpc845',
        options={'frequency': 1000000, 'connect_mode': 'attach', 'resume_on_disconnect': True}) as s:
    t = s.target
    if mode == 'status':
        show(t)
    elif mode == 'halt':
        minute = int(sys.argv[2]) if len(sys.argv) > 2 else 14
        show(t)
        wait_asleep(t)
        print('stopping RTC at 2000-01-01 00:%02d:00, halt bit set' % minute)
        call_set_time(t, (20, 0, 1, 1, 0, minute, 80))
        watch(t, int(sys.argv[3]) if len(sys.argv) > 3 else 300)
    elif mode == 'safety':
        show(t)
        wait_asleep(t)
        print('setting RTC to 2000-01-01 00:01:00 (running, year invalid); safety wake in ~1 s')
        call_set_time(t, (20, 0, 1, 1, 0, 1, 0))
        time.sleep(0.5)
        t.write16(sym['bad_clock_ticks.0'], 8990)
        watch(t, int(sys.argv[2]) if len(sys.argv) > 2 else 240)
    elif mode == 'jam':
        show(t)
        for n in range(3):
            print('I2C0 master off (forced failure %d)' % (n + 1))
            t.write32(0x40050000, 0)
            end = time.time() + 2
            while time.time() < end:
                d = bytes(t.read_memory_block8(sym['Present_Date_Time'], 7))
                if d[1] < 24:
                    print('FAIL: time read as', d.hex()); break
                time.sleep(0.05)
            show(t)
        watch(t, int(sys.argv[2]) if len(sys.argv) > 2 else 20)
    elif mode == 'watch':
        watch(t, int(sys.argv[2]) if len(sys.argv) > 2 else 120)

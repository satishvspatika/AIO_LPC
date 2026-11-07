import os
ROOT=os.path.abspath(os.path.join(os.path.dirname(__file__),'..'))
import subprocess,logging,time
from pyocd.core.helpers import ConnectHelper
from pyocd.core.target import Target
logging.disable(logging.CRITICAL)
A=ROOT+'/LPC845/LPC845_TWS.axf'
T=ROOT+'/tools/arm-toolchain/bin/arm-none-eabi-nm'
sym={}
for l in subprocess.check_output([T,A]).decode().splitlines():
    p=l.split()
    if len(p)==3: sym[p[2]]=int(p[0],16)
WR=sym['wake_reason']
with ConnectHelper.session_with_chosen_probe(target_override='lpc845',options={'frequency':1000000,'connect_mode':'attach','resume_on_disconnect':True}) as s:
    t=s.target
    def wait_restart(label,limit):
        t0=time.time()
        while time.time()-t0<limit:
            try:
                if t.read8(WR)!=ord('Z'):
                    time.sleep(1.5)
                    print(f"{time.strftime('%H:%M:%S')} {label}: restarted after {time.time()-t0-1.5:.1f}s, rst={t.read32(sym['reset_status'])} wake={chr(t.read8(WR))} tips={t.read32(sym['rain_day_tips'])}",flush=True)
                    return True
            except Exception: pass
            time.sleep(0.1)
        print(f"{label}: NO restart within {limit}s",flush=True); return False
    print(time.strftime('%H:%M:%S'),'start rst',t.read32(sym['reset_status']),'tips',t.read32(sym['rain_day_tips']),flush=True)
    # 1. crash: jump to an address with no code
    t.write8(WR,ord('Z')); t.halt(); t.write_core_register('pc',0xF0000000); t.resume()
    wait_restart('1 crash',20); time.sleep(8)
    # 2. damaged stack edge
    t.write8(WR,ord('Z')); t.write32(sym['end'],0)
    wait_restart('2 stack edge',20); time.sleep(8)
    # 3. main loop hung: park it on a branch-to-self in unused RAM, interrupts still run
    loop=sym['end']+0x400
    t.write8(WR,ord('Z')); t.halt(); t.write16(loop,0xE7FE); t.write_core_register('pc',loop); t.resume()
    wait_restart('3 hang',150)

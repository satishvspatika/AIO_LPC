import os
ROOT=os.path.abspath(os.path.join(os.path.dirname(__file__),'..'))
import subprocess,logging,time,struct
from pyocd.core.helpers import ConnectHelper
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
    def st(): return f"rf={t.read32(sym['rf_val'])} day={t.read32(sym['rain_day_tips'])} restored={t.read8(sym['rain_total_restored'])} rst={t.read32(sym['reset_status'])} trial={t.read8(sym['s_on_trial'])}"
    d=bytes(t.read_memory_block8(0x2400,0x6000)); print('slot A has 845-v1.3:', b'845-v1.3' in d)
    t0=time.time()
    while t.read8(sym['s_on_trial']) and time.time()-t0<120: time.sleep(1)
    print(time.strftime('%H:%M:%S'),'before',st(),flush=True)
    def wait_restart(label,limit):
        t0=time.time()
        while time.time()-t0<limit:
            try:
                if t.read8(WR)!=ord('Z'):
                    time.sleep(1.5); print(f"{time.strftime('%H:%M:%S')} {label}: restarted after {time.time()-t0-1.5:.1f}s, {st()}",flush=True); return
            except Exception: pass
            time.sleep(0.1)
        print(label,'NO restart',flush=True)
    t.write8(WR,ord('Z')); t.halt(); t.write_core_register('pc',0xF0000000); t.resume()
    wait_restart('crash',20); time.sleep(6)
    loop=sym['_noinit_end']+0x400
    t.write8(WR,ord('Z')); t.halt(); t.write16(loop,0xE7FE); t.write_core_register('pc',loop); t.resume()
    wait_restart('hang',150); time.sleep(6)
    print(time.strftime('%H:%M:%S'),'after 6 s',st(),flush=True)

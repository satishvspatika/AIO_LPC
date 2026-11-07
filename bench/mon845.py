import os
ROOT=os.path.abspath(os.path.join(os.path.dirname(__file__),'..'))
import subprocess,time,logging,os,sys
from pyocd.core.helpers import ConnectHelper
logging.disable(logging.CRITICAL)
R=ROOT+'/'
A=R+'LPC845/LPC845_TWS.axf'
T=ROOT+'/tools/arm-toolchain/bin/arm-none-eabi-nm'
sym={}
for l in subprocess.check_output([T,A]).decode().splitlines():
    p=l.split()
    if len(p)==3: sym[p[2]]=int(p[0],16)
dur=float(sys.argv[1]); step=float(sys.argv[2])
with ConnectHelper.session_with_chosen_probe(target_override='lpc845',options={'frequency':1000000,'connect_mode':'attach','resume_on_disconnect':True}) as s:
    t=s.target
    end=time.time()+dur; last=None
    while time.time()<end:
        try:
            n=t.read32(sym['s_next']); a=t.read8(sym['s_active']); st=t.read8(sym['s_staged'])
            rs=t.read32(sym['reset_status']); wr=t.read32(sym['wake_reason'])&0xff
            b=bytes(t.read_memory_block8(sym['gc_recv_buf'],118)).split(b';')[0]
            pin=t.read32(0xA0002100); rails=(pin>>26)&7
            line=f"next={n:#x} active={a} staged={st} rst={rs} wake={chr(wr)} rails={rails:03b} rx={b[:40].decode('ascii','replace')}..{b[-14:].decode('ascii','replace')}"
        except Exception as e:
            line='read failed: '+str(e)[:60]
        key=line.split('rx=')[0]
        if key!=last or n%1024<48:
            print(time.strftime('%H:%M:%S'),line,flush=True); last=key
        time.sleep(step)

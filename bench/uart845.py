import os
ROOT=os.path.abspath(os.path.join(os.path.dirname(__file__),'..'))
import subprocess,time,logging
from pyocd.core.helpers import ConnectHelper
logging.disable(logging.CRITICAL)
R=ROOT+'/'
A=R+'LPC845/LPC845_TWS.axf'
T=ROOT+'/tools/arm-toolchain/bin/arm-none-eabi-nm'
sym={}
for l in subprocess.check_output([T,A]).decode().splitlines():
    p=l.split()
    if len(p)==3: sym[p[2]]=int(p[0],16)
with ConnectHelper.session_with_chosen_probe(target_override='lpc845',options={'frequency':1000000,'connect_mode':'attach','resume_on_disconnect':True}) as s:
    t=s.target
    for k in range(6):
        b=bytes(t.read_memory_block8(sym['gc_recv_buf'],120)).split(b';')[0]
        print(time.strftime('%H:%M:%S'),'next',hex(t.read32(sym['s_next'])),'active',t.read8(sym['s_active']),'state',t.read32(sym['uart_recv_state']),'cnt',t.read32(sym['recv_cnt']),'pAck',t.read32(sym['pAck_xmit']),'hang_ms',t.read32(sym['time_since_reset_interval']),'STAT',hex(t.read32(0x40064008)),'INTEN',hex(t.read32(0x4006400C)),'CFG',hex(t.read32(0x40064000)),'rx',b[:18],len(b))
        time.sleep(3)
    x=bytes(t.read_memory_block8(sym['gc_xmit_buf'],40)); print('tx',x)
    print('WWDT TV',t.read32(0x4000000C),'rails',bin((t.read32(0xA0002100)>>26)&7))

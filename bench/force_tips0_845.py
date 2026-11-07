import os
ROOT=os.path.abspath(os.path.join(os.path.dirname(__file__),'..'))
import subprocess,time,logging
from pyocd.core.helpers import ConnectHelper
logging.disable(logging.CRITICAL)
A=ROOT+'/LPC845/LPC845_TWS.axf'
T=ROOT+'/tools/arm-toolchain/bin/arm-none-eabi-nm'
sym={}
for l in subprocess.check_output([T,A]).decode().splitlines():
    p=l.split()
    if len(p)==3: sym[p[2]]=int(p[0],16)
with ConnectHelper.session_with_chosen_probe(target_override='lpc845',options={'frequency':1000000,'connect_mode':'attach','resume_on_disconnect':True}) as s:
    t=s.target
    a=sym['rain_day_tips']
    tips=t.read32(a); rf=t.read32(sym['rf_val']); rails=(t.read32(0xA0002100)>>26)&7; rst=t.read32(sym['reset_status'])
    print(time.strftime('%H:%M:%S'),'before tips',tips,'rf',rf,'rails',bin(rails),'rst',rst)
    if tips==2 and rf==0 and rails==7:
        t.write32(a,0)
        print('after tips',t.read32(a))
    else:
        print('NOT FORCED: state not as expected')

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
def at(h,m,sec):
    x=time.localtime(); return time.mktime((x.tm_year,x.tm_mon,x.tm_mday,h,m,sec,0,0,-1))
while time.time()<at(9,44,30): time.sleep(0.5)
with ConnectHelper.session_with_chosen_probe(target_override='lpc845',options={'frequency':1000000,'connect_mode':'attach','resume_on_disconnect':True}) as s:
    t=s.target
    rails=lambda:(t.read32(0xA0002100)>>26)&7
    print(time.strftime('%H:%M:%S'),'start rails',format(rails(),'03b'),flush=True)
    if rails()==7:
        end=time.time()+5
        while time.time()<end and rails()==7:
            t.write32(sym['wake_key_pressed'],1)
        print(time.strftime('%H:%M:%S'),'after wake rails',format(rails(),'03b'),flush=True)
    else: print('NOT ASLEEP, wake skipped',flush=True)
    last=None;lasts=None
    while time.time()<at(9,49,15):
        r=rails(); tx=bytes(t.read_memory_block8(sym['gc_xmit_buf'],34)).split(b';')[0].decode('ascii','replace')
        ss=tx[18:32]; v=(r,ss)
        if v!=last:
            print(time.strftime('%H:%M:%S'),'rails',format(r,'03b'),'slot',ss,'tips',t.read32(sym['rain_day_tips']),'rf',t.read32(sym['rf_val']),'rst',t.read32(sym['reset_status']),'wake',chr(t.read8(sym['wake_reason'])),flush=True); last=v
        time.sleep(0.5)
    print(time.strftime('%H:%M:%S'),'end rails',format(rails(),'03b'),flush=True)

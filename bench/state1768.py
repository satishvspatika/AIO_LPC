import os
ROOT=os.path.abspath(os.path.join(os.path.dirname(__file__),'..'))
import subprocess,time,logging
from pyocd.core.helpers import ConnectHelper
logging.disable(logging.CRITICAL)
A=ROOT+'/LPC1768/Debug/LPC1768_AIO.axf'
T=ROOT+'/tools/arm-toolchain/bin/arm-none-eabi-nm'
import os
if not os.path.exists(T): T=ROOT+'/tools/arm-toolchain/bin/arm-none-eabi-nm'
sym={}
for l in subprocess.check_output([T,A]).decode().splitlines():
    p=l.split()
    if len(p)==3: sym[p[2]]=int(p[0],16)
def rs(t,n,size):
    b=bytes(t.read_memory_block8(sym[n],size)); return b.split(b'\0')[0].decode('ascii','replace')
with ConnectHelper.session_with_chosen_probe(target_override='lpc1768',options={'frequency':1000000,'connect_mode':'attach','resume_on_disconnect':True}) as s:
    t=s.target
    print(time.strftime('%H:%M:%S'),'state',t.get_state().name)
    print('u_reply:',rs(t,'s_u_reply',40),'ready',t.read8(sym['s_u_reply_ready']))
    print('last frame:',rs(t,'s_link_last_frame',48))
    a=(t.read32(sym['s_link_frames']),t.read32(sym['s_link_good']),t.read32(sym['s_link_bad']),t.read32(sym['s_link_bytes']))
    time.sleep(4)
    b=(t.read32(sym['s_link_frames']),t.read32(sym['s_link_good']),t.read32(sym['s_link_bad']),t.read32(sym['s_link_bytes']))
    print('frames good bad bytes',a,'->',b)
    print('fw line1:',rs(t,'s_fw_line1',17),'| line2:',rs(t,'s_fw_line2',17),'| substep:',rs(t,'s_status_sms_substep',20))
    print('845 wake',chr(t.read32(sym['s_845_wake'])&0xff) ,'reset status',t.read32(sym['s_845_reset_status']))

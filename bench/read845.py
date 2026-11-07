import os
ROOT=os.path.abspath(os.path.join(os.path.dirname(__file__),'..'))
import subprocess,time,logging,os
from pyocd.core.helpers import ConnectHelper
logging.disable(logging.CRITICAL)
R=ROOT+'/'
A=R+'LPC845/LPC845_TWS.axf'
T=R+'tools/arm-toolchain/bin/arm-none-eabi-nm'
if not os.path.exists(T): T=ROOT+'/tools/arm-toolchain/bin/arm-none-eabi-nm'
sym={}
for l in subprocess.check_output([T,A]).decode().splitlines():
    p=l.split()
    if len(p)==3: sym[p[2]]=int(p[0],16)
with ConnectHelper.session_with_chosen_probe(target_override='lpc845',options={'frequency':1000000,'connect_mode':'attach','resume_on_disconnect':True}) as s:
    t=s.target
    print(time.strftime('%H:%M:%S'),t.get_state().name)
    for n in ['s_active','s_staged','s_on_trial']:
        print(n,t.read8(sym[n]))
    for n in ['s_length','s_crc','s_next']:
        print(n,hex(t.read32(sym[n])))
    b=bytes(t.read_memory_block8(sym['gc_recv_buf'],120)); print('recv:',b.split(b';')[0][:118])
    x=bytes(t.read_memory_block8(sym['gc_xmit_buf'],70)); print('xmit:',x[:70])
    print('recv state',t.read32(sym['uart_recv_state']) ,'recv_cnt',t.read32(sym['recv_cnt']) if 'recv_cnt' in sym else None)
    print('journal first',hex(t.read32(0x1000)))
    img=open(R+'LPC1768/../LPC845/LPC845_TWS.bin','rb').read()
    print('slot A same as LPC845_TWS.bin', bytes(t.read_memory_block8(0x2400,len(img)))==img)
    import sys
    if len(sys.argv)<2: sys.exit(0)
    g=open(sys.argv[1],'rb').read()  # optional: the image expected in slot B
    fl=bytes(t.read_memory_block8(0x8400,len(g)))
    n=next((i for i in range(len(g)) if fl[i]!=g[i]),None)
    print('slot B vs given image: first difference at',n,'of',len(g),' blank from', next((i for i in range(len(g)) if all(c==0xFF for c in fl[i:i+64])),None))
    print('USART0 STAT',hex(t.read32(0x40064008)),'INTENSET',hex(t.read32(0x4006400C)))

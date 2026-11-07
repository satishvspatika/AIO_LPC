import os
ROOT=os.path.abspath(os.path.join(os.path.dirname(__file__),'..'))
import sys,time,logging
from pyocd.core.helpers import ConnectHelper
logging.disable(logging.CRITICAL)
R=ROOT+'/LPC845/'
with ConnectHelper.session_with_chosen_probe(target_override='lpc845',options={'frequency':1000000,'connect_mode':'attach','resume_on_disconnect':True}) as s:
    t=s.target
    b=open(R+'LPC845_BOOT.bin','rb').read(); a=open(R+'LPC845_TWS.bin','rb').read()
    fb=bytes(t.read_memory_block8(0,len(b))); fa=bytes(t.read_memory_block8(0x2400,len(a)))
    print('boot match',fb==b,'app match',fa==a)
    print('journal first word',hex(t.read32(0x1000)),'slot B first',hex(t.read32(0x8400)))
    print('WWDT MOD',hex(t.read32(0x40000000)),'TC',t.read32(0x40000004),'TV',t.read32(0x4000000C))
    v1=t.read32(0x4000000C); time.sleep(5); v2=t.read32(0x4000000C)
    print('WWDT TV',v1,v2,'feeds?',v2>=v1-5*3000 )
    print('VTOR',hex(t.read32(0xE000ED08)),'GPIO PIN0',hex(t.read32(0xA0002100)),'DIR0',hex(t.read32(0xA0002000)),'SYSRST',hex(t.read32(0x40048038)))
    print('pc',hex(t.read_core_register('pc')) if t.get_state().name=='HALTED' else t.get_state().name)

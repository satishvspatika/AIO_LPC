import os
ROOT=os.path.abspath(os.path.join(os.path.dirname(__file__),'..'))
import time,sys,datetime
from pyocd.core.helpers import ConnectHelper
DIR=0x2009C000; PIN=0x2009C014; SET=0x2009C018; CLR=0x2009C01C; PINSEL0=0x4002C000; RSID=0x400FC180
# Addresses below are from a v6.9.x LPC1768 map: re-read them from LPC1768/Debug/LPC1768_AIO.map before use
TICK=0x10003b24; LINK=0x100002c0; BAD=0x100002b4; INFO=0x000345e0; B=1<<5
ts=lambda: datetime.datetime.now().strftime('%H:%M:%S')
now=datetime.datetime.now(); m=now.minute%15
print('time',ts())
if m>=13 or m<4: print('too close to a slot, not running'); sys.exit(3)
s=ConnectHelper.session_with_chosen_probe(target_override='lpc1768',connect_mode='attach',frequency=1000000,options={'resume_on_disconnect':True})
s.open(); t=s.board.target
print('version',bytes(t.read_memory_block8(INFO,32)).split(b'\0')[0:6])
print('PINSEL0',hex(t.read32(PINSEL0)),'P0 DIR',hex(t.read32(DIR)),'P0.5 level',t.read32(PIN)>>5&1,'RSID',hex(t.read32(RSID)))
t0,l0=t.read32(TICK),t.read32(LINK); time.sleep(3); t1,l1=t.read32(TICK),t.read32(LINK)
print(f'before: tick {t0}->{t1} in 3s, frames {l0}->{l1}, damaged {t.read32(BAD)}',flush=True)
t.write32(CLR,B); t.write32(DIR,t.read32(DIR)|B)
print(ts(),'RST_845 driven low, level',t.read32(PIN)>>5&1,flush=True)
lost=False
try:
    for i in range(6):
        time.sleep(3)
        print(ts(),f'held {(i+1)*3:2}s tick {t.read32(TICK)} frames {t.read32(LINK)} damaged {t.read32(BAD)} RSID {hex(t.read32(RSID))}',flush=True)
except Exception as e:
    lost=True; print(ts(),'CONNECTION LOST:',str(e)[:80],flush=True)
if not lost:
    t.write32(DIR,t.read32(DIR)&~B)
    print(ts(),'released, level',t.read32(PIN)>>5&1,flush=True)
    try:
        for i in range(12):
            time.sleep(1)
            print(ts(),f'after {i+1:2}s: tick {t.read32(TICK)} frames {t.read32(LINK)} damaged {t.read32(BAD)} RSID {hex(t.read32(RSID))}',flush=True)
    except Exception as e:
        print(ts(),'CONNECTION LOST AFTER RELEASE:',str(e)[:60],flush=True)
try: s.close()
except Exception: pass

import os
ROOT=os.path.abspath(os.path.join(os.path.dirname(__file__),'..'))
import time,sys,datetime
from pyocd.core.helpers import ConnectHelper
CLK=0x40048080; DIR=0xA0002000; PIN=0xA0002100
R=[(26,'SHDN_3V8 modem'),(27,'SHDN_3V3 LPC1768'),(28,'SHDN_5V LCD')]
def show(t,tag):
    d=t.read32(DIR);p=t.read32(PIN)
    s=' '.join(f"P{n}:{'out' if d>>n&1 else 'in '}={'H' if p>>n&1 else 'L'}" for n,_ in R)
    print(f"{datetime.datetime.now():%H:%M:%S} {tag:10} {s}",flush=True)
    return d,p
now=datetime.datetime.now(); m=now.minute%15
if m>=12 or m<4:
    print("too close to a slot, not running",now.strftime('%H:%M:%S')); sys.exit(3)
with ConnectHelper.session_with_chosen_probe(target_override='lpc845',connect_mode='attach',frequency=1000000,options={'resume_on_disconnect':True}) as s:
    t=s.board.target
    clk=t.read32(CLK); print('clock reg',hex(clk))
    d,p=show(t,'before')
    if p>>27&1:
        print('RAIL ALREADY OFF (P27 high): unit asleep, press DSP first. Not running.'); sys.exit(4)
    t.reset_and_halt()
    print('state after reset-halt',t.get_state().name,'pc',hex(t.read_core_register('pc')),flush=True)
    c=t.read32(CLK); print('clock reg after reset',hex(c))
    t.write32(CLK,c|(1<<6))
    hi=0
    for i in range(10):
        d,p=show(t,f"held {i*3}s")
        hi=hi+1 if p>>27&1 else 0
        if hi>=2:
            print('P27 HIGH while held: releasing early'); break
        time.sleep(3)
    t.reset()
    print('released',datetime.datetime.now().strftime('%H:%M:%S'),flush=True)
    time.sleep(3)
    t.write32(CLK,t.read32(CLK)|(1<<6)) if False else None
    show(t,'after')

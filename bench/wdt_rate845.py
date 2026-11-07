import os
ROOT=os.path.abspath(os.path.join(os.path.dirname(__file__),'..'))
import time,logging
from pyocd.core.helpers import ConnectHelper
logging.disable(logging.CRITICAL)
with ConnectHelper.session_with_chosen_probe(target_override='lpc845',options={'frequency':1000000,'connect_mode':'attach','resume_on_disconnect':True}) as s:
    t=s.target
    t.halt()
    a=t.read32(0x4000000C); ta=time.time()
    time.sleep(4)
    b=t.read32(0x4000000C); tb=time.time()
    rate=(a-b)/(tb-ta)
    print('TV',a,b,'ticks/s',round(rate,1),'timeout min',round(703125/rate/60,2) if rate>0 else None)
    t.resume()
    time.sleep(0.5)
    print('after resume TV',t.read32(0x4000000C),'state',t.get_state().name)

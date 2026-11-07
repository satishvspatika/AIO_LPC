import os
ROOT=os.path.abspath(os.path.join(os.path.dirname(__file__),'..'))
import subprocess,time,logging,sys
from pyocd.core.helpers import ConnectHelper
logging.disable(logging.CRITICAL)
A=ROOT+'/LPC1768/Debug/LPC1768_AIO.axf'
T=ROOT+'/tools/arm-toolchain/bin/arm-none-eabi-nm'
sym={}
for l in subprocess.check_output([T,A]).decode().splitlines():
    p=l.split()
    if len(p)==3: sym[p[2]]=int(p[0],16)
def rs(t,n,size):
    return bytes(t.read_memory_block8(sym[n],size)).split(b'\0')[0].decode('ascii','replace')
end=time.time()+float(sys.argv[1]); last=None
while time.time()<end:
  try:
    with ConnectHelper.session_with_chosen_probe(target_override='lpc1768',options={'frequency':1000000,'connect_mode':'attach','resume_on_disconnect':True}) as s:
        t=s.target
        while time.time()<end:
            d=[t.read32(sym['g_ctrl_dbg']+4*i) for i in range(6)]
            line=f"off={d[0]:#x} try={d[1]} replies={d[2]} timeouts={d[3]} next={d[4]:#x} e={d[5]} other={t.read32(sym['g_ctrl_dbg_other'])} stepcalls={t.read32(sym['g_ctrl_dbg_stolen'])} rep={rs(t,'g_ctrl_dbg_reply',40)} u={rs(t,'s_u_reply',40)} rdy={t.read8(sym['s_u_reply_ready'])} frames={t.read32(sym['s_link_frames'])} good={t.read32(sym['s_link_good'])} bad={t.read32(sym['s_link_bad'])} sub={rs(t,'s_status_sms_substep',20)} ui={rs(t,'s_fw_line1',17)}|{rs(t,'s_fw_line2',17)}"
            k=(d[1],d[3],t.read32(sym['g_ctrl_dbg_other']),rs(t,'s_fw_line2',17))
            if k!=last or d[1]>0:
                print(time.strftime('%H:%M:%S'),line,flush=True); last=k
            time.sleep(0.4)
  except Exception as e:
    print(time.strftime('%H:%M:%S'),'no target:',str(e)[:60],flush=True); time.sleep(1)

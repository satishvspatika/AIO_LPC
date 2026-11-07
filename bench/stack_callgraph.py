import os
ROOT=os.path.abspath(os.path.join(os.path.dirname(__file__),'..'))
import re,sys,glob,subprocess,os
ci_dir,axf,objdump=sys.argv[1:4]
frames={};edges={}
for f in glob.glob(ci_dir+'/*.ci'):
    t=open(f,errors='ignore').read()
    for m in re.finditer(r'node: \{ title: "([^"]+)" label: "([^"]*)"',t):
        title,label=m.groups()
        name=title.split(':')[-1]
        s=re.search(r'(\d+) bytes \(static\)',label)
        frames[name]=max(frames.get(name,0),int(s.group(1)) if s else 0)
    for m in re.finditer(r'edge: \{ sourcename: "([^"]+)" targetname: "([^"]+)"',t):
        a,b=m.group(1).split(':')[-1],m.group(2).split(':')[-1]
        edges.setdefault(a,set()).add(b)
# C library frames and calls, read from the linked binary
out=subprocess.run([objdump,'-d',axf],capture_output=True,text=True).stdout
cur=None;size={};calls={}
for line in out.splitlines():
    m=re.match(r'^[0-9a-f]+ <([^>]+)>:',line)
    if m: cur=m.group(1); size.setdefault(cur,0); calls.setdefault(cur,set()); continue
    if not cur: continue
    m=re.search(r'push\s+\{([^}]*)\}',line) or re.search(r'stmdb\s+sp!,\s*\{([^}]*)\}',line)
    if m: size[cur]+=4*len([x for x in m.group(1).split(',')])+ (0)
    m=re.search(r'sub\s+sp,\s*#(\d+)',line) or re.search(r'sub\.w\s+sp,\s*sp,\s*#(\d+)',line)
    if m: size[cur]+=int(m.group(1))
    m=re.search(r'\bbl\w*\s+[0-9a-f]+ <([^>+]+)',line)
    if m: calls[cur].add(m.group(1))
    m=re.search(r'\bb(?:\.w)?\s+[0-9a-f]+ <([^>+]+)>',line)
    if m and m.group(1)!=cur: calls[cur].add(m.group(1))
for n in size:
    if n not in frames: frames[n]=size[n]
    edges.setdefault(n,set()).update(calls[n]) if n not in edges else None
alias={'snprintf':'sniprintf','vsnprintf':'vsniprintf','sscanf':'siscanf','printf':'iprintf','sprintf':'siprintf'}
for a,b in alias.items():
    if b in size:
        frames[a]=frames.get(b,size[b]) or size[b]; edges[a]=set(edges.get(b,set()))|calls.get(b,set())
for n in ('sniprintf','_svfiprintf_r','siscanf','__ssvfiscanf_r'):
    if n in size:
        frames[n]=max(frames.get(n,0),size[n]); edges.setdefault(n,set()).update(calls.get(n,set()))
sys.setrecursionlimit(10000)
memo={}
def depth(n,path=()):
    if n in path: return (0,[])
    if n in memo: return memo[n]
    best=(0,[])
    for c in edges.get(n,()):
        d=depth(c,path+(n,))
        if d[0]>best[0]: best=d
    r=(frames.get(n,0)+best[0],[n]+best[1])
    memo[n]=r
    return r
limits={'vFwUpdate_Task':768,'vGps_Task':768,'vTestSend_Task':768,'vStatusSms_Task':768,'vMain_Task':960,'vModem_Task':768,'vUi_Task':512,'vDisk_Task':512,'vKbd_Task':160}
print('%-18s %6s %6s %6s'%('task','need','have','spare'))
for t,w in limits.items():
    memo.clear(); d=depth(t)
    have=w*4; need=d[0]+32+32  # exception frame + context save margin
    print('%-18s %6d %6d %6d %s'%(t,need,have,have-need,'<<< OVERFLOW' if need>have else ''))
    print('    '+' > '.join('%s(%d)'%(x,frames.get(x,0)) for x in d[1][:12]))

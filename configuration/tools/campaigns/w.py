"""w.py <regex> <timeout_s> [log=hub|node] [--from N lines back start offset]  : wait until regex appears in log lines newer than script start (or print matches)"""
import re,sys,time,os
import glob; D=sorted(glob.glob(r'c:/Development/esphome/configuration/logs/*_review_rollout4/hub.log'))[-1].rsplit('hub.log',1)[0]
rx=re.compile(sys.argv[1]); to=float(sys.argv[2]); log=sys.argv[3] if len(sys.argv)>3 else 'hub'
A=re.compile(r'\x1b\[[0-9;]*m')
f=os.path.join(D,log+'.log')
def lines(): return [A.sub('',l) for l in open(f,'rb').read().decode('utf-8','replace').splitlines()]
n=len(lines()); t0=time.time()
while time.time()-t0<to:
    time.sleep(3)
    L=lines()[n:]
    m=[l for l in L if rx.search(l)]
    if m:
        print('\n'.join(m[:8])); sys.exit(0)
print('TIMEOUT'); sys.exit(1)

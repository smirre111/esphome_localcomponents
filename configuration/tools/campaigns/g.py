"""g.py <regex> [log=hub] [last N=15] [since HH:MM:SS]"""
import re,sys,os
import glob; D=sorted(glob.glob(r'c:/Development/esphome/configuration/logs/*_review_rollout4/hub.log'))[-1].rsplit('hub.log',1)[0]
rx=re.compile(sys.argv[1]); log=sys.argv[2] if len(sys.argv)>2 else 'hub'; n=int(sys.argv[3]) if len(sys.argv)>3 else 15; since=sys.argv[4] if len(sys.argv)>4 else ''
A=re.compile(r'\x1b\[[0-9;]*m')
L=[A.sub('',l) for l in open(os.path.join(D,log+'.log'),'rb').read().decode('utf-8','replace').splitlines()]
L=[l for l in L if rx.search(l) and l[:8]>=since]
print('\n'.join(L[-n:]))

"""lg.py <since HH:MM:SS>: per node login stats after a hub boot"""
import re,sys,glob,os
D=sorted(glob.glob(r'c:/Development/esphome/configuration/logs/*_review_rollout4/hub.log'))[-1]
A=re.compile(r'\x1b\[[0-9;]*m'); since=sys.argv[1]
L=[A.sub('',l) for l in open(D,'rb').read().decode('utf-8','replace').splitlines()]
L=[l for l in L if l[:8]>=since]
def secs(t): h,m,s=t.split(':'); return int(h)*3600+int(m)*60+float(s)
boot=[l for l in L if 'rst:0x' in l]
b=secs(boot[0][:12]) if boot else None
print('boot', boot[0][:12] if boot else None)
for n in ('1','2'):
    nm='RollladenWohnzimmer'+n
    sent=[l for l in L if nm in l and 'LoginMsg sent' in l]
    ack=[l for l in L if nm in l and 'Login acknowledged' in l]
    onb=[l for l in L if nm in l and 'Onboarding done' in l]
    rs=[l for l in L if nm in l and 're-sending its LOGIN now' in l]
    cfg=[l for l in L if nm in l and ('ClientConfig acknowledged' in l or 'Config push' in l or 'in sync' in l or 'Config hash changed' in l or 'already in sync' in l)]
    first=secs(sent[0][:12]) if sent else None
    a=secs(ack[0][:12]) if ack else None
    print(nm,'sends before ack:',len([s for s in sent if not a or secs(s[:12])<=a]),'first send +%.1fs'%(first-b) if first and b else '', 'ack +%.1fs'%(a-b) if a and b else 'NO ACK', 'onboarding', onb[0][:12]+' (+%.1fs)'%(secs(onb[0][:12])-b) if onb and b else None, onb[0].split('Onboarding done')[1][:30] if onb else '', 'LOGINre-sent lines:',len(rs))
    for c in cfg[:6]: print('    ',c[:12],c.split(']',2)[-1][:110])

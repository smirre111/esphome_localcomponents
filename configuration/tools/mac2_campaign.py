"""MAC-2 campaign: Mode A and Mode B, each with MAC-1 + MAC-2 both ON, 900 s,
plus an encrypted-ping turnaround run. Presses each button ONCE, waits for the
REPORT in the captured hub log.

    python tools/mac2_campaign.py <session_dir>
"""
import os, re, subprocess, sys, time
sess = sys.argv[1]; hub_log = os.path.join(sess, "hub.log")
os.makedirs(os.path.join(sess, "reports"), exist_ok=True)
ANSI = re.compile(r"\x1b\[[0-9;]*m")
def ctl(*a): return subprocess.run([sys.executable, "tools/hubctl.py", *a], capture_output=True, text=True).stdout
def note(m):
    subprocess.run([sys.executable, "tools/benchlog.py", "--note", m, "--dir", sess]); print(time.strftime("%H:%M:%S"), m, flush=True)
def lines(): return [ANSI.sub("", l) for l in open(hub_log, "rb").read().decode("utf-8", "replace").splitlines()]
def run(step, button, timeout_s=1150, ping_after=None):
    start = len(lines())
    note(f"PRESS once: {step} = {button}"); print(ctl("press", button), flush=True)
    if ping_after:
        time.sleep(25)
        note(f"PRESS once: {ping_after} (inside {step})"); print(ctl("press", ping_after), flush=True)
    t0 = time.time()
    while time.time() - t0 < timeout_s:
        time.sleep(10)
        new = lines()[start:]
        if any("ModeTest REPORT" in l for l in new):
            time.sleep(15); new = lines()[start:]
            open(os.path.join(sess, "reports", f"{step}.txt"), "w", encoding="utf-8").write("\n".join(new))
            note(f"REPORT captured for {step}"); return True
    note(f"TIMEOUT {step}"); return False

A2 = "Mode Test A — long, MAC-1 + MAC-2 (node 2)"
B2 = "Mode Test B — long, MAC-1 + MAC-2 (node 2)"
PING2 = "MAC Ping Start long, MAC-2 (node 2)"
TM = "Timed Mode (Mode B) — node 2"

ok = run("T2_turnaround", A2, ping_after=PING2)
time.sleep(60)
ok = run("A2_mac1mac2", A2) and ok
time.sleep(60)
note("Timed Mode ON, settle 120 s"); print(ctl("switch", TM, "on"), flush=True); time.sleep(120)
ok = run("B2_mac1mac2", B2) and ok
note("MAC-2 campaign done" if ok else "MAC-2 campaign done WITH TIMEOUTS")

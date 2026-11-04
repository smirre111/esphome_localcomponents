"""v1 cutover campaign (node 2 only). Node 1 Timed Mode OFF for the whole campaign, restored ON at the end.
T1/T2 Mode A turnaround with node 2 Timed Mode OFF; then node 2 Timed Mode ON, wait RE-ESTABLISHED,
quiet 60 s, B1 (MAC-1 counter) and B2 (MAC-1+MAC-2) 900 s. Steps end on node 2's REPORT with the step's flags.
Run from C:\\Development\\esphome\\configuration:  python -u <this> <session_dir>"""
import os, re, subprocess, sys, time
sess = sys.argv[1]; hub_log = os.path.join(sess, "hub.log")
os.makedirs(os.path.join(sess, "reports"), exist_ok=True)
ANSI = re.compile(r"\x1b\[[0-9;]*m")
def ctl(*a): return subprocess.run([sys.executable, "tools/hubctl.py", *a], capture_output=True, text=True).stdout
def note(m):
    subprocess.run([sys.executable, "tools/benchlog.py", "--note", m, "--dir", sess]); print(time.strftime("%H:%M:%S"), m, flush=True)
def lines(): return [ANSI.sub("", l) for l in open(hub_log, "rb").read().decode("utf-8", "replace").splitlines()]
def wait_for(rx, timeout_s, start):
    t0 = time.time()
    while time.time() - t0 < timeout_s:
        time.sleep(5)
        if any(re.search(rx, l) for l in lines()[start:]): return True
    return False
def run(step, button, flags, ping=None):
    start = len(lines())
    note(f"PRESS once: {step} = {button}"); print(ctl("press", button), flush=True)
    if ping:
        time.sleep(25); note(f"PRESS once: {ping} (inside {step})"); print(ctl("press", ping), flush=True)
    ok = wait_for(r"\[RollladenWohnzimmer2\] ModeTest REPORT: " + flags, 1150, start)
    time.sleep(20)
    open(os.path.join(sess, "reports", f"{step}.txt"), "w", encoding="utf-8").write("\n".join(lines()[start:]))
    note(("REPORT captured for " if ok else "TIMEOUT ") + step); return ok
TM1 = "Timed Mode (Mode B) — Node 1"; TM2 = "Timed Mode (Mode B) — Node 2"
boot = max(i for i, l in enumerate(lines()) if "rst:0x" in l)
wait_for(r"Wohnzimmer2\] Onboarding done", 300, boot)
note("node 1 + node 2 Timed Mode OFF"); print(ctl("switch", TM1, "off"), ctl("switch", TM2, "off"), flush=True)
time.sleep(45)
run("T1_counter_on", "Mode Test A — long, MAC-1 counter (node 2)", r"mode=1 prod=1 counter=1 crypto=0", "MAC Ping Start long (node 2)")
time.sleep(60)
run("T2_turnaround", "Mode Test A — long, MAC-1 + MAC-2 (node 2)", r"mode=1 prod=1 counter=1 crypto=1", "MAC Ping Start long, MAC-2 (node 2)")
time.sleep(60)
s = len(lines()); note("node 2 Timed Mode ON, waiting for RE-ESTABLISHED"); print(ctl("switch", TM2, "on"), flush=True)
note(f"RE-ESTABLISHED: {wait_for(r'Wohnzimmer2. Mode B RE-ESTABLISHED', 480, s)}"); time.sleep(60)
run("B2_mac1mac2", "Mode Test B — long, MAC-1 + MAC-2 (node 2)", r"mode=2 prod=1 counter=1 crypto=1")
note("restoring node 1 Timed Mode ON"); print(ctl("switch", TM1, "on"), flush=True)
note("cutover campaign done")

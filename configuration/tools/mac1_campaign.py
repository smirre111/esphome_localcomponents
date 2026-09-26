"""MAC-1 campaign driver: Mode A then Mode B, each counter-OFF then counter-ON, 900 s.

Presses each button ONCE (a double press restarts the hub's mark stream and voids the
run), waits for that run's `ModeTest REPORT` to appear in the captured hub log, saves
the raw report lines to <session>/reports/<step>.txt and moves on.  Everything is read
from the capture files, never from a live pipe.

    python tools/mac1_campaign.py <session_dir>
"""
import glob, os, re, subprocess, sys, time

sess = sys.argv[1]
hub_log = os.path.join(sess, "hub.log")
os.makedirs(os.path.join(sess, "reports"), exist_ok=True)
ANSI = re.compile(r"\x1b\[[0-9;]*m")

def ctl(*a):
    return subprocess.run([sys.executable, "tools/hubctl.py", *a], capture_output=True, text=True).stdout

def note(msg):
    subprocess.run([sys.executable, "tools/benchlog.py", "--note", msg, "--dir", sess])
    print(time.strftime("%H:%M:%S"), msg, flush=True)

def lines_now():
    return open(hub_log, "rb").read().decode("utf-8", "replace").splitlines()

def run(step, button, timeout_s=1150):
    start = len(lines_now())
    note(f"PRESS once: {step} = {button}")
    print(ctl("press", button), flush=True)
    t0 = time.time()
    while time.time() - t0 < timeout_s:
        time.sleep(10)
        new = [ANSI.sub("", l) for l in lines_now()[start:]]
        rep = [l for l in new if "ModeTest REPORT" in l or "REPORT" in l and "mode" in l.lower()]
        if rep:
            time.sleep(15)   # the report is followed by more lines (oneShot, phase, ...)
            new = [ANSI.sub("", l) for l in lines_now()[start:]]
            keep = [l for l in new if re.search(r"ModeTest|REPORT|oneShot|Wake|mark:|refus|DUP|windows", l)]
            open(os.path.join(sess, "reports", f"{step}.txt"), "w", encoding="utf-8").write("\n".join(keep))
            note(f"REPORT captured for {step}")
            return True
    note(f"TIMEOUT waiting for {step}")
    return False

A0 = "Mode Test A — long, MAC-0 baseline (node 2)"
A1 = "Mode Test A — long, MAC-1 counter (node 2)"
B0 = "Mode Test B — long, production profile (node 2)"
B1 = "Mode Test B — long, MAC-1 counter (node 2)"
TM = "Timed Mode (Mode B) — node 2"

ok = run("A0_mac0", A0)
time.sleep(60)
ok = run("A1_mac1", A1) and ok
time.sleep(60)
note("Timed Mode ON, settle 120 s for the grid ack")
print(ctl("switch", TM, "on"), flush=True)
time.sleep(120)
ok = run("B0_mac0", B0) and ok
time.sleep(60)
ok = run("B1_mac1", B1) and ok
note("campaign done, all reports captured" if ok else "campaign done WITH TIMEOUTS")

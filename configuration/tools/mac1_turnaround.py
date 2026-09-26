"""MAC-1 turnaround campaign: the SAME Mode A ModeTest with a MAC ping running inside it,
counter OFF then counter ON. The node records RxDone -> echo TX-fire only while a ModeTest
is running, and answers a plaintext ping only then (fw >= 1.1.5).

    python tools/mac1_turnaround.py <session_dir>
"""
import os, re, subprocess, sys, time
sess = sys.argv[1]; hub_log = os.path.join(sess, "hub.log")
os.makedirs(os.path.join(sess, "reports"), exist_ok=True)
ANSI = re.compile(r"\x1b\[[0-9;]*m")
def ctl(*a): return subprocess.run([sys.executable, "tools/hubctl.py", *a], capture_output=True, text=True).stdout
def note(m):
    subprocess.run([sys.executable, "tools/benchlog.py", "--note", m, "--dir", sess]); print(time.strftime("%H:%M:%S"), m, flush=True)
def lines(): return [ANSI.sub("", l) for l in open(hub_log, "rb").read().decode("utf-8", "replace").splitlines()]
def run(step, test_button, timeout_s=1150):
    start = len(lines())
    note(f"PRESS once: {step} = {test_button}"); print(ctl("press", test_button), flush=True)
    time.sleep(25)
    note(f"PRESS once: MAC ping long inside {step}"); print(ctl("press", "MAC Ping Start long (node 2)"), flush=True)
    t0 = time.time()
    while time.time() - t0 < timeout_s:
        time.sleep(10)
        new = lines()[start:]
        if any("ModeTest REPORT" in l for l in new):
            time.sleep(15); new = lines()[start:]
            open(os.path.join(sess, "reports", f"{step}.txt"), "w", encoding="utf-8").write("\n".join(new))
            note(f"REPORT captured for {step}"); return True
    note(f"TIMEOUT {step}"); return False
A0 = "Mode Test A — long, MAC-0 baseline (node 2)"
A1 = "Mode Test A — long, MAC-1 counter (node 2)"
ok = run("T0_counter_off", A0); time.sleep(60)
ok = run("T1_counter_on", A1) and ok
note("turnaround campaign done" if ok else "turnaround campaign done WITH TIMEOUTS")

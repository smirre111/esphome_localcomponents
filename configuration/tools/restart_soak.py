#!/usr/bin/env python
"""Hub-restart soak: restart the hub N times and measure how long each node takes
to get its session back. The check for docs/project-state.md open issue 1 (slow
plain LOGIN after a hub restart), which so far rests on ONE restart read out of
a serial log by hand.

    python tools/restart_soak.py --rounds 10                 # live, against the hub's web server
    python tools/restart_soak.py --analyze logs/<dir>/hub.log  # offline, from a captured hub log
    python tools/restart_soak.py --selftest                  # no hardware: checks the parsing

Live mode needs the session-diagnostics entities (lora_client/SessionDiag.h) in
the flashed yaml: per node `Session State`, `Session Recovery Time`, `Login
Messages`, `Stale Periods`. Each round: press 'Hub Restart', wait for the web
server to answer again, then poll until every node reads `confirmed` (or the
timeout). A node that is asleep is skipped for that round, not counted as slow.

Run tools/benchlog.py alongside so the serial logs of the same rounds exist:
the hub logs `Session recovered in N s (K LoginMsg)` and, when it goes wrong,
`SESSION STALE` followed by the node's session event log.

Output: one line per node and round, a summary per node, and `--csv FILE`.
PASS line: every recovery <= --pass-s (default 30 s) and no stale period.

DO NOT run this at night around the 23:00 SLEEP, and do not move blinds during
it (a node is deaf while its motor runs).
"""
import argparse, csv, re, statistics, sys, time

NODES_DEFAULT = ["RollladenWohnzimmer1", "RollladenWohnzimmer2"]


# ---- pure helpers (covered by --selftest) -------------------------------------

def norm(s):
    """Entity names reach us as 'sensor-rollladenwohnzimmer1_session_state' or
    'RollladenWohnzimmer1 Session State' depending on the ESPHome version."""
    return re.sub(r"[^a-z0-9]", "", str(s).lower())


def find_state(snapshot, node, entity):
    """The state of `node`'s `entity` in a hubctl snapshot, or None."""
    want = norm(node) + norm(entity)
    for key, value in snapshot.items():
        if norm(key).endswith(want):
            return value
    return None


def to_float(v):
    try:
        m = re.match(r"\s*(-?\d+(?:\.\d+)?)", str(v))
        return float(m.group(1)) if m else None
    except (TypeError, ValueError):
        return None


def node_view(snapshot, node):
    """What one node's diagnostics say, from one snapshot."""
    return {
        "state": find_state(snapshot, node, "Session State"),
        "recovery_s": to_float(find_state(snapshot, node, "Session Recovery Time")),
        "logins": to_float(find_state(snapshot, node, "Login Messages")),
        "stale_periods": to_float(find_state(snapshot, node, "Stale Periods")),
    }


RECOVERED = re.compile(r"\[(?P<node>[^\]]+)\] Session recovered in (?P<s>\d+)\.(?P<t>\d) s \((?P<n>\d+) LoginMsg\)")
STALE = re.compile(r"\[(?P<node>[^\]]+)\] SESSION STALE")
NODE_REPORT = re.compile(r"\[(?P<node>[^\]]+)\] Node reports its session path: (?P<reg>\d+) REGISTER sent, "
                         r"LOGIN accepted after (?P<ms>\d+) ms \(copy (?P<copy>\d+) of the burst\)")
SAVE_FAIL = re.compile(r"\[(?P<node>[^\]]+)\] NODE COULD NOT SAVE ITS register_seq")


def analyze_lines(lines):
    """Per node: recovery times (s), their LoginMsg counts, stale periods, node reports."""
    out = {}

    def rec(node):
        return out.setdefault(node, {"recoveries": [], "logins": [], "stale": 0,
                                     "node_reports": [], "save_fails": 0})

    for line in lines:
        m = RECOVERED.search(line)
        if m:
            r = rec(m.group("node"))
            r["recoveries"].append(int(m.group("s")) + int(m.group("t")) / 10.0)
            r["logins"].append(int(m.group("n")))
            continue
        m = STALE.search(line)
        if m:
            rec(m.group("node"))["stale"] += 1
            continue
        m = NODE_REPORT.search(line)
        if m:
            rec(m.group("node"))["node_reports"].append(
                {"registers": int(m.group("reg")), "wait_ms": int(m.group("ms")), "copy": int(m.group("copy"))})
            continue
        m = SAVE_FAIL.search(line)
        if m:
            rec(m.group("node"))["save_fails"] += 1
    return out


def summarize(times, pass_s):
    """(n, min, median, max, slow) of a list of recovery times."""
    if not times:
        return (0, None, None, None, 0)
    return (len(times), min(times), statistics.median(times), max(times),
            sum(1 for t in times if t > pass_s))


def verdict(per_node, pass_s):
    """PASS only with at least one recovery per node, none slow, no stale period."""
    ok = bool(per_node)
    for r in per_node.values():
        n, _, _, _, slow = summarize(r["recoveries"], pass_s)
        if n == 0 or slow or r["stale"] or r.get("save_fails"):
            ok = False
    return ok


def print_summary(per_node, pass_s):
    for node in sorted(per_node):
        r = per_node[node]
        n, lo, med, hi, slow = summarize(r["recoveries"], pass_s)
        if n == 0:
            print(f"{node}: no recovery measured, {r['stale']} stale period(s)")
            continue
        logins = r["logins"]
        print(f"{node}: {n} recoveries, min {lo:.1f} s, median {med:.1f} s, max {hi:.1f} s, "
              f"{slow} over {pass_s:g} s, {r['stale']} stale period(s), "
              f"LoginMsg per recovery max {max(logins) if logins else '-'}"
              + (f", register_seq save failures {r['save_fails']}" if r.get("save_fails") else ""))
        for rep in r.get("node_reports", []):
            print(f"    node's own account: {rep['registers']} REGISTER, LOGIN after {rep['wait_ms']} ms, "
                  f"copy {rep['copy']}")
    print("RESULT:", "PASS" if verdict(per_node, pass_s) else "FAIL")


# ---- live mode ------------------------------------------------------------------

def live(args):
    sys.path.insert(0, __file__.rsplit("restart_soak.py", 1)[0])
    import hubctl

    def snap():
        try:
            return hubctl.snapshot(seconds=4)
        except Exception as e:  # the hub is rebooting: no web server yet
            return None

    first = snap()
    if not first:
        sys.exit("the hub's web server does not answer (hubctl.HUB)")
    for node in args.nodes:
        if node_view(first, node)["state"] is None:
            sys.exit(f"no 'Session State' entity for {node}: flash a yaml with the session diagnostics first")

    per_node = {n: {"recoveries": [], "logins": [], "stale": 0, "node_reports": [], "save_fails": 0}
                for n in args.nodes}
    rows = []
    for rnd in range(1, args.rounds + 1):
        before = snap() or {}
        asleep = {n for n in args.nodes if node_view(before, n)["state"] == "asleep"}
        print(f"--- round {rnd}/{args.rounds}: restarting the hub" +
              (f" (asleep, skipped: {', '.join(sorted(asleep))})" if asleep else ""))
        try:
            hubctl.post("button", "Hub Restart", "press")
        except Exception as e:
            print(f"    restart request: {e} (a dropped connection is normal here)")
        t0 = time.time()
        time.sleep(3)
        pending = [n for n in args.nodes if n not in asleep]
        seen_stale = {n: 0.0 for n in args.nodes}
        while pending and time.time() - t0 < args.timeout:
            s = snap()
            if s:
                for node in list(pending):
                    v = node_view(s, node)
                    if v["stale_periods"]:
                        seen_stale[node] = v["stale_periods"]
                    if v["state"] == "confirmed":
                        wall = time.time() - t0
                        rec = v["recovery_s"] if v["recovery_s"] is not None else wall
                        per_node[node]["recoveries"].append(rec)
                        per_node[node]["logins"].append(int(v["logins"] or 0))
                        per_node[node]["stale"] += int(seen_stale[node])
                        rows.append([rnd, node, f"{rec:.1f}", f"{wall:.1f}", int(v["logins"] or 0),
                                     int(seen_stale[node])])
                        print(f"    {node}: confirmed, recovery {rec:.1f} s (hub's own figure), "
                              f"{wall:.0f} s after the restart, {int(v['logins'] or 0)} LoginMsg")
                        pending.remove(node)
            time.sleep(args.poll)
        for node in pending:
            per_node[node]["recoveries"].append(float(args.timeout))
            per_node[node]["stale"] += 1
            rows.append([rnd, node, "TIMEOUT", f"{args.timeout}", "", ""])
            print(f"    {node}: NO SESSION after {args.timeout} s")
        time.sleep(args.settle)
    if args.csv:
        with open(args.csv, "w", newline="") as f:
            w = csv.writer(f)
            w.writerow(["round", "node", "recovery_s", "wall_s", "login_msgs", "stale_periods"])
            w.writerows(rows)
    print_summary(per_node, args.pass_s)
    return 0 if verdict(per_node, args.pass_s) else 1


# ---- self-test ------------------------------------------------------------------

def selftest():
    snap = {
        "text_sensor-rollladenwohnzimmer1_session_state": "confirmed",
        "sensor-rollladenwohnzimmer1_session_recovery_time": "6.4 s",
        "sensor-rollladenwohnzimmer1_login_messages": "2",
        "sensor-rollladenwohnzimmer1_stale_periods": "0",
        "text_sensor/RollladenWohnzimmer2 Session State": "stale (heard, no session)",
        "sensor/RollladenWohnzimmer2 Stale Periods": 1,
        "sensor-rollladenwohnzimmer1_battery_voltage": "14.1 V",
    }
    v1 = node_view(snap, "RollladenWohnzimmer1")
    assert v1 == {"state": "confirmed", "recovery_s": 6.4, "logins": 2.0, "stale_periods": 0.0}, v1
    v2 = node_view(snap, "RollladenWohnzimmer2")
    assert v2["state"] == "stale (heard, no session)" and v2["stale_periods"] == 1.0, v2
    assert v2["recovery_s"] is None and v2["logins"] is None, v2
    assert find_state(snap, "RollladenWohnzimmer3", "Session State") is None
    # The entity name must END the key: 'Session State' is not 'Session State Age'.
    assert find_state({"sensor-rollladenwohnzimmer1_session_state_age": "9"},
                      "RollladenWohnzimmer1", "Session State") is None
    assert to_float("n/a") is None and to_float(None) is None and to_float("12") == 12.0

    log = [
        "10:00:01 [I][lora_client:1]: [RollladenWohnzimmer1] Session recovered in 6.4 s (1 LoginMsg)",
        "10:00:09 [I][lora_client:1]: [RollladenWohnzimmer2] Session recovered in 142.0 s (6 LoginMsg)",
        "10:00:40 [W][lora_client:1]: [RollladenWohnzimmer2] SESSION STALE: node heard for 30 s without a session (...)",
        "10:05:00 [I][lora_client:1]: [RollladenWohnzimmer1] Session recovered in 5.9 s (1 LoginMsg)",
        "10:05:02 [I][lora_client:1]: [RollladenWohnzimmer1] Node reports its session path: 1 REGISTER sent, "
        "LOGIN accepted after 6100 ms (copy 5 of the burst), register_seq save failures 0",
        "10:05:03 some unrelated line with Session recovered but not the pattern",
    ]
    a = analyze_lines(log)
    assert a["RollladenWohnzimmer1"]["recoveries"] == [6.4, 5.9], a
    assert a["RollladenWohnzimmer1"]["logins"] == [1, 1]
    assert a["RollladenWohnzimmer1"]["stale"] == 0
    assert a["RollladenWohnzimmer1"]["node_reports"] == [{"registers": 1, "wait_ms": 6100, "copy": 5}]
    assert a["RollladenWohnzimmer2"]["recoveries"] == [142.0] and a["RollladenWohnzimmer2"]["stale"] == 1
    assert summarize([6.4, 5.9], 30) == (2, 5.9, 6.15, 6.4, 0)
    assert summarize([142.0], 30) == (1, 142.0, 142.0, 142.0, 1)
    assert summarize([], 30) == (0, None, None, None, 0)
    assert verdict({"n1": a["RollladenWohnzimmer1"]}, 30) is True
    assert verdict(a, 30) is False, "node 2 was slow and stale"
    assert verdict({}, 30) is False, "nothing measured is not a pass"
    assert verdict({"n": {"recoveries": [], "logins": [], "stale": 0}}, 30) is False
    assert verdict({"n": {"recoveries": [5.0], "logins": [1], "stale": 1}}, 30) is False
    assert verdict({"n": {"recoveries": [5.0], "logins": [1], "stale": 0, "save_fails": 1}}, 30) is False
    assert verdict({"n": {"recoveries": [30.0], "logins": [1], "stale": 0}}, 30) is True, "30 s is the line, inclusive"
    b = analyze_lines(["[n] NODE COULD NOT SAVE ITS register_seq (1 failures): ..."])
    assert b["n"]["save_fails"] == 1
    print("restart_soak selftest: all passed")
    return 0


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--rounds", type=int, default=10)
    ap.add_argument("--nodes", nargs="+", default=NODES_DEFAULT)
    ap.add_argument("--timeout", type=int, default=600, help="seconds to wait for a session per round")
    ap.add_argument("--poll", type=float, default=5.0)
    ap.add_argument("--settle", type=float, default=120.0,
                    help="seconds between rounds (Mode B re-establishes ~2.5 min after a login)")
    ap.add_argument("--pass-s", type=float, default=30.0, dest="pass_s")
    ap.add_argument("--csv")
    ap.add_argument("--analyze", metavar="HUB_LOG")
    ap.add_argument("--selftest", action="store_true")
    args = ap.parse_args()
    if args.selftest:
        return selftest()
    if args.analyze:
        with open(args.analyze, errors="replace") as f:
            per_node = analyze_lines(f)
        print_summary(per_node, args.pass_s)
        return 0 if verdict(per_node, args.pass_s) else 1
    return live(args)


if __name__ == "__main__":
    sys.exit(main())

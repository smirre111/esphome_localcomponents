"""Build loradevices.bench.yml: production loradevices.yml plus the bench-only tweaks
(node 2 checks in every 5 min instead of 1 h; the 23:00 nightly sleep actions
become log lines). Nothing else differs, which the guard at the end asserts.
Flash it for a measurement session, then flash loradevices.yml again to restore
production.

    python tools/make_bench_yaml.py
"""
work = open("loradevices.yml", encoding="utf-8").read().splitlines()
out = list(work)

# Mode C needs several check-ins in minutes, not hours: node 2 only. BENCH ONLY -
# the production file keeps 1h, and flashing loradevices.yml restores it.
n = [i for i, l in enumerate(out) if l == "    checkin_interval: 1h"]
assert len(n) == 1, n
out[n[0]] = "    checkin_interval: 5min   # BENCH ONLY (make_bench_yaml.py)"

# The 23:00 nightly sleep deep-sleeps both nodes and ends any long Mode B hold
# (it cost a 14.5 h soak on 2026-10-07: the nodes came back unpromoted and, on
# the production profile, cannot re-promote). BENCH ONLY: replace the two sleep
# actions with a log line so the `then:` list stays valid.
for who in ("rol_1", "rol_2"):
    idx = [i for i, l in enumerate(out) if l.strip() == f"- loracover.on_sleep_start: {who}"]
    # Two uses: the nightly `time:` trigger (first in the file) and the production
    # "Sleep now" diagnostic button, which must keep working on the bench.
    assert len(idx) == 2, (who, idx)
    assert idx[0] < next(i for i, l in enumerate(out) if l == "button:"), "nightly trigger comes first"
    ind = out[idx[0]][:len(out[idx[0]]) - len(out[idx[0]].lstrip())]
    out[idx[0]] = f'{ind}- logger.log: "BENCH ONLY: nightly sleep for {who} disabled (make_bench_yaml.py)"'

# Guard: the bench file must be production + exactly the three changed lines above.
assert len(out) == len(work), (len(out), len(work))
assert sum(a != b for a, b in zip(out, work)) == 3, "bench differs from production by more than the bench-only tweaks"

open("loradevices.bench.yml", "w", encoding="utf-8", newline="\n").write("\n".join(out) + "\n")
print("loradevices.bench.yml written (production + 3 bench-only lines)")

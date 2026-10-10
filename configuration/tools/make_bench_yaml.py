"""Build loradevices.bench.yml: the working-tree loradevices.yml with the
diagnostic blocks that were COMMENTED OUT (relative to git HEAD) put back.

Only hunks where every new line is `indent + "# " + <the HEAD line>` are reverted;
any other edit (cover durations, ...) is kept, so the bench image differs from the
production one only by the diagnostics.  Flash it for a measurement session, then
`esphome upload loradevices.yml` again to restore production.

    python tools/make_bench_yaml.py
"""
import difflib, re, subprocess, sys

head = subprocess.run(["git", "show", "HEAD:configuration/loradevices.yml"],
                      capture_output=True, text=True, encoding="utf-8", check=True).stdout.splitlines()
work = open("loradevices.yml", encoding="utf-8").read().splitlines()

def commented_forms(line):
    # The editor put "# " at column 2 and kept the relative indent: "    name:"
    # became "  #   name:". Blank lines stay blank.
    if not line.strip():
        return {line}
    forms = {("# " + line).rstrip()}                       # column-0 style
    if line.startswith("  "):
        forms.add(("  # " + line[2:]).rstrip())            # column-2 style
    return forms

out, reverted, kept = [], 0, 0
sm = difflib.SequenceMatcher(None, head, work, autojunk=False)
for op, a0, a1, b0, b1 in sm.get_opcodes():
    if op == "equal":
        out += work[b0:b1]
    elif op == "replace" and a1 - a0 == b1 - b0 and all(
            work[b0 + i].rstrip() in commented_forms(head[a0 + i]) for i in range(a1 - a0)):
        out += head[a0:a1]; reverted += a1 - a0
    else:
        out += work[b0:b1]; kept += b1 - b0
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
    assert len(idx) == 1, (who, idx)
    ind = out[idx[0]][:len(out[idx[0]]) - len(out[idx[0]].lstrip())]
    out[idx[0]] = f'{ind}- logger.log: "BENCH ONLY: nightly sleep for {who} disabled (make_bench_yaml.py)"'

# Guard: the bench file must be production + exactly the three changed lines above
# (an injected block, or a revert of commented lines, would show up here).
assert len(out) == len(work), (len(out), len(work))
assert sum(a != b for a, b in zip(out, work)) == 3, "bench differs from production by more than the bench-only tweaks"

open("loradevices.bench.yml", "w", encoding="utf-8", newline="\n").write("\n".join(out) + "\n")
print(f"reverted {reverted} commented lines, kept {kept} other changed lines")

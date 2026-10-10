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

# MAC-1 buttons: the shipped long runs are all counter-off. Same grid, counter ON,
# so the delta against the MAC-0 long runs is MAC-1's cost.
MAC1 = """  - platform: template
    name: "Mode Test A — long, MAC-0 baseline (node 2)"
    id: rol_2_modetest_a_long
    entity_category: diagnostic
    on_press:
      # Counter OFF, crypto OFF: the pure MAC-0 numbers (FER_link, ppm, ping
      # turnaround). tools/mac1_turnaround.py step T0 presses exactly this name,
      # and a missing button made that step time out (~20 min) on 2026-10-09.
      - lambda: 'id(rol_2)->start_mode_test(900, 1093, 1, 1, true, false, false, true);'
  - platform: template
    name: "Mode Test A — long, MAC-1 counter (node 2)"
    id: rol_2_modetest_a_long_mac1
    entity_category: diagnostic
    on_press:
      - lambda: 'id(rol_2)->start_mode_test(900, 1093, 1, 1, true, true, false, true);'
  - platform: template
    name: "Mode Test B — long, MAC-1 counter (node 2)"
    id: rol_2_modetest_b_long_mac1
    entity_category: diagnostic
    on_press:
      - lambda: 'id(rol_2)->start_mode_test(900, 1500, 2, 1, true, true, false, true);'
  - platform: template
    name: "Mode Test A — long, MAC-1 + MAC-2 (node 2)"
    id: rol_2_modetest_a_long_mac2
    entity_category: diagnostic
    on_press:
      - lambda: 'id(rol_2)->start_mode_test(900, 1093, 1, 1, true, true, true, true);'
  - platform: template
    name: "Mode Test B — long, MAC-1 + MAC-2 (node 2)"
    id: rol_2_modetest_b_long_mac2
    entity_category: diagnostic
    on_press:
      - lambda: 'id(rol_2)->start_mode_test(900, 1500, 2, 1, true, true, true, true);'
  - platform: template
    name: "MAC Ping Start long (node 2)"
    id: rol_2_mac_ping_start_long
    entity_category: diagnostic
    on_press:
      # Pressed ~20 s AFTER a 900 s ModeTest starts: the node answers a plaintext
      # ping only while an authenticated ModeTest is armed (fw 1.1.5).
      - lambda: 'id(rol_2)->start_mac_ping(860, 1100, true, 0);'
  - platform: template
    name: "MAC Ping Start long, MAC-2 (node 2)"
    id: rol_2_mac_ping_start_long_mac2
    entity_category: diagnostic
    on_press:
      # Same as the plain long-ping button, but requesting encryption: pressed
      # ~20 s AFTER a "with MAC-1 + MAC-2" ModeTest starts, so the node's
      # crypto_enabled sublayer is on and cryptoRequired() (fw 1.1.7) refuses a
      # plaintext ping, accepting only this encrypted one.
      - lambda: 'id(rol_2)->start_mac_ping(860, 1100, true, 0, true);'""".splitlines()
anchor = next(i for i, l in enumerate(out) if 'name: "Mode Test B — long, sleep off (node 2)"' in l or 'long, sleep off (node 2)' in l)
out[anchor - 1:anchor - 1] = MAC1          # anchor-1 is its "- platform: template" line
open("loradevices.bench.yml", "w", encoding="utf-8", newline="\n").write("\n".join(out) + "\n")
print(f"reverted {reverted} commented lines, kept {kept} other changed lines")

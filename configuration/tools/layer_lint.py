#!/usr/bin/env python3
"""Ratcheting MAC-layering lint for the hub (local_components/).

Counts code-level crossings of the PHY / MAC-0 / MAC-1 / MAC-2 / APP
boundaries that the MAC separation refactor (docs/mac-separation-layering-plan.md,
review docs/mac-review-2026-10-16.md section 2) sets out to remove. Each
count may only FALL: a count above tools/layer_lint_baseline.json fails, a
count below it also fails until the baseline is lowered (--update) in the same
commit, so an improvement cannot silently be given back later.

Comments and string literals are stripped before counting, so a comment that
names a crossing ("parent_->send() BARE until ...") is not a crossing.

usage:
  layer_lint.py            check against the baseline (exit 1 on any rise)
  layer_lint.py --update   lower the baseline to the current counts
                           (refused while any count is above it)
  layer_lint.py --show     print the counts, change nothing

Kept independent of, but shaped like, the node repo's tools/layer_lint.py:
one JSON baseline of {metric: count}, one regex per metric, per-file scope.
"""
import json
import os
import re
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
LC = os.path.join(ROOT, "local_components")
BASELINE = os.path.join(ROOT, "tools", "layer_lint_baseline.json")

TRACKER = "lora_tracker/lora_tracker.cpp"
CLIENT = "lora_client/lora_client.cpp"
CHILDREN = ["loracover/cover/lora_cover.cpp", "loracover/sensor/lora_sensor.cpp"]

# metric -> (files, regex, what it means)
METRICS = {
    # MAC-0/PHY (tracker) parsing or switching on the APP schema (V1, V2).
    "tracker.app_cmd_cases": ([TRACKER], r"__CMD_\w+",
                              "tracker branches on/builds APP oneof cases"),
    # MAC-0/PHY (tracker) doing MAC-2 crypto itself (V2: beacon CMAC).
    "tracker.psa_calls": ([TRACKER], r"\bpsa_\w+\s*\(",
                          "PSA crypto calls inside the tracker"),
    # MAC-0 calling up into MAC-2 per copy (V1).
    "tracker.seal_upcalls": ([TRACKER], r"(?<!::)\bsealBurstCopyTag\s*\(",
                             "per-copy up-call into the listener's MAC-2"),
    # MAC-0 reading listener/client internals (V1 address lookup, V3 shadow).
    "tracker.client_iteration": ([TRACKER], r"\b(?:listeners_|clients_)\b",
                                 "tracker iterates registered listeners/clients"),
    # Direct data requests that bypass one framing path (V15).
    "client.raw_parent_send": ([CLIENT], r"\bparent_->send\s*\(",
                               "direct parent_->send calls in lora_client.cpp"),
    # Hand-built frames: each allocates its own MAC-1 counter (V15).
    "client.msgid_alloc_sites": ([CLIENT], r"(?<!::)\bincrTxMessageId\s*\(\s*\)",
                                 "incrTxMessageId() call sites"),
    # Hand-built frames: each seals itself (V15).
    "client.pack_sites": ([CLIENT], r"(?<!::)\bpack_operation_\s*\(",
                          "pack_operation_ call sites"),
    # APP picks plaintext/ciphertext implicitly (2.1, APP<->MAC-2).
    "client.app_picks_encryption": ([CLIENT], r"session_confirmed_\s*,\s*&buf",
                                    "producer passes encrypt=session_confirmed_"),
    # MAC-2 spread through the listener (target: one SessionKeyStore/LinkSecurity).
    "client.psa_calls": ([CLIENT], r"\bpsa_\w+\s*\(",
                         "PSA crypto calls in lora_client.cpp"),
    # APP telemetry completing MAC-0 delivery (V7 / F3).
    "client.telemetry_completes_delivery": ([CLIENT], r"handle_command_ack_\s*\(\s*this->ctl\(\)",
                                            "a non-ack message completes the tracked op"),
    # Raw wire bytes handed to APP children (V5).
    "client.raw_bytes_to_children": ([CLIENT], r"->set_response\s*\(\s*\w+\s*,\s*\w+\s*\)",
                                     "children receive raw wire bytes"),
    # APP children re-parsing the wire envelope (V5).
    "children.wire_unpack": (CHILDREN, r"\blora_client_response_message__unpack\s*\(",
                             "children unpack the wire envelope themselves"),
    # APP children running their own MAC-0 address filter (V5).
    "children.sender_filter": (CHILDREN, r"\bsenderaddress\b",
                               "children filter on the header's sender address"),
}


def strip_comments_and_strings(src):
    """Remove // and /* */ comments and the contents of string/char literals."""
    out = []
    i, n = 0, len(src)
    while i < n:
        c = src[i]
        nxt = src[i + 1] if i + 1 < n else ""
        if c == "/" and nxt == "/":
            j = src.find("\n", i)
            i = n if j < 0 else j
        elif c == "/" and nxt == "*":
            j = src.find("*/", i + 2)
            seg = src[i:(n if j < 0 else j + 2)]
            out.append("\n" * seg.count("\n"))
            i = n if j < 0 else j + 2
        elif c in "\"'":
            q = c
            out.append(q)
            i += 1
            while i < n and src[i] != q:
                if src[i] == "\\":
                    i += 1
                elif src[i] == "\n":
                    break
                i += 1
            out.append(q)
            i += 1
        else:
            out.append(c)
            i += 1
    return "".join(out)


def counts():
    cache = {}
    res = {}
    for name, (files, rx, _desc) in METRICS.items():
        total = 0
        for f in files:
            p = os.path.join(LC, f)
            if p not in cache:
                with open(p, "rb") as fh:
                    cache[p] = strip_comments_and_strings(fh.read().decode("utf-8"))
            total += len(re.findall(rx, cache[p]))
        res[name] = total
    return res


def main(argv):
    cur = counts()
    if "--show" in argv:
        for k, v in cur.items():
            print("%-40s %4d  %s" % (k, v, METRICS[k][2]))
        return 0
    try:
        with open(BASELINE) as fh:
            base = json.load(fh)
    except FileNotFoundError:
        base = {}
    rises = [(k, base.get(k), v) for k, v in cur.items() if k not in base or v > base[k]]
    falls = [(k, base[k], v) for k, v in cur.items() if k in base and v < base[k]]
    stale = [k for k in base if k not in cur]
    if "--update" in argv:
        if rises and base:
            for k, b, v in rises:
                print("REFUSED: %s rose %s -> %d" % (k, b, v))
            return 1
        with open(BASELINE, "w", newline="\n") as fh:
            json.dump(cur, fh, indent=2, sort_keys=True)
            fh.write("\n")
        print("baseline written (%d metrics)" % len(cur))
        return 0
    rc = 0
    for k, b, v in rises:
        print("LAYERING REGRESSION: %s %s -> %d (%s)" % (k, b, v, METRICS[k][2]))
        rc = 1
    for k in stale:
        print("STALE BASELINE METRIC: %s (no longer measured)" % k)
        rc = 1
    # A fall FAILS too until the baseline is lowered: otherwise a later change
    # could climb back to the old number unnoticed, and the ratchet would hold
    # nothing.
    for k, b, v in falls:
        print("RATCHET: %s fell %d -> %d; lower the baseline with "
              "tools/layer_lint.py --update in the same commit" % (k, b, v))
        rc = 1
    if rc == 0:
        print("layer_lint: %d metrics, none above baseline" % len(cur))
    return rc


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))

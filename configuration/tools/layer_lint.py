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
one JSON baseline of {metric: count}, one regex per metric.

Scope (review HB11): the tracker metrics cover the whole lora_tracker/
component (every .cpp and .h), so moving a crossing into a sibling file
(mac_mgmt.cpp) does not hide it. The include-direction metrics read the RAW
source (an #include path is a string literal, which the counting strip would
blank).
"""
import glob
import json
import os
import re
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
LC = os.path.join(ROOT, "local_components")
BASELINE = os.path.join(ROOT, "tools", "layer_lint_baseline.json")

TRACKER = ["lora_tracker/*.cpp", "lora_tracker/*.h"]   # the whole MAC-0 component
TRACKER_H = "lora_tracker/lora_tracker.h"
CLIENT = "lora_client/lora_client.cpp"
CLIENT_H = "lora_client/lora_client.h"
CHILDREN = ["loracover/*/*.cpp", "loracover/*/*.h"]
# Hub-owned sources outside MAC-2 (HubLinkSecurity.*) and the beacon tag
# (mac_mgmt.cpp): none of them may include PSA.
NON_MAC2 = ["lora_client/lora_client.cpp", "lora_client/lora_client.h", "lora_client/HubWire.*",
            "lora_client/lora_client_node.h", "lora_tracker/lora_tracker.cpp", TRACKER_H,
            "lora_tracker/automation.h", "loracover/*/*.cpp", "loracover/*/*.h"]
RAW = True   # 4th tuple element: count in the raw source, not the stripped one

# metric -> (files, regex, what it means[, RAW])
METRICS = {
    # MAC-0/PHY (tracker) parsing or switching on the APP schema (V1, V2).
    "tracker.app_cmd_cases": (TRACKER, r"__CMD_\w+",
                              "tracker branches on/builds APP oneof cases"),
    # MAC-0/PHY (tracker) doing MAC-2 crypto itself (V2: beacon CMAC).
    "tracker.psa_calls": (TRACKER, r"\bpsa_\w+\s*\(",
                          "PSA crypto calls inside the tracker"),
    # MAC-0 calling up into MAC-2 per copy (V1).
    "tracker.seal_upcalls": (TRACKER, r"(?<!::)\bsealBurstCopyTag\s*\(",
                             "per-copy up-call into the listener's MAC-2"),
    # MAC-0 reading listener/client internals (V1 address lookup, V3 shadow).
    "tracker.client_iteration": (TRACKER, r"\b(?:listeners_|clients_)\b",
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
    # MAC-1 state outside the shared Sequencer (S3: mac1::Sequencer owns it).
    "client.raw_frame_counter": ([CLIENT, CLIENT_H], r"\bframe_counter_\b|\bFrameCounter\b",
                                 "hand-rolled frame counter state in the listener"),
    # ARQ outside the delivery service (S7): the application building attempts
    # by msgid, matching acks by msgid, or re-sending stored frames itself.
    "client.app_arq_by_msgid": ([CLIENT, CLIENT_H],
                                r"\breq\.build\b|\bon_ack\b|\btx_tracked_op_\b|\bretransmit_tracked_op_\b"
                                r"|\btx_ctl_\b|\bframe_msgid\b|\btx_grid_sync_\b|\btx_schedule_config_\b",
                                "application code doing ARQ by msgid"),
    # Key handles outside MAC-2 (S2: HubLinkSecurity's SessionKeyStore owns them).
    "client.psa_key_handles": ([CLIENT, CLIENT_H], r"\bpsa_key_id_t\b|\bPSA_KEY_ID_NULL\b",
                               "PSA key handles in the listener"),
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
    # Include direction (review HB11): MAC-0's public header pulls in neither
    # the application (lora_client.h) nor the APP schema (blinds.pb-c.h).
    "tracker_h.includes_app": ([TRACKER_H],
                               r'#include\s*[<"][^>"]*(?:lora_client/lora_client\.h|blinds\.pb-c\.h)[>"]',
                               "lora_tracker.h includes lora_client.h or the protobuf stubs", RAW),
    # PSA only in MAC-2 (HubLinkSecurity.*) and the beacon tag (mac_mgmt.cpp).
    "psa_include_outside_mac2": (NON_MAC2, r'#include\s*[<"]psa/',
                                 "a hub source outside MAC-2 includes PSA", RAW),
    # The application moves MAC-1 counters only through the MAC-1 interface
    # (mac1_restart_session_ / mac1_skip_tx_ / mac1_restore_, the allocation
    # and the commit). This counts the raw writers; the interface's own
    # definitions are the baseline.
    "client.mac1_counter_writes": ([CLIENT, CLIENT_H],
                                   r"\bset_counters_no_write_\s*\(|\bseq_\.(?:restore|reset|commit|allocate|seedTxForTest)\s*\(",
                                   "raw MAC-1 counter writes in the listener"),
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


def expand(files):
    out = []
    for f in files:
        hits = sorted(glob.glob(os.path.join(LC, f)))
        if not hits:
            raise SystemExit("layer_lint: no file matches %s" % f)
        out.extend(h for h in hits if h not in out)
    return out


def counts():
    cache = {}
    res = {}
    for name, spec in METRICS.items():
        files, rx = spec[0], spec[1]
        raw = len(spec) > 3 and spec[3]
        total = 0
        for p in expand(files):
            key = (p, raw)
            if key not in cache:
                with open(p, "rb") as fh:
                    src = fh.read().decode("utf-8")
                cache[key] = src if raw else strip_comments_and_strings(src)
            total += len(re.findall(rx, cache[key], re.M))
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
    rises = [(k, base[k], v) for k, v in cur.items() if k in base and v > base[k]]
    new = [k for k in cur if k not in base]   # a new metric enters with --update
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
    for k in new:
        print("NEW METRIC: %s = %d (not in the baseline; add it with --update)" % (k, cur[k]))
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

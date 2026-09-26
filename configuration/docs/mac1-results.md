# MAC-1 (sequencing) across the three modes — 2026-09-26

**Build under test:** node 2 fw 1.1.4 (unchanged), hub = the 2026-09-26 onboarding fixes (`2370df9`) on a *bench* image (`tools/make_bench_yaml.py`: the diagnostics that the working-tree `loradevices.yml` comments out, plus two counter-ON buttons; Mode C only: node 2 `checkin_interval` 5 min). Production image restored afterwards. Raw captures: `logs/20260926_150533_mac1/` (git-ignored) — every line of COM6 and COM9, reports in `reports/`.

MAC-1 = frame counter, replay window, duplicate detection. Method (docs/mac-layer.md §4): the same grid run counter-OFF then counter-ON, back to back, 900 s, production power profile, MAC echo on, one press each. The delta is MAC-1's effect.

## Mode A (grid 1093 ms, 1 copy)

| | counter OFF | counter ON |
|---|---|---|
| mode actually run / refusal | 1 / 0 | 1 / 0 |
| detected = CRC-valid = addressed | 56 | 59 |
| **true FER** (CRC-valid → addressed) | **0 of 56** | **0 of 59** |
| counter-accepted | 55 | 58 |
| duplicates (DUP) | 1 (17 857 ppm) | 1 (16 949 ppm) |
| raw ppm (samples) | +10 (55) | +9 (58) |
| hub replay/decrypt rejections | 0 / 0 | 0 / 0 |

The one frame the counter did not accept in each run is the one duplicate: the replay window rejected a genuine duplicate and nothing else.

## Mode B (grid 1500 ms, 1 copy, promoted)

| | counter OFF | counter ON |
|---|---|---|
| mode actually run / refusal | 2 / 0 | 2 / 0 |
| windows armed / hit (WMR) | 595 / 594 (1 680 ppm) | 598 / 598 (0) |
| HW-8: one-shot n vs armed | 598 ≥ 595 | 601 ≥ 598 (0 missed both) |
| **true FER** | **0 of 601** | **0 of 601** |
| counter-accepted | 596 | 598 |
| DUP | 3 344 ppm (2) | 0 |
| raw ppm / **residual** | +9 / **−1** | +9 / **0** |
| phase error p50 / p99 / max (µs) | 659 / 2 821 / 2 921 | −48 / 919 / 1 596 |
| arm residual p99 (µs) | 411 | 408 |

Both pass the Mode B line (|residual| < 20 ppm, mode 2, windows armed > 0, 0 missed one-shots, ≥30 samples over ≥200 s). `counter-accepted` is short of `addressed` by 5 and 3: the beacon windows (~3 in 900 s at one per 349.5 s, broadcast, no counter) plus B0's 2 duplicates — **inferred from the counts, not read off a per-frame log**.

## Mode C (auto, 5 min check-ins, hub-side only; node serial untouched)

5 check-ins in 26 min, every one heard as `reason=TIMER_CHECKIN reset=DEEPSLEEP`, **`resume=1` on all five** (no re-login, no REGISTER), no `duplicate or old message ID`, no decrypt failure. Class A: 1 window / 1 hit per wake. Wake-timing error vs the counter rate: +23, +19, +15, +12 ppm (converging; recorded, not gating). So sequencing survives deep sleep: the counters resume across every wake.

## Reading

* **MAC-1 has no measurable effect on frame yield, clock rate or window timing** in A or B: FER 0 of ≥56 either way, ppm and residual unchanged, arm residual unchanged (408 vs 411 µs). The p99 phase-error difference in B (2 821 vs 919 µs) is between two runs both well inside the ±14 080 µs guard and points the wrong way for a counter cost; it is a run-to-run difference and is not attributed to MAC-1.
* **What it does do is right:** the replay window rejected exactly the duplicates (one per Mode A run) and nothing valid. Node-side, every `Rejected message ID: N … my MsgID: N` in the session (7) is one duplicate burst copy of a ModeTest control frame at a run's START or STOP.
* **Not measured here:** MAC-1's cost in *time* (MAC turnaround). No run exercised the MAC-ping echo, so there is no turnaround histogram (`turnaround n 0` in all four). That needs a MAC-ping run counter-OFF vs ON.
* The counter-accepted figure is computed even with the counter OFF (a diagnostic), so it is a check on the window's decisions, not a switch-cost measurement.
* Confidence: FER 0 of 56 bounds the true rate below ~5 % (95 %, 3/n); the B runs bound it below 0.5 %.

## Next layer

MAC-2 (crypto): `Mode Test A — with MAC-1 + MAC-2` and its Mode B equivalent, then the MAC-ping turnaround for both sublayers.

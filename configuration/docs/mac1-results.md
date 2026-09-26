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

MAC-2 (crypto): `Mode Test A — with MAC-1 + MAC-2` and its Mode B equivalent, then the MAC-ping turnaround with MAC-2 on.

---

## Addendum 2026-09-26 (evening): MAC-1's cost in time, measured

The gap above ("turnaround n 0 in all four") turned out to be two defects, and closing them is what made the measurement possible.

**Why turnaround had never been measured.** (1) A node that holds a session refused the hub's MAC ping: it is plaintext and MAC control was not in the node's plaintext-gate exemptions (host tests all ran on a fresh node with no session). (2) With MAC-1 on, control frames were checked against the SESSION replay window, which a ping (its own msgid counter) can never satisfy. Both fixed in node **fw 1.1.5**: a PING, only while an authenticated ModeTest is armed, passes the gate (MAC_CONFIG stays refused); with the counter on a ping is sequenced by its own `seq` (strictly increasing, gaps allowed, restarts with each test), so an unauthenticated frame cannot ratchet the session window.

**Why the first number was wrong.** First run (1.1.5): turnaround p50 **82.4 ms** (counter off) / 84.4 ms (on). The node's log showed the ping handler running 70 ms after the DIO0 interrupt with ~20 INFO lines between, each ~10 ms at 115200 baud: the number was the UART. DriftTest already clamps the noisy tags to WARN for this reason; ModeTest did not. Fixed in **fw 1.1.6** (clamped at arm, restored on every exit path including the node-owned deadline).

**Result, fw 1.1.6, Mode A, 900 s, ping running inside the test, production profile:**

| | counter OFF | counter ON |
|---|---|---|
| turnaround p50 / p99 (RxDone → echo enqueue) | **6 912 / 7 675 µs** | **6 926 / 7 660 µs** |
| samples | 52 | 47 |
| true FER (CRC-valid → addressed) | 0 of 105 | 0 of 106 (1 not addressed: counter-accepted 105) |

**MAC-1 costs ~14 µs at the median (p99: −15 µs) — inside the noise of two 50-sample runs.** The counter is a compare and an increment; the measurement now agrees.

**HW-7 gets its first real number: the MAC turnaround is ~6.9 ms**, against the plan's assumed 20 ms budget, the k+3 headline's 62.9 ms ceiling, the k+2 ack row's 36.5 ms ceiling and `kUplinkOffsetUs` = 60 ms. All hold with a wide margin. It was measured at one frame length (a bare ping); the per-byte FIFO read means other lengths still need the `payloadPadTo` sweep (unimplemented). It excludes CAD and backoff (it ends at TX enqueue).

**Caveats.**
* n ≈ 50 per run: the node is in free-running Mode A (~6 % catch), so a 900 s run yields ~50 echoes. The spread is tight (p99 − p50 ≈ 0.75 ms), so the median is well supported; a p99 from 50 samples is not.
* Running pings inside the test **perturbed the raw clock-rate figure** (ppm +33/+43 vs +10/+9 without pings, same period 1 093 008–9 µs). Do not read ppm from these runs; use the ping-free runs above.
* A ModeTest now runs the node's receive path at WARN, so node-side INFO lines are absent during a run (the hub log and the report are unaffected).

Raw captures: `logs/20260926_174545_turnaround/` (fw 1.1.5, the UART-inflated 82 ms) and `logs/20260926_182848_turnaround2/` (fw 1.1.6).


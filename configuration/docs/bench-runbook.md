# Bench runbook — the ten hardware measurements

**Operational companion to `test-plan.md` §10.6 and `implementation-plan.md` §12.**
**To start the session that runs this: `bench-session-prompt.md`.**
Those two say *what* each measurement is and *why* it matters, and they remain
authoritative on both. This document says **what to do at the bench, in what
order, and what result closes the item.** It adds no new claims about the
design; where a number is a gate, the gate is quoted from the plan.

**Session 1 ran 2026-09-12.** The host suite is green at 937. Ten items are
open, of which one is not a measurement at all (HW-6) and two need an external
instrument (HW-1's mean, HW-4).

**Four defects sat in front of the ten**, each invisible to the host suite:

1. **`setRtcSlowSrc()` had no caller anywhere in the firmware**, so
   `rtc_slow_src_` stayed `Unknown` on every build ever made ->
   `Demotion::BadClockSource` -> `timedRxActive()` false. **Mode B had never
   once been reachable on hardware.** It failed closed, which is why nothing
   ever reported it.
2. **A provisioned node and a freshly flashed hub deadlock**, trading REGISTER
   against LoginMsg forever at RSSI -36. A hub reflash creates that state every
   time, and no measurement can run without a session.
3. **1100 ms is phase-locked too** — see the grid-period rule in section 2.
   Measured: 277 marks sent, `detected 1`.
4. **Phase samples are committed for ANY addressed frame**, not only for frames
   that arrived on a mark, and `outside_guard` is a LATCH. One bursted
   ScheduleConfig yields a sample up to +/-750 ms out and poisons
   `phaseTrustworthy()` permanently. This is why HW-8 returned `windows 0/0`
   with `oneShot n 0`.

**Numbers now measured rather than assumed:**

| quantity | was | measured |
|---|---|---|
| node RX duty cycle | "~29 ms every 500 ms, a 5.9 % duty cycle" — never measured | **5.4-6.5 %**: 15 of 279 marks at a swept period, against 29440/500000 = 5.888 % predicted |
| `rtcSlowSrc` | asserted from the schematic | **2**, external crystal, from the beacon |
| node `CONFIG_FREERTOS_HZ` | 100 expected | **100 Hz** confirmed |
| node CPU, production profile | 240 MHz assumed | **240 MHz** confirmed |
| link margin, bench distance | unmeasured | **RSSI -36 dBm, SNR 5.5-5.75 dB**, and every detected frame passed CRC, address and MIC |

---

## 0a. Which code, and from where

| | |
|---|---|
| **Hub** | `github.com/smirre111/esphome_localcomponents` — ESPHome custom components under `configuration/` |
| **Node** | `github.com/smirre111/blindsesp` — ESP-IDF firmware under `main/`, driver under `components/lora/` |
| **Branch, both repos** | **`main`** |

`main` is the code. As of 2026-09-21 it carries everything: the protocol work,
the 927 host tests, `ModeTest`, Mode B, and the battery-voltage ADC fixes.
Nothing else needs checking out and nothing needs merging first.

```
git -C <hub>  checkout main && git -C <hub>  pull
git -C <node> checkout main && git -C <node> pull
```

**Why the history looks odd.** `main` was *adopted* rather than merged: the
development line shared no common ancestor with the old `main`, so the adoption
commit takes the development tree wholesale and records the old `main` as a
second parent. Every old commit stays reachable. `git log main` therefore shows
two unrelated root lines, and that is expected rather than damage. See
`merge-to-main.md`.

**If you need the previously deployed code**, it is the branch
`main-before-adopt-2026-09` in both repos — not a tag, because this
environment's credential cannot push tags.

**Branches you can ignore.** `claude/analysis-only-t4ter8` is the development
branch `main` was adopted from and is now redundant with it. `auto-mode-p0` is
an ancestor of `main`. `claude/blindsesp-battery-voltage-adc-78srik` was merged
in by CONTENT, so its commits are *not* in `main`'s history and it is preserved
as `retired/battery-voltage-adc`. None of them is where work happens now.

## 0. Before anything

| Check | Why it is first |
|---|---|
| Both repos on the same commit of the shared headers | `proto/regen_stubs.sh` vendors `blinds.proto`, `FrameCrypto.h`, `LoraTiming.h`, `TimedGrid.h`, `TimedModePolicy.h` and friends into the hub. The `schema_drift_*` ctest gates pass in CI; at the bench, a hub and node built from different commits fail **on the air only**, decoding fields the other end numbered differently. |
| `ctest` green before flashing | 927 tests. A bench session that starts from a red suite cannot tell a wiring fault from a regression. |
| Node built **with** `CONFIG_BLINDS_BENCH_NODE` (see `main/Kconfig`) | `MODE_SWEEP` and a non-zero `armOffsetUs` are refused outright without it — the sweep deliberately mis-arms windows, so a fleet node must never accept it. **HW-2 cannot run without this flag.** |
| Node on the **external 32.768 kHz crystal** | `rtcSlowSrc` must report 2. On the internal RC (~5 %) a node cannot hold phase between frames, Mode B is gated off at both ends, and HW-5's ppm figure is meaningless. Confirm from the beacon's `rtcSlowSrc`, not from the schematic. |
| Hub `CONFIG_FREERTOS_HZ` pinned to 1000 in `loradevices.yml` | Already pinned, with the reasoning inline. HW-9 is now a *confirmation*, not a discovery. |
| Note the firmware version and git SHA of both ends in the log | Every number below is only interpretable against the build that produced it. |

**Radio/PHY sanity, free:** `real_lora_interface_test` already asserts the node
programs SF7 / BW500 / CR4-8 / preamble 8 / sync 0x12 / CRC on, and recomputes
`kSymbolTimeUs` = 256 µs and `kCadUs` = 320 µs from those values. If the bench
disagrees with the arithmetic, that test is where the assumption lives.

---

## 1. Order of work

From `test-plan.md` §10.6's own ordering paragraph, with the dependencies made
explicit:

```
HW-9   free, immediate, confirms a pinned value
HW-10  an hour of ordinary traffic, no setup
  |
HW-8   the ARM mechanism itself — everything timed depends on it
  |
HW-3   ISR + wake latency distribution (needs B0's GPIO wake source)
HW-5   ppm under the production profile (PREREQUISITE: B0)
  |
HW-7   turnaround vs payload length — run BEFORE §5.2's constants are trusted
  |
HW-2   T_detect, the sweep (needs a bench-flagged node + timed mode on)
  |
HW-1   d_tx_ramp: variance on-node now, mean needs a scope
HW-4   RX-on current (needs a meter)
HW-6   not a measurement — a deployment decision
```

**Why HW-8 comes early:** it asks whether an `esp_timer` one-shot reliably wakes
the node from automatic light sleep. Every armed window in Mode B and every
Class A window hangs off that mechanism, and the plan asserts it nowhere. If
HW-8 fails, HW-2, HW-3 and B3 are all measuring something else.

**Why HW-5 is blocked on B0:** without a GPIO light-sleep wake source the RxDone
timestamp records when the CPU woke, up to 500 ms late, and the drift fit is
worthless. Confirm B0 has landed on the flashed build before spending a run.

---

## 2. The control surface

All of it is in `loradevices.yml`, `entity_category: diagnostic`, node 2 (the
bench unit).

**Starting a run** — buttons calling
`start_mode_test(duration_s, grid_ms, mode, copies, keepPowerProfile, enableCounter, enableCrypto, macEcho, armOffsetUs)`:

| Button | What it runs |
|---|---|
| Mode Test A — MAC-0 baseline | `(300, 1093, 1, 1, true, false, false, true)` — the delta every other run is measured against. **1093, not 1100** — see the grid-period rule below |
| Mode Test A — with MAC-1 counter | same grid, counter on. **Run back to back with the baseline**; the difference is MAC-1's cost |
| Mode Test A — with MAC-1 + MAC-2 | adds crypto |
| Mode Test B — timed windows | `(300, 1500, 2, ...)` |
| Mode Sweep ±4/±12 ms | `mode 4`, **bench node only** |
| Mode Test Stop | `stop_mode_test()` |

**Two grid-period rules that are not interchangeable**, and the node enforces
both:
- **Mode A: 1093 ms. NOT 1100 — that was wrong, and measured to be wrong.**
  A period phase-locked against the node's 500 ms RX interval means a frame
  landing in an RX-off gap does so *forever*. The old rule tested
  divisibility, so 1100 passed it and was documented as the safe value. The
  governing quantity is the **gcd**: marks land at `(k · grid) mod 500`, a
  lattice of step `gcd(grid, 500)` holding `500/gcd` phases, fixed for the
  life of the run.

  | grid | gcd | distinct phases | measured |
  |---|---|---|---|
  | 1000 ms | 500 ms | 1 | ~300 sent, 0 heard (historic) |
  | **1100 ms** | 100 ms | 5 | **277 sent, `detected 1` (0.36 %)** |
  | **1093 ms** | 1 ms | 500 (sweeps) | **279 sent, `detected 15` (5.4 %)** |

  Five phases 100 ms apart against a 29.44 ms window cover 147 ms of the
  500 ms interval, so whether *any* mark is ever heard is decided by a
  boot-time offset — about 71 % of runs at 1100 ms hear nothing at all.
  The condition for a lattice of step `g` to intersect every window of width
  `w` regardless of offset is `g ≤ w`, and that is now what the node
  enforces (`ModeTestPolicy.h`'s `periodsPhaseLock`, against
  `NodeContext::rx_window_us`). 1100 ms is refused.
- **Mode B: exactly the round (1500 ms), phase-locked to the node's slot.** In
  Mode B the rule inverts — that alignment *is* the mode.

**`keepPowerProfile`** is the YAML lambda's argument; the wire field is
`ModeTest.dropPowerProfile` and is its inverse. `true` in the YAML means the
**production** profile is kept, which is the default and the only setting under
which a power or timing number describes the shipped node.

**Reading a run:** the hub recomputes the report from the node's RAW counters —
the node never grades itself. The numbers are published one per template sensor
because a week of history cannot be graphed from a string, and the full report
line is up to 512 bytes against Home Assistant's 255-character cap on a
`text_sensor`:

`mode actually run`, `arm refusal`, `windows armed`, `windows hit`, `FER link`,
`window miss rate`, `phase error p99`, `turnaround p99`.

**Read these two first, every run, before any other number:**
1. **`mode actually run`** — DERIVED from what the node applied, not echoed from
   the request. Mode was echoed until it was fixed, so *every* "Mode Test B"
   number taken before that described Mode A.
2. **`arm refusal`** — 0 means armed. Anything else and the run describes
   nothing (see `ModeTestPolicy.h`'s `ArmRefusal`).

**Then `windows armed`.** It reads 0 when timed mode was reachable but the node's
phase never became trustworthy, and **WMR cannot tell you that** — a zero
denominator gives 0 ppm, the same number a flawless run gives.

**Alongside WMR, always read `RX windows skipped — radio busy`** (new this
session, U-2). A mark that was never armed is invisible to WMR by construction,
because `noteMarkArmed()` is what opens a mark: a node whose radio is wedged
reports a *flawless* miss rate while hearing nothing. A rising skip count is
what says so.

**Other live diagnostics worth having on screen:**
`Single shot active` and `Single shot refused — reason` (U-1: the reason is the
one you act on; the legend is in the YAML), `Node overdue` (U-5),
`Transmit pool exhausted — frames refused` (T-2).

---

## 3. The runs

### HW-9 — hub tick rate (confirmation, free)
**NODE HALF CLOSED. The procedure below was wrong and is struck through.**

1. ~~Boot both ends; read `tickRateHz` from the ModeTest report of any run.~~
   `ModeTestReport.tickRateHz` is a field the **NODE** fills, so no ModeTest
   run can report the hub's tick rate. HW-9 as written cannot close the hub
   half at all.
2. **Node half: CLOSED 2026-09-12** — `tick 100 Hz cpu 240 MHz`.
   `CONFIG_FREERTOS_HZ=100` is the reason `lora_reset()`'s `pdMS_TO_TICKS(1)`
   was zero ticks — expect 100 there and do not "fix" it. The 240 MHz
   separately confirms the production profile's pinned CPU frequency.
3. **Hub half: still open**, and it needs an observable that does not exist
   yet — the hub's own `configTICK_RATE_HZ` at boot, through `dump_config()`
   or a diagnostic entity. The value is pinned in `loradevices.yml`; what is
   unconfirmed is that the build honours the pin.

### HW-10 — hub RX-stamp uncertainty (an hour, no setup)
1. Run an ordinary traffic mix for an hour. No special mode.
2. Read the high-water mark: `dump_config()` prints the worst poll gap seen since
   boot; `rx_stamp_uncertainty_us()` gives it per packet.
3. **Closes when** you have the high-water number. It bounds how well the hub can
   place any uplink, which is what C2's ±1 ms gate is measured against.
4. **Expect it to be large during a burst** — `checkReception()` is not called
   while `lora_tx_busy_`, so the gap across a 17-copy burst *is* the burst. That
   is honest: the hub genuinely was not listening. Do not treat it as a defect.

### HW-8 — does a one-shot wake the node from light sleep?
1. Production profile (`keepPowerProfile` true). Arm a one-shot at a known offset.
2. Read the `oneShotErrorUs` histogram (`t_actual − t_target`) **and the miss
   counter**.
3. **Closes when** the histogram is bounded *and the miss count is zero*.
   **A single miss is a failed gate, not an outlier** — the whole ARM mechanism
   rests on this.

### HW-3 — RxDone ISR + light-sleep wake latency, as a distribution
1. Fixed grid, known marks. Run the Mode A baseline; record the `phaseErrUs`
   residual **spread** (that is ISR latency + wake latency + hub fire jitter).
2. Run the **identical** test with `dropPowerProfile` true (i.e. the lambda's
   `keepPowerProfile` false). **The delta is the light-sleep wake cost.**
3. Repeat with **the motor running**. §12.3 warns this is the item most likely
   to fail: production scales 40–240 MHz and the motor's PM lock changes the
   frequency mid-operation, so PLL relock time varies with what the node is
   doing. A constant here is the assumption under test, not a given.
4. **Closes when** you have the distribution (not a mean) for all three
   conditions.

### HW-5 — ppm under the production power profile
1. **Confirm B0 has landed on this build first.** Without the GPIO wake source
   the fit is worthless, not merely noisy.
2. The default run — `dropPowerProfile` unset — *is* this measurement.
3. Compare against `DriftTest`'s **+8 ppm**, which was taken with sleep
   **disabled** and says nothing about the clock Mode B actually runs on.
4. **Closes when** you have ppm under the production profile, with its sample
   count.

### HW-7 — node DRAIN + build turnaround, as a function of payload
1. MAC echo on (the baseline button already sets it). A MAC echo measures
   RxDone → TX fire and nothing else, which is what §4.3's servable-slot rule
   needs and cannot get from a command the application answers.
2. **Sweep `payloadPadTo` across 25 / 45 / 60 / 152 B.** The FIFO read is
   per-byte, so this must be measured *as a function of length* — a single
   figure is not an answer.
3. Read the `turnaroundUs` histogram per length.
4. **Gates:** feeds §5.2's parameterised servable-slot test. The k+3 headline
   survives up to **62.9 ms**; the **k+2 ack row breaks above 36.5 ms**. The
   assumed value was 20 ms and was never measured.
5. Cross-check: `kUplinkOffsetUs` is published as 60 000 µs and must be ≥ the
   measured turnaround, or the node misses **every** mark. The node counts its
   own aim hits and misses for exactly this reason — read them.

### HW-2 — `T_detect`, by sweeping the arm instant
1. **Bench-flagged node**, and **timed mode switched on** for node 2
   (`Timed Mode (Mode B)` switch — it makes Mode B *reachable*, not active).
2. Step `armOffsetUs` **late** in 500 µs increments until reception fails. The
   failure edge gives `T_detect` directly: a window armed `l` late catches the
   frame iff `l ≤ G`. The ±4/±12 ms buttons are starting points; finer steps
   need a lambda with the offset you want.
3. **Sweep early too.** The early edge returns `G` independently, and the two
   **must sum to `N·T_sym − T_detect`**. `SweepAnalysis.h` does that arithmetic.
   Two edges that do not sum are a result to distrust, not to average.
4. Run one press per offset and read the funnel in the report.
5. **No scope needed.** **Closes when** both edges are found and their sum
   checks out.
6. **This is what B3 is gated on.** Until it runs, every guard-band number in
   §5.4 rests on an assumed `T_detect`, and the detection registers
   (`RegDetectOptimize`, `RegDetectThreshold`) are never written — they sit at
   reset defaults.

### HW-1 — `d_tx_ramp`
1. **Partly unidentifiable by design, and say so rather than pretending
   otherwise.** A constant ramp is absorbed into the grid anchor and cancels;
   only its *variance* affects reception.
2. **On-node, no instrument:** run `MODE_A` at fixed payload and report the
   `phaseErrUs` spread. That bounds the variance, which is the part that matters.
3. **The mean needs a scope** on hub DIO0, or the wired-DIO0 hub of B5's gate.
   Its only provenance today is a commented-out `delayMicroseconds(220)`.
4. **Closes when** the variance is bounded on-node; the mean closes separately,
   with the scope, and may be deferred.

### HW-4 — RX-on current
1. **Current meter required.**
2. `ModeTest` gives an exactly known window count and duration, so a meter
   reading divides cleanly.
3. Run at `windowsArmed` = **1/round and 3/round**. **The difference is the
   marginal cost of a window** — which is the number §4.1 and §4.7 actually
   need, and it sidesteps the ~11 mA figure being a whole-node number for the
   measuring profile (240 MHz pinned, no light sleep) rather than a radio-only
   one.
4. **Closes when** you have the marginal per-window cost, not a single absolute.

### HW-6 — interactive vs automatic split
**Not a measurement.** A deployment decision across the 32-node target;
`ModeTest` cannot help. §4.5's "128 bursts/day" already assumes an answer.
Record the decision and its reasoning; do not book bench time for it.

---

## 4. After the measurements

Only one of these is optional.

1. **Write each number into the plan where its assumption lives**, replacing the
   assumption rather than being appended near it. §5.4's guard band, §4.3's
   servable-slot count, §4.1/§4.7's power figures, §2.4's residual budget.
2. **Turn the assertions that were waiting on a number into real ones.**
   `test-plan.md` §10.6 lists four numbers whose assertions cannot currently be
   reproduced; they become ordinary tests once the numbers exist.
3. **Then, and only then, B3's gate:** `T_detect` measured first, then reception
   ≥ Mode A over a week — WMR in `MODE_B` ≤ the `MODE_A` arm of the same week,
   **with `FER_air` equal within noise** across both arms at equal payload and
   power. If `FER_air` differs the two arms are not a fair comparison and the
   WMR result means nothing. **That check is the gate, not a footnote.**
4. **`POOL_SIZE`** (T-2) is the one open engineering choice, not a measurement:
   5 buffers against a queue of 20, and a placed frame holds its buffer until its
   mark, so a 32-node simultaneous push needs 32 buffers (~6.9 KB more static
   RAM). Every refusal path recovers, so this is throughput, not correctness.
   The bench is a good place to find out whether it bites.

---

## 5. Two limits the bench must not be asked to hide

**The host suite cannot measure elapsed time in the critical path.** 927 tests
assert the *order* of radio operations and which branch ran. A ~3.5 ms log line
once sat between the aimed instant and the CAD — a quarter of the ±14 080 µs
guard band — and it would pass every one of them. That is HW-7's territory and
it needs a scope.

**The ISRs are not under test.** `myinterrupts.h`'s handlers run in interrupt
context and capture the timestamps every phase measurement here rests on. The
host suite reaches everything downstream of the queue they post to and nothing
upstream. If a distribution below looks wrong in a way no software explains,
the ISR is the first place to look and the last place any test has been.

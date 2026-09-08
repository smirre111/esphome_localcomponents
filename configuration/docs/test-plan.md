# Test plan — validating Modes A, B and C

Companion to `implementation-plan.md` (authoritative design) and
`mode-diagrams.md` (drawings). This document specifies **what to test and how**;
it deliberately contains no test source. Every host test named here is a
`scenarios/*.cpp` file in the existing `configuration/tests/proto_sim/` harness,
and every hardware measurement is a procedure for the on-air test mode of §10.

Two deliverables:

1. **§4–§8 — host unit tests** covering all three modes, in the style the harness
   already uses (gtest; dependency-free policy headers compiled directly from
   node source; real `lora_client.cpp` / `CmdDispatcher.cpp` under shims).
2. **§10 — `ModeTest`**, an on-air test mode modelled on the existing
   `DriftTest`, which annotates the measurements that decide whether each mode
   works: frames offered/heard/decoded, phase error, ppm, arm and turnaround
   residuals — and which is designed to close the nine open measurements of
   `implementation-plan.md` §12.

§9 lists **four numbers in the plan that these tests cannot reproduce.** Writing
the specification surfaced them; they are stated rather than smoothed over.

### Scope: this is a MAC-layer test plan

Everything here tests the **MAC layer** — timing, frame error rates and the KPIs
of `mac-layer.md` §6. The application layer (covers, position, schedules,
telemetry) is deliberately out of scope, and the boundary is defined in
`mac-layer.md` §1.

Three consequences run through the whole document:

- **The three modes are MAC-0 constructs.** Mode A's burst geometry, Mode B's
  slot grid, Mode C's RX1/RX2 offsets — none has an application term. A test
  that needs to know what a blind is has crossed the boundary.
- **Security and sequencing are optional sublayers.** MAC-2 (AEAD) and MAC-1
  (frame counter, replay window) are **off by default** in the on-air test mode
  and switched on afterwards, so each one's cost lands in a measured delta rather
  than in the baseline (`mac-layer.md` §4).
- **A MAC test frame terminates at the MAC.** It is counted, timestamped and
  echoed without ever reaching the application. This is what closes the "inert
  frame" gap: there is no application dispatch to make safe, because there is no
  application dispatch.

**Prerequisite:** the on-air half of this plan needs `implementation-plan.md`
§8's **Track M** — M1 (MAC control frame + echo) and M2 (funnel counters) — which
is where that work is priced. The host tests of §4–§8 need none of it and can be
written today.

Application-layer tests already exist and stay where they are:
`real_lora_cover_test.cpp`, `scheduler_test.cpp`, `motor_policy_test.cpp`,
`schedule_text_test.cpp`, `auto_mode_policy_test.cpp`.

---

## 1. What "independent" means here, and where it runs out

"Validate the modes independently" has five distinct meanings, and they are worth
separating because the design satisfies four of them and cannot satisfy the
fifth without extra hardware.

| axis | meaning | how it is achieved |
|---|---|---|
| **I1 — independent of the code under test** | the verdict is not computed by the same function that made the decision | the node reports **raw** `t_rxdone`, `t_arm_target`, `t_arm_actual`, `msgid`, `len`; every derived quantity (phase error, hit/miss, ppm) is recomputed off-node from those. The mode's own "I hit my window" boolean is reported but never used as ground truth — it is a *value under test*. |
| **I2 — independent of the peer's claim** | neither end grades itself | the hub logs one line per transmit mark (`seq`, `t_fire`, `copies`, `len`); the node logs one line per RX event. Reconciliation is offline, joined on `seq`. `frames_offered` never comes from the node, `frames_heard` never comes from the hub. |
| **I3 — independent of the mode** | A vs B vs C is a fair comparison | one test mode, one counter set, one report schema. The mode changes only the *arming policy*; the instrumentation is byte-identical. An A/B run is then a controlled experiment, not two different experiments. |
| **I4 — independent of the driver's bookkeeping** | not trusting `LoraInterface`'s own state | lengths from `RegRxNbBytes`, SNR from `RegPktSnrValue`, CRC from `RegIrqFlags`, read directly in the report path. |
| **I5 — independent of both endpoints** | ground truth for "was it on the air at all?" | **requires a third radio** — the witness receiver of §10.5. Without it, `frames_offered` rests on the hub's claim that it transmitted, which is exactly the claim §2.3 of the plan shows can silently fail (a dropped `RegOpMode = TX` on a 1-tick semaphore timeout is a frame that never transmits, with no error). |

**I5 is the one that matters and the one that costs money.** Everything else is
software. A spare ESP32 + SX1278 in continuous RX, logging every frame it hears
with its own `esp_timer` stamp, is the only way to distinguish "the hub did not
send it" from "the node did not hear it" — and those two have opposite fixes. It
is a ~€10 part and it should exist before B3 is gated.

---

## 2. Test layers

```
L0  pinned constants      LoraTiming.h, burst stride, symTimeout      no I/O, no time
L1  policy headers        TimedGrid.h, TimedModePolicy.h,             pure functions
                          ClassAWindows.h, DriftEstimator.h
L2  closed form vs sim    reception geometry, servable slots,         two independent
                          beacon ceiling, airtime budgets             computations agree
L3  real code in the sim  real lora_client.cpp + real CmdDispatcher   production paths
                          over SimClock/SimRadio
L4  on air                ModeTest (§10) + witness receiver           the only layer that
                                                                      can falsify L0-L3
```

The layering rule the harness already follows: **anything that can be a
dependency-free header should be one**, because that is what makes it testable
at L1 rather than only at L4. `BootPolicy.h`, `AutoModePolicy.h`, `MotorPolicy.h`,
`ScheduleText.h`, `FrameCrypto.h` and `DriftEstimator.h` are all in the tree for
this reason. `LoraTiming.h`, `TimedGrid.h`, `TimedModePolicy.h` and
`ClassAWindows.h` join them.

**L2 is the layer that catches design errors rather than coding errors.** Each L2
test computes the same quantity twice by different routes — once in closed form,
once by running the sim — and asserts agreement. The 69.8 % independence trap of
plan §3 is exactly the class of error only L2 catches: the closed form was
*right*, the intuition was wrong, and no amount of unit-testing the closed form
would have found it.

---

## 3. Harness facts that constrain every test below

From `tests/proto_sim/README.md`, all learned the hard way:

- **`ctest` does not rebuild.** Always `cmake --build` first. A compile error
  leaves stale binaries reporting a cheerful pass.
- **A changed HEADER can leave stale OBJECTS even after a successful build**,
  if the machine's clock has moved backwards — `make` prints "Clock skew
  detected" and silently skips recompiling. The symptom is not a compile error
  but a LAYOUT MISMATCH: one translation unit sees the new class, another the
  old, and the result is `std::bad_alloc` inside an untouched function. Cost
  here: a stale shim object produced 19 phantom failures that survived several
  targeted rebuilds. **When a failure makes no sense, `--clean-first` before
  investigating anything else.**
- **Run the suite through `ctest`, not by executing a test binary directly.**
  `gtest_discover_tests` gives each case its own process; running the binary
  puts every case in one process, where the file-level node statics documented
  below leak between them. Same commit, same binary: 0 failures under ctest, 12
  under a direct run. The direct-run failures are the artefact.
- **There is no `settimeofday` shim.** The node clock under host test is real
  wall time and cannot be stepped. Any test involving schedule timing builds its
  entries relative to *now*. **This bites Mode C**: RX1/RX2 offsets must be
  tested against `SimClock`, not against the node's wall clock.
- **The wall clock the node sees is anchored to `CLOCK_MONOTONIC`**
  (`shims_node/host_clock.c`), because a machine whose clock jumps backwards
  otherwise fails those same tests in a way that looks like a logic bug. The
  six schedule tests in `real_cmd_dispatcher_test.cpp` that call
  `arm_missed_entry` read `time(nullptr)`, sleep 1.2 s, then read it again;
  production reads `gettimeofday`. A backward step between the two reads makes
  an entry armed "one minute ago" arrive from the future. Observed here on a
  container whose clock repeatedly snapped back three weeks: 6 failures out of
  644 in a parallel run, all six passing when re-run alone. The shim interposes
  both calls and answers `epoch_at_start + monotonic elapsed`, so an interval
  measured across a sleep is the interval that elapsed whatever the host did.
  **It is listed on the test EXECUTABLE, not only on the static library** — a
  definition of `time()` inside an archive is never pulled in, because the
  linker already has libc's and the archive member resolves nothing. This is
  still not the *settable* clock Mode C's window tests want: time can be
  observed here, not stepped.
- **Node state is file-level static and shared across every test in a binary.**
  `RTC_DATA_ATTR` is a no-op on the host. A Mode B test that leaves a node
  promoted will silently change every test defined after it — so **every timed
  mode test must demote in `TearDown`**, and the fixtures must assert the mode at
  `SetUp` rather than assume it.
- `sim/messages.h` and `sim/wire_codec.cpp` mirror the proto **by hand**. The
  `schema_drift_*` gates catch a stale generated stub but cannot catch a stale
  mirror struct. `GridSync`, `ModeTest` and `ModeTestReport` must be added in
  all three places.

---

## 4. Mode A — burst / windowed

Mode A is not new, and that is precisely why it needs tests: it is the fallback
every other mode falls back *to*, so a regression in it is a fleet-wide outage
with no safety net. Nothing here changes Mode A's behaviour; these tests pin what
it already does so that B3's slot-aware deferral cannot quietly break it.

### 4.1 `mode_a_geometry_test.cpp` — L2

The closed form of plan §3, computed two ways.

| test | assertion |
|---|---|
| `ClosedFormMatchesEnumeration` | union of `[88i mod P, +w)` for `i = 0…16`, computed by interval merge, equals a direct 1 µs-step enumeration over `[0, P)` |
| `ThreeWindowsPerRound` | `P = 500`, `w = 29` → union **482 of 500 = 96.40 %** |
| `OneWindowUnsynchronised` | `P = 1500`, `w = 29` → union **493 of 1500 = 32.87 %** |
| `ResidueGapStructure` | residues sort to gaps of 28 ms (×11) and 32 ms (×6); `11×28 + 6×29 = 482` |
| `IndependenceTrapIsWrong` | `1 − (1 − 493/1500)³ = 69.74 %`, and `ASSERT_NE` that against the true 96.40 % with a comment naming the 27-point error |
| `WindowWidthIsAParameterNotAConstant` | the same routine at `w = 29.44` gives 96.93 % / 33.37 %; **the table, the predicate and the union must be evaluated at one `w`** |
| `SimAgreesWithClosedForm` | drive `SimRadio` with the real 17-copy schedule and a 3-window receiver over 10⁵ random phases; empirical hit rate within ±0.5 % of 96.40 % |

`WindowWidthIsAParameterNotAConstant` is not pedantry — see §9.1.

### 4.2 `mode_a_burst_test.cpp` — L3

Against the **real** `lora_client.cpp` / shimmed tracker.

- `CopyStrideIsExactly88000us` — consecutive copy marks differ by 88 000 µs, not
  88 235. The node compiles `kCopySpacingUs = 88000` (`DriftEstimator.h`) and
  `kBurstTxIntervalMs = 88` (`CmdDispatcher.h:425`); a hub that drifts to
  `1500/17` reproduces the −2663 ppm bug that cost three firmware revisions.
- `BurstOccupies1450ms` — `16 × 88 + 42.048 = 1450.05 ms` for a 60 B command.
- `SendTaskBlocksAFurther400ms` — `responseWindowMs` (`lora_tracker.cpp:310`);
  **total 1850 ms against a 1500 ms round**. This is the assertion that makes
  §5.6's two-round deferral non-negotiable.
- `RetryReusesMsgidAndBytes` — a retransmit is byte-identical. Today
  `tx_tracked_op_` mints a fresh msgid (`lora_client.cpp:1166`, `:1276`); this
  test **is expected to fail on current firmware** and should be committed
  `DISABLED_` with the defect reference, not omitted. A lost ack makes the blind
  move twice — live today, not caused by this plan.
- `BurstCountStampedUnconditionally` — pins the current
  `sendPacketBurst` behaviour (`lora_tracker.cpp:420`) so B-1's per-frame TX
  policy has a before-picture to diff against.

### 4.3 `mode_a_reliability_test.cpp` — L2

- `RetryLadderExact` — `q = 0.90 → 99.999 %`, `q = 0.50 → 96.875 %`,
  `q = 0.30 → 83.193 %` for `1 − (1−q)⁵` (4 retries at
  `kOpRetryIntervalMs = 3000`, `kOpMaxRetries = 4`).
- `BurstFallbackStrictlyImproves` — asserts the *sign*, not the value; see §9.2.
- `RoundsTo999Percent` — `ceil(ln(0.001)/ln(1−p))`. **The answer depends on
  which `w` §9.1 settles on**, which is itself the argument for settling it:
  at `w = 29.44` the confined-region rows are 36 and 20 rounds; at `w = 29` they
  are **37 and 21**. `p = 0.964 → 3 rounds` either way. See §9.1 and §9.3.

---

## 5. Mode B — timed interactive

### 5.1 `lora_timing_test.cpp` — L0, the first deliverable

`LoraTiming.h` is the reference point; nothing else can check it, so it is pinned
hard and by hand.

| test | assertion |
|---|---|
| `SymbolTime` | `T_sym = 2⁷/500000 = 256.0 µs` exactly, as an integer µs constant |
| `PreambleToT0` | `T_pre = (8 + 4.25)·256 = 3136 µs` |
| `HeaderIsEightSymbols` | `T_hdr = 2048 µs` (structural; DIO3 not routed) |
| `NsymPinned` | `n_sym(25) = 72`, `n_sym(45) = 120`, `n_sym(60) = 152`, `n_sym(152) = 360` |
| `TimeOnAirPinned` | `toa(25) = 21.568 ms`, `toa(45) = 33.856`, `toa(60) = 42.048`, `toa(152) = 95.296` |
| `PayloadLengthConventionIsRegRxNbBytes` | `n_sym(58) = n_sym(60) = 152` but `n_sym(62) = 160` — **a 2-byte error in `PL` is worth 0 or 2.048 ms, discontinuously.** It would present as an intermittent crystal fault. This is the most valuable test in the file. |
| `T0FromRxDoneIsOneLine` | `T0 = t_rxdone − n_sym(len)·T_sym`; a fixture that also computes it as `t_rxdone − T_pay − T_hdr` asserts the two differ by exactly 2048 µs, so the double-count an earlier draft made is a *failing test* rather than a review finding |
| `GridPeriodsAreIntegerMilliseconds` | any grid or stride constant `% 1000 == 0` |
| `BurstStrideIs88000` | asserted here too, so hub and node share one source |

`LoraTiming.h` must compile unchanged into the hub, the node and this test —
three include paths, one file. The `check_header_drift.cmake` gate already used
for `FrameCrypto.h` and `Scheduler.{h,cpp}` applies verbatim (§8).

### 5.2 `timed_grid_test.cpp` — L1

`TimedGrid.h`: `T0_k(n) = A + n·1 500 000 + k·46 875`.

- `SlotPitchIsExact` — `1 500 000 / 32 = 46 875 µs`, integer, no remainder.
- `WindowSpan` — window `k` spans `[T0_k − 17 216, T0_k + 12 224]`;
  ARM lead `= T_pre + G = 3136 + 14 080`.
- `AdjacentWindowsAreClear` — `46 875 − 29 440 = 17 435 µs` between the close of
  window `k` and the open of `k+1`. Non-negative for every adjacent pair.
- `AnchorRoundWraparound` — `anchorRound` is `uint32`; the grid must be correct
  across `n = 2³²−1 → 0`. At 1.5 s a round counter wraps after 204 years, so this
  is cheap insurance, but the same arithmetic on a `uint16` field would wrap in
  27 hours — assert the width.
- `AnchorNeverMoves` — a second `GridSync` with the same `enable` must not shift
  `A`; only `enable=false → true` re-anchors.
- **Servable slots** (plan §4.3), parameterised on the turnaround `t`:

  | payload | hub occupies | next servable | nodes/round |
  |---|---|---|---|
  | 25 B ack | `−3.136 … +60.000` | k+2 | 16 |
  | 60 B command | `−3.136 … +80.480` | **k+3** | **10** |
  | 152 B `ScheduleConfig` | `−3.136 … +133.728` | k+4 | 8 |

  and the sensitivity, which is the part that will actually break:
  `k+3` survives `t ≤ 62.93 ms`; `k+4` survives `t ≤ 56.56 ms`;
  **`k+2` breaks above `t = 36.53 ms`.** `t` is assumed at 20 ms and has never
  been measured (§12.7) — so the test is written as a *function of `t`* with the
  thresholds asserted, not as three hard-coded row values. When HW-7 (§10.6)
  returns a number, one constant changes and the test says whether the geometry
  survived.

- `BeaconNeedsOneClearSlot` — a 45 B beacon at slot `b`'s `T0` occupies
  `−3.136 … +30.720`; slot `b+1`'s window opens at `+29.659`; overlap, therefore
  `ceil(33.856 / 46.875) = 1` slot must be left clear. **This test exists because
  the failure it prevents is invisible**: placing the beacon at slot 0 blinds the
  same node on every beacon round, forever, and it would present as "node 1 is
  unreliable".
- `WindowsPerRoundMustBeOneAt32Slots` — `750 / 46.875 = 16.000` exactly, so a
  second window at +750 ms lands on node `k+16`'s primary `T0` for **all 32
  nodes**. Generalised: a 29.44 ms window is 62.8 % of the 46.875 ms pitch, so
  the test asserts that *no* offset admits a disjoint second window at
  `slotCount = 32`. Not a probabilistic overlap — an exact, systematic one.

### 5.3 `timed_mode_policy_test.cpp` — L1

`TimedModePolicy.h`: promotion/demotion as a pure function of (ppm validity,
phase error, consecutive missed marks, `rtcSlowSrc`, confirmation age).

**The test that matters is the negative one.** Everything else is a convenience.

- `HubSingleShotWithNodeInModeAIsUnreachable` — exhaustive over the cross product
  of the input domain (ppm valid/invalid × phase error in/out of tolerance ×
  misses `0…K+2` × `rtcSlowSrc` ∈ {unknown, RC, crystal} × confirmation age
  `{fresh, stale}` × hub reboot flag): **no combination yields
  `hub = single-shot ∧ node = Mode A`.** That combination is the ~6 %-hit-rate
  failure the asymmetry rule exists to make unreachable by construction, so it is
  asserted by construction too.
- `DemotionIsUnilateralAndImmediate` — the node demotes on any of: K missed
  marks, no frame for `resyncMaxS`, `rtcSlowSrc` not the crystal. No hub
  agreement required, no delay.
- `PromotionRequiresAnUplinkObservedInItsSlot` — a node *claiming* readiness
  (`timedRxReady = true`) is never sufficient. A beacon saying "I am ready" is
  not evidence that the node's window is where it thinks it is.
- `NoPromotionWithin10MinutesOfDemotion` — the anti-flap hysteresis. Pinned as a
  number because at `beacon_interval_s = 0` a node would otherwise flap several
  times a day (plan §7).
- `RcOscillatorNodeStaysInModeAForever` — `rtcSlowSrc` reporting the internal RC
  (~5 %) is a permanent Mode A verdict, visible in Home Assistant rather than
  silent.
- `Proto3DefaultsAreTheSafeThing` — a zeroed `GridSync` means Mode A; a zeroed
  `NodeWakeBeacon` means "the hub must burst"; `rtcSlowSrc = 0` means unknown
  means Mode A. Decode an all-zero buffer and assert the resulting policy, so
  "deploy node-first" is verified rather than asserted in prose.
- `HubRebootForcesBroadcastDemote` — a hub restart emits `GridSync{enable=false}`
  as a burst **before anything else**. Without it the hub holds a new anchor on a
  fresh `esp_timer` epoch while 32 nodes hold the old one, and each burns up to
  `resyncMaxS` of missed windows — while the beacon that would re-anchor them is
  itself on a grid they no longer share.

### 5.4 `guard_band_test.cpp` — L1/L2

- `GuardFromSymbolTimeout` — `G = (N·T_sym − T_detect)/2`; at `N = 115`,
  `T_detect = 5 sym`: `(29.44 − 1.28)/2 = 14.08 ms`.
- `EarlyAndLateAreSymmetric` — early by `e` is caught iff `e ≤ G`; late by `l` is
  caught iff `l ≤ G`. Parameterised sweep either side of the edge.
- `SymTimeoutIsANamedConstantNotAMagicNumber` — `symTimeout = int(30.0f/0.26f)`
  (`LoraInterface.cpp:347`) came from a comment claiming 20 ms packets; real
  frames are 42–95 ms. The test asserts the *named* constant equals 115 so that
  changing the window is a deliberate act with a visible diff.
- `BeaconIntervalCeiling` — `G / ppm`: ±20 ppm (unmeasured crystal spec) →
  **704 s = 11.7 min**; ±2 ppm (measured) → **7040 s = 117 min**. Operating
  points are **half** each ceiling — 5.8 min and 58 min — leaving margin for ppm
  uncertainty and for one lost beacon (which doubles elapsed time since sync).
- `WideWindowFallback` — `N = 200` → 51.2 ms window, `G = 24.96 ms`, holds 20 ppm
  for 1248 s = 20.8 min at 3.41 % duty. Still better than today's 5.9 %.
- `AirtimeBudget` — pins plan §4.4 with all three rows derived, not typed:
  - today, all burst: `112 × 17 × toa(60) = 80.06 s/day` (0.0927 %)
  - Mode B + broadcast @5.8 min: `112 × toa(60) + (86400/348) × toa(45)
    = 4.71 + 8.41 = 13.12 s/day` (0.0152 %)
  - Mode B + **unicast** @5.8 min: `112 × toa(60) + (86400/348) × 32 × toa(45)
    = 273.7 s/day` (0.317 %)
  with `112 = 32 nodes × 3.5 cmd/day`. The assertion that carries the design
  decision is `unicast > today > broadcast` — **same mechanism, opposite sign,
  purely because of N.** A unicast keepalive is not viable at 32 nodes.
- `DriftEstimatorCouplesToTheGuard` — cross-reference the existing
  `drift_estimator_test.cpp` `drift_us_over` cases against `G`: the estimator's
  reported error bound must be smaller than the guard it is protecting, or the
  promotion criterion is unfalsifiable.

### 5.5 `timed_mode_scenario_test.cpp` — L3

Real `lora_client.cpp` + real `CmdDispatcher.cpp` over `SimClock`/`SimRadio`.

- `AcquisitionAToB` — node boots in A, hears a burst, learns `T0`, receives
  `GridSync`, replies in its slot, hub confirms K times, node promotes. Assert
  the full sequence and that **no single-shot is issued before confirmation K**.
- `PhaseSampleIsFilteredBySlotFirst` — `noteDriftSample` is called *before*
  parsing, by design (`frtosTasks.cpp:160-165`), so an unfiltered node stamps its
  neighbour's frames and `phaseErrUs` goes bimodal at 0 and ±46.9 ms. Drive two
  nodes on adjacent slots and assert node 1's `phaseErrUs` distribution is
  unimodal. **This is a B2 gate and it cannot be caught with one node.**
- `CachedAckOnReplayedMsgid` — a msgid-reuse retry produces the **cached ack**,
  not a drop. Required before single-copy downlink becomes normal, because that
  is when retries stop being rare. The node's replay filter requires
  `msgid > rx_id_` (`SessionManager.cpp:111-121`), so today a replay is silently
  dropped and the hub retries forever.
- `ContentChangeDuringRetryForcesNewMsgid` — the AEAD nonce is msgid-derived
  (`lora_client.cpp:256`), so a reused msgid with *different* plaintext is GCM
  nonce reuse. The hazard is narrow and sharp: `tx_tracked_op_` re-reads
  `op_position_` at pack time, so a user moving the blind mid-retry triggers it.
  Assert: same msgid ⟹ byte-identical ciphertext, or a new msgid.
- `BurstStrideIsCarriedNotCompiled` — the node must take `burstStrideUs` from
  `GridSync` rather than its compiled `kBurstTxIntervalMs = 88`
  (`CmdDispatcher.h:425` → `getBurstEndUs`, `:2836-2840`). Drive the sim with
  `burstStrideUs = 95000` and assert the node's reply deferral follows. This is a
  **live cross-repo coupling**, larger than the 88-vs-95 ms overrun §10 of the
  plan lists.
- `NoCadInModeB` — the slot is the arbitration. Assert no CAD and no pre-CAD
  backoff on the Mode B path; the unconditional `29·(1..10)` ms backoff
  (`LoraInterface.cpp:467`), quantised to 20…290 ms at the node's 100 Hz tick,
  makes both the turnaround and "in its slot" impossible. Mode A keeps CAD
  unchanged — assert that too, in the same test, so a refactor cannot delete
  both.

### 5.6 `mixed_mode_test.cpp` — L3, the property one node can never show

**Written — 12 tests, all passing.** Two nodes: node 1 in Mode B on slot `k`,
node 2 in Mode A, both against `sim/air_channel.h` rather than a closed form.

The file turns on a distinction the per-mode tests never need. Frames from one
sender never *collide* — one radio transmits one frame at a time — but that is
exactly why they *conflict*: the hub cannot begin a slot transmission while a
burst copy is still going out. Hub air time is a single serial resource. The
channel model grew a second predicate for it (`airOverlaps`, alongside
`collides`), and every result below is about the first, not the second.

What the tests assert:

- `ABurstCopyIsShorterThanItsStrideButNotByMuch` — a copy occupies 42.048 ms of
  its 88 ms stride: **47 %**. The premise everything else rests on.
- `AFullBurstDeniesAlmostTheEntireRound` — **31 of 32 slots**, against the
  plan's estimate of 30/32 (§4.5). Only the last slot survives. The stride is
  1.878 slot pitches, so copies walk *across* slot boundaries rather than
  landing on them, and each 42 ms shadow clips the slots on both sides. The
  incommensurate sweep that makes the burst reliable is what makes it total.
- `TheBlockedSetIsNotAnArtefactOfHowItIsComputed` — 31/32 is a suspicious
  enough number to re-derive slot by slot from the predicate.
- `ABlockedSlotFailsInTwoDifferentWays` — some denied slots hear **silence**,
  some hear a **frame addressed to the other node**. `detected` and `crcValid`
  increment on the latter, `addressed` does not. A KPI that stopped at
  `detected` would read that as a healthy link.
- `AFrameCaughtInTheWrongSlotIsStillTheWrongFrame` — nothing about window
  geometry distinguishes a copy meant for this node from one meant for another.
  The address filter is MAC-1, not timing.
- `ASingleCopyDownlinkBlocksAtMostOneSlot` — for every start slot. B4's
  remaining half is not an optimisation here: it is what makes mixed operation
  possible at all.
- `TheBeaconReservationIsFarTooSmallToParkABurstIn` — the burst spans >90 % of
  the round; `beaconClearSlots()` reserves a fraction of that. There is no hole
  to interleave into.
- `AForeignTransmitterCollidesWhereTheHubMerelyBlocks` — the two predicates,
  made explicit against each other.
- `AClassANodesRx1IsUnaffectedByTheGridButNotByTheBurst` — Mode C needs no clock
  agreement with the hub, and that independence buys nothing against hub
  occupancy.
- `TheNodesUplinkAndTheHubsBurstCanCollide` — a node's uplink *is* a foreign
  transmitter to a burst copy, so this one is a genuine collision and both
  frames are lost. Part of why Mode C is the last fallback.
- `OneRoundClearsTheAirButTwoIsStillTheRule` — **the correction this file
  produced.** On the air, one round suffices: the burst's last copy ends at
  1.447 s, inside the 1.5 s round, so round `n+1` is already clear for every
  slot. The two-round rule of §4.5 therefore does **not** come from air
  occupancy — it comes from the hub's single serialised `sendTask`, which blocks
  a further ~400 ms after the burst (1850 ms against a 1500 ms round), and no
  channel model can see that. The test exists so that `kDeferRounds` is never
  relaxed to 1 on the strength of a geometry argument that does not reach the
  constraint. The air margin is thin anyway: 53 ms, barely more than one slot.
- `TheMissedMarkCounterMustKeyOnAddressedFramesNotOnSilence` — the demotion
  trap, as the two counters a node could keep over the 31 denied slots. They
  must disagree, or the plan's insistence on "no frame addressed to me at my
  mark" would be academic. They do.

Covered elsewhere, deliberately not duplicated here:

| §5.6 item as originally specified | where it lives |
|---|---|
| `DeferralIsTwoRoundsNotOne` | `tx_queue_test.cpp` (the queue implements it); the *reason* is corrected above |
| `ReorderingQueueExpressesTheConstraint` | `tx_queue_test.cpp` — priority, eligibility, stable FIFO tiebreak, 17 tests |
| `CollisionRateBudget` | `mode_a_reliability_test.cpp::CollisionBudgetIsAQuarterOfACommandPerDay` |
| `ContentionRegionIsRejected` | `mode_a_geometry_test.cpp::ConfiningCopiesDestroysTheSweep` and `AgreeForConfinedBurstsToo` |

---

## 6. Mode C — auto (Class A)

### 6.1 `class_a_windows_test.cpp` — L1

`ClassAWindows.h`, referenced to the node's **own** SFD:

```
T0_uplink = t_txdone − n_sym(len)·T_sym
RX1 opens at T0_uplink + D1 − (T_pre + G)
RX2 opens at T0_uplink + D2 − (T_pre + G)
```

- `OffsetsAreFromT0UplinkNotTxDone` — a fixture computing them from `t_txdone`
  directly differs by `n_sym(len)·T_sym`, which is **payload-dependent** (18.4 to
  92.2 ms). Getting this wrong makes short frames work and long frames fail —
  the worst possible failure signature.
- `NoClockAgreementRequired` — inject an arbitrary node-vs-hub ppm and assert the
  window positions are unchanged. This is the whole reason Class A works for a
  node that has just booted with no phase, which is every auto-mode wake.
- `Rx2OnlyIfRx1Empty` — and both closed ⟹ sleep, immediately.
- `SleepOkClosesBothWindows` — `sleepOk` in `TimeSync` ends the wake at once.
- `SleepOkDefaultsToKeepWaiting` — proto3 zero means today's behaviour.

### 6.2 `auto_mode_wake_test.cpp` — L3

Extends the existing `sleep_wake_test.cpp` / `auto_mode_policy_test.cpp`.
**Fixture obligation:** one existing test leaves the interactive override set to
"interactive forever", which suppresses auto mode in every test defined after it.
These fixtures set a finite deadline and wait it out.

- `ResumeFallbackCancelledOnProvenSession` — **C0**, and free. It is armed for
  12 s and never cancelled; the decrypted `TimeSync` at 4.7 s already proved the
  session, and the log line at 13.6 s is the timer finding out nine seconds late.
- `WakeCostTier1` — with `sleepOk`, wake falls 28.1 s → ~7.7 s. Assert against
  the sim's virtual clock: today 73 % of every wake is the node proving a
  negative.
- `WakeCostTier2` — with RX1/RX2, ~3 s.
- `TodaysHubRepliesMissBothWindows` — the **negative** test, and the one that
  keeps C2 honest. The hub replies at +750 ms as a 17-copy burst
  (`lora_client.cpp:927`, `:2189`); copies land at `750 + 88i`:

  ```
  RX1 = [982.8, 1012.2] → nearest copies 926, 1014 — both outside
  RX2 = [1982.8, 2012.2] → nearest copies 1982, 2070 — both outside
  ```

  and even a fortunate alignment is only a `29.44/88 = 33.5 %` hit. **Class A
  requires a single copy at a precisely known offset; a burst is the opposite
  construction.** An earlier draft of the plan claimed these fitted; this test
  is what makes that class of claim checkable.
- `HubHasNoRxTimestamp` — assert (by construction, in the shim) that the hub's
  knowledge of when an uplink ended is quantised to the `esphome::delay(10)` poll
  loop (`lora_tracker.cpp:167`). `esp_timer_get_time`, `micros()` and `millis()`
  appear **zero times** in `local_components/lora_tracker/`. Committed
  `DISABLED_` and flipped when **Bx** lands.

---

## 7. Cross-mode

`mode_transition_test.cpp` — L3. The state machine of plan §4.6 as a whole,
because each mode's own tests pass while the transitions leak.

- `EveryModeReachesModeA` — from every state, on every failure injection (radio
  reset, session loss, hub reboot, `rtcSlowSrc` change, K misses), the node ends
  in Mode A. A reachability sweep, not a list of cases.
- `ModeAIsAbsorbingWithoutPositiveEvidence` — no path leaves Mode A except the
  promotion criterion of §5.3.
- `BAndCDoNotCoexistOnOneNode` — different populations, different mechanisms;
  assert the config surface cannot select both.
- `ReportSchemaIsModeAgnostic` — the §10 counter set is identical in all three
  modes. This is **I3**, asserted in software, and it is what makes the hardware
  A/B comparison a controlled experiment.

---

## 8. Drift gates (additions to `CMakeLists.txt`)

The harness already runs five: two protobuf stub gates, `FrameCrypto.h`,
`ScheduleText.h`, `Scheduler.{h,cpp}`. The rationale is identical for the new
shared headers — **a mismatch compiles and links cleanly on both sides and fails
only on the air**, as a timing error neither end agrees about.

| gate | source | vendored copy |
|---|---|---|
| `loratiming_drift_hub_vs_node` | `BlindsESP/main/include/LoraTiming.h` | `local_components/lora_client/LoraTiming.h` |
| `timedgrid_drift_hub_vs_node` | `BlindsESP/main/include/TimedGrid.h` | `local_components/lora_client/TimedGrid.h` |
| `schema_drift_*` (existing) | extended to cover `GridSync`, `ModeTest`, `ModeTestReport` | — |

`TimedModePolicy.h` and `ClassAWindows.h` are node-only and need no gate.

New `add_scenario` / dependency-free targets, following the existing pattern
(`if(EXISTS ${BLINDS_ESP_MAIN}/include/<Header>.h)` → `add_executable` → include
`${BLINDS_ESP_MAIN}/include` → link gtest):

```
lora_timing_test          mode_a_geometry_test        class_a_windows_test
timed_grid_test           mode_a_reliability_test     mode_transition_test
timed_mode_policy_test    mode_a_burst_test           mixed_mode_test
guard_band_test           timed_mode_scenario_test    auto_mode_wake_test
mode_test_report_test    (§10.4 — the report arithmetic, host-side)
```

---

## 9. Four numbers in the plan these tests cannot reproduce

Writing the assertions is what surfaced these. Each is small; none changes a
design decision; all four should be corrected in `implementation-plan.md` before
the tests are written, or the tests will be written to match the wrong value.

**9.1 The window width `w` is not used consistently.** Plan §3's 96.4 % / 32.9 %
use `w = 29` ms (explicitly, with a note that 29.44 gives 96.9 % / 33.4 %). But
§4.5's contention table — 17.7 % for 3 copies in 300 ms, 29.4 % for 5 in 400 ms —
only reproduces at `w = 29.44` (`3 × 29.44 / 500 = 17.66 %`; at `w = 29` it is
17.40 %). The two tables in the same document use different widths. The plan's
own warning ("use the same `w` in the predicate, the union and the table, or §9's
test will not reproduce these figures") predicted this exactly. **Recommendation:
`w = 29` everywhere** (the window rounded down is the conservative choice), which
moves the contention rows to 17.40 % and 29.00 % and does not change any
conclusion.

**9.2 The burst-fallback column of §4.7 is not derivable** from the stated
inputs. `q = 0.50` → retry-only 96.875 %, and the table's 99.99998 % implies a
per-attempt combined failure of ~0.018 over 4 attempts (`0.018⁴ = 1.05e-7`), i.e.
`1 − (1−q)(1−0.964)` — but that is not stated, and `q = 0.90` and `q = 0.30`
round to 100 % and 99.961 % under several different combining rules.
**Recommendation:** state the combining rule, or reduce the column to its only
load-bearing claim — *strictly better than retry-only, and not independent of
§4.5*. §4.3's `RetryLadderExact` pins the part that is derivable.

**9.3 "2 rounds — 3 s" to 99.9 % at `p = 0.964` is off by one.**
`0.036² = 1.296e-3 > 1e-3`, so 3 rounds (4.5 s) are needed. The 36-round and
20-round entries in the same table are correct. The conclusion — 17 copies across
the round beat any confined region by an order of magnitude — is untouched.

**9.4 The 4.7 s airtime figure for a 32-node clustered event in Mode B does not
derive** from any combination of `toa(60) = 42.048`, `toa(25) = 21.568` and 32.
`32 × (42.048 + 21.568) = 2.04 s`. The **elapsed** column, which the plan
correctly says is the one that matters, does check out exactly:
`ceil(32/10) = 4 rounds × 1.5 s = 6.0 s`, against `32 × 1850 ms = 59.2 s` today.
**Recommendation:** re-derive or drop the airtime cell; the elapsed comparison
carries the argument on its own.

---

## 10. `ModeTest` — the on-hardware test mode

**Built.** `ModeTestPolicy.h` (shared, drift-gated) holds every decision below
and has 24 host tests; `ModeTest` / `Hist` / `ModeTestReport` are on the wire in
both directions; `CmdDispatcher::handleModeTest` arms, runs and restores;
`LORAListener::start_mode_test` drives the grid; and `loradevices.yml` exposes
eight buttons — a MAC-0 baseline, the same run with MAC-1, the same again with
MAC-2, a Mode B run, a stop, and three bench-only sweep offsets.

All four histograms and the whole funnel now have sources. Wiring them turned
up four things that were reporting zero rather than reporting nothing:

- `mt_counters_` was a **second** `macfunnel::Counters` that nothing ever
  incremented, so every funnel field in the report read zero — the half of the
  report that matters most, silently empty. The report is now a **delta**
  against a snapshot of the real `funnel_`, which cannot drift from the
  counters the node actually keeps and which is the "difference between two runs
  of the identical grid" measurement the mode exists to make.
- `noteMarkOutcome` had **no caller**, so `windows_armed` stayed 0 and WMR — the
  rate §6 calls the one that actually distinguishes the three modes — read a
  permanent zero. Armed and hit are now separate events reported from where each
  is known: armed by `LoraInterface` when a **timed** window opens, hit at the
  address filter. A window that received a foreign frame, a CRC failure or an
  unparseable one counts as armed and not hit — counting both at the address
  filter would have dropped those out of the denominator and made WMR
  flattering.
- `macEcho` was stored and never read, so the flag did nothing. A running test
  with it off now silences the echo even for a frame that asks for one.
- `turnaroundUs` had no sink, so §12.7 — the measurement the servable-slot rule
  rests on — had no distribution. It is fed from the MAC echo's existing
  `RxDone → TX fire` figure.

Free-running Mode A windows are deliberately **excluded** from WMR: most are
legitimately empty because the hub is not sending, so counting them would make
the rate a measure of hub traffic rather than of whether marks are being met. A
mark is a promise; a Mode A window is a hope.

One thing is deliberately **not** built: the witness receiver (§10.5), which is
gap I2 — nothing on either end can say how many frames reached the air.


### 10.1 Why not extend `DriftTest`

`DriftTest` is the right precedent and the wrong vehicle. It is a good design —
switched over the air so a test can start and stop from Home Assistant without
reflashing, and **more importantly so the node can end it by itself**; the node
owns the duration and caps it; `stopDriftTest_()` restores the power profile on
*every* exit path including the node-owned deadline, so a hub that vanishes
mid-test cannot leave the node at 240 MHz with sleep disabled. `ModeTest` copies
all of that verbatim.

But `DriftTest` **disables light sleep outright** (`applyPowerProfile(true)`,
`CmdDispatcher.cpp:2154-2158`) and holds **continuous RX**. Both are correct for
what it measures and fatal for what `ModeTest` must measure:

- the +8 ppm figure the whole beacon-interval ceiling rests on was measured with
  light sleep **disabled** — it says nothing about the clock Mode B actually runs
  on (§12.5);
- continuous RX means the windowing under test is not exercised at all;
- and the light-sleep wake latency that §2.5 assumes is a repeatable constant
  (§12.3, flagged as **most likely to fail**) is precisely what gets switched off.

So `ModeTest` is a sibling, not a flag on `DriftTest`, and its defining property
is that **it runs under the production power profile by default.**

### 10.2 Protocol

`proto/blinds.proto`, node repo, regenerated into both trees and mirrored in
`sim/messages.h` + `sim/wire_codec.cpp` (§3).

```proto
message ModeTest {
  enum Mode { MODE_UNSPEC = 0; MODE_A = 1; MODE_B = 2; MODE_C = 3; MODE_SWEEP = 4; }

  bool   enable          = 1;
  uint32 durationS       = 2;   // node auto-exits; node caps it; 0 = node default
  Mode   mode            = 3;   // MODE_UNSPEC = "leave the node's mode alone"
  uint32 gridPeriodMs    = 4;   // hub cadence; integer ms (LoraTiming gate)
  uint32 copies          = 5;   // 1..17 — exercises B-1's per-frame TX policy
  uint32 payloadPadTo    = 6;   // pad to N bytes: sweep toa() across the table
  bool   keepPowerProfile= 7;   // DEFAULT TRUE — the difference from DriftTest
  uint32 reportEveryS    = 8;   // partial reports; 0 = final only
  uint32 seq             = 9;   // hub's monotonic mark index — the ruler
  int32  armOffsetUs     = 10;  // MODE_SWEEP: deliberate ARM error, for T_detect

  // Sublayers (mac-layer.md §4). BOTH DEFAULT OFF. Turning one on and
  // re-running the identical grid makes that sublayer's cost a measured delta.
  bool   enableCounter   = 11;  // MAC-1: frame counter + replay window
  bool   enableCrypto    = 12;  // MAC-2: AEAD
  bool   macEcho         = 13;  // MAC-0 replies with no application round trip
}
```

**The grid frame is a MAC control frame.** MAC-0 counts it, timestamps it and —
with `macEcho` — answers it, and it is never handed to `mac.on_payload`. No
application code runs, so there is nothing to make inert.

`macEcho` is what makes `turnaroundUs` mean what §4.3 of the plan needs. Today
the only way to obtain a reply is a command the application answers, so any
turnaround measured that way contains application dispatch — while the plan's
20 ms budget "budgets **zero** for DRAIN". A MAC echo measures `RxDone → TX fire`
and nothing else; `enableCounter` and `enableCrypto` then add their costs back
separately.

`enableCrypto = false` is not an invented test-only state: the link is already
unencrypted before the base-nonce exchange, so MAC-0 is the configuration every
node passes through on every cold boot.

**Arming is authenticated even when the traffic is not.** `ModeTest{enable}`
travels the normal application path; the frames it produces need no session. Two
rules follow (`mac-layer.md` §4): a node with **no** session must refuse the arm
command, or the unauthenticated bootstrap window becomes a way to hold 32 nodes
in a test mode; and `MODE_SWEEP` — which deliberately mis-arms windows — must be
refused by any node not flagged as a bench node.

`seq` rather than `msgid` as the ruler mark, for the same reason `DriftTest`
indexes by msgid: **a frame lost to the air leaves a gap instead of shifting
every later sample.** It is separate from `msgid` because `msgid` is also the
AEAD nonce input and the replay-filter key, and the test must be able to
retransmit without touching either.

```proto
message ModeTestReport {
  uint32 seqFirst = 1;  uint32 seqLast = 2;  uint32 elapsedS = 3;
  uint32 mode = 4;      bool   powerProfileProduction = 5;

  // The frame funnel (mac-layer.md §6.1), one counter per stage. Stage 0
  // (offered) is the hub's; stage 1 (on air) is the witness's; neither is
  // reported here, which is I2. I1: all of these come from raw radio events,
  // never from mode state.
  uint32 detected = 10;         // RxDone or RxTimeout fired
  uint32 crcValid = 11;
  uint32 addressed = 12;        // MAC-0 filter passed
  uint32 counterAccepted = 13;  // MAC-1, when enableCounter
  uint32 micValid = 14;         // MAC-2, when enableCrypto
  uint32 crcErrors = 15;  uint32 duplicates = 16;  uint32 micFailures = 17;
  uint32 seqGaps = 18;          // from `seq`, independent of MAC-1 being on

  // Windows — the mode's own decisions, reported as VALUES UNDER TEST
  uint32 windowsArmed = 20;  uint32 windowsHit = 21;  uint32 windowsEmpty = 22;
  uint32 missedMarks = 23;   uint32 demotions = 24;   uint32 promotions = 25;

  // Timing, in microseconds. Histograms as {min, p50, p95, p99, max, n}.
  Hist phaseErrUs = 30;      // T0_measured − T0_predicted
  Hist armResidualUs = 31;   // t_arm_actual − t_arm_target  (§2.4's ±100 µs claim)
  Hist turnaroundUs = 32;    // MAC echo only: t_reply_fire − t_rxdone (§12.7)
  Hist oneShotErrorUs = 33;  // esp_timer one-shot under light sleep (§12.8)

  int32  ppmEstimate = 40;  uint32 ppmSamples = 41;  int32 measuredPeriodUs = 42;
  int32  rssiMin = 43; int32 rssiMean = 44; int32 snrMin = 45; int32 snrMean = 46;

  uint64 sleepUs = 50;  uint64 wallUs = 51;   // light-sleep residency
  uint32 rtcSlowSrc = 52;  uint32 tickRateHz = 53;  uint32 cpuFreqMhz = 54;
  bool   counterOn = 55;   bool cryptoOn = 56;   // which sublayers this run used
}
```

`counterOn` / `cryptoOn` travel with the numbers for the same reason
`powerProfileProduction` does: a KPI measured at MAC-0 must never be quoted later
as a MAC-2 number by accident. That confusion — a figure measured under one
configuration and cited under another — is exactly what §12.5 of the plan is.

`Hist` is five int32s and a count — not a full histogram. **Distributions, not
means**: §12.3 flags the RxDone ISR latency as most likely to fail precisely
because production scales 40–240 MHz and the motor's PM lock changes the
frequency mid-operation, so PLL relock time varies with what the node is doing. A
mean would hide exactly that.

`tickRateHz` and `cpuFreqMhz` are reported by both ends, which settles §12.9 (the
hub's `CONFIG_FREERTOS_HZ` is unpinned in `loradevices.yml` and 88 ms is
consistent with 1000, 500, 250 and 125 Hz) at boot rather than by inference.

### 10.3 Lifecycle and safety

Copied from `DriftTest`, with two additions:

1. **Node owns the duration.** `modetest::testDurationS(requested)` caps it; a
   one-shot `esp_timer` ends the test independently of the hub. Every task loop
   blocks on its queue with `portMAX_DELAY`, so a poll would not do.
2. **Every exit path restores everything.** Power profile, log levels, continuous
   RX off, `symTimeout`, **and the node's mode and slot assignment**. This is the
   new one: `ModeTest` can force a mode, so it must put it back. A test that
   leaves a node in Mode B against a hub that has forgotten the grid is a node
   that stops answering.
3. **Deep sleep refused for the duration**, as today in
   `SystemCtrl::enterDeepSleepTask`.
4. **`keepPowerProfile = true` is the default**, and the report says which
   profile it ran under (`powerProfileProduction`), so a number measured with
   sleep disabled can never be quoted as a production number by accident. That
   confusion is what §12.5 is.
5. **Refuse to start below a battery threshold**, and refuse `MODE_SWEEP`
   entirely on a node that is not on the bench (it deliberately mis-arms
   windows).

Hub side mirrors `LORAListener::drift_timer_cb_` exactly, including the two
lessons already paid for there: the pre-built frame goes through the **normal
transmit queue** (`parent_->send()`), never `sendPacketOnce()` from the timer
callback — that bypasses TxDone handling and the return-to-RX in the tracker main
loop and left the SX1278 stuck in TX, hub silent until restarted (observed
2026-08-31) — and the **next** frame is built immediately after each send, in the
99 % of the period that is idle, so packing work cannot leak into the interval
being measured.

**Grid period choice.** `DriftTest` uses 1100 ms, deliberately not a multiple of
the node's 500 ms RX interval, because commensurate periods phase-lock: a 1000 ms
grid against 500 ms windows keeps a constant phase relationship, so a frame that
lands in the RX-off gap does so forever — observed, ~300 grid frames heard, zero.
**In `MODE_B` this inverts.** The grid period must be *exactly* 1500 ms and
phase-locked to the node's slot; that is the mode. So the incommensurability rule
applies to `MODE_A` and `MODE_SWEEP` only, and `ModeTest` must reject a
commensurate period in `MODE_A` rather than let the operator rediscover it.

### 10.4 The report is recomputed off-node (I1)

`mode_test_report_test.cpp` is a host test over `ModeTestStats.h` — the same
dependency-free accumulator the node compiles — fed synthetic event streams:

- `SeqGapsCountLostFramesNotReorderedOnes`
- `PhaseErrorIsComputedFromRawTimestamps` — from `t_rxdone`, `len` and the grid
  parameters alone; **not** from the node's window-hit flag
- `HistogramPercentilesAreExactForSmallN` — p95 of 20 samples must not silently
  become the max
- `CountersAreMonotonicAcrossPartialReports` — a partial report at `reportEveryS`
  must not reset the accumulator; `DriftTest` had exactly this bug shape (a
  re-arm must add to the average rather than restart it, "otherwise the result
  would only ever reflect the final burst")
- `RatesAreNamedApartNotConflated` — `FER_air`, `FER_link` and `WMR`
  (`mac-layer.md` §6.2) computed from the funnel counters and asserted distinct:
  a run where the hub silently fails to transmit must move `FER_link` and leave
  `FER_air` unchanged. Three quantities are routinely called "frame error rate"
  and they have different fixes.
- `SuccessRateDenominatorIsTheHubsCount` — `crcValid / offered`, where `offered`
  comes from the hub log joined on `seq`. **The node never computes its own
  frame-error rate**, because the node cannot know what it did not hear. This is
  I2 in one assertion.
- `CommandSuccessRateIsNotAMacKpi` — a guard test: the report carries no
  application outcome. Command success mixes MAC loss with application retry and
  idempotency; it is the right number for judging the product and the wrong one
  for judging a mode (`mac-layer.md` §6.4).
- `SeqGapsSurviveMac1BeingOff` — `seq` is an opaque mark, counted whether or not
  the frame counter is enabled. If gap detection silently depended on MAC-1, the
  default MAC-0 run would report no losses at all.

### 10.5 The witness receiver (I5)

A third radio — spare ESP32 + SX1278, continuous RX, no address filter — logging
`{t_rxdone, seq, len, rssi, snr, crc_ok}` for every frame on the channel.

It resolves the one ambiguity nothing else can: **"the hub did not transmit" vs
"the node did not hear".** Those have opposite fixes, and the plan already
identifies a mechanism for the first (a `RegOpMode = TX` write silently skipped
on a 1-tick semaphore timeout, hub `lora.cpp:158` — a frame that never
transmits, with no error). Without a witness, that failure is indistinguishable
from poor reception, and would be attributed to the mode under test.

It also gives, for free:
- an independent check on `copies` actually emitted per burst;
- the true copy stride on air, against the compiled 88 000 µs;
- interference visibility — frames on the channel from neither endpoint, which is
  the honest baseline for "433 MHz is shared with weather stations, garage
  remotes and alarm sensors".

The witness is passive and needs no protocol changes. It should exist **before
B3 is gated**, because B3's gate is "reception ≥ Mode A over a week" and that
comparison is only meaningful if both arms are measured the same way.

### 10.6 Bench procedures, mapped to the open measurements

| # | §12 item | procedure | needs |
|---|---|---|---|
| **HW-1** | `d_tx_ramp` (§12.1) | **Partly unidentifiable by design.** A constant ramp is absorbed into the grid anchor and cancels; only its *variance* affects reception. So: run `MODE_A` at fixed payload, report `phaseErrUs` spread — that bounds the variance. The **mean** needs a scope on hub DIO0 or the wired-DIO0 hub of B5's gate. State the split rather than pretending the mean is measurable on-node. | scope for the mean only |
| **HW-2** | `T_detect` (§12.2) | **UNBLOCKED.** The hub publishes a grid now — `enable_timed_mode()`, a per-node switch in `loradevices.yml`, default off — and a bench unit is built with `CONFIG_BLINDS_BENCH_NODE` so `MODE_SWEEP` and a non-zero `armOffsetUs` are no longer refused outright. Previously: `GridSync.armOffsetUs` (bench-gated) + `SweepAnalysis.h` exist and round-trip against the simulated radio, and the node arms from them. But `send_grid_sync()` is only ever called with `false`, so no `GridSync` carrying an `armOffsetUs` ever reaches a node and the sweep cannot start. See implementation-plan.md §11a. ~~Remaining: `LoraInterface` must arm at `gridstate::armInstantUs()`.~~ Done on the NODE — a one-shot timer at `gridstate::armDelayUs()` replaces the free-running periodic one while `timedRxActive()`. That half is real; the sweep still cannot run, for the hub reason above. 6 host tests on the delay arithmetic, including that `arm_offset_us` reaches the timer one-for-one, without which the sweep would measure nothing. **`MODE_SWEEP`.** Step `armOffsetUs` late in 500 µs increments until reception fails; the failure edge gives `T_detect` directly, since a window armed `l` late catches the frame iff `l ≤ G`. Sweep early too — the early edge returns `G` independently, and the two must sum to `N·T_sym − T_detect`. **No scope needed.** This was described as the B3 blocker; the dependency is now the other way round — the sweep is blocked on hub wiring (§11a), not B3 on the sweep. | a bench-flagged node + timed mode switched on |
| **HW-3** | RxDone ISR + light-sleep wake latency, **distribution** (§12.3) | Fixed grid, known marks; the `phaseErrUs` residual spread is ISR latency + wake latency + hub fire jitter. Separate the wake term by running the identical test twice, `keepPowerProfile` true then false; the **delta** is the light-sleep wake cost. Repeat with the motor running to catch the PM-lock frequency change §12.3 warns about. | none |
| **HW-4** | RX-on current (§12.4) | `ModeTest` gives an exactly known window count and duration, so a meter reading divides cleanly. Run at `windowsArmed` = 1/round and 3/round; the difference is the marginal cost of a window, which is the number §4.1 and §4.7 actually need — and it sidesteps the ~11 mA figure being a whole-node number for the measuring profile rather than a radio-only one. | current meter |
| **HW-5** | ppm under the **production** profile (§12.5) | The default `keepPowerProfile = true` run *is* this measurement. Compare against `DriftTest`'s +8 ppm (sleep disabled). Requires **B0** first — without a GPIO light-sleep wake source the timestamp records when the CPU woke, up to 500 ms late, and the fit is worthless. **B0 is therefore a prerequisite for HW-5, not merely for Mode B.** | B0 landed |
| **HW-6** | interactive/automatic split (§12.6) | Not a measurement. A deployment decision; `ModeTest` cannot help. | — |
| **HW-7** | node DRAIN + build turnaround (§12.7) | `turnaroundUs` histogram: `t_reply_fire − t_rxdone`, both from `esp_timer` on-node. Sweep `payloadPadTo` across 25/45/60/152 B — the FIFO read is per-byte, so this must be measured *as a function of length*. Feeds directly into §5.2's parameterised servable-slot test: **k+2 breaks above 36.5 ms**. | none |
| **HW-8** | `esp_timer` one-shot wakes from automatic light sleep (§12.8) | `oneShotErrorUs` histogram plus a miss counter: arm a one-shot at a known offset under the production profile, record `t_actual − t_target`. **The whole ARM mechanism depends on this and the plan asserts it nowhere.** A single miss is a failed gate, not an outlier. | none |
| **HW-9** | hub `CONFIG_FREERTOS_HZ` (§12.9) | ~~`tickRateHz` reported at boot by both ends, plus a `vTaskDelay(1)` duration check. Then pin it in `loradevices.yml`.~~ **PINNED** to 1000 in `loradevices.yml`, with the reasoning inline. The bench check is now a *confirmation* rather than a discovery: report `tickRateHz` at boot and assert it against the pinned value. The node's is `CONFIG_FREERTOS_HZ=100` and is the reason `lora_reset()`'s `pdMS_TO_TICKS(1)` was zero ticks. | none |
| **HW-10** | hub RX-stamp uncertainty (Bx) | New, and cheap: `dump_config()` prints the worst poll gap seen since boot, and `rx_stamp_uncertainty_us()` gives it per packet. Run a normal traffic mix for an hour and read the high-water mark — it bounds how well the hub can place any uplink, which is what C2's ±1 ms gate is measured against. **Expect it to be large during a burst**: `checkReception()` is not called while `lora_tx_busy_`, so the gap across a 17-copy burst is the burst. That is honest — the hub genuinely was not listening. | none |

Eight of ten close on-node with no external instrument. HW-1's mean and HW-4
need hardware; HW-6 is not a measurement. HW-10 is new with Bx and needs
nothing but a running hub — and its expected answer, ±5 ms, is already known to
fail C2's gate, so what it really measures is how much worse than nominal the
poll gap gets in practice.

**Ordering.** HW-9 is free and immediate. HW-2 gates B3 and needs only a bench
node. HW-5 needs B0. HW-7 should run before §5.2's servable-slot constants are
fixed, or that test will be written around an assumption.

### 10.7 Diagnostic surface

Following the existing `DriftTest` pattern — template buttons under
`entity_category: diagnostic` in `loradevices.yml`, per node:

```
"Mode Test Start (node N)"   → start_mode_test(mode, duration_s, grid_ms, copies)
"Mode Test Stop (node N)"    → stop_mode_test()
```

plus sensors fed by `ModeTestReport`, so a run is readable from Home Assistant
rather than only from a serial console: success rate, `phaseErrUs` p95, ppm and
sample count, `measuredPeriodUs`, missed marks, demotions, sleep residency.

Two entities carry more weight than the rest and should be visible permanently,
not only during a test — they are the fleet's early warning that Mode B is
degrading before it demotes:

- **`measuredPeriodUs`** — the ruler-mismatch alarm (`DriftEstimator.h:89`). If
  the hub's stride and the node's compiled expectation diverge, this is the one
  number that says so. It is what would have caught the 88.235-vs-88.000 error in
  minutes instead of three firmware revisions.
- **`rtcSlowSrc`** — gates Mode B entirely. A node that has fallen back to the
  internal RC oscillator must be **visibly** in Mode A, not silently.

The same warning as `DriftTest` applies and should be in the YAML comment: a test
run is not something to leave on. `MODE_A` with `keepPowerProfile = false` is
`DriftTest`'s power cost (~11 mA against a ~1.2 mA interactive average); the
default profile is much cheaper but still above idle.

### 10.8 Acceptance thresholds, per phase gate

The plan's gates restated as things `ModeTest` measures. Each is a *number the
report prints*, so a gate is passed or failed rather than argued.

| phase | gate | ModeTest verdict |
|---|---|---|
| **B0** | timestamps lose their 100 ms-scale outliers; jitter < 1 ms; **light-sleep residency unchanged** | `phaseErrUs` max < 1000 µs; `sleepUs/wallUs` within 2 % of a pre-B0 baseline run. The residency half is not optional — arming DIO0 only leaves the radio in STANDBY (~1.5 mA) for most of every round, a term that alone exceeds the whole 1.2 mA node average. |
| **B1** | bursts observably start on the grid; nothing regresses | witness log: first copy within ±2 ms of `T0_k`; `framesDecoded` rate unchanged vs the pre-B1 run |
| **B1a** | a frame can be placed "not before round n+2, behind nothing else" | mixed-mode run: zero collisions where the hub also held traffic for the timed node |
| **B2** | `phaseErrUs` inside ±2 ms in the field, on every node, over days | `phaseErrUs` p99 < 2000 µs, **unimodal** — a bimodal distribution at 0 and ±46.9 ms is the unfiltered-sample bug of §5.5 |
| **B3** | `T_detect` measured first; then reception ≥ Mode A over a week | HW-2 completed; then **`WMR`** in `MODE_B` ≤ the `MODE_A` arm of the same week, with **`FER_air` equal within noise** across both arms at equal payload and power. If `FER_air` differs, the two arms are not a fair comparison and the `WMR` result means nothing — that check is the gate, not a footnote. |
| **B4** | command success rate unchanged over a week | end-to-end ack rate, single-copy vs burst, same denominator |
| **B5** | p99 fire residual < 200 µs | `armResidualUs` p99 on the node; **the hub half is unmeasurable as built** — no `gpio_isr_handler_add` anywhere in the hub component, and `lora_endPacket(false)` polls `REG_IRQ_FLAGS` with `esphome::delay(2)`. Needs a wired DIO0 on the hub or a scope. |
| **Bx** | an uplink's `T0` known to ±1 ms | witness cross-check of the hub's new timestamp |
| **C1** | wake 28.1 s → ~7.7 s on a real check-in | `wallUs` per wake |
| **C2** | wake → ~3 s; no missed downlinks over a week | `wallUs` per wake; `windowsEmpty` where the hub log shows a frame was sent |

---

## 11. What none of this catches

Stated plainly, because a test plan that does not say where it stops is worse
than no test plan.

- **Interference.** Every host test assumes a clean channel. The 96.4 % of §3 is
  geometry only. Only the witness receiver measures the real thing, and only over
  weeks — a week of quiet says nothing about a neighbour's new weather station.
- **The `T_detect` assumption before HW-2.** Every guard-band number in §5.4 is
  parameterised on a **LoRaWAN rule of thumb**, not a datasheet value, and
  `RegDetectionOptimize` / `RegDetectThreshold` are never written
  (`LoraInterface.cpp:78-82`), so they sit at reset defaults. Until HW-2 runs,
  those tests verify internal consistency and nothing about the radio.
- **The p99 that matters.** ARM stays a priority-22 task dispatch — ±100 µs
  typical, **milliseconds at the p99** until NVS writes come off the RX path. The
  host tests cannot see this at all; `armResidualUs` can, but only over a run
  long enough to contain a real NVS write, which means a run containing real
  traffic, not a synthetic grid.
- **Silent SPI drops.** `lora_write_reg` skips on a 1-tick semaphore timeout on
  both ends. A skipped `RegOpMode = TX` is a frame that never transmits with no
  error, and a skipped ARM is a window that never opens. The witness catches the
  first. **Nothing here catches the second** — it presents as a missed mark,
  indistinguishable from poor reception. Making the take bounded *and reported*
  (plan §2.3) is the only fix, and it is a prerequisite for trusting
  `missedMarks` as a diagnostic at all.
- **Anything about 32 nodes.** Every test above runs 1–3 nodes. The servable-slot
  arithmetic, the beacon economics and the collision budget are all closed-form
  at N = 32 and **have never been observed at N > 3**. That is the largest
  untested assumption in the whole plan, and no amount of host testing changes
  it.

---

## 12. Cross-references

- `mac-layer.md` — **the layer boundary this plan is scoped to**: the MAC
  service interface, the MAC-0/1/2 sublayers, the frame funnel and the KPI
  definitions of §6, and the `msgId` triple-overload finding.
- `layering-proposal.md` — the original link/application argument (2026-08-31)
  that `mac-layer.md` sharpens.
- `implementation-plan.md` — authoritative design; §9 sketches these tests, §10
  the defects they are written against, §12 the measurements §10.6 closes.
- `mode-diagrams.md` — to-scale timing, sequences and power states.
- `tests/proto_sim/README.md` — harness build, and the three constraints of §3.
- `timed-window-node-analysis.md` — node-side evidence for every node file
  and line cited here.
- `timing-accuracy.md`, `wake-cost-proposal.md`, `power-analysis.md` — the
  measurements that already exist.

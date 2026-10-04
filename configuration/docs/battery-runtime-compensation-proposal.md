# Battery-voltage runtime compensation — a plan

**Status: IMPLEMENTED (2026-09-30).** Written 2026-09-27 as a plan; shipped as
node fw 1.1.10 plus the hub-side learned-duration/debug-button/hard-start-OTA
work (node `d223b61` "Learned travel durations, on-demand debug toggles, and
hard-start", hub `15e18c5` "Hub side: learned durations, debug buttons,
hard-start OTA support"). Hardware-verified on the open-move sample; node 2's
close-direction endstop does not reliably fire, which limits close-direction
learning. The plan below is kept for the design rationale — read it as
history, not as an open TODO.

Written 2026-09-27, from the user's observation on the bench: a blind moves
slower on a low battery than on a fresh one. This is a **plan**, not a change —
nothing here has been implemented.

The ask, restated as three parts: (1) correlate observed runtime with the
battery so low-voltage runs can be corrected for, (2) do the correction
whenever a **full** open/close run confirms where the truth is, and (3)
accumulate enough of that data over time to fit a voltage→runtime function
rather than a single hard-coded ratio.

---

## 1. What already exists to build this on

This is not a green field. Three mechanisms already in `MotorPolicy.h` /
`MotorCtrl.cpp` do most of the hard part:

* **A genuine "reached the physical end" signal that is independent of the
  timer.** The current-sense endstop (`endstopArmed()`, debounced over
  `kZeroCurrentStopCount` samples) fires when the motor current reads zero
  while driving into a physical end. This is not the timer, and not the
  run-time backstop — it is the one place in the whole system that knows,
  from the hardware, that the blind is **actually** at the stop, whatever the
  configured duration assumed. This is the ground truth the correction needs,
  and it already exists.
* **Motor current is already sensed and reported.** `m_lastMotorCurrentAmps`
  is a real ADC reading, converted and cached every tick, and already carried
  in the `CoverPosition` uplink (`voltage`, `current` — fields 2 and 3,
  `blinds.proto`).
* **Battery voltage is already sampled at exactly the right moment.**
  `MotorCtrl::onOperationBoundary()`'s end-of-move branch already calls
  `cmdDispatcher->measureAndSendBatteryVoltage()` — a fresh reading taken the
  instant a move ends. Today it exists for telemetry only; it is already the
  right hook to also record a calibration sample at.
* **The move duration is threaded through as data, not baked into the state
  machine.** `runMsForTarget()`, `maxRunMs()` and `radiusAfter()` all take
  `open_duration_ms` / `close_duration_ms` as **parameters** computed once at
  the start of a move. Nothing about the FSM, the snap logic, or the slack
  handling needs to change to feed those functions a corrected number instead
  of the raw configured one.

That last point is the plan's central design choice, and it exists because of
a lesson already on record
([[motor-move-termination]] / `architecture-review.md`): *"changing WHEN a
move ends breaks what assumed WHY it ended."* This plan deliberately does
**not** touch when or why a move ends. The current-sense endstop, the
run-time backstop, the snap-on-timer rule and the slack handling all stay
exactly as they are. The only thing that changes is the **input duration**
handed to the existing arithmetic before a full move starts — the same
function, a better number.

---

## 2. The core idea

For a **full** open or full close move (not a step, not an intermediate
target — see §5 for why those are excluded) that ends via the **current-sense
endstop** specifically (not the timer, not the backstop):

```
actual_ms   = elapsed time from move start to the endstop firing
nominal_ms  = the configured open_duration_ms / close_duration_ms
ratio       = actual_ms / nominal_ms
```

`ratio` is the correction the current battery state calls for, at the voltage
the node measured for that run. A ratio of 1.15 means the move actually took
15 % longer than the nominal (fully-charged) figure assumed — exactly the
"motor runs slower on a low battery" observation, now a number instead of an
impression.

**Why the endstop specifically, and not just any move that reached IDLE.** A
move that ends on the **timer** proves nothing — it is the thing being
corrected, not evidence about whether the correction is right. A move that
ends on the **backstop** means the model (even before any correction) was
wrong by more than 20 %, which is worth logging as an anomaly but is a poor
calibration point — something else may be wrong (a jam, a genuinely flat
battery, stiff hardware in cold weather). Only the endstop path says
"the blind really did reach the end, and it took this long."

---

## 3. Phased plan

Each phase is independently useful and safe to stop after, matching how the
Class A / `sleepOk` work was staged (`wake-cost-proposal.md`).

### Phase 0 — measure and report only, no behaviour change

* At `onOperationBoundary()`'s end-of-move branch, if the move that just ended
  was a **full** move (`!m_modeTargetPosition`) and ended via the **endstop**
  (needs a small addition: the stop reason is not currently threaded out of
  `motionTick()` as data — today it is implied only by which branch called
  `motorStop()`. A `enum class StopReason { Endstop, Backstop, Target, User,
  Timer }` set once per stop and read here is the minimal plumbing this needs).
* Compute `ratio` as above. Add it, and the voltage already being sampled at
  that instant, to the existing end-of-move telemetry.
* **Wire change, additive only:** two new `CoverPosition` fields —
  `float actualRunS` and `float nominalRunS` (or just `ratio`; a raw pair is
  more useful for later analysis than a pre-divided number, and proto3's
  "zero means absent" convention then reads correctly: a target move or a
  step reports both as 0, which is unambiguous). Old hub code ignores unknown
  fields; no other frame changes.
* **Nothing about how the motor runs changes in this phase.** Its entire
  purpose is to start building the dataset (§4) risk-free, and to let the
  hub/HA side show the numbers to a human before anything acts on them —
  which also doubles as confirmation that the observed slowdown is as large
  and as voltage-correlated as it looks from the bench.

### Phase 1 — apply a simple correction

* Once Phase 0 has a handful of samples spanning a meaningful voltage range:
  a first, deliberately simple model — `ratio ≈ V_nominal / V_measured`
  (motor speed roughly proportional to supply voltage under a roughly
  constant mechanical load) — computed **before** a full move starts, from
  the **most recent** voltage reading.
* The corrected duration feeds `runMsForTarget()` / `maxRunMs()` exactly where
  the raw configured duration does today. Everything downstream — the
  endstop, the backstop, the snap rule, the slack — is unchanged and keeps
  working exactly as it does now, because none of them care why the number
  they were given is the size it is.
* **Deliberately asymmetric, on purpose:** bias the correction to slightly
  *overestimate* the needed runtime rather than underestimate it. An
  overestimate is still caught cleanly by the endstop (which also produces
  another calibration sample). An underestimate stops the blind short of the
  physical end with **no signal that anything was wrong** — the timer fires,
  the transition still snaps the position to the extreme (because a
  timer-ended full move always does), and the reported position is now a
  lie the endstop never got a chance to correct. This asymmetry is the one
  place this plan has to be conservative rather than clever.
* A cheap safety net for free: `maxRunMs()`'s existing 20 % margin already
  gives a corrected-but-still-short estimate room to reach the endstop before
  the backstop fires. Whether that margin is generous enough once real
  corrected numbers exist is worth re-checking once Phase 0's data is in.

### Phase 2 — a real power→runtime function, not a single ratio

* Phase 1's `V_nominal / V_measured` is a physically-motivated guess, not a
  fit. §6 has since decided how this phase actually works, superseding the
  open question this section originally posed: the fit runs **on the node**,
  against **electrical power** `P = V × I` as the single regressor, from
  **exactly 2** endstop-confirmed full-move samples per direction —
  `ratio = m·P + c`, solved in closed form, the newest sample replacing the
  older of the two kept. This is deliberately not a richer multi-sample curve
  fit; see §6 for the full reasoning and §7 for how the first two samples are
  bootstrapped.
* The node's existing precedent (`DriftEstimator.h`'s incremental linear fit
  for crystal ppm) confirms a small, dependency-free, host-testable
  accumulator is a proven pattern here — this is the same shape of problem,
  just power-vs-ratio instead of time-vs-ppm.
* The hub-side telemetry from §3/§4 is retained for visibility (so a human
  can watch the ratio and the two live sample points on the hub), but the hub
  never computes or feeds back a model — per §6 decision 1.

---

## 4. What data to collect, and where

Per **full-move, endstop-terminated** stop:

| field | why |
|---|---|
| direction (opening/closing) | open and close slack, gearing and load differ |
| `nominal_ms` (the configured duration at the time) | the denominator `ratio` is relative to |
| `actual_ms` (elapsed to the endstop) | the numerator |
| battery voltage at move end (already sampled here) | the independent variable |
| battery voltage at move **start** (not currently sampled at that instant — a cheap addition, one more `readBatteryMonitoringADC()` call) | the load-sag on this specific run may matter more than the rested voltage; worth having both to find out which correlates better |
| motor current during the run (already sensed, currently only cached as the last sample) | a second candidate independent variable — current draw reflects both voltage *and* mechanical load, so it may separate "battery is weak" from "blind is stiff" better than voltage alone. Whether it is worth the complexity is a Phase 2 question, not a Phase 0 one. |

**Node-local:** per §6, this is where the model actually lives, not just a
cache — the 2 kept (power, ratio) samples per direction (4 numbers total,
plus the fitted `m`/`c`), persisted the same way `SessionManager.cpp`
persists its own NVS blob today (raw `nvs_get_blob`/`nvs_set_blob`, `magic` +
`version` framing). This must survive a reboot; it is the node's only copy.

**Hub-side:** the full history, for visibility only (per §6 decision 1, the
hub never computes or feeds back a model) — a template sensor or two, maybe a
small Lovelace graph of runtime vs power over the node's lifetime, useful for
a human sanity-checking the correction while it's new.

---

## 5. Why steps and intermediate-target moves are out of scope

A **step** (`MOTCMD_STEP_UP`/`DOWN`) runs for a fixed `kStepDurationMs` nudge
and deliberately never snaps to an extreme — "it ends wherever it ends." There
is no ground truth to compare against, so it can neither calibrate the model
nor benefit from a corrected duration in any way that means something.

An **intermediate target** move (`SUPPORT_TARGET_POS`) is computed by
`runMsForTarget()`'s closed-form inverse and is deliberately never snapped or
endstop-armed for a genuine physical end (`endstopArmed()` requires
`target_position` to be at an extreme). It should still receive the *same*
voltage correction applied to its computed run time — an intermediate target
is exactly as affected by a slow motor as a full move is — but it can never
**produce** a calibration sample, for the same reason a step cannot: nothing
confirms where it actually stopped.

---

## 6. Decisions (2026-09-27, user)

1. **The fit runs on the node.** Not the hub — this node has its own physical
   parameters (pack, gearing, wear) and must be able to correct itself with no
   hub in the loop. The hub-side telemetry fields from §3/§4 stay, for
   visibility only; the hub never computes or feeds back a model.
2. **Voltage and current together — combined as electrical power.** `P = V ×
   I`, measured for the run, is the single independent variable the model is
   fit against, not two separate regressors. This is also what makes decision
   3 exact rather than a rule of thumb:
3. **Exactly 2 calibration samples per direction.** A straight line is fully
   determined by 2 points and no more are needed: `ratio = m·P + c`, solved in
   closed form from the two most recent endstop-confirmed full moves in that
   direction (open and close tracked and fitted separately — different
   slack, gearing and load per §4's own reasoning). A third sample **replaces
   the older of the two** kept, so the model keeps tracking the pack as it
   ages rather than freezing at its first two readings.
4. **Before 2 samples exist, the move must run long enough that the endstop
   — not the timer — is what stops it**, so the very first moves are
   themselves usable calibration data rather than a coin flip against however
   wrong the factory `open_duration`/`close_duration` turns out to be. See §7
   for exactly how, and for the one subtlety this introduces (the live
   reported position during those first moves).
5. **Per-node, not per-fleet.** Packs are user-installed and vary in
   chemistry, age and internal resistance; a shared model would explain away
   real per-node differences.

---

## 7. Bootstrapping before 2 samples exist, precisely

Decision 4 needs two things kept **separate**, which today are the same
number:

* the duration **the hardware stop timer is armed with** — this is what must
  be inflated during bootstrap, so the endstop gets there first;
* the duration `radiusAfter()` divides by to report the **live position**
  during the move — this should stay at the best real estimate (the factory
  config, or the fitted model once it exists), or the live percentage shown
  in Home Assistant would crawl at whatever inflated rate the timer is using,
  even though the move itself is behaving normally and will still snap to the
  correct end position the moment it stops.

So: `MotorPolicy.h` gains a second duration parameter alongside the existing
one, used only to arm the backstop/timer, defaulting to equal the first
(today's behaviour, unchanged) whenever a direction already has its 2
samples. Before that, it is set to a fixed, generous bootstrap value — close
to the existing absolute ceiling (`kAbsMaxRunMs`, 120 s) rather than a new,
smaller multiplier of a number (the factory duration) that this whole
mechanism exists because it cannot be trusted yet. Real travel is "20-60 s"
per the existing comment in `MotorPolicy.h`, so a 100 s bootstrap ceiling
leaves the endstop every realistic chance to fire on its own, with the
absolute 120 s ceiling as the true backstop, exactly as it already is for
every other move.

---

## 8. What this does not solve

* **`kBattTrimFactor` is still 1.0 and still unverified** (`SKILL.md` /
  `lora-blinds-toolchain` memory: the raw-to-volts ADC conversion has never
  been checked against a multimeter). A correction fitted against an
  uncalibrated voltage reading still gives the right *runtime*, since it is
  the RAW reading that correlates with real voltage regardless of the trim
  factor — but the reported "at N.N volts" numbers in any human-facing graph
  would be off by whatever the trim error is. Worth fixing first if the
  voltage axis is going to be looked at directly, not required for the
  runtime correction to work.
* **A blind that is mechanically binding (not battery-limited) will look
  identical to a low-battery run** in a voltage-only model — both show a
  higher `ratio` at whatever voltage was measured. Motor current (open
  decision #2) is the natural way to tell these apart, if it turns out to
  matter in practice.
* **Cold-weather stiffness** is a second, unmodelled confound with the same
  shape as the mechanical-binding case above, and is out of scope for this
  plan entirely.

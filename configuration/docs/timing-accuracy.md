# Timing accuracy: how 8 ppm was measured, and what production must do

Measured 2026-08-31. Node 2 runs **+8 ppm** relative to the hub — 269 frames
over a 294 s baseline, with two independent estimators agreeing to 1 ppm.

This document exists because the measurement was hard for reasons that were
almost entirely about the *instrument*, not the crystal, and those reasons
constrain how a production interactive mode has to work.

---

## 1. The result

```
DriftTest RESULT: +8 ppm from 269 frames over 294 s (measured period 1100010 us)
```

Stable across the whole second half of the run: `+7, +9, +9, +9, +9, +8, +9, +8`.
The least-squares fit said +8 ppm; the independently measured grid period said
+9 ppm. That agreement is what makes it trustworthy — for most of the
development they disagreed in *sign*.

**What it is:** relative drift between node 2's clock and the hub's, with the
hub's own oscillator error folded in. It is **not** node 2's absolute crystal
error, and it is one measurement at one temperature.

**What it buys:**

| sleep interval | accumulated drift at 8 ppm |
|---|---|
| 10 min | 4.8 ms |
| 1 h | 29 ms |
| 6 h | 173 ms |

An earlier estimate assumed 20 ppm and quoted 432 ms for a 6 h sleep. The real
figure is 2.5× better, which makes a much tighter scheduled RX window practical.

---

## 2. Why the first attempts failed — the ruler, not the receiver

The first design fitted a line across the arrival times of one 17-copy burst.
It reported ±1500 ppm of scatter for a crystal pair that physically cannot
exceed about ±20 ppm — noise **fifty times larger than the entire possible
range of the answer**.

The cause was not on the node. `LORATracker::sendPacketBurst` paces copies with
`vTaskDelayUntil`, and the hub runs `CONFIG_FREERTOS_HZ = 1000`. **Every copy is
released on a 1 ms tick boundary.** Over the 1.4 s span of a burst, ±1 ms of
placement error is ~700 ppm of slope error.

The governing relation is simply:

```
resolution ≈ timestamp error / baseline
```

| baseline | 1 ms of error becomes |
|---|---|
| 1.4 s (one burst) | ~700 ppm |
| 294 s (one test) | ~3 ppm |

**A 200× longer baseline was worth more than any amount of receiver precision.**
This is the single most important conclusion in this document.

---

## 3. What the node must do

### 3.1 Timestamp at the interrupt, attribute after decode

Capture `esp_timer_get_time()` as the **first statement** of the DIO0 ISR.
Everything after it — queue hop, task wake, two SPI transactions, decrypt —
adds jitter far larger than the signal.

But *which frame* a timestamp belongs to can only be known after decoding. So:
**capture early, commit late.** Take the time at `RxDone`, carry it with the
event, and only add it to the fit once the frame is confirmed to be a timing
frame.

Two failures taught this:

* Reading a shared global (`g_dio0_rx_us`) three call-levels later meant a frame
  could be stamped with its *neighbour's* arrival time. Copies are 88 ms apart;
  whenever the task was delayed past the next one, the sample was wrong. The
  timestamp now travels inside the interrupt queue (`lora_irq_evt_t`).
* Committing *every* `RxDone` — on the theory that a timing test needs no
  decoding — folded acks, schedule pushes and CRC-passing noise into the fit.
  A 295 s run collected 291 "frames" where only ~268 were real.

The timing genuinely does not need decoding. **Identifying which frames count
does.** Those are different questions.

### 3.2 Sleep must be disabled, not merely discouraged

Waking from light sleep on a GPIO requires the wake source to be armed
explicitly (`gpio_wakeup_enable` / `esp_sleep_enable_gpio_wakeup`); an ISR alone
is not enough. With automatic light sleep active the node **slept through the
frames it was trying to time** — 17 per burst became 3–6.

An `ESP_PM_NO_LIGHT_SLEEP` lock is *not* sufficient: it asks the power manager
not to sleep while leaving the mechanism armed underneath. Use
`esp_pm_configure(light_sleep_enable = false)`.

Deep sleep is separate and must be refused explicitly — one run was truncated at
11 bursts by the node deciding to sleep on its own initiative.

### 3.3 Production must return to the normal profile

Both profiles live in **`SystemCtrl::applyPowerProfile()`** so they cannot drift
apart:

| | production | measuring |
|---|---|---|
| CPU frequency | scaling 40–240 MHz | pinned 240 MHz |
| automatic light sleep | **on** | off |
| typical current | ~1.2 mA | ~11 mA |

Every exit path — hub STOP, and the node-owned deadline — restores production.
A host test asserts this, because a node left in the measuring profile burns ~9×
its budget and nothing reports it until the pack is flat.

---

## 4. What the hub must do

### 4.1 Pace with a hardware timer, never the scheduler

`vTaskDelayUntil` and ESPHome's `set_interval` are both quantised to the
FreeRTOS tick (1 ms here). Use `esp_timer` — microsecond resolution, APB-derived.

### 4.2 Keep work out of the measured interval

Build the frame for tick *N+1* immediately after sending tick *N*. The timer
callback should do nothing but hand an already-packed buffer to the transmit
queue. Packing and encryption then happen in the idle ~999 ms *between* ticks,
where they cost nothing.

This is also why **plaintext frames are unnecessary**: crypto was never in the
timing path once the frame was pre-built. (And plaintext does not work anyway —
see §6.)

### 4.3 Do not transmit from the timer callback

Calling `sendPacketOnce()` directly from the `esp_timer` callback bypasses the
`TxDone` handling and the return-to-RX that live in the tracker's main loop. It
left the SX1278 stuck in TX, and **the hub went silent from the moment the test
was triggered until it was restarted**.

Transmit through the normal queue with `setBurstCopies(1)`. Reuse the proven
path; do not build a second one.

### 4.4 Commands must be bursted; only timing frames are single

A node in normal operation is in **windowed RX** — a ~29 ms window every 500 ms,
a 5.9 % duty cycle. That is the entire reason the 17-copy burst exists. A
single-copy START has roughly a 6 % chance of being heard, and in practice was
never heard at all: the node sat in windowed RX receiving nothing for eight
minutes.

**START and STOP burst. Only the timing frames, sent once the node is already in
continuous RX, are single copies.**

### 4.5 Beware commensurate periods

A 1000 ms grid against 500 ms RX windows **phase-locks**: the relationship never
changes, so if frames start landing in the node's RX-off gap they do so forever.
Observed exactly that — ~300 grid frames, zero heard, while a manually triggered
frame at an unrelated phase got through immediately.

The grid is now 1100 ms, deliberately not a multiple of the RX interval. The
17-copy burst never had this problem because 88 ms is incommensurate with 500 ms,
so copies sweep across the window.

---

## 5. Consequences for a production interactive mode

The original goal was **one RX window per 1.5 s round** instead of three, to cut
receive duty from 5.9 % toward ~2 %.

**It is now viable.** At 8 ppm the node can predict when the hub will transmit,
and the guard band is set by drift accumulated since the last sync:

```
guard band ≈ 2 × drift_ppm × time_since_sync + timestamp_jitter
```

At 8 ppm, 10 minutes since sync needs ~10 ms of guard — comfortably inside a
29 ms window. Six hours needs ~350 ms, which does **not** fit, so a node waking
from a long sleep must fall back to windowed RX until it re-syncs.

That suggests a two-tier receive strategy:

* **recently synced** (minutes) — one narrow scheduled window, drift-corrected
* **long sleep** (hours) — windowed RX for the first round, then switch

### Multiple nodes

Everything above is per-node, and each node has its **own** drift. Consequences:

1. **Drift is per-node state.** The hub must store a ppm figure per node, not a
   global constant. Nodes differ, and 8 ppm on this unit says nothing about
   node 1.
2. **Measure per node.** The test mode is addressed to one node at a time. Node 1
   has never been characterised.
3. **The hub's own oscillator is the common reference.** All measurements are
   relative to it, so hub error appears as a systematic offset shared by every
   node — it cancels for scheduling (both ends use the same grid) but must not
   be mistaken for a property of the nodes.
4. **Scheduled windows must not collide.** If several nodes narrow to one window
   each, the hub must place their transmissions in distinct slots — the current
   design has one node reply per post-burst window, which does not generalise.
5. **Re-measurement should be periodic and cheap.** Drift varies with temperature
   and ageing. A production node could refine its own estimate passively from
   ordinary hub traffic — every received frame with a known nominal instant is a
   sample — without ever entering the 11 mA test mode.

Point 5 is the most valuable follow-on: the same `LongFit` arithmetic running on
normal traffic, at zero extra power cost.

---

## 6. Things that seemed reasonable and were not

Recorded so they are not re-tried.

* **Plaintext test frames.** Intended to keep variable-time crypto out of the
  timing path. It was never in that path (§4.2), and the node rejects
  unencrypted commands once a session exists, so every frame was silently
  dropped. Exempting `DriftTest` would let an unauthenticated frame pin a node
  in ~11 mA continuous RX — a battery-drain vector on a 1.2 mA device.
* **`ESP_PM_NO_LIGHT_SLEEP` lock instead of disabling light sleep.** See §3.2.
* **Edge-triggered DIO0 during the test.** Introduced to defend against an
  interrupt storm that instrumentation later proved did not exist (`zeroflags=0`
  on every sample). Worse, `setup()` makes DIO0 RTC-capable and level-triggered
  *precisely so it can wake the CPU from light sleep*; edge triggering breaks
  that. The test now changes no interrupt configuration at all.
* **Per-burst fitting with a robust estimator.** Considered when per-burst
  numbers were scattering. Unnecessary once the baseline was extended — the
  outliers stopped mattering.
* **MCPWM hardware capture for RX timestamps.** Real and available (12.5 ns
  resolution, no ISR latency), and a true hardware TX trigger is *not* available
  on the SX1278 — transmission starts only on an SPI write to `RegOpMode`. But
  neither is needed: at 294 s of baseline the residual is already ~1 ppm.
  Revisit only if the baseline must shrink drastically.

  **Closed, 2026-09-03** — see `timed-window-mode-plan.md` §12.1. The capture
  timer is APB-clocked, and APB stops in light sleep, so the edge is never
  latched at all. Using it means holding the chip in WAITI across the RX window,
  which costs ~0.39 mA and erases the battery saving the window narrowing exists
  to produce. It remains free and useful **inside this bench test**, where light
  sleep is already disabled — that is the one place it would shorten the
  baseline. The ULP was examined as the light-sleep-capable alternative and
  rejected for a harder reason (§12.2): the ULP-FSM has no SPI, and none of the
  radio pins are RTC-capable, so it cannot reach the SX1278 at all.

---

## 7. Debugging lessons

* **Read the whole panic, not just the backtrace.** Three theories were spent on
  watchdog resets whose backtrace showed the idle task. The panic text named the
  real culprit — `taskMotorContro` — on the line above, which a log filter was
  discarding.
* **Change one variable at a time.** One build changed three things; the
  improvement was credited to the wrong one and that error persisted for hours.
* **Instrument instead of theorising.** Counting DIO0 wakes settled the
  interrupt-storm question in one run, after three firmware revisions of
  guessing.
* **Do not conclude from three samples.** A "16× improvement" reported from
  three bursts evaporated on the fifth.
* **`ctest` passes on stale binaries.** Always check the build's exit status
  separately.

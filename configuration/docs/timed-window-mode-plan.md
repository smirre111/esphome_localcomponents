# Timed-window mode: a plan

Today every hub→node exchange is **17 TX copies over 1.5 s against a 29 ms /
500 ms receive window** — brute-force diversity that guarantees a hit without
either end knowing what time it is. It works, and it costs the node a 5.9 %
receive duty cycle and the channel ~1.5 s of airtime per command.

This plans a second mode: **one transmission, one receive window**, placed by
agreement instead of by repetition. The node starts in today's mode, and
promotes only once clock agreement has been *demonstrated*, not assumed.

Companion documents: `timing-accuracy.md` (how 8 ppm was measured and what it
cost), `wake-cost-proposal.md` (why the wake is 28 s and what Tier 2 needs).
This document is the design those two point at.

---

## 1. The central idea: anchor first, thin out later

The tempting shape is "build a scheduled mode, switch to it, keep the old one as
a fallback". That makes the switch a cliff — the first time the new mode runs on
air is also the first time anything depends on it.

The better shape falls out of one observation:

> Both modes differ **only in how many copies are sent**. If every hub
> transmission already starts on the node's grid mark, then a burst and a single
> shot leave at the same instant, and the mode switch is a pure deletion of
> copies 1..16.

So the plan is:

1. Make the hub's transmit instant **deterministic** (§3).
2. **Anchor the existing 17-copy burst to a grid** — nothing changes on air (§4).
3. The node now gets a known nominal instant for every frame it already
   receives, so it can fit drift **passively, from ordinary traffic**, in the
   field, for free — which is `timing-accuracy.md` §5 point 5, and the thing
   that turns one bench measurement into a live per-node figure.
4. Only when that figure has converged does the hub start dropping copies.

The mode switch is then reversible at zero cost and cannot lose a frame, because
of this truth table:

| hub sends | node is WINDOWED | node is TIMED |
|---|---|---|
| **burst** (17 copies from the mark) | heard — today's behaviour | heard — copy 0 *is* the mark |
| **single shot** (1 copy at the mark) | **~6 %, effectively deaf** | heard |

Three of four cells work. The fourth is forbidden by construction (§5). That
asymmetry is the whole safety argument, and it only holds if bursts start on the
mark — which is why step 2 comes before step 4.

---

## 2. What the node's window actually costs today

| | today | timed |
|---|---|---|
| RX window | 29 ms every 500 ms | ~4 ms every personal round |
| receive duty | 5.9 % | ~0.13 % (at a 3 s round) |
| hub airtime per command | 17 × ~15 ms ≈ 250 ms, spread over 1.5 s | ~15 ms |
| latency to reach an awake node | ≤ 1.5 s | ≤ one personal round |

The airtime number matters as much as the battery one: on a shared 433 MHz
band, dropping from 17 copies to 1 is a 17× reduction in channel occupancy per
command, and it is what makes more than a handful of nodes possible at all.

---

## 3. P0 — a deterministic transmit instant

**Nothing else in this document is worth starting until this is done and
measured.** A window can only be as narrow as the jitter of the thing it is
trying to catch, and today that jitter is milliseconds, in code that has nothing
to do with pacing.

Measured against `lora_tracker.cpp` / `lora.cpp` as they stand, the path from
"decide to send" to "radio keys up" contains:

| step | cost | file |
|---|---|---|
| `ESP_LOGI("Sending packet of length %d")`, **inside the radio mutex** | ~5 ms blocking UART at 115200, plus a variable push to API log clients | `lora_tracker.cpp:496` |
| `lora_idle()` → `esphome::delay(1)` | `vTaskDelay(1)` — **0–1 ms, uniform** | `lora.cpp:713` |
| 6 config register writes that never change | each a blocking `spi_device_transmit` | `lora_tracker.cpp:498-503` |
| `lora_beginPacket()` → `lora_idle()` again | **a second 0–1 ms** | `lora.cpp:445` |
| FIFO fill, one blocking SPI transaction per byte | ~1 ms for 60 B | `lora.cpp:620` |
| `lora_tx()` → `REG_OP_MODE = TX` | ← air starts here | `lora.cpp:738` |

### P-1. Split *prepare* from *fire*

```
prepare(frame)   // mutex, standby, FIFO fill, payload length — in the idle time BEFORE the mark
fire()           // one SPI write: REG_OP_MODE = TX          — AT the mark
```

This is the change that makes the mode possible. After it, air-start jitter is
one SPI transaction plus mutex acquisition, not the whole ~1.5 ms setup
sequence. It is also exactly the discipline `timing-accuracy.md` §4.2 already
argues for ("build the frame for tick N+1 immediately after sending tick N"),
carried one level further down: the *frame* is pre-built today, but the *radio*
is not pre-loaded.

### P-2. Delete the millisecond delays from the TX path

`lora_idle()` and `lora_tx()` block a whole tick each. The correct values are
already sitting commented out one line above them — `delayMicroseconds(120)`
and `(220)` (`lora.cpp:733`, `:743`). Use `esp_rom_delay_us()`.

This removes 0–2 ms of uniformly distributed jitter from **every** transmission,
in both modes. Note this is also the likeliest true cause of the ±1500 ppm
scatter that `timing-accuracy.md` §2 attributes to `vTaskDelayUntil`: that call
is absolute and non-accumulating, so it contributes a constant per-burst offset
that cancels in a slope fit, whereas two `vTaskDelay(1)` calls contribute
independent per-copy noise — ±2 ms over a 1.4 s baseline is ~1400 ppm.

### P-3. Logger out of the hot path

Move the "Sending packet" log after `fire()`, or drop it to VERBOSE.

### P-4. Hoist the per-packet radio reconfiguration

SF / CR / BW / sync word / CRC are rewritten before every packet and never
change. Set them in `setup()`. Leaves preamble length, FIFO and payload length
as the only per-packet writes — and preamble only if TX and RX differ.

### P-5. Reserve the mark

`LORATracker::loop()` takes the radio mutex every ~10 ms to poll RX. A mark that
lands mid-`lora_receive_packet` is late by the length of a FIFO read. The grid
timer should assert a short pre-mark lock (~5 ms) that makes `receive()` yield.

### P-6. Measure it, and publish the number

A GPIO toggle in `fire()`, or simply `esp_timer_get_time()` at `fire()` minus
the nominal mark, published as a hub diagnostic sensor (p50 / p99 residual).

**Gate: p99 residual < 200 µs before P3 starts.** Without this number the mode
is unfalsifiable — every later symptom will be blamed on the node's crystal, and
`timing-accuracy.md` §7 is a list of what that costs.

P-1..P-6 are worth doing on their own merits. They make today's burst tighter
and cheaper and they touch no protocol.

---

## 4. P1 — the grid

### The clock

Hub `esp_timer_get_time()`. **Not** the wall clock: `time->now().timestamp` is
NTP-disciplined, steps, and is second-resolution. Wall time keeps owning the
schedule; the grid owns transmission instants, and the two never mix.

```
mark(n) = A + n · T_round          [hub µs]
```

`A` is set once when the grid starts and never moves. A free-running periodic
`esp_timer` — created once, never stopped — advances `n`. Stopping and
restarting it per transmission would re-randomise the phase and destroy the
thing being built.

Keep `T_round = 1500 ms`. It is already the node's assumed round and the
constant its burst-end arithmetic is built on; changing it buys nothing and
touches node code.

### Slots

For N nodes, prefer a **superframe** over intra-round slots in v1: node *k* owns
rounds where `n mod N == k`. Per-node arithmetic is then identical to the
single-node case, and the personal round is `N · T_round` (3 s for two nodes).
Intra-round slotting is a later optimisation for when N gets large enough that a
3 s personal round hurts.

Reuse the existing sequential `login_slot_` allocator (`lora_client.h:217`) —
it is already collision-free by construction, and the reasoning in that comment
(why not `address % N`) applies unchanged.

### What P1 changes on air

**Nothing.** The hub still sends 17 copies. It just starts them on the
addressed node's mark instead of whenever the queue drained.

### What P1 gives the node

Every received frame now has a known nominal instant, so:

```
air_start_node = rxdone_timestamp − time_on_air(len, sf, bw, cr, preamble)
offset(n)      = air_start_node − mark(n)
```

`time_on_air` is deterministic and both ends compute it identically — the
formula already exists at `tests/proto_sim/scenarios/auto_mode_proto_test.cpp:300`
and should move to a shared dependency-free header used by hub, node and tests.
Subtracting it is what makes a *variable-length* frame usable as a timing
sample; without it only constant-size frames work.

Fitting `offset(n)` against `n` over many rounds gives ppm — the same
`DriftEstimator` / `Accumulator` arithmetic that already exists, fed by ordinary
traffic instead of a bench grid. Constant residuals (node ISR latency, the hub's
SPI-write-to-air delay) fold into the intercept and cancel exactly, **provided
they are constant** — which is what §3 buys.

Baseline requirement, from `timing-accuracy.md` §2 (`resolution ≈ timestamp
error / baseline`): ~0.5 ms of residual over ~500 s gives ~1 ppm. So a node
stays unpromoted for the first several minutes after any resync.

P1 turns 8 ppm from *one bench result on one node at one temperature* into a
live per-node figure visible in Home Assistant, on both nodes, across days and
seasons. **That evidence is the precondition for trusting anything in §6.**

---

## 5. Mode state machine

RX discipline is a **new axis, orthogonal to `NodeMode`**. `INTERACTIVE` /
`AUTO` says when the node is awake; `WINDOWED` / `TIMED` says how it listens
while awake. Do not overload `NodeMode` — a timed auto node and a timed
interactive node are both meaningful.

```
BURST ──────────► SYNCING ──────────► TIMED
  ▲   ppm invalid    │  fit converged    │
  │   or rtc bad     │  span > 300 s     │
  │                  │  residual < X     │
  └──────────────────┴───────────────────┘
         K missed marks, ack failure,
         beacon reports timedRxActive=false,
         or any hub uncertainty
```

`SYNCING` is not a distinct on-air behaviour — it is `BURST` on air, on the
grid, while the node accumulates samples. That is the point of §4.

### The asymmetry rule

1. The node may drop to windowed RX **unilaterally, at any moment, without
   negotiation**.
2. The hub may send single-shot **only** with positive, recent confirmation that
   the node is in a timed window — the last K acks/beacons arrived inside their
   expected slots.
3. **Any hub uncertainty → burst.** Reboot, session change, missing beacon,
   stale confirmation, unknown firmware version: burst.
4. A single shot that goes unacked is retried **as a burst, immediately** — not
   after a renegotiation round-trip.

Rule 3 is what makes the forbidden cell of the §1 table unreachable. Disagreement
then costs airtime, never connectivity.

### Hysteresis

Promotion needs a long baseline; demotion is instantaneous; no promotion within
X minutes of a demotion. A marginal link must not flap between modes, because
flapping produces exactly the intermittent, weather-dependent symptom that is
hardest to diagnose from a roof.

---

## 6. The node's window

```
guard  = 2 · ppm_uncertainty · t_since_sync + tx_jitter + rx_isr_jitter + margin
window = [mark − guard − preamble_detect, mark + guard]
```

At 8 ppm over a 3 s personal round the drift term is ~48 µs — **drift is not
the binding constraint at this timescale**. The floor is the TX jitter of §3 and
the node's ISR jitter. At a 200 µs p99 residual, a ±2 ms window is generous
against today's 29 ms.

Two radio details that decide whether this works:

- **Open on the preamble, not the mark.** LoRa needs several preamble symbols to
  detect. At SF7/BW500 a symbol is 0.256 ms and the current 8-symbol preamble is
  2.05 ms.
- **Lengthen the TX preamble in timed mode** (16 symbols, +2 ms airtime) so the
  preamble is comfortably longer than the guard band. This is how LoRaWAN Class B
  finds its beacons; it is cheap, and it converts timing margin into a parameter
  you can turn.

Note the hub is mains-powered, so "one RX window" on the hub side is not about
power — it is about *reserving* the uplink instant so the hub never schedules a
transmission over the reply, and about knowing when a missing reply is actually
missing.

---

## 7. The constraint that decides how far this goes: the node's sleep clock

`esp_timer` counts APB, and **light sleep stops APB** — stated outright in
`tests/proto_sim/shims_node/esp_pm.h`. IDF compensates on wake from the RTC slow
clock, so the timed window's accuracy across any sleep is the *RTC's* accuracy,
not the 8 ppm measured awake at 240 MHz.

`loradevices.yml:281` already warns about this: the node runs an external
32.768 kHz crystal, and a fallback to the internal RC "will drift". The internal
RC is on the order of 5 % — 50,000 ppm — which destroys any window narrower than
seconds.

Therefore:

- The node reports its RTC slow-clock source in the beacon (`rtcSlowSrc`), and
  **timed mode is gated on the 32 kHz crystal**. A node on the RC oscillator
  stays in BURST forever, and that is visible in Home Assistant rather than
  silent.
- **A node waking from deep sleep always starts in BURST**, re-acquires, and
  promotes only after the baseline requirement is met. `timing-accuracy.md` §5
  already reaches this conclusion (350 ms of guard at 6 h does not fit a 29 ms
  window); this design keeps it.
- **The deep-sleep RTC drift is unmeasured**, and it is the number that decides
  whether the auto-mode case is ever reachable. It is the same experiment as the
  drift test with the node light-sleeping between marks (§9, P2). Until it
  exists, assume timed mode is an *awake-node* feature.

---

## 8. Protocol changes

The `.proto` source is **not in this repository** — only the vendored
`blinds.pb-c.{h,c}`. Schema changes are made upstream and regenerated into both
trees, then mirrored in `tests/proto_sim/sim/messages.h` and `wire_codec.cpp`.

New downlink command (`LoraClientOperationMessage.cmd`, next free tag 19):

```proto
message GridSync {
  uint32 roundIndex    = 1;  // n for THIS frame's mark
  uint32 roundPeriodUs = 2;
  uint32 superframeLen = 3;  // rounds per cycle
  uint32 nodeSlot      = 4;  // which round in the superframe is yours
  uint32 dlOffsetUs    = 5;  // your downlink mark within your round
  uint32 ulOffsetUs    = 6;  // when to answer
  uint32 guardHintUs   = 7;  // hub's own p99 TX residual
  bool   enable        = 8;  // promote / demote
}
```

Carrying the grid coordinates explicitly beats inferring them: nothing has to be
reconstructed after a reboot on either side, and `roundIndex` makes a lost frame
a gap rather than a shift (the same reasoning that made `msgid` the ruler mark in
the drift test).

Uplink additions to `NodeWakeBeacon`:

```proto
  int32  ppmEstimate    = ...;
  uint32 ppmSamples     = ...;
  uint32 ppmSpanS       = ...;
  uint32 ppmResidualUs  = ...;
  bool   timedRxActive  = ...;   // what the node is ACTUALLY doing
  uint32 rtcSlowSrc     = ...;   // gates the whole mode (§7)
```

`timedRxActive` is deliberately a report of behaviour, not of intent. The hub
must key rule 2 of §5 on what the node *is doing*, never on what it was last
told to do.

Two constraints this repo has already paid for once each:

- **proto3 defaults must mean the safe thing.** `enable = false` → burst;
  `timedRxActive = false` → hub must burst; `rtcSlowSrc = 0` → unknown → burst.
  Same reasoning as the `sleepOk` field's comment: the safe default has to be
  the one that costs power, never the one that drops a command.
- **Deploy node-first.** An unknown field decodes as `NOT_SET` with the header
  still readable, which is the property that makes node-first safe.

`LoraHeader.burstCount == 0` already means "single-shot, no deferral" — timed
frames use it as-is, so the node's existing burst-end logic is untouched.

---

## 9. Prerequisite defects

Two of these sit directly on the critical path.

**The transmit API cannot express a single-shot frame.** `sendPacketBurst`
unconditionally overwrites `header->burstcount = burstCopies`
(`lora_tracker.cpp:420`), and `setBurstCopies()` is a *global* on the tracker,
not a per-frame property — which is why the drift test has to set it and restore
it around the whole test (`lora_client.cpp:1976`, `:1997`). With two nodes in
different modes, a global is unusable. This must become

```cpp
send(buf, len, TxPolicy{ copies, mark_us });
```

before anything timed can coexist with ordinary traffic for another node.

**D5 is consequently not implemented.** `send_schedule_config` sets
`burstcount = 0` intending single-shot (`lora_client.cpp:2025`), and
`sendPacketBurst` overwrites it and sends 17 copies. At the measured 95.3 ms
airtime against an 88 ms period, `vTaskDelayUntil` never blocks (FreeRTOS
advances `*pxPreviousWakeTime` unconditionally on overrun), so those copies go
out back-to-back: ~1.6 s of continuous transmission with no RX gaps. Two tests
assert D5's *justification*; none asserts the behaviour. The `TxPolicy` change
fixes both.

**`responseWindowMs` serialises the transmit task.** A 400 ms `vTaskDelay` after
*every* burst (`lora_tracker.cpp:310`) would silently delay any timed frame
queued behind it. In timed mode the response window is the UL slot in the grid,
not a blocking delay in the TX task.

Two smaller ones, both cheap:

- `loradevices.yml:221` still says the node regresses against "88235 us
  spacing". The real constant is 88000, and `drift_estimator_test.cpp:135-146`
  records that the 88235 assumption cost three firmware revisions at −2663 ppm.
  Stale comments in this area are expensive.
- `CONFIG_FREERTOS_HZ = 1000` is load-bearing — for the 88 ms slot, for the
  node's hard-coded `kBurstTxIntervalMs = 88`, and for `esphome::delay(1)` — and
  is **not pinned** in `loradevices.yml`. At IDF's 100 Hz default,
  `pdMS_TO_TICKS(88)` becomes 80 ms and every burst-end prediction silently
  breaks. Pin it next to the four options already there.

---

## 10. Test strategy

The host harness already has the right shape; extend it rather than inventing a
second one.

- **`TimedGrid`** — mark computation, superframe/slot assignment, `roundIndex`
  wraparound. Dependency-free header, like `BootPolicy.h`.
- **`TimedModePolicy`** — promotion/demotion as a pure function: inputs are ppm
  validity, span, residual, consecutive misses, RTC source, confirmation age;
  output is BURST/SYNCING/TIMED. This is where §5's asymmetry rule gets *pinned
  by test* instead of argued in a document.
- **The negative test that matters:** "hub TIMED + node WINDOWED" must be
  unreachable — assert the policy cannot produce it from any input combination.
- **Shared `time_on_air`** pinned against the numbers already in
  `auto_mode_proto_test.cpp` (95.296 ms for 152 B).
- **Window sizing** against the `drift_us_over` cases already in
  `drift_estimator_test.cpp`.
- `sim_clock.h` needs a µs timeline alongside its ms `set_timeout`/`set_interval`
  model; the node shims already model `esp_timer` one-shots
  (`shims_node/esp_idf_stubs.c:171`).

---

## 11. Sequencing

| phase | content | gate before the next phase |
|---|---|---|
| **P0** | §3 determinism. No protocol change, no new mode. | p99 TX residual < 200 µs, published as a hub sensor |
| **P1** | §4 grid anchor + `TxPolicy` (§9). On air: unchanged. Node fits ppm passively and reports it. | ppm converges in the field, on **both** nodes, over days |
| **P2** | Sleep-clock characterisation (§7) + `rtcSlowSrc` reporting. | a number for RTC drift across light sleep |
| **P3** | Timed **downlink** only: single shot at the mark to a confirmed node; instant burst fallback. Uplink unchanged (hub is in continuous RX; it costs nothing). | command success rate unchanged over a week |
| **P4** | Timed **uplink**: node answers in its UL slot; CAD dropped (the slot *is* the collision avoidance, and CAD is itself a variable-latency step). Hub reserves the slot. | — |
| **P5** | Deep-sleep windows — **only if P2 says the RTC supports it.** | — |

P0 and P1 deliver most of what is reachable today, and they de-risk everything
after: P1 replaces a single bench measurement with continuous field evidence, at
zero airtime cost and zero risk to the link. **P3 is the first phase that can
break anything**, and it is guarded by the §5 asymmetry rule and by P1's data.

---

## 12. What this does not solve

- **Hub airtime is still linear in node count.** Timed mode makes each unicast
  17× cheaper but does not make it shared. Constant-in-N needs the broadcast
  beacon + pending bitmap of `wake-cost-proposal.md` Tier 3, which this design
  is a precondition for rather than a substitute for.
- **A single copy has no diversity.** Against non-network 433 MHz interference,
  17 copies are genuinely more robust than 1. That is the real cost of this
  change, and it is why the burst fallback must be *immediate on the first
  unacked frame* rather than after a timeout.
- **Nothing here shortens the wake itself.** `wake-cost-proposal.md` Tier 1
  (`sleepOk`) is independent and cheaper; do it first if it is not done.

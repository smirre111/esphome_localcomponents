# Five protocol questions, answered

Investigated 2026-08-30 against the current code and logs.

Short version: **(1) and (5) are the same project** — a scheduled-RX scheme is
blocked on sub-second time sync, not on the crystal. **(2) is already half
done.** **(3) rests on a false premise** — proto3 already gives the saving, and
`optional` would make frames slightly *larger*, though it fixes a real bug we
have seen. **(4) is cheap and worth doing**, but needs care about resolution.

---

## 1. Is the TimeSync offset taken from an RX interrupt?

**No.** The chain is:

```
DIO0 ISR (myinterrupts.cpp)  →  queues a flag ONLY, no timestamp
LoraInterface RX task        →  rx_buffer->timestamp = xTaskGetTickCount()   (tick resolution)
CmdDispatcher::handleTimeSync→  settimeofday() at PROCESSING time
```

The ISR captures nothing. The one timestamp that exists is a FreeRTOS tick taken
in the RX task, and it is not used for the clock — the clock is set when the
message is finally decrypted and dispatched, so the error includes the whole
ISR → queue → task → FIFO read → decrypt → dispatch latency.

**But that latency is not the limiting factor.** `TimeSync.epoch` is
`uint64` **whole seconds**:

```protobuf
message TimeSync {
    uint64 epoch     = 1;  // Unix UTC seconds at hub TX time
    int32  utcOffset = 2;
    uint64 dstNext   = 3;
}
```

So the protocol cannot express better than ±1 s no matter where the timestamp is
taken. The observed "drift correction 0 / +1 s" is quantisation, not measurement.
Adding an RxDone timestamp today would buy nothing.

**To actually improve it, three things are needed together:**

1. a sub-second field — `uint32 epochMillis`, or redefine `epoch` as ms;
2. timestamp at **RxDone** (DIO0) in the ISR, and subtract the packet airtime to
   recover the SFD instant. Airtime is computable and deterministic here —
   44.1 ms for a 62 B frame at SF7/BW500/CR4-8;
3. correct for **which burst copy arrived**. The header already carries
   `burstIndex` and `burstCount`, and copies are 88 ms apart, so the node can
   compute the hub's copy-0 instant as `rx_time − index × 88 ms − airtime`. That
   field already exists and is already read for reply deferral.

Point 3 is the neat part: the burst, which looks like pure overhead, is already
carrying the information needed to undo its own timing ambiguity.

---

## 2. Does a cover-movement wake also send a beacon, and can it carry status?

**A beacon is sent, and it already carries the status.** `NodeWakeBeacon` has
`voltage` (5) and `position` (6), and the logs show it:

```
Sending BEACON (reason=2 resume=1 clock=1 v=14.04 pos=1.00)
```

Every wake — check-in, button, or scheduled event — goes through the same
beacon-first path, so the merge you are describing is already in place *before*
the move.

**The duplication is after the move, not before it.** During the 06:00 open the
node published **9** position updates (`MotorCtrl: Publishing state`), which the
hub logged as `Position: 0%, 5%, 14%, 24%, 36%, 50%, 65%, 82%, 100%`. Each is a
separate uplink of ~20-30 B.

Whether that is worth changing depends on what the intermediate positions are
for. They give Home Assistant a live-moving cover, which is nice but costs 9
frames for a 36 s move. Options, cheapest first:

* **Throttle to every ~20 % of travel** — 5 frames instead of 9, HA still
  animates.
* **Send only the final position**, plus the existing end-of-move battery
  measurement. 1 frame instead of 9; HA jumps rather than animates.
* Leave it — uplinks are single-shot and ~25 ms each, so 9 of them is ~0.2 s of
  airtime against a 36 s move. Given §"power" below, this is not where the
  battery goes.

Recommendation: **leave it unless the airtime matters for collision reasons**.
At ~1.5 mA awake, 9 extra frames inside a move that is already happening costs
almost nothing.

---

## 3. Would `optional` fields make frames more efficient?

**No — the premise is inverted for proto3, and this file is `syntax = "proto3"`.**

In proto3 without `optional`, a scalar field at its default value (`0`, `false`,
`""`) is **not serialised at all**. That is already the saving you are looking
for. `ScheduleEntry.kind` is a live example: it is declared, never set, and
therefore costs zero bytes on the wire.

Adding `optional` gives *explicit presence*. A field explicitly set to `0` would
then **be** serialised — 2 bytes where it previously cost nothing. So `optional`
can only make frames the same size or larger.

**It does fix a real problem, though, and we have hit it.** Without explicit
presence you cannot distinguish "not set" from "set to zero". Node 1 emitted
beacons with `v=0.00`: those were ADC read failures, but the hub cannot tell them
from a genuine zero-volt reading, and any battery trend fitted through them is
corrupted. `optional float voltage` would let the hub skip the sample instead of
plotting it.

**Tooling:** protobuf-c 1.5.2 and protoc 3.21.12 are installed here; proto3
`optional` has been stable since protoc 3.15 and supported by protobuf-c since
1.4, so it generates `has_voltage` alongside `voltage` on both sides. **There is
no ESPHome-specific obstacle** — the hub compiles the same generated stubs, and
`regen_stubs.sh` already writes both copies.

Recommendation: **use `optional` for the telemetry fields where zero is a valid
reading and also a plausible failure** (`voltage`, `position`, `current`), for
correctness. Do not expect a size win; expect a small size cost.

---

## 4. A clock-drift test mode

Worth doing, with one caveat that decides the design.

**What we already have:** the beacon carries `nodeEpoch`, and the hub logs
`clock_offset` against its own time. Observed: 0 to +1 s per 6 h sleep, i.e.
**≤46 ppm**, consistent with a 32.768 kHz crystal at ±20 ppm.

**The caveat:** at 1-second wire resolution, a short test interval measures
nothing. At 20 ppm, drift over 10 minutes is 12 ms — invisible. Waking every
n minutes and reading a whole-second offset would return zeros until the
accumulated error crosses a second.

Two designs that do work:

* **Cheap, no protocol change — accumulate, then fit.** Set
  `checkin_interval` short (10 min) and let it run for a day. The *cumulative*
  offset grows linearly, so 144 samples quantised to 1 s still give a good slope
  fit. This measures exactly the thing that matters — drift per unit sleep — and
  needs only a config change plus a fit over the existing `clock_offset` log.
  Cost: 144 wakes/day instead of ~6, so run it deliberately and stop it after.
* **Better, needs the §1 work — millisecond epoch.** With `epochMillis` and
  RxDone timestamping, a single 10-minute interval resolves 12 ms directly and
  the test finishes in an hour rather than a day.

Recommendation: **do the cheap version first.** It answers "is the crystal
within spec" with no firmware change, and its result tells you whether the
millisecond work in §1 is even needed.

---

## 5. A hub RTC and a reduced-activity interactive protocol

The idea is sound and the arithmetic supports it — but it is blocked on §1, not
on hardware.

**Today:** the node opens 3 RX windows per 1500 ms round (`rxSlotsPerRound = 3`,
`rxIntervalMs = 500`), and the hub sends 17 copies at 88 ms spacing to guarantee
one lands in a window. Neither side knows *when* the other will be listening, so
both brute-force it.

**With synchronised clocks** both ends can agree on an instant. The node opens
one narrow window; the hub transmits only into it.

| scheme | node RX duty per round |
|---|---|
| today: 3 × 500 ms | 100 % |
| 1 × 500 ms | 33 % |
| 1 × 200 ms | 13 % |
| 1 × 100 ms | 6.7 % |
| 1 × 50 ms | 3.3 % |

And the hub's burst could drop from 17 copies to 2–3 for margin, cutting
downlink airtime by ~5×.

**What sets the window width is time since last sync:**

| since sync | drift @20 ppm | @50 ppm |
|---|---|---|
| 1.5 s | 0.03 ms | 0.08 ms |
| 1 min | 1.2 ms | 3 ms |
| 10 min | 12 ms | 30 ms |
| 1 h | 72 ms | 180 ms |
| 6 h | 432 ms | 1080 ms |

So a 100 ms window is comfortable if the two ends re-sync at least every
~10 minutes, which in interactive mode they can do essentially for free — every
frame exchanged is an opportunity to re-sync.

**The blocker:** the wire carries whole seconds, so the *sync error alone* is up
to 1000 ms — an order of magnitude worse than the drift the scheme must
tolerate. **A hub RTC does not fix this.** A better hub clock reduces the hub's
own error; it does nothing about a protocol that cannot express milliseconds.

### Suggested order

1. **§1 first**: `epochMillis` + RxDone timestamping + `burstIndex` correction.
   Without it nothing else in this section is possible.
2. **Measure the real sync error** once ms resolution exists — the residual after
   correction, not the crystal spec.
3. **Then** narrow the RX window, starting conservatively (1 × 200 ms) and
   tightening as the measured error allows.
4. **Then** reduce `txSlotsPerRound` from 17. Note this is *already* the top
   recommendation in `optimization-analysis.md`, and it has a cheaper first step
   that needs none of the above: log which `burstIndex` the node actually
   accepts. If it is usually copy 2 or 3, 17 is over-provisioned today and can be
   cut without any timing work at all.

### A caution on the motivation

The stated goal is reducing **hub** activity. The hub is mains-powered; its
airtime matters for channel occupancy and collisions, not for battery. The
node's RX duty does matter for battery — but per the power analysis, an awake
node draws only ~1.5 mA because light sleep is already effective, and in
automatic mode the node is asleep 99.7 % of the time anyway.

So this work is worth doing for **channel occupancy and interactive-mode battery
life**, and it is worth being clear that it will not move automatic-mode battery
life measurably. The thing that would is the deep-sleep leakage current, which is
still unmeasured.

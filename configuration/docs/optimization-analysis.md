# Optimisation analysis — airtime, frame length, protocol, power

Measured 2026-08-29, after the code-quality work. This is the "other parameters"
pass: what the system costs on the air and in battery, rather than how the code
reads.

Radio: SF7, BW 500 kHz, CR 4/8, preamble 8, CRC on, 433 MHz.

---

## 1. What a frame actually costs

Airtime at these settings, and what the 17-copy hub burst does to it:

| payload | airtime | ×17 burst | share of the 1500 ms round |
|---|---|---|---|
| 20 B | 19.5 ms | 332 ms | 22 % |
| 40 B | 29.8 ms | 506 ms | 34 % |
| **62 B** (ScheduleConfig, 2 entries) | **44.1 ms** | **750 ms** | **50 %** |
| **69 B** (ScheduleConfig, 3 entries) | **48.2 ms** | **819 ms** | **55 %** |
| 100 B | 66.6 ms | 1133 ms | 76 % |
| 150 B | 95.3 ms | 1620 ms | **108 %** |

**There is a hard ceiling around 130 bytes.** Above that a single burst cannot
finish inside its own 1500 ms round. Nothing today approaches it — the largest
observed frame is 69 B — but it constrains any future message, and it is not
written down anywhere in the code. A 4-entry schedule is ~76 B; an 8-entry one
(the documented maximum) would be ~110 B and put a burst at ~85 % of the round.

**Worth noting:** the node→hub direction sends **once**, not 17 times. The
asymmetry is deliberate (the hub listens continuously, the node in windows), and
it means uplink airtime is negligible next to downlink.

---

## 2. The 17× burst is the dominant cost — and it is unmeasured

Every downlink costs 17× its airtime. A full registration handshake is roughly
five downlink bursts (ClientConfig, CoverConfig, LoginMsg, TimeSync,
ScheduleConfig) ≈ **3.5–4 s of airtime**, and node 2's boot log shows the node
awake **~7.4 s** for it (REGISTER at 1.15 s, schedule applied at 8.52 s).

Is 17 the right number? **The data to answer that is already on the wire and is
being thrown away.** `LoraHeader` carries `burstIndex` and `burstCount`, and the
node reads them to schedule its deferred reply — but nothing records *which
index the node actually acted on*. If the node typically decodes copy 2 or 3,
17 is heavily over-provisioned and could drop to, say, 8 — halving downlink
airtime and the hub's TX energy.

**Cheapest useful change in this whole document:** log `burstIndex` at the node
when a frame is accepted, and report it in the next beacon or simply in the
serial log. One field, no protocol change, and it turns a guess into a
measurement. Do this before touching `txSlotsPerRound`.

I am deliberately not recommending a burst reduction on the current evidence —
the node's RX is windowed (3 × 500 ms per round), and the margin that 17 copies
buys against a missed window is exactly what a naive reduction would spend.

---

## 3. Protocol: bursts sent to nodes that cannot hear them

Observed in the hub log: **68 × `Login not acknowledged — retry n/24`**. Each of
those is a 17-copy LoginMsg burst transmitted at a node that is asleep.

`schedule_login_retry_()` backs off exponentially (base << retry_count, capped
at 1 h) but **never consults `is_node_awake_()`**, which this same class already
implements. The retry is time-based only.

Two follow-ons, and the second matters more:

1. Gating the retry on `is_node_awake_()` would suppress bursts into a sleeping
   node outright.
2. **But the wake model is wrong for automatic mode.** `is_node_awake_()`
   computes `last_sleep_epoch_ + sleep_duration_` — the *interactive* fixed
   sleep. An auto-mode node's wake comes from its schedule plus `beaconLead_s`,
   or from `checkinInterval_s`; `sleep_duration_` does not describe it. The hub
   knows all three (it pushes them), so it can compute the real next wake — it
   just doesn't.

Fixing (2) would also let the hub *land* a deferred login exactly when the node
wakes, instead of retrying blindly and hoping. It is the same information that
already drives `schedule_startup_login_()`, which is the one place the model is
used today.

This is the highest-value protocol item: it removes wasted airtime, wasted hub
TX energy, and — because a node that wakes into an in-progress burst has to sit
through it — some node awake time too.

---

## 4. Power: awake time dominates, and the two nodes differ

The node's battery cost is dominated by awake seconds, not by TX. Measured:

* full register → config → login → timesync → schedule: **~7.4 s awake**
* a resumed session skips most of that

So anything that converts a full handshake into a resume is worth several
seconds of radio-on per wake. Two bugs that silently disabled the resume path
have already been fixed (the `getRegistered()` flag in v1.0.22, and the persist
peer defaulting to 0xFF). **Whether resume now actually happens in production is
not yet established** — see §6, the beacon's `resume` flag does not measure it.

Observed battery: node 1 **11.02 V**, node 2 **14.03 V**. I do not know the pack
chemistry or what is normal here, so I am flagging the gap rather than calling
it a fault — but it is large enough to be worth a look, and node 1 is also the
node still on older firmware.

---

## 5. Things I checked that are NOT worth doing

Recorded so they are not re-investigated:

* **`ScheduleEntry.kind`** — documented as "telemetry / HA display only" and
  never read by the node, so it looked like free bytes. It is not: the hub never
  sets it, and proto3 omits default values, so it costs nothing on the wire.
* **Field numbers above 15** — `timesync = 16`, `schedule = 17`, `beacon = 16`
  each pay a 2-byte tag instead of 1. Real, but it is 1 byte per frame and
  renumbering is a breaking wire change requiring both ends to update
  simultaneously. Bad trade.
* **Memory** — hub at 13.0 % RAM / 52.0 % flash, node at 38 % flash free.
  Neither is a constraint; no reason to optimise for size.
* **Uplink airtime** — single-shot, negligible beside the 17× downlink.

---

## 6. Instrumentation gaps (these misled me today)

Two measurement traps worth fixing before trusting any further numbers:

* **`hub_events.log` is regex-filtered** by `watch_hub.ps1`. "Pushing config",
  "Config already in sync" and "LoginMsg sent" are not in the pattern, so
  counting them yields 0 — which reads as "never happens" rather than "not
  captured". I made exactly this error while writing this document.
* **The beacon's `resume` flag does not mean what its name suggests.** It is
  `session_.hasValidState()` — "I have persisted state" — not "I skipped the
  handshake". All 64 observed beacons report `resume=1` while 26 full REGISTER
  cycles also occurred. To measure resume effectiveness, count REGISTER cycles
  per wake, or add an explicit signal.
* The hub log has **no dates**, only HH:MM:SS, so any time-based filtering
  silently mixes days.

---

## Recommended order

1. **Log which `burstIndex` the node accepts** (§2). One line, no protocol
   change, and it is the prerequisite for the single biggest airtime saving.
2. **Teach the wake model about automatic mode, then gate login retries on it**
   (§3). Removes 68-observed wasted bursts and lands logins when the node is
   actually listening.
3. **Establish whether beacon-first resume is working** (§4/§6) — it is worth
   several seconds of awake time per wake and two bugs against it have been
   fixed, but nothing currently proves it fires.
4. Write the ~130 B frame ceiling into the code near `txSlotsPerRound`, so a
   future message does not discover it on the air.

Not recommended: reducing `txSlotsPerRound` before step 1; renumbering proto
fields; optimising for memory.

---

## Not measured

* **Actual current draw.** Everything above about power is derived from awake
  *time*, not from a measurement. A scope or a coulomb counter on the node would
  make the trade-offs concrete; I have not done that.
* **Duty-cycle compliance.** The numbers in §1 are airtime occupancy within a
  burst round, not a regulatory assessment, and Reinhold has previously said the
  duty cycling is not the concern here.
* **Range/link margin.** No RSSI/SNR analysis; the 17× burst may be buying link
  reliability that a bench measurement would not reveal.

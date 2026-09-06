# The MAC layer: boundary, sublayers and KPIs

`layering-proposal.md` (2026-08-31) argued for a link/application split from a
list of bugs. This document does the next step: it **draws the boundary
precisely enough to test against**, defines the MAC-layer KPIs, and stages
security and sequencing as optional sublayers that can be switched on after the
timing and frame-error numbers are trusted.

The trigger was concrete. `test-plan.md` §10's on-air test mode had a gap: a test
frame that exercises the real receive path must traverse address filtering, the
replay window and AEAD — but must not drive a motor. The best answer available
without a boundary was "decode fully, then dispatch to a sink that acks and
stops", which is a workaround for a missing layer. **With the boundary, the
question dissolves: a MAC-layer test frame is consumed by the MAC and never
reaches the application at all.**

Everything here is analysis. No code has been changed.

---

## 1. Where the boundary is

Extending `layering-proposal.md` §3, with the MAC service split from the parts
above and below it.

```
  ┌──────────────────────────────────────────────────────────┐
  │ APPLICATION   covers, position, roll geometry, schedules, │
  │               automatic-mode policy, battery telemetry    │
  └──────────────────────────────────────────────────────────┘
     mac.send(payload, {reliable, deadline})  →  delivered | failed
     mac.on_payload(cb)          mac.transaction_complete()
     mac.request_wake(when)      mac.link_state()
  ┌──────────────────────────────────────────────────────────┐
  │ MAC-2  security      AEAD, base nonce, session lifecycle  │  optional
  │ MAC-1  sequencing    frame counter, replay window, dedup  │  optional
  │ MAC-0  access        addressing, framing, RX windows,     │  ALWAYS
  │                      burst/slot scheduling, CAD, backoff, │
  │                      ack/retransmit, timing (T0, grid)    │
  └──────────────────────────────────────────────────────────┘
  ┌──────────────────────────────────────────────────────────┐
  │ PHY           SX1278: SF/BW/CR, preamble, CRC, RSSI/SNR   │
  └──────────────────────────────────────────────────────────┘
```

The three modes of `implementation-plan.md` are **entirely MAC-0 constructs**.
Mode A's burst geometry, Mode B's slot grid and guard band, Mode C's RX1/RX2
offsets: none of them has an application-layer term. This is the strongest
argument that the split is the right one — the plan that motivated all this work
never once needs to know what a blind is.

### Today's code against that boundary

| file | PHY | MAC-0 | MAC-1 | MAC-2 | APP |
|---|---|---|---|---|---|
| node `components/lora/lora.cpp` | ● | | | | |
| node `LoraInterface.cpp` | ● | ● RX windows, `symTimeout`, CAD, backoff, TX queue | | | |
| node `CmdDispatcher.cpp` | | ● address filter, burst deferral, drift | ● `msgid` replay | ● AEAD calls | ● cover, schedule, battery |
| node `SessionManager.cpp` | | | ● replay window | ● session, base nonce | |
| node `MotorCtrl.cpp`, `Scheduler.cpp` | | | | | ● |
| hub `lora.cpp` | ● | | | | |
| hub `lora_tracker.cpp` | ● | ● burst TX, RX poll, response window | | | |
| hub `lora_client.cpp` | | ● tracked ops, retransmit, wake model | ● `msgid` | ● AEAD, login | ● cover, schedule, telemetry |
| hub `loracover/` | | | | | ● |

Two rows carry the whole problem. `CmdDispatcher.cpp` and `lora_client.cpp` each
span **four layers**, which is `layering-proposal.md` §1's finding restated with
the layers named: that is why changing when a move ends broke position
reporting, and why a mode command vanished into an enum with no slot for it.

---

## 2. What is on the wire today, and what is missing

```
  SX1278 payload
  ┌───────────────────────────────────────────────────────────┐
  │ protobuf LoraClientOperationMessage                       │
  │   field 1: LoraHeader { destAddress, destSubnet,          │
  │                         senderAddress, msgId,             │
  │                         burstIndex, burstCount }          │
  │   oneof cmd: operation | sysop | clientconfig | ...       │
  │              | timesync | schedule | drifttest            │
  │              | encrypted (AEAD blob)                      │
  └───────────────────────────────────────────────────────────┘
```

**There is no MAC header.** The addressing fields are protobuf field 1 of the
*application* message, so:

- **MAC filtering requires the application schema.** To decide "is this frame
  mine?", the node protobuf-decodes a message type whose other fields describe
  blinds. A schema change in the application half is a change to the decoder the
  MAC depends on.
- **The header is neither fixed-offset nor fixed-length.** Protobuf varints and
  field ordering mean the destination address cannot be read at a known offset,
  so it cannot be read early, cannot be read from a truncated frame, and cannot
  be read without the full decoder.
- **`burstIndex` / `burstCount` are pure MAC-0** — burst scheduling and reply
  deferral — sitting in the same message as `LoraCoverOperation`.
- **`FrameCrypto.h` already reconstructs the fixed layout.** The AAD is
  `destAddress_BE32 || destSubnet_BE32 || senderAddress_BE32 || msgid_BE32`, a
  16-byte fixed-offset header built by hand at encrypt time. **The fixed MAC
  header already exists; it is just not on the wire.** It lives only inside the
  crypto, which is the one place it must not be the only copy.

### The clean form, and the honest cost

```
  ┌──────────────── MAC header (fixed, 16 B) ────────────────┬──── payload ────┐
  │ ver│flags│destAddr│destSubnet│srcAddr│frameCnt│burst idx/n│ APP bytes / MAC │
  │                                                          │ control message  │
  └──────────────────────────────────────────────────────────┴─────────────────┘
```

with `flags` carrying `encrypted`, `mac_control` (payload is for the MAC, not the
application), and `ack_request`.

This is a **breaking wire change** against a deployed fleet, and the tree already
records what such changes cost: six copies of the protobuf stubs across two
repos, kept in step by hand, with drift that "compiles and links cleanly on both
sides and fails only on the air". It is the right end state and it is not a
prerequisite for anything in `implementation-plan.md`.

**So the boundary is drawn in the code now and on the wire later.** §5's test
frame needs no wire change: it is a new oneof case that the MAC consumes and
never forwards. The compromise is explicit — MAC termination still runs the
application decoder to get there — and it is the difference between a two-week
change and a fleet migration.

---

## 3. `msgId` does three jobs, and that is a bug generator

One `uint32` is currently:

| role | layer | required behaviour on retransmission |
|---|---|---|
| AEAD nonce input (`frameCounter(msgid, downlink)`) | MAC-2 | **must not repeat with different plaintext** — GCM nonce reuse |
| replay-window key (`acceptRxId`: `msgid > rx_id_`) | MAC-1 | **must repeat** — a fresh id means the node executes the command twice |
| application command identity | APP | **must repeat** — the ack must be attributable to the same command |

The MAC-1 and MAC-2 requirements are directly contradictory the moment the
content changes mid-retry, and `implementation-plan.md` §4.7 documents the exact
path: `tx_tracked_op_` re-reads `op_position_` at pack time, so a user moving the
blind during a retry encrypts different plaintext under the same nonce. The
plan's remedies — retransmit byte-identical frames, add a cached ack — are both
**patches for the overload**, and both are correct given it.

Under the split they are separate fields with separate rules:

- **`frameCnt`** (MAC-1) — per-link, monotonic, increments once per *distinct*
  frame. A retransmission reuses it and the identical bytes, so nonce reuse is
  impossible by construction and the replay window admits the retransmission as
  a duplicate rather than rejecting it as stale.
- **`cmdId`** (APP, inside the payload) — constant across every MAC
  retransmission and across a MAC give-up-and-retry-later. Idempotency and the
  cached ack live here, where "arrived, but the ack was lost" is expressible.

This is the LoRaWAN arrangement (`FCnt` + MIC at the MAC, `FPort`/`FRMPayload`
above it), reached from this system's own defect list rather than by imitation.

**Scope note:** this does not have to ship with Mode B. `implementation-plan.md`
§8's B4 already carries the byte-identical-retransmit and cached-ack work, which
is the overload made safe. Splitting the field is the version that stops
generating defects, and it belongs with the wire change of §2.

---

## 4. Sublayers, and why they are switchable

MAC-0 is unconditional. MAC-1 and MAC-2 are **independently switchable for
test**, which is a testing property first and an architectural claim second.

| sublayer | provides | off by default in test | cost it hides when on |
|---|---|---|---|
| **MAC-0** access | addressing, framing, windows, scheduling, ack, timing | — always on | — |
| **MAC-1** sequencing | frame counter, replay window, duplicate detection | **yes** | frames rejected by the window are invisible in a naive FER |
| **MAC-2** security | AEAD encrypt/decrypt, base nonce, session lifecycle | **yes** | decrypt time sits inside the measured turnaround |

Three reasons this staging earns its keep, beyond convenience:

1. **Attribution.** Run MAC-0, record the KPIs. Switch on MAC-1, re-record: any
   change in frame yield is the replay window, and any change in turnaround is
   the counter check. Switch on MAC-2, re-record: the delta is AEAD. Each
   sublayer's cost becomes a **measured number** rather than an estimate. This
   matters directly — `implementation-plan.md` §4.3's servable-slot geometry
   turns on a node turnaround that has never been measured, and AES-GCM
   decryption of up to 152 bytes is inside it.
2. **A session is not always available.** Before the base-nonce exchange the link
   is already unencrypted today, so MAC-0 is not a new mode — it is the state
   every node passes through on every cold boot. Testing it is testing a real
   configuration.
3. **Failure separation.** With MAC-2 on, a wrong key, a wrong AAD, a byte-order
   slip and a missing direction bit all present identically as
   `psa_aead_decrypt: -149`. With MAC-2 off, a frame-yield problem is
   unambiguously radio or timing. The two failure families stop being confusable.

### Security of the test path

The frames being *measured* may be unencrypted. **Arming the test must not be.**

`ModeTest{enable}` travels the normal application path and is therefore
authenticated once a session exists; the test frames it then produces need no
session. That split is the right one — an attacker who can inject unauthenticated
test frames can waste receive time, which the node's own duration cap already
bounds, but cannot start the mode, change a node's slot, or suppress a window.

Two rules follow, and they are not optional:

- **A node with no session must not accept a `ModeTest` arm command.** Otherwise
  the unauthenticated bootstrap window becomes a remote way to hold 32 nodes in a
  test mode.
- **`MODE_SWEEP` (deliberate mis-arming, `test-plan.md` §10.6 HW-2) is
  bench-only** and must be refused by any node not explicitly flagged as a bench
  node. It intentionally breaks reception.

---

## 5. The MAC-layer test frame

A `ModeTest` grid frame is a **MAC control frame**: consumed by MAC-0, counted,
timestamped, and **never handed to `mac.on_payload`**. There is no sink, no inert
command, no application involvement, and no motor to guard against.

| property | value | why |
|---|---|---|
| carrier | its own oneof case; `mac_control` flag once §2's header exists | the frame is the ruler, as `DriftTest`'s already is |
| terminates at | MAC-0 | the application is not in the measurement |
| encryption | **off by default**, MAC-2 flag turns it on | §4 |
| frame counter | **off by default**, MAC-1 flag turns it on; `seq` is carried regardless as an opaque mark | §4, and `seq` must survive counter-checking being disabled |
| length | padded to 25 / 45 / 60 / 152 B | sweeps the frame table of `implementation-plan.md` §2.1 |
| reply | MAC-level echo, emitted by MAC-0 with no application round trip | isolates the MAC turnaround from application processing — the quantity §4.3 of the plan needs |

**The MAC echo is the part that makes the turnaround measurement honest.** Today
the only way to get a reply is a command the application answers, so any measured
turnaround includes application dispatch, and `implementation-plan.md` §4.3's
20 ms budget — which "budgets **zero** for DRAIN" — would be compared against a
number containing more than DRAIN. A MAC echo measures `RxDone → TX fire` and
nothing else. Running it with MAC-1 and MAC-2 on then adds their costs back,
separately, per §4's attribution argument.

---

## 6. MAC KPIs

The KPIs are defined on **frames**, not on commands. This is the substantive
change from `test-plan.md` as first written, which counted `framesDecoded` and a
success rate whose denominator mixed layers.

### 6.1 The frame funnel

Each stage is counted separately, because each has a different owner and a
different fix.

| # | stage | counted by | a loss here means |
|---|---|---|---|
| 0 | **offered** | hub MAC log (`seq`, `t_fire`, `len`, `copies`) | — |
| 1 | **on air** | witness receiver | 0→1 lost: the hub did not transmit. A dropped `RegOpMode = TX` on a 1-tick semaphore timeout is exactly this, and it is silent |
| 2 | **detected** | node: RxDone or RxTimeout fired | 1→2 lost: window misplaced, or signal below sensitivity |
| 3 | **CRC valid** | `RegIrqFlags` | 2→3 lost: collision or interference — **this is the true FER** |
| 4 | **addressed to me** | MAC-0 filter | not an error: another node's traffic |
| 5 | **counter accepted** | MAC-1 (when on) | replay, duplicate, or a counter desync |
| 6 | **MIC valid** | MAC-2 (when on) | wrong key, wrong AAD, session mismatch |
| 7 | **delivered to app** | — | **out of scope for these tests** |

### 6.2 The rates, named separately

Three quantities are routinely called "frame error rate" and have different
fixes. Naming them apart is most of the value.

| KPI | definition | attributes to |
|---|---|---|
| **FER_air** | `1 − crc_valid / on_air` | channel: interference, range, collisions |
| **FER_link** | `1 − crc_valid / offered` | channel **plus the transmitter's own failures** — the only rate that catches a frame that was never emitted |
| **WMR** — window miss rate | `1 − windows_hit / windows_armed` | **timing**, and it is mode-specific: Mode A's geometry, Mode B's guard band, Mode C's offsets |
| **DUP** | `duplicates / crc_valid_addressed` | MAC-1 only; burst copies make this large and healthy in Mode A |
| **MIC_FAIL** | `mic_fail / counter_accepted` | MAC-2 only; non-zero is a session bug, never a channel effect |

`FER_link − FER_air` is the transmitter's own loss, and it is **only observable
with the witness receiver**. Without it the two collapse into one number and a
hub that silently fails to transmit is indistinguishable from a node that fails
to hear — opposite fixes, same symptom.

**WMR is the KPI that actually distinguishes the three modes.** FER is a property
of the channel and should be within noise across modes at equal payload and
equal transmit power; if it is not, the modes are not being compared fairly.
Everything Mode B changes shows up in WMR and in the timing KPIs.

### 6.3 Timing KPIs

All as distributions — `{min, p50, p95, p99, max, n}`, never means. `T0` per
`implementation-plan.md` §2.1.

| KPI | definition | gate it serves |
|---|---|---|
| **phase error** | `T0_measured − T0_predicted` | B2: p99 < 2 ms, and **unimodal** |
| **arm residual** | `t_arm_actual − t_arm_target` | B5: p99 < 200 µs; the ±100 µs typical / milliseconds p99 claim of §2.4 |
| **MAC turnaround** | `t_reply_fire − t_rxdone`, MAC echo only | §4.3's servable-slot rule: **k+2 breaks above 36.5 ms** |
| **one-shot error** | `t_actual − t_target` for an `esp_timer` one-shot under the production power profile | §12.8 — the whole ARM mechanism rests on this and nothing asserts it |
| **ppm** | `DriftEstimator` slope + sample count + `measuredPeriodUs` | §12.5, and `measuredPeriodUs` is the ruler-mismatch alarm |

### 6.4 What is deliberately not a MAC KPI

- **Command success rate.** It mixes MAC loss with application retry and
  application idempotency. It is the right number for judging the *product* and
  the wrong one for judging a mode.
- **Wake duration and energy per wake.** Real, and dominated by application
  behaviour (schedule evaluation, position reporting) plus MAC-0 windows.
  Measured, reported, and attributed to the wake path — not to the MAC.
- **Anything about blinds.** Position error, travel time, slat slack.

---

## 7. What this does not require

Keeping the scope honest, because `layering-proposal.md` §5's caveat still
stands — the system works, and the argument is about the rate of future bugs.

| not required | why |
|---|---|
| the fixed-offset wire header (§2) | the test frame rides the existing envelope; the header is the end state, not a prerequisite |
| splitting `msgId` (§3) | B4's byte-identical retransmit + cached ack make the overload safe without splitting it |
| a big-bang refactor | `layering-proposal.md` §4's incremental path is unchanged: session/crypto, then delivery, then wake scheduling |
| any application change | the MAC test frame never reaches the application, which is the whole point |

What **is** required, and it is small: MAC-0 must be able to consume a frame,
count it, timestamp it and echo it without the application. That is one dispatch
branch on each side, and it is the boundary's first real customer.

---

## 8. Order

1. **Name the layers in the existing code.** No moves — comments and a header
   listing what belongs where. It is what makes the next steps reviewable.
2. **MAC control frame + MAC echo.** One dispatch branch per side. Unlocks the
   whole KPI set of §6 at MAC-0.
3. **Split the KPI counters by funnel stage** (§6.1), replacing the mixed-layer
   success rate.
4. **MAC-1 / MAC-2 switches**, with the arming rules of §4.
5. Then, and only if the fleet's defect rate says it is worth it: the wire header
   (§2) and the `msgId` split (§3).

Steps 1–4 are the ones `test-plan.md` depends on. Step 5 is a separate decision
with a fleet migration behind it.

---

## 9. Cross-references

- `layering-proposal.md` — the original argument, from a day's bug list.
- `test-plan.md` — the host tests and the on-air `ModeTest` mode; §10 is scoped
  to MAC-0 by this document.
- `implementation-plan.md` — the three modes, all MAC-0; §2.1 T0, §4.3
  turnaround, §12 open measurements.
- `architecture-review.md`, `hub-refactor-analysis.md` — prior structural work.

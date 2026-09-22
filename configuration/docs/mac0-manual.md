# MAC-0: the mechanism, and how to read its numbers

**What this is.** A working manual for the lowest layer of the LoRa link — what
MAC-0 measures, *how* each number is physically obtained, and how to avoid the
handful of readings that look like results and are not.

**What this is not.** It does not redefine the KPIs — `mac-layer.md` §6 owns
those — and it is not a run procedure; `mac0-final-campaign.md` owns that, and
`bench-runbook.md` owns the bench mechanics. This explains the *mechanism* the
other three assume.

---

## 1. Where MAC-0 sits, and why it is measured alone

MAC-0 is the frame layer: it receives, filters by address, timestamps, counts,
and — for a MAC control frame — replies. A `ModeTest` frame **terminates at
MAC-0**. It is never handed to `mac.on_payload`, so there is no application
dispatch, no command semantics, and no motor involved (`mac-layer.md` §5).

Two layers sit on top and are independently switchable:

| layer | adds | default in a MAC-0 run |
|---|---|---|
| **MAC-1** | frame counter / replay window | off |
| **MAC-2** | encryption + MIC | off |

The reason for the separation is attribution. A "command success rate" folds
MAC loss, application retry and application idempotency into one number, so a
regression in any of the three moves it and none of them can be told apart. It
is the right number for judging the product and the wrong one for judging a
mode (`mac-layer.md` §6.4).

**The MAC echo is what makes turnaround honest.** Any reply produced by the
application includes dispatch time. A MAC echo measures `RxDone → TX fire` and
nothing else.

---

## 2. The three reception geometries

This is the substance. All three modes run the *same* frames over the *same*
radio; what differs is **when the node is listening**, and that is what each
mode's numbers are really about.

### Mode A — free-running receive

The node re-arms a receive window continuously (`kFreeRunRxIntervalMs = 470`),
giving roughly 6.3 % listening duty. The hub compensates by **bursting**: 17
copies at an 88 ms stride, spanning 1408 ms of a 1500 ms round.

Reception is therefore a **coincidence** problem — the hub is buying overlap
probability with airtime. A low `FER_link` here mostly reports listening duty,
not channel loss, which is why the campaign notes `FER_link ≈ 93 %` in Mode A
is *expected* and not a fault.

### Mode B — the grid

Time is divided into rounds of `kRoundUs = 1'500'000` with `kSlotCount = 32`.
Each node owns one slot and opens a single window of `kWindowUs = 29'440` per
round, with `kGuardUs = 14'080` of guard. A beacon every
`kBeaconEveryRounds = 233` rounds (349.5 s) re-anchors the node's phase;
`resyncMaxS = 352` is the staleness limit.

Reception becomes a **prediction** problem. The hub sends one copy, not
seventeen, because it knows where the window is. Everything Mode B changes
shows up in **WMR** and in **phase error** — not in FER, which is a channel
property and should be within noise across modes.

### Mode C — deep sleep, Class A

The node sleeps and wakes on its own schedule. Its receive windows are
**anchored to its own uplink**, not to any hub clock:

```
node uplink T0 ──┬── +1 000 000 µs ──> RX1 opens
                 └── +2 000 000 µs ──> RX2 opens
```

Two consequences follow, and both have caused real defects:

1. **Hub↔node clock drift cannot move a Class A window.** The window hangs off
   the node's own transmission, so the ppm figure does not gate reception in
   Mode C. It feeds only the hub's *prediction* of the next check-in. At
   −130 ppm a 900 s sleep mispredicts by ~117 ms against a check-in window
   measured in seconds — three orders of magnitude inside what the prediction
   needs. This is why Mode C's ppm is recorded but is **not a gate**.
2. **The hub must aim from *that node's* uplink stamp.** The tracker's
   `last_rx_t0_us_v` is global — one radio, one variable, overwritten by every
   frame from every node. Reading it 750 ms after the uplink that triggered a
   reply means that on a busy fleet the hub aims at *someone else's*
   transmission. `send_into_class_a_window_` therefore reads
   `last_uplink_t0_us_`, captured per-node in `admit_frame_`.

The placement ladder declines in a fixed order, and the order matters when
reading a refusal:

| rung | meaning |
|---|---|
| `NotSendable` | no parent/buffer |
| `NotClassA` | node is not in AUTO, or is grid-aligned — **checked before the stamp** |
| `NoUplinkStamp` | Class A, but the hub has never heard this node |
| `WindowsPast` | both windows gone: the node is asleep |
| `Placed` | a frame was aimed at an open window |

---

## 3. The funnel: who counts what, and why it takes two ends

`mac-layer.md` §6.1 defines the stages. The mechanism question is *where each
count physically comes from*, and in Mode C the answer is unusual.

| stage | counted by | how it reaches you |
|---|---|---|
| **offered** | hub | `class_a_stats_.offered++` at the `Placed` return |
| **windows / hits** | node | carried in the **next** beacon's `prevwake*` fields |
| **detected** | node (RxDone/RxTimeout) | same beacon fields |
| **crcValid** | node (`RegIrqFlags`) | same beacon fields |

**Why the node reports its previous wake rather than the current one.** In
Mode C the node is asleep, and *opening its serial port resets it* — the sleep
being the thing under test. So the node cannot be interrogated live. It
accumulates its own funnel during a wake and ships it as fields on the *next*
uplink, where the hub adds it to a running ledger.

### The idempotence key, and why it exists

The hub accumulates only when `prevbeaconmsgid` changes:

```cpp
if (b->prevbeaconmsgid != 0 &&
    b->prevbeaconmsgid != this->class_a_last_prev_msgid_) { ... }
```

Firmware 1.1.2 and 1.1.3 spent **two** uplinks per wake (a `MODE_CHANGED` and a
`TIMER_CHECKIN`). Both carried the *same* previous-wake funnel. Without the key,
one wake would be counted twice and the apparent FER would be silently halved.
1.1.4 fixed the double uplink — but the ledger must not *depend* on that fix
holding, which is precisely what the key buys.

---

## 4. Reading the numbers

```
true FER  = 1 − crcValid / detected     channel loss: collision, interference
hit rate  = hits / offered              did the frame land in an open window
WMR       = 1 − windows_hit / windows_armed   timing; mode-specific
FER_link  = 1 − crc_valid / offered     channel PLUS the hub's own failures
```

`FER_link − FER_air` is the transmitter's own loss and is **only** observable
with a witness receiver. Without one, a hub that silently fails to transmit and
a node that fails to hear are the same symptom with opposite fixes.

### Four readings that look like results and are not

**Cumulative rates hide their own history.** The 2026-09-22 campaign ended at
`offered 20, hits 19` — "95 %". But every sample from 08:02 onward added
`+1 hit` per `+1 offered`; the single miss belonged to one interval-transition
wake at 07:56. The marginal rate was 100 % and the cumulative figure was one old
miss amortised over a growing denominator. **Report the miss and where it
happened, not the ratio.**

**A falling counter is a reboot, not a loss.** The counters are cumulative since
hub boot and reset to zero on OTA.

**An empty window used to be ambiguous — that is why `offered` exists.** Without
a hub-side count, a window that caught nothing could equally mean "nothing was
aimed at it" or "aimed and missed". These have opposite fixes. Any Class A
result taken on a build without the ledger cannot distinguish them.

**A zero-byte HTTP read is not a zero count.** See §5.

---

## 5. Getting the counters out

Five diagnostic sensors expose the ledger. **Address them by entity name, not
by object id:**

```
/sensor/Class%20A%20%E2%80%94%20replies%20offered%20(node%202)
```

The hub logs `Object ID URLs will be removed in 2026.7.0` on every object-id
request, and this is the same reason `object_id:` is not a valid
`sensor.template` option — ESPHome is moving to name-based URLs.

**Discover, never guess.** The root page is a ~174-byte JavaScript shell
containing no entity identifiers at all, so grepping it returns nothing and
looks exactly like a failed upload. The ids and names both live in the
`/events` SSE stream:

```
data: {"name_id":"sensor/Class A — replies offered (node 2)","id":"sensor-class_a_____replies_offered__node_2_",...}
```

Guessing the slug is unreliable in a way that bites precisely where it hurts:
the display name `Class A — frames CRC-valid (node 2)` slugifies to
`class_a_____frames_crc-valid__node_2_` — the internal **hyphen survives**. A
guess of `crc_valid` returns 0 bytes on the one sensor that is the **numerator
of the true FER**, and a script that treats an empty body as `0` will report a
perfect FER from a broken URL.

---

## 6. Running a measurement

Procedure lives in `mac0-final-campaign.md`; the rules that protect the
measurement itself:

- **Never open the node's serial port during a Mode C run.** It resets a
  sleeping node. Every Mode C number must be hub-side or carried in a beacon.
- **Never flash the hub and a node at the same time.**
- **`esphome upload` does not recompile.** Compile explicitly and check
  `config_hash` changed; a 4-second upload has shipped a stale image before.
  Verify the *running* device (its boot banner carries the compile timestamp),
  not the uploader's exit status.
- **One variable per run.** Mode A's grid period must be incommensurate with
  the node's receive period (1093 ms, not 1100); in Mode B the rule inverts.
- **Sample size is part of the result.** "FER 0" means nothing without its `n`.
  The 15-minute cadence yielded n=5 in 80 minutes; 5-minute cadence yielded
  n=21 in under two hours. High check-in rates are a **temporary** bench
  setting — they cost battery and must be restored afterwards.

---

## 7. Known limits

- **No witness receiver**, so `FER_air` and `FER_link` collapse into one number
  and a silent transmit failure is indistinguishable from a receive failure.
- **A promoted QUIET node is invisible to the hub** — a `PhaseReport` needs an
  uplink.
- **Mode C's counter-fit discrepancy is deprioritised, not explained**: the
  wake-clock fit reads ≈ −130 ppm against a −139 ppm crystal period.
- **The transition wake is unexplained.** When the check-in interval changes,
  one wake opens several windows that catch nothing (8 of them on 2026-09-22).
  Every subsequent wake is 1:1. Attributed by position, not by mechanism.

---

## 8. Cross-references

| document | owns |
|---|---|
| `mac-layer.md` | the boundary, the sublayers, the KPI definitions |
| `mac0-final-campaign.md` | the three-mode run procedure and recovery |
| `bench-runbook.md` | bench mechanics, flashing, capture |
| `implementation-plan.md` | every measurement, dated, with its numbers |

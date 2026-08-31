# Cutting the wake: a proposal

Measured 2026-08-31. A routine check-in wake costs **28.1 s**, of which the
actual work is **4.7 s**. The rest is the node waiting.

The question that prompted this was "what does a 1 h check-in interval cost
instead of 6 h". The useful answer turned out not to be about the interval at
all.

---

## 1. Where the 28 seconds go

From node 2's own uptime stamps, a real resume:

```
   919 ms   BOOT reason=DEEPSLEEP  rtc_ram=VALID
  1029 ms   Read config.txt
  1129 ms   SessionMgr: restored state (base_nonce, counters)
  1149 ms   Deep-sleep wake with valid session — resuming (beacon-first)
  1149 ms   REGISTER fallback armed for 12000 ms
  1169 ms   Sending BEACON
  4699 ms   CMD TIMESYNC received (drift +2 s)
 13609 ms   session resumed OK — skipped REGISTER handshake
 25109 ms   hub quiet for 20000 ms — sleeping
 28139 ms   esp_deep_sleep_start()
```

| phase | duration | share |
|---|---|---|
| boot → beacon sent | 1.17 s | 4 % |
| beacon → TimeSync reply | 3.53 s | 13 % |
| **TimeSync → quiet window expires** | **20.4 s** | **73 %** |
| drain-wait → sleep | 3.03 s | 11 % |

**Everything useful is finished at 4.7 s.** The remaining 23 s is the node
proving to itself that nothing more is coming.

---

## 2. The actual defect

> The node infers "the hub has finished" from **silence**, using a fixed
> timeout, instead of being **told**.

That is the whole problem, and it is the one thing every mature low-power radio
protocol gets right.

There is a second instance of the same mistake in the same path.
`resumeFallbackCb` is armed for 12 s to catch a hub that rebooted while the node
slept, and it is **never cancelled** — it runs to expiry and only then checks
whether the session was proven. The decrypted TimeSync at 4.7 s already proved
it. The log line at 13.6 s is the timer finding out nine seconds late.

It is not on the critical path today only because the 20 s window is longer. It
becomes the binding constraint the moment that window shrinks.

---

## 3. How other systems solve it

**LoRaWAN Class A** — the closest analogue, and the canonical answer. The node
transmits, then opens **two short RX windows at fixed offsets** (RX1 at 1 s,
RX2 at 2 s). If nothing arrives, it sleeps *immediately*. Downlink is possible
**only** in those windows, so the network must queue traffic until the node's
next uplink. Awake time is TX plus two brief windows — tens of milliseconds.

**LoRaWAN `FPending`** — the elegant part. A downlink carries a bit meaning "I
have more for you". The node sleeps at once unless that bit is set, in which
case it opens another window. The answer to *"how long should I wait"* comes
from the peer, never from a timer.

**LoRaWAN Class B** — periodic network **beacons** give time-synchronised
"ping slots". Nodes open short RX slots at agreed instants. Requires clock
discipline between the ends, which is exactly what the drift work established
(**8 ppm**, see `timing-accuracy.md`).

**BLE connection events + slave latency** — a fixed connection interval, and the
peripheral may skip up to N events when it has nothing to say. The central
buffers. Same principle: the schedule is agreed, the waiting is bounded.

**Zigbee / Thread sleepy end devices** — the child polls its parent ("anything
for me?") and the parent answers yes or no. The node sleeps on "no". Again an
explicit answer rather than a timeout.

The common thread: **bound the listening by agreement, not by silence.**

---

## 4. Proposal, in tiers

Each tier is independently useful and strictly enables the next.

### Tier 0 — cancel the resume fallback when the session is proven

Free, and it unblocks everything below. `session_proven_` is already set the
moment a decrypted downlink arrives; cancel the timer there instead of letting
it expire.

*Saves nothing today. Prevents Tier 1 from being capped at 13.6 s.*

### Tier 1 — an explicit "nothing more for you" flag

Add `moreData` (or `sleepOk`) to `TimeSync`, or to any downlink. The hub already
knows: it has a per-node queue. The node sleeps on the first downlink that says
the hub is finished, instead of waiting 20 s.

**28.1 s → ~7.7 s.** One proto field, one hub check, one node branch.

### Tier 2 — scheduled RX windows (Class A shape)

The node beacons, opens **two short windows at fixed offsets**, and sleeps if
they are empty. The hub must answer promptly or queue for the next wake.

**→ ~3 s**, bounded by boot (1.2 s) and the drain-wait (3 s), which then become
the things worth attacking.

### Tier 3 — broadcast beacon + pending-data map

One hub transmission serves every node:

* `TimeSync`'s payload (`epoch`, `utcOffset`, `dstNext`) is **byte-identical for
  every node** — it is a broadcast frame that is currently unicast N times.
* Add a small **pending bitmap**: "nodes 3 and 7 have traffic waiting". Every
  other node sleeps immediately on hearing it.

Hub downlink airtime then becomes **constant in node count** rather than linear.
Requires nodes to wake on a common schedule, which needs the clock agreement the
drift work measured.

**Already in place:** `broadcastAddressing = 0xFF` exists, the node's admission
check already accepts broadcast frames (`CmdDispatcher.cpp:2516, 2719`), and
there are commented-out attempts at broadcast in the hub. The wire format does
not need changing to try this.

---

## 5. What it is worth

Awake seconds per day, at the measured ~1.45 mA awake current:

| | 6 h interval | 1 h interval |
|---|---|---|
| today (28 s wake) | 112 s | 672 s |
| Tier 1 (7.7 s) | 31 s | 185 s |
| Tier 2 (3 s) | 12 s | 72 s |

**Tier 2 makes hourly check-ins cheaper than today's six-hourly ones** — 72 s/day
against 112 s/day. That is the result worth chasing: not a trade between
responsiveness and battery, but both improving together.

Tier 1 alone at 6 h cuts check-in awake time by **72 %**.

---

## 6. The trade-off, stated honestly

The quiet window is not pointless. It is the node's guarantee that a command or
schedule push issued just before it sleeps still lands this wake rather than
waiting for the next one. Shortening it moves late traffic to the following
check-in.

That penalty scales with the interval, which is why Tiers 1–2 and a shorter
interval belong together: at 6 h a missed downlink waits six hours; at 1 h it
waits one. **The two changes make each other affordable.**

---

## 7. Caveat

Deep-sleep current is still **unmeasured** (`power-analysis.md` §"Not
measured"). Everything above is awake-time arithmetic, which is the right basis
for comparing these options against each other — but it cannot say what any of
them do to battery life in absolute terms. One meter reading would fix that, and
it also gates whether node 1's 46 % is anomalous.

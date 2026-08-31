# Separating the link layer from the application

Written 2026-08-31, at the end of a long debugging day, because the evidence for
this is strongest while the bugs are still fresh.

The proposal: split what is currently one class on each side into a **link
layer** (session, crypto, delivery, wake scheduling) and an **application
layer** (covers, schedules, telemetry).

---

## 1. The argument is a list of today's bugs

Nearly every defect found today was a **layering violation** rather than a
coding error. Each would have been structurally impossible, or trivially
visible, with a link/application boundary in place.

| bug | what crossed the boundary |
|---|---|
| **Mode sysops silently dropped.** `CMD_MODE_AUTO`/`CMD_MODE_INTERACTIVE` fell through `default: return 0` and did nothing, while the ack still told the hub they had worked. | `blindsSysPbToCmd()` maps **link-layer** commands into an **application** enum (`BlindsSysCmd`). Commands with no application meaning had nowhere to go. |
| **`sleepOk` needs a carrier message.** Sleep permission is a link-layer fact but rides on `TimeSync`, which is only sent in reply to a beacon — so it is delivered only when an application message happens to follow. | Link-layer state signalled through an application message. |
| **The 20 s quiet window exists at all.** The node infers "my peer has finished" from silence. | There is no link-layer concept of a *transaction*, so the node has nothing to ask. |
| **Login stagger duplicated in three places** (startup, no-clock fallback, post-sleep), two of which were missed on the first fix. | Session scheduling has no single owner. |
| **Drift measurement and RX windows** live inside `CmdDispatcher`, beside motor commands. | Pure link-layer work with no home of its own. |
| **A hub restart invalidates every session**, and the recovery path is spread across startup login, retry backoff, wake modelling and the register handshake. | Session lifecycle has no boundary to be reasoned about at. |

`CmdDispatcher` (node) and `LORAListener` (hub) each do all of: framing,
addressing, AEAD, replay windows, retransmit, wake scheduling, cover
operations, schedule policy, and battery telemetry.

That is why changing *when a move ends* broke *position reporting*, and why a
mode command could vanish into an enum with no slot for it. These are not
careless mistakes; they are what a missing boundary feels like from the inside.

---

## 2. The split

**Link layer** — everything about getting bytes to a peer reliably and cheaply:

* addressing and framing; burst and RX-window scheduling
* session establishment: register, login, base-nonce exchange
* AEAD, replay protection, message-id windows
* reliable delivery: tracked ops, acknowledgement, retransmit, give-up
* **wake and sleep scheduling**, including "the link is idle, you may sleep"
* time distribution (a link service: both ends need a shared clock)

**Application layer** — everything about blinds:

* cover operations, position, roll geometry
* schedule storage and automatic-mode policy
* battery and telemetry

---

## 3. The interface is where it pays

```
mac.send(payload, reliable)      -> delivered | failed
mac.on_payload(cb)
mac.transaction_complete()       <- nothing further queued for this peer
mac.request_wake(when)
mac.session_state()              -> none | establishing | live
```

`transaction_complete()` is **`sleepOk` in its correct home.** As a link-layer
primitive it fires whenever the link goes idle, rather than needing an
application message to carry it. The delivery gap found today — the flag only
arriving if a `TimeSync` happens to follow the last pending item — would not
exist, because there would be no carrier to depend on.

Likewise `CMD_MODE_AUTO` becomes a link-layer call with a link-layer effect
(change the wake schedule), never routed through an application command enum
that has no value for it.

---

## 4. How to get there without a rewrite

A big-bang refactor of a working system is the wrong move, and today showed
exactly why: four separate changes each broke something adjacent, and each was
caught only because the blind physically misbehaved in front of someone.

The incremental path is **already in use**. `MotorPolicy.h`, `BootPolicy.h`,
`AutoModePolicy.h`, `DriftEstimator.h` and `ScheduleText.h` are the same move at
small scale — lift a decision out of the god object into a pure, host-testable
unit. The link boundary is that idea one level up, and it can be approached the
same way:

1. **Session + crypto first.** The most self-contained piece, with the clearest
   interface: `establish()`, `encrypt()`, `decrypt()`, `state()`. It is also
   where a mistake is most expensive, so it benefits most from being testable in
   isolation.
2. **Reliable delivery next.** Tracked ops, ack, retransmit, give-up — already
   nearly a unit, currently interleaved with application dispatch.
3. **Wake scheduling third.** This is where `sleepOk`, the quiet window, the RX
   windows and the drift correction belong together. It depends on (1) and (2)
   being separable first.
4. **The application falls out** as whatever is left.

Each step should keep the node running and the tests green, in the same way the
policy headers were extracted.

---

## 5. Honest caveats

* **This is not urgent.** The system works. The argument is about the *rate* of
  bugs like today's, not about a present failure.
* **The boundary will leak at first.** Time distribution is a link service that
  the application also cares about (schedules are in local time). Expect to get
  that one wrong initially.
* **Two implementations, one protocol.** The node is ESP-IDF C++ and the hub is
  an ESPHome component; the boundary has to be drawn twice, and the two will
  drift unless the wire format keeps them honest. The existing `regen_stubs.sh`
  plus the schema-drift ctest gates are the model for that.
* **The measured wins so far came from protocol changes, not structure.**
  `sleepOk` cut a wake from 28 s to 20 s; the drift work reached 8 ppm. Neither
  needed this refactor. Structure buys fewer *future* bugs, which is real but
  slower to show up — and should not be oversold against work that has numbers
  attached.

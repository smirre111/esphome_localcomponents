# Hub onboarding: one node at a time, and nothing in the clear

**Written 2026-09-26** from full serial captures of node 2 (COM6) and the hub (COM9) across a hub restart and a node reset, with both nodes on the air. Captures are in `logs/` (git-ignored), taken with `tools/benchlog.py`.

## What was seen

After a hub restart or a node reset the logs showed retransmitted and unacknowledged frames, and frames in the clear that should have been encrypted. Four separate causes, all in the hub:

| # | cause | evidence |
|---|---|---|
| 1 | **`CoverConfig` packed raw.** `confirm_session_` replays the stored REGISTER into every child node; `LoraCoverComponent::send_remote_config` built the frame itself and called `send()`, so it always went out in the clear. The node refuses plaintext once it holds a session. | node: `Rejecting PLAINTEXT command (cmd_case=13)`; 13 is `CMD_COVERCONFIG`. Same defect class as the earlier ClientConfig fix. |
| 2 | **Ack timers armed at enqueue.** The hub airs one 17-copy burst plus a response window at a time, ~1.85 s. Login, ScheduleConfig, GridSync and tracked-op timers all started at `send()`, so a frame queued behind four others had not left the radio when its 5 s timer expired, and the retransmit joined the same queue. | hub: 16 bursts back to back for 30 s; every ScheduleConfig "not acknowledged" three times while the node was acking each one, but after the hub had moved on to msgid 6, 7, 8. |
| 3 | **Two nodes onboarding in parallel.** The only separation was `slot × 3 s` between the *first* login of each node. One node's onboarding is ~8 frames and ~15 s of airtime, so the two interleaved. | node 2's login sat 11 s in the queue; node 1's ScheduleConfig failed outright. |
| 4 | **Pushes racing a rebuilt session.** A rebooted node beacons; the hub queues a TimeSync and ScheduleConfig; the node's REGISTER arrives 0.2 s later and the hub's login starts. The queued pushes fire anyway, the TimeSync under the old key (undecryptable) and the ScheduleConfig in the clear (`send_login()` had un-confirmed the session). | node: `Rejecting PLAINTEXT command (cmd_case=17)` (SCHEDULE); the login itself sat 3 s behind the two wasted bursts. |

There is no protocol or wire change and no new frame type. Every fix orders when the hub starts something it was always going to do.

## What changed

| # | fix | where |
|---|---|---|
| 1 | `LORAListener::send_downlink()` is the one way a child node hands a downlink to the radio; it encrypts whenever the session is confirmed. The cover uses it. | `lora_client.cpp`, `lora_cover.cpp` |
| 2 | `LORATracker::txDrainUs()` prices every accepted frame as its own burst behind the others; `ack_wait_ms_(base)` adds it to the four ack timers (capped at 60 s). | `lora_tracker.{h,cpp}`, `lora_client.cpp` |
| 3 | **`OnboardingGate.h`**, a dependency-free policy: a node may start a login challenge only while it holds the gate, and holds it until it is *settled* (session confirmed, ≥3 s for the follow-up pushes to be queued, the hub's queue drained, the schedule acked or its retries spent). Waiters are FIFO. Escape hatches so an absent node cannot block the fleet: released if the node never answers within 15 s (and the hub's own backlog is empty), and capped at 60 s regardless. | `OnboardingGate.h`, `lora_client.cpp` |
| 4 | `relogin_pending_`: raised by a REGISTER or `send_login()`, cleared by `confirm_session_`; TimeSync, ScheduleConfig and GridSync **publish** wait while it is up. `confirm_session_` already re-sends all three. A grid *withdrawal* is not deferred: it is the safe direction and carries no key. | `lora_client.cpp` |

Also: the per-REGISTER `[E] MAC address does not match` (logged by every non-addressed listener, twice per handshake) is now a debug line.

## Measured, same two-node scenario

| | before | after |
|---|---|---|
| hub restart, both nodes: hub warnings/errors | 12 | **0** |
| ScheduleConfig retries | 7 | **0** |
| plaintext refusals on node 2 | 1 (CoverConfig) | **0** |
| both nodes settled | 44 s after boot | **37 s** |
| node 2 reset, hub running: plaintext refusals | 1 (ScheduleConfig) | **0** |
| node 2 reset: REGISTER to settled | 11 s, two wasted bursts (old-key TimeSync, plaintext ScheduleConfig) | **7 s** |

Node 2 now also accepts its CoverConfig, which it did not before: two `CMD CONFIG` saves after a hub restart.

Host suite: 1127 to 1165, each new test proven red-first or mutation-killed (gate policy 12 mutants, gate integration 8, ack timers 5 plus 3 in the tracker, rebuild guard 7).

## Not done, on purpose

* **Node-driven logins are not gated.** `send_login()` calls made in reaction to a node's own frame (a beacon asking for a re-login, the config-sync relogin) go out immediately, because the node is awake and asking right now. They can still overlap another node's onboarding. Hub-initiated logins, REGISTER-driven logins and login retries are gated.
* **Tracked cover commands sent during a rebuild** still fall back to plaintext and rely on their own retry ladder. A user command in that window is rare and is retried.
* **The double login per hub boot.** Every hub boot sends a LoginMsg asking for a REGISTER (`config_synced_` is false per boot), then a second login: two extra bursts per node to re-check a config the node already holds. It could be skipped by pushing config encrypted straight after the first login, but that changes what a node sees, and it needs the node's `needs_config` answer, which only the REGISTER carries.
* **Node-side log levels.** `Rejected message ID: N, ignoring` (duplicate burst copies) and `LoginMsg rate-limited` are logged at E/W on the node although they are the burst working as designed. Changing them needs a node flash.

## Reproducing

```
python tools/benchlog.py --tag mytest         # captures COM6 + COM9 to logs/<ts>_mytest/, every line, timestamped
python tools/benchlog.py --note "pressed X"   # from another shell: marks the moment
python tools/benchlog.py --reset node         # pulses EN on the port the logger already holds
curl -X POST -d "" http://192.168.178.91/button/hub_restart/press
```

`--reset` exists because closing the port is what resets an ESP32 uncontrollably; pulsing RTS on the held port captures the boot. Analyse the files afterwards, with ANSI stripped (`sed -E 's/\x1b\[[0-9;]*m//g'`), rather than grepping live.

# local_components -- the LoRa blinds hub

ESPHome external components for the hub of the LoRa blinds system. The node
firmware lives in a separate repo (BlindsESP); the two sides share the wire
contract described in `docs/mac-wire-format.md`.

## Components

| component | layer | what it is |
|---|---|---|
| `lora_tracker/` | PHY + MAC-0 | The radio. `lora.cpp` is the SX1278 driver. `LORATracker` owns the send task, burst/placed transmission (the burst engine: `sendPacketBurst` and its per-copy steps, with per-copy stamping and tagging in `burst_frame.h`), the RX poll and the demultiplexing to listeners (`UplinkDemux.h`), and the beacon. Its parts: `TxPool` (`tx_pool.h`, the buffer pool), `TxScheduler` (`tx_scheduler.h`, queue order, supersession, the channel calendar) and `HubGrid` (`hub_grid.h`, anchor, rounds, beacon marks). `mac_mgmt.*` is the MAC management unit: the fleet key, the GridBeacon and its FLEET tag, the shadow pending mask. |
| `lora_client/` | MAC-1, MAC-2, delivery, APP | One `LORAClient` (a `LORAListener`) per node: the ESPHome facade and the application, with its parts behind narrow host interfaces: `HubSession.h` (the node's MAC-1 counters and reservations, MAC-2 keys, v1 framing, the uplink security stages, the onboarding gate, and the login / session state machine, which reports to the listener through SessionHost events), `NodeLinkTiming.h` (Mode B / C placement and the §4.6 belief), `DeliveryService.h` (its channel slots are private; the listener keeps the Control command, `ControlCommand`), `BenchStreams.h`. MAC-1: `Sequencer.h` (FCNT, replay window) and `FcntReservation.h`. MAC-2: `HubLinkSecurity.*` (`SessionKeyStore`, seal, retag, verify). The delivery service (`DeliveryService.h`, policy in `DeliveryPolicy.h`). The bench streams (`BenchStreams.h`: MAC ping, drift test, ModeTest and its report). The wire: `MacWire.h` (header codec), `HubWire.*` (bodies, routes), `PortTable.h`. The application: covers, schedules, sleep model, auto mode, Mode B supervisor. Most `*.h` here are pure, host-tested policy headers. |
| `loracover/` | APP children | The Home Assistant entities of a node (cover, sensors, switches, buttons, text, ...). They receive decoded `AppIndication`s from their listener and send through its public API; they never see wire bytes. |
| `blindsproto/` | schema | The generated protobuf-c stubs of `BlindsESP/proto/blinds.proto`. Never edit by hand: `BlindsESP/proto/regen_stubs.sh` writes both repos' copies. |
| `rika_stove/` | -- | Unrelated component. |

## Vendored files

Several headers in `lora_client/` are OWNED BY THE NODE REPO and carried here
because the ESPHome build only compiles `local_components/`. The list is
`tests/proto_sim/cmake/vendored_shared_headers.txt`; each copy has a banner
above `#pragma once` and is byte-compared with the node's file by a ctest
drift gate. Change them in the node repo and re-vendor; never edit them here.

## Read this first

1. `docs/mac-wire-format.md` -- the frame on the air.
2. `lora_client/MacSap.h`, `lora_client/HubFrames.h` -- the layer service types and the hub's internal frame types.
3. `lora_client/HubLinkSecurity.h` -- MAC-2 and its locking rules.
4. `lora_client/DeliveryPolicy.h` -- how commands are retried and acked.
5. `docs/threading-model.md` -- which task owns what (loop, esp_timer, send task) and the locks.
6. In `lora_client.cpp`: the receive path `set_response` -> `admit_frame_` ->
   `handle_encrypted_` -> `deliver_uplink_`, and the send path `deliver_` ->
   `DeliveryService::tx_` -> `place_ctl_` -> `send_aligned_` -> `mac0_send_`.
7. In `lora_tracker.cpp`: `send` -> `serviceTxQueue` -> `sendPacketBurst`.

## Security notes

- **REGISTER replay (review HB14, `lora_client/RegisterFreshness.h`).** REGISTER
  bypasses the session replay window, so the node numbers it
  (`ClientRegister.register_seq`, strictly increasing, persisted, under the
  K_AUTH tag). Each listener keeps the highest value seen since the hub booted
  (RAM only) and drops any REGISTER whose value is not above it, before the 5 s
  rate limit. `register_seq = 0` (firmware older than node 82a7658) gets the
  rate limit only, but once a node has sent a numbered REGISTER an unnumbered
  one is refused as an old capture. Side effect: a node DOWNGRADED by OTA to
  firmware without the field cannot REGISTER again until the hub reboots
  (press 'Hub Restart').

## Session diagnostics (`lora_client/SessionDiag.h`)

What happened to a node's REGISTER / LOGIN path, visible in Home Assistant and
the log instead of only in a serial capture. `HubSession` notes every event
where it happens; the listener publishes on change.

| Entity (loracover platform) | Meaning |
|---|---|
| `binary_sensor`, `type: session_stale` | ON while the node is heard WITHOUT a session (keyless uplink, no session id on record, accepted REGISTER) and none was confirmed within 30 s of the first such uplink. Cleared by the next confirmed session or a sleep. A silent node is never stale. |
| `text_sensor`, `type: session_state` | `confirmed`, `login pending`, `no session`, `asleep`, or `stale (heard, no session)`. |
| `sensor: session_recovery_time` | Seconds from the first sign a session was needed (LoginMsg sent, node heard without a session, session dropped) to its confirmation. No value before the first recovery after a hub boot. |
| `sensor: login_messages`, `registers_accepted`, `registers_refused`, `keyless_uplinks`, `sessions_dropped`, `stale_periods` | Counters since hub boot (`total_increasing`). |

Log lines to grep: `SESSION STALE` (WARN, once per period, with the counters and
the last REGISTER refusal reason) and `Session recovered in N s (K LoginMsg)`.
A keyless uplink is unauthenticated, so a forged frame can raise the flag;
nothing acts on these values.

**Event log.** Each node keeps its last 24 session events in RAM (LOGIN sent
and how: 0 burst, 1 aimed after an uplink, 2 placed on the REGISTER mark,
3 phase sweep; REGISTER accepted / refused; uplink without a session;
confirmed; dropped; put to sleep; stale raised; node restored its counters;
node report). A run of the same event is one entry with a count.
`LORAListener::dump_session_events()` writes it to the log with wall-clock
times (`session log NN: -S s (HH:MM) <event> xN arg=A span=S s`); the hub does
that by itself when a stale period starts, and the yaml has a `Dump Session
Log` button per node.

**The node's own account.** Node fw >= 1.1.59 reports in its beacon how many
REGISTERs it sent, how long it waited for the LOGIN it accepted, which copy of
the burst that was and how often it could not save its `register_seq`
(`NodeWakeBeacon` fields 36..39, node `SessionForensics.h`). The hub logs each
new report once (`Node reports its session path: ...`) and warns on a save
failure (`NODE COULD NOT SAVE ITS register_seq`).

**Measuring it.** `tools/restart_soak.py --rounds N` restarts the hub N times
and prints each node's recovery time; `--analyze hub.log` does the same from a
captured log.

## Startup demote sweep

After a restart the hub's grid has a new anchor, so it broadcasts `GridDemote`
before anything else. One unplaced burst reaches a Mode B node only if a copy
falls into its one ~29 ms window per round (about 1 in 3), and a node that
misses it stays Mode-B-only, where every LOGIN is the same lottery. The
broadcast therefore goes out as four bursts, the later three at the first
one's phase + 22 / 44 / 66 ms (the LOGIN sweep's table,
`HubSession::kLoginSweepOffsetsUs`), 2 s apart (`kDemoteSweepGapMs`). Log:
`startup: broadcast grid demote sent (... burst K of 4 of the phase sweep)`.

## Tests and checks

- `tests/proto_sim` -- the host test suite (gtest + CMake, WSL). It compiles
  the real hub sources and real node sources (`-DBLINDS_ESP_ROOT=<BlindsESP>`)
  and includes the drift gates, golden frames and the shared wire vectors.
- `tools/layer_lint.py` -- the ratcheting layering lint (also a ctest).

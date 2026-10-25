# local_components -- the LoRa blinds hub

ESPHome external components for the hub of the LoRa blinds system. The node
firmware lives in a separate repo (BlindsESP); the two sides share the wire
contract described in `docs/mac-wire-format.md`.

## Components

| component | layer | what it is |
|---|---|---|
| `lora_tracker/` | PHY + MAC-0 | The radio. `lora.cpp` is the SX1278 driver. `LORATracker` owns the transmit pool and queue, the send task, burst/placed transmission with per-copy stamping and tagging (`burst_frame.h`), the RX poll and the fan-out to listeners, and the grid (anchor, rounds, beacon timing). `mac_mgmt.*` is the MAC management unit: the fleet key, the GridBeacon and its FLEET tag, the shadow pending mask. |
| `lora_client/` | MAC-1, MAC-2, delivery, APP | One `LORAClient` (a `LORAListener`) per node. MAC-1: `Sequencer.h` (FCNT, replay window) and `FcntReservation.h`. MAC-2: `HubLinkSecurity.*` (`SessionKeyStore`, seal, retag, verify). The delivery service (`deliver_` / `dlv_*`, policy in `DeliveryPolicy.h`). The wire: `MacWire.h` (header codec), `HubWire.*` (bodies, routes), `PortTable.h`. The application: covers, schedules, sleep model, auto mode, Mode B supervisor. Most `*.h` here are pure, host-tested policy headers. |
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
   `dlv_tx_` -> `place_ctl_` -> `send_aligned_` -> `mac0_send_`.
7. In `lora_tracker.cpp`: `send` -> `serviceTxQueue` -> `sendPacketBurst`.

## Tests and checks

- `tests/proto_sim` -- the host test suite (gtest + CMake, WSL). It compiles
  the real hub sources and real node sources (`-DBLINDS_ESP_ROOT=<BlindsESP>`)
  and includes the drift gates, golden frames and the shared wire vectors.
- `tools/layer_lint.py` -- the ratcheting layering lint (also a ctest).

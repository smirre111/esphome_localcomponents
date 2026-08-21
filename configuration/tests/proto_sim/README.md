# LoRa Blinds protocol simulation tests

Host-native GoogleTest harness for the protocol that lives across the hub
(ESPHome `local_components/lora_client/`, `local_components/lora_tracker/`,
`local_components/loracover/`) and the node (`BlindsESP/main/CmdDispatcher.cpp`,
`SystemCtrl.cpp`).

The harness runs on a desktop machine — no MCU, no SX1278, no FreeRTOS — so
scenarios that would take hours to reproduce on hardware (sleep cycles,
hub-reboot mid-handshake, race between scheduler cancel and pending callback)
fire in milliseconds and never go flaky.

## Phase 1 (this commit)

* `CMakeLists.txt` — pulls in GoogleTest via `FetchContent`.
* `sim/sim_clock.{h,cpp}` — virtual time + named scheduler that mirrors
  ESPHome `Component::set_timeout` / `set_interval` / `cancel_*` semantics.
* `sim/sim_radio.{h,cpp}` — in-memory broadcast medium between one hub and
  N nodes, with a transcript of every air frame for assertions.
* `sim/messages.h` — plain-struct mirror of `blinds.proto`. Phase 2 will
  swap this for real protobuf-c round-trip using `local_components/blindsproto/blinds.pb-c.c`.
* `sim/hub_model.{h,cpp}` — `HubListener` + `HubTracker`, mirroring
  `LORAListener` and `LORATracker`. Same code paths: NVS restore-with-skip-64,
  login-startup timer, pending_login_nonce reuse, msgid replay check.
* `sim/node_model.{h,cpp}` — `NodeModel` mirroring `CmdDispatcher` (msgid
  replay, address filter, CMD_LOGIN rate-limit, register/clientconfig/login
  handling).
* `scenarios/register_test.cpp` — scenario **A1** (two-node cold register)
  and **A2** (wrong-MAC ignored).
* `scenarios/login_test.cpp` — scenario **B2** regression for the
  IV-mismatch bug shipped in the recent fix: hub must reuse the same base
  nonce on every retry of a login challenge.

## Build

Verified on WSL (Ubuntu 24.04, gcc 13.3.0, cmake 3.28.3) from the Windows
checkout:

```powershell
wsl -- bash -c "cd /mnt/c/Development/esphome/configuration && cmake -S tests/proto_sim -B build/proto_sim && cmake --build build/proto_sim -j && ctest --test-dir build/proto_sim --output-on-failure"
```

Native Linux/macOS:

```sh
cmake -S tests/proto_sim -B build/proto_sim
cmake --build build/proto_sim -j
ctest --test-dir build/proto_sim --output-on-failure
```

Native Windows (Visual Studio Build Tools):

```powershell
cmake -S tests\proto_sim -B build\proto_sim
cmake --build build\proto_sim --config Debug
ctest --test-dir build\proto_sim -C Debug --output-on-failure
```

Expected output: **78/78 tests passing in ~2 s** (phases 1–3 + the auto-mode P0
schema tests, both sides). The hub's `lora_client.cpp` and the node's
`CmdDispatcher.cpp` run as REAL production code under the harness.

> **Run this before starting each phase of [auto-mode-plan.md](../../docs/auto-mode-plan.md).**
> The suite had rotted to non-building between v1.0.9 and 2026-08-21 because
> nothing ran it routinely; while broken it provided zero coverage but looked
> like a test suite. `sim/messages.h` and `sim/wire_codec.cpp` mirror the proto
> BY HAND — the `schema_drift_*` gates catch a stale generated stub, but they
> cannot catch a stale mirror struct. Add proto fields in all three places.

System dependencies on WSL/Linux: `libprotobuf-c-dev`, `libmbedtls-dev`,
both standard on Ubuntu 24.04.

## Phase 2 — done

* `sim/wire_codec.{h,cpp}` translates `proto_sim::*` structs ↔ real
  `blinds.pb-c.{h,c}` bytes via the generated stubs the ESPHome firmware
  itself compiles. The on-air format is byte-identical to production.
* `sim/crypto.{h,cpp}` provides AES-GCM-128 / SHA-256 via mbedtls — the
  same library the on-device PSA Crypto layer wraps. The pinned
  `SHA-256("LoRaKey1")[0:16]` test would fail if the key string drifted.
* Encrypted path: `NodeModel::send_resp_` encrypts when `peer_base_` is
  populated, `HubListener::on_frame` unwraps the `EncryptedPayload`
  envelope, validates IV against the stored base nonce, decrypts, and
  re-parses the inner message.

## Phase 3 — landed (partial)

`tests/proto_sim/shims/` provides a thin ESPHome/ESP-IDF compatibility
layer (Component scheduler, EntityBase, ESPPreferenceObject, optional,
format_mac_addr_upper, log macros, esp_bd_addr_t, esp_random, plus
re-exports of the proto and the production lora_client headers).

The `real_lora_client` CMake target compiles
`local_components/lora_client/lora_client.cpp` — the **actual production
code** — into a host library. Scheduler/NVS/send calls route through the
same `SimClock` / `SimRadio` / NVS slot store the model tests already
use.

`scenarios/real_lora_client_test.cpp` is the first scenario running
against the real code: it asserts the `pending_login_nonce_` retry-reuse
invariant on three direct `send_login()` calls. Reverting the production
fix and rebuilding makes it fail; restoring makes it pass — verified.

### Node-side landed too

`tests/proto_sim/shims_node/` provides FreeRTOS (queues, semaphores,
portMUX_TYPE, tasks), ESP-IDF (esp_err_t, esp_log, esp_mac, esp_timer,
esp_system, esp_random, esp_task_wdt), and sibling-class shims
(`MotorCtrl`, `SystemCtrl`, `LoraInterface`, `Packet`, `myinterrupts.h`,
`utilities.h`). Plus a `lora.h` SX1278 stub.

A small CMake step copies the production `CmdDispatcher.cpp`,
`comm_utils.{c,h}`, `CmdDispatcher.h`, `common.h`, `Packet.h` into a
`node_stage/` directory alongside the sibling shims — required because
`#include "MotorCtrl.h"` from `CmdDispatcher.h` searches its own
directory first, and the only way to redirect that is to put the shim at
the same path.

The `real_cmd_dispatcher` library compiles the actual production
`CmdDispatcher.cpp`. `scenarios/real_cmd_dispatcher_test.cpp` exercises:

* `CmdLoginResetsCountersAndStoresNonce` — CMD_LOGIN handler enqueues
  AVAILABLE ack; second CMD_LOGIN within 5 s is rate-limited.
* `CmdOperationReplayRejected` — replayed CMD_OPEN with the same msgid
  is silently dropped by the production replay filter.
* `CmdClientConfigAppliesAddressOnlyForMatchingMac` — CLIENTCONFIG with
  a non-matching MAC must not change `cfgAddress` (multi-node-on-one-air
  guarantee).

Each test is spot-check-verified: e.g. reverting `LOGIN_RATE_LIMIT_MS =
5000` to `0` in production source makes the rate-limit test fail; restore
and it passes.

### Remaining phase-3 work

* Port the remaining hub-side scenarios (A4, B3, C1–C3, D1–D5, E1–E6) to
  the real LORAListener via the same adapter pattern.
* Add more node-side scenarios (CLIENTCONFIG happy path, encrypted reply
  round-trip with pack_response_message, geometry application).
* Compile the real `lora_tracker.cpp` against shims (currently the hub
  test uses a minimal shim implementation of `LORATracker::send` /
  `register_client`).

### Portability bug fixed by phase 3

`lora_client.cpp` used `std::min(uint64_t_expr, static_cast<uint64_t>(…))`.
On ESP-IDF Xtensa, `uint64_t` is `unsigned long long`, so template
deduction worked. On host Linux x86-64, `uint64_t` is `unsigned long` —
mismatched with the `1000ULL` multiplication's `unsigned long long` —
and the deduction failed to compile. Fix: explicit template argument
`std::min<uint64_t>(…)`. Real portability bug the test harness caught.

## Scenario coverage status

| ID  | Scenario                                                | Phase |
|-----|---------------------------------------------------------|-------|
| A1  | Two-node cold REGISTER routed by MAC                    | ✅ 1  |
| A2  | Wrong-MAC REGISTER ignored                              | ✅ 1  |
| A3  | Simultaneous REGISTER from rol_1 + rol_2                | 1    |
| A4  | REGISTER while previous login pending                   | 1    |
| A5  | Hub reboot mid-registration                             | 1    |
| A6  | NVS version mismatch                                    | 1    |
| B1  | Single LoginMsg → ACK → login_acked                     | ✅ 1  |
| B2  | Retry reuses pending nonce (regression for current bug) | ✅ 1  |
| B3  | Stale interval race with REGISTER 500ms timer           | 1    |
| B4  | Lost ACK → retry → eventual ack                         | 1    |
| B5  | kMaxLoginRetries exhausted                              | ✅ 1  |
| B6  | Hub reboot, node still holds old base_nonce             | 1    |
| B7  | BaseNonceExchange recovery path                         | 1    |
| C1  | Sleep → fallback timer armed                            | ✅ 1  |
| C2  | Early wake REGISTER cancels fallback                    | ✅ 1  |
| C3  | Fallback fires when REGISTER lost                       | ✅ 1  |
| C4  | Hub reboot during node sleep                            | 1    |
| C5  | Multi-node stagger                                      | 1    |
| D1  | rol_2 OPEN routed only to addr 18                       | ✅ 1  |
| D2  | rol_2 reply rejected by rol_1 listener                  | ✅ 1  |
| D3  | Broadcast destaddress (0xFF)                            | ✅ 1  |
| D4  | Two nodes sharing same address (sanity check)           | 1    |
| D5  | Node with cfgAddress=0 only accepts LOGIN/CLIENTCONFIG  | ✅ 1  |
| E1  | Encrypted CMD_OPEN round trip                           | ✅ 2  |
| E2  | Replayed CMD_OPEN rejected                              | ✅ 2  |
| E3  | Out-of-order encrypted packets                          | ✅ 2  |
| E4  | Spoofed unencrypted msgid (DoS document)                | ✅ 2  |
| E5  | Tampered ciphertext fails decrypt + does NOT advance rx | ✅ 2  |
| E6  | Tampered AAD fails                                      | ✅ 2  |
| F1  | OTA → restart → re-handshake                            | 1    |
| F2  | CMD_STATUS while node asleep                            | 1    |
| F3  | CMD_ENABLE_WIFI / DISABLE_WIFI                          | 1    |
| G1  | CoverConfig with full geometry                          | 1    |
| G2  | CoverConfig with zero geometry (proto3 default)         | 1    |
| G3  | Mixed-zero geometry guard                               | 1    |
| H1  | Max-size packet (250 B)                                 | ✅ 2  |
| H2  | Truncated packet rejected                               | ✅ 2  |
| H3  | Random garbage bytes survive without crash              | ✅ 2  |
| H4  | msgid wraparound                                        | 1    |
| H5  | Burst-dupe dedupe + NVS throttle                        | 1    |

(✅ implemented in this commit; remaining rows are placeholders for the next
PRs against `tests/proto_sim/`.)

## Naming convention for scheduler entries

Production code uses unscoped names like `"login_startup"`, `"login_retry"`.
The harness prefixes them with the listener name (`"login_startup_rol_1"`)
so multi-node tests aren't subject to ESPHome's name collision behaviour.
When the phase-2 swap to real source files lands, the prefix is added by a
thin shim around `Component::set_timeout`.

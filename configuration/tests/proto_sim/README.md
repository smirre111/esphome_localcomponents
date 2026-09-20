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

Expected output: **176/176 tests passing in ~9 s** (phases 1–3 + the auto-mode P0
schema tests, both sides). The hub's `lora_client.cpp` and the node's
`CmdDispatcher.cpp` run as REAL production code under the harness.

> **ALWAYS `cmake --build` before `ctest`.** ctest runs whatever binaries are
> already there — it does NOT rebuild, and it reports a cheerful pass on a
> binary that no longer matches the source. This bit twice in one session: once
> when a compile error left stale binaries passing 138/138, and once when a
> mutation-test edit was restored in the source but never rebuilt, so the
> "failure" was a mutant that no longer existed. If a result surprises you,
> rebuild first and check the build output separately.
>
> The same applies to `idf.py flash` after editing: the node source and the
> harness's `node_stage/` copy are separate, and only a build refreshes the copy.
>
> **Run this before starting each phase of [auto-mode-plan.md](../../docs/auto-mode-plan.md).**
> The suite had rotted to non-building between v1.0.9 and 2026-08-21 because
> nothing ran it routinely; while broken it provided zero coverage but looked
> like a test suite. `sim/messages.h` and `sim/wire_codec.cpp` mirror the proto
> BY HAND — the `schema_drift_*` gates catch a stale generated stub, but they
> cannot catch a stale mirror struct. Add proto fields in all three places.

### Two harness constraints that will bite you

**There is no `settimeofday` shim.** glibc's fails without root, so the node
clock under test is REAL WALL TIME and cannot be stepped. `give_clock()` sets
`s_clock_valid` (which is what `shouldRunAutoMode()` actually checks) but does
not move the clock. Any test involving schedule timing must build its entries
RELATIVE TO NOW, not from a pinned epoch — a schedule pinned to a fixed date
silently falls outside the catch-up window and the test passes for the wrong
reason.

**Several pieces of node state are file-level statics shared by every test in
the binary** — `s_interactive_until`, `s_last_exec_epoch`, the cached firmware
version. `RTC_DATA_ATTR` is a no-op on the host, so they persist across
fixtures. One existing test leaves the interactive override set to "interactive
forever", which suppresses auto mode in every test defined after it. If a test
needs auto mode active, replace the override with a finite deadline and wait it
out rather than assuming a clean slate.

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

### The real `LoraInterface.cpp` and the real driver

`real_lora_interface_test` is the one target that does NOT shim
`LoraInterface.h`. It compiles the real
`components/lora/lora.c` driver and the real `main/LoraInterface.cpp`,
with `shims_node/lora_hal_stub_node.cpp` standing in for the SPI/GPIO
layer and recording every register write, PHY setting, DIO mapping and
transmitted payload into `shims_node/lora_node_recorder.h`.

Two things to know before touching it:

* It has its own `IFACE_STAGE` and its own copy of `CmdDispatcher.cpp`,
  because `real_cmd_dispatcher` was compiled against the *shim*
  `LoraInterface.h` and the two definitions cannot be linked together.
* The real driver's include directory must be ordered **before**
  `NODE_SHIM_DIR`, and `lora_hal_stub_node.cpp` includes its header as
  `<lora.h>`, not `"lora.h"`. A quoted include resolves to the shims' own
  13-line `lora.h` stub first, whose inline bodies then collide with the
  real ones.

What it pins is the PHY the whole design is derived from: `init()`
programs SF7 / BW500 / CR4-8 / preamble 8 / sync 0x12 / CRC on, and the
test *recomputes* `LoraTiming.h`'s `kSymbolTimeUs` (256) and `kCadUs`
(320) from the values the radio was handed rather than re-asserting the
constants — so a PHY change that quietly invalidates every timing number
in the plan fails here instead of in the field. It also pins the
asleep-with-interrupts-cleared start state, DIO1 → `RX_TIMEOUT` (the only
edge that ends a receive window), byte-exactness of `sendPacketBytes()`,
and `maxUplinkAimWaitUs() == 290 ms`.

It also reaches the receive task's body. `loraRxTask` was a ~370-line
`for(;;)`, so the extraction that made the node's uplink path testable
(`CmdDispatcher::serviceTxCommand` / `runOneTxCommand`) was applied to it
as well: `serviceRxWindow`, `armTimedRxWindow`, `armContinuousRx`,
`noteRxWindowSkipped`, `transmitOneQueuedFrame` and `beginCad`. The loop
keeps the watchdog feed and the two bounded waits, and nothing else.

Answering a CAD from a test needs the `Recorder::on_cad` hook rather than
seeding the queue, and that is the point rather than a quirk: production
resets `lora_cad_queue_` immediately before `lora_cad()`, so any answer
placed earlier is wiped. Delete that reset and
`AStaleCadAnswerIsNotConsumedAsThisCadsAnswer` transmits — which is the
real failure, since the 2 s CAD receive abandons an answer that stays
queued and the next CAD consumes it. One wart to know about: the queue is
created with a one-byte item size while the consumer reads into an `int`,
so a test must queue a `uint8_t`.

### The real `frtosTasks.cpp` — the interrupt path

`frtos_tasks_test.cpp` compiles and runs it, in the same binary because it
needs the same real `LoraInterface` and the same recording HAL. It is the
last of T-1.

That file **is `main.cpp`** for the target: the task bodies reach the
firmware through `motCtrl` / `sysCtrl` / `loraIf` / `cmdDispatcher`, which
live in `main.cpp` on the node, so the fixture points them at its own
objects — and deliberately leaves some null, because every use of
`cmdDispatcher` and `loraIf` in the interrupt path is null-checked and
those guards are worth exercising rather than sidestepping.

Both `for(;;)` bodies were lifted into `serviceDio0Event()` and
`serviceDio1Event()`. What the tests hold them to is the TX_DONE branch
pairing the edge with the length the transmit loop recorded (C2's window
origin: `T0_uplink = TxDone − the frame's own air time`) and the DIO1
`RX_TIMEOUT` branch, which is the only place the node learns a window
closed empty and therefore the only source of `noteMarkMissed()` and of
every demotion.

Two things to know before touching it:

* `extern_stubs.cpp`'s `spawnTaskBatteryMonitor` /
  `spawnTaskMotorCurrentMonitor` are behind
  `PROTO_SIM_HAVE_REAL_FRTOSTASKS`, which this target defines. With both
  definitions present it linked **silently**, and which one won was decided
  by archive member-extraction order.
* `intQueueDio0` / `intQueueDio1` are defined by `LoraInterface.cpp`, not
  by the shims. Defining them again is a multiple definition.

The ADC shim (`esp_adc/adc_oneshot.h` + `adc_stub.c`) models nothing
analogue — that belongs on a bench next to HW-1 — but it does reproduce
one real driver behaviour on purpose: a second open of the same unit
returns `ESP_ERR_INVALID_STATE`, which is why *both* spawn guards in
`frtosTasks.cpp` exist.

### Two limits no extraction lifts

The **ISRs themselves** stay out of reach: `myinterrupts.h`'s handlers run
in interrupt context and capture the timestamps every phase measurement
rests on. What is reachable is everything downstream of the queue they
post to.

And **elapsed time**. These tests assert the ORDER of radio operations and
which branch ran, never how long the critical path took. A log line inside
the aimed critical path (~3.5 ms of UART inside a ±14 080 µs guard band;
this actually happened) still would not be caught here. That one needs a
scope, and it is HW-7's number.

### Remaining phase-3 work

* Port the remaining hub-side scenarios (A4, B3, C1–C3, D1–D5, E1–E6) to
  the real LORAListener via the same adapter pattern.
* Add more node-side scenarios (CLIENTCONFIG happy path, encrypted reply
  round-trip with pack_response_message, geometry application).
* ~~Compile the real `lora_tracker.cpp` against shims~~ — **done**, as
  `real_lora_tracker_test`. The shim `LORATracker` still exists and is
  what `e2e_test` and the older hub scenarios drive, so it remains true
  that those cannot see frame placement, the prepare/fire split or queue
  supersession; only the real target can.
* ~~Extract the `loraRxTask` transmit sequence~~ — **done**; see the
  section above.
* ~~Compile `frtosTasks.cpp` and extract its interrupt body~~ — **done**;
  see above. T-1 is closed.

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

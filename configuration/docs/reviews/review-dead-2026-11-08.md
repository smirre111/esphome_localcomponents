# Dead features and dead code: review of 2026-11-08

Scope: hub `c:\Development\esphome\configuration` (main 9a901ad) and node `C:\Development\PlatformIO\PlatformIO\BlindsESP` (main 7ea87ca, fw 1.1.58). This was a read-only pass. Nothing in either repo was edited, built, flashed or committed. All scripts and intermediate outputs are in `scratchpad/dead/`.

This is a fresh pass after the HB18/N32 fixes. HB18's `SleepTrigger`, the AM43/Tuya encoder, `app_id`, `kHubAddr` duplicate and MacSap orphans are gone, and so are N32's `rxPacket`/`txPacket`/`txbuf_mutex`/`kPersistNvs*`, the `TICKS_TO_WAIT` throttle, `handleNotSet` and `advanced_ota_example_task`. The `onReceiveNew` and `cfgAddress` names that are still in the code are live: `onReceiveNew` is the real RX entry point, and `cfgAddress` is now only a parameter name. What remains is listed below.

## How the evidence was produced

1. **Linker ground truth (the strongest evidence).**
   - Hub: `C:\hsr11\configuration\.esphome\build\loradevices\.pioenvs\loradevices\firmware.{map,elf}` was built on 2026-11-06 from sources identical to 9a901ad (`diff -rq --strip-trailing-cr` against `local_components` and `loradevices.yml` is empty).
   - Node: `build\BlindsV3.{map,elf}` was built on 2026-11-05 at 20:13, 2 minutes after the 7ea87ca commit.
   - Method: parse "Discarded input sections" for `.text.*` from our objects, demangle with `c++filt`, and keep only symbols absent from `nm -C` of the ELF (`scratchpad/dead/hmap.py`, `gone.py`).
   - Result: 222 hub symbols (≈130 of them protobuf-c pack/unpack helpers, which are ignored here) and 83 node symbols.
   - **Caveat:** gc-sections also drops the out-of-line copy of a function whose calls were all inlined. Every linker hit was therefore cross-checked with a source caller count. Those with a real caller were dropped: `unpack_uplink_`, `enqueue_setup_`, `ctl_busy_`, `config_goes_acked_`, `HubSession::derive_keys`/`generation`/`stale_drops`/`drop_candidate_`, `NodeLinkTiming::uplink_mark_fire_`/`next_at_phase_`, `fireStampFor_`, `beaconRoundForT0`, `MacUplink::packCopy`, `Buttons::button_stop_press_2s`, `logIncomingFrame_`.
2. **Caller counts.**
   - `lizard --csv` lists 1270 hub and 1266 node functions. For each short name, word-boundary occurrences were counted with comments stripped, per category: hub prod, hub YAML, hub test, node prod, node test, tools (`callers.py`, `usage.py`).
   - The hub's vendored copies of node headers were excluded so they are not double-counted.
   - `tests/proto_sim/build_clean/` was also excluded. It is git-ignored, 137 MB, and holds STALE staged node sources: it still has `CmdDispatcher::interactiveRemaining`, which now lives in AppController. Counting it hides dead code, and it hid about 40 items in a first run.
3. **Protobuf.** Every field and enum value of `proto/blinds.proto` was checked against `.field`/`->field` (lowercased, the protobuf-c member names) and against `__VALUE` uses in each repo (`proto.py`).
4. **Other checks.**
   - YAML: live versus commented-out entities.
   - The schema keys of every `__init__.py` versus `to_code`.
   - Kconfig symbols versus `CONFIG_*` uses and `sdkconfig`.
   - `#if 0` blocks and commented-out-code blocks (`commented.py`).
   - Headers that no firmware file includes.
   - `git ls-files` for tracked artefacts.
   - Tests: `DISABLED_` tests and test files not registered in any CMake target.
5. **Not available:** cppcheck and clang-tidy are not installed in WSL. Steps 1 and 2 replace them.
6. **Findings that need a decision.** Items that touch the wire, the NVS layout or the tests are marked *needs decision*. No item touches the NVS layout or `kRestoreStateVersion`. Proto fields may only ever be REMOVED-AND-RESERVED (number and name), never renumbered.

## Summary table

Ranked by misleading impact first, then size. "Lines" is the approximate number of removable lines (code plus declarations). Test lines are shown separately.

| # | Item | Repo | Lines removable | Risk |
|---|---|---|---|---|
| D1 | `SKILL.md` describes the deleted v0/AES-GCM protocol and calls the node repo "hub firmware" | hub | 487 (rewrite or delete) | none |
| D2 | Shadow pending mask is structurally inert: in 1448 logged beacons the mask never cleared once | hub | ~300 prod + 264 test (or fix the input) | none on the wire (log only) |
| D3 | `BatteryRuntimeCalib.h` (the "fit runs on the node") is never included by the firmware | node (+ 1 node unit test) | 117 + test | none |
| D4 | DriftTest is unreachable in production on both ends | hub + node + proto | hub ~270, node ~185, proto msg | proto: reserve the message |
| D5 | Six node-only headers are vendored into the hub tree and compiled only by hub header tests | hub | 1634 + 6 shims | test plumbing |
| D6 | Per-slot schedule entity UI (superseded by `text:`): 4 platforms + `set_slot_*` + commented YAML | hub | ~650 | none (no YAML user) |
| D7 | HW-2 arm sweep (`MODE_SWEEP`, `SweepAnalysis.h`, a DISABLED test) is unreachable in production | hub + node | ~250 + 124×2 + test | decision |
| D8 | proto_sim model layer still simulates the deleted protocol (AES-GCM, base_nonce, v0 message names) | hub tests | ~2700 test lines (12 scenarios + sim models) | test count drops |
| D9 | `loradevices.yml` commented-out diagnostics (≈288 config lines + 186 note lines); accessors only they call; `make_bench_yaml.py` revert machinery is dead | hub | ~470 YAML + ~30 C++ + ~25 py | none |
| D10 | Wi-Fi password in tracked `sdkconfig`/`sdkconfig.old` (unused symbols); private keys tracked | node | config | **security, not just dead code** |
| D11 | IDF example residue: `protocol_examples_common` (2286 lines), OTA resumption, provisioning paths, `EXAMPLE_*` Kconfig | node | ~2286 + ~250 | build deps |
| D12 | `mybutton.cpp`/`.h` fully `#if 0`, still in CMake SRCS | node | 307 | none |
| D13 | Unused SX127x driver API, forked in two non-drift-gated copies | both | hub ~465, node ~546 | none (keep as lib?) |
| D14 | SystemCtrl/utilities legacy (SPIFFS, old battery ADC, deep-sleep init, UDP, commented sleep code) | node | ~200 | none |
| D15 | Hub dead members (LORATracker, LORAListener BLE heritage, forwarders, counters) | hub | ~150 | none |
| D16 | Node dead small functions (`buffer_ref_inc`, `comm_utils` BE helpers, `taskShutdownWifi`, ...) | node | ~110 | none |
| D17 | Node `CmdDispatcher` test-only forwarders, the no-op `noteDriftSample`, base-nonce wrappers | node (+ hub tests) | ~120 + test edits | test edits |
| D18 | `CmdDispatcherTestPeer`: 68 of 178 re-exports are used by no test; 2 unused `ForTest` seams | node + hub | ~70 | none |
| D19 | Proto: 9 `ModeTestReport` fields neither set nor read; `SchedAction` unused; values never sent | proto | ~15 proto lines | regen both stubs |
| D20 | Small loracover/lora_client cruft: duplicate `sleep_control` action, no-op `send_remote_config`, empty polling `update()`, unused member | hub | ~45 | none |
| D21 | Stale ad-hoc tools (`campaigns/{g,lg,w}.py`, `op.sh` into an old scratchpad, `audit_main_adoption.sh`) | hub | ~120 | none |
| D22 | Tracked artefacts: `debug.log` (2021), `sdkconfig.old`, `component.mk`×2, `lib/README`, `gdbinit`, `.vscode` | node | ~3900 | none |
| D23 | Commented-out code blocks and `#if 0` | both | hub ~71, node ~150 | none |
| D24 | Naming debt left from v0: `msgid` (=FCNT), `base_nonce` (=session id), `sendCommandAck` | both | rename, not remove | wide diff |
| D25 | Stale docs and undocumented truths (see the last section) | hub | docs | none |

**Totals, approximate.**
- Hub production code: about 1,900 lines removable without a decision (D1 doc 487, D6 650, D9 ~525, D13 465, D15 150, D20 45, D21 120, D23 71), plus about 2,200 lines that are decision-dependent (D2 300, D4 270, D5 1634).
- Hub tests: about 2,700 lines (D8), plus header tests and the DISABLED test if D2, D5 or D7 go.
- Node: about 1,300 lines of code (D3 117, D12 307, D13 546, D14 200, D16 110, D17 120) plus about 2,500 of vendored example component and dead config paths (D11), plus about 3,900 of tracked artefacts (D22), plus D4/D7 if decided (~430).

---

## D1. `SKILL.md` describes a protocol that no longer exists (hub, 487 lines, misleading: highest)

- **Where:** `c:\Development\esphome\configuration\SKILL.md` (tracked). It calls itself an "Expert Skill" that "activates automatically" for both repos.
- **Evidence:** `grep -n -iE "LoraHeader|GCM|EncryptedPayload|msgId|CommandAck|BaseNonceExchange" SKILL.md` returns 20+ hits.
  - It documents the v0 envelope: `EncryptedPayload encrypted`, `BaseNonceExchange basenonce`, a `LoraHeader` table with `msgId`, and a section headed "Security / AES-GCM-128 Encryption" (IV = base_nonce‖msgid, AAD, 8-byte GCM tag).
  - It also documents `CommandAck ack = 15`.
  - Line 4 calls BlindsESP "(hub firmware)". BlindsESP is the node.
  - Every one of these names is deleted or reserved in `blinds.proto`.
- **Test-only:** no.
- **Risk:** none.
- **Action:** delete it, or rewrite it as a pointer to `docs/project-state.md` and `docs/mac-wire-format.md`. If any agent tooling loads it, it is actively harmful. Effort: S.

## D2. The shadow pending mask can never clear (hub, ~300 prod lines, misleading: high)

- **Where:**
  - `lora_client/PendingShadow.h` (212 lines).
  - Glue: `lora_client.cpp:~440-460` (`pending_shadow_inputs`), the `serviceBeacon` gather in `lora_tracker.cpp`, the log at `lora_tracker.cpp:711`, `mac_mgmt.{h,cpp}` `addShadowSource`, and the counters `shadowBeacons`/`shadowBeaconsWithClear`/`shadowLastMask` (test-only accessors).
  - Test: `pending_shadow_test.cpp` (264 lines).
- **Evidence:**
  - Reason bit `kNotLatencyTolerant` (`PendingShadow.h:122`) is set whenever `latency_tolerant` is false.
  - `lora_client.cpp:453` sets `in.latency_tolerant = this->auto_mode_`. Both production nodes are interactive (Timed Mode, not auto), so every node's bit is always LISTEN.
  - In every hub log in `logs/`: `grep -ah "beacon shadow" */api.log */hub.log | wc -l` gives 1448 lines, and the same filtered with `grep -v "beacons_with_clear=0/"` gives **0**.
  - Example, 2026-11-08 08:25: `would=0xffffffff ... beacons_with_clear=0/157 | RollladenWohnzimmer1 slot 0 LISTEN (timesync,not-tolerant,recent)`.
  - No tool parses the line. `tools/layer_lint.py` only names "shadow" in a lint rule.
- **Why it matters:** `docs/project-state.md` §7 item 5 plans to "collect data over a soak before ever transmitting the mask". With the current input, that soak cannot produce a single "would clear" sample, because the decision is pre-empted by the proxy.
- **Test-only:** prod code; it only logs. No wire effect: the beacon still sends `allListening()`.
- **Risk:** none (log-only).
- **Action (needs decision):** either
  - (a) give it a real latency-tolerance input (an explicit flag, or "auto mode OR the Timed Mode node has an empty queue") so the soak can mean something, or
  - (b) delete PendingShadow and its glue until wake-cost Tier 3 is actually scheduled, keeping `PendingData.h` (the wire-side mask, node-consumed) as is.
- **Effort:** S for (b), M for (a). In either case, record in project-state that the soak as specified cannot produce data.

## D3. `BatteryRuntimeCalib.h` is never used by the firmware (node, 117 lines, misleading: high)

- **Where:** `main/include/BatteryRuntimeCalib.h`. Its only users are node `test/` and the `test/CMakeLists.txt:123` comment.
- **Evidence:**
  - `grep -rl 'include *[<"]BatteryRuntimeCalib.h' main components` returns nothing.
  - `batterycalib::addSample`, `predictRatio` and `mostRecentCurrent` have only definition occurrences in prod (callers.py: `prod=1 nodetest=14/5/3`).
  - `git log --oneline --all -- main/include/BatteryRuntimeCalib.h` returns only `d223b61`. It was added and never wired.
- **Why it matters:** the file says "Decisions ... 1. The fit runs on the node". The hub sensor schema (`loracover/sensor/__init__.py`) says "the node runs the fit, the hub only shows it". `docs/battery-runtime-compensation-proposal.md` is headed "IMPLEMENTED". In reality the node only reports the measured `actualRunS`/`nominalRunS` (`TelemetryBuilder.cpp:206-207`) and uses "learned travel durations" (`MotorCtrl.cpp:118+`). No power→runtime correction runs.
- **Test-only:** yes.
- **Risk:** none.
- **Action:** delete the header and its unit test, or wire it. Either way, fix the three comments or docs. Effort: S.

## D4. DriftTest is unreachable in production on both ends (hub ~270, node ~185, misleading: medium)

- **Hub:**
  - `DriftStream` in `BenchStreams.h:246-279` and `lora_client_bench.cpp:615-650` and `956-1105`, plus `start_drift_test`/`stop_drift_test` (`lora_client.h:312-317`) and `drift_tick_for_test`.
  - Linker evidence: `DriftStream::start/stop/build_frame_/timer_cb_` are discarded and absent from `firmware.elf`.
  - The only callers are the commented-out YAML buttons (`loradevices.yml:338-373`, `# - lambda: 'id(rol_2)->start_drift_test(300, 1100);'`) and tests (`hubtest=5`).
- **Node:**
  - `BenchTools::handleDriftTest`, `stopDriftTest_` and `driftTestExpiredCb_` (`BenchTools.cpp:42-221`).
  - It refuses to start unless `benchNode()` (`BenchTools.cpp:105`). `bench_node_` is `true` only `#ifdef CONFIG_BLINDS_BENCH_NODE` (`CmdDispatcher.h:811`), and that symbol is not set in `sdkconfig` (`grep -c CONFIG_BLINDS_BENCH_NODE= sdkconfig` gives 0).
  - So even a hub that sent DriftTest would get "REFUSED (not a bench node)".
- **Proto:** `DriftTest` message (HubAppPdu case).
- **Test-only:** effectively yes (proto_sim drift tests).
- **Risk:** removing the proto message means reserving its PDU case number and name in both stub copies (regen with `proto/regen_stubs.sh`).
- **Action (needs decision):** keep it as a documented bench-only feature (then say so in project-state: "DriftTest needs a bench build with CONFIG_BLINDS_BENCH_NODE plus uncommented YAML"), or remove it from both repos and reserve the PDU case. Effort: M.

## D5. Node-only headers vendored into the hub firmware tree (hub, 1634 lines, misleading: medium)

- **Where:** `local_components/lora_client/{GridState.h 505, ModeTestPolicy.h 561, AckCache.h 190, PhaseTracker.h 148, SweepAnalysis.h 124, MacSublayers.h 106}`. There are also re-export shims in `tests/proto_sim/shims/esphome/components/lora_client/`.
- **Evidence:**
  - For each, `grep -rl "[</\"]$b[>\"]" local_components` finds no includer other than the file itself.
  - Namespace uses in hub prod: `modetest::`, `gridstate::` and `phase::` appear only in comments; `macsublayers::`, `ackcache::` and `sweep::` appear 0 times.
  - They exist in the hub tree only because `tests/proto_sim/CMakeLists.txt:204-226` `add_header_test(...)` compiles header tests against `SHARED_HDR_DIR = local_components/lora_client`. The drift gate (`vendored_shared_headers.txt`) then forces byte equality.
  - `MacWireVectors.h` (129) is also unincluded, but that is intended: the hub tests use it as shared vectors.
- **Why it matters:** a reader of `local_components/` sees 1600 lines of "hub" Mode-A/B grid, ModeTest and AckCache logic that the hub never runs.
- **Test-only:** yes.
- **Risk:** test plumbing only. The header tests would have to point at `${BLINDS_ESP_MAIN}/include`, so they would need the node tree, which with `CI` set is already mandatory.
- **Action (needs decision):** drop these six from `vendored_shared_headers.txt` and from `local_components`, and build those header tests against the node tree. Alternatively, move them under `tests/proto_sim/vendored/` so they stop looking like hub code. Effort: S-M.
- **The mirror case:** `main/include/TxQueue.h` (298) is the node repo's "source" copy, but no node file includes it. It is a hub-only scheduler kept in the node repo for the vendoring direction. Document it, or move its ownership to the hub.

## D6. The superseded per-slot schedule entity UI (hub, ~650 lines)

- **Where:**
  - `loracover/datetime/` (54 py + 86 C++), `loracover/number/` (40 + 65), `loracover/select/` (53 + 141).
  - `loracover/switch/schedule_enable_switch.{cpp,h}` (67) plus the `CONF_SLOT` branch of `switch/__init__.py`.
  - `LORAListener::set_slot_{minute,days,action,position,enabled}` (`lora_client.h:271-275`, `lora_client.cpp:1067-1109`).
  - The commented YAML block `loradevices.yml:1596-1698` ("SLOT-ENTITY SCHEDULE UI (kept for comparison; superseded by `text:`)") and `1508-1526`.
- **Evidence:**
  - Linker: `set_slot_*` and `ScheduleEnableSwitch::{setup,write_state,dump_config}` are discarded and absent from `firmware.elf`.
  - Usage: `set_slot_minute hubprod=3` (declaration, definition and the entity's own call), with 0 test uses.
  - No live YAML entity uses `slot:`.
- **Test-only:** no tests at all.
- **Risk:** none. The schedule NVS (`save_schedule_`) stays, because the text UI uses it.
- **Action:** remove. Effort: S.

## D7. HW-2 arm sweep (`MODE_SWEEP`) is unreachable in production (hub + node, misleading: medium)

- **Node:**
  - `modetest::armRefusal` returns `SweepOffBench` for `Mode::Sweep` unless `is_bench_node` (`ModeTestPolicy.h:328`), and `CONFIG_BLINDS_BENCH_NODE` is unset.
  - The sweep mis-arming path is `sweepMisArming_`, `Mac0Access` arm-offset handling and `BenchTools.cpp:485`.
  - `SweepAnalysis.h` (124 lines, plus the hub copy) has no includer in either firmware.
- **Hub:**
  - The sweep buttons are commented out (`loradevices.yml:604-667`).
  - The 9th `start_mode_test` argument (`arm_offset_us`) is used only by those.
  - `tests/proto_sim/scenarios/frtos_tasks_test.cpp:472` `DISABLED_ASweepsMisArmedWindowIsNeverRescuedByAnExtension` is the suite's one disabled test, waiting for a per-window gate that was never built.
  - `sweep_analysis_test.cpp` tests a header nobody uses.
- **Risk:** the proto `ModeTest.Mode MODE_SWEEP` value and `armOffsetUs` field must stay or be reserved.
- **Action (needs decision):** declare HW-2 abandoned (delete `SweepAnalysis.h`, the disabled test, the YAML block and the sweep branch; keep the enum value reserved), or keep it with an explicit "bench build only" note. Effort: M.

## D8. The proto_sim model layer simulates the deleted protocol (hub tests, ~2700 lines)

- **Where:**
  - `tests/proto_sim/sim/{hub_model,node_model}.{h,cpp}` (837), `sim/crypto.{h,cpp}` (the GCM part), `sim/messages.h` (structs named `LoraClientOperationMessage`, `LoraClientResponseMessage`, `EncryptedPayload`).
  - 12 model-only scenarios: `addressing, boundary, cover_config, crypto, garbage, login, race, reboot, recovery, register, sleep_wake, sysop` (1654 lines, 47 TESTs). They include only `sim/hub_model.h`/`sim/node_model.h`.
- **Evidence:**
  - `sim/crypto.h:10` says "The sim models' own AES-GCM-128 helpers (model-to-model only; production is ...)".
  - `hub_model.cpp:272` and `node_model.cpp:94` call `derive_gcm_iv`/`aes_gcm_encrypt|decrypt`.
  - `HubNvsBlob{version 2, rx_message_id}` models an old NVS shape.
  - The header claims the model "intentionally matches the production code path-for-path". It no longer does: production uses CTR+CMAC under K_enc/K_mac on wire v1. The sim's v1 helpers (`v1_*`) are separate and are used by the real-code tests.
- **Test-only:** yes.
- **Risk:** the pass count (1875) drops by roughly 47. Some scenario intents (garbage, race, boundary) may be worth porting to the real-code harness first.
- **Action (needs decision):** port what is still meaningful to `real_lora_client_test`/`e2e_test`, then delete the models and the GCM helpers. At minimum, mark them "legacy model, v0-era semantics" in `tests/proto_sim/README.md`. Effort: M-L.

## D9. Commented-out diagnostics in `loradevices.yml`, their accessors, and the bench generator (hub)

- **Where:** `loradevices.yml` has 904 comment lines in 1732. There are 288 commented config lines (`grep -cE '^\s*#\s*(- |[a-z_]+:( |$))'`) and 186 double-commented notes, in blocks at `338-373` (drift), `604-667` (sweep), `1030-1296` (U-2 rx-busy, U-5 overdue, tx policy/refusal, demotion reason, phase spread/guard/samples/err, Class A ledger, pool failures, worst poll gap) and `1596-1698` (slot UI).
- **Hub accessors reachable only from those commented lambdas:**
  - Zero callers anywhere: `node_demotion_reason`, `node_timed_rx_active`, `node_phase_spread_us`, `node_phase_outside_guard`, `node_phase_samples`, `node_phase_err_us` (each `hubprod=1`).
  - Dead: `reset_class_a_stats` (`lora_client.h:772`).
  - `poolAllocationFailures` is linker-discarded and appears only in the commented YAML.
  - `node_rx_busy_skips`, `class_a_stats`, `node_overdue`, `worst_poll_gap_us`, `txPolicyNow` and `txRefusalNow` are test-used.
- **`tools/make_bench_yaml.py`:**
  - The docstring says it builds the bench yaml "with the diagnostic blocks that were COMMENTED OUT ... put back".
  - The code now asserts `sum(a != b ...) == 3` ("bench differs from production by more than the bench-only tweaks"), so the difflib revert branch can never contribute. The docstring and roughly 25 lines are dead.
- **Risk:** none.
- **Action:** delete the commented entity blocks (git history keeps them), keep the essays that explain live entities, delete the six zero-caller accessors and `reset_class_a_stats`, and fix the generator's docstring and revert branch. Effort: S.

## D10. A credential and private keys are tracked in the node repo (security, not just dead code)

- **Wi-Fi credentials:**
  - `sdkconfig` and `sdkconfig.old` (both tracked) contain `CONFIG_ESP_WIFI_SSID="FRITZ!Box 7590 TD"` and `CONFIG_ESP_WIFI_PASSWORD="2346…"`.
  - Neither symbol is used by any source: `grep -rw CONFIG_ESP_WIFI_SSID main components` gives 0 and `CONFIG_ESP_WIFI_PASSWORD` gives 0.
  - It is a dead Kconfig pair (from `main/Kconfig`) that holds a live home Wi-Fi password in git history.
- **Private keys:** `cert_store/RootCA.key`, `cert_store/certificate.key` and `https_hosted/ca_key.pem` are tracked `-----BEGIN PRIVATE KEY-----` files (the OTA server CA).
- **Action (needs decision, owner):**
  - Remove the `ESP_WIFI_SSID`/`ESP_WIFI_PASSWORD` Kconfig entries and blank them in `sdkconfig`.
  - Untrack `sdkconfig.old`.
  - Rotate the Wi-Fi password if the repo was ever pushed anywhere non-private.
  - Decide whether the OTA CA keys belong in git.
- **Effort:** S, plus rotation.

## D11. IDF example residue on the node (~2500 lines)

- **`components/protocol_examples_common/`** (2286 lines, a vendored copy of the IDF example component, in `REQUIRES`):
  - Its only call from `main/` is `example_configure_stdin_stdout()` in `OtaService.cpp:336`, under `#ifdef CONFIG_EXAMPLE_FIRMWARE_UPGRADE_URL_FROM_STDIN`, which is unset.
  - `#include "protocol_examples_common.h"` in `SystemCtrl.cpp:30` and `WifiProvisioning.cpp:35` is otherwise unused. `example_connect` is never called.
- **OTA resumption:** `example_ota_res_*` (~120 lines, `OtaService.cpp:86-194` plus 4 more `#ifdef` sites) is compiled out because `CONFIG_EXAMPLE_ENABLE_OTA_RESUMPTION` is unset. Partial HTTP download, URL-from-stdin and the skip-version-check option are also off.
- **Provisioning:** `CONFIG_WIFI_PROVISIONING_ENABLED` is unset, so the SoftAP/BLE paths are unreachable: `custom_prov_data_handler` is linker-discarded, and the QR, sec2 salt/verifier and BLE branches (13 `#ifdef CONFIG_EXAMPLE_PROV_TRANSPORT_BLE` sites) are dead.
- **Kconfig:** `main/Kconfig` (324 lines) declares 27 `EXAMPLE_*`/`ESP_*` symbols. `EXAMPLE_PROV_TRANSPORT`, `EXAMPLE_PROV_USING_BLUEDROID`, `ESP_WIFI_SSID` and `ESP_WIFI_PASSWORD` are used 0 times; 10 more are unset and guard dead code.
- **Also:** `SystemCtrl.cpp:422-423` uses `CONFIG_EXAMPLE_EXT1_WAKEUP_*` and `utilities.h` has an empty `#if CONFIG_EXAMPLE_EXT1_WAKEUP`.
- **Risk:** build dependencies only. Keep the Wi-Fi station and HTTPS-OTA path that production uses.
- **Action:** drop `protocol_examples_common` (removing the include and REQUIRES), drop the OTA-resumption and provisioning branches or keep them deliberately with a note, and prune the Kconfig. Effort: M.

## D12. `mybutton.cpp`/`.h` is entirely `#if 0` (node, 307 lines)

- **Where:** `main/mybutton.cpp` (269 lines, line 1 is `#if 0`), `main/include/mybutton.h` (38 lines, line 1 is `#if 0`). Still listed in `main/CMakeLists.txt` SRCS, so it compiles to an empty object. `Buttons.cpp` is the live replacement.
- **Action:** delete both and the SRCS entry. Effort: S.

## D13. Unused SX127x driver API in two forked copies (hub 465, node 546)

- **Where:** `local_components/lora_tracker/lora.cpp` (1438) and node `components/lora/lora.cpp` (1456). They differ by 274 diff lines (`diff --strip-trailing-cr | grep -c '^[<>]'`), and they are not drift-gated.
- **Linker-discarded:**
  - Hub (34 functions): `lora_write_reg1`/`lora_read_reg1` (49 and 47 lines), `lora_parsePacket`, `lora_send_packet`, `lora_setGain`, `lora_compensateFrequencyOffset`, `lora_dump_registers`, `lora_peek`, `lora_endPacket`, `lora_setSymbolTimeout`, `lora_cad`, `lora_rxSingle`, `lora_end`, `lora_close`, ...
  - Node: a similar set, plus `lora_receive_packet`, `lora_setTxPower`, `lora_setOCP`, and `lora_caddone` (`prod=1`).
- **Risk:** none. It is a driver library, so keeping a complete API is defensible.
- **Action (needs decision):** prune at least the debug-only `*_reg1` helpers and the dead TX/RX packet API, or single-source the driver with a drift gate like the other shared files. Effort: S (prune) or M (single-source).

## D14. Node SystemCtrl and utilities legacy (~200 lines)

All are linker-discarded and have declaration+definition only:
- `SystemCtrl::mountSPIFFS` (43 lines) and `unmountSPIFFS`. The SPIFFS partition is unused (CMake comment F-2); only LittleFS is used.
- `SystemCtrl::setupBatteryMonitoringADC` and `readBatteryMonitoringADC` (battery sampling moved to `frtosTasks.cpp` `batteryMeasure`).
- `SystemCtrl::setupUdp`, `shutdownOTA`, `getConfigSleepDuration`, `getConfigHostname`.
- `MotorCtrl::resetGPIO`, `clip` (`MotorCtrl.cpp:48`).
- `utilities.cpp`: `initDeepSleep`, `before_sleep_callback`, `after_wake_up_callback`, plus the commented-out `initLightSleep`/`enterDeepSleep`/`enterLightSleep`/`reInitWakePins` (`utilities.cpp:112-187`, 76 lines) and the matching commented declarations in `utilities.h`.
- `SystemCtrl::unloadBatteryMonitoringADC` is live (`frtosTasks.cpp:973`).

**Action:** remove. Effort: S.

## D15. Hub dead members (~150 lines)

All have zero callers in prod or tests unless noted; `-` means none (from `usage.py`, prod count = declaration only).
- **LORATracker:**
  - `sendPacketOnce` (linker-discarded; `.cpp:1346` + `.h:340`).
  - `TxPool::ref_inc` (23 lines; linker-discarded; `tx_pool.h:89`).
  - `rxUnrouted()` (and the `rx_unrouted_` counter: incremented at `lora_tracker.cpp:322`, never read: a counter nothing consumes).
  - `lateMarks()` (its counter is logged elsewhere).
  - `get_last_snr()`.
  - `TxScheduler::after_listen` (`tx_scheduler.h:183`).
  - `poolAllocationFailures` (D9).
- **LORAListener, BLE heritage:** `remote_bda_`, `get_remote_bda()`, `check_addr(esp_bd_addr_t&)`, `address_uint64()`/`get_address()` (duplicates of `session_mac_address()`), `address()` (`lora_client.h:1157-1190`). Also the shim `tests/proto_sim/shims/esp_bt_defs.h`, which exists only for `esp_bd_addr_t`. `address_str()` is live.
- **LORAListener, forwarders left over from the splits:**
  - `noteUplinkPlacement_`, `acquire_onboarding_`, `release_onboarding_`, `requestMsgidRelogin_`, `mac1_skip_tx_` (only `tools/layer_lint.py` names it), `last_mode_test_report`.
  - `pack_test_frame_` and `wait_callback_idle_` (linker-discarded; declared at `lora_client.h:1273/1280`, defined in `lora_client_bench.cpp:484/510`).
  - `reset_timesync_policy_for_test` (an unused seam).
- **Others:** `ModeBSupervisor::warmupInFlight`, `ModeTestStream::marks_sent`, `mac2::LinkSecurity::authKeyReady` (`HubLinkSecurity.h:169`).
- **Test-only, not dead:** `msUntilNextT0`, `msUntilNextClearT0`, `busyUntilUs`, `placedBusyUntilUs`, `nextTxEligibleUs`, `SessionKeyStore::setMacKeyForTest`/`setInstalledSessionIdForTest`/`staleDrops`, `notePhaseReportForTest`, `deriveRootKeyForTest`. They are linker-discarded but used by proto_sim. Keep them as seams, or move them behind a test peer as on the node.
- **Action:** remove the dead ones. Effort: S.

## D16. Node dead small functions (~110 lines)

All have declaration+definition only. The `buffer_ref_inc`, `taskShutdownWifi` and `comm_utils` cases looked test-used until the stale `build_clean/` copies were excluded.
- `LoraInterface::buffer_ref_inc` (23 lines) and `arrivingWindowGeneration`.
- `comm_utils.c` `u32_to_be`, `u64_to_be`, `u64_from_be` (all three linker-discarded).
- `taskShutdownWifi` (`frtosTasks.cpp:988`, declared in `frtosTasks.h:94`).
- `Mac0Access::refusalStreak`, `rxWindowGen` (`Mac0Access.h:1208/1221`).
- `txqueue::Queue::peek` (`TxQueue.h:172`; the hub uses the queue, not this).
- `Packet::toString` (`Packet.h:57`).
- `nodewallclock::dstNext` (linker-discarded; reached only by the test-only `CmdDispatcher::getDstNext`).

**Action:** remove. Effort: S.

## D17. Node `CmdDispatcher` test-only forwarders (~120 lines, plus hub test edits)

- **Linker-discarded, used only by hub proto_sim tests:**
  - Arm-decision forwarders: `nextArmInstantUs`, `classAArmInstantUs`, `classAAction`, `shouldArmNextWindow`, `noteMarkOutcome`, `classAArmDelayUs` (thin forwarders to `Mac0Access`).
  - `runOneTxCommand`, `noteAnchorFixed_`, `simulateWakeForTest`, `lastReasonAnnounced`, `resetModeAnnounceState`, `getLastBatteryVoltage`.
  - `isClockValid`/`getUtcOffset`/`getDstNext`.
  - `get_base_nonce`/`set_base_nonce`/`clear_base_nonce` (the "base nonce" is the session id; `BaseNonceExchange` itself was deleted on 2026-10-06).
- **`CmdDispatcher::noteDriftSample`:** an explicit no-op "kept for the hub's proto_sim tests" (`CmdDispatcher.cpp:710-714`, 32 test call sites).
- **`Mac0Access::noteMarkOutcome`:** its own comment says "kept for callers that know both halves". Its only caller is the test-only forwarder.
- **Action:** move these into `CmdDispatcherTestPeer` (where they belong after N19), and delete `noteDriftSample` together with its test calls. Effort: S-M (proto_sim edits in step).

## D18. Test seams nobody uses (node + hub, ~70 lines)

- **`main/include/CmdDispatcherTestPeer.h`:** 68 of 178 `using CmdDispatcher::X` re-exports are referenced by no test in either repo. Examples: `m0_`, `ls_`, `session_`, `uplink_`, `app_`, `bench_`, `telemetry_`, `mgmt_`, `handleDriftTest`, `handleModeTest`, `serviceTxCommand`, `new_deep_sleep_task`, `rx_memory_pool`, `startWifi`, `stopWifi`, `kUplinkTxRampUs`, ... (`usage.py $(cat peer.txt)` with lines lacking `hubtest`/`nodetest`).
- **Unused seams:**
  - `sessionProvenHookFiredForTest` (`CmdDispatcher.h:409` plus the peer; no test).
  - Hub `reset_timesync_policy_for_test` (D15).
- **Production use of a test-named accessor:** `CmdDispatcher.h:924` initialises the production member `mac_ping_seq_hw_` from `receiver_.macPingSeqHwRefForTest()`.
- **Action:** trim the peer to what is used, delete the two seams, and rename the accessor used in production. Effort: S.

## D19. Proto fields and values that nobody sets or reads (wire; reserve, never renumber)

- **Neither repo sets or reads them:** `ModeTestReport.missedMarks(23)`, `demotions(24)`, `promotions(25)`, `rssiMin(43)`, `rssiMean(44)`, `snrMin(45)`, `snrMean(46)`, `sleepUs(50)`, `wallUs(51)`. Check: `grep -rn "[.>]missedmarks\b"` and the same for the others gives 0 in both repos.
- **Set by the node, never read by the hub** (dead telemetry, keep or drop): `NodeWakeBeacon.awakeWindow_ms(7)`, `nextEventEpoch(8)`, `ModeTestReport.crcErrors(15)`, `windowsEmpty(22)`.
- **Enums:**
  - `SchedAction` is used by no code (schedules carry raw action numbers).
  - `WakeReason.WAKE_TIMER_EVENT` is never produced (`CmdDispatcher.cpp:316-344` returns only BUTTON, TIMER_CHECKIN, BOOT, UNKNOWN or MODE_CHANGED).
  - `AckStatus.ACK_BUSY` is never sent (the proto comment says so).
- **Action:** remove the 9 fields as `reserved 23 to 25, 43 to 46, 50, 51;` plus their names, and decide about `SchedAction` and `WAKE_TIMER_EVENT`.
- **Risk:** removing fields changes the generated C struct layout. Regenerate both copies (`proto/regen_stubs.sh`) and flash both ends in the normal order. Proto3 ignores unknown fields, so a mixed-version pair stays wire-compatible.
- **Effort:** S.

## D20. Small hub component cruft (~45 lines)

- `lora_client/__init__.py:264-277`: the `lora_tracker.sleep_control` action duplicates `loracover.on_sleep_start` (same `SleepAction`). It is used in no YAML. Line 74 is a commented duplicate of the `SleepAction` declaration.
- `LORAClientNode::send_remote_config` is a virtual that is a no-op in 3 of 4 implementations (`sensor/lora_sensor.cpp:27`, `button/trigger_ota.cpp:24`, `button/debug_sysop_button.cpp:24`). `LoraCover::on_register` logs "sending sensor config (no-op)".
- `LoraCover` (sensor) is a `PollingComponent("120s")` whose `update()` is empty (`lora_sensor.cpp:139`).
- `LoraCover::current_sensor_` (`lora_sensor.h:57`) is written 0 and never read.

**Action:** remove. Effort: S.

## D21. Stale ad-hoc tools (hub, ~120 lines)

- `tools/campaigns/g.py`, `lg.py` and `w.py` hard-code `logs/*_review_rollout4/`.
- `tools/campaigns/op.sh` `cd`s into a past session's scratchpad (`.../scratchpad/r4`) to run `w.py`.
- `tools/audit_main_adoption.sh` is the one-shot audit for the main-adoption migration, which is DONE 2026-09-21 per `docs/merge-to-main.md`.
- Still valid: `mac1_campaign.py`, `mac2_campaign.py`, `mac1_turnaround.py` and `review_campaign3.py`. All the button and switch names they press exist in the live YAML (checked by script).
- **Action:** delete the four stale tools, or parameterise g/lg/w by session dir. Effort: S.

## D22. Tracked artefacts in the node repo (~3900 lines)

- `debug.log` (a 2021 VS Code debug adapter log).
- `sdkconfig.old` (3815 lines; see D10).
- `main/component.mk` and `components/lora/component.mk` (legacy GNU-make build files; IDF 6 is CMake-only).
- `lib/README` (PlatformIO template).
- `gdbinit` and `.vscode/*` (personal).
- `ToDo.md` (one item, "partially done").
- `cert_store/RootCA.srl`.
- Also: the hub `tests/proto_sim/build_clean/` is correctly git-ignored, but it is a 137 MB stale staged node tree that silently skews any grep. Delete it locally.
- **Action:** `git rm` the above (except whatever the owner wants to keep). Effort: S.

## D23. Commented-out code and `#if 0` (hub ~71, node ~150)

- **Hub:** `lora_tracker.cpp:228-258` (three blocks, 26 lines); `lora_tracker/lora.cpp:19, 1298-1328, 1432` (also `#ifdef TODO` at `lora.cpp:436`).
- **Node:**
  - `CmdDispatcher.cpp:91` (9 lines).
  - `LoraInterface.h:95,119`.
  - `frtosTasks.cpp:662`.
  - `MotorCtrl.h:15,20`.
  - `MotorCtrl.cpp:1069` `#if 0` (13 lines in `setupPwmChannels`).
  - `utilities.cpp:112-187` (D14).
  - The node's `lora.cpp` has the same blocks as the hub's (and `#ifdef TODO` at `:439`).
- The tool for this: `commented.py`, which flags runs of three or more `//` lines where at least half are code-like.

## D24. Naming debt left from v0 (not dead, but misleading)

- **`msgid`:** 264 hub and 157 node occurrences (`grep -rniE msgid`). `hubframes::Hdr::msgid` and `NodeFrames` `MacFrameHdr::msgid` are commented "the FCNT". `ack_msg_id`, `incrTxMessageId`, `tx_message_id` and `schedulePushMsgid` all mean FCNT or cmd_id. CLAUDE.md says "the old three-way msgId overload is gone"; on the wire it is, in identifiers it is not.
- **"base nonce":** 60 node occurrences (`SessionManager::getBaseNonce/setBaseNonce`, `CmdDispatcher::*_base_nonce`, `peerstate.base_nonce`). It is the session id. The persisted struct field name must stay as is (NVS layout), but the accessors can be renamed.
- **`sendCommandAck`:** sends a `MacAck` (node `MacReceiver.h:91`, `CmdDispatcher.h:373`).
- **Action:** a rename-only sweep (no wire or NVS change). Effort: M (large diff, mechanical).

## D25. Docs and code out of step

**Documented, but not true or not reachable:**
- `SKILL.md` (D1).
- `tests/proto_sim/README.md` still describes "AES-GCM-128 / SHA-256", "unwraps the `EncryptedPayload`", "B7 BaseNonceExchange recovery path", "msgid wraparound" and the model layer as mirroring production (D8).
- `docs/sequence-charts.md` (no "superseded" header) draws `CommandAck`, plaintext `BaseNonceExchange` and AES-GCM failures.
- `docs/mac-layer.md:97`, which project-state cites as the "layer semantics" doc, draws the outer frame as "field 1: LoraHeader {...}" (v0).
- `docs/battery-runtime-compensation-proposal.md` "IMPLEMENTED" and the sensor schema's "the node runs the fit" (D3).
- `docs/project-state.md` §8 says `implementation-plan.md` is 2074 lines; it is 3626, and it contains 19 v0-term hits.
- `make_bench_yaml.py` docstring (D9).
- `CmdDispatcher.h:810` says "setBenchNode() remains for tests". It does, but nothing says that a production build can therefore never run DriftTest or the sweep (D4, D7).

**True in code, but undocumented:**
- The shadow mask cannot clear while nodes are interactive (D2).
- DriftTest and HW-2 sweep need a `CONFIG_BLINDS_BENCH_NODE` build that does not exist in the tracked `sdkconfig` (D4, D7).
- Six node-only headers live in the hub tree for test reasons only, and `TxQueue.h` lives in the node repo but is hub-only (D5).
- `BatteryRuntimeCalib.h` is not wired (D3).
- `protocol_examples_common` is effectively unused (D11).
- `tests/proto_sim/build_clean/` is stale and skews searches (D22).

## Items checked and found live (false positives excluded)

- Static helpers with one caller (lizard plus count = 2): `s_import_ctr_key`, `s_v1_seal_locked`, `hubwire::s_mac_pdu`/`s_app_pdu`, `spawn_send_task`, node `main.cpp` boot helpers, `ConfigStore` parsers, `OtaService` validators, the `MacUplink` helpers, `blindsOpPbToCmd`.
- Calls inlined but present (see the Method caveat).
- ESPHome codegen setters: every `cv.Optional` key in the 6 component schemas reaches `to_code`.
- `SleepAction` (used by `on_sleep_start`).
- MacSap primitives (all used now).
- Optimistic single shot (the switch is ON in production).
- `ModeBSupervisor` and the production ModeTest buttons. Only MODE_SWEEP is bench-gated; `keep_power_profile` is used (`droppowerprofile`).
- Mode C and auto mode (reachable via the Auto Mode switch).
- `kV0FirstByte` (used by the node wire tests to prove v0 frames are refused).
- `DriftEstimator.h` (used by ModeTest and `Mac0Access`, not only by DriftTest).
- No `.cpp` in node `main/` is missing from SRCS.
- No scenario file is unregistered in either CMake.

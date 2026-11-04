# Node firmware review — BlindsESP @ e8647fc (fw 1.1.52)

Reviewer: independent, read-only. Scope: `main/` (incl. `main/include`), `tools/layer_lint.py` + baseline, `test/`.
Intent docs read: configuration `CLAUDE.md`. All findings checked against the code, not the docs.
Excluded as already known: Mode B phase-error / H-1 timing variation, node 1 position, shadow pending mask, K_root dev literal (H5).

Ranking: **Critical** = can brick or move a blind / break crypto now; **High** = a real defect that is reachable in the field; **Medium** = a real defect with narrower reach, or a structural problem that is actively producing bugs; **Low** = hygiene with some risk; **Nit** = cosmetic.
Effort: S < ½ day, M 1–3 days, L > 3 days.

No Critical findings. 3 High, 14 Medium, 17 Low, 4 Nit.

---

## 1. Correctness / security

### N1 — HIGH — Session reset and key re-derivation race the uplink sealer (CTR keystream reuse + hub wedge)
**Where:** `CmdDispatcher.cpp:3239` (`handleLogin`: `session_.seq().reset()`), `:3291-3301` (`setBaseNonce` + `ls_.deriveSessionKeys`), `:365-371` (`serviceTxCommand` REGISTER: `seq().reset()` + `clear_base_nonce`), `LinkSecurity.h:156-159` (destroys `k_enc_`/`k_mac_`), versus `macDataRequest_` `CmdDispatcher.cpp:596-632`.
**Evidence:** `session_tx_mutex_` is documented (`CmdDispatcher.h:977-986`) as guarding "allocate -> seal -> enqueue". The only `xSemaphoreTake` on it is in `macDataRequest_`. The counter **reset** and the **key swap** happen without it:
- `handleLogin` runs on `taskDsptchLora`. It resets the Sequencer, installs the new base nonce and swaps K_enc/K_mac with no lock.
- The REGISTER case runs on `taskTxProcessing`. It resets the counters and clears the nonce *before* calling `macDataRequest_`, so it is outside the mutex too. Meanwhile the RX task can be inside `macDataRequest_` building a MAC echo or a ModeTest report.

Both tasks are priority 6 on core 0. `taskLoraRx` (priority 7, `main.cpp:390-401`) preempts them on every window arm. When the preempted task goes back on the ready list, the other priority-6 task can run, so a switch in the middle of `macDataRequest_` is routine, not theoretical.
**Interleaving:** the TX task allocates FCNT=N (old session) and is preempted before `packUplinkFrame_`. The RX task's `handleLogin` resets tx to 0 and derives the new keys. The TX task resumes, and `get_base_nonce` + `sealWireSession` seal FCNT=N under the **new** K_enc/K_mac. Later the new session reaches FCNT=N again under the same key, session id, address and direction. That is two ciphertexts with the same keystream on the air. On the hub, the ForwardRatchet also jumps to N, so every node uplink 1..N-1 is rejected as old until the counter passes N. A second race: `deriveSessionKeys` destroys the key id the sealer may be using mid-`ctr_crypt_with_key_id`. PSA key ids are recycled, and `wireFleetTagValid` imports a key on every beacon.
**Fix:** take `session_tx_mutex_` around the whole reset → `setBaseNonce` → derive → `saveSession` → `save` block in `handleLogin`, and around reset + `clear_base_nonce` in the REGISTER case. Simplest: move the REGISTER state change into `macDataRequest_` under the lock it already holds. Then add a host test that interleaves `allocTxId_` and `handleLogin`. **Effort S.**

### N2 — HIGH — `sleepDuration` from ClientConfig is unbounded: 0 means "sleep until someone presses a button"; out of range can abort
**Where:** `CmdDispatcher.cpp:2062` (`setSleepDuration(clientconfig->sleepduration)`), `SystemCtrl.cpp:298-301`, `:419-441`, `:620-625`; `ConfigStore.cpp:211` (read as `i32`, stored in `uint64_t`).
**Evidence:** no clamp anywhere. In interactive mode, or on the hub's CMD_SLEEP path, `sleep_s = g_config.sleepDuration_s`. `if (time_us > 0)` then skips the timer wake entirely, so a 0 leaves EXT1 (the buttons) as the only wake source. A remote battery node is then dark until someone touches it. A negative value in `config.txt` becomes ~1.8e19 s. `esp_sleep_enable_timer_wakeup` rejects out-of-range values with `ESP_ERR_INVALID_ARG`, and that call is wrapped in `ESP_ERROR_CHECK` (`SystemCtrl.cpp:440`, `:624`): abort → panic → the same at the next sleep. The value is persisted, so it survives reboots. One typo in HA YAML (`sleep_duration: 0`) is enough.
**Fix:** a policy-header clamp, e.g. `sleepDuration ∈ [60 s, 7 d]`, with 0 meaning "use the default". Apply it in `setSleepDuration`, in `ConfigStore` load, and in `enterDeepsleep`. Replace `ESP_ERROR_CHECK` on the timer wake with a fallback to the default duration. **Effort S.**

### N3 — HIGH — No OTA rollback: a bad image bricks a field node
**Where:** `sdkconfig:466` `# CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE is not set`, `:3593`. `esp_ota_mark_app_valid_cancel_rollback` is never called.
**Evidence:** OTA is driven over LoRa (`Trigger OTA1`) to nodes with no serial access. An image that crashes before the radio is up (bad Kconfig, a static-init fault, an NVS migration bug) leaves the node in a boot loop with no way back except a cable. The project has already hit an unreachable node (node 1 stuck, per CLAUDE.md).
**Fix:** enable app rollback. Call `esp_ota_mark_app_valid_cancel_rollback()` only after the first confirmed session (`noteSessionProven`) or after N minutes of healthy uptime. This does not touch the NVS layout or `kRestoreStateVersion`. **Effort S–M** (needs one bench OTA cycle).

### N4 — MEDIUM — Sequencer persistence can record a snapshot that was never written; the finding-4 guard is bypassed by the persist hook
**Where:** `Sequencer.h:217-222` (`notePersisted()` copies the **live** `tx_`/`rx_`), `:168-171` (`commit` → `persist()` on the RX task, no lock), `:130` (`allocate` → `persist()` on the TX task under `session_tx_mutex_`); `SessionManager.cpp:184-227` (`save()` reads, writes NVS, then calls `notePersisted()`); `CmdDispatcher.cpp:3313-3319`.
**Evidence (a) race:** `save()` snapshots `txId()` before a slow NVS commit and calls `notePersisted()` afterwards, which records whatever `tx_` is by then. If the TX task allocated ids during the RX task's commit-triggered `save()`, `last_persisted_tx_` is set above the value actually in NVS. The next `tx_persist_margin` (64) allocations then skip persisting. After a reboot, `restore()` = stored + 64 can be at or below an FCNT already used under the **resumed** keys: keystream reuse.
**Evidence (b):** `handleLogin` correctly skips `session_.save()` when `saveSession()` (lsess) fails (finding 4). But the Sequencer's rx-margin hook (every 16 accepted downlinks) and tx-margin hook then call `save()` anyway, writing PersistState with the new base nonce and near-zero counters while lsess still holds the **old** nonces. On resume, `loadSession()` overwrites the base nonce with the old hub_nonce (`SessionManager.cpp:297`), so the node derives the OLD keys with LOW counters. That is the exact hazard H3/finding 4 set out to close.
**Fix:** (a) `save()` passes the written `(tx, rx)` to `notePersisted(tx, rx)`, and Sequencer access is serialised (take `session_tx_mutex_` in `admitSessionCounter_`, or give SessionManager its own mutex). (b) Add a `session_durable_` flag that `saveSession()` sets and `handleLogin` clears before deriving; `save()` refuses while it is false. No NVS layout change. **Effort S.**

### N5 — MEDIUM — Cover target position is neither range-checked nor NaN-checked; in target mode the endstop is disarmed
**Where:** `CmdDispatcher.cpp:1866-1884`, `MotorCtrl.h:163-167`, `MotorCtrl.cpp:408-423`, `MotorPolicy.h:264-274` (`runMsForTarget`, no clamp), `MotorPolicy.h:299-302` (`endstopArmed` returns early when `mode_target_position`).
**Evidence:** with `position = 3.0`, `frac > 1`, so the run time exceeds the full travel and the current-sense endstop is not armed for target moves. The motor drives into the mechanical stop until the absolute ceiling (`kAbsMaxRunMs` = 120 s), and the node then reports `m_Position = 3.0`. A negative value or NaN goes through `sqrtf(negative)` → NaN → `(uint32_t)NaN` (undefined behaviour). The hub/HA normally sends 0..1, but the node is the last line of defence for the motor. `setTargetPosition` also writes two fields read by the motor task without a lock.
**Fix:** `if (!(position >= 0.f && position <= 1.f)) return NotExecuted;` (or clamp), plus clamp `frac` to ≤ 1 in `runMsForTarget`. Pass the target in the motor command instead of writing shared fields. **Effort S.**

### N6 — MEDIUM — A replay of the CURRENT session's LOGIN makes the node transmit twice, with no rate limit (battery DoS)
**Where:** `CmdDispatcher.cpp:3123-3140` (retry-of-live-session branch, before the rate limit at `:3186-3195`).
**Evidence:** the branch for `prior_hub_nonce == login->nonce` calls `sendWakeBeacon()` and `sendAvailable()` and returns before F-30's 5 s limiter. The K_AUTH tag carries no freshness, so one captured LOGIN can be replayed indefinitely, and every copy costs the node two uplinks (CAD + TX ≈ 2×100 ms at ~120 mA). Finding 3(d) moved the limiter so replays cannot starve legitimate logins; that is right, but this branch needs its own floor.
**Fix:** a separate limiter (e.g. at most one opening resend per 5 s, or per `kHubInterFrameGapMs`) on the retry branch. **Effort S.**

### N7 — MEDIUM — Example code in the production RX path: a 50 ms sleep and a hex dump per frame
**Where:** `CmdDispatcher.cpp:5522-5527`: `vTaskDelay(50 / portTICK_PERIOD_MS); // Simulate processing time` and `ESP_LOG_BUFFER_HEX(...) // Example: Print received data as hex`.
**Evidence:** every received frame, including all 17 copies of a burst and neighbours' frames, holds the dispatch task for 50 ms plus a UART hex dump (~10 ms+ at 115200), with only 5 pool buffers. Under burst plus beacon load this is the likeliest cause of "RX data queue full" drops, and it adds dispatch latency to every downlink.
**Fix:** delete both lines (keep the hex dump at `ESP_LOGV` if wanted). **Effort S** (re-run one Mode A/B campaign).

### N8 — MEDIUM — Heap, logging and NVS writes inside the seal critical section
**Where:** `macDataRequest_` (`CmdDispatcher.cpp:596-632`) holds `session_tx_mutex_` across `allocTxId_` → persist hook → `save()` (NVS write), `requestMsgidRelogin_` (`ESP_LOGE`), `mayPackPlaintext_` (`ESP_LOGW`), `packUplinkFrame_` (`malloc` + `memcpy`, `:5388`) and `Mac0Access::dataRequest` (`ESP_LOGI`, `Mac0Access.h:682`).
**Evidence:** the stated rule is no heap or logging in the verify/seal hot path. The primitives in `LinkSecurity` comply; the request path around them does not. A flash write under the mutex blocks the MAC-ping echo (RX task) for its whole duration, which inflates exactly the turnaround H-1 measures. The `malloc` is pointless: the frame is copied into a pool TX buffer straight afterwards.
**Fix:** seal straight into a borrowed TX pool buffer (or a static frame buffer under the mutex). Defer persist writes to after the unlock. Move log lines outside the lock, passing data out by return value. **Effort M.**

### N9 — MEDIUM — Cross-core, unsynchronised MAC-0 state
**Where:** `Mac0Access.h:598-655` (`mark_window_open_`, `missed_marks_` read-modify-written by `noteMarkArmed` on taskLoraRx core 0, `noteMarkMissed` on the DIO1/DIO0 tasks core 1 (`frtosTasks.cpp:584`), and `noteMarkHit`/`noteAddressedHit` on the dispatcher core 0); `setGrid` (`Mac0Access.h:309-363`) writes `grid_` (64-bit anchor + params) on the priority-6 dispatcher while priority-7 `taskLoraRx` reads it in `nextArmDelayUs` (`LoraInterface.cpp:1336,1397`); `drift_rx_us_` "which two tasks write" (`Mac0Access.h:150`).
**Evidence:** only `funnel_` and `classa_` are locked. `taskLoraRx` can preempt `setGrid` half way and arm against a torn grid (new params, old anchor). It self-heals one round later, but it shows up as unexplained missed marks, which is the phase noise the team keeps chasing. Lost `missed_marks_` updates only affect counters.
**Fix:** a `grid_mux_` plus a snapshot-copy accessor (the same pattern as `classaSnapshot_`), and `std::atomic` or the funnel lock for the mark fields. **Effort S–M.**

### N10 — MEDIUM — Pre-session plaintext accepts every application command, not just the bootstrap set
**Where:** `admitPlaintextGate_` `CmdDispatcher.cpp:4613-4643`, `frameadmit::plaintextRejected`.
**Evidence:** until `hasSession_()` (a valid NVS blob or a proven session) is true, plaintext OPERATION, SCHEDULE, TIMESYNC, SYSOP (OTA, SLEEP) and DRIFTTEST are all admitted. That covers a factory-fresh node, or one whose NVS was erased (`main.cpp:293-297` erases NVS on `NO_FREE_PAGES`). Anyone in range can then open blinds, install a schedule, set the clock or trigger OTA. The hub never needs these in plaintext (it defers ops until login), so the carve-out buys nothing.
**Fix:** before a session exists, admit plaintext only for LOGIN, CLIENTCONFIG, GRIDSYNC (no key adoption) and GRIDDEMOTE; refuse the rest. This lives in `FrameAdmission.h` (pure, host-tested). **Effort S.**

### N11 — LOW — ClientConfig hostname logged with `%s` over a non-terminated protobuf `bytes`
`CmdDispatcher.cpp:2043`: `ESP_LOGI(..., "%s", (char *)clientconfig->name.data)`. `ProtobufCBinaryData` is not NUL-terminated, so this over-reads the heap. Also no null check on `clientconfig`. Fix: `"%.*s", (int)name.len, name.data`. **S.**

### N12 — LOW — Unknown sysop is acked as Executed
`handleSysop` `CmdDispatcher.cpp:2001-2011`: an op that maps to `sysCmd == 0` and is neither a mode nor a debug op reaches `setSystemCommand(0)` ("Not a valid command") and returns `Executed`, so the hub believes it was delivered. Return `NotExecuted`, or ack with a NACK status. **S.**

### N13 — LOW — `checkQueuesIdle()` says "cannot enter deep sleep" and then returns true (= sleep)
`CmdDispatcher.cpp:5756-5759`. On the OTA boot path (`regular_start == false`) or after a setup failure this lets the node sleep. Return false, or fix the message to say what is intended. **S.**

### N14 — LOW — Timers created with unchecked returns, so bench modes never expire
`handleModeTest` `CmdDispatcher.cpp:2700-2703` (`esp_timer_create` unchecked, then `esp_timer_start_once` on a possibly-null handle). The same pattern is used for the drift test. If creation fails, ModeTest leaves the sublayers off, logs at WARN and possibly the power profile dropped (240 MHz, no light sleep) until reboot. Refuse to arm when the timer cannot be created. **S.**

### N15 — LOW — DriftTest has no bench gate
`handleDriftTest` `CmdDispatcher.cpp:2258+`: it puts the radio in continuous RX (~11 mA) and blocks deep sleep (`SystemCtrl.cpp:538`). Unlike ModeTest (`modetest::armRefusal`: bench node, authenticated, battery), nothing gates it beyond the plaintext gate. Apply the same refusal policy. **S.**

### N16 — LOW — `rx_frame_` dangles after the early returns in `receiveFrame_`
`CmdDispatcher.cpp:5123-5160`: it is set before PDU decode, and the early returns (`pdu == nullptr`, `admitFrame` false, cmd_id dedupe) skip the reset at `:5165`, leaving it pointing at a pool buffer that goes back to the pool. It is reset at the next frame and nothing reads it in between today, but `beaconMacIsValid` is public. Use an RAII guard. **S.**

### N17 — LOW — Crypto primitive hygiene (`comm_utils.c`)
`import_key_for_alg` (`:73-75`) gives every key ENCRYPT|DECRYPT|SIGN|VERIFY (MESSAGE and HASH), so the CTR key can also MAC and vice versa. Grant only the usage each algorithm needs. `derive_root_key` leaves `hash[32]` (half of it is K_root) on the stack without zeroising. `ctr_crypt_with_key_id` does not `psa_cipher_abort` when `psa_cipher_finish` fails (`:149-152`). **S.**

### N18 — LOW — `node_nonce = esp_random()` with RF off
`CmdDispatcher.cpp:3293`. On ESP32 with Wi-Fi and BT off and no `bootloader_random_enable()`, `esp_random()` is documented as only pseudo-random. It does not matter while K_root is public (H5), but it will once keys are provisioned. Note it in the H5 design. **S.**

---

## 2. Encapsulation

### N19 — MEDIUM — `CmdDispatcher` is effectively an all-public struct
**Where:** `CmdDispatcher.h:59-960` and `:1220-1241` are `public`. That includes every handler (`handleLogin`, `handleGridSync`, `handleSysop`, `admitFrame` and its stages, `receiveFrame_`, `openSessionFrame_`), so any task can reach a handler without going through `admitFrame`. It also includes the raw collaborators (`motCtrl`, `sysCtrl`, `loraIf`), the three command queues, `m0_` (`Mac0Access`), `mac0ForTest()` (a mutable reference), the per-frame state `rx_frame_`/`rx_auth_`/`rx_kauth_ok_`/`rx_cmd_id_`, and dead members `rxPacket`, `txPacket`, `cfgAddress`, `cfgSubnet`, `txbuf_mutex`. There are 25 `*ForTest` seams. `:911-915` says it outright: "do NOT add access specifiers here; an earlier attempt flipped everything below to private and broke the harness at link time".
**Why it matters:** the layering below exists only as method-name conventions. Nothing stops `frtosTasks.cpp` or the hub's proto_sim calling `handleLogin` directly.
**Fix:** add `private:` and give the harness one `friend class CmdDispatcherTestPeer` (in `test/dispatcher_harness.h` and proto_sim). Delete the dead members. Move the `*ForTest` seams into the peer. **Effort M** (the hub's proto_sim must change in step).

### N20 — MEDIUM — PHY and the dispatcher depend on each other; the radio calls straight into MAC/APP
**Where:** `LoraInterface.cpp` includes `CmdDispatcher.h` and makes about 15 calls into it (`noteMarkArmed`, `timedRxActive`, `announceModeChangeIfPending` (`:1299`, which queues an **uplink**), `nextArmDelayUs`, `uplinkCadStartUs`, `classAPendingWindow`, `getBurstEndUs`, `get_free_buffer`/`submit_rx_buffer`). `frtosTasks.cpp:327-584` (DIO tasks) call `noteDriftSample`, `noteFrameDetected`, `noteClassAWindowResult`, `noteUplinkSent`, `noteMarkMissed`. The RX buffer pool type is `CmdDispatcher::rx_buffer_t`, owned by the dispatcher and filled by the radio (`LoraInterface.cpp:476`).
**Why it matters:** about 30 forwarders in `CmdDispatcher.h:280-335` exist only to let PHY reach `m0_`, and PHY can trigger an application-level uplink. That is the opposite of "each layer interfaces only to the next".
**Fix:** give `LoraInterface` and the DIO tasks a `Mac0Access&` (or a small `RadioEvents` interface implemented by `Mac0Access`). Move `rx_buffer_t` and the pool into `LoraInterface` (it already owns the TX pool). The mode-change announcement should be a flag MAC-0 raises and the TX task polls. **Effort L.**

### N21 — MEDIUM — Mutable accessors let callers bypass layer invariants
- `LinkSecurity::sublayers()` returns a non-const reference (`LinkSecurity.h:320`). `handleModeTest` writes `ls_.sublayers().counter_enabled = mt->enablecounter` directly (`CmdDispatcher.cpp:2665-2666`), bypassing `sublayerArmRefusal`. It is only correct because `modetest::armRefusal` happens to check the same things.
- `SessionManager::seq()` returns a non-const Sequencer (`SessionManager.h:108`). `CmdDispatcher` calls `seq().reset()` itself (`:365`, `:3239`), so the reset is split from the nonce and persist policy that should go with it (see N4).
- `Mac0Access`'s banner says "S5c makes the state PRIVATE" (`:44`) and, six lines later, "The state members are still public" (`:50-52`). The state is in fact private, and `friend struct Mac0AccessTest` (`:63`) remains.
**Fix:** intent methods, e.g. `ls_.applyTestSublayers(cfg, ctx)` and `session_.beginNewSession(hub, nonce)` (reset + nonce + persist ordering in one place, under the lock from N1). Make the accessors const. **Effort M.**

### N22 — LOW — The MAC-0 header knows MAC-1 and admission policy, and is not "pure"
`Mac0Access.h:17-28` includes `Mac1Freshness.h`, `FrameAdmission.h` and `TimedModePolicy.h`, plus FreeRTOS queue/task headers and `esp_log`. The banner says the S5b interface is "PURE — no logging, no radio", but `noteClassAWindowResult` (`:747-750`), `noteBurstArrival` (`:918`) and `dataRequest` (`:670-682`) log, and `dataRequest` touches FreeRTOS queues. It also logs under the tag `"CmdDispatcher"` (`:61`), so field logs misattribute MAC-0. Freshness reaching MAC-0 is the documented two-phase design; take it as a `mac::Freshness` value from `MacSap.h` (already the case) and drop the include. **Effort S.**

### N23 — LOW — The application header pulls in crypto and the radio driver
`CmdDispatcher.h:3-45` includes `psa/crypto.h`, `comm_utils.h`, `lora.h`, `<iostream>`, `<sstream>` and `<queue>` (39 includes in all). Every translation unit that needs the dispatcher therefore sees PSA and the SX1278 driver. layer_lint does not catch includes (see N25). **Effort S.**

### N24 — LOW — Global singletons and file-scope RTC state
`extern CmdDispatcher *cmdDispatcher`, `loraIf` and `sysCtrl` are used by `SystemCtrl.cpp`, `MotorCtrl.cpp`, `LoraInterface.cpp` and `frtosTasks.cpp`, which hides the dependency graph. `CmdDispatcher.cpp:62-117`, `:1070-1077` holds 16 `RTC_DATA_ATTR` statics for three unrelated concerns: the wall clock, the beacon wake-clock forensics, and mode-announce/interactive state. Each group belongs in its own small module (`NodeWallClock`, `WakeForensics`, ...). **Effort M.**

### N25 — LOW — layer_lint: two baseline entries are cheap to clear, and the tool has blind spots
`python tools/layer_lint.py` reports `crypto main.cpp:2, phy CmdDispatcher.cpp:1, phy SystemCtrl.cpp:1, txpath 2, ackpath 2`. Baseline verdicts:
- `crypto main.cpp:2`: **one of the two is a string literal** (`"psa_crypto_init() failed: ..."`, `main.cpp:280`). `strip_comments` keeps string literals and then regex-matches them, which is a lint bug. The other is `psa_crypto_init()` in `initPlatform`. That is justified as platform bring-up but redundant: `LinkSecurity::initAuthKey` already calls it. Move it, or whitelist `main.cpp` for `psa_crypto_init` only.
- `phy CmdDispatcher.cpp:1` = `lora_packetRssi()` in `logReceivedPacket_` (`:4485`). **Not justified:** carry RSSI in `rx_buffer_t` next to `rx_us`.
- `phy SystemCtrl.cpp:1` = `lora_clearInterrupts(0xff)` (`SystemCtrl.cpp:524`). **Not justified:** `LoraInterface::phyClearIrqFlags` already exists and is used two lines away.
- `txpath 2` and `ackpath 2` are by design (one request path, ack + re-ack).

Blind spots: no include-graph rule (e.g. `psa/crypto.h` outside crypto files, or `CmdDispatcher.h` included by PHY); no rule for upward calls from `LoraInterface`/`frtosTasks` into the dispatcher; the `mac1` rule matches only the retired SessionManager forwards, so `session_.seq().reset()` in CmdDispatcher passes unnoticed; `test/` is not scanned. **Effort S.**

---

## 3. API layering

### N26 — MEDIUM — The SAP primitives are not the API; there are three header models
`MacSap.h` defines `Mac0DataReq`/`Mac0DataInd`/`SecDataReq`/`SecDataInd`/`TxOpts`. Outside `test/unit_mac_sap_test.cpp` none of them is used. The real calls between layers are bespoke methods taking raw buffers (`verifyWireSession(frame, len, session_id, fcnt)`, `sealWireSession(frame, cap, hdr_len, ...)`, `commitTiming(ticket, f, hdr, bool, bool, now, settled)`). A received header lives in three shapes:
- `macwire::WireHdr` (u8 fields);
- `MacFrameHdr` (`NodeFrames.h:31-44`, the same fields widened to u32 "for old names");
- `mac::MacHdr`.

Two converters link them (`wireHeader` `CmdDispatcher.cpp:5072`, `toMacHdr` `:160`). The truncation guard `linksec::fitsMacHdr` (`LinkSecurity.h:72-84`) can no longer fail, because the source fields are u8, and its comment ("the CMAC is computed over the MacHdr") describes the deleted v0 wire.
**Fix:** make `WireHdr` (or `MacHdr`) the single decoded header, delete `MacFrameHdr`'s widening and `fitsMacHdr`, and either route the layer calls through the SAP structs or delete them and say the SAP is the methods. **Effort M.**

### N27 — MEDIUM — God class: `CmdDispatcher` (5 804 + 1 241 lines, about 200 members)
It owns all of the following:
- the RX pipeline (decode, auth, admission);
- uplink framing and sealing;
- the session entity (LOGIN, REGISTER retry, resume fallback, beacon ladder);
- MAC management (GridSync, beacon, demote, MacControl);
- bench tooling (ModeTest about 700 lines with about 25 `mt_*` members, DriftTest, sweep);
- six application handlers;
- auto mode, schedule and interactive override;
- the wall clock;
- the battery cache;
- debug flags in NVS;
- deep-sleep and queue-idle policy;
- four task loops and the RX buffer pool.

Largest and most complex functions (lizard, CCN > 10):

| function | NLOC | CCN |
|---|---|---|
| `scheduletext::parse` (ScheduleText.h:120) | 146 | 51 |
| `motorpolicy::nextState` (MotorPolicy.h:92) | 73 | 42 |
| `otaservice::task` (OtaService.cpp:243) | 169 | 35 |
| `CmdDispatcher::handleModeTest` (:2433) | 162 | 34 |
| `wifiprov::stop` / `event_handler_prov` | 84/120 | 27/27 |
| `LoraInterface::armNextRxWindow` (:1237) | 120 | 23 |
| `LoraInterface::armTimedRxWindow` (:578) | 63 | 22 |
| `app_main` (main.cpp:570) | 117 | 22 |
| `CmdDispatcher::handleLogin` (:3084) | 95 | 20 (274 lines incl. comments) |
| `serviceDio0Event` / `serviceDio1Event` | 121/55 | 19/19 |
| `handleDriftTest`, `handleSchedule` | 77/63 | 18/18 |
| `CmdDispatcher::serviceTxCommand` (:312) | 149 | 14 (240 lines) |

**Proposed split (no behaviour change; each piece is constructible on the host):**
1. `MacReceiver`: `receiveFrame_`, `openFrame_`, `openSessionFrame_`, `admitFrame` and stages, `extendFireRound_` (~900 lines).
2. `MacUplink`: `macDataRequest_`, `packUplinkFrame_`, seal/plain, `allocTxId_`, owning `session_tx_mutex_` (~300).
3. `SessionEntity`: `handleLogin`, register/resume/beacon-retry timers, `loadPersistentState`, superseded-nonce seeding (~700). Its `beginNewSession()` is the N1/N4 lock point.
4. `MacManagement`: GridSync, GridBeacon, GridDemote, MacControl (~600).
5. `BenchTools` (ModeTest + DriftTest + sweep, behind `CONFIG_BLINDS_BENCH_TOOLS`) (~1 200).
6. `AppController`: the six handlers, auto mode, schedule, interactive override (~1 200).
7. `TelemetryBuilder`: the per-case bodies of `serviceTxCommand` (one function per uplink case).
8. `NodeWallClock`: the clock statics and `formatLocalTime`.

`CmdDispatcher` then becomes the ~200-line wiring object. **Effort L** (stage it, like S0–S8).

### N28 — LOW — Duplicated persistence entry points
Three wrappers reach `SessionManager::save()`: `savePersistentState()`, `maybePersist_()` and `startOTA()`'s direct `session_.save()` (`:1772`). There are also dead duplicates of the NVS namespace and key at `CmdDispatcher.cpp:65-66` (`"loractr"` / `"peerstate"`, unused). Editing one copy and not the other is how an NVS layout break happens, which is the one thing that must never change. Delete the dead constants and keep a single persist API on SessionManager. **Effort S.**

### N29 — LOW — Bench and diagnostic surface mixed into the production API
`CmdDispatcher.h:521-578` (ModeTest sinks, made public "so the radio-facing code can feed them without a friend"), `MacCounters`, sweep mis-arming, `noteMarkOutcome` ("kept for callers that know both halves"), and register-level forensics in `serviceDio1Event` (`frtosTasks.cpp:561-580`). Group them behind one `diag::` interface compiled for bench builds. **Effort M.**

---

## 4. Human readability

### N30 — MEDIUM — Essay comments bury the code
Comment-only lines: `CmdDispatcher.cpp` 2 009 of 5 804 (35 %), `CmdDispatcher.h` 632 of 1 241 (51 %), `Mac0Access.h` 577 of 1 533. Examples:
- `admitPlaintextGate_` has a ~125-line preamble (`:4488-4612`) for a 30-line function, describing four exemptions in the order they were added, including the now-removed `BaseNonceExchange`.
- `admitSequence_` has 40 lines of history (`:4857-4896`).
- `handleLogin` is 274 lines, of which about 95 are code.
- At least 9 "Measured 2026-.. on node 2" anecdotes and 45 "F-nn / finding / review" references in the .cpp.

Comments that describe what *used to* happen ("It used to...", "Before this...", "F-39: removed 2026-05-25") belong in commit messages or `docs/`. A new reader cannot tell current behaviour from history.
**Fix:** a comment budget. Keep per function a 1–5 line *why* plus the invariant, with a link to the doc or commit for the history. Lean on `static_assert`s and the policy-header tests, which already encode most invariants. Start with `admitFrame`, `handleLogin` and `CmdDispatcher.h`. **Effort M.**

### N31 — MEDIUM — Stale or misleading comments (each one is wrong today)
- `CmdDispatcher.cpp:1898-1901` (`handleOperation`) and `:2006-2010` (`handleSysop`): "the hub retransmits with a fresh (incrementing) msgid ... each copy acknowledged independently". Since S7/S8, retries are pack-once (same FCNT) and command identity is `cmd_id`.
- `CmdDispatcher.cpp:1749-1753` (`startOTA`): "handleSysop() calls sendCommandAck() right after setSystemCommand()". The ack is now `deliverApp_` → `ackExecuted_`, after the handler returns.
- `PortMap.h:11-12`: "Until the wire stage (S8) the port is not carried in the frame; it is COMPUTED here". S8 is live, the port is on the wire, and the table is now a consistency map.
- `LinkSecurity.h:72-78` (`fitsMacHdr`): "The CMAC is computed over the MacHdr". The v1 tag is over the raw bytes.
- `Mac0Access.h:44` vs `:50`: the private/public contradiction (N21).
- `CmdDispatcher.h:45`: two comments fused onto one `#include` line ("oneof -> mac::Port ... // MAC-2: keys, Encrypt-then-CMAC ...").
- `CmdDispatcher.h:197-216`: the P2b resume banner runs straight into the REGISTER-retry banner, so the declarations under it are mislabelled.
- `CmdDispatcher.cpp:5561-5563`: banner "F-4: Acknowledge a received command" above `noteAnchorFixed_()`.
- `admitAddress_`/`admitSequence_` comments still refer to `rx_message_id_`, `destAddress` and `onReceiveNew`'s body; those are now `Sequencer`, `Mac0Access::retargetUplink` and `receiveFrame_`.
- `CmdDispatcher.h:47` says "Old Packet class definition removed", yet `Packet.h` is still included and `rxPacket`/`txPacket` are still members.
- `main.cpp:310-312`: `timeout_ms = 8000; // 30 seconds` and `idle_core_mask = 0; // Monitor idle tasks on both cores` both contradict the code.
- `LoraInterface.cpp` and `Mac0Access.h:67`: "LoraHeader.destaddress" (deleted type).

**Fix:** one sweep PR. **Effort S.**

### N32 — LOW — Dead code
- Unused members: `rxPacket`, `txPacket`, `cfgAddress`, `cfgSubnet`, `txbuf_mutex`, and `rx_pool_mutex` (created at `:5411`, never taken).
- `kPersistNvsNamespace`/`kPersistNvsKey` (`:65-66`).
- `static portMUX_TYPE mux` (unused, `frtosTasks.cpp:255`).
- The `TICKS_TO_WAIT` ISR throttle (`myinterrupts.cpp:43-49`, constant 1). This is a trap: set to 2, the ISR disables the pin and does not queue the event, so nothing re-enables it and the node goes deaf.
- `handleNotSet` and `onReceiveNew` (a 1-line forwarder).
- The proto_sim compatibility `handleSysop(msg, hdr)` overload in the header.
- SessionManager's legacy forwards (`nextTxId`, `acceptRxId`, ...) kept for the hub's tests.
- `noteMarkOutcome`.
- `example_deep_sleep_register_ext1_wakeup` (`utilities.cpp:193`); the task name `"advanced_ota_example_task"`; `CONFIG_EXAMPLE_*` symbols in `SystemCtrl.cpp:408` and `OtaService.cpp:250`.

**Effort S.**

### N33 — LOW — Magic numbers
- `vTaskDelay(50)` (N7).
- `LOGIN_RATE_LIMIT_MS` as a function-local static (`:3186`) while its sibling `kGridDemoteRateLimitMs` is a class constant.
- SX1278 register addresses `0x01/0x18/0x1B/0x14/0x15/0x1E/0x1F` and `rssi_raw - 164` inline in `serviceDio1Event` (`frtosTasks.cpp:568-580`); they belong in the PHY as named readers.
- The hand-rolled MAC-to-u64 loop repeated twice (`:373-386`, `:2024-2037`).
- Priorities and stack sizes in `createApplicationTasks` scattered with "F-7: increased from 4096".

**Effort S.**

### N34 — LOW — Inconsistent naming and conventions
- `msgid` and `FCNT` mean the same thing (`MacFrameHdr::msgid // the FCNT`).
- Member styles mix `snake_case_`, `camelCase_`, `camelCase` and `rx_memory_pool`; queues are `rxCmdQueueNew`/`txCmdQueueNew` ("New" relative to what?).
- Method suffixes `handleX`, `handleX_`, `noteX_`, `processX` and `serviceX` carry no consistent meaning.
- Typos in identifiers and logs: `setCmdDispathcer` (`main.cpp:669`), `taskMotorContol`, `"Dest Adreess"` (`:5046`), `"semaphor"`.
- Handlers marked "Extracted verbatim" keep 4-space bodies inside 2-space functions, and the same 3-line "Extracted verbatim from the onReceiveNew switch ..." comment is pasted above 8 handlers.

**Effort S** (rename pass, mechanical).

### N35 — LOW — Onboarding
`README.md` is empty (0 bytes). `main/include/README` is PlatformIO boilerplate about C header files. `test/README` is 11 lines. All design documentation lives in the other repo. There is no file map, no task/core/priority table, no explanation of "S0–S8"/"F-nn"/"N-n", and no pointer to the hub's docs. Add a 1–2 page `README.md`: architecture diagram, the task table, the read-this-first path below, and how to run the host tests and `layer_lint`. **Effort S.**

### N36 — NIT — `logIncomingFrame_` logs six INFO lines per frame before any filtering (`:5043-5051`)
These are on top of `logReceivedPacket_` and the N7 hex dump. At 115200 baud each line costs ~1 ms of dispatch time. Demote to DEBUG.

### N37 — NIT — `checkQueuesIdle()` uses `printf` while the rest of the file uses `ESP_LOG*` (`:5769-5799`)

### N38 — NIT — `ack.status` is always `ACK_OK`
No NACK path exists, although `AckStatus` has values for it. This ties in with N12.

### N39 — NIT — `Sequencer::reset()` comment: "The persisted marks are deliberately left alone (unchanged behaviour)"
It does not say why this is safe. It is safe only because every `reset()` is followed by a successful `save()` that resets them, and N4(b) shows that does not always hold. Document the precondition, or reset `last_persisted_*` to `UINT32_MAX` to force a write.

---

## What is good

- **Policy headers with host tests.** `FrameAdmission.h`, `Sequencer.h`, `ClassAWindows.h`, `MotorPolicy.h`, `TimedModePolicy.h`, `AckCache.h` and `ModeTestPolicy.h` keep the decisions pure and testable, and the dispatcher stages only hold the side effects. This is the right architecture, and the reason most of the findings above are cheap to fix.
- **The receive order is right and visible.** Shape → address filter before any crypto → tag before cipher → FCNT space only after the tag → decrypt → admission (plaintext gate before the replay ratchet) → retarget last. The REGISTER/LOGIN bypass is where the docs say it is, and LOGIN's MIC is checked before any state change.
- **`Sequencer.h`** is a small, clear, shared MAC-1 core: two-phase check/commit, a sliding-window bitmap, fail-closed allocation with persist-before-handout, and restore that marks the margin seen. Its only problems are the concurrency and snapshot issues in N4.
- **`MacWire.h` decode** is strict: version, reserved bits, port range, length checks before any field is read. **`LinkSecurity`** primitives allocate nothing and log nothing; key material is zeroised after import; it uses `psa_mac_verify` (constant time).
- **Invariants encoded as `static_assert`s**: quiet window ≥ fallback + hub gap; the beacon ladder fits inside the fallback; RX-arm priority > dispatch priority; the burst stride; PDU case numbers equal DL/UL numbers.
- **Hardening that holds up:** the burst-end clamp against forged headers, the GridDemote rate limit, the superseded-hub-nonce ring persisted across sleep, cmd_id at-most-once execution, IRAM-safe ISRs with the panic handler in IRAM, deep sleep refused while a wake source is pending, and the 120 s drain cap.
- **The `layer_lint` ratchet** is a good mechanism: cheap, enforced, and honest about what remains.

## Readability verdict

**Hard for a newcomer, though the problem is mostly volume rather than structure.** The core ideas (layers, the admission pipeline, the policy headers) are sound and individually well named. The difficulty is:
1. One 7 000-line class where every concern meets.
2. Comments that are often longer than the code and narrate history, some of it no longer true (N31). A reader has to work out which paragraphs describe current behaviour.
3. Three representations of the same header, and the dispatcher's 30 forwarders, which make "who owns this state" a search exercise.
4. No in-repo orientation (N35).

A competent embedded engineer would need about 2–3 days to find their way around safely. After a comment-budget pass (N30/N31), the dead-code sweep (N32) and the first split steps (N27: MacReceiver, MacUplink, SessionEntity), that would be under one day.

**Read-this-first path (current code):**
1. Hub repo `CLAUDE.md` (Protocol layers) → `docs/mac-wire-format.md` §2–4.
2. `main/include/MacSap.h` (the layer vocabulary) → `MacWire.h` (the header codec) → `NodeFrames.h` (internal frame types) → `MacWirePdu.h` (body ↔ view) → `PortMap.h`.
3. `Sequencer.h` (MAC-1) → `LinkSecurity.h` (MAC-2) → `FrameAdmission.h` (admission decisions).
4. `CmdDispatcher.cpp`: `receiveFrame_` (:5091) → `openFrame_`/`openSessionFrame_` (:5172/:5208) → `admitFrame` (:4937, read the stage list at :4380-4399 and skip the long preambles) → `dispatchCommand` (:4227) → `deliverApp_` (:4319) / `handleManagement_` (:4258) / `handleLogin` (:3084).
5. TX: `serviceTxCommand` (:312) → `macDataRequest_` (:593) → `packUplinkFrame_` (:5358).
6. Boot and tasks: `main.cpp` `app_main` (:570), `decideBootSession` (:433), `createApplicationTasks` (:334); `frtosTasks.cpp` `serviceDio0Event` (:253); `LoraInterface::armNextRxWindow` (:1237).
7. Only then `Mac0Access.h`, preferably read through its public section (:235 onward).

**Hard to follow:** where a frame's per-frame state lives (`rx_frame_`, `rx_auth_`, `m0_.setRxStamp`, `ls_.setFrameAuthenticated`, all set in different places); which task calls what (there is no task diagram, and the threading rules are spread across member comments); the session lifecycle (spread across `handleLogin`, the REGISTER case in `serviceTxCommand`, `loadPersistentState`, `SessionManager` and `LinkSecurity`).

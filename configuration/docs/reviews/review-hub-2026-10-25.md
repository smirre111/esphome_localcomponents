# Hub review: LoRa blinds hub at HEAD 29c5b1e

Read-only review of `c:\Development\esphome\configuration`, covering `local_components/{lora_client,lora_tracker,loracover,blindsproto}`, `tools/layer_lint.py` and `loradevices.yml`, checked against CLAUDE.md, `docs/mac-separation-layering-plan.md` and `docs/mac-wire-format.md`. Nothing was edited, built or flashed. I ran `tools/layer_lint.py` (result: 16 metrics, none above baseline) and lizard in WSL (`-C 10 -L 80`).

Not re-reported, as agreed: Mode B phase-error / H-1 timing variation, node 1 position, the undecided shadow-mask question, and the K_root dev literal (H5).

## Tree size

| file | lines | comment lines |
|---|---|---|
| lora_client/lora_client.cpp | 6171 | 2096 (34 %) |
| lora_client/lora_client.h | 1894 | 983 (52 %) |
| lora_tracker/lora_tracker.cpp | 1752 | 466 |
| lora_tracker/lora.cpp (PHY driver) | 1438 | - |
| lora_tracker/lora_tracker.h | 680 | 373 |
| lora_client/TimedModePolicy.h / ModeTestPolicy.h / HubLinkSecurity.cpp / ModeBSupervisor.h / GridState.h | 600 / 561 / 540 / 519 / 505 | |

`LORAListener` has about 230 member functions and about 200 data members in one class.

Functions that lizard flags at CCN > 10 or length > 80:

| function | NLOC | CCN | length (incl. comments) |
|---|---|---|---|
| `LORATracker::sendPacketBurst` | 122 | 33 | 227 |
| `scheduletext::parse` | 146 | 51 | 156 |
| `scheduletext::format` | 52 | 21 | 55 |
| `LORATracker::send` | 70 | 17 | 106 |
| `LORAListener::handle_mode_test_report_` | 77 | 16 | 124 |
| `LORAListener::setup` | 91 | 15 | 136 |
| `LORAListener::dlv_attempt_` | 35 | 14 | 35 |
| `macwire::decode` | 48 | 14 | 49 |
| `LORAListener::start_mac_ping` | 65 | 13 | 95 |
| `LORAListener::next_wake_epoch_` | 42 | 13 | 53 |
| `LORAListener::handle_register_` | 76 | 11 | 152 |
| `LORAListener::send_into_class_a_window_` | 63 | 11 | 139 |

Functions that are short in code but long because of comments: `send_login` (41 NLOC, 137 lines), `confirm_session_` (37 NLOC, 123 lines), `mode_test_timer_cb_` (48 NLOC, 107 lines), `fill_grid_sync_` (26 NLOC, 84 lines).

---

## Findings

### Critical
None found.

### High

**HB1 — ModeTest and drift frames take a session FCNT and write NVS state from the esp_timer task, and the comment that guards against this is wrong.** Effort: M.
- **Where:** `lora_client.cpp:4596-4701` (`mode_test_timer_cb_`) → `build_mode_test_frame_` at `:4575` → `mac_frame_(op,&f)` at `:3852-3855` → `incrTxMessageId()` at `:3795-3826` → `seq_.allocate(persist_())` → `persist_hook_` at `:4054-4059` → `save_state_(true)` at `:265-285` → `rtc_.save()`. The drift test does the same: `drift_timer_cb_` at `:4526` → `build_drift_frame_` at `:4843`.
- **Evidence:** the comment in `incrTxMessageId` (`:3797-3801`) says *"The esp_timer callbacks (mode_test_timer_cb_/drift_timer_cb_) send frames prebuilt by build_*_frame_() and do not call this, so today every caller is on the ESPHome loop."* That is false: each callback rebuilds the next frame on the timer task. Because the Sequencer persist margin is 1, every mark persists.
- **Why it matters:**
  - **Production runs this, not only the bench.** `ModeBSupervisor` starts a 60 s ModeTest on every re-establishment, which means about 40 marks, each doing `rtc_.save()` off the loop task.
  - **ESPHome preferences are not safe to call from two tasks.** ESPHome's ESP32 preference backend `save()` appends to a global, unsynchronised pending-save vector. The main loop saves and syncs the same vector, so the two can corrupt it. The likely result is a rare heap corruption or crash.
  - **The Sequencer is not consistently locked.** `seq_.commit()` runs on the loop without `session_tx_mutex_` (`setRxMessageId`, `:4049-4052`). `allocate()` runs under that mutex on the timer task. Both write `last_persisted_*`.
  - **The design already knew this was wrong.** The MAC-ping path deliberately reserves a block of FCNTs so that it never calls `incrTxMessageId()` off-loop (`lora_client.h:1116-1123`, `start_mac_ping` at `:1841-1874`). ModeTest and drift skipped that safeguard.
- **Fix:**
  - Give ModeTest and drift the same pre-reserved FCNT block that MAC ping uses, so no allocation happens off the loop.
  - Or build the next frame on the loop: the timer posts an event and the loop task builds and queues.
  - Take `session_tx_mutex_` around every `seq_` access, or make the Sequencer loop-only.
  - Correct the comment.

**HB2 — State shared across tasks (loop, esp_timer, tracker send task) is read and written without synchronisation.** Effort: M.
- **MAC ping calls loop-owned code from the timer task.** `mac_ping_timer_cb_` (`lora_client.cpp:1786`) calls `send_aligned_` on the esp_timer task. That function writes `last_placed_t0_us_`, `last_placed_single_shot_` and `last_placed_optimistic_` (`:2713`, `:2722`, `:2781`) and calls `hubBeliefNow_()`. The header says these members belong to the loop (`lora_client.h:1099-1101`, *"deliberately not last_placed_t0_us_, which belongs to the ESPHome loop"*). `place_ctl_` (`:2848-2853`) reads them straight after its own `send_aligned_`, so a ping tick in between gives a tracked command the wrong `sent_single_shot` / `mark_us`. That breaks Rule 4 and the ack-latency samples.
- **The tracker's `send()` runs on two producer tasks with no lock.** In `send()` (`lora_tracker.cpp:938-1043`), `supersede_.note()` (`:1001`), the read-modify-write of `queued_air_end_us_` (`:1015-1021`) and `placed_busy_until_us_` (`:1034-1035`) all run on both the loop and the esp_timer task (ModeTest marks). `supersede_.isCurrent()` is read on the send task (`:427`) and `burst_busy_until_us_` is written there (`:457`). The `int64_t` fields are torn on 32-bit Xtensa. A torn update to the `SupersedeTable` can drop the current cover op at the head of the queue, or send a superseded one. The code admits the problem in a comment (*"Written without a lock, like burst_busy_until_us_"*, `:1027`) and clamps it in `ack_wait_ms_` (`lora_client.h:1280-1283`), but does not fix it.
- **Stopping a ModeTest races the running callback.** `stop_mode_test_` (`:4789-4810`) sets the plain bool `mode_test_active_ = false`, calls `esp_timer_stop()` (which does not wait for a callback already running), then calls `build_mode_test_frame_(false)` on the loop. A callback that is mid-flight writes `mt_frame_` and `mt_seq_++` at the same time, so the STOP frame can come out torn. The supervisor's abort path takes this route. `mode_test_active_`, `mac_ping_active_`, `drift_test_active_`, `mt_frame_len_` and `lora_tx_busy_` are all plain `bool` / `size_t` shared across tasks.
- **Fix:**
  - Put the tracker's producer-side bookkeeping (`supersede_`, `queued_air_end_us_`, `placed_busy_until_us_`, `burst_busy_until_us_`) under `reservations_mutex_`, or move it to the send task when the frame is drained.
  - Make the active flags `std::atomic<bool>`.
  - Give the timer callbacks their own placement state, as ModeTest already does with `mt_last_mark_t0_us_`.
  - Make stop paths wait for the callback, for example with a "stopping" flag plus a wait.

### Medium

**HB3 — A new Control command whose first pack fails resends the previous command's stored bytes.** Effort: S.
- **Where:** `dlv_attempt_` (`lora_client.cpp:3217-3251`).
- **Evidence:** `s.frame` is overwritten only `if (framed)` (`:3241-3245`). For a SameBytes request the function returns `f.msgid` even when packing failed (`:3248`), so `dlv_tx_` sets `awaiting_ack` and arms the timer (`:3287-3291`). The retry then takes `retry && same_bytes && !s.frame.bytes.empty()` (`:3220`) and resends `s.frame` — the last successful command, which may be an earlier OPEN, a SLEEP or an OTA. `s.frame` is cleared only in `clearSessionKeys_` (`:1370`), not in `deliver_` (`:3089`) or when a command is acked. The same stale frame also defeats the at-most-once OTA check `resendable()` (`:2883-2885`), because the frame is not empty.
- **Trigger:** a seal refusal (FCNT spent or no key), an oversized body, or an allocation failure.
- **Fix:** clear `s.frame` and `s.frame_tag` in `deliver_` (or whenever `!retry`), and return 0 when a first attempt could not be framed.

**HB4 — Unauthenticated plaintext uplinks before a session exists move MAC-1 and feed the hub's timing and beacon beliefs.** Effort: M.
- **Where:** `admit_frame_` (`:1029`, `seq_.check`) → `accept_plaintext_uplink_` (`:2251-2254`) → `commit_rx_msgid_` (`:1124`, rx commit + `mac0_commit_timing_`) → `deliver_uplink_` → `dispatch_payload_` (`:1966`, `handle_beacon_`) and `dispatch_mac_payload_` (`:1995`, `notePhaseReport_` + `dlv_ack_`).
- **Evidence:** while `!session_confirmed_` — that is, after every LOGIN (`:4127`) and after a hub boot — a forged AUTH=NONE frame carrying the node's SRC and an FCNT within rx+1024 is admitted and committed. After `send_login` resets the counters to 0 (`:4120`), one forged frame with FCNT 1000 makes the hub reject the node's real session-opening uplink (FCNT around 1) as "duplicate or old". The session cannot confirm until the next login retry, so an attacker can repeat this indefinitely: a pre-authentication DoS.
- **The same path drives more than the counter.** A forged beacon runs `note_node_reset_` (advances the hub TX counter), `regrid_after_reboot_`, `note_beacon_clock_` (moves `last_beacon_epoch_`, which feeds the sleep model and `beacon_overdue_`), `notePhaseReport_`, and publishes the auto-mode switch to HA.
- **The comment is wrong.** `:5093-5095` says *"handle_beacon_ runs only for a DECRYPTED beacon, which is what makes this an authenticated observation"* — but the plaintext path reaches it.
- **Fix:**
  - Pass `authentic` into `dispatch_payload_` and `dispatch_mac_payload_`. On unauthenticated frames, do not commit MAC-1, do not commit timing, and ignore PhaseReport, ACK and the beacon's MAC fields.
  - Keep a separate plaintext high-water mark for bootstrap duplicate suppression instead of moving `seq_`.

**HB5 — After a hub reboot, cover ops and sysops go out in plaintext until the first LOGIN.** Effort: S.
- **Evidence:** `relogin_pending_` defaults to false (`lora_client.h:1010`) and is only set by `handle_register_` (`:957`) or `send_login` (`:4128`). A node whose session was restored is registered, but its LOGIN is delayed: 5 s plus stagger plus the onboarding gate, or until the node's modelled wake (`schedule_startup_login_`, `:606-648`). In that window `ctl_begin_` (`:3006`) is not deferred, and `v1_frame_` packs plaintext (`encrypt=session_confirmed_=false`). The node, which still holds its NVS session, refuses the frame. After about 15 s of retries the command ends in FailRelogin. The same applies to Fire requests: `fireDeferred` is false.
- **Fix:** in `setup()`, set `relogin_pending_ = true` when `registered_` (no session is confirmed yet), or gate on `!session_confirmed_ && registered_`.

**HB6 — `sleep_duration` is silently truncated to 16 bits, and its default is `False`.** Effort: S.
- **Evidence:** `lora_client.h:302` has `set_sleep_duration(uint16_t)`, but the member is `uint64_t` (`:1401`) and the schema is `cv.int_range(0, 864000)` with `default=False` (`lora_client/__init__.py:344`). A configured `86400` (the header's own default) becomes 20864 s. An omitted key becomes 0 and is pushed to the node in `ClientConfig`. Production sets 21600, which fits.
- **Fix:** make the setter take `uint32_t`, give the schema a sane positive default, and add a test that pins it.

**HB7 — The session id and the key epoch are coupled only by call order.** Effort: S.
- **Evidence:**
  - `SessionKeyStore::setSessionId` (`HubLinkSecurity.cpp:35-39`) does not move `key_epoch`.
  - `retagFrameV1` reads the session id under `session_mutex_` and the key epoch later under `keys_mutex_` (`:495-506`). `sealFrameV1` does the same (`:407-421`).
  - It is correct today only because every caller that sets a new session id calls `clearSessionKeys_()` first (`send_login`: `:4089`, then `:4133`). `deriveSessionKeysForTest` (`lora_client.cpp:1389-1393`) already breaks that rule.
- **Why it matters:** a future caller can produce per-copy tags under (new session id, old key) that the node rejects, with nothing in the store to stop it.
- **Fix:** keep the session id inside `Keys` under `keys_mutex_`, bump `key_epoch` whenever it changes, and read it inside `macUnderKeyLock_`.

**HB8 — Heap allocation and logging sit in the per-copy send path.** Effort: S.
- **Evidence:**
  - `sendPacketBurst` calls `malloc(clen)` and `free` for every copy between stamping and firing (`lora_tracker.cpp:1191-1196`). It also logs `ESP_LOGW` per copy on a seal failure or a late mark (`:1184`, `:1206`, `:1214`), plus `ESP_LOGI "Sending packed burst"` (`:1059`).
  - `serviceTxQueue` logs `ESP_LOGI` and `ESP_LOG_BUFFER_HEX` of every frame on the send task, right before the post-burst RX window (`:441`, `:474`).
  - `send()` logs `"Free buffers available"` at INFO on every call, including from the esp_timer task (`:961`).
  - Elsewhere: `sealFrameV1` does `new V1SealWork` (`HubLinkSecurity.cpp:414`), `uplink_decrypt_` mallocs (`lora_client.cpp:1570`), and `v1_frame_` creates a 255 B `std::vector` per frame.
- **Why it matters:** the stated rule is no heap in the seal or verify hot path. This sits beside, not inside, the known H-1 timing variation.
- **Fix:** copy into a 255 B buffer on the stack or a member buffer (`WireBurstFrame` already holds one, so `sendPacketAt(frame.data(), frame.len())` would do), demote these logs to DEBUG or counters, and drop the hex dump.

**HB9 — The shadow pending mask has no inputs in production.** Effort: S.
- **Evidence:** `HubMacManagement::sources_` is filled only by `LORATracker::register_listener` (`lora_tracker.cpp:1536-1540`). The only caller is `SleepTrigger`'s constructor (`automation.h:35`), and the `SleepTrigger` codegen is commented out (`lora_tracker/__init__.py:108-305`). So every `beacon shadow` line evaluates zero nodes. The plan doc mentions this (`mac-separation-layering-plan.md` S5: *"only SleepTrigger objects reach register_listener()"*). CLAUDE.md still tells the reader to collect shadow data over a soak.
- **Scope:** this is not the "shadow mask undecided" question. It means the data that decision depends on cannot exist.
- **Fix:** register the real listeners — `register_client` could call `mgmt_.addShadowSource` — or delete the shadow plumbing until it is needed. Delete `SleepTrigger` (see HB18).

**HB10 — The god class: public data and children reaching into internals.** Effort: L.
- **Most mutable state is public.** In `lora_client.h`, the public sections run `:289-894`, `:1148-1364`, `:1368-1369` (`LORATracker *parent_`), and `:1388-1517`. They expose:
  - MAC-1 and MAC-2 internals: `seq_`, `session_tx_mutex_`, `session_confirmed_`, `login_acked_`, `pending_login_nonce_`.
  - Addresses and registration state: `short_address_`, `subnet_address_`, `registered_`.
  - The schedule tables `sched_entries_` and `auto_mode_`, and the beacon fields.
  - All `mbs_*` supervisor glue (`:1244-1274`), `ack_wait_ms_`, `incrTxMessageId`, `setRxMessageId`, `persist_hook_`, and more than 25 `*ForTest` seams mixed into the production API.
- **Other places that reach in:**
  - `LORAClientRestoreState::apply` writes the internals directly (`:1878-1883`).
  - `LoraCover` (sensor) reads `parent_->parent_->get_last_rssi()` (`lora_sensor.cpp:95-96`). That is an APP child reading the PHY's global "last packet" RSSI. It is correct only because dispatch is synchronous.
  - Children hold the whole `LORAListener*`.
  - `LORATracker` exposes the internals of its send engine: `sendPacketOnce`, `sendPacketBurst`, `preparePacket`, `firePacket`, `sendTask`, `serviceTxQueue`, `drainHandoffQueue_` (`lora_tracker.h:377-455`).
- **Fix:**
  - Make data private, behind accessors.
  - Move test seams into a `friend struct LORAListenerTestAccess`.
  - Give children a narrow `NodeAppPort` interface: `send_cover_operation`, `send_cover_config`, schedule setters, and an `AppIndication` that carries RSSI and SNR.

**HB11 — Layer crossings that `layer_lint` does not see.** Effort: M.
- **The MAC-0 header pulls in the application.** `lora_tracker.h:18-19` includes `lora_client.h` (the whole APP god class) and `blinds.pb-c.h`.
- **The MAC management unit depends on APP.** `mac_mgmt.cpp` includes `lora_tracker.h` and calls `LORAListener::pending_shadow_inputs` and `get_name`.
- **Every loracover platform pulls in PSA and MAC-2.** `lora_client.h:24, 47` includes `HubLinkSecurity.h` and `<psa/crypto.h>`. `lora_tracker.cpp:25-28` still includes PSA *"for the beacon MAC"*, which moved to `mac_mgmt`.
- **APP code sets MAC-1 counters directly:**
  - `advance_tx_for_boot_` (`:5230`, from the beacon handler).
  - `start_mac_ping`'s reservation (`:1866-1873`).
  - `LORAClientRestoreState::apply` (`+64`).
  - A Sequencer API (`skipTx(n)`, `reserveTx(n)`) would own these.
- **Why the lint misses them:**
  - It matches identifiers with regexes, file by file. It covers only `lora_tracker.cpp`, `lora_client.cpp` / `.h` and two children.
  - Moving code into a sibling file of the same component (`mac_mgmt.cpp`, `HubLinkSecurity.cpp`) zeroes a metric without changing which way the dependency points.
  - It does not check includes.
  - It can be dodged by renaming (e.g. `incrTxMessageId`).
- **The current baseline is justified:**
  - `raw_parent_send = 1` is `mac0_send_` itself.
  - `msgid_alloc_sites = 3` are `mac_frame_`, the ping reservation and `send_login`.
  - `client_iteration = 3` is the RX fan-out in `checkReception` plus `register_client`.
- **Fix:**
  - Add include-direction rules to the lint: `lora_tracker/*` must not include `lora_client.h` or `blinds.pb-c.h`, except in `mac_mgmt`; only `HubLinkSecurity.*` may include `psa/`.
  - Scope metrics by component directory, not by file.

**HB12 — Every listener sends the startup broadcast GridDemote.** Effort: S.
- **Evidence:** `LORAListener::setup()` (`:102`) calls `broadcast_grid_demote()`, which queues an identical broadcast 17-copy burst once per listener. With 2 nodes that is 2 bursts. With the planned 32 nodes it is about 45 s of air at boot, and the 5-buffer pool drops most of them, so logins are delayed and pool failures are counted.
- **Fix:** send it once from the tracker, in `setup()` after `startGrid()`.

### Low

**HB13 — `send_into_class_a_window_` ignores the result of `mac0_send_`.** `:5634-5648`: it returns `Placed` and counts `class_a_stats_.offered++` even when the pool refused the frame. That silently loses a TimeSync and inflates the Mode C ledger. Fix: propagate the bool and add `ClassAPlacement::Dropped`. Effort: S.

**HB14 — REGISTER replay is still a sustained DoS lever.** The 5 s rate limit (`:877-888`) stops rapid replay, but one captured REGISTER replayed every 5 s keeps calling `clearSessionKeys_` and forcing a relogin on the node indefinitely. Fix: bind REGISTER freshness to a node boot counter, or to a NODE_NONCE that the hub remembers per MAC (refuse a repeat). This needs a node change. Effort: M.

**HB15 — Control-channel ack ids are not cleared on FCNT reset.** `dlv_forget_ids_` (`:3490-3494`) clears only Config and Mac, while `send_login` resets FCNT to 0. A Control request still waiting (DeferRelogin) keeps old-session ids that can alias new-session FCNTs. It is benign today because channel order and re-pack mitigate it. Fix: key ack ids by (session epoch, fcnt), or clear CH_CONTROL as well. Effort: S.

**HB16 — Format-string undefined behaviour and log noise.**
- `handle_register_` prints `uint64_t mac_addr` and `address_uint64_` with `%d` (`:842-843`), on every REGISTER and for every listener.
- `admit_frame_` logs 3 INFO lines per uplink per listener with `%d` for `uint32_t` (`:999-1001`).
- `send()` uses `%d` for `size_t` (`lora_tracker.cpp:966`, `:1041`); `serviceTxQueue` does the same (`:441`).
- `set_response` logs `"Could not read protobuf"` (`:2193`) for a v1 decode failure.
- Fix: use `PRIu64` / `%zu` and demote these to DEBUG. Effort: S.

**HB17 — The tracker's `send()` truncates oversized frames instead of refusing them.** `lora_tracker.cpp:964-979` sends a corrupt 255-byte prefix. Fix: return false. Effort: S.

**HB18 — Dead code.** Effort: S.
- `automation.h` `SleepTrigger`, which inherits a whole `LORAListener`, and the `Automation` class.
- `loracover_base.h`: the AM43 / Tuya BLE `LoraCovEncoder` / `LoraCovDecoder`, UUIDs and command bytes. They are still instantiated (`lora_cover.cpp:39-40`, `lora_sensor.cpp:20-21`).
- `LORAClient::app_id` and `LORATracker::app_id_` are written but never read.
- `hubwire::kHubAddr` is unused and duplicates `kHubAddress`.
- MacSap.h service primitives with zero hub uses: `Mac0DataReq/Ind`, `SecDataReq/Ind`, `TxOpts`, `SecRxStatus`, `mac::Freshness`.
- `LORATracker::sendPacketOnce`, `msUntilNextT0`, `msUntilNextClearT0`, the two-argument `nextClearT0ForSlotUs`, and `placedBusyUntilUs`, if unused by tests.
- Commented-out code blocks: `lora_client.cpp:755-823`; `lora_cover.cpp:44-59, 154-187, 194-195`; `lora_tracker.cpp:106-116, 137-140, 231-272, 954-958, 1051-1052`; `lora_tracker.h:42-48, 164-176, 654-658`; `lora_client.h:317-318, 1321, 1360, 1414-1415`.
- `LoraCover::update()` and `loop()` are empty.

**HB19 — Stale, contradictory or history-only comments.** Effort: M.
- **Contradictory:** `fill_grid_sync_` has two paragraphs about `txround`. One says *"stays 0 … Pre-existing, unchanged"* and the other says *"It was hardcoded 0 …"* (`:2363-2380`).
- **References to code that no longer exists:**
  - `retransmit_tracked_op_()` (`:1362`), `tx_tracked_op_()` (`:3781`), `op_frame_` (4×), `s_base_nonce_map` (5×), `k_enc_key_id_` / `installed_session_id_` (5×), `LoraHeader` (6×), `kMaxMissedMarks`, `schedule_op_retry_`.
  - `"captured in admit_frame_"` (`:5546`; `lora_client.h:1071` era): it is now `commit_rx_msgid_`.
  - `handle_register_`'s banner says *"TAKES OWNERSHIP … child nodes re-parse those bytes"* (`:825-829`). S6 removed that.
  - `mac_mgmt.h:8-10` mentions `beaconMac` and *"until the wire stage, S8"*.
  - `mac_mgmt.cpp:31-32` says `LORAListener::setup()` calls `psa_crypto_init()`.
  - MacSap.h (vendored, owned by the node) says *"until stage S8 … LoraHeader"* (`:33-34`, `:50-51`, `:59-60`).
- **Inconsistent numbers:** the downlink preamble-to-T0 is *"4160 us"* in `lora_tracker.h:109` and *"3136 us"* in `lora_client.cpp:5573`.
- **Orphaned or misplaced blocks in the header:**
  - `lora_client.h:1155-1166` describes a removed plaintext duplicate-suppression member.
  - `:595-604` has a *"B1 grid-aligned downlink DEFAULT OFF"* heading sitting over `send_grid_sync`.
  - `:904-911` has a drift-test comment above `send_aligned_`.
  - `:946-960` is the login-slot essay attached to `last_beacon_epoch_`.
- **Fix:** delete the history narration (git has it), keep the one-line "why", and fix the contradictions.

**HB20 — Leftovers in `loracover`.** `on_app_indication` writes `this->position` without applying `invert_position_` (`lora_cover.cpp:150`), while `control()` does invert. `busy_`, `target_position_` and `current_operation` are never maintained, because the loop body is commented out. The stop result is ignored (`:80-82`). Float equality is used for "already at target" (`:88`), and the test-trap note in CLAUDE.md records that silent no-op. Effort: S.

**HB21 — Weak points in the test infrastructure.** If the node repo is missing, all cross-repo drift gates are skipped with only a WARNING and the suite still passes (`tests/proto_sim/CMakeLists.txt:62-68`). Each gate is also `if(EXISTS …)` per header (`:541-553`), so renaming a header silently removes its gate. The CI workflow is untracked (`git status: ?? ../.github/`). Fix: fail when `BLINDS_ESP_ROOT` is absent in CI, assert the header list exists, and commit the workflow. Effort: S.

**HB22 — Unchecked returns and lifetimes.**
- `esp_timer_create` results are ignored (`:4757`, `:4896`).
- The listener destructor never stops or deletes its three esp_timers, and the callbacks hold `this` (a host-test hazard).
- `LinkSecurity::initAuthKey` is lazily called from the tracker task (`copySealerV1` → `authTagV1_`, `HubLinkSecurity.cpp:435`) with an unsynchronised check-then-set (`:212-233`).
- `persist_hook_` always returns true (`:4054-4059`), so the Sequencer's "fail closed" persist (`Sequencer.h:118-133`) is not real. In addition, with `preferences: flash_write_interval: 30min`, a "persisted" id may not be on flash for 30 minutes. That is harmless here because every session restarts at a fresh LOGIN, but the comments claiming persist-before-use mislead.
- Effort: S.

**HB23 — Every listener decodes every uplink.** `checkReception` (`lora_tracker.cpp:329-332`) hands the same buffer to every client. Each one runs `macwire::decode`, allocates `hubwire::Uplink` and runs the protobuf unpack, then `admit_frame_` discards it on address. At 32 nodes that is 32 decodes and allocations per frame on the loop task, plus the INFO logs from HB16. Fix: demultiplex by `WireHdr.src` in MAC-0 (a map from address to listener; REGISTER goes to a matching-MAC lookup) and decode once. Effort: M.

### Nit

- **HB24 — Duplicated constants:**
  - The hub address appears as `kHubAddress`, `hubwire::kHubAddr` and `MacWireVectors kHubAddr`.
  - The FCNT limit appears as `framecrypto::kMsgidCtrLimit`, `macwire::kFcntLimit` and `mac1` `kFcntLimit`.
  - `LOGIN_STAGGER_MS = 3000` is defined 3 times (`:213`, `:622`, `:3669`).
  - `NODE_BOOT_MARGIN_MS` is defined twice, and the literal `5000` boot margin appears at `:218` and `:630`.
  - `kRegisterRateLimitUs` duplicates the node's constant.
- **HB25 — Global macros in headers:** `POOL_SIZE`, `BUFFER_SIZE`, `UART_NUM`, `RX_QUEUE_SIZE` and `MAGIC_NUMBER` are defined at global scope in `lora_tracker.h:36-40`, and the `CONFIG_*_GPIO` pin macros in `lora_tracker.cpp:11-15`. `rx_buffer_t` is the TX buffer type. `buf_[255]` sits next to `BUFFER_SIZE` 256. The static pool and queues are file-scope globals plus `global_lora_tracker`.
- **HB26 — Inconsistent conventions:**
  - Some log lines use the literal tag `"LORAListener"` instead of `TAG` (`:3594`, `:3610`, `:3777`, `:3787`).
  - Closing-namespace comments are wrong: `// namespace am43`, `LoraCov`, `esp32_ble_tracker`.
  - Naming mixes camelCase (`incrTxMessageId`, `hubBeliefNow_`, `txPolicyNow`) with snake_case.
  - `handle_beacon_` (`:5078`) and two `ESP_LOGI` lines (`:5257`, `:5270`) are not indented.
  - `unpack_uplink_` is a one-line wrapper for `v1_receive_`, and `uplink_tag_verdict_` wraps `v1_verify_uplink_`. Both are leftover v0/v1 indirections.
  - `mac_frame_(op, msgid, encrypt)` just forwards to `v1_frame_`.
- **HB27 — Docs disagree with the code:**
  - CLAUDE.md lists a hub `Mac0Access` layer. No such class exists on the hub; the plan doc shows it as an open node-only item.
  - CLAUDE.md says the key lock is taken only in `sealFrameV1` / `macUnderKeyLock_`, but every accessor of the store also takes it. That is fine, and the header states it correctly.
  - CLAUDE.md's "Current state" section is about 300 lines of dated history before any architecture description.

---

## Split proposals along layer lines

1. **Bench streams.** Move `BenchStreams` (ModeTest, drift, MAC ping, `MacStats`, the ModeTest summary) out of `LORAListener`. That is about 900 lines of `.cpp`, 60 members and 3 esp_timers. It would own its timer task and get FCNTs only through a session `reserve(n)` API, which also fixes HB1 and HB2.
2. **Delivery service.** Make the delivery code a `DeliveryService` class owning `slots_`, `ctl_queue_`, timers and `AckIds`, behind an `IDeliveryHost` interface (frame, place, `relogin_pending`). That moves `deliver_` / `dlv_*` / `ctl_*` (about 700 lines) out and makes `DeliverySlot` private.
3. **Session.** Make `HubSession` own the MAC-1 and MAC-2 lifecycle: `send_login`, `confirm_session_`, `handle_register_`, the `uplink_*` pipeline, `seq_`, the nonce state and the onboarding gate. That is about 1000 lines.
4. **Per-node link timing.** Make `NodeLinkTiming` own Mode B/C placement and belief: `send_aligned_`, `nextPlacementT0_`, `send_into_class_a_window_`, `belief_`, `notePhaseReport_`, `noteUplinkPlacement_`, `hubBeliefNow_`, `ack_latency_`.
5. **What stays.** `LORAListener` becomes the ESPHome component facade plus the APP: schedule, sleep model and sensors, auto mode, beacon-to-APP fields, and the `ModeBSupervisor` adapter.
6. **Tracker.** Split `LORATracker` into `TxPool` (the buffer pool), `TxScheduler` (`serviceTxQueue`, reservations, supersession), `BurstEngine` (`sendPacketBurst` broken into `waitCopyInstant` / `stampCopy` / `sealCopy` / `fireCopy` / `rxGap`, which brings CCN 33 down to 8 or less) and `HubGrid` (anchor, rounds, beacon timing).
7. **Header.** Strip `lora_client.h`'s essays into the `.cpp` or docs. The target is a header readable in one screenful per class.

## What is good

- **The MAC-2 unit is genuinely well done:**
  - PSA key ids never leave `HubLinkSecurity`, and the key id and epoch are read *and used* under `keys_mutex_`.
  - No logging or heap inside the lock, and the retag input is built on the stack.
  - Candidate session keys are verified before they are installed and destroyed on every failing exit.
  - CMAC is verified before CTR decrypt, and the FCNT space check runs only after authentication.
  - The `FrameSeal` key-epoch binding drops stale burst copies (F4).
- **Policy is kept pure and host-tested.** Decisions live in dependency-free headers — `DeliveryPolicy`, `ModeBSupervisor`, `SleepModel`, `TimedModePolicy`, `Sequencer`, `SingleShotAckWait`, `ClassAWindows` — with about 1660 proto_sim tests, golden frames, shared wire vectors (`MacWireVectors.h`) and mutation-checked characterisation tests.
- **The wire layer is clean.** `WireBurstFrame` stamps each copy with byte writes and one CMAC. `HubWire` is small and clear, with a route table and a single uplink view type. `PortTable` is the one demultiplexing table.
- **The delivery model is sound.** Retries resend the same packed bytes, so the node re-acks from its cache instead of executing twice. Supersession happens at the head of the queue. OTA is at-most-once. SLEEP is tracked and its effects are applied on the ack.
- **Security fixes are in place.** Plaintext is refused after a session confirms. REGISTER carries a K_AUTH tag and a rate limit. The startup GridDemote is exempt from the plaintext gate but can only make a node listen more.
- **The ratcheting layer lint is a good idea,** and its current baseline values are honest (with the gaps listed in HB11).

## Readability verdict

**Hard to follow; a new engineer cannot get oriented from the code.** The algorithms in the pure headers are readable. The integration layer is not:

- **One 6171-line `.cpp` and a 1894-line header form a single class.** About half the header is prose, so you cannot read the class's shape from its declaration.
- **Comments narrate history.** Dated measurements, review-finding numbers (F3, H2, U-5, §4.6, B1/B3/C2/D4…) and descriptions of earlier versions bury the current rule. Several of these comments are now wrong (HB19), which is worse than no comment.
- **Names are coded and inconsistent.** Plan section numbers stand in for concepts (`B1`, `P2b`, `U-2`, `D1`), naming conventions are mixed, and leftover v0/v1 indirection wrappers remain.
- **The threading model is not written down anywhere.** It is the source of HB1 and HB2: which task owns which member is stated in scattered comments, some of them false.

**What a new engineer should read first:**
1. `docs/mac-wire-format.md`.
2. `MacSap.h` and `HubFrames.h`.
3. `HubLinkSecurity.h`.
4. `DeliveryPolicy.h`.
5. In `lora_client.cpp`, read only `set_response` → `handle_encrypted_` → `deliver_uplink_`, and `deliver_` → `dlv_tx_` → `place_ctl_` → `send_aligned_` → `mac0_send_`.
6. In `lora_tracker.cpp`, read `send` → `serviceTxQueue` → `sendPacketBurst`.

There is currently no single page that says this. A short architecture-and-threading page (which task owns what, and which entry points exist) would do more for onboarding than the roughly 3000 lines of in-code commentary.

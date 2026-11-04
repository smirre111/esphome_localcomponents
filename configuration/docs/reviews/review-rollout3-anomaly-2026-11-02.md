# Rollout-3 anomaly review (hub 7e14954 / 21b7b80 / c9c07ed, node 87b3161 fw 1.1.56)

Read-only analysis, 2026-11-02. No repo file was edited. Scripts and the scratch build are under
`scratchpad/ana/` and `scratchpad/snap/`. Hub line numbers are from 7e14954.

## Verdict

**Neither anomaly was caused by 7e14954, 21b7b80 or c9c07ed.**
- **(1) The slow logins** happen because a Mode B node rarely hears an unplaced 17-copy burst. A LOGIN is such a burst. A hub reboot leaves the node listening on the old grid, so each LOGIN reaches it about 1 time in 3. The retry backoff of 5 s × 2^n then turns 5–6 misses into about 5 minutes. This was already true under 41d3990. Round 2 was quick by luck, with only 4 logins in the sample.
- **(2) The "same-ms clumps"** are a logging artifact. They are not simultaneous transmissions.

**7e14954 does contain one real defect (F3).** Several unplaced bursts that arrive while a listen window is open all get the same earliest-send time. The second one then leaves about 1.9 s late. Because it is now handled as a placed burst, all its overdue copies go out back to back, with no 88 ms spacing. Two failing host tests confirm this. It did not affect the login bursts in these logs, which were never deferred.

**Recommendation:** fix F3 before pushing 7e14954. The fix is small (see F3). 21b7b80 and c9c07ed are safe to push as they are. F1, F6 and F7 are older problems that these logs exposed. F6 (stale sleep record) is a real hub bug and can leave a node stuck for hours. It should be fixed next, but it does not block this push.

---

## A. Measurements

### A1. How clumping was measured

The measure: take consecutive `Packet sent` timestamps, compute the interval between each pair, and count the share of intervals that are 0 ms (same millisecond) or at most 2 ms. Script: `ana/clump.py`. Each window was measured on both logs:
- `hub.log`: the serial capture, timestamped by the host;
- `api.log`: the ESPHome native API stream.

| log | window | intervals | same-ms | ≤2 ms | 80–96 ms bin |
|---|---|---|---|---|---|
| r3b hub.log (serial) | all after 16:04 | 9289 | 24.6 % | **28.9 %** | **0** |
| r2 hub.log (serial) | whole file | 6542 | 16.5 % | 19.3 % | **0** |
| r3b hub.log | production 16:04–16:47 | 2606 | 37.0 % | 44.2 % | 0 |
| r2 hub.log | production 22:03–22:18 | 741 | 36.7 % | 44.7 % | 0 |
| r3b hub.log | campaign 17:07–17:59 | 4128 | 6.3 % | 7.3 % | 0 |
| r2 hub.log | campaign 22:20–23:12 | 4263 | 7.6 % | 8.7 % | 0 |
| r3 (155622) hub.log, **old hub 41d3990** | node 1 OTA | 238 | 48.7 % | 53.4 % | 0 |
| r3b api.log | production 16:04–16:47 | 2483 | 1.9 % | 8.4 % | 628 |
| r2 api.log | production 22:03–22:18 | 706 | 0.0 % | 0.4 % | 238 |
| r3b api.log | campaign | 4125 | 0.3 % | 2.1 % | 98 |
| r2 api.log | campaign | 4263 | 0.0 % | 0.3 % | 192 |

- The 28.7 % figure matches the serial log, at ≤2 ms after the flash (28.9 %).
- The 2.3 % figure could not be reproduced with the same method on any round-2 log. It was probably taken from a different log source.
- **Measured the same way on the same source, the serial log clumps just as much under the old hub.** Comparable windows: 37.0 % vs 36.7 % (production) and 6.3 % vs 7.6 % (campaign). The old hub 41d3990 shows 48.7 % during the 15:58 OTA.
- The serial log never contains an interval in the 80–96 ms bin, in either round. The host reader stamps in chunks of roughly 200 ms, so its timestamps cannot show inter-copy spacing at all.

### A2. Clumps are a logging artifact, not simultaneous transmission

**1. A same-millisecond pair is physically impossible.**
- `Packet sent` is logged in `firePacket()` after `lora_waitTxDone()` (lora_tracker.cpp ~1455–1470).
- Two TxDone events are therefore at least one airtime apart.

**2. The API timestamps come from the main loop.**
- The send task is not the main task. ESPHome puts its log lines in a ring buffer, which the main loop drains (`.esphome/build/loradevices/src/esphome/components/logger/logger.cpp:84-86, 194-204`).
- A same-ms group in api.log therefore only means the main loop did not run for at least 88 ms.
- Example burst: intervals `[1, 258, 6, 0, 92, …]`. Three copies were flushed together after a 258 ms stall, which equals 3 × 88 ms.

**3. On-air spacing is unchanged.**
- 17-copy bursts span 1413 ms (p50) in r3b against 1423 ms in r2. Both match 16 × 88 = 1408 ms.
- The 6–60 ms bin, where genuinely bunched copies would appear, is the same in both rounds: 8.4 % vs 8.8 %.
- `stamp misses` and `placed late` grow at similar rates in both rounds.

**4. What did increase is a small amount of main-loop latency** in the production window: API same-ms went from 0 % to 1.9 %. It lines up with more REGISTER/LOGIN work in this round. `lora_tracker took a long time (155–186 ms)` appears at each REGISTER/login, and the same warning appears at 92–103 ms in r2. Round 3 had more hub restarts and REGISTER flows. This is not radio behaviour.

**5. Neither the 21b7b80 gate poll (500 ms) nor deferred bursts can explain it.**
- The login bursts in question left immediately. For example: `LoginMsg sent 16:14:10.104` → first `Packet sent 16:14:10.109`, `16:13:48.106` → `.106`.
- No listen window was live at those moments.

### A3. Login retry timeline (same parser on both rounds)

| round | node | trigger | first LoginMsg | acked | sends | duration |
|---|---|---|---|---|---|---|
| r2 41d3990 | n1 | hub flash | 22:04:05.08 | 22:04:31.49 | 2 | 26 s |
| r2 | n2 | hub flash | 22:04:10.20 | 22:04:18.80 | 2 | 9 s |
| r2 | n1 / n2 | bench OTA 22:18 | 22:18:56 / 22:19:18 | 22:19:05 / 22:19:26 | 2 / 2 | 9 s / 8 s |
| r3b 7e14954 | n1 | hub flash (hub up 16:03:19) | 16:04:21.86 | 16:09:58.49 | 7 | **5 min 37 s** |
| r3b | n2 | hub flash | 16:04:37.51 | 16:04:39.30 | 1 | 2 s |
| r3b | n2 | `Command failed after retries` 16:13:29 | 16:13:29.43 | 16:18:57.27 | 7 | **5 min 28 s** |
| r3b | n2 / n1 | hub restarts 16:35, 16:39 | — | — | 1–2 | 2–18 s |

Retry instants follow `5000 << n` plus the transmit drain: +6.8, 11.9, 22.0, 41.7, 81.9 and 162 s.

### A4. How often a Mode B node hears an unplaced burst

Script: `ana/catch.py`.
- **Method:** take every hub burst with at least 14 copies while node 2 shows Mode B mark windows. Count it as heard if node 2's serial log has an `LoraInterface: Received` inside the burst span.
- **r2: 13 of 37 bursts heard (35 %). r3b: 26 of 120 (22 %).** The difference is within noise; both are about 1/3.
- **Geometry check.** The window is 29.44 ms. The preamble is 12 symbols at 256 µs plus sync, and 5 symbols are needed for detection. A copy can therefore only be caught if its preamble starts in a span of about 31 ms out of every 88 ms (about 35 %). That matches the measurement.
- At p ≈ 0.3, 5–6 misses in a row happen with 12–17 % probability per login. Two such streaks in one session are unlucky but plausible. Round 2's 4 logins at attempt 2 had about a 9 % chance.
- **Node 2 serial confirms it.** From 16:13:30 to 16:18:55, all 217 mark windows closed empty, and the node never had an `RxDone`. It was transmitting position uplinks the whole time, and the hub dropped them as `No session keys derived yet`. The LOGIN finally landed in a mark window at 16:18:56.066, and the node then logged "Hub reports it rebooted — demoting to Mode A".

---

## B. Findings, ranked by likelihood of causing (1) and (2)

### F1. Cause of (1): LOGIN is an unplaced burst, and a Mode B node hears it only ~1/3 of the time (high likelihood; existed before)

**Code.**
- `LORAListener::session_send` uses `mac0_send_(buf, len, TxPolicy{}, seal)` (lora_client_session.cpp:652-654). That is copies 0 → 17, never placed, never aimed at a Class A window.
- Backoff: `kLoginRetryBaseMs = 5000` (HubSession.h:232), doubling in `schedule_login_retry_` (lora_client_session.cpp:295).
- After a hub reboot the node keeps its old-grid Mode B windows. It only demotes when a LOGIN reaches it (`hub_rebooted`).

**Supporting evidence.**
- Node 2 kept sending uplinks every ~5 s, each followed by RX1/RX2 windows. The hub dropped every one at lora_client_session.cpp:1111 ("No session keys derived yet").
- Node 1 did the same at 16:07:57.

**Fix (separate change, not a blocker).**
- When an uplink arrives from a peer whose login is pending (the keyless drop, or "No base nonce"), re-send the pending LoginMsg into that uplink's Class A RX1 window. `send_into_class_a_window_` already exists. Rate-limit it, for example once per 5 s per node.
- Alternatively, place the LOGIN on the node's last known mark when the hub still has a phase belief.
- Either turns a ~1/3 chance into roughly certain delivery.

### F2. Node-side: a Mode B node misses even placed frames while its motor runs (high likelihood for the 16:13 failure; existed before; node issue)

- **r3b 16:13:00–16:13:29:** the single shot plus 4 placed 17-copy retries for op 105 were all missed. Node 2's mark windows closed empty at the right times (for example window 16:13:06.576 against `placing ... 1066 ms out` at 16:13:05.424). Duty was `light sleep 0.2%` (motor running).
- **r2 22:11:19–22:11:44:** the same pattern. The single shot and retries 1–3 were missed; retry 4 was heard.
- **Possible lead:** with the motor on, the node's log timestamps advance 1530 ms per 1500 µs-of-T0 round, against exactly 1500 ms when it sleeps. Node clock and esp tick disagree by 2 % without light sleep.
- **Consequence:** a command sent while the blind moves exhausts its retries, which causes `Command failed → relogin`. The relogin then hits F1.
- Hand to the node agent.

### F3. 7e14954 defect: bursts deferred past one listen window share a start, and the late one goes out back to back (real; not the cause here)

**Code path.**
- `deferPastListen_` (lora_tracker.cpp:847-861) sets `earliest_us = after_listen(...)`.
- `after_listen` (tx_scheduler.h:183) looks only at listen windows. It ignores frames already queued or placed. So every unplaced burst handed over during the window gets the same `earliest_us` (the window's end), and `reserve_placed` (lora_tracker.cpp:949) reserves the same span twice.
- The queue sends the first at the window's end. It sends the second only when the first ends (~1.9 s later).
- Because `tx_earliest_us > 0`, the second goes down the placed path. `waitCopyInstant_` (lora_tracker.cpp:1067) computes copy k at `earliest + k*88 ms`, all of which are already past. There is no wait, `rxGap_` is not paced, and each copy fires at `now + kStampLeadUs`.
- Its real span is also never reserved, because `begin_burst(..., placed=true)` at line 442 skips that.

**Effects.**
- 17 copies go out back to back (about prepare + airtime apart).
- This breaks the Mode A 3-window coverage geometry.
- It also breaks the node's `burstIndex × 88 ms` arithmetic, so the node computes the wrong burst end and copy-0 instant (reply-defer timing and phase samples).

**Confirmed with host tests** (scratch snapshot of hub 7e14954 and node 87b3161, WSL build; both fail as predicted):
- `ReviewListen.TwoBurstsDeferredPastOneWindowGetDistinctStarts`: `placedBusyUntilUs` does not move for the second burst.
- `ReviewListen.ASecondDeferredBurstKeepsItsEightyEightMsSpacing`: B is released 1 824 448 µs late, and the minimum copy gap is 0 µs in the harness (no air time).
- Full suite on the snapshot: 1792 pass, plus these 2 failures, plus `layer_lint_hub`, which fails only because `tools/` was not in the archive.

**When it bites:**
- any time two or more unplaced bursts are queued during one window;
- typically an onboarding (TimeSync, GridSync, Schedule, ClientConfig, CoverConfig) or a LOGIN/REGISTER burst while another node's single-shot ack is pending;
- the nightly sleep sequence, where single-shot SLEEPs create windows one after another.

**Fix (small):**
- (a) In `deferPastListen_`, floor the start at the queue's drain and placed horizon: `start = max(after_listen(...), queued_air_end_us_, placed_busy_until_us_)`, then re-check against the windows. Or iterate `after_listen` and reservations until both are clear.
- (b) Defence in depth for every late placed frame (this also existed before; r2 shows `placed late` up to 1.8 s): in `sendPacketBurst`, when `not_before_us` is more than `kGuardUs` in the past and the frame is not on-mark, re-base copy instants from the actual copy-0 departure. Copy k then leaves at copy 0 + k × stride and keeps the 88 ms geometry.
- The two tests above (sketch in `ana/listen_char_test.cpp`) become the regression tests.

### F4. Listen-window leak or blocking of logins (ruled out)

- A window lives at most `kAckListenMs = 3 s` after the mark: `w.end_us > now_us` (tx_scheduler.h:183-200, `overlaps_locked_`).
- It is released on ack, give-up, abandon, and before the node's own retry (lora_client_delivery.cpp:66, 140, 159, 174).
- A window is reserved only for a single placed tracked command (lora_client_delivery.cpp:83-89). No such command existed during node 1's 16:04–16:10 logins.
- During node 2's 16:13–16:18 logins, every login burst left within 0–250 ms of `LoginMsg sent`.
- The 8-slot table evicts the window that ends first. That is harmless: expired windows sort first.
- Minor: `release_listen(0)` clears every free slot. That is harmless.

### F5. 21b7b80 sleep gate (no effect on (1) or (2); one minor gap)

- The gate is separate from the onboarding gate and polls only while a SLEEP is waiting.
- Minor gap: a SLEEP that did not go on the air releases the gate immediately (`try_start_sleep_`, lora_client.cpp ~805). When it is later sent (after the relogin or the queued command), it goes out without holding the gate. That reverts to pre-21b7b80 behaviour in that corner case only.
- The stagger test at 16:52:43 worked as designed: node 2 waited, node 1 was acked at 16:52:46, node 2 at 16:52:49.

### F6. c9c07ed (no effect on login)

- `note_authentic_uplink` only acts on authenticated uplinks in a confirmed session.
- Side note: after a REGISTER, node 2 received two distinct TimeSyncs within 4 s (18:03:15 / 18:03:18; login path plus reset path). This is cosmetic.
- The repeated `Node reset (DEEPSLEEP, resumed wake) — advancing tx_message_id by 16` on every MODE_CHANGED beacon also appears in r2, as POWERON. It is not new.

---

## C. Host tests

- Built proto_sim in WSL against a `git archive` of hub 7e14954 and node 87b3161, with `-DBLINDS_ESP_ROOT=<snapshot>`. Build dir: `~/review_r3_build`.
- Added the two characterisation tests to the snapshot only: `ana/listen_char_test.cpp`, appended to `scenarios/real_lora_tracker_test.cpp` with CRLF preserved.
- Results are as listed under F3.
- I did not run the same comparison on 41d3990: it has no listen windows, so the scenario does not exist there. The behaviour that makes late placed bursts bunch is identical in both trees (`waitCopyInstant_`).
- The harness has no air time and does not advance the clock in `vTaskDelay`, so it cannot model the over-the-air catch rate (F1). The log statistic in A4 is the evidence for that.

---

## D1. ClientConfig / CoverConfig: lost with no retry, and the hash makes the loss permanent

**What happened (18:03:10):**
- After the production restore, node 2 (Mode B) was sent ClientConfig and CoverConfig.
- It received only one 51-byte `CMD CONFIG`. It saved `sleepDuration 600` unchanged, so the ClientConfig carrying 21600 was lost.
- The hub nevertheless set `config_synced_ = true` (lora_client_session.cpp:199).

**Root causes:**
1. `kConfigPushPolicy = {Mode::Fire, Shape::Bare, ...}` (DeliveryPolicy.h:253) is an unplaced burst. For a Mode B node that is the ~1/3 lottery from A4.
2. The route table has `ack_req=false` for `DL_CLIENTCONFIG` and `DL_COVERCONFIG` (HubWire.cpp:29-30), so a loss is never noticed.
3. **Separate bug that turns one loss into a permanent desync:** `save_state_` writes `restore.config_hash = currentConfigHash_()` on every NVS save (lora_client.cpp:237), whether or not the config was delivered. Any FCNT persist after a hash change therefore stores the new hash. The next hub boot then reads "Config unchanged" (lora_client.cpp:90-92) and never pushes again. That is why the fix-up needed a deliberately different intermediate hub build.

**Minimal reliable fix (hub-only):**
- **(a)** Persist the hash of the last config the node **acknowledged**, not the current one. Keep a `synced_config_hash_` member: set it on ack, save it in `save_state_`, compare against it at restore.
- **(b)** Make the encrypted pushes Acked:
  - `ack_req=true` for `DL_CLIENTCONFIG` and `DL_COVERCONFIG` in HubWire.cpp. The node follows the header's ACK_REQ (MacReceiver.cpp:180-187); it only logs a warning until its PortMap.h table is updated too.
  - A new policy `kConfigAckedPolicy = {Mode::Acked, Shape::Auto, Resend::SameBytes, 3, 5000, FailKeep, ...}` on the Config channel. Shape::Auto places the frame on the node's mark, or sends a single shot.
  - Set `config_synced_` (and the persisted hash) only when **both** are acked. ClientConfig and CoverConfig come from different components, so collect two ack ids, the way ScheduleConfig already uses an ack-id set.
- **(c)** Keep the plaintext bootstrap push for an unprovisioned node as Fire. Do not mark it synced. Re-send it encrypted and acked at that node's first confirmed session.

**Risks:**
- Two more acked ladders share the Config channel with ScheduleConfig. Supersede rules need care: config pushes must not be superseded by schedule pushes.
- More airtime on loss. That is bounded: 3 retries.
- A node without the PortMap update logs one warning per frame.
- **Alternative (verify by beacon):** add a `configHash` field to `NodeWakeBeacon`, where the node hashes the fields it applied, and reconcile it like `schedVersion`. This self-heals across every loss mode, but it needs a proto change and node work. Recommended as a follow-up, not as the minimal change.

---

## D2. Stale sleep record after a hub restart — a hub restore bug

**Mechanism:**
- `last_sleep_epoch_` is persisted (lora_client.cpp:236, applied at lora_client.h:1644). Nothing ever clears it once the node is known to be awake again.
- `last_heard_epoch_` exists only in RAM (lora_client.h:1271), so it is 0 after every hub boot.
- `next_wake_epoch_` predicts `last_sleep + sleep_duration_` using the hub's **current** config (lora_client.cpp:290). After the bench sleep at 16:52 (600 s), the production build (21600 s) predicted a wake at 22:52.

**Observed:**
- At 18:00:59 and 18:33:02 the hub logged `Login retry deferred 17508 s / 15587 s — node asleep until then` (lora_client_session.cpp:313).
- At 18:33 node 2's single LOGIN missed (F1). Its uplinks were dropped before authentication (lora_client_session.cpp:1111), and that path never calls `note_node_heard_` (lora_client.cpp:363). The node stayed stuck until the RTS reset at 18:54.

**Answer: yes, it is a restore bug, and a heard uplink should end it.**
- **Fix 1:** consume the record. Once the node is heard or confirmed after `last_sleep_epoch_` (first authenticated uplink, REGISTER, or login ack), zero `last_sleep_epoch_` and persist it. Alternatively persist `predicted_wake_epoch` computed with the duration actually commanded, rather than recomputing from the current YAML.
- **Fix 2:** a keyless or stale-key encrypted uplink from a provisioned peer whose login is pending (the line-1111 drop) should cancel the stretched `login_retry` and re-send the LOGIN at once, rate-limited and ideally into that uplink's RX1 window (as in F1).
  - Do **not** mark the node "heard" for the Asleep sensor on an unauthenticated frame.
  - Security cost: an attacker could trigger LOGIN bursts. That is airtime only, and it is bounded by the rate limit; REGISTER replay is already limited the same way.
- **Fix 3 (cheap guard):** never let the sleep stretch exceed `sleep_duration` as last commanded to the node. Cap it at, for example, the configured duration plus margin from `last_sleep_epoch_`, and never beyond a value the node was actually given.

---

## Pre-push checklist

- **7e14954:** fix F3 (a) and add the two ReviewListen tests. (b) is recommended at the same time. 21b7b80 and c9c07ed can be pushed as they are.
- **Next:** D2 fixes 1 and 2 (hub-only), D1 (a) (one line of intent), then D1 (b).
- **F1:** login into the Class A window after an uplink. This is the real fix for "Mode B node takes minutes to log in after a hub reboot".
- **F2:** hand to the node agent (motor-run deafness in Mode B, both rounds).

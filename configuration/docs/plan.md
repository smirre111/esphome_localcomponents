# LoRa Blinds — Fix / Hardening Plan

Prioritised backlog from the 2026-07-12 node stability audit (runaway tasks /
resets / battery anomaly). Node changes need a rebuild (IDF 6.0) + reflash or
HTTPS-OTA; hub changes are an `esphome` OTA. See [activity-log.md](activity-log.md).

Priority: **P1** = can reset/brick the node, do first. **P2** = wrong data or
silent failure. **P3** = latent / cleanup.

---

## P1 — msgid counter desync after a hub reboot drops one-shot commands (OTA)

- **Observed (2026-07-14, node 2):** after the hub OTA/reboot while node 2 stayed
  awake, the OTA trigger was silently dropped.  Node 2 serial showed the OTA
  arriving as a burst but rejected: `Rejected message ID: 5, my MsgID: 5`.  The
  hub↔node2 TX/RX msgid counters had drifted to both ≈5, so the hub's command
  (msgid ≤ node RX) failed the strict-greater replay check on the node.  A single
  successful cover op (msgid 6 > 5) resynced it; node 1 was unaffected (its
  counters happened to stay aligned).
- **Why cover ops survive but OTA doesn't:** cover ops retransmit with a *fresh
  incrementing* msgid until acked (F-4), so a retry eventually exceeds the node
  RX counter AND a full failure now clears the session (Hub-P2).  Sysops
  (`CMD_OTA`, sleep, wifi, config) are sent **once, unacked** — one rejected msgid
  is lost with no recovery.
- **Fix — general (fixes ALL message types): reset both msgid counters to a shared
  base (0) whenever a new session / base nonce is established.**  The AES-GCM IV is
  derived from (base_nonce, msgid); a fresh per-session base_nonce means resetting
  msgid to 0 cannot cause IV reuse, so it is cryptographically safe.  Wire both
  sides to zero their counters at the *same* event — the node when it stores a new
  base nonce (BaseNonceExchange), the hub when it (re)provisions / confirms the
  session — so every session starts aligned and no post-reboot command can be a
  stale-msgid replay.  (Removes the current `apply(): tx_message_id + 64` guess.)
- **Fix — delivery reliability (complementary): route important sysops (at least
  `CMD_OTA`) through the tracked/acked path** (retransmit with fresh msgid until a
  CommandAck, like cover ops), so even a transient miss recovers.
- **Immediate workaround:** send any cover op (or power-cycle the node) to resync,
  then re-trigger the OTA.
- **Effort:** medium (counter-reset is hub + node; touches the login/base-nonce
  path).  Hub OTA + node OTA.

## P2 — Config-sync via a login flag / config-hash (closes 3 gaps at once)

- **Problem:** config is delivered to a node ONLY on a `REGISTER`
  (`lora_client.cpp:695`, gated by `needs_config || !config_synced_`). Nodes
  register on boot/deep-sleep wake, so sleeping nodes get changes on next wake —
  but an **always-awake node re-establishes via LOGIN after a hub reboot, not
  REGISTER, so it never receives config updates** until it reboots. (Observed:
  node 2 only picked up the new `sleep_duration` because a serial-monitor restart
  rebooted it.)
- **Design (idea from 2026-07-15):** have the hub's `LoginMsg` carry a
  **config-sync signal**; the node reacts by sending a `REGISTER`, which flows
  through the existing config-push path. Two variants:
  - simple: a bool (e.g. `request_register`) set when `!config_synced_` (pushes
    once per hub boot);
  - better: a **config version/hash** — hub sends its current config-hash in the
    login, node echoes the hash it last applied; hub requests a register only when
    they differ (pushes only on a *real* change, not every unrelated hub OTA).
- **Fixes three things together:**
  1. **Awake-node config gap** — awake nodes get config updates without a reboot.
  2. **msgid counter desync (P1 above)** — `send_login()` resets frame counters to
     0, so a config-triggered register/login also resyncs counters, mitigating the
     OTA/sysop drop.
  3. **Wake efficiency (was: "wake re-registers instead of login")** — wake can
     become **login-first, register-only-if-config-changed**: a provisioned node
     just logs in (lightweight) and only takes the ~1.5 s config bursts when the
     hub's login says the config actually changed. Currently `main.cpp:472` always
     calls `sendRegister()` on wake, needlessly re-pushing config every wake.
- **Cost:** new `LoginMsg` proto field (regen stubs → 4 locations) + small hub
  (`send_login`) and node (login handler → `sendRegister`) changes. Hub OTA + node
  OTA.

## P1 (ROOT CAUSE, reproduced) — `taskLoraRx` blocks on the radio semaphore → Task WDT reset

- **Captured live (2026-07-14 ~10:33, node 2 serial, after ~41 h uptime):**
  ```
  E task_wdt: Task watchdog got triggered ... did not reset the watchdog in time:
  E task_wdt:  - taskLoraRx (CPU 0)
  E task_wdt: Aborting.
  rst:0xc (SW_CPU_RESET)
  ```
  (The printed backtrace decodes to `esp_timer` — that's the *running* task at the
  ISR, a red herring; the starved task is `taskLoraRx`.)
- **Root cause:** in `loraRxTask` the TX path acquires the radio mutex with **no
  timeout** — `xSemaphoreTake(rx_tx_semaphore_, portMAX_DELAY)`
  (`LoraInterface.cpp:393`). The WDT is fed only at the top of the loop
  (`:331`). If `rx_tx_semaphore_` is never given back — a leaked give, or a
  **missed DIO0/DIO1 RX-done/TX-done IRQ** so the ISR handler never releases it —
  `taskLoraRx` blocks forever, starves the 8 s WDT, and the node resets. Rare,
  load-dependent → looks random; matches the long-standing "node needed a reset".
  Note the RX acquire at `:337` already uses a 700 ms timeout — the TX path at
  `:393` is the inconsistency.
- **Fix:** (a) give `:393` a bounded timeout (like `:337`) and on failure log +
  retry / re-init the radio instead of blocking; (b) audit every path that takes
  `rx_tx_semaphore_` to guarantee it is always given back (ISR RX-done, TX-done,
  RX-timeout, and error paths); (c) consider a radio-watchdog that re-inits the
  SX127x if no IRQ arrives within N ms of starting a TX/RX.
- **Note:** the Task WDT already auto-recovers (reboot + session resume), so this
  is self-healing today — but it's a real periodic reset worth eliminating.
- **Effort:** small–medium. Node OTA.

## P1 — `taskDeepSleep` starves the Task WDT → panic reset under load

- **Symptom:** intermittent unexplained resets; a node that "needed a reset".
- **Root cause:** `taskDeepSleep` calls `esp_task_wdt_add(NULL)`
  (`frtosTasks.cpp:403-409`) then runs `enterDeepSleepTask()`, an infinite loop
  that waits for the queues to drain (`vTaskDelay(3000)` / `+1000; continue`)
  and **never calls `esp_task_wdt_reset()`**. WDT = 8000 ms, `trigger_panic=true`
  (`main.cpp:269-273`). If the queues are not idle within 8 s of a `CMD_SLEEP`,
  the WDT panics → `ESP_RST_TASK_WDT`. Load-dependent, so it looks random; the
  reset is classified `unexpected_reboot`, masking that it's a panic.
- **Fix:** call `esp_task_wdt_reset()` at the top of the `while(1)` in
  `SystemCtrl::enterDeepSleepTask()` (keeps WDT protection against a real hang);
  or don't subscribe `taskDeepSleep` to the WDT at all.
- **Effort:** 1 line. Node OTA.

## P1 — `adc_oneshot_read` wrapped in `ESP_ERROR_CHECK` → panic on transient failure

- **Symptom:** rare crash/reset during a battery measurement.
- **Root cause:** `ESP_ERROR_CHECK(adc_oneshot_read(...))` (`frtosTasks.cpp:362`)
  aborts the whole node if the read returns any error.
- **Fix:** capture the return; on error log + skip this cycle (keep last value),
  do not abort. Same review for other `ESP_ERROR_CHECK` on runtime (non-init)
  calls in the battery/motor tasks.
- **Effort:** small. Node OTA.

## P2 — Battery send transmits stale cache → 0 V transmitted (observed)

- **Symptom (observed 2026-07-12 ~15:05):** two battery messages carrying 0 V.
- **Root cause:** `measureAndSendBatteryVoltage()` (`CmdDispatcher.cpp:714-722`)
  queues an async measure (cmd=1) to `taskBatteryMonitor`, then **immediately**
  `sendBatteryVoltage()` transmits the LKG cache `lastBatteryVoltage_` — before
  the queued measurement runs. On a fresh boot/wake the cache is 0 → 0 V on air.
- **Fix options:** (a) have `taskBatteryMonitor` send after it measures for the
  motor-end trigger too (drop the separate immediate send); or (b) suppress the
  send when `lastBatteryVoltage_ == 0` / not-yet-measured; or (c) do a synchronous
  measure-then-send. Prefer (a): single owner of the ADC + send.
- **Effort:** small–medium. Node OTA.

## P2 — Battery-silence gap 13:39→15:05 (~86 min) — investigate

- **Observation:** no battery messages for ~86 min, then activity + the 0 V pair.
- **Reasoning:** `taskBatteryMonitor` force-sends on a 5-min timeout
  (`frtosTasks.cpp:345-349`), so an **awake** node can never be silent >5 min.
  An 86-min silence ⇒ the node was **deep-asleep**, **reset-looping**, or the
  task **died/blocked**.
- **Diagnostic:** correlate the timestamps with the node reset reason
  (`esp_reset_reason` at boot) and sleep logs on COM6; check the hub log for a
  `Received REGISTER` / `session confirmed` at ~15:05 (a wake/boot marker). Add a
  battery-value pattern to the hub log monitor so future gaps are captured live.
- **Note:** likely coupled to the P1 WDT reset if the silence began with a
  sleep attempt under load.
- **Effort:** investigation.

## P2 — Hub: stale-`session_confirmed_` never self-heals

- **Symptom (observed 2026-07-11 22:27):** node 1 cover ops failed 4× and marked
  failed **without** forcing a re-login, because `session_confirmed_` was still
  true; needed a manual node reset. (Node 2, unconfirmed, recovered automatically.)
- **Root cause:** the auto-relogin in `schedule_op_retry_` only fires
  `if (!session_confirmed_)`.
- **Fix:** on marking a command failed, also clear `session_confirmed_` (and/or
  after N consecutive command failures) so the next op / an immediate re-login
  rebuilds the session even from a stale-confirmed state.
- **Effort:** small. Hub OTA only (no node reflash).

## P3 — Inter-node cross-talk logs a decrypt error (benign)

- **Observed (2026-07-12/13, node 2 serial):** two related RX errors, both from
  node 2 (addr 18) processing frames not addressed to it on the shared channel:
  1. `Missing base nonce for peer 17, cannot decrypt` — irregular, several/hour.
     Peer 17 = node 1's *uplinks*; no shared key → fails at nonce lookup.
  2. `AES-GCM decryption/authentication failed` — **daily at ~23:00** (seen
     2026-07-12 23:00 and 2026-07-13 23:00), coinciding with node 1's nightly
     sleep/wake login handshake. This is the hub's *downlink to node 1*: node 2
     shares the hub key so it gets past the nonce check into GCM decrypt, then
     fails the auth tag (node 1's counter/context, not node 2's).
  Both drop the frame safely; **no functional harm**, node 2 stays healthy.
- **Root cause:** node attempts AEAD decrypt before fully filtering by destination
  address, so a frame not addressed to it reaches the decrypt path.
- **Fix:** drop/ignore frames whose destination isn't this node (addr or subnet)
  *before* attempting decrypt; and/or downgrade this line from `E` to `D`.
- **Effort:** small. Node OTA. Low priority (log noise + tiny CPU waste).

## P3 — Cleanup / latent

- **`taskShutdownWifi`** subscribes to the WDT then `vTaskDelete(NULL)` without
  `esp_task_wdt_delete(NULL)` (`frtosTasks.cpp:411-421`). Currently **dead code**
  (never spawned) — delete it or fix the unsubscribe before anyone wires it up.
- **`checkQueuesIdle()` livelock:** if any queue can stay non-empty forever, the
  node both never sleeps and (pre-P1-fix) WDT-resets every 8 s. Confirm the queues
  reliably drain; consider a max-wait cap in `enterDeepSleepTask` that sleeps
  anyway after a bound.
- **Verify** the AES-GCM TX `out_buf` (`CmdDispatcher.cpp:493`) is freed on every
  transmit branch by its caller (counting looked balanced; confirm by path).

---

## Clean (audited, no action)

- Heap: crypto `malloc`s matched by `free()` on all branches; protobuf
  `__unpack`/`__free_unpacked` balanced per exit path.
- No unbounded containers (only bounded ADC averaging).
- Other WDT-subscribed tasks feed correctly: `LoraInterface` (resets 331/382),
  `MotorCtrl` (reset 362). Idle-task watching disabled (`idle_core_mask=0`), so
  only explicitly-added tasks can trip the WDT — `taskDeepSleep` was the exposure.

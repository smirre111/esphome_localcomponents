# Hub threading model

Status: current as of 2026-10-25 (review fixes HB1, HB2, HB7, HB8). Read this
before you touch anything in `local_components/lora_client` or
`local_components/lora_tracker` that a timer, the send task, or the loop task
can reach.

The hub runs on three task contexts. There is no radio interrupt: the hub
polls the SX1278 from the loop.

| context | what runs there | stack |
|---|---|---|
| **ESPHome loop task** | every `Component::loop()` (`LORATracker::loop` → `receive()` → every listener's `set_response`, `serviceBeacon`, the deferred reports), every `set_timeout` / `set_interval` callback, every HA entity action (cover control, buttons, switches), `setup()` | ESPHome default (8 KB) |
| **esp_timer task** ("Tmr Svc", priority 22) | `mode_test_timer_cb_`, `drift_timer_cb_`, `mac_ping_timer_cb_`: one frame per tick | 3.5 KB (`StackWatch.h`) |
| **tracker send task** (`sendTaskDispatcher`, core 1, priority 1) | `sendTask` → `serviceTxQueue` → `sendPacketBurst` → the per-copy `CopySealer` (`LinkSecurity::copySealerV1`) | 4 KB |

## Who owns what

**Loop task only** (no lock; another task must never touch it):

- the MAC-1 `Sequencer` (`seq_`): allocation and commit. Allocation persists
  through ESPHome preferences (`persist_hook_` → `save_state_` →
  `rtc_.save`), and ESPHome's preference backend is not thread-safe.
  `incrTxMessageId()` is therefore loop-only.
- all of the listener's application, delivery and session state: `slots_`,
  `ctl_queue_`, `belief_`, `last_placed_*`, `session_confirmed_`,
  `relogin_pending_`, the sleep model, the schedule, `uplink_pt_`.
- `send_aligned_` (it writes `last_placed_*` and reads the belief).
- the PSA session-key install/clear (via `SessionKeyStore`, which locks
  internally) and the uplink verify/decrypt.

**esp_timer task only:**

- each stream's last-mark state: `mt_last_mark_t0_us_`,
  `mac_ping_last_mark_t0_us_`. Timer frames are placed with
  `send_placed_from_timer_` (MAC ping) or the ModeTest callback's own
  placement, never with `send_aligned_`.
- the stream's frame buffer while its callback runs (`mt_frame_`,
  `drift_frame_`, `mac_ping_frame_`). The loop touches it only before the
  timer is started and after a stop has waited for the callback (below).

**Send task only:** the copy buffer `copy_buf_`, the burst pacing, the H-1
latency buckets, and the anomaly counters it increments (`tx_late_placed_`,
`tx_mark_late_`, `tx_stamp_misses_`, `tx_seal_drops_`,
`tx_superseded_drops_`; 32-bit, read by the loop for its report).

## Shared state and its lock

| state | written by | read by | protection |
|---|---|---|---|
| FCNT reservations `mac_ping_fcnts_`, `mt_fcnts_`, `drift_fcnts_` (`FcntReservation.h`) | loop (reserve, invalidate) | esp_timer (take) | `LORAListener::session_tx_mutex_` |
| session keys, session id, key epoch, generation (`SessionKeyStore::Keys`) | loop (install, clear, set session id) | send task (retag), esp_timer (seal), loop | `SessionKeyStore::keys_mutex_` |
| tracker channel schedule: `supersede_`, `burst_busy_until_us_`, `placed_busy_until_us_`, `queued_air_end_us_`, `reservations_` | `send()` on loop and esp_timer; `serviceTxQueue` on the send task | both, plus the placement queries | `LORATracker::reservations_mutex_` |
| the radio (SPI + mode) | send task (TX, RX re-arm between copies) | loop (`receive()`) | `LORATracker::radio_mutex_` (FreeRTOS mutex) |
| the TX buffer pool and queues | `send()` (any producer), send task | | FreeRTOS queues + `pool_mutex` |
| stream flags `mode_test_active_`, `drift_test_active_`, `mac_ping_active_`; tracker `lora_tx_busy_` | loop / send task | the other task | `std::atomic<bool>` |
| `mt_cb_busy_`, `drift_cb_busy_`, `mac_ping_cb_busy_` | the callback (RAII `CallbackBusy`) | loop (stop) | `std::atomic<bool>` |

## Rules

1. **The Sequencer is loop-only.** A frame built off the loop takes its FCNT
   from a block the loop reserved (`reserve_fcnt_block_`, one persist per
   run) via `take_reserved_fcnt_`. A block is dropped on every counter
   restart or key change (`send_login`, `clearSessionKeys_`,
   `installSessionKeys_`) and is bound to the key epoch it was reserved
   under; a frame whose seal reports another epoch is dropped
   (`sealed_in_epoch_`). Reason: an FCNT is a CTR counter-block input; one
   sealed twice under one key reuses keystream (review P1).
2. **Nothing logs, allocates, or makes a virtual call under
   `keys_mutex_`.** The seal and the retag read the session id, the key
   epoch and the key and use them inside the one lock, so a CTR block or MAC
   prefix never pairs one session's id with another session's key (HB7). A
   changed session id moves the key epoch.
3. **The per-copy send path neither logs nor allocates.**
   `sendPacketBurst` counts anomalies; `LORATracker::loop` reports them at
   most every 10 s (`drainTxAnomalyReport_`) and drains the H-1 report
   (`drainTagLatencyReport_`). No hex dumps on the send task.
4. **Nothing logs under `reservations_mutex_`.** `session_tx_mutex_` is held
   briefly by the timer (`take_reserved_fcnt_`, `sealed_in_epoch_`, no
   logging) and by the loop across an allocation, whose persist logs at
   DEBUG; the timer never waits on it for long because the loop's critical
   sections are short. `keys_mutex_` is never taken while
   `session_tx_mutex_` is held, and vice versa (the epoch is read before the
   reservation lock is taken), so the mutexes have no ordering cycle.
5. **Stopping a timer stream waits for its callback.** `esp_timer_stop`
   does not wait for a callback that is already running. Each stop sets the
   active flag false, stops the timer, then `wait_callback_idle_` spins until
   the callback's busy flag clears, and only then builds the STOP/OFF frame
   in the shared buffer. The esp_timer task outranks the loop, so the spin is
   short.
6. **A timer callback never calls loop-owned code.** In particular not
   `send_aligned_`, `incrTxMessageId`, `set_timeout`, `save_state_`, or any
   delivery-service entry point. If a timer needs loop work, it must hand it
   over (an atomic flag the loop polls) rather than doing it in place.
7. **Callbacks into ESPHome APIs** (`set_timeout`, preferences, entity
   publish) happen on the loop only.

## Entry points by context

- loop: `LORATracker::loop` / `receive` → `LORAListener::set_response` →
  `admit_frame_` → `handle_encrypted_` / plaintext path → `deliver_uplink_`;
  `deliver_` → `dlv_tx_` → `place_ctl_` → `send_aligned_` → `mac0_send_` →
  `LORATracker::send`; `serviceBeacon`.
- esp_timer: `*_timer_cb_` → `build_*_frame_(…, &block)` /
  `pack_test_frame_` → `mac_frame_(op, fcnt, …)` → `LORATracker::send`.
- send task: `sendTask` → `serviceTxQueue` → `sendPacketBurst` →
  `WireBurstFrame::seal` → `LinkSecurity::copySealerV1` → `retagFrameV1`.

## Known gaps

- `grid_aligned_` and `grid_slot_` are plain fields read by the timer
  placement; they change only on a Timed Mode switch, on the loop.
- `LinkSecurity::initAuthKey` is lazily callable from the send task
  (`copySealerV1` → `authTagV1_`) with an unsynchronised check-then-set; in
  practice `setup()` initialises it first (review HB22).
- The host test build runs every callback synchronously, so the locking and
  the stop wait are reviewed, not exercised, by `tests/proto_sim`.

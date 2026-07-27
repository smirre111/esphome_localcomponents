# LoRa Blinds — Activity Log

Running log of work on the ESPHome LoRa blinds system (hub + battery ESP-IDF
nodes). Newest entries first. See also the repo git history for exact diffs.

## Agenda / open items

> Prioritised fix backlog lives in **[plan.md](plan.md)**. The 2026-07-12
> stability audit items (WDT-starvation resets, battery 0 V race, ADC panic,
> stale-session hardening, config-sync) have shipped; all nodes are on the latest
> firmware.

- [ ] **Slack calibration** — `open_slack`/`close_slack` and travel-only
  `open_duration`/`close_duration` are first estimates; keep tuning against
  observed partial-move accuracy.

## Log

### 2026-07-27 — v1.0.12: configurable battery interval + stale-proto cleanup

- **Battery update interval is now configurable from the hub YAML** (default
  15 min; was hardcoded 5 min on the node). New `ClientConfig.batteryInterval`
  proto field (stubs regenerated via WSL protoc-c to all 5 locations); hub
  `battery_update_interval` option (a time period, e.g. `15min`) pushed via the
  existing config path; node stores it in `config.txt` and `taskBatteryMonitor`
  reads it each cycle (floored at 10 s). `0` = unset → node keeps its default, so
  version-skew is safe both directions. Node → v1.0.12; hub OTA'd
  (`config_hash=0x353a4b75`, banner-confirmed).
- **Removed the stale ESPHome-side proto reference copy**
  `local_components/lora_tracker/proto/blinds.proto` — unused (the hub builds from
  the `blindsproto` stubs; the authoritative proto lives in `BlindsESP/proto`).
- Deploy: hub OTA'd now (skew-safe, so hub-first was fine); nodes pick up v1.0.12
  and the configured interval on their next OTA from `https_hosted`.

### 2026-07-27 — v1.0.11: tracked/acked sysops + cross-talk pre-decrypt filter + deep-sleep cap

- **Node 1 recovery confirmed.** After the node-1 outage (silent ~85 min while the
  hub retried login 4×), node 1 came back on-air (power-cycled): registered,
  config pushed, encrypted session confirmed, strong link (−42 dBm). The transient
  0 V battery on boot was the known stale-cache artifact (already fixed in fw);
  the real reading followed at 10.4 V / 27 %.
- **Plan reconciliation.** Verified the whole backlog against source: most of the
  2026-07-12 audit (P1 WDT/ADC/msgid, P2 battery-0 V / config-sync / hub
  self-heal) was already shipped in v1.0.8–v1.0.9; the plan was stale. Trimmed
  plan.md + this agenda to the genuinely-open items.
- **Three fixes implemented, built green, deployed, validated (v1.0.11 + hub OTA):**
  - **(a) Tracked/acked sysops (P1).** Generalised the cover-op retransmit
    machinery to any command: `tx_cover_operation_` → `tx_tracked_op_` (+ shared
    `begin_tracked_op_`), `triggerOTA()` → `send_tracked_sysop_(CMD_OTA)`. The node
    now `sendCommandAck`s every sysop, so a dropped OTA frame retransmits (fresh
    msgid) until acked instead of being lost. SLEEP intentionally left on its
    wake/login-fallback path (node still acks it, harmlessly).
  - **(b) Cross-talk decrypt (P3).** Node drops foreign encrypted frames by
    destination address *before* GCM decrypt; hub's check already preceded decrypt
    so its `Address not for me` was downgraded `E`→`D`. Validated: 0 error-level
    cross-talk lines in the post-update login round.
  - **(c) Deep-sleep hard cap (P3).** `enterDeepSleepTask` caps the drain-wait at
    the motor's absolute max run-time (`kAbsMaxRunMs`, 120 s), then sleeps anyway.
- **Deploy order = nodes before hub** (only the sysop-ack change is version-skew
  sensitive: new-hub + old-node would retransmit an unacked OTA 4× + force
  re-login; new-node + old-hub is harmless). Nodes flashed v1.0.11 first, then hub
  OTA (`config_hash=0xa101c0c3`). Post-update login round: both nodes registered,
  config-synced (`request_register` 1→0), encrypted sessions confirmed. Restart
  button (`button.restart`) also added to loradevices.yml for one-click reboots.

### 2026-07-15 — Config-sync via login flag (P2) implemented + v1.0.9 P1 validated

- **P1 msgid-desync reorder (v1.0.9)** validated live on node 2: ran 17+ min with
  ZERO cross-talk errors (previously "Missing base nonce peer 17" hit ~every
  15 min). Address filter now runs before the msgid check, so overheard node-1
  frames are dropped before they can pollute the global rx counter.
- **P2 config-sync (simple variant) implemented & deployed:**
  - `LoginMsg.request_register (bool, field 2)` added; stubs regenerated to all
    4 locations. Hub `send_login()` sets it to `!config_synced_`; node `CMD_LOGIN`
    handler sends a `REGISTER` when set (reusing the `SYSCMD_REGISTER` path).
  - Node built v1.0.10 (serial `app-flash` COM6, LittleFS preserved); bin staged to
    `https_hosted/`. Hub compiled `config_hash=0x857efa16` + OTA'd.
  - Validated: flag serializes & is sent (node 1 got `request_register=1` at 18:40);
    a v1.0.9 node ignores it and acks (no regression); normal `request_register=0`
    boot path unchanged (node 2 18:14).
  - **Validated end-to-end on confirmed v1.0.10 (22:33):** node 2 awake (uptime
    ~61 min, no `rst:0x`), hub rebooted (`config_synced_=false`) → hub
    `request_register=1` → node 2 serial `requests re-register — sending REGISTER`
    → hub `Registered` + `Pushing config (synced_this_boot=0)` → follow-up login
    `request_register=0` (converges) → ack. Triggering login came from the
    resume-path guarantee (`Session resumed without config sync — scheduling
    re-login`), so both new pieces proven together. Awake-node config gap closed.
  - **CRITICAL flash finding (fixed):** node 2 had been booting OLD firmware from an
    OTA slot all session — serial `app-flash` writes `factory` (0x20000) but a prior
    HTTPS-OTA had pointed `otadata` at an OTA slot. Every "v1.0.9"/"v1.0.10" flash
    was a silent no-op on the boot slot. Fixed with `esptool erase-region 0x630000
    0x2000` (fall back to factory; LittleFS preserved); boot banner then confirmed
    `offset 0x20000` + `App version: 1.0.10`. An earlier 19:03 "validation" (on old
    fw, and a monitor-restart-induced boot register) was retracted. See plan.md.
  - Node 1 still old fw (installed → HTTPS-OTA only, currently blocked on the
    `:8070` OTA host not being reachable on the LAN).
  - **Nuance / follow-up:** `request_register` only rides on `send_login()`; the
    resume path bypasses it. Config-sync therefore fires at modeled-wake / on retry
    / on forced re-login — not on a clean resume. See plan.md P2 for options.
  - Node 1 still on v1.0.9 (OTA to v1.0.10 when convenient to activate the flag on it).

### 2026-07-11 — Slat-slack feature + calibration

- Diagnosed the partial-position overshoot (commanded 30 %/60 % landing at
  ~50 %/90 %): roller-shutter slats travel with gaps and nest at the sill, so the
  measured full time folded slat compression into the 0–100 % position map.
- Designed the fix with the user: `open_duration`/`close_duration` become
  **bar-travel-only**; slat nesting is a **phase outside** the 0–1 range
  (`sealed == position 0`, no new persisted state).
  - **Un-seal head** (opening from sealed): motor runs up with position pinned at
    0 while the slats spread, then travel counts (`open_slack`).
  - **Seal tail** (full close): extra run budget past position 0 so the max-run
    backstop doesn't cut the compression before the motor's own limit
    (`close_slack`).
- Implemented end-to-end: proto `CoverConfig.openSlack/closeSlack` (+ stubs synced
  to all 4 locations), hub cover schema/plumbing, node `MotorCtrl` phases,
  `SystemCtrl` persistence, `CmdDispatcher`/`main.cpp` wiring. Defaults 0 =
  backward-compatible. Node `PROJECT_VER` → **1.0.7**.
- Built both (node IDF 6.0 green, hub esphome green), copied `BlindsV3.bin` →
  `https_hosted/` for OTA, deployed hub, verified live: blind moves correctly.
- Calibration values (both covers): `open_duration 38`, `close_duration 36`,
  `open_slack 7`, `close_slack 7`.
- Commits pushed: hub `0524a93` + `1e120a6` (`esphome_localcomponents`),
  node `9c5daf0` (`BlindsESP`).

### Earlier (prior sessions) — hub reliability + slim packets

- Hub reliability fixes (#2 sendTask self-destruct → continue, #5a msgid replay
  window, #5b login exponential backoff, #7 senderAddress → `kHubAddress`,
  half-open-session auto-relogin). Hub `0c959db`, node `e887168`.
- Slim on-air AEAD envelope (53 → 29 B), AES-GCM-128 with truncated tag,
  session-confirm plaintext fallback, motor endstop + supply-race fixes.
- Deep-sleep wake behavior confirmed: node **keeps position** on wake (restored
  from RTC RAM), re-registers/re-logins; only a true cold power-on drives
  `FULL_UP` to re-reference.

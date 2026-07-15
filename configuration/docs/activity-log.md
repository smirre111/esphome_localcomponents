# LoRa Blinds — Activity Log

Running log of work on the ESPHome LoRa blinds system (hub + battery ESP-IDF
nodes). Newest entries first. See also the repo git history for exact diffs.

## Agenda / open items

> Prioritised fix backlog lives in **[plan.md](plan.md)** (2026-07-12 stability audit:
> WDT-starvation reset, battery 0 V race, ADC panic, stale-session hardening).

- [ ] **Node radio/task-hang diagnosis** — after a hub reboot a node once required a
  physical power-cycle to recover. Audit (2026-07-12) found a strong candidate:
  `taskDeepSleep` starves the 8 s panic WDT → reset under load (see plan P1).
  Still worth a live `idf.py -p COM6 monitor` to confirm the reset reason.
- [ ] **Battery anomaly (2026-07-12)** — ~86 min with no battery messages
  (13:39→15:05), then two 0 V transmissions. Root-caused to the send-stale-cache
  race + investigate the silence window (plan P2).
- [ ] **Slack calibration** — `open_slack`/`close_slack` and travel-only
  `open_duration`/`close_duration` are first estimates; keep tuning against
  observed partial-move accuracy.
- [ ] **Second node** — `RollladenWohnzimmer2` shares the same config; confirm it
  behaves like node 1 once it has taken v1.0.7 + the slack `CoverConfig`.

## Log

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

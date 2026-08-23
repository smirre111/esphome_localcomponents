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

### 2026-08-23 — Boot forensics: the restart that undid the schedule

**Symptom.** A node executed its scheduled CLOSE correctly (`Position: 0%`),
restarted about a minute later, and the boot path drove the blind fully open
again. The beacon reported `reason=BOOT`, which lumps POWERON, SW, EXT and USB
together — not enough to tell a power event from a reset line being pulled, so
the cause was being guessed at. The supply had recently been checked, so the
sag hypothesis was dropped and the boot-reason mechanism used instead.

**Two RTC_NOINIT markers.** RTC_NOINIT survives deep sleep and soft resets and
is garbage only after a true power loss, so it answers what the reset reason
cannot: did RTC RAM survive, and did the previous run actually reach
`esp_deep_sleep_start()`? The sleep marker is armed in the instruction *before*
that call, so seeing it without `ESP_RST_DEEPSLEEP` means the node reset while
going to sleep rather than waking from it. A second marker carries auto vs
interactive across the reset — `config.txt` cannot, because the boot policy
rewrites the mode before any diagnostic reads it. Every boot now logs
`BOOT reason=… causes=… rtc_ram=… prev=… prev_mode=…`.

**The actual bug.** The `MOTCMD_FULL_UP` reference move exists to rebuild a
position after RTC RAM is lost, but was gated on `powerOnBoot` — true for every
reset that is not a deep-sleep wake, including `esp_restart`, panic and the
watchdogs, all of which keep RTC RAM and a perfectly good position. Moving the
blind there is not a harmless default: it destroys what the schedule just
established. Now gated on the marker.

**Measured, and counterintuitive.** On this ESP32 an EN-pin reset — any serial
monitor or debugger asserting DTR/RTS — reports `ESP_RST_POWERON` and **does**
clear RTC RAM. It is not `ESP_RST_EXT` and it is a genuine cold boot, so the
reference move is correct there. The consequence for testing is larger than the
firmware fix: **attaching or reattaching a serial monitor mid-test is itself a
reboot**, so a node that appears to restart spontaneously during an observed
event may be reacting to the observer. This is a live candidate for the original
symptom. Verification therefore uses one continuously-open port with no
reconnects.

**A bug the hardware caught in the fix.** The first build logged
`rtc_ram=LOST prev=cold` and then *skipped* the reference move — the exact
inverse of the bug. `g_sleep_marker` is overwritten early in `app_main()`, so
reading the live variable later always saw "running". The boot-time value is now
latched, and a test pins the ordering.

**Testing.** Decisions live in `BlindsESP/main/include/BootPolicy.h`,
dependency-free like `Scheduler.h`, so the harness compiles the shipped code:
12 new tests, **167/167** total, mutation-verified (restoring the unconditional
move fails two).

Node `PROJECT_VER` → **1.0.15**, flashed to node 2 and copied to
`https_hosted/`. Node commit `ff7fa34`, hub commit `3d07563`.

### 2026-08-22 (evening) — auto mode proven on hardware; four real bugs found

**Automatic mode works.** Node 2 slept and woke itself for a scheduled event:

```
Beacon: reason=TIMER_CHECKIN clock_offset=-4 s fw=10014 resume=1
'RollladenWohnzimmer2 Clock Offset' >> -4 s
```

`reason=TIMER_CHECKIN` is a genuine deep-sleep timer wake, ~30 s ahead of the
event as `beacon_lead` intends. **`clock_offset = −4 s` is the D1 measurement
outstanding since P1** — the external 32.768 kHz crystal lost 4 s across the
sleep, comfortably within budget and the first real evidence for the
uncapped-sleep decision.

Four bugs found, all of which presented as something else:

1. **`config.txt` read truncated at 256 bytes** — the big one. Adding the P3/P4
   schedule fields pushed the file past a fixed `char buf[256]`, and a `fread`
   that fills its buffer leaves no null terminator. cJSON failed, **every**
   setting silently reverted to its compiled default, and the node booted
   unprovisioned at address 0 — re-registering on every boot, with the hub
   re-provisioning it each time. Presented for hours as "login not
   acknowledged" and "the schedule never persists". Now sized from the file,
   heap-allocated with a 4 KB cap, always terminated, freed on every path, and
   the parse failure logs loudly.
2. **A failed OTA stranded the node.** Both failure paths ended in
   `vTaskDelete(NULL)`; one logged "Rebooting ..." immediately before not
   rebooting. Only success called `esp_restart()`. A failed update left the node
   in WiFi OTA mode, off the LoRa air, recoverable only by physical
   power-cycling. Both paths now restart.
3. **The wake beacon was sent inside the register→login window**, where msgid
   counters are not yet synchronised, so the hub dropped it as a replay
   (`duplicate or old message ID: 3 … My MsgID: 11`). With no beacon the hub
   never learned the node's clock or schedule version, so **P4 was dead on the
   register path** while the node's own log looked healthy. Beacon now goes out
   after `CMD_LOGIN` (register path) or at boot (resume path only).
4. **Entering deep sleep from the RX task crashed the node** ~1 s after a
   schedule arrived. Sleep is now QUEUED as `SYSCMD_SLEEP` and runs on
   `processSysCommand`'s task — the context the nightly `CMD_SLEEP` has always
   used. Reverting the inline call also exposed that *nothing* put the node to
   sleep in auto mode at all; both the boot path and the schedule handler now
   queue it.

Also: **boot always comes up INTERACTIVE** (user decision). Auto mode is never
resumed from stored config — an unexplained reboot is exactly when you want the
node reachable, not asleep. The stored schedule *version* is cleared too, so the
node reports 0, the hub sees a mismatch and re-pushes with `mode=AUTO`; that
push is what re-arms it. Self-healing, one ~150 B frame per reboot, entries kept.

Plus: unprovisioned nodes now **retry REGISTER** every 60 s (a single lost frame
used to strand them), and the harness gained **sleep-path coverage** —
`enterDeepsleep()` was a bare no-op, which is why the crash passed the suite and
failed on hardware. 152/152, key assertions mutation-checked.

**Still open:** node 1 is parked on 1.0.13 and needs a power-cycle plus a serial
flash; the OTA path itself is still unreachable from the nodes; and the per-slot
HA editing entities (last P4 item) are not built.


### 2026-08-22 — Node firmware: failed OTA no longer strands the node

- **Pre-existing bug, found while diagnosing "login not acknowledged" on node 1.**
  Both OTA *failure* paths in `SystemCtrl.cpp` ended in `vTaskDelete(NULL)`; only
  the *success* path called `esp_restart()`. One failure path even logged
  `"Rebooting ..."` immediately before not rebooting.
- Consequence: any failed update left the node parked in WiFi OTA mode with the
  radio app not running — invisible to the hub, uncommandable, recoverable only
  by physically power-cycling it. The hub meanwhile bursts LoginMsg at a node
  that is not listening and reports "Login not acknowledged", which points the
  investigation at the protocol rather than at the node being in the wrong mode.
- Observed on node 1: parked at 192.168.178.51 for hours, through a manual reset
  and a second OTA attempt. **Not related to the auto-mode work** — but it is
  what made two OTA attempts look like a protocol regression.
- Every exit from those paths is a failure *before* the new image is committed,
  so the running partition is untouched and a restart just brings the node back
  on air. Both now `esp_restart()`.

### 2026-08-22 — Node 2 flashed to v1.0.14 over serial; OTA path still broken

- **The OTA never landed on either node.** Both were still on 1.0.13 the whole
  time; they entered OTA mode, failed to download, and sat there off the LoRa
  air. That silence was misread (by me) as a possible v1.0.14 fault — it was not.
  Login and TimeSync work fine.
- Node 2 flashed over the FT2232 debugger in ~19 s and verified live:
  `App version: 1.0.14`, `Sending BEACON (reason=0 resume=0 clock=0)`,
  `CMD_LOGIN → counters reset`, `CMD TIMESYNC … clock established`.
- **Config wipe → hub re-provision confirmed on hardware**: the full flash
  cleared `config.txt`, the node wrote defaults, and the hub pushed the real
  values back (`openDuration 38 / closeDuration 36 / slack 7,7`). The new P3/P4
  fields persisted too (`autoMode`, `schedVersion`, the five timings, `schedule`).
- Fixed a misleading log line it exposed: `shouldRunAutoMode()` checked the clock
  before the mode, so a node with `autoMode:0` logged *"Auto mode requested but
  clock is not valid"* on every boot — a warning about a request nobody made.
- **Still open:** nodes cannot reach `schmidteinander.local:8070`. The server is
  healthy (HTTP 200 from inside WSL) and the firewall allows it, but no
  connection from a node ever arrives. Serial flashing works around it.

### 2026-08-22 — Auto-mode P4a: hub schedule push (YAML-defined)

- **The hub can now own and push a schedule.** New `lora_client` options:
  `auto_mode`, `interactive_timeout`, `checkin_interval`, `beacon_lead`,
  `post_event_window`, `catchup_window`, and a `schedule:` list (max 8) of
  `{time, days, action, position}`. `days` accepts presets (`daily`,
  `weekdays`, `weekend`, `mon-sat`) or a list of day names, and is compiled to
  the Monday-first bitmask the node uses.
- **Reconciliation is by VERSION, not by a dirty flag.** The hub CRC32s its
  canonical blob; the node echoes the version it has applied in every beacon;
  a mismatch triggers a push. That makes it self-healing — a node that missed a
  push, was reflashed, or lost `config.txt` reports a stale version and is
  corrected on its next wake, with no hub-side memory of owing anything.
  Version 0 is never minted, since that is the node's "no schedule" sentinel.
- Push is deferred 2 s behind the TimeSync (clock first — a schedule is useless
  without one) and sent **single-shot**, since ~152 B overruns the 88 ms burst
  slot.
- **Two silent bugs, one root cause, caught by deliberately feeding the
  validator bad input.** `cv.enum()` returns an `EStr` (a *str subclass*)
  carrying the mapped int in `.enum_value`:
  - `config[CONF_ACTION] == SCHED_ACTIONS["position"]` compared a string to an
    int — always False, so the "position needs a percentage" validator **never
    fired**;
  - codegen did not emit the mapped value either: `action: position` generated
    `add_schedule_entry(450, 31, 0, 0)` — **action 0 = OPEN**. A schedule
    entry meaning "go to 40 %" would have fully opened the blind instead.

  Neither failure produced any diagnostic. Switched to `cv.one_of` (plain
  string) with the string→int mapping done once, explicitly, in `to_code`.
  Verified afterwards from the generated `main.cpp`, not just from a clean
  compile: `(720, 32, 3, 40)` = Sat 12:00, position, 40 %.
- Hub compiles (`config_hash=0x6fc050ce`); **138/138 tests still pass**.
  `auto_mode` left **false** in YAML — nothing sleeps yet.
- **P4b: the HA mode switch + schedule-pending indicator.** New
  `loracover` **switch** platform (`AutoModeSwitch`) and a `type:` option on the
  existing binary_sensor platform (`command_failed` — the default, so existing
  configs are untouched — or `schedule_pending`).
- The switch expresses **intent, not truth**. Requested and actual mode
  legitimately diverge: the node refuses auto mode without a valid clock or a
  usable schedule, and a button press at the blind flips it back on its own. So
  the hub re-publishes the switch state from what the node **reports in its
  beacon**, which is what stops the HA toggle from claiming a blind is scheduled
  when it is actually sitting in interactive mode.
- `schedule_pending` is briefly true after an edit; persistently true means
  pushes are not landing.
- Hub compiles (`config_hash=0x772a2504`); wiring verified in the generated
  `main.cpp`; **138/138 tests pass**.
- **D4: the interactive→auto return timer landed.** A button press now
  *suspends* automatic mode rather than turning it off. The first cut wrote
  `autoMode=false` to `config.txt`, which meant one press silently disabled the
  schedule until someone re-enabled it in HA — and on a node that afterwards
  only wakes on its check-in, that could be days. A schedule that stops working
  quietly is worse than one that never worked.
- The hub's configured mode is now never touched. The override is a local
  RTC-backed deadline that expires by itself, with an `esp_timer` that puts the
  node back to sleep when the window ends (without it the node would just stay
  awake — the entire cost of the feature). Every press refreshes the window, not
  just the one that woke the node. `interactiveTimeout == 0` is honoured as the
  proto documents it — "stay interactive until told otherwise", not "expire
  immediately". With no valid clock the override is treated as still active,
  since staying responsive is the safe failure.
- 6 new tests, **144/144**. Verified non-vacuous by mutation: commenting out the
  override check fails two of them.
- **Remaining in P4:** only the per-slot editing entities (time/action/position/
  enabled per schedule row). The mode switch gates the feature; per-slot editing
  is convenience on top of the YAML schedule.

### 2026-08-22 — Auto-mode P3 (started): schedule arithmetic

- **`Scheduler.{h,cpp}` — the calendar logic, dependency-free** (only
  `<stdint.h>`/`<time.h>`), so it compiles identically on device and host and
  the tests exercise the *exact* production source with no shims or staging.
- `next_occurrence()` (strictly after now, lowest index wins a tie, 8-day search
  so a same-weekday-next-week entry is still found) and `last_missed()` (latest
  in the window, because replaying an earlier missed entry after a later one
  leaves the blind in the wrong end state — the opposite tie rule).
- **All local-time arithmetic is `local = utc + offset` via `gmtime_r`** — no
  timezone database, no `setenv("TZ")`. The hub has already resolved DST,
  sunrise/sunset and jitter into absolute local minutes-of-day before a schedule
  reaches the node.
- The subtle case now pinned by a test: **the weekday mask must be evaluated in
  LOCAL time**. A Tuesday-only 00:30 entry at UTC+2 actually fires at 22:30 UTC
  on *Monday*; checking the UTC weekday would skip it and the blind would simply
  never move.
- 24 new tests, **127/127 passing**. Verified non-vacuous by mutation: breaking
  the Monday-first weekday mapping (`(tm_wday + 6) % 7` → `tm_wday`) fails 8 of
  them.
- **Schedule persistence + auto-mode sleep/wake/execute + the I8 guard landed**
  the same day. `struct Config` carries mode, schedule version, the five timings
  and up to 8 entries, persisted in `config.txt` (short keys — it lives in a
  32 KB LittleFS partition and is rewritten on every push). Every field defaults
  to its current value, so a config written by older firmware simply leaves auto
  mode off and an upgraded node behaves exactly as before.
- `CMD_SCHEDULE` applies and persists a pushed schedule, and **is acked** —
  unlike TimeSync — because the hub retransmits until acknowledged and must know
  its pending config landed.
- `enterDeepsleep()` now sleeps to `next_event − beacon_lead`, capped by the
  check-in interval, falling back to the legacy fixed `sleepDuration` whenever
  auto mode is off or unusable. **`shouldRunAutoMode()` is the single gate**: it
  refuses without a valid clock (I8) and without a schedule that can actually
  fire (Q9 at runtime) — a node that sleeps against a schedule it cannot
  evaluate does not fail loudly, it just stops answering for weeks.
- A button wake always wins: it flips the node back to interactive so someone
  standing at the blind gets a responsive device.
- **A test caught a real contradiction between the implementation and the
  proto.** `setSchedule()` treated `0` as "unset, keep default" for all timings,
  but `blinds.proto` documents *meaningful* zeros for three of them
  (`interactiveTimeout` = stay interactive, `checkinInterval` = no check-in,
  `catchupWindow` = never replay). Uniform-but-wrong handling would have
  silently disabled whatever the hub asked for. Zero handling now follows the
  proto per field; `beaconLead`/`postEventWindow` have no documented zero and
  keep their defaults.
- 14 new node tests, **138/138 passing**. Node builds (0x139480, 39 % free).
- **Remaining in P3:** nothing. P4 (hub pending-config store, schedule push, HA
  entities) is next; the interactive→auto return timer is part of it.

### 2026-08-21 — Auto-mode P2 (partial): wake beacon + clock-offset sensor

- **Node sends a `NodeWakeBeacon` on every boot/wake** carrying wake reason,
  its own clock, mode, voltage, position, `sessionResume`, `clockValid` and
  `fwVersion`. `classifyWakeReason()` reads the deep-sleep cause **before** the
  reset reason — a deep-sleep wake *is* `ESP_RST_DEEPSLEEP`, so the other order
  would mislabel every scheduled wake as a boot. Crash-like resets report
  `WAKE_UNKNOWN`, making a reset-looping node visible rather than silent.
- **Hub publishes `clock_offset` (node epoch − hub epoch) as a new sensor** on
  both nodes. **This closes P1's open question**: crystal drift is now a Home
  Assistant number rather than something only a serial cable shows. Suppressed
  when the node reports no valid clock, so a node awaiting its first TimeSync
  cannot show a ~56-year offset.
- **P2b (2026-08-22): resume-first wake — the I2 battery saving.** A provisioned
  node waking with a valid persisted session now skips REGISTER → config → login
  entirely (~4 s of awake radio) and announces itself with the encrypted beacon.
  The hub half already worked for free (any successful decrypt sets
  `session_confirmed_`/`login_acked_`).
- The failure this is built around is a **silent node**: hub rebooted while we
  slept, holds no nonce, cannot decrypt us, and we believe we are connected. So
  resume is never taken bare — a 12 s one-shot fallback re-registers unless a
  **decrypted** downlink proves the session. A *plaintext* frame deliberately
  does not count: that is exactly the `BaseNonceExchange` case where the
  fallback SHOULD fire. If the timer cannot even be created, the node registers
  immediately — losing the saving beats risking silence.
- **The hub now answers every beacon with a TimeSync**, doing two jobs in one
  frame: refreshing the node's clock each wake (correcting accumulated drift)
  and, being encrypted, serving as the node's proof that resume worked.
- Node builds (0x138120, 39 % free); hub compiles (`config_hash=0xa48f5a27`);
  **103/103 tests pass** (16 new). **Not deployed** — needs a PROJECT_VER bump to
  1.0.14 plus the matching `kFirmwareVersion`, which a test enforces.
- Harness: `esp_timer` one-shots now modelled (deterministic — a test fires them
  explicitly), and the node fixture calls `psa_crypto_init()`. That second one is
  the same trap as the hub tests: production does it in `app_main`, and without
  it every decrypt fails with "PSA key not available" while key, IV and AAD all
  look correct. Also fixed `CmdDispatcher.h` using `esp_timer_handle_t` without
  including `esp_timer.h` — it only compiled on-device via a transitive include.

### 2026-08-21 — Auto-mode P1: TimeSync end-to-end (built + tested, NOT deployed)

- **The node can now learn the time.** It has no clock source of its own (no
  SNTP, no RTC battery), so the hub seeds it: `send_timesync()` pushes epoch +
  local UTC offset 750 ms after the encrypted session is confirmed; the node
  applies it, stores offset/`dstNext` in RTC memory (survives deep sleep — as
  does the clock itself, since the ESP32's time base is the RTC timer) and logs
  local wall time.
- **The log line reports drift, not just time**: the correction is measured
  before being applied, so each re-sync prints the node's accumulated error.
  That is why TimeSync ships ahead of the scheduler — it characterises the
  32.768 kHz crystal against real sleep cycles before anything depends on it.
- No-op when the hub's own clock is invalid (sending epoch 0 would waste node
  awake time). Not acked — the P2 beacon carries `nodeEpoch` back instead, so an
  ACK would just spend battery.
- **Found a protocol detail the test harness had never modelled:** the AES-GCM
  nonce counter carries a **direction bit** — `msgid | (1ULL << 63)` on downlink,
  bare `msgid` on uplink. Every previous harness test decrypted uplinks only, so
  the first downlink test failed the tag check with correct key, AAD and base
  nonce. Sim gained explicit `derive_gcm_iv_uplink/downlink`. **SKILL.md's
  crypto table was wrong about this** (and about the AAD being 20 B, the tag
  being 16 B, and the ciphertext covering the full inner message) — corrected.
- Verified: node builds (0x137ae0, 39 % free), hub compiles
  (`config_hash=0x04280b55`), **87/87 tests pass** (9 new).
- **OTA image staged: node bumped to v1.0.13** and copied to
  `https_hosted/BlindsV3.bin` (verified `ver=1.0.13` in the app descriptor).
  The bump is **functionally required**, not cosmetic: `validate_image_header()`
  returns `ESP_FAIL` when the incoming version equals the running one, so an
  OTA of a build still stamped 1.0.12 is refused before anything is written.
- **DEPLOYED AND VERIFIED 2026-08-21 ~19:20.** Nodes OTA'd to v1.0.13 (manual),
  hub OTA'd via `esphome upload`. Hub log:

  ```
  [19:20:13] [RollladenWohnzimmer1] Login acknowledged by node (encrypted session confirmed)
  [19:20:14] [RollladenWohnzimmer1] TimeSync sent (epoch=1787332813 utcoffset=+7200 s msgid=2)
  [19:20:24] [RollladenWohnzimmer2] Login acknowledged ... TimeSync sent (epoch=1787332824 ...)
  ```

  Node serial confirms the other half — `App version: 1.0.13`,
  `CLK: Using external 32kHz crystal` (D1's premise verified on hardware), and
  `CMD TIMESYNC: 2026-08-21 19:34:54 (UTC+2) — clock established`.
- Both `Command Failed` sensors were ON before the hub update (stale state from
  the node OTA — the node reboots into OTA before acking the tracked sysop) and
  cleared to OFF on the fresh session.
- **Caveat:** opening COM6 resets the node via the DTR/RTS auto-reset circuit,
  which triggers its power-on `MOTCMD_FULL_UP` reference move — i.e. reading
  node serial physically moves that blind. Ask before doing it. (Node 2 does
  have a powered motor supply — it reports 14.4 V — so that move was real.)

## Closed — "node 2 battery ADC reads zero" was a MISDIAGNOSIS (2026-08-22)

**There is no ADC fault.** Node 2 reports 14.4 V / 100 %. Recorded here because
the reasoning error is the reusable part:

- `frtosTasks: ADC: value: 0` in the node log is the **motor-current** task
  (`ADC_UNIT_2`, channel 8 = GPIO25), *not* the battery. The battery task logs
  `Battery voltage: …` and never appeared in a 25 s capture because its interval
  is 15 minutes. Zero motor current at idle is correct.
- The `0.0 V / 0 %` seen in Home Assistant was **stale hub state**: the hub had
  25 days of uptime and was read moments before its OTA. After the reboot and a
  fresh session, node 2 reported a real measurement.

Two unrelated zeros, joined into a fault that did not exist.

Code checked while investigating, all sound: the `ADC_CHANNEL_8` "invalid on
ADC1" suspicion was wrong (the handle is *named* `adc1_handle` but is
`ADC_UNIT_2`); `batteryAcquireSupply()` returning false only happens when the
motor is already running, i.e. the divider's rail is already powered; and
`ADC_ATTEN_DB_6` is not saturating (14.4 V through the 113k/13k divider is
~1.66 V, inside the ~1.75 V range).

**Real leftover, minor:** `adc1_handle` in `frtosTasks.cpp` holds an
`ADC_UNIT_2` handle. That misleading name is what made the wrong hypothesis
plausible in the first place — worth renaming.

### 2026-08-21 — Auto-mode P0: proto scaffolding (built, NOT deployed)

- **Plan written and approved:** [auto-mode-plan.md](auto-mode-plan.md) — an
  automatic (scheduled) node mode where a node deep-sleeps between time-based
  events instead of listening continuously. 6 phases (P0–P5), deployed
  **node-first, hub-second** throughout (nodes are OTA'd *through* the hub, so a
  hub update that broke the link would strand them).
- **P0 shipped to the bench only:** new proto messages `TimeSync`,
  `ScheduleEntry`, `ScheduleConfig`, `NodeWakeBeacon` + enums `SchedAction`,
  `NodeMode`, `WakeReason`; oneof wiring `CMD_TIMESYNC=16`, `CMD_SCHEDULE=17`,
  `PROTO_BEACON=16`; `ClientOperation` gains `CMD_MODE_AUTO=5` /
  `CMD_MODE_INTERACTIVE=6`. **Nothing sends or handles them yet — zero
  behaviour change by design.**
- Stubs regenerated via WSL `protoc-c` (protobuf-c 1.5.2) and distributed to all
  7 files; `cmp`-verified identical. Diff is additive-only (+1091/−12; the 12 are
  descriptor array sizes and one trailing comma).
- **The gate for the whole plan passed:** unknown-field / empty-`oneof` handling
  is already safe on all four receive paths (node `onReceiveNew`, hub
  `set_response` ×3) — quiet ignore, correct `free_unpacked`, no log flood. This
  is what makes node-first deployment safe in every later phase.
- Builds: node `BlindsV3.bin` 0x135ef0 B (39 % free), links clean; hub compiled,
  `config_hash=0x04280b55`.
- **Not flashed.** Deployment (nodes first, then hub) pending — P0 + P1 will be
  deployed together.

#### Test harness repaired — it had not built since ~v1.0.9

`tests/proto_sim` was **broken and providing zero coverage**: it still modelled a
protocol version that no longer exists. Repaired and extended; **78/78 pass**.
The suite now runs before entering each phase. Drift it had missed: the
`LoraHeader.encrypted` flag (removed), `EncryptedPayload.algo/iv/aad` (removed —
slim tag+ciphertext envelope), 20-byte AAD (now 16), 16-byte AEAD tag (now
**truncated to 8**), full-message ciphertext (now **payload-only**), the 500 ms
REGISTER→login delay (now 4000 ms), and — the subtle one — `login_acked_` now
being set **only** on a successful decrypt, so a plaintext reply no longer
acknowledges a login challenge. Missing shims added (`nvs`, `binary_sensor`,
`MotorCtrl::setRuntime/setSlack`, `SystemCtrl::setSlack/setBatteryInterval`,
`esp_err_to_name`).

Two traps worth remembering: the `schema_drift_esphome_stubs` gate had pointed at
the proto directory deleted in v1.0.12 (removed — the hub-vs-esphome gate covers
it); and PSA Crypto is initialised in `LORAListener::setup()`, which these
scenarios never call — the resulting key-import failure is the one branch in
`decrypt_payload_gcm` that returns false **without logging**.

New `scenarios/auto_mode_proto_test.cpp` (13 tests) covers round-trip of every
new message through the real stubs, the unknown-field forward-compat property,
and the frame budget measured on real bytes: **8-entry ScheduleConfig = 152 B
on air, 95.3 ms airtime** — under the 255 B limit but over the 88.2 ms burst
slot, so D5 (single-shot, non-bursted schedule push) is now test-enforced.

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
- Deploy: hub OTA'd, then **all nodes OTA'd to v1.0.12** (OTA host `:8070`
  reachability fixed) — the configurable 15-min battery interval is now active
  system-wide.
- **Closed the P2 battery-silence-gap investigation:** root cause was a depleted
  battery (consistent with node 1's low-battery outage + recovery this session).

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

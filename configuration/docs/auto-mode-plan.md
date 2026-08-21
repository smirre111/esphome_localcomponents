# LoRa Blinds — Automatic (Scheduled) Node Mode — Implementation Plan

**Status:** DRAFT — for review, nothing implemented yet.
**Date:** 2026-08-21
**Scope:** Add a battery-saving *automatic* mode where a node sleeps between
scheduled events instead of listening continuously, executes time-based
open/close/position commands autonomously, and re-syncs with the hub at each
wake.

Companion docs: [plan.md](plan.md) (fix backlog) · [activity-log.md](activity-log.md)

---

## 0. Decisions already taken (from the review round)

| # | Decision | Rationale |
|---|----------|-----------|
| D1 | **No cap on sleep length.** Node sleeps straight through to the next event. | Confirmed: `CONFIG_RTC_CLK_SRC_EXT_CRYS=y` — the node's RTC runs off the external 32.768 kHz crystal (±20 ppm ≈ **2 s/day**), not the internal RC. Drift is a non-issue. |
| D2 | **Entry format:** time-of-day + 7-bit weekday mask + action, **plus** sunrise/sunset-relative entries. Sun times are resolved **hub-side**; refresh interval configurable (default: on every beacon, forced push ceiling `7d`). | Node stays dumb and autonomous; hub already has HA's sun data. |
| D3 | **Up to 8 schedule slots per node**, flexible count (YAML list length decides), and the whole schedule **must fit in one LoRa frame**. | Byte budget verified in §3.3 — 8 entries ≈ 150 B on air vs. the 255 B SX1278 limit. |
| D4 | **Button press → interactive for 30 min → back to auto** (timeout configurable, `0` = stay interactive). Plus a periodic **check-in wake** (default 6 h) so hub-side config never waits longer than that. | |
| D5 | **`ScheduleConfig` is sent single-shot (`burstCount = 0`), not bursted.** | Frame limit is genuinely 255 B (§3.3), but every hub downlink today is burst-repeated 17× at **88 ms** spacing, and a ~150 B frame needs ~95 ms of airtime — it would overrun its own slot. Right after a beacon the node is provably listening, so the burst is unnecessary anyway. |
| D6 | **Deploy order is always node first, then hub.** | Nodes are OTA'd *through* the hub; if a hub update broke the link, the nodes would be unreachable. This makes "new node firmware must run correctly against the *old* hub" a hard requirement on every phase (§6.1). |

All of §8's questions are now answered (Q5–Q9 accepted as proposed) and I1–I8
are accepted. Remaining decisions: none — this plan is ready to execute.

---

## 1. Ideas on top of the request — **all accepted (I1–I8)**

These were *not* in the instruction file. All eight are in scope.

| # | Idea | Why it's worth it | Cost |
|---|------|-------------------|------|
| **I1** | **Beacon-first, execute-second.** Wake `beacon_lead` (default 30 s) *before* the event, do the beacon exchange, apply any pending config, *then* execute at the exact event time. | A schedule change you made in HA an hour ago takes effect on **this** event rather than the next one — including cancelling it. Without this, every edit is one event late. | Free (same wake) |
| **I2** | **Skip the login handshake on auto-mode wakes.** The node already persists msgid counters + hub base nonce to NVS (F-5). Beacon carries `session_resume=1`; the hub re-logins only if a decrypt actually fails. | Saves ~4 s of awake radio per wake — the single biggest battery item in a wake cycle (`kRegisterToLoginDelayMs` alone is 4 s today). | Small |
| **I3** | **Missed-event catch-up.** On wake, execute any enabled entry whose time passed while the node was off/rebooting, within `catchup_window` (default 30 min, `0` = off). | Today a reboot or a flat-battery hour silently loses the morning open. | Small |
| **I4** | **HA diagnostics:** `next event` text sensor, `mode` text sensor, `config pending` binary sensor, `clock offset (s)` sensor (hub epoch − beacon epoch). | Makes an otherwise invisible, sleeping system debuggable without a serial cable. Clock offset also proves D1 empirically. | Small |
| **I5** | **`skip_next` / `run_now` one-shots.** HA buttons that queue "skip the next scheduled event" or "run entry N at the next wake". | The obvious real-world escapes (guests, holiday, window cleaner) without editing the schedule. | Small |
| **I6** | **Randomised jitter** per entry (`jitter: ±15min`), hub-resolved like sun times. | Presence simulation while away — the classic use for scheduled blinds. | Small |
| **I7** | **DST-aware push, gated by a shift threshold** (`resync_threshold`, default **15 min**). The hub recomputes resolved entry times continuously, but only marks the node's config dirty when some resolved entry moves by **≥ threshold** vs. what the node has applied. | Entries are *local* times. Without this a node that only beacons weekly runs an hour off for up to a week after a DST change. The threshold is what stops the opposite problem: sunrise drifts 1–2 min/day and jitter (I6) re-rolls constantly, so an ungated rule would mark config dirty on *every* beacon and push a frame each time. DST (60 min) always crosses the threshold; sun drift only does so after ~1–2 weeks. | Small |
| **I8** | **Sanity clamp: refuse to sleep without a valid clock.** If the node has never received a `TimeSync` (or the RTC is invalid), it stays interactive and keeps asking, instead of sleeping blind. | Prevents a node from disappearing forever after a battery swap. | Small |

**I7 threshold detail.** `resync_threshold` is a per-node YAML option. The
comparison is per entry, on the *resolved* local minute-of-day, against the
values last acked by the node — so the rule is "push when at least one entry
would fire ≥ 15 min away from where the node currently thinks it fires". A
*manual* edit in HA always pushes regardless of threshold: an explicit user
change is never suppressed as noise.

---

## 2. Design overview

### 2.1 Two modes

```
                 HA switch / sysop CMD_MODE_*
        ┌──────────────────────────────────────────┐
        ▼                                          │
┌───────────────┐   button press (any)     ┌───────────────┐
│  AUTO         │─────────────────────────►│  INTERACTIVE  │
│  radio off    │                          │  radio in RX  │
│  deep sleep   │◄─────────────────────────│  (today's     │
│  wake@event   │  interactive_timeout      │   behaviour)  │
└───────────────┘  (default 30 min, 0=off) └───────────────┘
```

**INTERACTIVE** is exactly today's behaviour — nothing about it changes, so a
node that never gets an auto-mode config behaves as it does now.

### 2.2 Auto-mode wake cycle

```
   T-30s   wake (RTC timer)                     ┌ radio on
     │  1. NodeWakeBeacon → hub                 │
     │     (reason, sched_version, node_epoch,  │
     │      mode, voltage, position)            │
     │  2. hub replies: TimeSync                │
     │     + ScheduleConfig  *if* pending       │
     │  3. node applies → recomputes next event │
   T=0    4. execute this entry's action        │
     │  5. CoverPosition + ClientBattery → hub  │
   T+20s  6. post_event_window expires          └ radio off
     │  7. compute next event, deep sleep
     ▼
```

Wake reasons: `TIMER_EVENT`, `TIMER_CHECKIN`, `BUTTON`, `BOOT`, `UNKNOWN`.
`BUTTON` additionally flips the node to INTERACTIVE and arms the 30-min timer.

### 2.3 Who owns what

| Item | Owner | Notes |
|------|-------|-------|
| Schedule **source of truth** | **Hub** (YAML defaults + HA edits, persisted in ESPHome flash) | Last write wins — an HA edit overwrites a still-pending update, per your instruction. |
| Schedule **working copy** | **Node** (LittleFS, next to `config.txt`) | Node must survive a hub outage indefinitely. |
| Sync mechanism | `schedule_version` (CRC32 of the canonical blob) in the beacon vs. hub's pending hash | Same pattern as the `config-hash` idea already parked in [plan.md](plan.md) P2 — this implements it for schedules, and we can reuse it for `ClientConfig` later. |
| Wall clock | Hub → node via `TimeSync` at every beacon | Node has **no** clock source today (`timeSyncTime` in `main.cpp:62` is an unused stub). |
| Sun times, jitter | **Hub** resolves to absolute minute-of-day | Node never computes astronomy. |

---

## 3. Protocol changes

### 3.1 New messages

```protobuf
// ---- hub -> node ----

message TimeSync {
    uint64 epoch      = 1;  // Unix UTC seconds at hub TX
    int32  utcOffset  = 2;  // local offset incl. DST, seconds
    uint32 dstNext    = 3;  // epoch of next DST transition, 0 = unknown
}

enum SchedAction {
    SCHED_OPEN     = 0;
    SCHED_CLOSE    = 1;
    SCHED_STOP     = 2;
    SCHED_POSITION = 3;  // uses positionPct
}

message ScheduleEntry {
    uint32 minuteOfDay = 1;  // 0..1439, LOCAL time, hub-resolved (sun/jitter applied)
    uint32 dayMask     = 2;  // bit0=MON .. bit6=SUN
    SchedAction action = 3;
    uint32 positionPct = 4;  // 0..100, only for SCHED_POSITION
    uint32 kind        = 5;  // 0=fixed 1=sunrise 2=sunset (telemetry/UI only)
}

enum NodeMode { MODE_INTERACTIVE = 0; MODE_AUTO = 1; }

message ScheduleConfig {
    uint32 version              = 1;  // CRC32 over the canonical blob
    NodeMode mode               = 2;
    uint32 interactiveTimeout_s = 3;  // 0 = never auto-return
    uint32 checkinInterval_s    = 4;  // 0 = no check-in wake
    uint32 beaconLead_s         = 5;
    uint32 postEventWindow_s    = 6;
    uint32 catchupWindow_s      = 7;  // 0 = no catch-up
    repeated ScheduleEntry entries = 8;  // <= 8, only ENABLED entries are sent
}

// ---- node -> hub ----

enum WakeReason { WAKE_BOOT=0; WAKE_TIMER_EVENT=1; WAKE_TIMER_CHECKIN=2;
                  WAKE_BUTTON=3; WAKE_UNKNOWN=4; }

message NodeWakeBeacon {
    WakeReason reason      = 1;
    uint32 schedVersion    = 2;  // version the node currently has applied
    uint64 nodeEpoch       = 3;  // node's clock at TX -> hub measures drift
    NodeMode mode          = 4;
    float  voltage         = 5;
    float  position        = 6;
    uint32 awakeWindow_ms  = 7;  // how long the node will keep listening
    uint64 nextEventEpoch  = 8;  // 0 = none scheduled
    bool   sessionResume   = 9;  // I2: node kept its AEAD session, skip login
    bool   clockValid      = 10; // I8: false -> node refuses to sleep
    uint32 fwVersion       = 11; // capability gate for the hub
}
```

### 3.2 Oneof wiring (new field numbers only — never renumber)

`LoraClientOperationMessage.cmd` (9–15 used) → add:
```protobuf
TimeSync       timesync = 16;
ScheduleConfig schedule = 17;
```
`LoraClientResponseMessage.proto` (9–15 used) → add:
```protobuf
NodeWakeBeacon beacon = 16;
```
`ClientOperation` enum (0–4 used) → add:
```protobuf
CMD_MODE_AUTO        = 5;   // immediate transition for an awake node
CMD_MODE_INTERACTIVE = 6;
```
Mode lives in **both** places on purpose: `ScheduleConfig.mode` is the persisted
setting; the sysops are the immediate, **tracked/acked** toggle for an awake node
(they ride the existing `send_tracked_sysop_` retransmit path for free).

### 3.3 Frame budget — is the limit 255 or 128?

**It is 255 here.** The 128 figure is real but comes from a configuration this
project does not use. Verified in both drivers:

| Check | Node `components/lora/lora.cpp` | Hub `lora_tracker/lora.cpp` | Meaning |
|-------|-------------------------------|------------------------------|---------|
| `FIFO_TX_BASE_ADDR` / `FIFO_RX_BASE_ADDR` | both `0` (L507-508) | both `0` (L413-414) | The whole 256-byte FIFO backs the single packet in flight. **The common 128/128 split (`TxBase=0x80`, `RxBase=0x00`) — which is where "128 bytes" usually comes from — is not used here.** |
| `MAX_PKT_LENGTH` | `255` (L79) | `255` (L95) | Driver ceiling. |
| `REG_MAX_PAYLOAD_LENGTH` (0x23) | setter exists (L1105) but is **never called** | setter exists (L1023), **never called** | Stays at the chip reset default `0xFF` = 255. |
| RX/TX buffers | `BUFFER_SIZE 256` | `BUFFER_SIZE 256`, TX staging `buf_[255]` | Consistent end to end. |

`REG_PAYLOAD_LENGTH` is an 8-bit register, so 255 is the hard chip maximum in
explicit-header LoRa mode regardless. (The *other* common source of a low cap is
LoRaWAN's regional duty-cycle payload limits, 51–242 B by data rate — not
applicable, this is raw point-to-point LoRa.)

**Budget:**

| Part | Bytes |
|------|-------|
| Outer header (6 varint fields + tags) | ~15 |
| `EncryptedPayload` tag + framing | ~20 |
| `ScheduleConfig` scalars (7 fields) | ~20 |
| 8 × `ScheduleEntry` (~12 B each incl. nested tag+len) | ~96 |
| **Total on air** | **≈ 150 B** |
| Limit | **255** |

Fits in one frame with ~100 B headroom. If we ever need >8 entries, the fallback
is a `repeated uint32` bit-packed form (11 b minute, 7 b days, 3 b kind, 7 b
position → ~5 B/entry), which buys ~30 entries. Not needed now.

### 3.4 The real constraint is airtime, not the frame limit → D5

Your question turned up something the byte budget alone hides. At SF7 / BW500 /
CR4/8 the symbol time is 0.256 ms, so:

| Frame | Approx. airtime |
|-------|-----------------|
| Typical cover op today (~50 B) | ~36 ms |
| `ScheduleConfig` with 8 entries (~150 B) | **~95 ms** |

Every hub downlink today is **burst-repeated `txSlotsPerRound = 17` times at
`roundDurationMs / txSlotsPerRound = 1500/17 = 88 ms`** spacing
([lora_tracker.h:148-158](../local_components/lora_tracker/lora_tracker.h#L148)),
and the node hard-codes the matching `kBurstTxIntervalMs = 88`
([CmdDispatcher.h:165](../../../Development/PlatformIO/PlatformIO/BlindsESP/main/include/CmdDispatcher.h#L165))
to predict when the burst ends. A 95 ms frame **overruns its own 88 ms slot** —
the burst would stretch, and the node's `burst_end_us_` estimate would drift
early, putting its deferred reply back on top of the burst.

**D5: send `ScheduleConfig` single-shot (`burstCount = 0`), not bursted.** The
burst exists to catch a node whose listening window is unknown; right after a
beacon the node is *provably* awake and listening, so a single copy plus the
existing retransmit-until-acked tracking (§5.1) is both sufficient and cheaper.
This also keeps the 88 ms slot assumption untouched — no change to the burst
machinery, and no risk to existing traffic.

Consequence for the phase plan: P4 must assert `burstcount == 0` on the schedule
path, and P0 should add a debug-build check that any bursted frame stays under
~80 ms of airtime so this class of bug can't return unnoticed.

### 3.5 Stub regeneration

Per [SKILL.md](../SKILL.md): regenerate with WSL `protoc-c`, then distribute to
**all five** locations atomically (`BlindsESP/proto`, `components/blinds/src` +
`include`, `main/include`, `esphome/.../blindsproto`). Forgetting
`components/blinds/` gives a linker error, not a compile error.

**Version skew is safe both directions** (proto3 ignores unknown fields): an old
node never sends a beacon → hub falls back to today's `sleep_duration` model; an
old hub never sends `ScheduleConfig` → node stays interactive.

---

## 4. Node changes (`BlindsESP`)

| File | Change |
|------|--------|
| `main/include/SystemConfig.h`, `SystemCtrl.{h,cpp}` | Extend `struct Config` with `mode`, `interactiveTimeout_s`, `checkinInterval_s`, `beaconLead_s`, `postEventWindow_s`, `catchupWindow_s`, `schedVersion`, `ScheduleEntry entries[8]`, `entryCount`. Extend `loadConfiguration()`/`saveConfiguration()` (LittleFS). **New:** `setSchedule()`, `getNextEventEpoch()`. |
| **new** `main/Scheduler.{h,cpp}` | Pure scheduling logic, unit-testable off-target: `nextOccurrence(entries, now_local, dayMask)`, `missedSince(entries, from, to)` (I3), local↔UTC conversion from `TimeSync.utcOffset`. Keeps calendar maths out of `CmdDispatcher`. |
| `main/CmdDispatcher.{h,cpp}` | Handle `CMD_TIMESYNC` (→ `settimeofday`, mark `clockValid`), `CMD_SCHEDULE` (→ validate, persist, ack, recompute), `CMD_MODE_AUTO/INTERACTIVE`. New `sendWakeBeacon(reason)`. Reinterpret `CMD_SLEEP` in auto mode as "sleep to next event". |
| `main/SystemCtrl.cpp` `enterDeepsleep()` | Replace the fixed `g_config.sleepDuration_s` timer with `next_wake = min(nextEvent − beaconLead, now + checkinInterval)`. Keep EXT1 button wake unchanged. Guard: if `!clockValid` → **do not sleep** (I8). Defensive: if `rtc_clk_slow_src_get() != XTAL32K` (silent IDF fallback to the RC oscillator — `CONFIG_RTC_XTAL_CAL_RETRY=1`), log a warning and cap sleep at 1 h with a resync wake. |
| `main/main.cpp` | Boot/wake dispatch: classify `WakeReason` from `esp_sleep_get_wakeup_causes()`; in auto mode go beacon → apply → execute → sleep instead of entering the interactive idle loop. Retire the unused `timeSyncDone`/`timeSyncTime` RTC stubs in favour of real state. |
| `main/Buttons.cpp` | Every button callback: if `mode == AUTO`, first `switchToInteractive()` (arms the 30-min timer, sends a `WAKE_BUTTON` beacon), then execute the motor command as today. |
| RTC RAM | `last_executed_epoch[8]` + `mode` + `interactive_until` in `RTC_DATA_ATTR` so a wake cycle can't double-execute an entry (mirrored to NVS for crash survival). |

**Battery-relevant note:** `lora_sleep()` is already called before deep sleep, so
the SX1278 is off — a hub *cannot* wake a sleeping node on demand. That is
exactly why the check-in wake (D4) exists. Separately, `enterDeepsleep()` puts
`loraDio0Pin`/`loraDio1Pin` into the EXT1 wake mask
([SystemCtrl.cpp:1659](../../../Development/PlatformIO/PlatformIO/BlindsESP/main/SystemCtrl.cpp#L1659))
while the following `esp_sleep_enable_ext1_wakeup_io()` calls list only the three
button pins. Harmless today (radio asleep → DIO never asserts) but misleading;
propose removing the DIO bits as a drive-by cleanup.

---

## 5. Hub changes (ESPHome)

| File | Change |
|------|--------|
| `lora_client/__init__.py` | New schema: `mode`, `interactive_timeout` (30min), `checkin_interval` (6h), `beacon_lead` (30s), `post_event_window` (20s), `catchup_window` (30min), `sun_refresh_interval` (7d), and a `schedule:` list (≤8) of `{time \| sun: sunrise/sunset, offset, days, action, position, jitter}`. Codegen emits one HA entity group **per listed slot** — so the slot count is flexible up to 8, fixed at compile time (ESPHome entities cannot be created at runtime). |
| `lora_client/lora_client.{h,cpp}` | New state: `pending_sched_blob_` + `pending_hash_`, `applied_hash_`, `next_node_wake_epoch_`, `node_mode_`. New `send_schedule_config()`, `send_timesync()`, `handle_beacon_()`. Extend `TrackedOpKind` with `CONFIG` so schedule pushes reuse the existing retransmit-until-acked machinery. |
| `lora_client/lora_client.cpp` — **awake model** | `is_node_awake_()` / `ms_until_node_awake_()` currently assume `last_sleep_epoch_ + sleep_duration_`. **That model is wrong in auto mode** and must be replaced by a hub-side mirror of the node's schedule (same `Scheduler` maths). Also: suppress the hourly login retry against a node known to be asleep — today it would burn airtime for nothing. |
| `lora_client/lora_client.cpp` — persistence | Add `pending_hash_` / `applied_hash_` / `node_mode_` to `LORAClientRestoreState`; bump `kRestoreStateVersion` 2 → 3 (old blobs are discarded by the existing version check — no migration needed). |
| **new** `loracover/switch/` | `auto_mode` switch (sends `CMD_MODE_*` when awake, otherwise queues into the pending config) + per-slot `enabled` switches. |
| **new** `loracover/datetime/`, `select/`, `number/` | Per slot: `datetime.time` (time-of-day), `select` (action), `number` (position %), `select` (day preset — see Q5). All `restore_value: true` so HA edits survive a hub reboot. Any write → recompute hash → mark pending → push at next beacon (or immediately if the node is awake/interactive). |
| **new** `loracover/text_sensor/` | I4: `next event`, `mode`; plus `config pending` binary sensor and `clock offset` sensor. |
| `loradevices.yml` | Add the `schedule:` blocks; **remove/guard the 23:00 `on_sleep_start` automation for any node in auto mode** — it conflicts with schedule-driven sleep (see R1). |
| `sun` component | Required in YAML (with lat/lon) for sunrise/sunset entries. |

### 5.1 Pending-config store (your §"Hub changes")

```
HA edit ──► recompute canonical blob + CRC32 ──► pending_hash_
                                                    │
                     (overwrites any still-pending update — last write wins)
                                                    │
beacon arrives ──► beacon.schedVersion != pending_hash_ ?
                        │ yes → send ScheduleConfig (tracked, retransmit-until-acked)
                        │        on CommandAck → applied_hash_ = pending_hash_
                        └ no  → send TimeSync only, node sleeps sooner
```
Survives hub reboot via `ESPPreference`. The `config pending` binary sensor (I4)
makes the queued state visible in HA instead of silent.

---

## 6. Delivery phases

Each phase is independently buildable, flashable and testable. Proto regen +
both firmwares always ship together (SKILL.md rule).

| Phase | Content | Verifiable by |
|-------|---------|---------------|
| **P0** ✅ | Proto: all new messages + oneof/enum wiring; regenerate stubs to all 7 files; rebuild both firmwares with **no behaviour change**. | **Done — built, not yet deployed.** See §6.2. |
| **P1** ✅ | `TimeSync` end-to-end. Hub sends it once the encrypted session is confirmed; node sets its clock, stores the UTC offset in RTC memory and logs local time + drift. No scheduling yet. | **Done — DEPLOYED and verified on hardware 2026-08-21.** See §6.4. |
| **P2** ◐ | `NodeWakeBeacon` + hub `handle_beacon_()` + session-resume (I2). Node still in interactive mode; beacon fires on boot only. | **Beacon + hub handling + clock-offset sensor done and tested (§6.5). The node-side REGISTER-skip that realises I2's battery saving is NOT done — see §6.5.** |
| **P3** | Node scheduler: `Scheduler.{h,cpp}` + config persistence + auto-mode sleep/wake/execute + I8 clock guard. Schedule hard-coded in YAML defaults only (no HA editing yet). | A node executes a YAML schedule for 48 h unattended; battery drain measured against the interactive baseline. |
| **P4** | Hub pending-config store + `ScheduleConfig` push + HA entities (switches, datetime, selects, numbers) + I4 diagnostics. | Edit a time in HA → node applies it at the next beacon; `config pending` clears on ack. |
| **P5** | Sun/jitter resolution (D2, I6), DST push + threshold (I7), catch-up (I3), `skip_next`/`run_now` (I5), interactive-timeout return (D4). | Sunrise entry tracks the actual sunrise across a week; a DST switch corrects within one beacon; sun drift does *not* cause a push until it crosses 15 min. |

### 6.1 Deployment order per phase (D6) — node first, then hub

Nodes are OTA'd *through* the hub, so a hub update that broke the link would
strand them. Every phase therefore deploys **all nodes first, verify, then the
hub** — which imposes a hard rule on the code:

> **New node firmware must behave identically to the old node firmware when
> talking to the old, not-yet-updated hub.**

Concretely, per phase:

| Phase | Node-first safe because… |
|-------|--------------------------|
| **P0** | Pure stub regeneration, no behaviour change. The one thing to verify: a new-numbered field arriving at an **old** parser lands in protobuf-c's unknown-field list and leaves the `oneof` case at `0` — so both sides must handle "no known `cmd`/`proto` set" as a quiet ignore, not an error-log flood or an assert. **This is the single most important check in the whole plan**, because it is what makes every later phase's node-first order safe. Verify it *before* P0 leaves the bench. |
| **P1** | Node accepts `TimeSync` but never requires it; an old hub simply never sends one and the node's `clockValid` stays false → I8 keeps it interactive = today's behaviour. |
| **P2** | Node sends a beacon; an old hub sees an unknown `proto` case and ignores it (per the P0 check). Session-resume (I2) only *skips* a login the hub would otherwise re-drive anyway, and the hub re-logins on any decrypt failure — so the fallback path is the existing one. |
| **P3** | **Q9 is what makes this safe:** a node with no pushed schedule defaults to INTERACTIVE and can only enter auto mode once a hub has sent a `ScheduleConfig` with ≥1 enabled entry. An old hub never sends one, so a P3 node against an old hub is just a normal interactive node. |
| **P4/P5** | Hub-side only, plus node handlers already shipped in P3. Deploy hub last as usual. |

Rollback at every phase is "reflash the previous node image", which is exactly
why the node must never be left dependent on a hub feature that isn't live yet.

### 6.2 P0 result (2026-08-21) — built, **not deployed**

**The unknown-field / empty-`oneof` check passed on all four receive paths, with
no code change required.** This was the gate everything else rested on:

| Path | Behaviour on an unknown `oneof` case | Verdict |
|------|--------------------------------------|---------|
| Node `CmdDispatcher::onReceiveNew` | `CMD__NOT_SET` is an explicit case → quiet `ESP_LOGI`, then the normal `free_unpacked` tail | Safe |
| Hub `LORAListener::set_response` | `if`/`else-if` chain, no error `default:`; falls through and still frees `rcv_message` | Safe |
| Hub `LoraCoverComponent::set_response` | `if`-based `proto_case` checks only | Safe |
| Hub `lora_sensor` `set_response` | `if`-based `proto_case` checks only | Safe |

No error-log flood, no leak, no assert. A useful side effect for P2: an
unrecognised-but-msgid-valid frame still passes the replay check and sets
`login_acked_` / `session_confirmed_`, which is exactly what a beacon needs.

**Additive-only proof.** `git diff` on the stubs shows +1091 / −12, and all 12
"deletions" are mechanical: descriptor array sizes (`[8]`→`[10]`, `[5]`→`[7]`)
and a trailing comma on the last enum value. Every pre-existing `oneof` case
value is unchanged (9–15); new ones are `CMD_TIMESYNC = 16`,
`CMD_SCHEDULE = 17`, `PROTO_BEACON = 16`.

**Builds:** node `BlindsV3.bin` 0x135ef0 B (39 % of partition free), links clean
— which is the real test of the `components/blinds/src/` copy, since a missed
copy there fails at *link*, not compile. Hub compiled, `config_hash=0x04280b55`.

**Latent trap logged for P3:** `blindsSysPbToCmd()` (`CmdDispatcher.cpp:191`)
maps unhandled sysops to `default: return 0`. All real commands are ASCII `'1'`–
`'5'` (49–53), so `0` matches nothing and the new `CMD_MODE_AUTO` /
`CMD_MODE_INTERACTIVE` values are inert today — but P3 must add explicit cases
rather than depend on that.

### 6.3 Test harness — repaired and extended (run before every phase)

`tests/proto_sim` (host GoogleTest, run under WSL) had **rotted since roughly
v1.0.9 and did not build at all**. It was mirroring a protocol version that no
longer exists, so it was silently providing zero coverage. Repaired as part of
P0; **78/78 now pass**, and the suite is the gate before each subsequent phase.

Protocol drift the harness had missed — each of these is a place the *tests*
disagreed with shipped production behaviour:

| Stale assumption | Production reality |
|------------------|--------------------|
| `LoraHeader.encrypted` flag exists | Field 5 removed; encryption inferred from the oneof case |
| `EncryptedPayload` carries `algo`, `iv`, `aad` | Slim envelope: **tag + ciphertext only**; IV/AAD rederived from the plaintext outer header |
| AAD is 20 bytes (5 fields) | **16 bytes** (4 fields) |
| AEAD tag is 16 bytes | **Truncated to 8** (`kAesGcmTagBytes`) for the slim envelope |
| Ciphertext covers the full inner message | **Payload-only** — the inner header is stripped before encryption |
| REGISTER → login delay is 500 ms | `kRegisterToLoginDelayMs` = **4000 ms** (800 ms on the fast path) |
| A plaintext reply acknowledges a login | `login_acked_` is set **only** inside the successful-decrypt branch |
| Header had no burst fields | `burstIndex` / `burstCount` added |
| `ClientConfig`/`CoverConfig`/`LoginMsg`/`ClientRegister` field sets | `batteryInterval`, `openSlack`/`closeSlack`, `request_register`, `needs_config` all missing from the mirror; `CommandAck` absent entirely |

Also fixed: a missing `nvs.h`/`nvs_flash.h` shim (F-5 persistence), missing
`binary_sensor` shim (F-4 command-failed sensor), `MotorCtrl::setRuntime/setSlack`
and `SystemCtrl::setSlack/setBatteryInterval` shims, and `esp_err_to_name`.

**Two harness gaps worth remembering:**

1. **`schema_drift_esphome_stubs` had been failing-by-misconfiguration** since
   v1.0.12 — it pointed at `local_components/lora_tracker/proto/`, deleted in
   that release. Removed; `schema_drift_hub_vs_esphome` covers the same ground
   from the authoritative proto and now passes against the P0 stubs.
2. **PSA Crypto is initialised in `LORAListener::setup()`**, which these
   scenarios deliberately never call. Without an explicit `psa_crypto_init()`
   the key import fails — and that is the *one* branch in `decrypt_payload_gcm`
   that returns false **without logging**, so it presents as a bare
   "AES-GCM decryption/authentication failed". Harness now calls it up front.

New P0 coverage (`scenarios/auto_mode_proto_test.cpp`, 13 tests):

- Round-trip of `TimeSync` / `ScheduleConfig` (8 entries) / `NodeWakeBeacon` /
  the mode sysops through the **real generated stubs**, including a
  negative-`utcOffset` case pinning that field's signedness and an empty-schedule
  case pinning Q9.
- Forward compatibility: an unknown field number decodes as `NOT_SET` with the
  **header still readable** — the property the node-first deploy order rests on.
- Frame budget and airtime, measured on real encoded bytes rather than estimated:

  ```
  [budget] 8-entry ScheduleConfig: plaintext 139 B, on-air (AEAD) 152 B,
           airtime 95.296 ms (limit 255 B)
  ```

  152 B against the 255 B limit (§3.3 confirmed), and **95.3 ms > the 88.2 ms
  burst slot** — D5 is now enforced by a test, not just argued in this document.
  A companion test asserts routine cover ops stay under 75 % of the slot, so
  fattening a common message gets caught.

> One design note surfaced while writing these: field numbers are deliberately
> **reused across the two message types** (10 is `operation` downlink and `avail`
> uplink; `timesync`=16 downlink pairs with `beacon`=16 uplink). Direction picks
> the parser, so this is safe and consistent with the existing schema — but it
> does mean a mis-routed frame mis-parses rather than failing cleanly. Worth
> knowing when debugging.

### 6.4 P1 result (2026-08-21) — built and tested, **not deployed**

**Node** (`CmdDispatcher.cpp`): `CMD_TIMESYNC` handler applies `settimeofday()`,
stores the UTC offset and `dstNext`, and marks the clock valid. Clock state
(`s_clock_valid`, `s_utc_offset_s`, `s_dst_next`) lives in `RTC_DATA_ATTR`
because plain RAM does not survive deep sleep — while the *clock itself* does,
since the ESP32's time base is the RTC timer. Exposed via
`CmdDispatcher::isClockValid() / getUtcOffset() / getDstNext() /
formatLocalTime()`; `isClockValid()` is the gate I8 will use in P3.

The handler logs the correction **before** applying it, so on a re-sync the log
line is the node's accumulated drift — which is the whole reason TimeSync ships
ahead of the scheduler: it characterises the crystal against real sleep cycles
on real hardware, turning D1 from a datasheet claim into a measurement.

Local time is rendered as `epoch + utcOffset` through `gmtime_r`, deliberately
avoiding `setenv("TZ")`/`tzset()` and the timezone database — entries are local
minutes-of-day and the hub has already resolved DST.

**Hub** (`lora_client.cpp`): `send_timesync()` pushes `epoch` +
`ESPTime::timezone_offset()`, deferred 750 ms after the session is confirmed so
it does not pile onto the just-completed login exchange. It is a **no-op when
the hub's own clock is invalid** — sending epoch 0 would make the node burn
awake radio time on a frame it must discard, and the next login retries anyway.

**Deliberately not acked.** Config pushes aren't acked either, and P2's beacon
carries `nodeEpoch` back, which is the designed way for the hub to observe
drift. An ACK here would spend battery on a redundant transmission.

**A protocol detail the harness had never modelled**, found by the first
downlink decrypt test: the GCM nonce counter carries a **direction bit** —
`counter = msgid | (1ULL << 63)` on downlink, bare `msgid` on uplink
(`kDownlinkNonceFlag`, defined identically in `lora_client.cpp` and
`CmdDispatcher.cpp`). Without it the tag check fails even though key, AAD and
base nonce are all correct. The sim now has
`derive_gcm_iv_uplink()`/`derive_gcm_iv_downlink()` so the direction is explicit
at every call site — P4's schedule push is a downlink and would have hit this
too. [SKILL.md](../SKILL.md) had documented the IV without the direction bit,
and its AAD (20 B) / tag (16 B) / full-message-ciphertext entries were also
stale; all corrected.

**Verification:** node builds (0x137ae0, 39 % free), hub compiles
(`config_hash=0x04280b55`), **87/87 tests pass** (9 new: 6 node-side covering
clock establishment, negative offsets, local-time rendering, `dstNext`, the
zero-epoch guard and the no-ack property; 3 hub-side covering the deferred push,
that it is encrypted, and that no push happens without a valid hub clock).

One thing the host tests deliberately do **not** assert: the actual wall clock.
`settimeofday()` needs `CAP_SYS_TIME` and fails for an unprivileged host user —
harmless, because every property the scheduler depends on is independent of
whether the host clock moved. **That gap is now closed on hardware** (below).

**Deployed and verified 2026-08-21 ~19:20.** Hub log shows the designed sequence
on both nodes — session confirmed, then `TimeSync sent` ~750 ms later with
`utcoffset=+7200`; the epoch decodes to exactly the log's own wall-clock time.
Node serial closes the loop: `App version: 1.0.13`, and
`CMD TIMESYNC: 2026-08-21 19:34:54 (UTC+2) — clock established`. It also
confirms D1's premise directly — `CLK: Using external 32kHz crystal`.

**Still outstanding for P1's real purpose:** the *drift* number. The node only
prints `drift correction ±N s` on a RE-sync, i.e. when it already had a clock.
The observed sync followed a power-on reset (cleared RTC memory), so it printed
"clock established" instead. Drift becomes visible either on the next hub reboot
while a node stays powered, or — far more usefully — via P2's beacon, which
carries `nodeEpoch` to the hub and turns drift into a Home Assistant sensor
rather than something only visible on a serial cable.

> **Do not read node serial casually.** Opening COM6 resets the node through the
> DTR/RTS auto-reset circuit, and its power-on path runs `MOTCMD_FULL_UP` to
> re-establish a position reference — so attaching a serial monitor physically
> drives that blind to the top.

### 6.5 P2 result (2026-08-21) — beacon done, I2's saving NOT done

**Delivered and tested (98/98):**

*Node* — `SYSCMD_BEACON` builds a `NodeWakeBeacon` carrying wake reason, node
epoch, mode, voltage, position, `sessionResume`, `clockValid` and `fwVersion`.
`classifyWakeReason()` reads the deep-sleep cause **before** the reset reason,
because a deep-sleep wake *is* `ESP_RST_DEEPSLEEP` — checking the reset reason
first would mislabel every scheduled wake as a boot. Crash-like resets (panic,
WDT, brownout) report `WAKE_UNKNOWN` rather than `WAKE_BOOT`, so a reset-looping
node is visible as such — that is precisely the signature of the earlier silent
battery outage. Queued on every boot, after any REGISTER (which resets counters;
a beacon queued before it would be dropped by the hub's own replay filter).

*Hub* — `handle_beacon_()` records the reported state and derives
`clock_offset = node_epoch − hub_epoch`, published through a new
`clock_offset` sensor on the `loracover` sensor platform. **This is what closes
P1's open question:** crystal drift becomes a Home Assistant number instead of
something only a serial cable reveals. Suppressed when the node reports
`clockValid = false`, otherwise a node that never got a TimeSync would show a
~56-year "drift" and make the sensor useless.

**NOT delivered — the node-side REGISTER-skip.** I2's actual battery saving is
the node choosing to send *only* a beacon on wake instead of running the full
REGISTER → config → login sequence (~4 s of awake radio, `kRegisterToLoginDelayMs`
alone). Two thirds of the machinery is already in place and verified:

- The hub sets `session_confirmed_` **and** `login_acked_` on any successful
  decrypt, so an encrypted beacon that decrypts *already* suppresses the login
  challenge — no hub change needed.
- If the hub has no nonce for that peer (it rebooted while the node slept), it
  logs `No base nonce for peer N — re-provisioning` and sends a
  `BaseNonceExchange`, which the node handles via `CMD_BASENONCE`.

What is missing is the node's boot-path decision to take that route. It is
deliberately left out rather than rushed: it changes the existing wake path,
whose failure mode is a node that never re-establishes and goes **silent** — the
worst outcome this system has. It needs its own cycle with an explicit fallback
timer (beacon, and if no downlink arrives within N seconds, fall back to
REGISTER) and a test for "hub rebooted while the node slept".

> **Version coupling:** `CmdDispatcher::kFirmwareVersion` (10013) is
> hand-maintained against `PROJECT_VER` in the node's `CMakeLists.txt`. A test
> asserts they match, so the next version bump will fail that test until both
> are updated — intentional, so the hub's capability gate can never report a
> version the node is not running.

---

## 7. Risks / consistency issues found while reviewing the existing code

| # | Issue | Handling |
|---|-------|----------|
| **R1** | ~~The nightly 23:00 `on_sleep_start` overrides the schedule.~~ **Downgraded — your call accepted.** In auto mode the node is asleep at 23:00 anyway, so the frame is simply lost; and `CMD_SLEEP` is reinterpreted in auto mode as "sleep to next event" (§4), so even a node that *is* awake does the right thing. The automation stays as-is for interactive mode. | **One residual that does need handling:** hub-side `enterSleep()` also sets `last_sleep_epoch_` and clears `login_acked_` / `session_confirmed_`. Clearing session state at 23:00 for an auto-mode node would defeat I2 — the hub would force a full re-login at the next beacon and burn the ~4 s we just saved. → `enterSleep()` must skip that bookkeeping for nodes in auto mode. |
| **R2** | `sleep_duration` (YAML + `ClientConfig`) becomes meaningless in auto mode. | Keep it — it remains the interactive-mode fallback and the legacy path. Document, don't remove. |
| **R3** | ~~Hub's awake model blocks auto mode until P4.~~ **Downgraded — your call accepted.** A wrong `is_node_awake_()` makes the hub send a login challenge into the void and retry on the existing backoff; the node is asleep with the radio off, so it costs the node nothing and nothing breaks. Auto mode can therefore go live at P3, and the schedule mirror becomes a P4 *optimisation* rather than a gate. | **Residual:** those futile retries are still transmissions on a shared 433 MHz channel and can collide with the *other* node's beacon window. With 2 nodes the odds are low; worth fixing at P4 as planned, not before. Retry backoff already reaches a 1 h ceiling, which bounds it. |
| **R4** | More wakes ⇒ faster `msgid` consumption; `kPersistTxMargin = 64` is reserved per NVS persist. | uint32 space is ample, but verify the NVS write *frequency* doesn't rise materially (flash wear) — a check-in every 6 h is ~4 writes/day, fine. |
| **R5** | Entries are **local** times; DST shifts them by an hour. | I7 (recommend accepting). Without it, up to a week of 1-hour error. |
| **R6** | A node in auto mode is unreachable between wakes — an HA command can sit for up to `checkin_interval`. | Inherent to radio-off deep sleep (the hub *cannot* wake it — §4). Mitigated by the check-in wake + the `config pending` sensor so the delay is visible rather than mysterious. |
| **R7** | Battery cost of auto mode is not yet measured. | P3 explicitly includes a drain measurement against the interactive baseline before we commit to defaults. If a wake cycle turns out expensive, `checkin_interval` is the tuning knob. |

---

## 8. Decisions (Q5–Q9 — all answered, as proposed)

**Q5 — Day-mask UI:** one `select` per slot with presets (`Daily / Mon–Fri /
Sat+Sun / Mon–Sat / Sunday only / Off`); arbitrary masks remain expressible in
YAML. **4 entities per slot**, not 11.

**Q6 — Conflicts:** lowest slot index wins on a tie; an event that fires while
the motor is already moving is **dropped and logged**, not queued.

**Q7 — Battery reporting in auto mode:** the periodic `battery_update_interval`
timer is disabled; battery is carried in the **beacon**, i.e. reported only when
the node wakes (≥ every `checkin_interval`).

**Q8 — Mode switch scope:** per node, matching every other entity in the system.
An HA helper/script can drive "all nodes" if wanted.

**Q9 — Default mode with no schedule:** INTERACTIVE. Auto mode engages only once
a `ScheduleConfig` with ≥1 enabled entry has been pushed. This is also what makes
the node-first deploy order safe (§6.1) — a new node against an old hub can never
put itself to sleep.

---

## 9. Ready to execute

No open decisions remain. Plan is:

1. **P0 first, and stop there for a verification round** — specifically the
   unknown-field / empty-`oneof` behaviour called out in §6.1. Everything else
   rests on it.
2. Then P1 → P5 in order, each deploying **nodes first, hub second** (D6).
3. Battery drain measured at P3 against the interactive baseline before auto mode
   is left running unattended (R7).

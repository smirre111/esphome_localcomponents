# LoRa Blinds — Fix / Hardening Plan

Remaining open items. The 2026-07-12 stability audit's P1 reset bugs
(`taskLoraRx` semaphore hang, `taskDeepSleep` WDT starvation, ADC panic, msgid
desync) and its P2 items (battery 0 V race, config-sync, hub stale-session
self-heal) shipped in v1.0.8–v1.0.9 + hub OTA. **v1.0.11 (2026-07-27)** shipped
the remaining P1 tracked/acked sysops (OTA), the P3 cross-talk pre-decrypt filter
+ log downgrade, and the P3 deep-sleep drain-wait hard cap.  **All nodes are on
the latest firmware.** See [activity-log.md](activity-log.md).

Node changes need a rebuild (IDF 6.0) + reflash or HTTPS-OTA; hub changes are an
`esphome` OTA.

Priority: **P1** = can reset/brick the node. **P2** = wrong data or silent
failure. **P3** = latent / cleanup.

---

## P2 — config-hash variant of config-sync (better than the shipped bool)

- The shipped simple variant sets `request_register` whenever `!config_synced_`,
  pushing config once per hub boot regardless of whether config actually changed.
- **Better:** the hub sends a config version/hash in the login; the node echoes
  the hash it last applied; the hub requests a register only when they differ —
  so config is pushed only on a *real* change, and wake can become login-first /
  register-only-if-changed (skips the ~1.5 s config bursts on an unchanged wake).
- Also closes the resume-path nuance more cleanly than the current deferred
  re-login guarantee.
- Cost: new `LoginMsg` hash field (regen stubs → 4 locations) + hub/node changes.

## P2 — battery-silence gap (investigation)

- 2026-07-12: ~86 min with no battery messages, then activity. An awake node
  force-sends every 5 min, so a long silence ⇒ deep-asleep, reset-looping, or a
  dead/blocked task. The 0 V race that accompanied it is fixed, but the silence
  window itself was never explained.
- **Diagnostic:** correlate future gaps with the node reset reason
  (`esp_reset_reason`) + sleep logs on serial; watch the hub log for a
  REGISTER / session-confirm at the gap's end (a wake/boot marker).

---

## Clean (audited, no action)

- Heap: crypto `malloc`s matched by `free()` on all branches; protobuf
  `__unpack`/`__free_unpacked` balanced per exit path.
- No unbounded containers (only bounded ADC averaging).
- All WDT-subscribed tasks feed correctly: `LoraInterface` (per-retry + loop
  resets), `MotorCtrl`, and `taskDeepSleep` (loop-top reset). Idle-task watching
  disabled (`idle_core_mask=0`), so only explicitly-added tasks can trip the WDT.

## Shipped (was open in this plan)

- **P1 — one-shot sysops (CMD_OTA) now tracked/acked** (v1.0.11): OTA rides the
  same retransmit-until-acked path as cover ops (`tx_tracked_op_` /
  `send_tracked_sysop_`); the node acks every sysop. A dropped OTA frame now
  recovers instead of being lost.
- **P3 — inter-node cross-talk decrypt noise** (v1.0.11): the node drops foreign
  encrypted frames by destination address *before* decrypting; the hub's
  equivalent check already preceded decrypt and its `Address not for me` line was
  downgraded from `E` to `D`.
- **P3 — `checkQueuesIdle` hard cap** (v1.0.11): the deep-sleep drain-wait is
  capped at the motor's absolute max run-time (120 s); past that it logs and
  sleeps anyway instead of looping forever.

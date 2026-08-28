# Architecture review — node + hub

Structural review of both codebases, 2026-08-28. **Scope: architecture,
encapsulation and obvious waste.** Not a line-by-line audit of all 19,670 lines,
and nothing here is profiled — "efficient" below means structure, not measured
performance.

## Size

| | Lines | Largest files |
|---|---|---|
| Node (`BlindsESP/main`) | 12,257 | `CmdDispatcher.cpp` 2,876 · `SystemCtrl.cpp` 2,105 · `MotorCtrl.cpp` 1,087 |
| Hub (`local_components`) | 7,413 | `lora_client.cpp` 1,810 · `lora.cpp` 1,339 · `lora_tracker.cpp` 758 |

`CmdDispatcher.cpp` alone is 23% of the node.

---

## 1. CmdDispatcher is a god object — the headline finding

2,876 lines, **78 public declarations**, and at least ten distinct
responsibilities:

| Responsibility | Examples |
|---|---|
| LoRa frame receive + dispatch | `onReceiveNew`, address filter, replay filter |
| AES-GCM crypto | key import, IV derivation, encrypt/decrypt |
| Replay protection | `rx_message_id_` / `tx_message_id_` |
| Session persistence | `savePersistentState`, `loadPersistentState`, NVS |
| Wake classification | `classifyWakeReason` |
| Automatic-mode policy | `shouldRunAutoMode`, `computeSleepSeconds`, `runDueScheduleEntry` |
| Interactive override (D4) | `enterInteractiveMode`, `isTemporarilyInteractive` |
| Six independent timers | register retry, resume fallback, clock retry, beacon retry, auto sleep, interactive end |
| Motor command forwarding | `setMotorCommand`, `setBlindOperation` |
| Telemetry | beacon, position, battery |

**This is not a style complaint — it is where the bugs came from.** Every
hard-to-find defect this month lived in a *seam between two of these
responsibilities*, not inside any one of them:

* **F8** — sleep policy fired before the session handshake finished.
* **F10** — schedule execution and sleep scheduling disagreed about when an
  entry was due.
* **F15** — the resume path was gated on a provisioning flag owned by a
  different concern.
* **F19** — sleep policy and session lifetime were entangled.

A god object has no seams you can see; you only find them by tripping over them
on hardware at 06:00.

### The counter-example that proves the fix works

`BootPolicy.h` was extracted precisely this way: pure, dependency-free
decisions, no ESP-IDF includes. It compiles directly into the host harness, is
mutation-verified, and caught a real ordering bug (the latched marker) *before*
it shipped. That is the model.

### Suggested split (incremental, not a rewrite)

1. **`SessionManager`** — crypto, msgid counters, base nonce, NVS persistence.
   Currently the most tangled and the source of F15/F19.
2. **`AutoModePolicy`** — `shouldRunAutoMode`, `computeSleepSeconds`,
   `runDueScheduleEntry`, and the sleep/clock/beacon timers. Mostly pure
   decisions over (clock, schedule, mode) — the same shape as `BootPolicy.h`,
   so testable without hardware.
3. **`Telemetry`** — beacon/position/battery assembly.

Do them one at a time, each behind the existing test suite. The value is not
tidiness; it is that policy becomes testable on the host instead of on a roof.

---

## 2. Encapsulation

* **Public mutable state.** `destAddress` (CmdDispatcher.h:268) and the FreeRTOS
  queues are public. The tests reach directly into `rxCmdQueueNew`,
  `txCmdQueueNew`, `sysCmdQueueNew` — convenient, but it means the queue layout
  is now part of the public contract.
* **Two `public:` sections** (lines 34 and 380) with `protected:` between them,
  so the interface is not readable in one pass.
* **Accessor asymmetry.** `get_base_nonce` is protected while `destAddress` is
  public, though the two are used together. This blocked a test during the F19
  work and pushed it toward asserting behaviour instead — which turned out
  better, but by accident.

`get_base_nonce()` also returns **true for any peer entry that exists, even with
a zero nonce**, so "we have a session" is satisfied by one that cannot decrypt
anything. That is a latent correctness bug, not just an interface wart.

---

## 3. Dead code — 784 commented-out lines on the node

| File | Commented-out | Share |
|---|---|---|
| `mybutton.cpp` | 128 | 27% |
| `Buttons.cpp` | 79 | 20% |
| `frtosTasks.cpp` / `utilities.cpp` | 64 / 62 | 13% |
| `MotorCtrl.cpp` | 116 | 10% |
| `CmdDispatcher.cpp` | 162 | 5% |

Git holds the history; the comments hold only doubt. They actively mislead —
during this review `motorBusyFlag` looked like live state and was in fact a
member whose every use was commented out (since deleted).

**Naming:** `mybutton.cpp` holds the callback bodies and `Buttons.cpp` the
registration. That is a reasonable split under two unhelpful names
(`ButtonActions.cpp` / `ButtonSetup.cpp` would say it).

---

## 4. Duplicated definitions

**Buffer constants defined twice, independently:**

```
include/CmdDispatcher.h:29   POOL_SIZE = 5      BUFFER_SIZE = 256
include/LoraInterface.h:23   POOL_SIZE = 5      BUFFER_SIZE = 256
```

Two 1,280-byte pools sized by two constants that must agree and are not linked.
If one is raised for a larger frame and the other is not, the result is a
buffer overrun in the layer that was not updated. They belong in `common.h`.

**Generated protobuf stubs exist in four hand-synced source locations:**

```
BlindsESP/proto/blinds.pb-c.{c,h}                    (regenerate here)
BlindsESP/components/blinds/src|include/blinds.pb-c.*  (what the node links)
BlindsESP/main/include/blinds.pb-c.h
configuration/local_components/blindsproto/blinds.pb-c.*
```

Missing one produces a **linker** error while everything still compiles — a
failure mode already documented as having cost real time. This should be one
generated location plus include paths, or a build step that copies them.

---

## 5. The recurring defect shape

Seven instances found this month, all identical in form: **a value maintained in
parallel with the thing it describes.**

`kFirmwareVersion` vs `PROJECT_VER` · RTC mode marker vs the running mode ·
`state_.position_` vs `m_Position` · `lastBatteryVoltage_` vs the battery ·
`registerd_lora` vs "is provisioned" · `timeSyncDone` vs TimeSync state ·
`motorBusyFlag` vs the motor FSM.

Three were found only by mutating them; the tests were green throughout. The
architectural remedy is the same each time — **derive, don't mirror** — and it
is worth applying as a review question rather than waiting for the eighth.

---

## 6. Resources

* **Eight FreeRTOS tasks**, ~50 KB of stack (5120 + 6144 + 4096 + 6144 + 8192 ×3
  + 4096). Comfortable on ESP32 RAM, but eight contexts on a node whose duty
  cycle is ~40 s per six hours is a lot of machinery for the work done.
* **Drain-wait granularity.** `enterDeepSleepTask` polls every 3 s, so the node
  stays awake up to ~3 s past the motor stopping. Four wakes a day makes this
  negligible; worth knowing, not worth fixing.
* **Airtime is dominated by the 17-copy downlink burst**, which is deliberate —
  the node's receiver is windowed. Not waste.

---

## Priority

| | Change | Why |
|---|---|---|
| 1 | Delete the 784 commented-out lines | Zero risk, immediate legibility, removes false state |
| 2 | Single-source `POOL_SIZE` / `BUFFER_SIZE` | Silent buffer-overrun risk |
| 3 | Fix `get_base_nonce()` zero-nonce acceptance | Latent correctness bug |
| 4 | Extract `AutoModePolicy` | Biggest testability win; the `BootPolicy.h` pattern already proved it |
| 5 | Single-source the proto stubs | Removes a known linker-error trap |
| 6 | Extract `SessionManager` | Largest and riskiest; do last, behind the suite |

1–3 are safe and quick. 4 and 6 are the real architectural work and should each
be a separate change with the full suite green between them.

# MAC-0 final verification campaign — all three modes, ONE build

**Why this exists.** MAC-0 has passed in all three modes, but never on the same
firmware. Mode B passed on fw **1.1.2** (2026-09-21); Mode A and Mode C last
passed on **1.0.94** (2026-09-16). Between those builds the node retired the
`MissedMarks` demotion, made the beacon heartbeat (`SyncStale`) the sole
link-health criterion, added the `WAKE_MODE_CHANGED` announcement, and reordered
`timedRxActive()`. Each mode's last pass therefore describes a firmware that no
longer exists.

**The single property this campaign establishes:** *A, B and C all pass on one
build, measured in one sitting, with nothing reflashed in between.*

**Node 2 already runs 1.1.2, so no node flash is required.** That is the point —
reflashing mid-campaign would destroy the property being established.

`bench-runbook.md` remains the authority on procedure and `test-plan.md` §10.6 on
method. This document only sequences them and states what closes each mode.

---

## Step 0 — preconditions, done ONCE before any run

Do every hub change here, together, and upload **once**. A hub upload midway
through the campaign resets `Timed Mode` to OFF (`restore_mode: ALWAYS_OFF`),
which between Mode B and Mode C would silently void the run.

| # | action | why |
|---|---|---|
| 0.1 | Confirm the node runs the build under test — beacon `fw=10102` for 1.1.2, or the boot banner's `App version:` | `app-flash` is a silent no-op on the boot slot for a node that has OTA'd |
| 0.2 | Confirm `rtcSlowSrc` reads **2** (external crystal) from a ModeTest report | Mode B is gated on it; the RC oscillator (~5 %) keeps the node in Mode A permanently |
| 0.3 | In `loradevices.yml`, set node 2 `checkin_interval: 15min` | Mode C needs it: each check-in is one wake-clock sample and one Class A funnel report. It is harmless during A and B — an interactive node never sleeps, so no check-in fires |
| 0.4 | If the campaign could run past 23:00, comment out `- loracover.on_sleep_start: rol_2` (`loradevices.yml`, the `time:`/`on_time:` block) | It deep-sleeps the bench node mid-run. It killed a 900 s run at 869 s on 2026-09-14 and the run reported nothing. **Restore it afterwards** |
| 0.5 | Compile and upload the hub, once | Any later upload resets Timed Mode |
| 0.6 | Wait out or re-apply anything set in the 30 min before the upload | `preferences: flash_write_interval: 30min` — settings changed inside that window are lost on reboot |
| 0.7 | Note the hub commit and node fw in the log you keep | Every number below is meaningless without the pair |

**Do not flash the hub and a node at the same time.** No node flash is needed
here at all.

---

## Step 1 — Mode A (≈17 min)

**Precondition: `Timed Mode (Mode B) — node 2` = OFF.** Mode A means no grid.

Press **`Mode Test A — long, MAC-0 baseline (node 2)`**
→ `start_mode_test(900, 1093, 1, 1, true, false, false, true)`
(900 s, grid 1093 ms, mode A, 1 copy, production profile, counter off, crypto
off, macEcho on).

**1093 ms, not 1100.** The grid period must not phase-lock against the node's
free-running listen interval. 1093 gives `gcd = 1 ms`, so the phase walks 93 ms
per mark and sweeps every residue. The node refuses the phase-locked case
outright (`ModeTestPolicy.h periodsPhaseLock`) rather than measuring an empty
denominator.

| read | pass condition |
|---|---|
| `mode actually run` | **1** — derived from what the node applied, never echoed from the request |
| `arm refusal` | **0** |
| true FER (CRC-valid → addressed) | **0**, quoted with its sample count and 95 % upper bound (≈ 3/n) |
| raw ppm | reported, **no pass line** — Mode A has no residual |
| `ppmSamples` | **≥ 30 over ≥ 200 s** |
| WMR | not applicable — Mode A arms no timed windows |

**`FER_link` ≈ 93 % here is listening duty, not loss.** An unpromoted node
listens ~6 % of the time. Reading it as loss has misled this project before.

---

## Step 2 — Mode B (≈17 min)

**Preconditions:**
- `Timed Mode (Mode B) — node 2` = **ON**.
- **The grid must be adopted on a SETTLED node clock** — more than 60 s after
  boot, past the crystal recalibration. Measured 2026-09-21: a provisional
  anchor cost **85 ms** of phase tail (p99 88 276 µs against 925 µs settled). If
  the node has just booted, withdraw and re-publish the grid (Timed Mode off/on)
  after the first minute.

Press **`Mode Test B — long, production profile (node 2)`**
→ `start_mode_test(900, 1500, 2, 1, true, false, false, true)`.

**In Mode B the incommensurability rule inverts:** the period must be exactly the
round (1500 ms) and phase-locked to the node's slot. That *is* the mode.

**One copy, not eight.** The 8-copy bootstrap burst is for promoting a cold node
only; it adds ~2.7 ms of phase tail and ~39 000 ppm DUP. Promotion from a
single-copy run takes 12–15.7 s via the promotion trial.

The four-clause pass line, all of which must hold:

| clause | pass when |
|---|---|
| **clock rate** | **\|residual ppm\| < 20**, node vs hub, `prod=1` |
| **mode engaged** | `mode actually run` = **2**, `arm refusal` = **0**, `windows armed` > 0 |
| **HW-8** | `oneShot n` ≥ `windows armed` → **0 missed one-shots** |
| **sample quality** | `ppmSamples` ≥ **30** over ≥ **200 s** |

Also record: `windows armed / hit`, WMR, `phaseErr p50/p99/max` against the
±14 080 µs guard, `armResidual p99`, true FER.

---

## Step 3 — Mode C (≈50 min)

**Run it straight after Mode B, not overnight** — that is how the 2026-09-16 pass
was taken, and it keeps the build and conditions identical.

**Preconditions, both load-bearing:**
1. **Withdraw the grid first: `Timed Mode (Mode B) — node 2` = OFF.** Otherwise
   the hub places marks and beacons at a node that is asleep.
2. **Do not open the node's serial port.** Opening COM6 toggles DTR/RTS and
   **resets a sleeping node**, which is precisely the thing under test. Mode C is
   measured **hub-side only**.

Turn **`RollladenWohnzimmer2 Auto Mode`** ON. Use the **switch**, not the YAML:
`auto_mode:` in `loradevices.yml` is only a first-boot seed — once a schedule is
stored, `load_schedule_()` restores the persisted mode and logs "YAML seed
ignored".

Then wait for **at least three check-ins** at the 15 min pace (~45–50 min). The
first is the transition wake (the node was awake and interactive when AUTO was
pushed) and behaves differently from the rest.

| read (hub log and beacons) | pass condition |
|---|---|
| check-ins heard / expected | **all heard**, each `reason=TIMER_CHECKIN reset=DEEPSLEEP fw=<build>` |
| **wake-timing error** | **\|ppm\| < 20** — the pass line, same threshold as Mode B |
| session | **`resume=1`** on every wake — no re-login, no REGISTER |
| true FER over the auto wakes | **0**, with its sample count |
| wake-clock counter rate | recorded, not gated (≈ −128 to −132 ppm; the crystal is ≈ −139 ppm) |
| Class A funnel per wake | `prevwake` windows / hits / detected / crcValid, recorded |

A rate needs **two paired samples**, so `Wake timing:` does not appear until the
second check-in.

**Known limitation, state it rather than resolve it:** this run cannot separate
"a window armed after an uplink the hub did not answer" (legitimately empty) from
"armed and missed". That needs the node's own funnel, and the node's serial port
is exactly what a Mode C run may not touch.

When finished: **Auto Mode OFF**, restore `checkin_interval: 1h`, restore the
23:00 sleep line, and upload the hub once.

---

## Capturing the numbers

**`ppm`, `residual`, `Wake clock:` and `Wake timing:` exist ONLY in the hub's log
line — never as sensors.** A run whose report was not captured has not been
measured; this was lost once on 2026-09-21 and cost a whole 300 s run.

```bash
# start the run, then capture across its END
timeout 380 curl -s -N http://192.168.178.91/events > run.txt
grep -aoE "ModeTest REPORT[^\\]{0,320}" run.txt
grep -aoE "oneShot p99[^\\]{0,200}" run.txt
grep -aoE "(Wake clock|Wake timing)[^\\]{0,200}" run.txt
grep -aoE "Beacon: reason=[A-Z_]+[^\\]{0,110}" run.txt
```

**The report line truncates at 512 bytes** and has cut `oneShot` mid-field. So
read the sensors as well — `Mode Test — mode actually run / arm refusal / windows
armed / windows hit / FER link / window miss rate / phase error p99` survive
independently of the log.

**Do not read the node's state from the hub's `Timed RX` sensors.** The hub
cannot see a promoted node that has nothing to say (see below): those sensors sit
at `5 / 0 / 7` throughout a healthy Mode B run. Read the **ModeTest report** for
the node's state.

---

## What a clean sweep closes, and what it does not

**Closes:** MAC-0 in all three modes on one build — the property that has never
held. On success, record it in `implementation-plan.md` §12 with the hub commit
and node fw, and update the `bench-runbook.md` status table.

**Does NOT close, and must not be implied by a green campaign:**

* **The promoted-quiet-node visibility gap.** A `PhaseReport` rides an uplink, and
  a promoted node with nothing to say never sends one, so `NoPhaseReport` keeps
  single-shot refused for exactly the node Mode B exists for. Confirmed three
  times on 2026-09-21 across two firmwares and two copy counts. This needs a
  design decision (a carrier), not a measurement.
* **HW-8's strength.** `oneShot n` counts Grid **and** Beacon arms, so the
  difference goes negative on a healthy run and a run that truly lost three
  one-shots would compute 0 and read as perfect. A like-for-like counter does not
  exist. Record HW-8 as passing on weaker evidence than its gate asks for.
* **HW-2** (blocked on the per-window extension gate), **HW-1 / HW-4** (need a
  scope and a current meter), **HW-3** (needs the three-condition distribution
  with the motor running).
* **The provisional-anchor result.** The 85 ms figure rests on three runs, not a
  controlled experiment. A deliberate boot-minute grid adoption would confirm it.

---

## Time budget

| phase | duration |
|---|---|
| Step 0, including one hub upload | ~10 min |
| Mode A | ~17 min |
| Mode B | ~17 min |
| Mode C | ~50 min |
| **total** | **~1 h 35 m** |

Start with margin before **23:00**, or disable the sleep line at 0.4. A campaign
interrupted by the nightly sleep produces no report at all.

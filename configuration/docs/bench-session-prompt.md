# Prompt for the hardware-measurement session

Paste the block below as the **first message** of a fresh Claude Code session.

It is deliberately short. Everything it needs to know is in the repo —
`bench-runbook.md` is the procedure — so this only has to point there, set the
posture, and name the things the session cannot work out for itself.

**Before pasting, answer the two questions at the bottom of the block** (B0, and
which instruments are on the bench). If you leave them, the session will ask,
which is fine but wastes a turn.

**Last updated 2026-09-21.** If the status list below disagrees with
`bench-runbook.md`, the runbook wins — it sits next to the code.

---

```text
Bench session for the LoRa blinds project. The hardware is set up and ready.

Repos, both on branch `main`:
  github.com/smirre111/esphome_localcomponents   — the hub (ESPHome components)
  github.com/smirre111/blindsesp                 — the node firmware (ESP-IDF)

Node 2 (the bench node) runs fw 1.1.2. The host suite is green — run it and
take whatever count ctest reports as the baseline; do not trust a number
written in a document.

START HERE, in this order:
  0. configuration/docs/mac0-final-campaign.md — IF the task is the MAC-0 final
     verification. A, B and C on ONE build in one sitting, with the
     preconditions, the pass line for each mode, and what a green sweep does
     NOT close. It sequences the runbook; it does not replace it. Node 2
     already runs the build under test, so do not reflash to start it.
  1. configuration/docs/bench-runbook.md   — the procedure and the CURRENT
     status of every HW item. Follow it; do not re-derive it.
  2. configuration/docs/test-plan.md §10.6 — authoritative on METHOD.
  3. configuration/docs/implementation-plan.md §12 and §0 — authoritative on
     WHY each number matters and what rests on it.

WHERE THE TEN MEASUREMENTS STAND (runbook's table is authoritative):
  CLOSED   HW-5 (ppm 10, n 598, residual -1), HW-9 (node 100 Hz / hub 1000 Hz),
           HW-10 (RX-stamp high-water 191 611 us)
  WEAK     HW-8 passes, but on weaker evidence than its own gate asks for —
           no miss count exists in either direction. See review finding 12.
  BLOCKED  HW-2 on a per-window extension gate (see the DISABLED_ witness in
           frtos_tasks_test.cpp); HW-7 on unimplemented `payloadPadTo`
  OPEN     HW-3 (needs the three-condition distribution, motor running);
           HW-1 and HW-4 need a scope and a current meter
  N/A      HW-6 is a deployment decision, not a measurement

Ground rules I care about:

- Run the runbook's §0 preconditions before flashing anything. The node must be
  built WITH CONFIG_BLINDS_BENCH_NODE or HW-2 cannot run at all, and
  rtcSlowSrc must read 2 (external crystal) — confirm from the beacon, not the
  schematic.
- NEVER COMMIT THE BENCH FLAGS. CONFIG_BLINDS_BENCH_NODE,
  CONFIG_BLINDS_BENCH_LNA_GAIN and CONFIG_BLINDS_RX_WINDOW_SINGLE live in the
  working-tree sdkconfig only. Stage files by name; never `git add -a`.
- NEVER FLASH THE HUB AND A NODE AT THE SAME TIME.
- After flashing a node, READ THE WHOLE BOOT LOG, not just the version line.
  It must show "Loaded app from partition at offset 0x20000" (erase otadata at
  0x630000 first or app-flash is a silent no-op on a node that has OTA'd) AND
  no warnings. A correct-looking banner hid a broken build on 2026-09-20.
- PROJECT_VER fields are limited to 0-99 each; after x.y.99 bump the MINOR
  field. A version the node cannot parse is reported as fw 0, which makes the
  hub refuse single-shot forever and burst 17 copies for every downlink. A
  configure-time guard now enforces this.
- The 23:00 nightly sleep is ACTIVE for node 2 again. It deep-sleeps the bench
  node mid-run — it killed a 900 s run at 869 s. Start long runs with margin,
  or comment the line out in loradevices.yml for the duration and restore it.
- Read `mode actually run` and `arm refusal` before any other number in a
  ModeTest report. Mode used to be echoed from the request rather than derived
  from what the node applied, so a run can look like Mode B and be Mode A.
- Read `windows armed` next, and the `RX windows skipped — radio busy` sensor
  alongside WMR. A mark that was never armed is invisible to WMR by
  construction, so a wedged radio reports a flawless miss rate while hearing
  nothing.
- Read the `Timed RX — demotion reason` sensor before pressing a sweep.
  Anything but 0 voids the point whatever the offset. Live reasons are now
  1 GridDisabled, 2 BadClockSource, 4 SyncStale, 5 NoPhase; 3 and 7 are
  retired and never returned.
- The node now TELLS the hub when its mode changes (`WAKE_MODE_CHANGED`, node
  1.1.2). A reason change spends one uplink, throttled to 60 s, and the hub's
  `Timed RX — demotion reason` follows it within a beacon. Before this the hub's
  view of a quiet node could lag arbitrarily. If that sensor looks stuck,
  confirm the node is on 1.1.2 or later before believing what it says.
- The MAC ping CANNOT promote a cold node — it is a turnaround instrument, not
  a promotion one. Measured 2026-09-21: 190 placed pings at 1100 ms over 280 s
  promoted nothing and exhausted the transmit pool (68 frames refused, placed
  frames dropped with their mark unconsumed). Promote with `Mode Test B —
  bootstrap burst`; every promotion on record (12 / 13.4 / 15.69 s) came from a
  ModeTest placing one mark per round. Note a ModeTest RESETS the node's phase
  accumulators when it ends, so the hub's `Phase — samples` returns to 0.
- Report what the instrument said, including when it contradicts the plan.
  Several numbers in the plan are assumptions whose stated provenance is "never
  measured". If the bench disagrees, the plan is what changes.
- As each number lands, write it into the plan where its assumption lives,
  replacing the assumption rather than appending near it. Then turn the
  assertions that were waiting on that number into real tests.
- Keep the host suite green. Commit per measurement with the raw numbers in the
  commit message, and push to `main`.

One thing to verify early, because it is unverified and everything battery-
related depends on it: `kBattTrimFactor` in the node's `main/frtosTasks.cpp` is
1.0 and has never been checked against real hardware. Compare the
`Battery: raw=… -> …V` log line against a multimeter at the pack terminals and
adjust it if the divider resistors are off nominal. The conversion itself was
fixed on 2026-09-11 — it used to read ~11 % low — so do not trust a battery
number until this is done.

Do not book bench time for HW-6; it is a deployment decision, not a measurement.

Two things I need to tell you rather than have you infer them:
  - B0 (the GPIO light-sleep wake source) is / is not on the build under test:
    ______________.  HW-5's ppm figure is worthless without it.
  - Instruments on the bench today: ______________.  HW-1's mean needs a scope
    and HW-4 needs a current meter; the other eight close on-node. If an
    instrument is absent, do the on-node part and defer the rest explicitly
    rather than approximating.
```

---

## Notes on the prompt, for whoever maintains it

**Why it does not list the ten measurements in detail.** The runbook does, with
the method and the closing condition for each. Duplicating them here would
create a second copy to drift, which is the failure mode both plans are
organised against. The one-line status summary is the exception: a session needs
to know what is already closed before it plans its day, and that is cheap to
keep current. If it goes stale, delete it rather than let it mislead.

**Why HW-8 gets special billing.** It is the only item whose failure invalidates
other items. Everything else can be taken in any order without silently
corrupting a neighbour. Note it currently passes on weaker evidence than the
gate asks for (review finding 12) — treat that as unfinished, not as closed.

**Why `kBattTrimFactor` is called out by name.** It is the one number the repo
knows is unverified and cannot verify itself, and it scales every battery
reading. The conversion around it was wrong by ~11 % until it was fixed, so a
plausible-looking voltage is not evidence that the trim is right.

**Why the boot-log rule is stated so bluntly.** On 2026-09-20 a node was built,
committed and flashed with `PROJECT_VER 1.0.100`. The banner read
`App version: 1.0.100` and looked perfect; four lines later the log said
`Could not parse firmware version '1.0.100' - reporting 0`. A node reporting 0
is `FirmwareUnknown` to the hub, which then bursts 17 copies for every downlink
— Mode B's entire airtime saving, silently off, with a symptom that looks like
"Mode B won't engage". The host suite was green throughout, because it asserts
that 1.0.100 is unparseable and nothing connected that assertion to the version
actually being shipped.

**Why the demotion-reason rule changed.** `MissedMarks` (3) and
`RecentlyDemoted` (7) were retired on 2026-09-20. `MissedMarks` counted hub
SILENCE rather than link health — the hub publishes an all-listening pending
mask, so every node arms every round and three legitimately empty windows,
4.5 s of ordinary quiet, demoted a healthy node. The 600 s anti-flap hold
existed only to rate-limit the resulting flap, so it went with it. Demotion now
rests on the beacon heartbeat (`SyncStale`, ~5.9 min). Sweeps therefore no
longer need a pre-wait to sit out a hold.

**Why the announcement rule earns a line.** The mechanism shipped in node 1.1.1
and did not work: `timedRxActive()` returned early on `!timed_rx_enabled_`
*before* the edge was detected, so a withdrawn grid — the transition the hub most
needs — could never be announced. The node announced once at boot and was silent
for the rest of its life. Four mutation-killed witnesses missed it because every
one of them ran with timed RX ENABLED, so the early-returning path was never
entered; one even exercised the same REASON by a different PATH, which made the
coverage look complete. Fixed in 1.1.2 and confirmed on hardware
(`Beacon: reason=MODE_CHANGED … fw=10102`, hub reason tracking to 1). The general
lesson, which applies to every guard clause here: **a mutant only shows an
assertion is load-bearing for the lines the tests REACH — it says nothing about a
path no test visits.** Prove a regression test red-first.

**Why the two blanks exist.** Both are facts about the physical bench that no
amount of reading the repo will establish, and both change what the session
should do rather than merely how it reports. A session that guesses at B0 will
produce a ppm figure that looks fine and means nothing.

**If the runbook and this prompt ever disagree, the runbook wins** — it sits
next to the code it describes and is the thing a session actually reads.

# Prompt for the hardware-measurement session

Paste the block below as the **first message** of a fresh Claude Code session.

It is deliberately short. Everything it needs to know is in the repo —
`bench-runbook.md` is the procedure — so this only has to point there, set the
posture, and name the two things the session cannot work out for itself.

**Before pasting, answer the two questions at the bottom of the block** (B0, and
which instruments are on the bench). If you leave them, the session will ask,
which is fine but wastes a turn.

---

```text
Bench session for the LoRa blinds project. The hardware is set up and ready.

Repos, both on branch `main`:
  github.com/smirre111/esphome_localcomponents   — the hub (ESPHome components)
  github.com/smirre111/blindsesp                 — the node firmware (ESP-IDF)

`main` carries everything: the protocol work, 927 green host tests, ModeTest,
Mode B, and the battery-voltage ADC fixes. Nothing needs merging first.

START HERE, in this order:
  1. configuration/docs/bench-runbook.md   — the procedure for all ten
     measurements. Follow it; do not re-derive it.
  2. configuration/docs/test-plan.md §10.6 — authoritative on METHOD.
  3. configuration/docs/implementation-plan.md §12 and §0 — authoritative on
     WHY each number matters and what rests on it.

Goal: work HW-1 through HW-10 in the runbook's order and record the results.
Nothing in this project has ever been measured on hardware, so every number is
new and every timing figure in the plan is still an assumption.

Ground rules I care about:

- Run the runbook's §0 preconditions before flashing anything. In particular the
  node must be built WITH CONFIG_BLINDS_BENCH_NODE or HW-2 cannot run at all,
  and rtcSlowSrc must read 2 (external crystal) — confirm that from the beacon,
  not from the schematic.
- Read `mode actually run` and `arm refusal` before any other number in a
  ModeTest report. Mode used to be echoed from the request rather than derived
  from what the node applied, so a run can look like Mode B and be Mode A.
- Read `windows armed` next, and the `RX windows skipped — radio busy` sensor
  alongside WMR. A mark that was never armed is invisible to WMR by
  construction, so a wedged radio reports a flawless miss rate while hearing
  nothing.
- Report what the instrument said, including when it contradicts the plan.
  Several numbers in the plan are assumptions whose stated provenance is "never
  measured". If the bench disagrees, the plan is what changes.
- HW-8 is the early gate: a single missed one-shot wake is a FAILED gate, not an
  outlier, because every armed window in Mode B and Class A depends on that
  mechanism. If it fails, stop and tell me — HW-2, HW-3 and B3 would all be
  measuring something else.
- As each number lands, write it into the plan where its assumption lives,
  replacing the assumption rather than appending near it. Then turn the
  assertions that were waiting on that number into real tests.
- Keep the host suite green (927 tests). Commit per measurement with the raw
  numbers in the commit message, and push to `main`.

One thing to verify early, because it is unverified and everything battery-
related depends on it: `kBattTrimFactor` in the node's `main/frtosTasks.cpp` is
1.0 and has never been checked against real hardware. Compare the
`Battery: raw=… -> …V` log line against a multimeter at the pack terminals and
adjust it if the divider resistors are off nominal. The conversion itself was
just fixed — it used to read ~11 % low — so do not trust a battery number until
this is done.

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

**Why it does not list the ten measurements.** The runbook does, with the method
and the closing condition for each. Duplicating them here would create a second
copy to drift, which is the failure mode both plans are organised against.

**Why HW-8 gets special billing.** It is the only item whose failure invalidates
other items. Everything else can be taken in any order without silently
corrupting a neighbour.

**Why `kBattTrimFactor` is called out by name.** It is the one number the repo
knows is unverified and cannot verify itself, and it scales every battery
reading. The conversion around it was wrong by ~11 % until 2026-09-21, so a
plausible-looking voltage is not evidence that the trim is right.

**Why the two blanks exist.** Both are facts about the physical bench that no
amount of reading the repo will establish, and both change what the session
should do rather than merely how it reports. A session that guesses at B0 will
produce a ppm figure that looks fine and means nothing.

**If the runbook and this prompt ever disagree, the runbook wins** — it sits
next to the code it describes and is the thing a session actually reads.

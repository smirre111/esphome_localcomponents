# Power and RF-efficiency review — 2026-09-27

Requested by the user: a focused pass on node battery life and RF efficiency —
keep RF on-time (message count, burst size) and CPU awake time as low as
reliability allows. This is a **review**, not a change; nothing here has been
implemented. Severity is about impact on power/airtime, not correctness.

Scope: the node's sleep/wake/RX-window machinery and the hub's downlink
traffic pattern. Out of scope: motor/actuation power, the security findings
from `review-2026-09-15.md` (cited only where they overlap).

---

## What is already good, and should not be re-litigated

These came up while tracing the current code and are worth stating so the
findings below read as the delta against a codebase that already got the big
things right, not as a first pass.

* **Light sleep is doing its job.** `SystemCtrl::applyPowerProfile(false)`
  scales 40–240 MHz and enables automatic light sleep; the one hardware
  measurement on record (`power-analysis.md`) puts the awake current at only
  ~1.3–1.6 mA. The 278× duty-cycle gap between interactive and automatic mode
  does **not** translate 1:1 into an energy gap for exactly this reason.
* **Per-frame verbose logging was already found and fixed** (`mode-b-power-goal`
  memory): it was 8.6 % of Mode B's CPU-awake time, now clamped to WARN outside
  a measurement run. The residual cost of the once-a-minute `duty:` diagnostic
  line (`LoraInterface::maybeLogDuty_`) is negligible by comparison — checked
  this pass, not re-flagging it.
* **The quiet-window and Class-A shape from `wake-cost-proposal.md` are both
  implemented.** Tier 1 (`sleepOk`) cut a routine wake from 28 s to ~20 s
  (`layering-proposal.md`); Tier 2 (Class A, Mode C) is the auto-mode shape in
  production today and passed MAC-0 on 1.1.4. Both were "proposed" in the docs
  that still describe them — they are done.
* **The auto-mode wake model is no longer wrong.** `next_wake_epoch_()` predicts
  an automatic node's real wake (schedule minus beacon lead, or check-in
  interval) rather than the interactive fixed `sleep_duration_`
  (`optimization-analysis.md` §3's concern — closed as U-5). The login retry
  backoff (`schedule_login_retry_`) already stretches its delay to
  `ms_until_node_awake_()`, so a hub restart does not blindly burst a sleeping
  node every few seconds.
* **The 17-copy burst is tuned to the RX window, not padding it**
  (`optimization-analysis.md` §2) — confirmed still true, not re-derived here.
  Cutting it without changing the window scheme would lose frames, not save
  power.
* **The window-end extension is capped** (review finding 4, fixed) rather than
  able to hold the radio in RX indefinitely on noise.

---

## Findings

| # | severity | area | finding |
|---|---|---|---|
| 1 | **High (recurring)** | protocol | TimeSync fires unconditionally on every wake/login, full 17-copy burst, with no drift threshold |
| 2 | Medium (per hub restart) | protocol | Every hub boot spends two logins per node, one of them unnecessary for an already-provisioned node |
| 3 | Medium (unmeasured) | power | Deep-sleep current is still the single biggest unknown in the whole power model |
| 4 | Low–Medium (unverified) | power | The two LoRa DIO pins are deep-sleep wake sources for the node's entire sleep duration, not just its entry |
| 5 | — (design option, not new) | protocol | The promoted-quiet-node gap means the hub still bursts 17 copies at a node Mode B was built to serve cheaply |
| 6 | — (design option, not new) | protocol | Broadcast TimeSync + a pending-data bitmap (Tier 3) would make hub airtime constant in node count, not linear |

### 1. TimeSync: unconditional, full burst, every wake — High

**Where.** `LORAListener::send_timesync()` is called 750 ms after every
confirmed login — both from `confirm_session_()` (every wake/reconnect) and
from the beacon handler — with no check on how much the node's clock has
actually drifted. It is a TimeSync (unacknowledged, so Rule 4 requires the
full burst, never single-shot), so this is **17 copies, every single wake,
forever**, for both interactive and automatic nodes.

**Why it matters more than it looks.** Every other per-wake cost in this
system (login, config sync, schedule push) is amortised — it only fires when
something actually changed, or once per session. TimeSync is the one downlink
that fires on **every** wake unconditionally, which makes it the largest
steady-state contributor to both node RX-on time and hub TX energy once
routine login noise is already minimised. For an automatic node checking in
every few hours off a crystal measured at a few hundred ppm, the accumulated
drift between wakes is milliseconds — almost certainly inside whatever
tolerance the schedule engine needs — yet the burst costs the same regardless
of whether the clock is 2 ms off or 20 s off.

**What would change.** A `clock_offset` a node can already report in its own
beacon; a `sleepOk`-shaped clause ("send this only if predicted drift exceeds
X ms") would remove the burst on most wakes. This is exactly
`wake-cost-proposal.md` Tier 3's stated first easy step, and it has not been
done: "TimeSync's payload is byte-identical for every node... currently unicast
N times."

**Not recommended as a blind cut.** The node has no independent clock source,
so *some* cadence of TimeSync is load-bearing; the finding is about the
unconditional trigger, not about removing it.

### 2. Two logins per hub boot — Medium

**Where.** Found and left explicitly parked during the 2026-09-26 onboarding
work (`docs/hub-onboarding.md`, "Not done, on purpose"): every hub boot sends
a LoginMsg with `request_register=1` (because `config_synced_` is false per
process, not per node), the node answers with REGISTER regardless of whether
its config actually needs re-pushing, and only then does the real login
happen — two ~1.5 s bursts where one would do for a node whose config has not
changed.

**Cost.** Bounded to hub restarts (not a per-wake cost like #1), but it is
airtime and awake time spent on *every* node, *every* hub restart, that a
`needs_config` answer already known from the previous session could avoid.
Fixing it needs the node's `needs_config` bit persisted or predicted by the
hub across its own restart — a small but real design change, not a one-line
fix, which is why it was parked rather than done inline.

### 3. Deep-sleep current — unmeasured, Medium priority only because nothing else is left

**Restating `power-analysis.md` §5 and §6, not re-deriving.** The awake current
is measured (~1.3–1.6 mA); the deep-sleep current is not, and the document's
own model spans 20–200 µA — enough range to make automatic mode anywhere from
a 12× to a 1.1× improvement over interactive. This is still the single number
that would settle the most, and it is bench work (HW-1/HW-4), not a code
change: it needs a µA-capable meter in series with the pack, which the bench
sessions have not had available.

### 4. LoRa DIO0/DIO1 as deep-sleep wake sources for the whole sleep duration — unverified, worth a cheap check

**Where.** `SystemCtrl::enterDeepsleep()` arms `esp_sleep_enable_ext1_wakeup`
on a GPIO mask that includes the three control buttons **and** the SX1278's
DIO0 and DIO1 pins, for the node's entire deep-sleep duration (hours for an
automatic node). `waitForRadioQuietBeforeDeepSleep()` (added after a real
panic-on-entry bug) makes sure both lines are quiescent **before** the sleep
call, but nothing guards the pins **during** the sleep itself.

**Why this is a question, not a confirmed defect.** The node does not need to
receive anything during deep sleep — the scheduled wake comes from the RTC
timer, not the radio — so the only reason DIO0/DIO1 would be armed at all is
if a physical wake-on-radio path is wanted for some other reason (none is
documented). Whether this is actually a live risk depends on hardware
behaviour this review cannot settle from the source: does the SX1278 in its
own `MODE_SLEEP` ever re-assert a DIO line from RF energy near the node, or
from an IRQ flag latched before `lora_sleep()` was called? If either can
happen, every such event is a full, wasted deep-sleep wake — indistinguishable
in the logs from a legitimate check-in unless the wake reason is checked.

**What would settle it cheaply.** The node already prints a wake-reason line
on every boot (`BOOT reason=... causes=0x...`); a week of automatic-mode
uptime with that log checked for a wake whose cause bit points at EXT1-via-DIO
rather than the timer would answer this without any hardware change. If it
turns out to matter, the fix is one line: drop DIO0/DIO1 from the deep-sleep
wake mask, since the timer wake already exists and interactive nodes (which
might plausibly want a radio-triggered wake) do not deep-sleep in the first
place.

### 5 & 6. Restated design options, not new

Both already on record and explicitly not fixed by choice, included here only
because they are squarely inside this review's theme (RF on-time) and worth
having in one place with the rest:

* **The promoted-quiet-node gap** (parked 2026-09-27): a `PhaseReport` rides an
  uplink, and a promoted node with nothing to say never sends one, so the hub
  keeps refusing single-shot for exactly the node Mode B exists to serve
  cheaply — every downlink to a genuinely quiet promoted node still costs a
  17-copy burst on the hub's side. This does **not** undermine Mode B's node-
  side RX-window saving (that comes from the node opening one window per round
  instead of three, independent of what the hub sends); it is purely a hub TX-
  energy and airtime cost.
* **Tier 3 from `wake-cost-proposal.md`** — broadcast TimeSync (finding #1
  above is the same frame) plus a pending-data bitmap, making hub downlink
  airtime constant in node count rather than linear. Not needed at 2 nodes;
  becomes the right lever if the fleet grows toward the ~32-node target.

---

## Recommended order, if any of this is picked up

1. **TimeSync drift threshold (#1).** Highest frequency, smallest change,
   directly reduces RX-on time and hub airtime on literally every wake for
   every node.
2. **Check the DIO0/DIO1 wake-source question (#4).** Free — a week of
   existing boot-reason logging answers it; only worth a code change if the
   answer is yes.
3. **Deep-sleep current measurement (#3).** Not a code change; needs the
   instrument. Settles the automatic-mode battery-life question outright.
4. **Two-logins-per-boot (#2)** and **Tier 3 (#6)** — real but lower urgency;
   the first costs only at hub restarts, the second matters more as the fleet
   grows.

Not recommended: touching the 17-copy burst count, the window-extension cap,
or the drain-wait poll interval — each was already measured or reviewed and
found tuned rather than wasteful.

---

## Addendum 2026-09-27 (afternoon): #1, #2 and #4 implemented, per user decision

User decisions, in order: (1) TimeSync's cadence for Mode B and plain
interactive nodes is unchanged — only an **automatic-mode** node is throttled,
to **once a week** (not the drift-threshold idea above, and not the 5-minute
figure first suggested for Mode B — the user clarified they had that cadence
in mind for the *ppm/drift beacon* mechanism, which is separate and untouched).
(2) Avoid the REGISTER round trip when avoidable. (4) Drop the LoRa DIO0/DIO1
pins from the deep-sleep wake source; keep them for light sleep.

**#1 — TimeSync.** `TimeSyncPolicy.h` (hub-only, pure): unthrottled unless
`auto_mode`, in which case throttled to `kAutoModeIntervalUs` (one week), with
the first-ever send for a node always going through (a node's clock cannot be
evaluated by `shouldRunAutoMode()` otherwise). 7 tests, 3 mutants killed.

**#2 — the register.** `config_synced_` used to be pure in-RAM state, reset to
false by every hub process restart regardless of whether the node's config had
actually changed. `LORAClientRestoreState` (the existing per-node NVS blob,
bumped v2→v3) now also carries an FNV-1a hash of what `ClientConfig` sends; a
hub reboot compares the persisted hash against the current one at `setup()`
and sets `config_synced_ = true` when they match, so `send_login()`'s
`request_register` stays false and the node is not asked to re-identify
itself. 4 tests, 5 mutants killed.

**#4 — deep-sleep wake source.** `SystemCtrl::enterDeepsleep()`'s EXT1 GPIO
mask no longer includes `loraDio0Pin`/`loraDio1Pin` — only the three buttons.
Light-sleep wake (`LoraInterface::wakeSourceEnable()` →
`esp_sleep_enable_gpio_wakeup()`) is a separate ESP-IDF API and is untouched.
No host test is possible (`SystemCtrl.cpp` is hardware-only, not in the host
build); verified by inspection and by the node's continued normal light-sleep
operation on hardware after the flash.

### A real consequence of #2, found the hard way

Encoding the new field into the **versioned** restore-state blob means the
version bump itself forces exactly one "no restored state" boot for every
already-provisioned node — which is precisely the precondition for a
previously-documented deadlock ([[lora-blinds-system]]): the hub sees
`registered=no` and never *proactively* schedules a login
(`schedule_startup_login_()` only runs `if (this->registered_)`), while a node
that has not itself rebooted has no path to *spontaneously* send REGISTER. Hit
on **both** nodes on the very first boot after this shipped. Node 2 (serial
available) recovered via `esptool ... --after hard-reset chip-id`, twice — the
second time because a hub restart fired inside ESPHome's 30-minute preference-
flush window and lost the just-saved v3 record, reproducing the same deadlock.
**Node 1 has no serial port on this bench and was left stuck**, needing a
physical button press — user decision 2026-09-27, deferred rather than done in
this session.

**The general lesson: bumping `kRestoreStateVersion` (or any change that
invalidates a node's persisted hub-side state) reintroduces this deadlock for
every node that does not itself reboot at the same time.** It is not a defect
in this change specifically — it is a property of the persistence mechanism
that any future schema change will trigger again. Worth a line in
`bench-runbook.md` or wherever the next such change is made: budget one
recovery per serial-reachable node, and expect any node without one to need a
physical reset.

Live-hardware confirmation of the actual "REGISTER skipped" behaviour (the
point of #2) could not be completed in this session: it needs a hub restart at
least 30 minutes after a node's config was last saved, and the two restarts
attempted both landed inside that window (the second one *because* of the
recovery from the first). The logic itself is proven in host tests (5/5
mutants killed, including a dedicated "unchanged config" and "changed config"
case each); what is still unconfirmed is the real NVS flash round-trip timing
on this hub, which host tests cannot exercise at all.

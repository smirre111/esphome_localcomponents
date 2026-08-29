# Proposal: entering the blind schedule from Home Assistant

Today the schedule lives in `loradevices.yml` and changing it means editing
YAML and recompiling/reflashing the hub. This proposes making it editable from
Home Assistant.

## The state today

* The schedule is seeded once at setup, from codegen: `__init__.py` emits
  `add_schedule_entry(...)` calls into `sched_entries_[8]`.
* Those entries live in **RAM only**. A hub reboot re-seeds them from the
  compiled YAML.
* The hub already has a versioned NVS restore blob — `LORAClientRestoreState`
  (msgids, login flag, `last_sleep_epoch`) — so the persistence *mechanism*
  exists; the schedule simply is not in it.
* Any hub-side change already reaches the node correctly: `sched_dirty_` →
  new `schedule_version()` CRC → the node reports an older version in its
  beacon → hub pushes. **No protocol change is needed for any option below.**
* The component provides `binary_sensor`, `button`, `cover`, `sensor`,
  `switch`. There is no `number`, `select`, `datetime` or `text` platform yet.

## Persistence is the real problem, not the UI

We already found that **a hub reboot silently reverts auto mode to the YAML
default**, because the `Auto Mode` switch has no `restore_mode` and nothing
persists the intent. An HA-entered *schedule* would have exactly the same
failure: set it in HA, reboot the hub, and it quietly reverts to whatever was
compiled in — with no error anywhere.

So whichever option is chosen, it must persist, and the auto-mode switch should
be fixed in the same change. Otherwise the schedule survives a reboot and the
mode that executes it does not, which is arguably worse than neither.

---

## Option A — per-slot entities (recommended)

Expose N schedule slots per node as native ESPHome entities:

| entity | platform | purpose |
|---|---|---|
| `Slot n Time` | `datetime` (type `time`) | when |
| `Slot n Action` | `select` | open / close / position |
| `Slot n Days` | `select` | daily / weekdays / weekend / mon … sun |
| `Slot n Position` | `number` (0-100) | only for `position` |
| `Slot n Enabled` | `switch` | on/off without deleting |

All with `restore_value: true`, so ESPHome's own preference system persists them
to flash — no new NVS work, and the existing restore blob stays untouched.

**Why this one:** it is what "enter the schedule in HA" actually means — time
pickers and dropdowns, validated in the UI, editable from a phone. It keeps the
truth on the hub, so it survives an HA outage. And it flows through the existing
dirty→CRC→push path unchanged.

**Cost:** entity count. With 4 slots × 2 nodes that is ~40 entities. Mitigations:
- 4 slots rather than the protocol's 8 (current use is 3);
- one `Days` **select** with the existing YAML vocabulary rather than 7 switches;
- hide `Position` behind `entity_category: config`.

**Work:** three new platforms in the component's codegen (`datetime`, `select`,
`number`) plus wiring each `on_value` to update `sched_entries_[n]` and set
`sched_dirty_`. This is the bulk of the effort — the C++ side is small.

## Option B — one text entity per node

A `text` entity holding e.g. `06:00 wd open; 21:45 d close`, parsed on the hub.

Two entities total and trivial codegen. But no UI validation, a parser to write
and test on the hub, and a typo silently yields a wrong or empty schedule. Good
as a debug/bulk-edit escape hatch, poor as the primary interface.

## Option C — an ESPHome API service

`api: services:` exposing `set_schedule_entry(index, minute, daymask, action,
position)`, called from HA automations or scripts.

Very flexible and almost no entities. But the schedule then lives in HA, so it
must be re-pushed after every hub reboot by an automation, and an HA outage plus
a hub reboot loses it entirely unless the hub persists what it was told anyway.
Also no ready-made UI — you would build one from helpers.

Best as a **complement** to A (bulk import, or driving the schedule from a
calendar/sun-based automation), not as the primary path.

---

## Recommendation

**Option A**, with three things done together:

1. Add the slot entities with `restore_value: true`.
2. Give the `Auto Mode` switch the same treatment — otherwise the schedule
   persists and the mode that runs it does not.
3. Keep the YAML schedule as the **first-boot seed only**: apply it when no
   persisted value exists, so an existing installation behaves exactly as now
   until someone edits it in HA.

Point 3 matters — without it, adding this feature would silently discard the
schedule people already have.

## Two things to expect, whichever option is chosen

* **An edit does not reach a sleeping node immediately.** The node learns of it
  at its next wake, via the beacon version mismatch — up to `checkin_interval`
  (6 h today), or sooner if a scheduled event wakes it first. That is inherent
  to a battery node and worth surfacing in the UI (the `Schedule Pending`
  binary sensor already exposes exactly this).
* **Frame budget.** A schedule push is ~62 B for 2 entries, ~69 B for 3. Each
  entry adds ~7 B, and the 17-copy burst means a frame cannot exceed ~130 B
  without overrunning its 1500 ms round. 8 entries ≈ 110 B is inside that but
  close; 4 slots per node keeps it comfortable. See `optimization-analysis.md`.

## Not addressed here

* Sunrise/sunset-relative entries. `ScheduleEntry` already carries a `kind`
  field (0 = fixed, 1 = sunrise, 2 = sunset) that nothing currently sets or
  reads, so the wire format anticipates it — but the node has no solar
  calculation and the hub does not send one. Doing it hub-side (HA knows sun
  times) and pushing absolute minutes is the cheaper path, at the cost of a
  daily re-push.
* Per-entry overrides such as "skip next occurrence".

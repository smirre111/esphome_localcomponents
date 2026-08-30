# Schedule text syntax

The blind schedule is entered as a single line in the `<node> Schedule` text
entity in Home Assistant. One line replaces what was twenty per-slot entities.

```
06:00 daily open; 21:45 daily close
06:00 weekdays open; 07:30 weekend open; 21:45 daily close
08:00 mon,thu position:65; 12:00 sat stop
```

Set it, and `<node> Schedule Status` shows `ok` — or `error:` and the reason,
in which case **nothing is changed** and the box snaps back to the live
schedule.

---

## Grammar

```
schedule := entry ( ';' entry )*
entry    := HH:MM  <days>  <action>
days     := daily | weekdays | weekend | daylist
daylist  := day ( ',' day )*
day      := mon | tue | wed | thu | fri | sat | sun
action   := open | close | stop | position:<0-100>
```

| Field | Accepts | Notes |
|---|---|---|
| time | `HH:MM`, 00:00–23:59 | 24-hour, local time. `6:00` is accepted and normalised to `06:00`. |
| days | `daily`, `weekdays` (Mon–Fri), `weekend` (Sat–Sun), or a comma list | `mon,thu` works. No spaces inside the list. |
| action | `open`, `close`, `stop`, `position:N` | `N` is 0–100. `position` **requires** a percentage. |

Case-insensitive; extra spaces and stray `;` are tolerated. **Maximum 8
entries** — that is the wire format's limit, not an arbitrary one.

An **empty** value is valid and clears the schedule. It is the only way to
remove the last entry.

---

## What happens after you save

1. The hub parses the line. On any error nothing is applied — see below.
2. On success the schedule is stored **in the hub's flash**, so it survives a
   hub reboot. The YAML `schedule:` block is only a first-boot seed.
3. The schedule version (a CRC) changes, so the node sees a mismatch in its next
   wake beacon and the hub pushes the new schedule.
4. **A sleeping node does not get it immediately.** An automatic-mode node is
   asleep between events and its check-in, so a change can take up to
   `checkin_interval` (6 h by default) to land. `<node> Schedule Pending` shows
   ON while a change is still undelivered.

The text box always displays the **canonical form of what the hub actually
holds**, never what you typed. So `6:0 D Open` comes back as `06:00 daily open`,
and a rejected edit visibly reverts.

---

## Errors

A rejected line changes nothing — not even the entries before the mistake. This
matters: applying the valid half of a bad line would leave a schedule nobody
asked for.

| Status message | Cause |
|---|---|
| `error: time out of range: '25:99'` | hour > 23 or minute > 59 |
| `error: expected HH:MM, got '0600'` | missing colon, or wrong length |
| `error: unknown day 'funday'` | not one of the day names/presets |
| `error: unknown action 'wiggle'` | not open/close/stop/position |
| `error: position needs a percentage, e.g. position:40 — 'position'` | bare `position` |
| `error: percentage above 100 in 'position:140'` | N > 100 |
| `error: missing action after '06:00'` | entry ended early |
| `error: more than 8 entries` | wire-format limit |

The message always names the offending token, so it can be found in a long line.

---

## Actions

| Action | Effect on the node |
|---|---|
| `open` | drive fully open |
| `close` | drive fully closed |
| `stop` | halt the motor where it is |
| `position:N` | drive to N % open |

**A note on `position` and `stop`:** the wire enum is `OPEN=0 CLOSE=1 STOP=2
POSITION=3` — it is *not* dense. An earlier version of the Home Assistant action
dropdown assumed `position` was 2 and therefore wrote **STOP**, so a scheduled
position move would have halted the blind wherever it was. The values are now
pinned by a test against `blinds.proto`. It is worth knowing if you ever read
raw `a:` values in `config.txt` on the node.

---

## Limits and the reason for them

* **8 entries** — `ScheduleConfig` carries at most 8, and each entry is ~7 bytes
  in a frame the hub sends as a 17-copy burst. A 3-entry schedule is ~69 B and
  already occupies about 55 % of its 1500 ms transmit round; a frame above
  ~130 B could not finish inside its own round at all.
* **Minute resolution** — the wire format stores minutes since local midnight.
* **Local time** — entries are evaluated in the node's local time, which the hub
  supplies via TimeSync along with the UTC offset. A node without a valid clock
  refuses automatic mode rather than guessing.

---

## Comparison with the per-slot entity UI

Both implementations exist. The slot entities are commented out in
`loradevices.yml` and can be re-enabled by swapping which block is commented.

| | text | per-slot entities |
|---|---|---|
| entities per node | 2 | 20 |
| arbitrary day masks (`mon,thu`) | yes | no — presets only |
| `stop` action | yes | no |
| position | in the syntax | needed a separate control |
| mistyping possible | yes, but rejected with a reason | no |
| flash used | 52.6 % | 53.5 % |

The honest trade-off is the last row but one: a picker cannot be mistyped, and
that was the entity version's main argument. The text format answers it by
refusing bad input outright and naming the problem, rather than applying part
of it.

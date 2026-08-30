# Power analysis — automatic vs interactive mode

Measured 2026-08-30 from node 2's serial log (5 days) and the hub's beacon
telemetry.

**Read this first:** the duty cycles below are measured. The *currents* are not —
nobody has put a meter on the node. So the ratio between the two modes is solid
and the absolute battery life is a model. Section 5 says exactly what to measure
to close that gap.

---

## 1. Awake time — measured

A battery node's cost is dominated by how long it is awake, not by transmitting.
Node 2, automatic mode, from `rst:0x5` to `esp_deep_sleep_start()` per wake:

| date | wakes | awake s | duty |
|---|---|---|---|
| 2026-08-26 | 5 | 177 | 0.20 % |
| 2026-08-27 | 9 | 357 | 0.41 % |
| 2026-08-28 | 6 | 233 | 0.27 % |
| 2026-08-29 | 7 | 233 | 0.27 % |
| 2026-08-30 | 2 | 100 | 0.12 % (partial day) |

29 wakes total: **mean 38 s, median 29 s, range 23–73 s.**

Two wake shapes:

* **check-in / beacon-lead wake — ~28 s.** Wake, resume the session, take a
  TimeSync, wait out the hub's quiet window, sleep.
* **scheduled event — ~46–71 s.** The same plus the motor run, with the
  drain-wait holding sleep off until the blind stops.

A scheduled event costs **two** wakes, not one: the node wakes `beacon_lead`
(30 s) early, finds nothing due, naps ~1 s, then wakes again to execute. That is
deliberate — it lets a schedule edit land before the move — but it means the
21:45 close costs ~28 s + ~46 s rather than one 46 s wake.

---

## 2. Interactive mode — from the design, not measured

An interactive node stays awake and reachable. The hub sends `CMD_SLEEP` at
23:00 for `sleep_duration` = 21600 s (6 h), so it sleeps 23:00→05:00 and is
awake the other 18 hours.

|  | awake per day | duty |
|---|---|---|
| automatic | 233 s | **0.27 %** |
| interactive | 64 800 s | **75 %** |

**~278× more awake time in interactive mode.**

This is the whole finding. Everything below is arithmetic on top of it.

---

## 3. Energy model — assumed currents

Because the currents are unmeasured, here is the sensitivity rather than a
single number. The node enables automatic light sleep
(`esp_pm_configure(..., light_sleep_enable = true)`), so the "awake" figure is
an average that depends heavily on how effective that is with the LoRa RX timer
firing every 500 ms.

| awake / deep-sleep | auto mAh/day | interactive mAh/day | ratio |
|---|---|---|---|
| 80 mA / 20 µA | 5.7 | 1440 | 255× |
| 80 mA / 200 µA | 10.0 | 1441 | 145× |
| 25 mA / 20 µA | 2.1 | 450 | 215× |
| 25 mA / 200 µA | 6.4 | 451 | 70× |
| 10 mA / 20 µA | 1.1 | 180 | 160× |

Illustrative life on a 2000 mAh pack (**the real pack capacity is unknown**):

| assumption | automatic | interactive |
|---|---|---|
| 80 mA / 20 µA | ~354 days | **1.4 days** |
| 25 mA / 20 µA | ~954 days | **4.4 days** |
| 25 mA / 200 µA | ~312 days | **4.4 days** |

Note what changes and what does not. In automatic mode the **deep-sleep leakage
dominates** — going from 20 µA to 200 µA roughly triples consumption, while the
awake current barely matters. In interactive mode the opposite holds: sleep
current is irrelevant and awake current is everything.

**So the two modes need opposite optimisations.** Shortening the ~28 s wake
would help automatic mode very little; reducing board leakage would help a lot.

---

## 4. Empirical cross-check

Node 2's own beacon telemetry, automatic mode:

```
2026-08-28 12:00  v=13.96      2026-08-29 13:42  v=14.04
2026-08-28 21:44  v=13.96      2026-08-30 03:45  v=13.99
2026-08-29 03:45  v=14.04      2026-08-30 05:59  v=13.99
```

Flat within ±0.08 V over two days — consumption is below the resolution of the
measurement. Consistent with the model's automatic-mode figures; it does not
distinguish between them.

**No interactive-mode data exists to compare against.** Every beacon in the logs
reports `TIMER_CHECKIN` or `BUTTON`, both automatic-mode wake reasons, so both
nodes have been in automatic mode throughout the captured period.

**Do not read the node 1 / node 2 voltage gap as a mode cost.** Node 1 sits at
~11.0 V and node 2 at ~14.0 V, but those are different packs — plausibly 3S
versus 4S — and the difference says nothing about consumption. Node 1 also
emitted several `v=0.00` beacons, which are ADC read failures rather than a flat
battery, and would corrupt any trend fitted through them.

---

## 5. What to measure to make this real

In rough order of value:

1. **Deep-sleep current.** The single number that decides automatic-mode life,
   and the model spans 20–200 µA — a 3× swing in battery life. Needs a µA-capable
   meter in series with the pack, node in deep sleep.
2. **Average awake current**, with the LoRa RX timer running, to see whether
   automatic light sleep is doing anything. Decides whether interactive mode is
   1.4 days or 4.4.
3. **Pack capacity and chemistry**, per node. Everything in §3 is per-2000 mAh.
4. **Motor energy per move** — a full close is ~36 s of motor at a current the
   position frames already report as raw ADC counts. Two moves a day, and it may
   rival the idle budget in automatic mode.

Until at least (1) and (3) exist, treat §3 as ratios only.

---

## 6. Conclusions that hold regardless of the missing measurements

* **Automatic mode is not slightly better, it is a different regime** — 0.27 %
  duty versus 75 %. No plausible current assumption closes a 278× gap in awake
  time.
* **Interactive mode is a days-scale battery budget**, not a months-scale one.
  It is fine as a temporary state — a button press, a maintenance window, an OTA
  — and expensive as a resting state.
* **That makes the auto-mode persistence fix a power fix**, not only a
  correctness one. Before it, a hub reboot could silently leave a node in the
  wrong mode; if that direction had been interactive, the node would have gone
  from a months-scale to a days-scale budget with nothing to indicate it.
* **In automatic mode, chase leakage, not awake time.** Halving the 28 s wake
  saves ~1 mAh/day at 80 mA; halving deep-sleep leakage from 200 µA to 100 µA
  saves ~2.4 mAh/day. The wake is already short enough that it is not the target.

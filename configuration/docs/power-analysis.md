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

## 3. Energy — anchored on a measurement

**Measured (Reinhold, meter on the node): interactive mode averages 1.2 mA in
total, including LoRa RX.** That is 28.8 mAh/day.

### What it implies about the awake current

Interactive is 64 800 s awake + 21 600 s deep sleep. Solving for the awake
current across plausible sleep currents:

| assume deep sleep | => awake current |
|---|---|
| 20 µA | 1.59 mA |
| 200 µA | 1.53 mA |
| 1000 µA | 1.27 mA |

**The awake current is only ~1.3–1.6 mA.** Automatic light sleep
(`esp_pm_configure(..., light_sleep_enable = true)`) is working very well: the
CPU spends almost all of its "awake" time in light sleep between the 500 ms
LoRa RX windows.

> An earlier version of this document modelled 25–80 mA awake. That was wrong
> by a factor of ~20, and it inflated the conclusion below. The measurement
> replaced it.

### What that does to the auto-vs-interactive comparison

Because being awake is cheap, the 278× duty-cycle ratio does **not** become a
278× energy ratio. Auto mode's consumption is dominated by the deep-sleep
current, which is still **unmeasured**:

| deep sleep | auto mA | auto mAh/day | interactive mAh/day | auto advantage |
|---|---|---|---|---|
| 20 µA | 0.101 | 2.4 | 28.8 | **11.9×** |
| 50 µA | 0.131 | 3.1 | 28.8 | 9.2× |
| 200 µA | 0.280 | 6.7 | 28.8 | 4.3× |
| 500 µA | 0.580 | 13.9 | 28.8 | 2.1× |
| 1000 µA | 1.078 | 25.9 | 28.8 | **1.1×** |

(auto wake modelled at 30 mA average — a wake does real radio work, unlike
interactive idle. The column is insensitive to this: at 20 µA sleep, changing
the wake current from 10 to 80 mA moves auto from 0.05 to 0.24 mA, still far
below interactive.)

**So the honest answer is a range, 1.1× to 12×, and one measurement decides
where in it we sit.** If the board leaks ~1 mA in deep sleep, automatic mode is
barely cheaper than interactive despite being asleep 99.7 % of the time.
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

## 6. Conclusions

* **Automatic mode is cheaper, but by between 1.1× and 12× — not by the 278×
  the duty cycle suggests.** Being awake costs only ~1.5 mA because light sleep
  is effective, so the awake fraction is not the whole story.
* **Deep-sleep current is now the ONLY thing that matters**, and it is the one
  number nobody has measured. It decides whether auto mode is a 12× win or a
  rounding error. Measuring it is minutes of work with a µA-capable meter and
  is worth more than any further modelling.
* **Shortening the wake is not worth doing.** At 233 s/day and ~30 mA, the
  entire awake budget is ~1.9 mAh/day. Halving it saves under 1 mAh/day against
  an interactive baseline of 28.8.
* **Interactive mode is ~29 mAh/day.** Whether that is acceptable depends on the
  pack, which is also unmeasured — on 3000 mAh it is about 100 days.
* The auto-mode persistence fix still matters, but for correctness rather than
  as a dramatic power saving: a node silently left interactive costs somewhere
  between a little and a lot more, depending on that same sleep current.

### Where this document was wrong

The first version assumed 25–80 mA awake and 20–200 µA sleep, and concluded a
~278× advantage and a "months versus days" difference. The awake figure was out
by ~20×. Interactive mode is not days-scale; it is ~100 days on a 3000 mAh pack.
The duty-cycle measurement was right; the energy conclusion drawn from it was
not, because it rested on an unmeasured current that turned out to be far lower
than assumed.
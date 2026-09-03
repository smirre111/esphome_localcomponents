# The node side of timed-window mode — what the firmware actually does

`timed-window-mode-plan.md` was written from the hub alone; the node firmware
(`smirre111/BlindsESP`, branch `auto-mode-p0`) was not readable at the time and
three of its sections were explicitly marked as assumptions. This document
checks those assumptions against the code.

The result is mixed and worth stating up front rather than burying.

**Survived.** The node's receive-timestamp path is already correct. The RX
window is a hardware symbol timeout, so narrowing it needs no new mechanism.
The 32 kHz crystal is configured. Deep-sleep windows are not viable. Temperature
does not threaten a short round. And the plan's asymmetry rule turns out to be
independently derivable from the airtime side.

**Did not survive.** The node's FreeRTOS tick is 100 Hz, not 1000 (§1). The
determinism work is hub-only — the node already made the driver fix the plan
proposed, and already has the ISR-safe SPI path the plan needs (§3). The RX
window's opening edge is a task-**priority** problem, not a delay problem, and
task latency dominates the guard band the plan sized from drift (§4). The timed
uplink is a new transmit path rather than a modification of the existing one
(§6). The plan would have reintroduced a 2663 ppm ruler error the node's own
header exists to prevent (§7). Its headline economics overstate the battery win
by ~20× (§10). One claim of mine needs softening on its own terms (§13), and one
recommendation I reached for had already been made and retracted by this project
(§14).

Two of those — §10 and §14 — were answered in `protocol-questions.md` and
`optimization-analysis.md` before the plan was written. Neither was cited.

**§15 records what an adversarial review of this document then broke**, and it
contains the two findings that matter most: the node arms no GPIO light-sleep
wake source, so the passive drift fit the plan is built on cannot work under the
production power profile; and the guard band needs a sync frame every ~125 s,
which makes the broadcast beacon a prerequisite rather than a follow-on and
turns the airtime argument break-even at today's two nodes.

Node paths below are relative to the BlindsESP repo; hub paths to
`configuration/`.

---

## 1. The correction that matters most: the node's tick is 100 Hz

```
sdkconfig:2075   CONFIG_FREERTOS_HZ=100
```

The hub runs at 1000 Hz; **the node runs at 100 Hz**. The plan assumed 1 ms
throughout. Consequences:

| code | intent | actual at 100 Hz |
|---|---|---|
| `vTaskDelay(1)` in `lora_tx()` (`components/lora/lora.cpp:831`) | ~1 ms settle | **10 ms** |
| `pdMS_TO_TICKS(slotDurationMs)` = `pdMS_TO_TICKS(29)` | 29 ms | 2 ticks = **20 ms** |
| `pdMS_TO_TICKS(10)` on the TX dequeue (`LoraInterface.cpp:426`) | 10 ms | 1 tick |
| any `pdMS_TO_TICKS(x)`, x < 10 | x ms | **0 ticks — no delay at all** |

So the node's task-scheduling granularity is 10 ms, five times coarser than the
plan's assumed jitter floor and comparable to the *entire* RX window it is
supposed to be placing.

**This is decisive for the design, not merely untidy.** Any node-side timed
window has to be armed from `esp_timer`, never from a task delay — and the
margin for arming it through a task at all is thin. It also means the hub and
node cannot share timing constants that are expressed in ticks.

Worth noting the shape of the risk: nothing here *fails*, it just quietly
rounds. `pdMS_TO_TICKS(29)` silently becoming 20 ms is the kind of defect that
never produces an error message.

## 2. The hub's 1000 Hz is load-bearing across two repos and pinned in neither

`docs/timing-accuracy.md` asserts `CONFIG_FREERTOS_HZ = 1000` for the hub;
`loradevices.yml:22-26` pins four sdkconfig options and not that one. The node
then hard-codes constants that only hold at 1000 Hz:

```
main/include/DriftEstimator.h:68   kCopySpacingUs = (kRoundUs/1000/kTxSlots)*1000   // 88000
main/include/CmdDispatcher.h:425   static constexpr int kBurstTxIntervalMs = 88;
```

The evidence that the hub really is at 1000 Hz is indirect but strong: the node
historically reported −2156 and −2597 ppm while assuming an 88235 µs ruler
(`DriftEstimator.h:63-70`), and 88000/88235 − 1 = −2663 ppm. A 100 Hz hub would
emit at 80 ms and produce ≈ −93 000 ppm, which was never seen.

So: confirmed by measurement, guaranteed by nothing.

## 3. The two radio drivers have diverged — and the node's is the fixed one

The plan's §3 P-2 proposed replacing the millisecond delays in the hub's
`lora_idle()` / `lora_tx()` with the µs values already sitting commented out
beside them. **The node has already done exactly that**, in its own copy of the
same driver:

| function | node `components/lora/lora.cpp` | hub `lora_tracker/lora.cpp` |
|---|---|---|
| `lora_idle()` | `vTaskDelay(1)` **commented out** (:805) | `esphome::delay(1)` **active** (:713) |
| `lora_cad()` | commented out (:823) | active (:734) |
| `lora_rxSingle()` | commented out (:838) | active (:753) |
| `lora_tx()` | `vTaskDelay(1)` active (:831) — but *after* the mode write | active (:744) |
| `lora_rxContinuous()` | active (:845) | active (:762) |
| ISR-safe SPI (`lora_write_reg_isr`, `spi_device_polling_transmit`) | **present** (:168, :204) | **absent** |

Two things follow.

**P-2 is a convergence task, not a novel change.** The node has been running
without those delays; that is field evidence the SX1278 does not need a
millisecond in standby, which was the main risk in proposing it.

**The node already has the mechanism the plan's P-1 needs.** `lora_write_reg_isr()`
uses `spi_device_polling_transmit()` and is callable from an ISR. Combined with
`ESP_TIMER_ISR` dispatch (`CONFIG_ESP_TIMER_INTERRUPT_LEVEL=1`,
`CONFIG_SPI_MASTER_ISR_IN_IRAM=y`, `CONFIG_ESP_TIMER_IMPL_TG0_LAC=y`), a
"fire at the mark" that is a single polled SPI write from a timer ISR is
achievable on the node **today**, with jitter in the low microseconds. The hub
needs that path built.

This inverts part of the plan's sequencing: the hub, not the node, is the side
that needs the determinism work.

## 4. The node's RX window is already esp_timer-paced — and still not deterministic

`LoraInterface::setupRXPollingTimer()` (`LoraInterface.cpp:553-570`) starts a
periodic `esp_timer` at `rxIntervalMs * 1000` = 500 000 µs, dispatched
`ESP_TIMER_TASK`. The plan assumed the window was task-timed. It is not — the
*pacing* is already microsecond-accurate.

What is not deterministic is everything after the callback:

```
esp_timer task (prio 22)  -> xSemaphoreGiveFromISR              LoraInterface.cpp:588
taskLoraRx  (prio 5, core 0) wakes                               main.cpp:369-377
  -> xSemaphoreTake(rx_tx_semaphore_, 700 ms)                    LoraInterface.cpp:382
  -> lora_idle(); 4 register writes; lora_rxSingle()             LoraInterface.cpp:408-417
```

`taskLoraRx` runs at **priority 5**, below four application tasks pinned to the
same core at **priority 6** — `taskSysProcessing`, `taskTxProcessing`,
`taskRxProcessing`, `taskDsptchLora` (`main.cpp:325-358`). The window's opening
edge therefore inherits whatever those tasks are doing.

For a 29 ms window that is invisible. For the ~4 ms window the plan proposes it
is the dominant error term, and unlike crystal drift it has no bound that can be
computed — it is scheduling load.

**The plan's guard-band formula was wrong in its leading term.** It had drift
dominating; at a 3 s round drift contributes ~48 µs while task latency
contributes milliseconds. Either the arming has to move into the timer callback
itself (feasible — see §3), or `taskLoraRx` has to outrank the application
tasks, or both.

## 5. The RX window is a hardware symbol timeout — which is good news

```
LoraInterface.cpp:347   int symTimeout = int(30.0f / 0.26f);   // ~115 symbols
LoraInterface.cpp:415   lora_setSymbolTimeout(symTimeout);
LoraInterface.cpp:417   lora_rxSingle();
```

`RX_SINGLE` on the SX1278 waits for a preamble for `symbolTimeout` symbols and
then times out in hardware. At SF7/BW500 a symbol is 0.256 ms, so 115 symbols ≈
29.5 ms — the "29 ms window" is a radio parameter, not a software timer.

That is the right primitive for timed mode and it needs no new mechanism:
narrowing the window is `lora_setSymbolTimeout(n)` with a smaller `n`. The
minimum is 4 symbols (the SX127x register floor) ≈ 1.02 ms, and detection needs
several preamble symbols anyway. **A ~4 ms window is 16 symbols — comfortably
inside what the hardware supports.**

## 6. The node's uplink is far less deterministic than the plan assumed

The plan treated the timed uplink (P4) as "drop CAD". CAD is the smallest of
four problems. In `LoraInterface::loraRxTask` the transmit path is:

| step | cost | line |
|---|---|---|
| TX queue is drained **only once per RX loop iteration**, and the loop is gated on the 500 ms RX semaphore | **up to ~500 ms of dequeue latency** | `LoraInterface.cpp:382, 426` |
| burst-end deferral `vTaskDelay(pdMS_TO_TICKS(wait_ms))` | 10 ms quantised | `:436-446` |
| **unconditional random pre-CAD backoff**, `slotDurationMs + slotDurationMs*(esp_random()%10)` — runs on *every* transmit, free channel or not | nominal 29–319 ms, quantised to 20–310 ms | `:467` |
| CAD, bounded 2 s wait for the DIO0 result | 0–2000 ms | `:474` |
| `sendPacketBytes`: 6 config writes + FIFO + async TX | ~1 ms | `:182-221` |

Total uplink placement uncertainty is **hundreds of milliseconds, deliberately
randomised**. A timed uplink slot cannot be reached by tuning this path; it
needs a parallel one that bypasses the dequeue gating, the deferral, the backoff
and CAD, and is armed from the same timer that arms the RX window.

And there is a second-order effect worth naming: `xLoraPeriodicRXSemaphore` is
a **binary** semaphore (`LoraInterface.cpp:38`), so ticks that arrive while the
loop is inside the transmit path are dropped rather than queued. With up to five
CAD retries, each costing a randomised backoff plus a 2 s bounded CAD wait, a
single uplink attempt can leave the node **deaf for seconds** — during which the
hub's burst diversity is the only thing keeping the link alive. That is an
argument for fixing the uplink path regardless of whether timed mode is built.

That is a bigger piece of work than the plan implied, and it is the reason to
keep P3 (timed downlink) and P4 (timed uplink) firmly separate: P3 is reachable
with modest changes, P4 is a new transmit path.

## 7. The trap the plan would have set for itself

`DriftEstimator.h:47-70` is explicit that the ruler constant must mirror the
hub's **integer** arithmetic — `1500/17 = 88`, not the exact 88.235 — and
records that getting this wrong cost three firmware revisions at −2663 ppm.

The plan proposes re-pacing hub transmissions with `esp_timer`. The natural
implementation of "88.235 ms" on a microsecond timer is `88235`. That single
choice would reintroduce the exact bug the node's header exists to prevent, on a
node whose constant is compiled in and deployed separately.

Mitigations, all cheap:

- Any hub grid period must be an **exact integer number of milliseconds**, so
  the two representations cannot disagree.
- `DriftEstimator::Result::measured_spacing_us` and
  `LongFit::measured_period_us()` already exist precisely to catch this
  (`DriftEstimator.h:89`, `:205`) — the node must *report* them to the
  hub, not just log them, so a mismatch is visible in Home Assistant rather than
  on a serial cable.
- The host tests already pin `kCopySpacingUs == 88000`
  (`drift_estimator_test.cpp:135-146`). A matching assertion is needed on the
  hub side, so the two constants are pinned from both ends.

## 8. Nothing consumes the ppm figure today

`drift_fit_.ppm()` and `drift_acc_.mean_ppm()` appear only inside `ESP_LOGx`
calls (`CmdDispatcher.cpp:2127, 2201, 2230, 2777`). The estimate is not
persisted, not sent to the hub, and not used for anything.

So the plan's "node reports ppm in the beacon" is genuinely new work — but small
work, and it is the cheapest possible first step: it makes the field behaviour
of the estimator observable before anything depends on it.

Likewise `rtcSlowSrc`: `main.cpp:121-147` already reads
`rtc_clk_slow_freq_get()` and logs it every boot. `sdkconfig:1412` sets
`CONFIG_RTC_CLK_SRC_EXT_CRYS=y`, so the 32 kHz crystal is *configured* — but the
logging exists because configuration is not proof, and the value never leaves
the node. Adding it to the beacon is a one-field change.

## 9. Light sleep is on, and its effect on the timebase is the real unknown

```
SystemCtrl.cpp:438-450   applyPowerProfile(false) -> light_sleep_enable = true, 40-240 MHz
sdkconfig:1616           CONFIG_PM_ENABLE=y
sdkconfig:1628           CONFIG_PM_LIGHTSLEEP_RTC_OSC_CAL_INTERVAL=100
sdkconfig:1813           CONFIG_ESP_TIMER_IMPL_TG0_LAC=y
```

`esp_timer` on this target counts APB via TG0 LAC, and APB stops in light sleep;
IDF compensates on wake from the RTC. So between two RX windows the node's
timebase is *part APB, part RTC*, with a compensation step at each transition.

The measured +8 ppm was taken with light sleep **disabled**
(`applyPowerProfile(true)`, `timing-accuracy.md` §3.2). **It therefore says
nothing about the clock the timed mode would actually run on.** This is the
single largest unquantified risk in the design, and it is cheap to close: run
the existing drift test with the production power profile instead of the
measuring one, and compare.

Deep sleep is a separate and worse case, and the existing measurement settles
it: `wake-cost-proposal.md` records 919 ms from wake to the first log line. A
scheduled window cannot be placed through a ~1 s variable boot. The plan's
"deep-sleep wake always starts in BURST" stands, and P5 should be treated as
unlikely rather than deferred.

## 10. The economics are about 2x, not 45x — and this project has made this
      exact error before

The plan led with duty cycle: 5.9 % → ~0.13 %, a 45× reduction. That is true and
energetically almost meaningless.

`power-analysis.md` §3 measures interactive mode at **1.2 mA total** and derives
an awake current of only **~1.3–1.6 mA**, because automatic light sleep already
works well. Taking RX-on at ~11 mA and 5.9 % duty, the receive windows cost
about **0.65 mA** of a ~1.45 mA awake budget. Removing nearly all of it:

| | today | timed |
|---|---|---|
| awake current | ~1.45 mA | ~0.8 mA |
| interactive average | 1.2 mA | ~0.65 mA |
| mAh/day | 28.8 | ~15 |
| days on a 3000 mAh pack | ~100 | ~190 |

**Roughly 2×.** Real, worth having, and not what a 45× duty-cycle ratio
suggests.

**And the project had already worked this out.** `protocol-questions.md`
§"Is it worth it?" contains the same arithmetic — "at an SX1278 RX current of
~11 mA, 5.9 % duty contributes ~0.65 mA ... roughly a third of the whole
budget" — together with the caution that it "will not move automatic-mode
battery life measurably". The plan simply did not cite it. That is the more
embarrassing failure of the two: not a wrong number, an uncited existing answer.

It is worth stating plainly because `power-analysis.md` §"Wrong turns"
records the project making precisely this mistake once already — an earlier
version modelled 25–80 mA awake and inflated the auto-vs-interactive conclusion
by ~20×, and the correction notes "the duty-cycle measurement was right; the
energy conclusion drawn from it was not". The plan's headline repeated the
shape of that error.

The stronger justification for the mode is the other one: **17× less channel
occupancy per command**. That is what decides how many nodes the system can
carry, and it does not depend on any unmeasured current.

## 11. Temperature — a constraint neither document mentions

A 32.768 kHz tuning-fork crystal has a parabolic tempco, roughly
−0.035 ppm/°C² about a 25 °C turnover. For a blind motor on an exterior window:

| ambient | offset from calibration |
|---|---|
| 25 °C | 0 ppm |
| 5 °C | ≈ −14 ppm |
| 0 °C | ≈ −22 ppm |
| −10 °C | ≈ −43 ppm |

Over a 3 s personal round, even 43 ppm is 130 µs — irrelevant. Over a 6 h deep
sleep it is 0.93 s.

So temperature does not threaten the short-round design, and it independently
kills the long-sleep one: the 8 ppm figure is a fair-weather bench number, and
budgeting a deep-sleep window against it would be optimistic by roughly 5×.
It also means a one-off commissioning measurement is not sufficient — the
estimate has to be maintained, which is another argument for §8's "report ppm
continuously" being the first step.

## 12. The determinism budget

"As deterministic as possible" needs a number per contributor, not a list of
suspects. Both drivers use `spi_device_transmit()` — the blocking,
interrupt-driven variant — for every register access, each wrapped in a
semaphore take/give and two CS GPIO toggles: call it ~20-30 us per access
against ~2 us of actual clocked data at 9 MHz. Overhead dominates by an order
of magnitude, and every byte of payload is its own access.

### Hub transmit: release to air

| stage | today | achievable |
|---|---|---|
| pacing release (`vTaskDelayUntil` / `esp_timer`) | deterministic | deterministic |
| protobuf re-pack + `malloc`, **per copy** | 50-200 us, varies with msgid varint width | 0 — pre-built |
| radio mutex vs. the main loop's RX poll | 0 to ~4 ms (a 255 B FIFO read is 255 blocking transactions) | 0 — pre-mark lock |
| `ESP_LOGI` on blocking UART, **inside the mutex** | ~6 ms at 115200 + a variable push to API log clients | 0 — after fire |
| `lora_idle()` -> `esphome::delay(1)` | snaps to the next tick: **0-1 ms** | ~120 us busy-wait |
| 6 config register writes that never change | ~150 us | 0 — hoisted to `setup()` |
| `lora_beginPacket()`: 2 reads + `lora_idle()` -> `delay(1)` | **0-1 ms** | 0 — in prepare |
| FIFO fill, **one SPI transaction per byte** | **~1.5 ms at 60 B, ~4 ms at 152 B** | 0 — in prepare |
| `lora_tx()` — the single write that starts the air | ~25 us | ~5 us polled |
| **air-start uncertainty** | **~+-2 ms jitter + a 1-4 ms payload-dependent offset** | **~+-10 us** |

The payload-dependent term deserves separate attention. It is not jitter; it is
a **bias that scales with frame length**, and the node's time-on-air correction
does **not** remove it, because it happens before the air. A 60-byte ack and a
152-byte schedule push leave the hub with ~2.5 ms of different pre-air latency.
Any timing model that treats "the hub transmitted at the mark" as
length-independent is wrong by that much.

Two fixes, either sufficient: write the FIFO in **one** multi-byte SPI
transaction (the SX1278 supports address-then-burst; the ~60x per-byte overhead
is pure waste), or move the whole fill into the prepare phase so it is outside
the measured instant. Doing both is better — the burst write also removes ~1 ms
of latency from every ordinary transmission.

### Node receive: tick to window open

| stage | today | achievable |
|---|---|---|
| periodic `esp_timer` (`LoraInterface.cpp:568`) | +-few us | same |
| `ESP_TIMER_TASK` dispatch (prio 22) -> `xSemaphoreGiveFromISR` | ~10 us | — |
| `taskLoraRx` (prio **5**) scheduled against four prio-**6** tasks on core 0 | **unbounded under load** | 0 — arm inside the timer callback |
| `rx_tx_semaphore_` take, 700 ms bound (`:382`) | 0, or blocked behind a transmit | — |
| `lora_idle()` + 4 register writes + `lora_rxSingle()` | ~120 us (the delays are already gone here) | ~30 us polled |
| **window-open uncertainty** | **milliseconds under load** | **tens of us** |

The node's receive *timestamp* path, by contrast, is already right: the ISR
takes `esp_timer_get_time()` as its first statement and the value travels with
the event through the queue (`myinterrupts.cpp:26-42`), so the residual is GPIO
interrupt latency — a few microseconds with an IRAM ISR. Nothing to fix.

### A latent defect the budget exposes

`lora_write_reg()` and `lora_read_reg()` take the SPI semaphore with a **1-tick
timeout and silently do nothing on failure** — hub `lora.cpp:158, 207`, node
`components/lora/lora.cpp:143, 251`, and in both the `else` branch contains only
a comment. On the node a tick is 10 ms, so a contended bus silently drops
register writes.

The consequences are graded by which write is lost: a dropped FIFO byte
corrupts the payload and the CRC turns it into a lost packet; a dropped
`REG_OP_MODE = TX` means the frame is never transmitted at all, with no error
anywhere. Per-byte FIFO writes maximise the number of chances for this to
happen — 60 opportunities per ordinary frame.

This is worth fixing on its own merits, and it is a second independent reason to
collapse the FIFO fill into one transaction.

**Correction (second review).** An earlier version of this section said the
node's ISR-safe variants "deliberately do not take that semaphore". That was
wrong — it came from grepping for `xSemaphoreTake(xSemaphore`, which matches the
*commented-out* line 197 and misses the live one immediately below it:

```
components/lora/lora.cpp:197   // if (xSemaphoreTake(xSemaphore, (TickType_t)1) == pdTRUE)
components/lora/lora.cpp:198   if (xSemaphoreTakeFromISR(xSemaphore, &xHigherPriorityTaskWoken) == pdTRUE)
components/lora/lora.cpp:204       spi_device_polling_transmit(__spi, &t);
```

It does take it — and what it takes is a **mutex**
(`xSemaphoreCreateMutex()` at `:108`). FreeRTOS forbids mutex operations from an
ISR: `xSemaphoreTakeFromISR` on a mutex-typed handle bypasses priority
inheritance and the paired give never disinherits. `spi_device_polling_transmit`
is separately not ISR-callable, since it acquires the bus lock and can block.

So the "ISR-safe SPI helper" is unsound twice over, and its block time is zero —
a held mutex means the write is silently skipped with no timeout at all, a worse
version of the `lora.cpp:158` defect below. Anything built on it inherits a
silent-miss path. This does not remove the prepare/fire split as an approach,
but the fire cannot be dispatched from an ISR on this codebase as it stands.

## 13. Revisiting my own claim about the +-1500 ppm — and softening it

`timed-window-mode-plan.md` §3 asserted that the historically observed
+-1500 ppm scatter over a 1.4 s burst was *not* caused by `vTaskDelayUntil`, in
contradiction of both `timing-accuracy.md` §2 and the LongFit comment in
`DriftEstimator.h:150-165`. Having now read both sides of the link, the honest
version is more nuanced than either.

Walking the hub's path for burst copy *n*:

1. `vTaskDelayUntil` unblocks `sendTask` at tick `T0 + 88n` — **exactly**, since
   `xLastWakeTime` advances by a fixed 88 and never accumulates error.
2. The task then does W microseconds of work: protobuf re-pack, mutex, and the
   ~6 ms blocking `ESP_LOGI`.
3. `lora_idle()` calls `esphome::delay(1)`, which blocks until the **next tick**
   — i.e. it lands at `T0 + 88n + ceil(W)`.
4. A few register writes, then `lora_beginPacket()` calls `lora_idle()` again:
   another wait to the next tick.
5. FIFO fill, then `lora_tx()`.

So air-start is `T0 + 88n + ceil(W) + 2 ms + fill_time`. The variable term is
`ceil(W)` — an **integer** number of milliseconds. If W is stable, air-start is
deterministic; if W's sub-millisecond variation happens to straddle a boundary,
`ceil(W)` flips and air-start jumps by a whole millisecond.

That is: **the two `delay(1)` calls are a quantiser that converts sub-millisecond
work variability into +-1 ms discrete jumps**, and W is dominated by a ~6 ms log
line whose duration is not perfectly repeatable.

Where each account is right:

- `timing-accuracy.md`'s **arithmetic** is right — 1 ms over 1.408 s is ~710 ppm,
  and +-1 ms peak-to-peak is ~1400 ppm, which matches the observation closely.
- Its **conclusion** is right — a longer baseline makes the quantisation
  irrelevant, and that is what actually fixed the measurement.
- Its **mechanism** conflates *release* with *transmission*. The tick appears in
  the chain, but `vTaskDelayUntil` is not the quantiser; the two `delay(1)`
  calls downstream of it are.
- My earlier phrasing ("not caused by tick quantisation") over-corrected in the
  other direction. Tick quantisation is involved — just not at the pacing call.

Why the distinction is worth the paragraph: the misattribution makes the TX path
look innocent, and the TX path is exactly what a timed window depends on.
"Lengthen the baseline" fixes a *measurement*; it does nothing for a *scheduled
transmission*, which has no baseline to lengthen.

**What would settle it**: instrument it rather than reason about it. Toggle a
GPIO in `lora_tx()` and capture the mark-to-edge interval, which is P-6 in the
plan and is cheap. If air-start clusters on 1 ms boundaries, the quantiser
account is right; if it is smoothly distributed, something else dominates and
this section is wrong. This project's own §7 lesson — "instrument instead of
theorising" — applies to this document as much as to any other.

## 14. A cheap first measurement — and a mistake I made reaching for it

My first draft of this section proposed a cheaper alternative to the whole plan:
count which `burstIndex` the node actually accepts, on the theory that if it is
usually copy 2 or 3 then 17 copies are over-provisioned and can simply be cut
with no timing work at all.

**That is a recommendation this project has already made and already retracted.**
`optimization-analysis.md` §2 is titled "CORRECTED: it is not over-provisioned",
and explains why:

> The first version of this section said the 17-copy burst was probably
> over-provisioned and could be roughly halved. **That was wrong**, and it came
> from misreading `rxIntervalMs` (the interval BETWEEN the node's RX windows) as
> the window duration.

With copies every 88 ms and a ~29 ms window, a window catches a packet start
with probability ≈ 33 %, and three windows give **1.00 expected catches per
round**. `txSlotsPerRound = 17` is tuned to that, not padded — cutting it
without widening the window or synchronising the ends pushes the catch rate
below one and starts losing downlinks.

So the burstIndex counter is still worth building, but for the opposite reason:
**as confirmation of the 33 % model**, not as a prelude to cutting anything. If
the observed distribution is roughly uniform across indices the model holds; if
it clusters early, something unmodelled is happening and is worth understanding
before touching the radio. It is cheap — `burstindex` is already parsed on every
accepted frame (`CmdDispatcher.cpp:2834`), it is simply not recorded — and it is
the one measurement that validates the receive model the whole design rests on.

### And the corroboration that matters

The same document ends §2 with:

> The real lever on downlink airtime is the scheduled-window scheme in
> `protocol-questions.md` §5, which lets the window and the burst shrink
> together — and which is blocked on sub-second time sync, not on the burst
> count.

That is an independent argument, reached from the airtime side rather than the
timing side, that the timed-window mode **is** the right thing to build — and
that the blocker is exactly the sub-second time sync the plan is about. It is
the strongest support the proposal has, and the plan did not cite it either.

Two documents, two uncited answers, both found only after reading the node.
The pattern is worth naming: **this project's design documents already contain
more than any one of them assumes**, and a plan that does not survey them
re-derives — and sometimes re-breaks — settled questions.

## 15. What an adversarial review changed

A reviewer was asked to attack the analysis above and the plan it corrects.
Most of what it found stands. Recording both what it broke and where it
overreached, because the second is as useful as the first.

### 15.1 The finding that may kill P1 outright

**Nothing in the node arms a GPIO light-sleep wake source.** `grep` for
`gpio_wakeup_enable` and `esp_sleep_enable_gpio_wakeup` across the whole node
tree returns **zero hits**. `pm_lock_handle_rx` is declared at `main.cpp:99`
and externed in two places but is never created and never acquired — it is
dead. Production runs `light_sleep_enable = true` (`SystemCtrl.cpp:444`) with
`CONFIG_PM_ENABLE=y` and `CONFIG_FREERTOS_USE_TICKLESS_IDLE=y`.

The project has already measured the consequence, at
`CmdDispatcher.cpp:2159-2161`:

> waking from light sleep on a GPIO needs the wake source explicitly armed, so
> with it enabled the node slept through most of a burst (**17 copies became
> 3-6**).

The reviewer read this as frame loss. It is worse than that for this design,
and also less bad: because DIO0 is **level**-triggered (`LoraInterface.cpp:122`)
the line stays asserted, so a frame received during light sleep is not lost —
the packet sits in the FIFO and the ISR runs when the CPU next wakes. The
*packet* survives.

**The timestamp does not.** `esp_timer_get_time()` in the ISR then records the
instant the CPU woke, not the instant the frame arrived. That is precisely why
the drift test disables light sleep outright rather than holding a PM lock.

So the plan's §4 premise — "the node fits drift passively from ordinary
traffic, in the field, for free" — **is not achievable under the production
power profile**. The options are:

1. arm GPIO light-sleep wakeup, which is code that exists nowhere in the tree
   and appears in no phase of the plan; or
2. run the ~11 mA measuring profile permanently, which deletes the entire power
   case.

There is no third option, and "P1 costs nothing" was the reason P1 came first.
**Arming the GPIO wake source becomes P0.5**, ahead of everything else.

That the level-triggered DIO0 configuration is what keeps this from being frame
loss rather than merely timestamp loss is load-bearing and documented nowhere
except as a robustness note about interrupt lock-up (`LoraInterface.cpp:118-121`).

### 15.2 The objection that reshapes the economics

The plan's §6 guard band is

```
guard = 2 · ppm_uncertainty · t_since_sync + tx_jitter + rx_isr_jitter + margin
```

and it evaluated `t_since_sync` at **one 3 s round**, concluding drift is
negligible. That substitution is only legal if a timing-bearing frame arrives
every round. Nothing in this system does that — the hub transmits when it has
traffic, a handful of times a day.

At the measured 8 ppm, and before any allowance for the *uncertainty* the
formula actually calls for:

| t_since_sync | 2·8 ppm·t | fits a ±2 ms window? |
|---|---|---|
| 3 s | 0.05 ms | yes |
| 2 min | 1.9 ms | marginal |
| 10 min | 9.6 ms | no |
| 1 h | 57.6 ms | **worse than today's 29 ms window** |

**A ±2 ms window requires a sync frame every 125 s per node.** That is not a
tuning parameter; it falls out of the arithmetic.

The reviewer's conclusion — that paying for those keepalives reverses the
airtime argument — is right in direction. Its magnitude is not: it priced a
sync *every round*, which the guard band does not require. Priced at the
125 s the arithmetic actually demands, ~15 ms per frame:

| | today (17 copies) | timed + **unicast** sync | timed + **broadcast** sync |
|---|---|---|---|
| 2 nodes, 5 cmd/node/day | 2.5 s/day | 21.7 s/day | 10.9 s/day |
| 2 nodes, 20 cmd/node/day | 10.2 s/day | 22.2 s/day | 11.4 s/day |
| 5 nodes, 5 cmd/node/day | 6.4 s/day | 54.4 s/day | 11.2 s/day |
| 10 nodes, 5 cmd/node/day | 12.8 s/day | 108.8 s/day | 11.5 s/day |

Two conclusions, and they are the most important in this document:

**A unicast keepalive is fatal.** It is 2–9× *worse* than today at every node
count, and it scales with N — the opposite of what the mode is for.

**The broadcast beacon is a prerequisite, not a follow-on.** With one shared
sync frame the cost is flat at ~11 s/day regardless of N, and the crossover
against today's burst is around **3–5 nodes**. The plan listed
`wake-cost-proposal.md` Tier 3 under "what this does not solve". It belongs
*before* the timed window, because it is the only thing that makes the timed
window's headline claim true.

And at today's **two** nodes, the airtime argument is break-even at best. The
case for the mode there rests entirely on the node battery — which is §10's
~2×, from an unmeasured current.

One thing the reviewer missed in its own objection: window *cadence* and sync
*cadence* are independent. The node must open a window every few seconds for
command latency; the hub need only transmit every ~125 s for sync. Most windows
are empty and cost 4 ms of RX. So the plan's 0.13 % receive-duty figure survives
this objection — it is the *hub airtime* claim that does not.

### 15.3 Accepted corrections

- **My hub-tick inference was overstated (§2).** `pdMS_TO_TICKS(88)` yields
  exactly 88 ms at 1000, 500, 250 **and** 125 Hz — 88 is a multiple of all four
  tick periods. The measurement rules out 100 Hz (80 ms) and 200 Hz (85 ms) and
  nothing else. This matters because §13's mechanism depends on the tick
  *period*: at 500 Hz, `esphome::delay(1)` truncates to **zero ticks** and the
  quantiser argument disappears entirely. (The reviewer said "a 2 ms quantum";
  that is wrong in the direction that would have been harmless.) The tick rate
  must be pinned in YAML and read back from the generated sdkconfig, not
  inferred.
- **The ~6 ms `ESP_LOGI` cost is unverified.** `sendPacketBurst` runs on
  `sendTaskDispatcher`, pinned to core 1 (`lora_tracker.cpp:88-93`), not the
  ESPHome main task. Recent ESPHome routes logs from non-main tasks through a
  ring buffer for the main loop to drain, which would make that call a memcpy,
  not a UART block. ESPHome is not vendored here and I could not check it. §12's
  table asserts a cost that has not been shown. Treat it as a hypothesis that
  P-6 tests, not a finding.
- **My backoff range was wrong.** `29 + 29·(esp_random()%10)` is 29·(1..10) =
  **29–290 ms**, not 29–319. Quantised at 100 Hz it is
  {20, 50, 80, 110, 140, 170, 200, 230, 260, 290}, **mean 155 ms**. I failed to
  apply my own §1 to my own §6.
- **"Unbounded under load" (§4) is unearned.** All four priority-6 tasks block
  on `portMAX_DELAY` queue receives, so they consume CPU only when a frame is in
  flight. The sharper statement is worse for the design: the delay is
  **correlated, not random** — those tasks run *because* a frame arrived, doing
  protobuf, AES-GCM and NVS work, so the window that opens late is precisely the
  one after traffic. In a design where every round carries a frame, that is
  every window. Also, `taskMotorContol` is priority **3**, *below* `taskLoraRx`,
  so a moving blind does not delay the window — a point in the design's favour
  I had missed.
- **Temperature (§11) is attached to the wrong oscillator.** The 8 ppm was
  measured with `esp_timer`, which counts APB from the 26 MHz XTAL — an AT-cut
  crystal with a cubic TC of a few ppm, not a tuning fork's parabola. My −22 /
  −43 ppm figures apply only in the RTC-timekeeping regime, i.e. across sleep.
  And `CONFIG_PM_LIGHTSLEEP_RTC_OSC_CAL_INTERVAL=100` recalibrates the 32 kHz
  against the XTAL, tracking temperature out for light sleep. The section
  survives only for deep sleep, where it widens a margin that already fails —
  it changes no decision.
- **The 11 mA has no provenance and fails a sanity check.** It appears in
  `timing-accuracy.md:117` as a whole-node figure for the *measuring* profile —
  240 MHz pinned, no light sleep — where the CPU alone would draw far more.
  It is most likely the SX1278 datasheet RX current mislabelled as a node
  measurement. My §10 arithmetic is self-consistent (radio in RX while the CPU
  light-sleeps behind a level-triggered DIO0), but its input is unmeasured, and
  `power-analysis.md` says at the top that no current except the 1.2 mA was
  measured. One meter reading across an RX window settles the whole §10.
- **I cannot conclude the nodes run interactive from the YAML.**
  `lora_client.cpp:1707` logs "Restored ... mode=%s from flash (**YAML seed
  ignored**)" — `auto_mode: false` in `loradevices.yml` is a first-boot seed
  only. This cuts against my own framing: Tier 1 `sleepOk` saves awake seconds
  on **check-in wakes**, which exist only in auto mode, while the timed window
  helps only awake nodes. **They target different modes and are not competing
  for the same milliamp.** "Do Tier 1 instead" was the wrong trade to offer.

### 15.4 New defects found

- **`lora_reset()` has no reset pulse on the node.**
  `components/lora/lora.cpp:1327-1332` uses `vTaskDelay(pdMS_TO_TICKS(1))`
  twice; at 100 Hz both evaluate to **zero ticks**. The SX1278 reset pulse and
  its settling time are gone. The hub's fork of the same function uses
  `esphome::delay(1)` and at 1000 Hz still gets a real millisecond. One line,
  and it belongs in §3's divergence table. It is the only surviving
  `pdMS_TO_TICKS(<10)` in the node tree.
- **`RX_SINGLE` cannot exceed 262 ms.** `lora_setSymbolTimeout` clamps to 1023
  (`components/lora/lora.cpp:1072`), and a symbol is 0.256 ms. So
  `timing-accuracy.md` §5's "six hours needs ~350 ms" does not merely fail to
  fit a 29 ms window — **it cannot be expressed in `RX_SINGLE` at any setting**.
  Any wide-window fallback needs `RX_CONTINUOUS` plus manual timing, which is
  different code, not a different constant.
- **`getBurstEndUs` inherits the burst-overrun defect.**
  `CmdDispatcher.cpp:2836-2840` predicts `remaining × 88 ms`. A 152 B
  ScheduleConfig airs for 95.3 ms, and (per the plan's §9) it *is* bursted 17×
  despite `burstCount = 0`, so `vTaskDelayUntil` never blocks and copies go
  back-to-back at ~95 ms. The node under-predicts the burst end by ~7.3 ms per
  remaining copy — up to ~117 ms from copy 0 — against a
  `kBurstReplyMarginMs` of 40 (`LoraInterface.cpp:432`). The node transmits into
  the tail of the hub's burst. Same site, minor: it anchors on
  `esp_timer_get_time()` at processing time rather than the `rx_us` it already
  holds.
- **`symTimeout` is a magic number tied to an assumed frame length.**
  `LoraInterface.cpp:347`: `int(30.0f/0.26f)`, commented "20 ms per packet
  (+10 ms tolerance)". The plan's own §10 pins real airtime at 95.296 ms for
  152 B — 3× the assumption. It happens not to matter today because the timeout
  governs preamble detection only, but it is the same undocumented cross-repo
  coupling as `kBurstTxIntervalMs`.
- **The replay window makes the plan's burst-fallback rule ambiguous.**
  `SessionManager::acceptRxId` (`SessionManager.cpp:111-121`) requires
  `msgid > rx_id_`, and a rejected frame is dropped with **no ack**
  (`CmdDispatcher.cpp:2693-2697`). The plan's §5 rule 4 — "an unacked single
  shot is retried as a burst, immediately" — does not say whether the msgid is
  reused. Reusing it means the retry is rejected before an ack can be generated,
  so a command that executed can never be acked. Minting a new one means the
  node executes the command twice. The hub already mints a new msgid per retry
  (`lora_client.cpp:1277`), so **duplicate execution is reachable today** — the
  17-copy burst simply makes retries rare. Single-shot makes them routine, which
  turns a latent defect into an operational one. The protocol has no way to
  express "arrived but the ack was lost", and it needs one.

### 15.5 Where the reviewer overreached

- It read the light-sleep problem as **frame loss**. Level-triggered DIO0 means
  the packet survives and only the timestamp is corrupted (§15.1). That is worse
  for P1 and better for P3 than it claimed.
- Its airtime figure of ~864 s/day assumed a sync every round. The guard band
  requires one every 125 s, giving ~11–22 s/day (§15.2).
- Its arithmetic for a 500 Hz hub tick ("a 2 ms quantum") is wrong; the value
  truncates to zero.
- It claimed §13 "described the mechanism backwards". §13 already states that
  `delay(1)` rounds elapsed pre-work **up to the next tick edge**, which is what
  the reviewer then asserts as the correction.
- Its M4 — "a 4.1 ms preamble does not fit comfortably inside a 4 ms window" —
  is a category error. The preamble is the hub's transmission; the window is the
  node's listening interval. They overlap, they do not nest. The weaker version
  of the point survives: the real budget is `window ≥ 2·guard + detection
  (~5 symbols, 1.3 ms)`, so 0.13 % is optimistic once the guard band is honest.

---

## What changes in the plan

| plan section | change |
|---|---|
| §3 P-1/P-2 | The node has already removed the delays and already has an ISR-safe SPI write. The determinism work is **hub-only**, and the hub should adopt the node's driver changes rather than invent them. |
| §3 gate | Keep the p99 < 200 µs gate, but it now applies to the hub alone; the node's equivalent gate is RX-window arming latency, which is a **task-priority** problem, not a delay problem. |
| §5 grid period | Must be an exact integer number of milliseconds. Non-negotiable — see §7. |
| §6 guard band | Reorder the terms: task latency dominates, drift does not. Add the RX symbol-timeout floor (4 symbols ≈ 1 ms) as the hardware bound. |
| §6 window arming | Arm from the `esp_timer` callback (or ISR), not from `taskLoraRx`; or raise `taskLoraRx` above the four priority-6 application tasks. |
| §7 sleep clock | Sharpen: the 8 ppm figure was measured with light sleep **off**. Re-measure with the production profile before P3. Deep sleep is settled against by the 919 ms boot. |
| §8 protocol | Smaller than estimated: `rtcSlowSrc` is already read, ppm is already computed. Add `measuredSpacingUs` to the beacon as the ruler-mismatch alarm. |
| §11 P4 | Timed uplink is a **new transmit path**, not a modification of the existing one. Re-scope. |
| headline | Lead with 17× airtime, not 45× duty cycle. The battery win is ~2×, and `protocol-questions.md` had already said so. |
| sequencing | Insert a P-minus-1: **count which `burstIndex` the node accepts**, as confirmation of the 33 % catch model — not, per `optimization-analysis.md` §2, as a prelude to cutting the burst. |
| §12 justification | Cite `optimization-analysis.md` §2: the scheduled-window scheme is already identified there as *the* lever on downlink airtime, blocked on sub-second sync. That is the proposal's strongest support and it was not referenced. |
| §3, new | Collapse the byte-at-a-time FIFO fill into one SPI transaction, on both ends. It removes ~1–4 ms of **payload-dependent** pre-air latency that no time-on-air correction can undo, and it removes ~60 chances per frame for a silently-dropped register write. |
| §3 P-6 | Promote from "publish a diagnostic" to **the experiment that decides §13**: if air-start clusters on 1 ms boundaries the quantiser account is right; if not, this analysis is wrong about the mechanism. |
| **§11, new P0.5** | **Arm GPIO light-sleep wakeup on DIO0** before anything else. Without it the RxDone timestamp is the CPU-wake instant, not the arrival instant, and P1's passive drift fit cannot work under the production power profile (§15.1). Nothing in the node tree does this today. |
| **§12, reversed** | The **broadcast beacon is a prerequisite, not a follow-on**. A unicast 125 s keepalive costs 2–9× today's airtime and scales with N; a broadcast one is flat at ~11 s/day. Without it the mode's headline claim is false (§15.2). |
| **§6, corrected** | `t_since_sync` is set by **traffic**, not by the round period. A ±2 ms window needs a sync every **125 s per node**, and that keepalive — not the command traffic — is the dominant airtime term. |
| §2 headline, again | At **two** nodes the airtime argument is break-even at best; crossover against today's burst is ~3–5 nodes. The case at N=2 rests entirely on the ~2× battery figure, whose input current is unmeasured. |
| §3 §12 caveat | The ~6 ms `ESP_LOGI` cost is a **hypothesis**, not a finding — `sendPacketBurst` runs off the main task and ESPHome may buffer it. P-6 tests it. |
| §9 divergence | Add `lora_reset()`: `pdMS_TO_TICKS(1)` is **zero ticks** at 100 Hz, so the node has no SX1278 reset pulse at all. |
| §6 fallback | A wide-window fallback cannot use `RX_SINGLE` — it clamps at 262 ms. That is different code, not a different constant. |
| §5 rule 4 | Specify msgid semantics for the burst retry. Reuse ⇒ the replay filter rejects it and no ack is ever possible; new ⇒ the node executes twice. The protocol cannot currently express "arrived, ack lost". |

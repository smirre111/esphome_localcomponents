# Performance review 2026-11-08: is there a regression?

Read-only. Hub `main` 9a901ad, node `main` 7ea87ca (fw 1.1.58). Sources: every `logs/<session>/hub.log` (serial capture, HH:MM:SS host stamps), `node.log` (node 2 serial only), `notes.txt`, `campaign.txt`, plus git diffs `29c5b1e..HEAD` (hub) and `e8647fc..HEAD` (node). Parsers used: `scratchpad/an.py`, `scratchpad/an2.py` (counts per session; the boot-relative numbers come from an2.py). Nothing was edited, built, flashed or pushed.

## Verdict

**There is no system-wide software performance regression in the logs.** On node 2, the healthy reference node, every metric since 2026-11-03 is the same as the 2026-10-19/10-24 baselines or better:
- cover-op latency p50 is 2.0-2.9 s with 0 retransmits (it was 2.5-13.5 s, with retransmits);
- a login after a REGISTER takes 6 s;
- Mode B comes back ~160-175 s after a login (unchanged);
- FER 0 and MIC_FAIL 0.

**What the owner feels is node 1.** Since 2026-11-07 16:31 its downlinks fail: cover ops fail at 43 % (3 of 7) and 1 of 3, LoginMsg success is 3 %, and GridSync ack rates are 9 of 33 and 2 of 16. Before that it had 0 failures in 34 cover ops between 10-19 and 11-06. The evidence puts the cause at the **physical layer, not in the code**:
- **Node 1's own receive counters show the problem at the radio.** On 11-07 its funnel records 883 payload-CRC errors in one day, against 21 on 11-06, 37 on 10-17 and ~0 on node 2.
- **Node 1's uplink path loss stepped up by 3-6 dB at 16:31 on 11-07.** Before the step its uplink RSSI was -77/-78; after it, -80..-86. No software event happened at that moment.
- **The downlink was already the weak direction, by design.** The node sends at 17 dBm on PA_BOOST; the hub sends at 8 dBm on RFO, unchanged since 2026-02 / 09-15. On node 2 that asymmetry is measured at **18 dB**: the hub hears node 2 at -42 dBm, node 2 hears the hub at -60 dBm.
- **For node 1 that leaves almost no margin.** Its estimated downlink is about -96 dBm on good days and -100..-104 dBm after the step. The measured node noise floor is -101/-102 dBm.

The software makes this worse in only two ways. A deaf node costs the other nodes onboarding-gate time (R3), and it burns airtime (R4). Two small genuine software costs exist: the node turnaround step (R5) and the longer onboarding settle (R6).

## Ranked findings

### R1. Node 1 downlink margin collapse (physical layer). Likelihood: high. Not a code regression.

**Evidence.**
- **Node-side RX funnel ("prev wake" in the hub's `Wake clock` line; a day's counts, frozen at deep-sleep entry, `CmdDispatcher.cpp:1208`).** `detected` = RxDone; `crcValid` = no PAYLOAD_CRC_ERROR flag (`frtosTasks.cpp:333`, `MacFunnel.h:80`). The 11-04 episode had 217 CRC errors on what appears to be a test-free day: no session is logged for 11-04, and the 11-06 day with heavy test traffic had only 21.

| reported in session | day covered | node 1 marks / hits / detected / crcValid | CRC fail | node 2 same |
|---|---|---|---|---|
| 20261018_082522 | 10-17 | 42393 / 412 / 754 / 717 | 4.9 % | - |
| 20261105_060459 | 11-04 | 41763 / 550 / 1160 / 943 | **18.7 %** | 43008/40/311/307 (1.3 %) |
| 20261107_155515 | 11-06 | 32379 / 318 / 651 / 630 | 3.2 % | 31281/110/293/293 (0 %) |
| 20261108_082018 | 11-07 | 21119 / 677 / 2085 / 1202 | **42.4 %** | 42406/332/763/762 (0.1 %) |

- **Hub-side uplink RSSI from node 1** (`Received packet` lines in the -70..-95 band; SNR is saturated at ~5.8 throughout):

| session | n | p10 | p50 | p90 |
|---|---|---|---|---|
| 10-26 | 48 | -80 | -79 | -78 |
| 10-31 | 201 | -76 | -76 | -74 |
| 11-02 | 128 | -82 | -79 | -77 |
| 11-03 | 117 | -80 | -78 | -76 |
| 11-05 | 86 | -83 | **-81** | -79 |
| 11-06 | 85 | -78 | -77 | -77 |
| 11-07 | 112 | -82 | -78 | -77 |
| 11-08 | 32 | -84 | **-82** | -79 |

- **The 11-07 onset, to the second.** Node 1 was received at -77/-78 until 16:31:19 (hub.log, `20261107_155515`). From 16:31:24 onward it was -83, -81, -80..-86, and stayed there all evening and on 11-08 (one reading of -92 at 09:20). The first failed command came at 16:33:16. Logins, GridSync and warm-ups were flawless from 16:07 to 16:31 on the same firmware: hub 0e4d2cf, then 9a901ad from 16:16, and fw 1.1.58.
- **Dose-response across sessions.** The weak-RSSI sessions (11-05 at -81, 11-07 after 16:31, 11-08 at -82) are exactly the sessions with node-1 trouble. 11-06 at -77 was flawless. The 11-05 trouble that motivated ab95dcf/7edb3f0 (20 LoginMsg misses) was therefore probably partly RF as well. That is confounded, and it means the 11-06 validation of the post-REGISTER fix ran on a good-RF day.
- **Downlink/uplink asymmetry, measured on node 2.** In node 2's serial, every received downlink shows `CmdDispatcher: RSSI: -60/-61`; at the hub, node 2's uplinks arrive at -42. The idle-window background is `rssi -101/-102`; on 10-19/10-24 it was -103/-104, so the noise environment is about 1-2 dB higher in November. The config behind this: hub `lora_tracker.cpp:175` `lora_setTxPower(8, PA_OUTPUT_RFO_PIN)` (last changed cdc6b30, 2026-09-15); node `components/lora/lora.cpp:425` `lora_setTxPower(17)` (PA_BOOST); SF7 / BW 500 kHz (`LoraTiming.h:45-46`).
- **Longer frames fail more**, which is what a link near its noise floor does:
  - 86 B GridSync: 2 of 16 acked on 11-08;
  - ScheduleConfig: 0 acked on 11-08;
  - 26-29 B cover ops: get through, often after retries.
- **"Only right after its own uplink" fits the RF picture as well.** A marginal link succeeds when the node listens long enough to see several copies of a burst.

**What the software does NOT explain, checked:**
- **No radio-configuration change** in either repo since the baselines. Hub `lora.cpp/.h` and `loradevices.yml` are untouched since 29c5b1e. The node's `components/lora` is untouched. In node `main/LoraInterface.cpp` the diff since e8647fc shows no register, frequency, SF, BW, gain or CRC call. The node sdkconfig only adds app rollback.
- **No counter or replay desync can produce payload-CRC errors**: those come from the radio, below MAC-1. A desync would also be deterministic. Instead, on 11-08 the same session lost a cover op (10:21, 4 retries) and later acked "open" in 3 s (10:27); after login at 10:24 a 0.9 op was acked after 4 retransmits. Each LOGIN also starts fresh FCNTs. The hub's `Not one ack since the reboot` detector never fired in any session.
- **regfallback (89f8f01) never ran on node 1** in the two bad sessions: 0 `REGISTER received` from node 1 on 11-07 and 11-08. If it ever got stuck it would make the node listen MORE (free-running Mode A), not less.
- **A grid misalignment** would be reached by the phase sweep within 4 bursts. Node 1 missed 8 sweep bursts plus 4 retransmits between 16:33 and 16:37 on 11-07, and 28-61 per restart on 11-08. Node 2 was reached in at most 4 every time.

**User-visible effect.**
- Node 1 cover commands fail, or take ~25 s through 4 retransmits plus a forced relogin.
- It does not return to Mode B: on 11-07 it armed 21119 marks against ~42000 on a normal day, i.e. about 9 h in Mode A at ~3x the RX duty.
- Schedule edits do not reach it.
- Logins come only at some of its 15-minute check-ins (11-08: 2 of 9).

**Remedy.**
1. **Physical first.** Check node 1's antenna and connector, anything placed or moved near node 1 or the hub antenna around 16:30 on 11-07, and its supply. Low risk.
2. **Raise the hub's downlink power.** Move the hub to PA_BOOST: `lora_setTxPower(N)` with N around 10-14 dBm. The node-2 serial `RSSI:` line gives an A/B before and after. One line; no wire, timing or airtime change. Risks:
   - confirm the module actually wires PA_BOOST;
   - regulatory: 433 MHz in the EU is 10 mW ERP, and the node's 17 dBm is already above that, so a symmetric legal level for both sides is the clean fix;
   - expect more energy per hub copy (mains-powered, so irrelevant).
3. **Make node 1's link observable without serial.** Put the node's last downlink RSSI and CRC-error count into the TIMER_CHECKIN beacon. The day-old funnel in the wake beacon is the only node-side view today. This is a node + hub proto change (field addition).

**Capture that would settle it:**
- node 1 serial for one hub restart plus one cover op: does it see `rssi` around -100 and `PAYLOAD_CRC_ERROR`, or nothing at all?
- or, without serial: after the hub power change, node 1's next day funnel CRC rate and its cover-op success;
- an SDR / third receiver near node 1 during a failure, to see an interferer and the hub's copies;
- node 1 uplink RSSI before and after physically moving or re-seating its antenna.

### R2. Node 1 deafness mutes its own Mode B and costs power (consequence of R1). Likelihood: high.

- Warm-ups fail (3 in a row on 11-07, 16:39-16:54), GridSync is never acked, and the node stays in Mode A ("node stays in Mode A until its next login").
- Marks on 11-07: 21119, against 32379 (11-06) and 41763/42393 on normal days.
- Battery level: 97 % (10-19) -> 88 % (10-31) -> 79-80 % (11-08). That is ~0.75 %/day before 10-31 and ~1.1 %/day after. The level is coarse (1 % steps, voltage-based), so no real conclusion is possible; at most it is suggestive of more Mode A time.
- **Remedy:** fix R1. No code change.

### R3. A deaf node delays the other nodes' recovery through the onboarding gate (software amplifier; 9a901ad, 28e502a, D1). Likelihood: high. Effect: moderate, only while a node is deaf.

- **Node 2's login after a plain Hub Restart:**
  - 11-08 (9a901ad, node 1 deaf): +59 s (flash), +58, +54, +32 s, on sweep burst 4 three times out of four;
  - 11-07 B1/B2 (0e4d2cf, node 1 healthy): +24 / +38 s;
  - 11-07 R1-R3 (9a901ad, node 1 healthy): +8, +36, +58 s;
  - 10-19..10-31: +5..+39 s.
- **The mechanism**, as seen after R2 on 11-08 at 09:45 (hub.log `20261108_082018`). Node 1 takes the gate first (`Node awake — login challenge in 5 s`, node 2 at 8 s) and holds it 15 s as `node silent` (`OnboardingGate.h:51` kSilentAfterMs). Node 2 then gets sweep bursts 1-3 and the gate goes back to node 1 for 15 s. Node 2's burst 4 comes at +52 s and is acked at +54 s. In other words, the four sweep bursts are spread over two or three gate turns. The runbook (3b) itself expects about two gate turns.
- **After a successful login, a deaf node holds the gate for 40-51 s**, because `schedule_outstanding` (`OnboardingGate.h:87`) waits for a ScheduleConfig ack that never comes. Observed holds for node 1: 50 s and 39 s (11-08), 45 s (11-07), 51 s (11-02); the cap is 60 s.
- **Remedy (hub, low risk):**
  - a node that went `NodeSilent` on its previous turn should not hold the gate 15 s on its next turn while another node waits; keep the 15 s for the first turn only;
  - release `Settled` once LOGIN plus GridSync are acked, and let ScheduleConfig/config continue outside the gate (they are acked on their own channels);
  - mutation-test both.

### R4. Airtime spent on a deaf node (9a901ad fast retries + F1 re-send; 7edb3f0). Likelihood: certain. Effect: small at two nodes, scales with fleet size.

- **LOGIN bursts dominate the airtime.** On 11-08 the hub logged 3445 `Packet sent` in 2.13 h, and 117 of the LoginMsgs were 17-copy bursts (103 of them to node 1). That is ≈1990 copies, ~58 % of all TX. On 11-07: 88 LoginMsgs ≈ 1500 of 5049. On 10-24: 14 LoginMsgs in 12.5 h.
- **Each F1 re-send (`Node transmits without the new session`) restarts the challenge.** The `retry n/24` counter goes back to 1 (e.g. 09:45:26 `retry 5/24` with `burst 31 of the phase sweep`), so the three fast 5-s retries repeat after every 15-minute check-in: about 5-7 bursts per check-in per deaf node, indefinitely.
- **About 1 % channel time per deaf node.** The arithmetic: ~7 bursts × 1.4 s span per 15 min ≈ 1 % channel occupancy per deaf node, against a ~1.2 s 17-copy burst span.
- **Duplicate challenge firing** (two `Login challenge firing` <4 s apart after a boot, 1-3 per session since 10-31, e.g. 11-08 09:45:32/09:45:35) costs one extra burst. It predates the sweep.
- **Remedy (hub, low risk):**
  - do not reset `login_retry_count` / the fast-retry budget on an F1 re-send (keep the sweep index, which already continues);
  - de-duplicate the boot-time double challenge.
- With 32 nodes and several marginal links, this matters for airtime more than for latency.

### R5. Node MAC turnaround stepped up at fw 1.1.55 and partly recovered (node class splits). Likelihood: medium that it is real. Effect: negligible to users.

- **Node 2 ModeTest REPORT** (node.log, n = 47-56 per run, one run per configuration per round):

| round | fw | Mode A MAC-1 p50/p99 us | MAC-1+2 p50/p99 us |
|---|---|---|---|
| 10-19 | 1.1.50 | 6927 / 7499 | 8810 / 9884 |
| 10-24 | 1.1.52 | 6999 / 8014 | 8926 / 9783 |
| 10-26 | 1.1.54 | 6814 / 7630 | 8647 / 9489 |
| 11-01 | 1.1.55 | **7798 / 8554** | **9813 / 11038** |
| 11-02 | 1.1.56 | 7442 / 8196 | 9664 / 10405 |
| 11-03 | 1.1.57 | 7251 / 7970 | 9323 / 10169 |
| 11-06+ | 1.1.58 | not measured | not measured |

- **Size.** The +0.4..1.0 ms (5-12 %) step at 1.1.55 is far larger than the ±0.1 ms spread of the three earlier runs. 1.1.55 brought N19/N24/N27/N20/N29: CmdDispatcher split into MacReceiver/MacUplink/SessionEntity/AppController, `frtostasks::Context`, `RadioMacPort` virtual calls. By 1.1.57 it is down to +0.3-0.6 ms.
- **No new heap, logging or lock on the measured path was found.**
  - The echo path `MacUplink::dataRequest` (`MacUplink.cpp:71-114`) holds `session_tx_mutex_` across `maybePersist()`. `fire_us` is stamped after it, so a persist write would land inside the turnaround, but that has been the case since a008d23 / 1b0ee50 (S3, 10-17) and p99 stays below 11 ms, so persists are rare there.
  - Virtual dispatch costs nanoseconds, not 0.5 ms.
  - Most likely explanation: code layout / flash-cache effects plus the extra indirection. It cannot be attributed without a profile.
- **Against the budget** (the 60 ms reply offset) it is immaterial.
- **Remedy:** none needed. Re-run the regression campaign on 1.1.58 to close the gap in the table.

### R6. Longer onboarding settle since D1 (acked config one at a time) and placed follow-ups. Likelihood: high. Effect: small.

- **`Onboarding done (settled, N s)`** was 8-12 s typical (15-22 occasionally) on 10-19..10-31. From 11-03 it is 11-34 s, from 11-06 17-32 s on config-change flashes, and 8-28 s on 11-07 plain restarts.
- **Why.** `assess()` waits for `tx_drain_us <= 0` and no outstanding ScheduleConfig (`OnboardingGate.h:86-87`). D1 (e098356/f0c5929) adds acked ClientConfig/CoverConfig, up to 3 tries 5 s apart, on CH_SETUP. Placed follow-ups (`placing 55 B at slot 1 T0, 6587 ms out`) also push the drain several seconds into the future.
- **Cost.** The next node's login is delayed by the same 5-20 s after a hub boot.
- **Remedy:** see R3 (release the gate at LOGIN + GridSync). Low risk.

### R7. Items reviewed and found NOT to cost measurable latency, airtime or reliability

- **Listen windows (7e14954) and F3 (bcb0580).**
  - Bursts are deferred by at most 3 s (`kAckListenMs`) per pending single shot. The windows expire by themselves (`end_us > now`) and are released on ack, retry or give-up.
  - F3 re-bases a late burst once, at copy 0 (`lora_tracker.cpp:1043-1048`); no cumulative stride growth.
  - Deferrals are not logged, so their frequency cannot be counted (see "cannot establish"). `placed late` totals stay 3-8 per session in every round since 10-26.
  - Node 2 cover-op latency did not grow; it improved to p50 1.96-2.9 s with 0 retransmits on 11-05..11-08.
- **Staggered SLEEP gate (21b7b80).** It costs the second node at most the first node's SLEEP ladder (~25-30 s if the first is deaf). No overnight capture exists to measure it. With node 1 deaf at 23:00 it would delay node 2's sleep by ~30 s; harmless.
- **The 30 s LOGIN hold-off for warm-ups (7edb3f0) adds nothing at boot**: it sits inside the 75 s settle. Mode B re-established 158-175 s after login on node 2 in every round (10-24: 160 s; 11-08: 157-172 s).
- **FCNT-jump reboot detection, register_seq, N10, ACK_ERROR.** No `REGISTER replayed`, `ACK_ERROR` / `refused by the node`, `Rejected message ID` or `Not one ack since the reboot` in any session since 10-19. `REGISTER rate-limited` appears once (10-31; a test). `No free buffers`: once (11-07).
- **Hub per-copy hot path.** The sealer is a function pointer, the copy is packed into `copy_buf_`, and there is no heap or log per copy (HB8). H-1 tag-write p50 improved from ~2750 us (10-24) to 1450-1650 us (11-03). Send-task stack minimum is 1200-1336 of 4096 in every round; esp_timer 1528-1848.
- **Out-of-slot warnings**: 5-20 per session, the same rate in all rounds (benign, after each op).

## Metrics appendix

### A1. Cover ops (tracked "Cover op sent" / "Sysop sent" -> "Tracked op acknowledged"; latency in s; small n, test sessions)

| session | hub / fw | node 1 n (acked/failed), p50 / max, with retransmit | node 2 same |
|---|---|---|---|
| 10-19 | 914a09a / 1.1.50 | 2 (2/0), 5.7 / 8.3, 1 | 2 (2/0), 8.2 / 13.7, 1 |
| 10-24 | 29c5b1e / 1.1.52 | 3 (3/0), 2.9 / 14.7, 1 | 2 (2/0), 13.5 / 25.2, 1 |
| 10-26 | b46c47a / 1.1.54 | 5 (5/0), 3.5 / 8.3, 2 | 5 (4/1*), 2.5 / 3.1, 1 |
| 10-31 | 41d3990 / 1.1.55 | 5 (5/0), 2.1 / 6.8, 1 | 2 (2/0), 13.3 / 24.7, 1 |
| 11-02 | 7e14954 / 1.1.56 | 9 (9/0), 2.7 / 8.9, 2 | 11 (9/2*), 2.7 / 7.9, 3 |
| 11-03 | f0c5929 / 1.1.57 | 6 (6/0), 2.1 / 2.7, 0 | 6 (5/1*), 2.9 / 14.1, 3 |
| 11-05 | regfix / 1.1.57 | 2 (2/0), 1.9 / 2.1, 0 | 2 (2/0), 2.5 / 2.9, 0 |
| 11-06 | 7edb3f0 / 1.1.58 | 2 (2/0), 2.5 / 2.5, 0 | 2 (2/0), 2.7 / 2.9, 0 |
| 11-07 | 0e4d2cf, 9a901ad / 1.1.58 | **7 (4/3)**, 2.6 / 3.1, 3 | 4 (4/0), 2.0 / 2.3, 0 |
| 11-08 | 9a901ad / 1.1.58 | **3 (2/1)**, 14.1 / 25.5, 2 | 2 (2/0), 2.2 / 2.3, 0 |

\* deliberate test artefacts: an op sent while that node's motor ran, or BOOTFIX reset trials (notes.txt).

### A2. Login after hub boot (first `Login acknowledged` after `rst:`; LoginMsg count before it)

- 10-19..10-31, all hub boots: node 1 +5..+36 s (1-3 msgs; one +290 s and one +398 s on 10-24/10-26 bench boots); node 2 +5..+39 s (1-3).
- 11-02..11-05 config-change flashes (REGISTER path, before ab95dcf / regfallback): node 1 +57..+368 s (4-24 msgs); node 2 +11..+204 s.
- 11-06, four config-change flashes (7edb3f0 + fw 1.1.58): after its REGISTER, each node logged in on the first LoginMsg 6.0-6.6 s later; measured from hub boot, node 1 +11..+31 s, node 2 +5..+47 s (the first flash counted). Plain restart 09:55: node 1 +142 s, node 2 +208 s (6 msgs each).
- 11-07 plain restarts: node 1 +5..+42 s; node 2 +7..+58 s; flash P at 17:16: node 1 +520 s (deaf).
- 11-08: node 1 +1562..+2366 s (25-61 msgs, deaf); node 2 +32..+59 s.
- After REGISTER (`REGISTER received` -> ack): node 2 always 2.5-6.6 s (1 msg). Node 1: 6-14 s on 11-06; 13.9-328 s on 11-05; 5.8-247 s on 11-03; 2.3 s on 10-31.

### A3. Mode B RE-ESTABLISHED after hub boot

158-245 s in every round for healthy nodes, i.e. login + ~160 s. Unchanged across all rounds (A2 rows). For node 1 on 11-08: +2326 s after the flash, then never re-established after R1-R3.

### A4. GridSync / ScheduleConfig acks (acked / published incl. re-publishes; schedule acks)

| session | node 1 gs | node 1 sc ack / fail | node 2 gs | node 2 sc ack / fail |
|---|---|---|---|---|
| 10-24 | 6/6 | 5 / 0 | 7/8 | 7 / 0 |
| 10-31 | 5/5 | 3 / 0 | 9/8 | 7 / 0 |
| 11-02 | 9/22 | 5 / 1 | 17/20 | 11 / 0 |
| 11-03 | 9/9 | 7 / 0 | 13/13 | 10 / 0 |
| 11-05 | 5/5 | 5 / 0 | 5/5 | 5 / 0 |
| 11-06 | 6/6 | 6 / 0 | 8/9 | 8 / 0 |
| 11-07 | **9/33** | 6 / 1 | 8/9 | 7 / 0 |
| 11-08 | **2/16** | **0 / 2** | 4/4 | 4 / 0 |

### A5. LoginMsg traffic and `Packet sent`

| session | span h | Packet sent | /h | LoginMsg n1 / n2 | n1 login ack | onboarding `node silent` n1 / n2 |
|---|---|---|---|---|---|---|
| 10-24 | 12.5 | 6245 | 500 | 6 / 8 | 4 | 1 / 0 |
| 10-31 | 30.6 | 6543 | 214 | 7 / 11 | 5 | 0 / 1 |
| 11-03 | 2.1 | 8129 | 3954 | 42 / 19 | 8 | 12 / 3 |
| 11-05 | 0.8 | 3326 | 4345 | 61 / 20 | 5 | 12 / 7 |
| 11-06 | 0.7 | 3354 | 4574 | 16 / 23 | 6 | 4 / 4 |
| 11-07 | 1.5 | 5049 | 3283 | 68 / 20 | 11 | 29 / 3 |
| 11-08 | 2.1 | 3445 | 1614 | **103** / 14 | 3 | **60** / 3 |

`Packet sent` per hour depends mostly on the test program (ModeTest marks, MAC ping); compare the LoginMsg columns.

### A6. Other counters (per session, all nodes)

- `Rejected message ID`: 0 everywhere. `REGISTER replayed`: 0. `ACK_ERROR` / `refused by the node`: 0. `No free buffers`: 1 (11-07). `TX scheduler full`: 0.
- `tx anomalies` totals: placed late 3-8; stamp misses 2-10 (10 on 11-08, the highest; long uptime + many sweep bursts); copies dropped unsealed 17 on 10-26 only.
- Foreign or noise frames at the hub (RSSI ≤ -95, SNR ~ -12): 3.6-5.2/h on 10-31..11-08, constant. No new interferer at the hub.
- Node 2 idle-window background RSSI: -103/-104 (10-19, 10-24) -> -101/-102 (11-03..11-08), i.e. +1-2 dB.

### A7. Battery (hub sensors, first..last per session)

- **Node 1:** 12.5 V 97 % (10-19) · 12.4 V 93 % (10-24) · 12.4 V 92 % (10-26) · 12.2 V 88 % (10-31) · 12.1 V 85 % (11-02) · 12.1 V 84 % (11-03) · 12.0 V 81-83 % (11-05/06) · 12.0 V 81 % (11-07) · 12.0 V 79-80 % (11-08).
- **Node 2:** 11.3 V 55-58 % (10-19/24) · 9.9 V 11 % then recharged to 11.2 V 55 % (10-26) · 11.2 V 53 % (10-31) · 11.1 V 49-51 % (11-02) · 10.9-11.1 V 44-50 % (11-03) · 11.1 V 49-52 % (11-05/06) · 11.0-11.1 V 47-51 % (11-08).

## What could NOT be established

- **Node 1's real receive state at the failures.** No node-1 serial. What it saw (RSSI, header or CRC errors, nothing) is inferred from the day-old funnel and the RSSI asymmetry measured on node 2. The node-1 downlink RSSI figures above are an estimate (uplink minus the 18 dB measured on node 2), not a measurement.
- **The interferer.** Whether a physical obstruction or change near node 1 (the 3-6 dB step) or a local interferer (the CRC errors; +1-2 dB background on node 2) dominates. Both point away from software.
- **Whether node 2's moving motor affects node 1.** Two 11-07 failures began during or just after node 2 moves; 17:12 and 11-08 10:21 did not. The data cannot separate it from R1.
- **Listen-window and F3 deferral frequency and duration.** Not logged.
- **Production days.** No overnight capture: the 23:00 SLEEP gate, the 05:00 wake, and any staggered-SLEEP delay are unmeasured. Every session is a test session with an unrepresentative traffic mix.
- **No turnaround campaign on fw 1.1.58 or hub 7edb3f0 / 9a901ad.** Last measured 11-03.
- **Power.** RF duty and current are not measured. Battery % is a coarse voltage proxy; only node 1's Mode B mark count shows the extra Mode A time.
- **Sample sizes**: cover ops n = 2-11 per round per node; login-after-restart n = 1-4 per build; one turnaround run per configuration per round. Every per-round p50 above is anecdotal except the turnaround rows.
- **The 11-05 node-1 REGISTER failures** (the motivation for ab95dcf/7edb3f0/89f8f01) coincide with a weak-RSSI day (-81). How much of that was fixed-phase miss and how much RF cannot be split after the fact.

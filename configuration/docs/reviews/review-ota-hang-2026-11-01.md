# Node 1 after the OTA to fw 1.1.55 (2026-10-31): investigation (read-only)

## TL;DR

Node 1 did **not** hang and was **not** off the air. It booted 1.1.55 about 21 s after the OTA ack, just as 1.1.54 did on 10-26. It then resumed its session (F-5), and every 15 min it sent a battery report that the hub authenticated and accepted, for **27 h**. The power cycle was at **2026-11-01** 20:03, not 10-31: hub uptime at that line is 98349 s, and the capture started 10-31 16:44:33.

What was missing was its **two boot beacons**. They were sent into the 17-copy TimeSync burst the hub was transmitting to node 2 at that moment, and lost. Without a reset beacon, the hub never ran `advance_tx_for_boot_` (+16 on its downlink FCNT). The node, though, had restored its replay mark at `saved_rx + 16`, with the whole margin marked seen. So the node silently rejected every downlink the hub sent; it was deaf but still transmitting. The F-5 boot path (SW/panic/WDT/brownout reset) has no beacon-retry ladder and no REGISTER fallback; only the deep-sleep resume path has them. Nothing recovered the node until a POWERON reset forced REGISTER and LOGIN.

This is **not caused by the 1.1.55 refactor**. The F-5 branch and the counter restore are identical in 1.1.54. On 10-26 it worked only because the hub happened to be listening when the beacons went out.

## Evidence (hub.log = logs/20261031_164433_review_rollout2/hub.log)

1. **The FCNT continuity shows one F-5 resume reboot, and that the two beacons were sent but never heard.**
   - Last uplink before the reboot: rx_id=858, the OTA ack at 16:45:35.452.
   - The next uplinks the hub received were rx_id **924 and 925** (16:45:58.221 / .630): two 27-B battery frames.
   - The F-5 restore adds `tx_persist_margin` = 64 (`main/include/Sequencer.h:219`), so the first post-boot FCNTs were 922 and 923. Those are the MODE_CHANGED and BOOT beacons, and the hub never logged a `Received packet` for them.
   - Same pattern on 10-26 (`logs/20261026_103203_review_rollout/hub.log:1214-1232`): 174, then beacons 238 and 239, which the hub did receive. That log line also shows "advancing tx_message_id by 16".
2. **The node stayed alive and its uplinks stayed authenticated.**
   - rx_id 926 → 1034 arrived on a strict 15-min battery cadence, from 17:00:58 (10-31) to 20:01:29 (11-01).
   - Every one was decrypted and accepted, with `NVS save` lines and the Battery Level sensor updating.
   - So the TX task, the battery task, timers and MAC-2 all worked on 1.1.55.
3. **The node was deaf to downlinks.**
   - The hub's downlink FCNT stayed at 1077/1078 for the whole 27 h; it waits for a beacon before sending TimeSync.
   - The only downlink sent was the 23:00 nightly SLEEP, msgid 1078: 1 single shot, then a 17-copy burst, then 4 retransmits, all unacked (`SLEEP command UNCONFIRMED`, hub.log ~18335).
   - Battery reports continued all night, so the node never slept.
4. **The node rejects that FCNT by construction.**
   - `Sequencer::restore()` sets `rx_ = saved_rx + 16; seen_ = ~0` (`Sequencer.h:214-217`). saved_rx lies in [1062, 1077], so rx_ lies in [1078, 1093] and 1078 is always "seen".
   - `MacReceiver::handleDuplicateFrame_` (`MacReceiver.cpp:528-571`) re-acks only from the RAM ack cache, which is empty after a reboot. Otherwise it logs `Rejected message ID: 1078, ignoring, my MsgID: N` and stays silent.
   - The hub compensates **only** from a beacon's reset reason (`lora_client.cpp:1532-1538`, `advance_tx_for_boot_`).
   - The hub sent fewer than 16 new downlinks, so it never climbed past the floor.
5. **Why the beacons were lost: the hub was transmitting.**
   - On 10-26 the ack-to-beacon time was 20.5 s. That puts the beacons here at about 16:45:55.9–56.4.
   - At 16:45:55.554 the hub placed node 2's TimeSync as a Mode A burst ("not a Class A node — burst", 983 ms out). It logged "Packet sent" ×17 from 16:45:56.58 to 57.81, and was deaf while transmitting.
   - Node 2 was onboarding at that moment after its own serial flash at 16:44:52.
   - Node CAD (`LoraInterface.cpp:907-1050`) passes in the gaps between copies, or gives up after 5 tries (`TX abandoned`). Either way the frame is gone after its FCNT was spent.
   - The battery frames, queued later, went out right after the burst ended (58.2 / 58.6).
6. **The F-5 path has no safety net.**
   - `main.cpp:459-465` (1.1.55) and `main.cpp:458-465` (1.1.54) are identical. They set `resumed_session` and the beacon is queued once.
   - Only the deep-sleep branch calls `armResumeFallback()`: 2 beacon retries 4 s apart, then REGISTER after 12 s (`SessionEntity.cpp:180-240`, `SessionEntity.h:89-102`).
   - The node only beacons again on a mode change. That never happened, because the grid is RAM-only and is lost on reset, and the hub never re-sent GridSync because it never knew about the reboot.

## Ranked hypotheses

| # | Hypothesis | Verdict | Evidence |
|---|---|---|---|
| H1 | Boot beacons lost (hub burst) → hub does not advance tx +16 → node replay floor drops all downlinks; the F-5 path has no beacon retry or REGISTER fallback | **~90%, fits every fact** | items 1–6 above |
| H2 | 1.1.55 hang after the restart (`frtostasks::bind` ordering, injected radio, NodeRtcState/WakeForensics) | **Refuted** | 27 h of accepted, correctly sequenced uplinks from the app tasks |
| H3 | Stalled in 1.1.54's WiFi OTA mode (download, `wifiprov::shutdown`) with the boot partition already switched | **Refuted** | OTA mode runs no app tasks (`main.cpp:760-772`), yet FCNT +64 battery frames arrived 23 s after the ack |
| H4 | RTC layout change across `esp_restart()` gives garbage state | **Real but harmless here** | see below |
| H5 | 1.1.55 RX path dead after a SW reset, independent of the replay floor | Cannot be excluded without serial, but H1 alone explains the deafness | serial check below |

**H4 detail.** The bootloader reloads `.rtc.data` on every reset that is not a deep-sleep wake, including `esp_restart`. So the RTC_DATA_ATTR variables the refactor moved (NodeRtcState, WakeForensics, NodeWallClock) start clean, not as garbage. This also matches `pos=0.00` in the beacons after the 10-26 SW reset.

Only `.rtc_noinit` keeps old bytes, and it moved:
- 1.1.54 `.rtc.data` is 0x7c bytes (parsed from `https_hosted/BlindsV3.bin`), so noinit sits at 0x5000007c.
- 1.1.55 `.rtc.data` is 0x74 bytes (`build/BlindsV3.elf`), so noinit sits at 0x50000074.

In 1.1.55 after the OTA restart:
- `trigger_ota` (0x50000074) reads the padding of the old `sleep_enter_time3`, which is 0.
- `g_mode_marker` (0x50000078) reads the old `store_position` float.
- `g_sleep_marker` (0x5000007c) reads the old `trigger_ota` (0) plus 3 bytes of residue.

Result: `rtc_ram=LOST` and the mode marker is reset to interactive. Cosmetic: the F-5 decision is driven by the reset reason, not by the markers. It is a latent hazard, though. A future layout where a stale 0x01 byte lands on `trigger_ota` would start an unrequested WiFi OTA at boot.

## Rollback side finding (N3 is a no-op on node 1)

The 1.1.55 image never saw an authenticated downlink before the power cycle, so `confirmRunningImage` (it acts only on PENDING_VERIFY, `OtaService.cpp:243-257`) never fired. A rollback-capable bootloader would have marked the image ABORTED at the power cycle and booted 1.1.54. Instead the node came back on 1.1.55. So node 1's **bootloader does not support rollback**: OTA never updates the bootloader, and node 1's predates N3 (091a46a, 2026-10-25), even though sdkconfig has `CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE=y`. To confirm, check the "2nd stage bootloader" compile date in node 1's boot banner. App rollback only protects a node after a serial flash of the bootloader.

## What would confirm H1

- **Serial on a repro.** Soft-reset a node (any SW reset, e.g. an OTA) while the hub is bursting to the other node. Expect, in order:
  - `F-5: restored state ... rx=R`
  - `Sending BEACON` / `uplink: NN bytes queued (FCNT T+64)`
  - `CAD detected` or `TX abandoned`, with no hub `Received packet`
  - for the next hub command: `Rejected message ID: <hub fcnt>, ignoring, my MsgID: R+16`
- **Cheap hub-only repro.** Drop the node's reset beacon in proto_sim (or with a hub test hook), then send one command. It goes unacked until the hub has spent 16 FCNTs.
- **Operational check after any OTA.** No `Beacon: ... fw=<new>` within 60 s of the ack means this failure mode, even though battery uplinks keep arriving.

## Minimal fixes (described, not implemented)

1. **Hub (protects deployed nodes, no node flash).** In the authenticated-uplink accept path, treat an FCNT jump of at least 64 (the node's `tx_persist_margin`) over the last accepted rx as a node restore. Then:
   - do the beacon-less half of `advance_tx_for_boot_`: `mac1_skip_tx_(modebsup::kTxBootMargin)` once, with the `last_boot_advance_us_` dedupe so a following beacon does not advance it twice;
   - forget the grid acknowledgement, i.e. re-publish GridSync;
   - mark TimeSync due.

   Skipping downlink FCNTs is always safe. A false positive (64 lost uplinks in a row) only costs 16 ids.

   **Test (proto_sim):** node model resumes via F-5, its beacons are dropped, then a battery uplink arrives with a +66 jump. Assert:
   - the hub's next downlink FCNT is at least old + 17 and the node model accepts and acks it;
   - no double advance when the beacon arrives too;
   - no advance for a +1..+63 gap.

   Mutation-check the threshold and the dedupe.
2. **Node (root gap).** In `decideBootSession`'s F-5 branch (`main.cpp:459`), call `cmdDispatcher->armResumeFallback()` like the deep-sleep branch does. The beacon is re-sent at 4 s and 8 s if no decrypted downlink arrives, then REGISTER at 12 s. Cost: at worst one extra handshake after a crash or OTA.

   **Test (host, CmdDispatcherTestPeer):** an F-5 boot with no downlink gives 2 beacon re-sends then a REGISTER after `kResumeFallbackMs`; an F-5 boot with a decrypted downlink in time gives no REGISTER.
3. **Hardening, optional.** Make `trigger_ota` a 32-bit magic instead of `==1`, and give the RTC_NOINIT markers a versioned fixed-layout struct. Test with a pure bootpolicy function: only the magic starts OTA.

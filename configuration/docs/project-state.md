# LoRa blinds: project state and how to continue (canonical, committed)

Written 2026-11-04 at the end of the layering refactor, review-fix and rollout campaign. **Read this first in a new session.** `CLAUDE.md` (git-ignored, local only) is a short pointer to this file; if `CLAUDE.md` is missing, this file plus `docs/rollout-runbook.md` is everything needed. Dated history is archived in `docs/history-2026-10.md`; the review reports with finding ids are in `docs/reviews/`.

## 1. What this is

ESPHome hub plus ESP-IDF node firmware controlling motorized window blinds over a custom LoRa link (SX1278).
- Hub repo: github.com/smirre111/esphome_localcomponents = `c:\Development\esphome\configuration` (mains-powered; ESPHome components `local_components/{lora_client,lora_tracker,loracover,blindsproto}`, `loradevices.yml`, `tests/proto_sim`, `tools/`).
- Node repo: github.com/smirre111/BlindsESP = `C:\Development\PlatformIO\PlatformIO\BlindsESP` (battery-powered, ESP-IDF, IDF 6.0).
- Two nodes on the bench/production: node 1 `RollladenWohnzimmer1` (OTA only, no serial; its bootloader predates rollback support), node 2 `RollladenWohnzimmer2` (COM6, the test node). Hub on COM9 and 192.168.178.91.

## 2. Status at 2026-11-06 (running in production; origin LAGS, see the table)

| Item | Value |
|---|---|
| Hub `main` | `7edb3f0` local (origin/main = `f0c5929`), production `loradevices.yml` flashed from `C:\hsr9` (config_hash is build-path specific, `0xb604e08b`) |
| Node `main` | `7ea87ca` local (origin/main = `747c88a`), fw **1.1.58**, both nodes run it |
| proto_sim | 1812 pass, 1 disabled (hub, against node main) |
| Node host ctest | 27/27 |
| Hub compile | `esphome compile loradevices.yml` clean, 0 warnings, flash 62.1 %, RAM 13.6 % |
| Wire | MAC wire v1 (live since 2026-10-19); v0 deleted |

The whole body of work of 2026-10-16 .. 2026-11-04: MAC layer separation S0-S8, a three-way Opus review (node, hub, proto: ids N1-N39, HB1-HB27, P1-P23, all fixed or consciously deferred), structural class splits in both repos, three production fixes found on hardware (boot deafness, SLEEP ack collision, config reliability), and four hardware rollouts. Details: section 6 and `docs/reviews/`.

## 3. Architecture (textbook layering, each layer talks only to the next)

PHY (SX1278) / MAC-0 access (addressing, framing, RX windows, bursts, slot scheduling, CAD, ack/retransmit, timing, Modes A/B/C) / MAC-1 sequencing (`Sequencer.h`: FCNT, replay window, dedup, freshness) / MAC-2 security (Encrypt-then-CMAC, K_auth/K_enc/K_mac, session lifecycle) / APP (covers, schedules, telemetry).
Shared dependency-free headers: the node copy is the source, the hub vendors byte-identical copies (`MacSap.h`, `Sequencer.h`, `MacWire.h`, `MacWireVectors.h`, `FrameCrypto.h`, generated `blinds.pb-c.*`, ...); drift gates in the hub CMake compare them. Regenerate with node `proto/regen_stubs.sh`; the hub list of vendored files is `tests/proto_sim/cmake/vendored_shared_headers.txt`.

Wire v1 spec: `docs/mac-wire-format.md` (12-byte header, extensions STAMP/NODE_NONCE/ACKED_FCNT, one protobuf PDU per PORT 0 Mac0Mgmt / 1 Session / 2 App, CMAC-8 over all raw bytes). Layering plan and per-stage commits: `docs/mac-separation-layering-plan.md`; layer semantics: `docs/mac-layer.md`; threading: `docs/threading-model.md` in BOTH repos (read before touching tasks, locks or ISRs).

### Hub classes (`local_components/`; read its `README.md`)
`lora_client.cpp` component/app (2030 lines) plus verbatim TU splits `lora_client_{bench,delivery,session,timing}.cpp` and `lora_client_internal.h`. Classes behind small host interfaces (`SessionHost`, `DeliveryHost`, `StreamHost`): `HubSession` (MAC-1 counters, MAC-2 link, login/session lifecycle, onboarding gate, REGISTER replay state, FCNT-jump reboot detection), `DeliveryService` (all hub->node application frames: slots Control/Config/Mac/Setup, ARQ ladders, `DeliveryPolicy.h`), `NodeLinkTiming` (Mode B/C placement, belief, ack latency), bench streams `MacPingStream`/`DriftStream`/`ModeTestStream`, `ModeBSupervisor`; tracker side `TxPool`, `TxScheduler` (queue order, supersession, channel calendar, listen windows), burst engine, `HubGrid`. `HubLinkSecurity` + `SessionKeyStore` own keys. `PortTable.h`, `HubFrames.h`, `HubWire.cpp` are the hand-kept mirrors of node `PortMap.h`/`NodeFrames.h` (pinned by `mirror_tables_test`).

### Node classes (`main/`; read its `README.md`)
`CmdDispatcher` (1375 lines, members private, one `friend CmdDispatcherTestPeer`) delegates to `MacReceiver` (frame open, admission, port demux, MAC acks), `MacManagement` (GridSync/Beacon/Demote/MacControl), `SessionEntity` (LOGIN, REGISTER retry, resume fallback, beacon ladder), `AppController` (covers, auto mode, handlers), `TelemetryBuilder`, `MacUplink` (uplink path and session lock), `BenchTools` (ModeTest, DriftTest), `NodeRtcState`, `WakeForensics`, `NodeWallClock`, `NodeNoinit` (versioned RTC_NOINIT block, 32-bit OTA magic), `BootPolicy.h`, `frtostasks::Context` (task bodies take injected objects, `bind()` in main.cpp), `RadioMacPort` (the radio reaches the MAC only through it), `Mac0Access.h`, `LinkSecurity.h`, `Sequencer.h`, `PortMap.h`, policy headers (`TimedModePolicy.h`, `SleepDurationPolicy.h`, ...) with host tests.

## 4. Protocol behaviours that are deliberate (do not "fix")

- No plaintext GridSync before a session; before a session the node accepts in the clear only ClientConfig, CoverConfig (bootstrap), LOGIN, GridBeacon, GridDemote. The hub ignores pre-session plaintext MAC frames (no counter, timing, ack, PhaseReport, beacon handling).
- A node acks iff the frame sets ACK_REQ and the handler ran; a recognised-but-refused command (bad cover target, unset/unknown sysop, empty ScheduleConfig) answers `MacAck ACK_ERROR`; the hub fails the request at once, no relogin.
- Encrypted ClientConfig/CoverConfig are acked, retried, sent one at a time on `CH_SETUP`; the node sequence-checks the encrypted ClientConfig and runs it once; the hub saves the hash of the config the node ACKNOWLEDGED and marks in sync only after all acks; the plaintext first-time push stays unacked and is re-sent encrypted at the first session.
- REGISTER carries `register_seq` (persisted in node NVS key `regseq`, strictly increasing); the hub refuses a non-increasing value after the tag check (0 after a numbered one is refused too: a node OTA-rolled back to firmware older than 82a7658 cannot REGISTER until the hub reboots). Plus the existing time rate limit.
- Boot: every resume (deep sleep and soft reset/OTA restart, "F-5") arms the fallback: beacon re-sends ~4 s and ~8 s, REGISTER at 12 s if no downlink decrypts. Hub: an authenticated uplink whose FCNT jumps by >= 64 past the last accepted one is a node reboot (advance downlink FCNT by 16 once, deduped with a reset beacon, re-publish GridSync, TimeSync). Background: `docs/reviews/review-ota-hang-2026-11-01.md`.
- Nightly SLEEP: one node at a time through a hub-wide gate; a burst is never started on top of another node's pending single-shot ack window (listen windows, 3 s ceiling); deferred bursts keep the 88 ms copy spacing. Sleep record stores the COMMANDED duration and is cleared when the node is heard; a keyless uplink from a node with a pending login re-sends its LOGIN (placed into its receive window; rate limit 5 s).
- Node OTA: app rollback enabled (image confirmed on the first proven session) but node 1 has an old bootloader (no rollback until it is serial-flashed once; OTA never updates the bootloader). `trigger_ota` is a 32-bit magic in a versioned RTC_NOINIT block.
- `K_root` is still the public `"LoRaHome"` dev literal (H5, user decision; per-node keys need a key-exchange design). SHA-256("LoRaHome")[0:16] = `16efc5cc9770cb2cb8e84a8c9feff412` (always compute, never copy).

## 5. Hard constraints (from the project owner / hard-won)

Never change the NVS layout or `kRestoreStateVersion` (a mismatch deadlocks nodes without serial; node 1 has none). Keep the REGISTER/LOGIN bypass order. No heap, virtual calls or logging in the verify/seal/retag hot paths; no logging under `SessionKeyStore::keys_mutex_` or the node `session_tx_mutex_` seal lock. Node 1 is OTA-only: flash it BEFORE the hub, hub last; no backward compatibility is needed. Never push or flash without being asked (production deploys are also blocked by the permission classifier until the user says so in chat). Do not move blinds blindly during Mode C runs; wait ~85 s for the motor between cover ops (a node is deaf while it moves). Edit CRLF files with Python `rb`/`wb`, not `sed -i`. Never run `git status` from WSL on these repos (use PowerShell). Mutation-check every new test (restore the mutant afterwards; never filter mutation output through a pipe that can truncate). Empty output is never success.

## 6. Hardware verification history of this campaign

| Date | What | Result |
|---|---|---|
| 2026-10-19 | wire v1 cutover (hub 914a09a, fw 1.1.50) | Mode A 6.93/7.50 ms, MAC-1+2 8.81/9.88 ms, Mode B 598/598, FER 0 |
| 2026-10-24 | final S0-S8 (hub 29c5b1e, fw 1.1.52) | Mode A 7.00/8.01, 8.93/9.78, Mode B 598/597 |
| 2026-10-26 | review fixes (hub b46c47a, fw 1.1.54) | 6.81/7.63, 8.65/9.49, Mode B 600/598 0 gaps |
| 2026-11-01 | class splits (hub 41d3990, fw 1.1.55) | 7.80/8.55, 9.81/11.04, Mode B 598/596. Node 1 OTA then deaf 27 h (boot-beacon bug, fixed later) |
| 2026-11-02 | boot fix + stagger + windows (hub 7e14954, fw 1.1.56) | 7.44/8.20, 9.66/10.41, Mode B 600/598 0 gaps; F-5 and stagger tests pass |
| 2026-11-03 | D1/F3/D2/F1 (hub f0c5929, fw 1.1.57) | 7.25/7.97, 9.32/10.17, Mode B 598/597 1 gap, FER 0, MIC_FAIL 0; config ack, F-5, stale-sleep, stagger tests pass |
| 2026-11-06 | post-REGISTER LOGIN fix (hub 7edb3f0, fw 1.1.58): placed/alternating LOGIN + node Mode A fallback | config-change flashes x4: both nodes logged in on the first (placed) LoginMsg, 6.0-6.6 s after their REGISTER (node 1 was 2-5 min before); Mode B re-established after every flash; reset/restart sanity fine except a slow plain-LOGIN recovery after a hub restart (see section 7) |

Known noise: Mode B phase error p50/p99 varies run to run (105/926 .. 4029/5445 us, bimodal by frame length 49/50, per-length offset; FER and MIC_FAIL stay 0), H-1 tag-write max 8-13 ms, "Packet sent" clumps in the serial/API log are a logging artifact (log drain), not simultaneous TX. Send-task stack minimum on the production hub ~1350 of 4096 free (bench yaml ~1204), esp_timer ~1700-1850 of 3584.

## 7. Open issues and next steps (most valuable first)

1. **Slow plain LOGIN after a hub RESTART (nodes still on the Mode B grid) -- remaining part of the old slow-LOGIN issue.** The post-REGISTER case is FIXED and hardware-verified 2026-11-06 (`logs/20261106_*_regfallback_rollout*`): hub commits `ab95dcf` (the LOGIN of a REGISTER's challenge is a burst whose copy 0 is placed on the node's mark = REGISTER T0 - 60 ms + whole rounds, stamps up to 75 s) and `7edb3f0` (LOGINs alternate placed/unplaced; every LOGIN holds the next Mode B warm-up off for 30 s), plus node `7ea87ca` fw 1.1.58 (from REGISTER until a LOGIN is accepted the node stops arming its Mode B grid and free-runs Mode A windows; `Mac0Access` `awaiting_session`). Four consecutive config-hash-changing hub flashes (per-node ClientConfig hash, e.g. battery 14<->15 min; the YAML config_hash does NOT trigger a REGISTER): both nodes, every time, one REGISTER and the first (placed) LoginMsg acked 6.0-6.6 s later (node 1 before: 2-5 min, 20 misses in 5 cycles). Root cause was a fixed-phase miss: a Mode B node listens only in its ~29 ms mark window and sends its REGISTER at mark + 60 ms, the hub's unplaced LOGIN burst met that window at the same phase every cycle. STILL OPEN: (a) after a plain `Hub Restart` (no config change, no REGISTER) the plain-LOGIN recovery took 2m22s (node 1) and 3m28s (node 2) in the round-5 sample (round 4: 2 s .. 70 s): the nodes are still on the Mode B grid because the hub's startup GridDemote is rate-limited to one per hour on the node, and the unplaced plain-LOGIN bursts miss their narrow mark windows; onboarding gates were released as 'node silent' several times; idea: aim the first plain LOGIN with the F1 keyless-uplink re-send machinery on any heard uplink, or let the node leave the grid when it hears nothing from the hub for a few rounds after boot (design decision: power vs recovery time); (b) the request_register LOGINs BEFORE a REGISTER (1-2 per flash, no fresh uplink stamp); (c) node 1's real reception state (no serial). Caveat on sample sizes: all of this is n=4 flashes + 1 restart.
2. **Verify node 1's real sleep duration**: it has no serial; its config was acknowledged (hub log) but not read back. First real 23:00 sleep after 2026-11-03: it must wake ~6 h later, not after 10 min (a 600 s test value was once pushed).
3. **Node 1 rollback**: needs one serial flash of its bootloader (physical access). Then verify rollback with a deliberately bad OTA on the bench.
4. **CI**: untracked `c:\Development\esphome\.github\workflows\proto_sim_tests.yml` lives in the parent repo; it must check out BlindsESP next to the hub tree and needs a `BLINDSESP_TOKEN` secret if that repo is private; with `CI` set a missing node tree now fails configure.
5. Shadow pending mask (`beacon shadow` log lines) now has real inputs: collect data over a soak before ever transmitting the mask (wake-cost Tier 3, `docs/wake-cost-proposal.md`); broadcast TimeSync still not done.
6. Config-loss recovery (D1) is only host-tested; hub-restart-with-lost-config on hardware was not exercised.
7. Optional: second optimistic single-shot op ~25 s after a move misses its single shot (hub could skip the optimistic shot while a node's motor runs); node 1 position belief after an OTA reset is off (send "open" once); deep-sleep current is ASSUMED uA-level (not measured, user decision); H5 key provisioning; telemetry `optional` fields.
8. Cleanup by hand: scratch dirs `C:\hs`, `C:\hs1`..`C:\hs4`, `C:\hsr2`..`C:\hsr4`; untracked `.vscode/`, `LoaNode.md`, `events150.csv`, `full150.csv`, `trace150.json`, `../git`, `../tasmota-minimal.bin.gz` in the hub tree.

## 8. Where things are

- Procedures (build, test, flash, OTA, bench campaigns, traps): `docs/rollout-runbook.md`. Bench runbook: `docs/bench-runbook.md`. Tools: `tools/{hubctl,benchlog,make_bench_yaml,mac1_turnaround,mac1_campaign,mac2_campaign,layer_lint}.py`, `tools/campaigns/` (the last campaign driver and helpers).
- Reviews and reports (finding ids, evidence, fix status): `docs/reviews/` (`review-node/hub/proto-2026-10-25.md`, `review-ota-hang`, `review-rollout3-anomaly`, `node-handoff-2026-11.md` = the node agent's per-commit handoff log with the test-peer change list, `mac-review-2026-10-16-full.md`).
- Logs of every hardware session: `logs/<timestamp>_*` in the hub dir (notes.txt per session lists the steps).
- Older docs: some are stale; the hygiene list (which are superseded or shipped plans) is in `CLAUDE.md` (local) and below. `docs/implementation-plan.md` is the authoritative Mode B/C design doc (2074 lines). Superseded: `docs/timed-window-mode-plan.md`, `docs/superframe-structure.md`; shipped plans: `docs/auto-mode-plan.md`, `docs/merge-to-main.md`, `docs/battery-runtime-compensation-proposal.md`. Grep `docs/` and `docs/reviews/` before proposing a new analysis: conclusions have been re-discovered more than once.
- Model/agent memory for this project: `C:\Users\reinh\.claude\projects\c--Development-esphome\memory\` (index `MEMORY.md`).

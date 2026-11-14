# Pick-up: hardware test of the session diagnostics, the demote sweep and the node's session report

Written 2026-11-12 in a cloud session that had **no hardware, no ESPHome and no ESP-IDF**. Everything below was implemented and host-tested there; **nothing was compiled for the ESP32 or run on the air.** This file is what a session on the real machine (`c:\Development\esphome\configuration` + `C:\Development\PlatformIO\PlatformIO\BlindsESP`, COM6 node 2, COM9 hub) needs to finish the job. Procedures it refers to (`docs/rollout-runbook.md` sections 1, 2, 3b) are not repeated here.

## 0. How to start the session

Open the hub directory in Claude Code on the Windows machine and paste:

> Read `docs/hardware-pickup-session-diag.md` and `docs/project-state.md`, then carry out the pick-up from step 1. Stop and ask before every flash and before anything that touches node 1.

## 1. What is on the branches

Both repos, branch **`ccr-4b558aad-w6vrnk`** (branched from `main` = hub `9a901ad`, node `7ea87ca`). Fetch and check out both:

```powershell
git -C c:\Development\esphome fetch origin ccr-4b558aad-w6vrnk ; git -C c:\Development\esphome checkout ccr-4b558aad-w6vrnk
git -C C:\Development\PlatformIO\PlatformIO\BlindsESP fetch origin ccr-4b558aad-w6vrnk ; git -C C:\Development\PlatformIO\PlatformIO\BlindsESP checkout ccr-4b558aad-w6vrnk
```

| # | Change | Repo | Needs |
|---|---|---|---|
| A | **Session diagnostics**: per node `Session Stale` (problem sensor: heard without a session for > 30 s), `Session State` (text), `Session Recovery Time`, six counters since hub boot; log lines `SESSION STALE` and `Session recovered in N s (K LoginMsg)` | hub | hub flash |
| B | **Session event log**: last 24 REGISTER / LOGIN events per node with times; dumped to the log when a stale period starts and by the `Dump Session Log` button | hub | hub flash |
| C | **Demote phase sweep**: the startup `GridDemote` broadcast goes out as four bursts (first one's phase + 0 / 22 / 44 / 66 ms, 2 s apart) instead of one, so a Mode B node is taken off the dead grid after a hub restart and then hears any LOGIN burst | hub | hub flash |
| D | **Node session report**: node fw **1.1.59** counts REGISTERs sent, the wait for an accepted LOGIN, the burst copy it caught and failed `register_seq` saves, and reports them in the beacon (`NodeWakeBeacon` fields 36..39); the hub logs each new report once | node + hub | node flash, then hub |
| E | `tools/restart_soak.py`: restarts the hub N times and reports each node's recovery time | hub | A flashed |

A, B, C and E work with node fw 1.1.58: **the hub can be flashed without touching either node.** D is the only part that needs a node flash, and an old node simply reports nothing (all-zero fields are not on the wire).

Not done, on purpose: no node-side "fall back to Mode A when the session looks dead". A healthy, idle Mode B node hears only beacons, so the node has no safe criterion (see the node handoff, 2026-11-05, on SyncStale); C solves the hub-restart case from the hub side.

## 2. Before anything is flashed (WSL + Windows, no hardware)

1. **Regenerate the stubs.** The cloud container's protoc-c (1.4.1) does not reproduce the committed stubs byte for byte, so the proto change of D was carried onto them as a patch. In WSL: `proto/regen_stubs.sh` (node repo). Expected: `git status` clean in BOTH repos. If files change, look at the diff (it must only concern `NodeWakeBeacon` fields 36..39), commit the regenerated files in both repos, and continue.
2. **Host tests.** Node: `cmake -S test -B ~/nodehost && cmake --build ~/nodehost -j8 && (cd ~/nodehost && ctest -j4)`, expect the previous count + `session_forensics`. Hub: `cmake -S tests/proto_sim -B ~/psbuild && cmake --build ~/psbuild -j8 && (cd ~/psbuild && ctest -j4)`, expect every test to pass (the container ran 1924, with the two `schema_drift_hub_vs_*` gates SKIPPED for lack of the right protoc-c and one test disabled; here those two gates must read PASSED). Check the build's own exit status first.
3. **Compile the hub.** `esphome clean loradevices.yml` (a header was added), then `esphome compile loradevices.yml` from a short path (`git archive` into a fresh `C:\hsrN`, copy `secrets.yaml`). This was never compiled with ESPHome. Things that could fail and where:
   - `local_components/loracover/{sensor,text_sensor,binary_sensor}/__init__.py`: new schema keys (`session_recovery_time`, the six counters, `type: session_state`, `type: session_stale`); only syntax-checked.
   - `loradevices.yml`: 18 new diagnostic entities (9 per node) and two `platform: template` buttons calling `id(rol_N)->dump_session_events();`.
   - C++: host-compiled with g++, so real errors are unlikely; warnings count (the build was at 0).
   Record flash and RAM use against `docs/project-state.md` section 2 (62.1 % / 13.6 %). If flash is tight, drop counters from the yaml first (`registers_accepted`, `sessions_dropped`).
4. **Compile the node** (only for D): IDF 6.0, `idf.py build`; `App version: 1.1.59`. Never compiled with ESP-IDF either; `SessionForensics.h` is header-only, four call sites.

## 3. Hardware, step 1: hub only (A, B, C, E) — no node flash

Preconditions: both nodes on fw 1.1.58, awake, Mode B re-established; not within an hour of the 23:00 sleep; `tools/benchlog.py` on both serial ports and an `esphome logs` API connection open (the hub reboots every ~15 min without one).

1. Flash the hub (runbook section 2 step 5). Expect at boot: `startup: broadcast grid demote sent (... burst 1 of 4 of the phase sweep)` and bursts 2..4 two seconds apart, then the logins.
2. **Entities.** `python tools/hubctl.py state session` : both nodes `Session State = confirmed`, `Session Stale = OFF`, `Session Recovery Time` a few seconds, counters small. `python tools/hubctl.py press "RollladenWohnzimmer2 Dump Session Log"` : `session log: N entries` and the numbered lines appear in the hub log.
3. **The restart soak** (this is the open issue 1 check, runbook 3b, now measured by the hub itself):
   `python tools/restart_soak.py --rounds 10 --csv logs/<session>/restart_soak.csv`
   It waits 120 s between rounds; raise `--settle` to ~200 if Mode B has not re-established by the next restart (the test is only meaningful with the nodes on the grid). Afterwards `python tools/restart_soak.py --analyze logs/<session>/hub.log` must agree with the live numbers.
4. Per restart, on node 2's serial log: `GridDemote: grid WITHDRAWN by the hub — back to Mode A` within ~8 s of the hub boot (that is C working), then the LOGIN accepted.

**Pass**: every recovery of both nodes <= 30 s over 10 restarts, no `SESSION STALE`, `Mode B RE-ESTABLISHED` by itself after each. Before these changes the one measured restart took 2 min 22 s (node 1) and 3 min 28 s (node 2).

**If a node is slow**, the hub now says why without more instrumentation: the `SESSION STALE` warning is followed by that node's event log. Read it for: how the LOGINs were sent (`arg` of `LOGIN sent`: 0 burst, 1 aimed after an uplink, 2 placed on the REGISTER mark, 3 phase sweep), whether uplinks arrived in between (`uplink without a session`), and REGISTER refusals. Node 2's serial shows whether the demote was heard; a `GridDemote: rate-limited` line means the node honoured a demote less than an hour ago and is ignoring this one (node rule, once per hour) — space the restarts or note it as a finding.

**Also check** (regressions the sweep could cause): `tx anomalies ... placed late` does not grow faster than before; no `No free buffers` at boot (the four demote bursts are spread over 6 s for that reason); the first node's login is not delayed by more than ~2 s compared with before.

## 4. Hardware, step 2: node session report (D) — node 2 first

Order as always: node before hub; **node 2 (serial) first, node 1 (OTA) only after node 2 has run a day and only when the user says so** (node 1 cannot be recovered over serial and has no rollback).

1. Flash node 2 with 1.1.59 (runbook section 2 step 4: bootloader + app only, never erase NVS/storage). Boot banner `App version: 1.1.59`, beacon `fw=10159`.
2. Sanity: session comes back (`Login acknowledged`), Mode B re-establishes, one cover op each way acked (wait ~85 s between ops).
3. Trigger a REGISTER cycle: a config-change hub flash (runbook 3a) or an RTS reset of node 2 (`python tools/benchlog.py --reset node`, a POWERON reset). Expected in the hub log, once: `Node reports its session path: 1 REGISTER sent, LOGIN accepted after ~6000 ms (copy K of the burst), register_seq save failures 0`. Compare with node 2's own serial log (REGISTER line to `CMD_LOGIN from peer`): the wait must match within the log resolution.
4. A plain `Hub Restart`: the report reads `0 REGISTER sent, LOGIN accepted after 0 ms (copy K ...)` (no REGISTER, nothing to time) — or nothing new if K did not change.
5. Regression campaign on node 2 (runbook section 3): turnaround and Mode B numbers against `docs/project-state.md` section 6. The node change adds no work to the receive path, so the numbers must stay inside the usual spread.
6. Node 1 by OTA only after the above, then the hub is already current. First beacon `fw=10159`; its report after the next REGISTER cycle is the first time node 1's REGISTER count and LOGIN wait are visible at all.

**Pass**: the report appears once per change (not on every beacon), matches the serial log, and a beacon with nothing to report has the same length as on 1.1.58.

## 5. Rollback

- Hub: flash `main` (`9a901ad`). The new entities disappear from Home Assistant (they show as unavailable until removed); nothing is persisted by A-C (no NVS layout change, `kRestoreStateVersion` untouched).
- Node 2: serial-flash 1.1.58. No NVS or RTC layout changed in 1.1.59. A hub on this branch treats a 1.1.58 node as "no report".

## 6. What to write down, and where

`logs/<timestamp>_sessdiag/notes.txt` per session as usual. When done, update `docs/project-state.md`: section 2 (HEADs, fw), section 6 (a table row with the restart-soak numbers: per node n, median, max, stale periods), section 7 item 1 (close it or restate what is left), and delete or archive this file. Merge the branches to `main` only after step 3 passed (hub) and step 4 passed (node), and only when the user asks.

## 7. Known limits of what was tested without hardware

- The sweep table is proven against the node's real receive window in the host tests for 60 phases, but the hub's actual fire-time jitter (`placed late`) is not modelled: that is what the 10 restarts are for.
- `Session Stale` can be raised by a forged keyless frame (unauthenticated by nature); nothing acts on it.
- The counters and the event log are RAM: a hub reboot clears them. The soak reads them before the next restart for that reason.
- `tools/restart_soak.py` live mode was never run against a hub; only its parsing is self-tested. The entity-name matching accepts both `sensor-<node>_<entity>` and `<Node> <Entity>` forms; if it reports `no 'Session State' entity`, print `python tools/hubctl.py state session` and adjust `find_state`.

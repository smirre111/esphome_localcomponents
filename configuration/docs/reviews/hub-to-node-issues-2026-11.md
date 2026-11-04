# Hub -> node notes (hub agent, 2026-10-25)

## Node 05696bd (stage 3): not a regression, but the handoff undercounts the hub impact

Against node d3eb776/05696bd the hub proto_sim suite had **83** failures, not the
12 listed in node-handoff.md stage 3 (79 RealNodeFixture.*, Seam.AGridAligned...,
2 Irq.*, RealCmdDispatcherFresh.AdmitFrame_AnUnprovisioned...). All are the
designed consequence of N10 (a sessionless node refuses plaintext
OPERATION/SYSOP/TIMESYNC/SCHEDULE/MODETEST/MACCONTROL); none is a node bug.

Fixed on the hub side (hub commit "proto_sim: node-side tests run in a session
where node N10 requires one"): RealNodeFixture::useSession() + auto-seal in
pack_op, Seam::bringUpSession() (real LOGIN round trip), Irq irqSession()
(K_AUTH LOGIN with CONFIG_BLINDS_LORA_FLEET_KEY). Suite: 1761 pass against 05696bd.

Please document (node side):
- in the N10 commit/handoff/docs: which test fixtures must hold a session now,
  and that the hub repo carries node-behaviour tests (real_cmd_dispatcher_test,
  seam_test, frtos_tasks_test) -- measure the hub suite against the node HEAD
  (BLINDS_ESP_ROOT=<snapshot>) and list the actual failures;
- that a node test that wants a K_AUTH LOGIN must use the host build's fleet
  phrase (shims_node/sdkconfig.h, "ProtoSimTestKeyNotReal"), not "LoRaHome".

## Hub follow-ups already done for your stage 3
- P3: hub now fails a request at once on MacAck status != ACK_OK (no retries, no
  relogin) -- hub b53650f.
- P5/HB4: hub no longer acts on a plaintext MacAck / PhaseReport / beacon before
  a session -- hub 85ac778.
- ClientConfig sleep_duration: hub schema now 60..604800 s (= your clamp).

---

## 2026-11-02 — review rollout-3 D1: ClientConfig / CoverConfig are now ACKED by the hub (branch hub-fix3)

**What the hub does now (hub-fix3, D1 commit):** the encrypted ClientConfig and CoverConfig to a node
holding a session go out on the hub's new setup channel as ACKED requests: FLAGS.ACK_REQ = 1, placed,
same bytes re-sent up to 3 times 5 s apart. The hub records the node in sync (and persists the config
hash) only on the acks. The plaintext ClientConfig/CoverConfig to an UNPROVISIONED node is unchanged
(ACK_REQ = 0, fire-and-forget) and is re-sent encrypted + acked at that node's first session.

**Node today (87b3161) already works with this, with one warning per frame:**
- `MacReceiver::ackExecuted` (main/MacReceiver.cpp:184-191) follows the header's ACK_REQ (P3) and
  acks, because `AppController::handleClientConfig` / `handleCoverConfig` return
  `AppResult::Executed` (main/AppController.cpp:705, :1002).
- But `portmap::ackedWhenExecuted` says false for both, so every such frame logs
  `case %d arrived with ACK_REQ=1, expected 0 -- following the header`.
- A ClientConfig for another MAC returns NotExecuted -> no ack -> the hub retries 3 times and then
  degrades (treated as in sync for this hub boot, the acknowledged hash NOT moved, no relogin).

**Requested node change (small, no wire change):**
1. `main/include/PortMap.h:46-47`:
   `{DL_CLIENTCONFIG, mac::Port::App, true},` and `{DL_COVERCONFIG, mac::Port::App, true},`
   (the hub expects an ack for these kinds now).
2. `main/AppController.cpp:756` comment ("config pushes (ClientConfig/CoverConfig) are not acked
   either") is stale: only TimeSync stays unacked.
3. Test (node host suite): an authenticated ClientConfig (own MAC) with ACK_REQ=1 yields exactly one
   MacAck with ACKED_FCNT = its FCNT and no "expected 0" warning; same for CoverConfig; a
   ClientConfig naming another MAC yields no ack.
4. Re-export the PortMap mirror if it is vendored/mirrored; the hub's `mirror_tables_test` pins the
   hub route table (HubWire.cpp:29-30, still `false`) against `portmap::ackedWhenExecuted`. When the
   node change lands the hub flips HubWire.cpp:29-30 to `true` in the same step (hub side, then the
   per-request ACK_REQ override for the setup channel becomes a no-op).

## 2026-11-04 — rollout-4: the LOGIN after a REGISTER misses a Mode B node (hub branch hub-regfix)

**Observed (logs/20261103_163727_review_rollout4, hub f0c5929, fw 1.1.57).** After a hub flash that
changed the config hash, node 1 (Mode B before the flash) needed 368 s to log in: 7 request_register
LOGINs before its REGISTER (16:53:49..16:55:43), then 5 REGISTER cycles 60 s apart
(16:55:45..16:59:46) with 4 unanswered LoginMsg bursts each. Node 2 logged in on the first
post-REGISTER LOGIN both times, but only because it had already demoted to Mode A (16:50:12
`timed RX: reason 4 ... Grid arming off`; at 16:53 it was still in Mode A). The hub's startup
GridDemote is ignored by the node (`GridDemote: rate-limited ... (min 3600000 ms)`,
MacManagement.h:127), so a node keeps its old-grid Mode B windows across a hub reboot.

**Hub side (done on hub-regfix):** every LOGIN of a challenge started by a REGISTER is now the
normal burst with copy 0 placed on the node's mark (REGISTER T0 - ulOffset + whole rounds, after
the hub's queue drains). This relies on the node sending its REGISTER aimed at mark + ulOffset
(`Mac0Access::uplinkCadStartUs`, retry_count 0, phase trusted); when it is not aimed the burst
still covers a free-running window, as before.

**Proposed node change (not required by the hub fix; makes it independent of the uplink aim):**
1. When `SessionEntity::sendRegister()` gives up the session, also stop grid arming until the next
   accepted CMD_LOGIN (back to the free-running Mode A window, as phase loss does:
   `LoraInterface` "Grid arming off — back to the free-running RX window"). A node that is
   re-registering has, by definition, no session with the hub that owns the grid it is armed on,
   and the hub demotes it at the LOGIN anyway (`Hub reports it rebooted — demoting to Mode A`).
   Cost: Mode A duty (~6 % RX) for the seconds until the LOGIN lands, instead of minutes of Mode B
   misses.
2. Same idea for the request_register LOGINs that precede the REGISTER (the slow first 116 s for
   node 1): the hub has no fresh uplink stamp then. A node in Mode B whose mark windows have caught
   nothing addressed to it for N rounds already demotes (reason 4, took ~97 s on node 2 at 16:50);
   a shorter N while no session-proving downlink has arrived since the last hub GridBeacon would cut
   that. Needs a node-side decision on N (power vs recovery time).
3. Test (node host suite): after `sendRegister()` the node's next armed window is the free-running
   one (no mark arming) until a LOGIN is accepted; a LOGIN arriving in a Mode A window is adopted.

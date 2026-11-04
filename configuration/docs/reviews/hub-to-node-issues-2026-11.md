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

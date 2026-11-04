# Node -> hub handoff (2026-10-25 review fixes)

## 000000. D1 (2026-11-02) -- branch **node-portmap**, HEAD **747c88a** (fw 1.1.57), from main 87b3161. NOT on main.

Image: scratchpad\images\node_portmap_1.1.57_747c88a.bin (not flashed). Commits: f1ebac9 (change), 747c88a (fw).
Node:
- PortMap: DL_CLIENTCONFIG / DL_COVERCONFIG acked = true. New `portmap::ackReqExpected(cmd, authenticated)` =
  acked && sealed (your rule); the receiver warns only when the header disagrees with THAT, so your sealed acked
  config and your plaintext bootstrap (ACK_REQ=0) are both silent. The ack still follows the header, after the
  handler executed; a ClientConfig for another MAC is not executed -> no ack.
- **Behaviour change (why 1.1.57):** a SEALED ClientConfig is now in the MAC-1 sequence
  (`frameadmit::sequenceCheckApplies(kind, was_encrypted)`). Before, ClientConfig was always exempt (because the
  plaintext one skips the address filter), so every burst copy of your now-acked ClientConfig was re-executed
  (LittleFS save) and re-acked. Now: first copy executed + acked, copies inside the burst span silent, your retry
  after >2 s re-acked (ack cache). The plaintext bootstrap ClientConfig stays unsequenced. Your downlink FCNTs are
  monotonic, so nothing of yours needs to change for this.
- No wire change (goldens, MacWireVectors unchanged).

**Hub changes needed** (verified on a copy of your current tree, hub-fix3 at e098356):
1. `local_components/lora_client/HubWire.cpp:29-30`: DL_CLIENTCONFIG / DL_COVERCONFIG route `ack_req` false -> **true**
   (your mirror_tables_test MirrorTables.EveryDownlinkCaseHasThePortAndAckRuleTheNodeHas fails until you do).
2. `local_components/lora_client/lora_client_session.cpp:1488`: `ackReqFlag(r.ack_req, ack_req)` ->
   `ackReqFlag(r.ack_req && seal, ack_req)` -- a route's ACK_REQ on sealed frames only, so the plaintext bootstrap
   ClientConfig/CoverConfig stays ACK_REQ=0 (matches the node's ackReqExpected). Other acked routes are always sealed.
3. `tests/proto_sim/scenarios/mac_wire_v1_test.cpp:168-169` (HubWireRoute.EveryAckedDeliveryAndOnlyThoseAskForAMacAck):
   the two rows -> `true`.
4. Golden `hub_encrypted_fire_pushes` (golden_frames_test.cpp:261, GoldenHubFrames.EncryptedFirePushes): the sealed
   ClientConfig at fcnt 5 sent by a direct `send_remote_config()` after the session (the FIRE path, not your acked
   setup channel) now carries ACK_REQ=1. Decide: either route every sealed config through the acked setup channel
   (then that send is acked and the golden is re-recorded), or keep that Fire send and re-record with ackreq=1 (the
   node will ack it; you would ignore the ack). With 1-3 applied and only this golden differing, everything else
   passes (1804/1806 before re-recording: the route row test until (3), and this golden).

## 00000. 2026-11-01 23:00 nightly SLEEP "UNCONFIRMED" on both nodes -- NOT a node regression (no node change)

Evidence: logs/20261031_164433_review_rollout2 (node.log = node 2 serial, api.log = hub).
- **Node 2 slept correctly.** node.log 18910-18952: SLEEP (SYS OP 4, FCNT 88) received 23:00:02.85, ACK queued,
  the usual ~3 s drain wait (identical to fw 1.1.4: logs/20260926_150533_mac1/node.log: 'Enabling timer wakeup',
  'Sys command: Status!', 'No tasks in queues', 'Sleep marker armed'), `esp_deep_sleep_start()` at 23:00:05.96,
  next line a DEEPSLEEP_RESET boot at 05:00:05 (6 h). The windows after 'Enabling timer wakeup' are that drain wait.
- **Its ACK was lost because the hub was transmitting.** Node 2's CAD saw the channel busy at 23:00:04.51 and sent the
  ACK in a gap at 23:00:04.719 -- inside the hub's 17-copy fallback burst to NODE 1 (api.log 18041-18060,
  23:00:04.097-05.501). The hub is deaf while it bursts; its node-2 fallback burst at 23:00:07 reached a sleeping node.
- **Node 1 never acked because it was deaf** (review-ota-hang: hub tx_id for node 1 still 1078 = inside node 1's
  restored replay floor), so it dropped the SLEEP and kept its 15-min battery reports (23:01:05). That missing ack is
  what triggered the node-1 burst that ate node 2's ack.
**Hub actions:** (1) the review-ota-hang FCNT-jump advance (fixes deployed <=1.1.55 nodes; node side fixed on
node-boot-fix); (2) do not start a fallback burst for node A while node B's tracked single-shot ack window is open
(or stagger the nightly SLEEPs: send the next node's SLEEP only after the previous one is acked or has failed) --
otherwise one unanswering node makes every other node's SLEEP ack collide with the hub's own burst.

## 0000. BOOT FIX (2026-11-01) -- branch **node-boot-fix**, HEAD **87b3161** (fw 1.1.56), from main 0faa6bc. NOT on main.

Image: scratchpad\images\node_bootfix_1.1.56_87b3161.bin (not flashed). Local commits only.
- **f15080b** review-ota-hang: an F-5 resume (unexpected reboot incl. an OTA restart, provisioned, persisted
  state) now arms the resume fallback like a deep-sleep resume: boot beacon, 2 re-sends ~4 s apart if no
  downlink decrypts, then REGISTER after 12 s. Boot decision is a pure policy (bootpolicy::bootSession /
  armsResumeFallback in BootPolicy.h). Audit: no other boot/restore path resumes from a restored replay floor
  (deep-sleep resume already armed; REGISTER paths restart counters at LOGIN; the OTA-trigger boot runs no session).
- **86a24bd** RTC_NOINIT: one layout-tagged block `g_noinit` {layout, sleep_marker, mode_marker, ota_trigger}
  (NodeNoinit.{h,cpp}); a foreign/garbage block is reset at boot (rtc_ram=LOST, never an OTA); the OTA trigger
  is the 32-bit magic kOtaTriggerArmed. First boot after an OTA from <=1.1.55 reads rtc_ram=LOST once (harmless).
- **87b3161** fw 1.1.56.

Hub-visible changes: **none** (no wire, NVS or timing change). For you:
- `NodeNoinit.cpp` is a new entry in NODE_DISPATCHER_SOURCES (staged and compiled automatically; it defines only
  `g_noinit`). Your `extern_stubs.cpp` definition of `uint8_t trigger_ota` is now unused by node code (harmless;
  delete it whenever convenient). BootPolicy.h gained functions; your boot_policy_test is unaffected.
- What you will see on air after a node soft-reset whose beacon you miss: the same beacon up to twice more
  (~4 s and ~8 s later) and, if none of your downlinks decrypt within 12 s, a REGISTER (the normal handshake).
  The hub-side safety net in review-ota-hang.md (advance tx on a >=64 FCNT jump) is still worth having for
  nodes on <=1.1.55.
Verified: node host suite 27/27, goldens + MacWireVectors unchanged, layer_lint OK, IDF 6.0 build clean, your
proto_sim (copy of your current tree) 1785/1785 at every commit.

## 000. ROUND 4 (2026-10-31, on top of 5126acb) -- DONE. main HEAD **e34fdae** (fw stays 1.1.55: no behaviour change); branch **review-n19** final HEAD **c755193** = b69e7fc + e9933a1 + c755193 (needs you first)

Each commit: node suite green, goldens/vectors unchanged, layer_lint OK, firmware builds, your
proto_sim (copy with the mirror overlaid) green. No hub change needed unless a line says so.

**CORRECTION (read this):** several commit messages and lines below say "your copy 1777/1778, the failure is
layer_lint_hub / LORAClient errors from your own uncommitted lora_client WIP". That was WRONG: my check overlaid a
STALE snapshot of your lora_client/ onto your live tree. With a fresh snapshot, main **e34fdae** against your current
tree: BUILD OK, proto_sim **1778/1778** (layer_lint_hub included). Nothing on main needs a hub change.

- **85ecfc3 N27 MacManagement** (`main/MacManagement.{cpp,h}`, in node_dispatcher_sources.cmake).
  GridSync/GridBeacon/GridDemote/MacControl handlers + MacCounters, restore timer, demote rate limit
  moved. Removed from CmdDispatcher: handleGridSync, handleGridBeacon, handleGridDemote,
  handleMacControl, applyMacConfig_, beaconMacIsValid, demoteGrid_, withdrawGrid_, adoptGrid*_,
  sublayerRestoreCb_, mac_, sublayer_restore_timer_, last_grid_demote_ms_ (none used by your tests).
  Kept: macCounters(), resetMacCounters(), CmdDispatcher::MacCounters (alias), hasNetKey().

- **66d0721 N27 SessionEntity** (`main/SessionEntity.{cpp,h}`, in node_dispatcher_sources.cmake).
  handleLogin/switchSession_/noteVerifiedLogin_, REGISTER retry, resume fallback, beacon ladder,
  noteSessionProven + proven hook, session half of loadPersistentState. Removed from CmdDispatcher
  (none used by your tests): handleLogin, noteVerifiedLogin_, switchSession_, SessionSwitch,
  *RetryCb/resumeFallbackCb, armBeaconRetry/cancelBeaconRetry, startRegisterRetry_, hasHubSession_,
  resume_timer_, beacon_retry_timer_, beacon_retries_, register_retry_timer_, last_login_ms_,
  last_login_resend_ms_, session_proven_hook_*, kBeaconAckMs, kBeaconRetryJitterMs, kLogin*.
  Kept as forwards: sendRegister, isProvisioned, armRegisterRetry, cancelRegisterRetry,
  canResumeSession, armResumeFallback, noteSessionProven, setSessionProvenHook,
  sessionProvenHookFiredForTest, loadPersistentState, kRegisterRetryMs, kResumeFallbackMs,
  kMaxBeaconRetries.

- **cfa76be N27 MacReceiver** (`main/MacReceiver.{cpp,h}`, in node_dispatcher_sources.cmake).
  receiveFrame/openFrame_/openSessionFrame_, admitFrame + all stages, the port demultiplexer, the MAC
  acks, maybeRequestGridSync, sendCommandAck; all per-frame state (rx_frame_, rx_auth_, rx_kauth_ok_,
  rx_ack_req_, rx_cmd_id_, last_cmd_id_, last_error_fcnt_, error_reacks_, rx_rssi_, mac_ping_seq_hw_).
  Kept on CmdDispatcher: onReceiveNew, sendCommandAck, maybeRequestGridSync,
  forceNextAppNotExecutedForTest, handleSysop(msg, hdr), AppIndication/AppResult (aliases of
  macrx::), and **DEPRECATED** `uint32_t &mac_ping_seq_hw_` (a reference into the receiver) for your
  real_cmd_dispatcher_test.cpp:244 `using CmdDispatcher::mac_ping_seq_hw_;` and :8173
  `disp.mac_ping_seq_hw_ = 100;` -- compiles unchanged; please move to a seam when N19 lands.
  Dead `noteBurstArrival_` removed. Your copy: 1777/1778, the one failure is layer_lint_hub
  (client.msgid_alloc_sites 1->2) from your own uncommitted lora_client edits.

- **058da48 N24 NodeRtcState** (`main/NodeRtcState.{cpp,h}`, in node_dispatcher_sources.cmake): battery cache,
  mode-announce record, auto-mode/interactive RTC facts. CmdDispatcher API unchanged.

- **ffedde3 N27 TelemetryBuilder** (`main/TelemetryBuilder.{cpp,h}`, in node_dispatcher_sources.cmake):
  serviceTxCommand's bodies, one function per uplink case. serviceTxCommand / runOneTxCommand unchanged.
  factoryMac64() is now a free function declared in TelemetryBuilder.h.

- **b186549 N27 AppController** (`main/AppController.{cpp,h}`, in node_dispatcher_sources.cmake): the six handlers,
  auto mode + auto-sleep, interactive override, clock retry. Every CmdDispatcher method your tests call is a
  forward (handleSysop both forms, enter/clearInteractiveMode, isTemporarilyInteractive, interactiveRemaining,
  shouldRunAutoMode, computeNextEvent, computeSleepSeconds, runDueScheduleEntry, armAutoSleep, autoSleepQuietMs).

- **a21d8e5 N24 NodeGlobals.h**: the singletons declared once; SystemCtrl gets the radio injected. frtosTasks.cpp
  now includes NodeGlobals.h (found via main/include; nothing for you to change). Removing the globals from
  frtosTasks needs your frtos_tasks_test changed -- see the review-n19 list.

- **e34fdae N29 BenchDiag.h**: RadioMacPort loses noteModeTestArmResidual/noteModeTestOneShotError and gains
  benchDiag() (a BenchDiagSink*, implemented by BenchTools). No fake RadioMacPort exists in your tests, so nothing
  to change; CmdDispatcher::noteModeTest* still exist as plain methods.


### review-n19 (NOT on main -- adapt your proto_sim in ONE pass against c755193, then tell me / the coordinator and we merge)

Node commits on branch `review-n19` (on top of main e34fdae), local only. Final HEAD **c755193**.
Image: scratchpad\images\node_review_1.1.55_n19_c755193.bin (not flashed).
- **b69e7fc N19:** `CmdDispatcher` public = production API only; everything else private (177 names) with
  `friend class CmdDispatcherTestPeer;`. `main/include/CmdDispatcherTestPeer.h` (test-only) derives from
  CmdDispatcher and re-publishes every private member by using-declaration; it is in
  NODE_DISPATCHER_STAGED_HEADERS, so your NODE_STAGE and IFACE_STAGE get it automatically (no CMake change).
  Also removed: the deprecated `modeTestExpiredCb_` / `mode_test_active_` shims (you no longer use them).
- **e9933a1 N26 follow-up:** `MacFrameHdr::{destaddress,destsubnet,senderaddress,burstindex,burstcount}` are
  `uint8_t` (msgid/fireround/fireoffsetus/sessionnonce stay u32). Wire bytes unchanged.
- **c755193 N24 step 5:** frtosTasks.cpp (task bodies, serviceDio0Event/serviceDio1Event, battery and
  motor-current tasks) no longer reads the motCtrl/sysCtrl/loraIf/cmdDispatcher globals. It works on a
  `frtostasks::Context {motor, sys, radio, disp}` bound once with `frtostasks::bind(...)` (declared in
  frtosTasks.h; main.cpp binds right after creating the objects). `bind({})` unbinds; a context with
  `disp = nullptr` is "dispatcher not wired yet" (the null guards are unchanged). The globals still exist
  (MotorCtrl/Buttons C callbacks use them; your extern_stubs.cpp definitions can stay), but assigning them
  no longer affects frtosTasks -- your Irq fixture must bind instead (26 Irq tests fail until it does).

**Exact hub changes, all of them** (generated by applying them to a copy of your current tree; with them:
BUILD OK, proto_sim 1778/1778):
1. `local_components/lora_client/HubFrames.h` struct `Hdr`: the same five fields `uint32_t` -> `uint8_t`
   (your mirror_tables_test pins the two layouts).
2. `scenarios/frtos_tasks_test.cpp` Irq fixture: `frtostasks::bind(...)` instead of assigning the four globals
   (lines 116-119, 122-123, 1103; see the list).
3. Four test files, all mechanical, for N19: include `CmdDispatcherTestPeer.h` after `CmdDispatcher.h`;
   construct `CmdDispatcherTestPeer` where you construct `CmdDispatcher`; probes (`NodeProbe`, `DispProbe`)
   derive from `CmdDispatcherTestPeer` and their `using CmdDispatcher::X;` become
   `using CmdDispatcherTestPeer::X;`; helper parameters / pointers to a test's dispatcher take the peer;
   static calls of now-private members (`resetModeAnnounceState`, `lastReasonAnnounced`, ...) name the peer.
   The `cmdDispatcher` global stays `CmdDispatcher *`.

Line by line, against your current tree (`-` your line, `+` the replacement, or "delete the line"). The
frtos_tasks_test.cpp fixture lines are applied before the N19 rewrites; line numbers are of your
unmodified file:
```
scenarios/frtos_tasks_test.cpp:116
    - motCtrl       = &mot;
    + frtostasks::bind({&mot, &sys, &lif, &disp});   // as main.cpp binds them (N24)
scenarios/frtos_tasks_test.cpp:117
    - sysCtrl       = &sys;
    (delete the line)
scenarios/frtos_tasks_test.cpp:118
    - loraIf        = &lif;
    (delete the line)
scenarios/frtos_tasks_test.cpp:119
    - cmdDispatcher = &disp;
    (delete the line)
scenarios/frtos_tasks_test.cpp:122
    - motCtrl = nullptr; sysCtrl = nullptr;
    + frtostasks::bind({});
scenarios/frtos_tasks_test.cpp:123
    - loraIf = nullptr;  cmdDispatcher = nullptr;
    (delete the line)
scenarios/frtos_tasks_test.cpp:1103
    - cmdDispatcher = nullptr;
    + frtostasks::bind({&mot, &sys, &lif, nullptr});   // the dispatcher not wired yet
scenarios/e2e_test.cpp:41
    - #include "CmdDispatcher.h"
    + #include "CmdDispatcher.h"
#include "CmdDispatcherTestPeer.h"   // review N19: private members via the peer
scenarios/e2e_test.cpp:91
    - CmdDispatcher disp{&mot, &sys, &lif, motorMux, buttonMux};
    + CmdDispatcherTestPeer disp{&mot, &sys, &lif, motorMux, buttonMux};
scenarios/frtos_tasks_test.cpp:38
    - #include "CmdDispatcher.h"
    + #include "CmdDispatcher.h"
#include "CmdDispatcherTestPeer.h"   // review N19: private members via the peer
scenarios/frtos_tasks_test.cpp:94
    - struct DispProbe : CmdDispatcher {
    + struct DispProbe : CmdDispatcherTestPeer {
scenarios/frtos_tasks_test.cpp:97
    - : CmdDispatcher(m, s, l, mm, bm) {}
    + : CmdDispatcherTestPeer(m, s, l, mm, bm) {}
scenarios/frtos_tasks_test.cpp:385
    - void openMarkWindowAt(Probe &lif, CmdDispatcher &disp, int64_t start_us,
    + void openMarkWindowAt(Probe &lif, CmdDispatcherTestPeer &disp, int64_t start_us,
scenarios/frtos_tasks_test.cpp:728
    - CmdDispatcher *disp{nullptr};
    + CmdDispatcherTestPeer *disp{nullptr};
scenarios/frtos_tasks_test.cpp:785
    - void irqSession(CmdDispatcher &disp) {
    + void irqSession(CmdDispatcherTestPeer &disp) {
scenarios/frtos_tasks_test.cpp:797
    - CmdDispatcher &d = *g_irq_session.disp;
    + CmdDispatcherTestPeer &d = *g_irq_session.disp;
scenarios/real_cmd_dispatcher_test.cpp:26
    - #include "CmdDispatcher.h"
    + #include "CmdDispatcher.h"
#include "CmdDispatcherTestPeer.h"   // review N19: private members via the peer
scenarios/real_cmd_dispatcher_test.cpp:163
    - CmdDispatcher *disp{nullptr};
    + CmdDispatcherTestPeer *disp{nullptr};
scenarios/real_cmd_dispatcher_test.cpp:167
    - std::vector<uint8_t> seal_op(CmdDispatcher &disp, const DownlinkView &inner, const DlHdr &h,
    + std::vector<uint8_t> seal_op(CmdDispatcherTestPeer &disp, const DownlinkView &inner, const DlHdr &h,
scenarios/real_cmd_dispatcher_test.cpp:188
    - std::vector<uint8_t> seal_op(CmdDispatcher &disp, const DownlinkView &inner, const DlHdr &h,
    + std::vector<uint8_t> seal_op(CmdDispatcherTestPeer &disp, const DownlinkView &inner, const DlHdr &h,
scenarios/real_cmd_dispatcher_test.cpp:240
    - struct NodeProbe : CmdDispatcher {
    + struct NodeProbe : CmdDispatcherTestPeer {
scenarios/real_cmd_dispatcher_test.cpp:243
    - : CmdDispatcher(m, s, l, mm, bm) {}
    + : CmdDispatcherTestPeer(m, s, l, mm, bm) {}
scenarios/real_cmd_dispatcher_test.cpp:244
    - using CmdDispatcher::mac_ping_seq_hw_;       // the MAC ping high-water mark
    + using CmdDispatcherTestPeer::mac_ping_seq_hw_;       // the MAC ping high-water mark
scenarios/real_cmd_dispatcher_test.cpp:256
    - using CmdDispatcher::simulateWakeForTest;
    + using CmdDispatcherTestPeer::simulateWakeForTest;
scenarios/real_cmd_dispatcher_test.cpp:257
    - using CmdDispatcher::packUplinkFrame_;   // the v1 seal point, for the msgid guard
    + using CmdDispatcherTestPeer::packUplinkFrame_;   // the v1 seal point, for the msgid guard
scenarios/real_cmd_dispatcher_test.cpp:304
    - CmdDispatcher::resetModeAnnounceState();
    + CmdDispatcherTestPeer::resetModeAnnounceState();
scenarios/real_cmd_dispatcher_test.cpp:740
    - CmdDispatcher disp(&mot, &sys, &lif, motorMux, buttonMux);
    + CmdDispatcherTestPeer disp(&mot, &sys, &lif, motorMux, buttonMux);
scenarios/real_cmd_dispatcher_test.cpp:758
    - CmdDispatcher disp(&mot, &sys, &lif, motorMux, buttonMux);
    + CmdDispatcherTestPeer disp(&mot, &sys, &lif, motorMux, buttonMux);
scenarios/real_cmd_dispatcher_test.cpp:777
    - CmdDispatcher disp(&mot, &sys, &lif, motorMux, buttonMux);
    + CmdDispatcherTestPeer disp(&mot, &sys, &lif, motorMux, buttonMux);
scenarios/real_cmd_dispatcher_test.cpp:1229
    - void give_clock(CmdDispatcher &disp, uint32_t msgid, uint64_t epoch, int32_t offset) {
    + void give_clock(CmdDispatcherTestPeer &disp, uint32_t msgid, uint64_t epoch, int32_t offset) {
scenarios/real_cmd_dispatcher_test.cpp:1572
    - bool drain_for_sleep(CmdDispatcher &d) {
    + bool drain_for_sleep(CmdDispatcherTestPeer &d) {
scenarios/real_cmd_dispatcher_test.cpp:1582
    - bool drain_for_sleep_after_quiet_window(CmdDispatcher &d) {
    + bool drain_for_sleep_after_quiet_window(CmdDispatcherTestPeer &d) {
scenarios/real_cmd_dispatcher_test.cpp:1693
    - bool queueHoldsRegister(CmdDispatcher &d) {
    + bool queueHoldsRegister(CmdDispatcherTestPeer &d) {
scenarios/real_cmd_dispatcher_test.cpp:1955
    - int drain_beacons(CmdDispatcher &d) {
    + int drain_beacons(CmdDispatcherTestPeer &d) {
scenarios/real_cmd_dispatcher_test.cpp:2101
    - EXPECT_LE(total, CmdDispatcher::kMaxBeaconRetries)
    + EXPECT_LE(total, CmdDispatcherTestPeer::kMaxBeaconRetries)
scenarios/real_cmd_dispatcher_test.cpp:2221
    - EXPECT_GE(disp.autoSleepQuietMs(), CmdDispatcher::kAutoSleepQuietMinMs)
    + EXPECT_GE(disp.autoSleepQuietMs(), CmdDispatcherTestPeer::kAutoSleepQuietMinMs)
scenarios/real_cmd_dispatcher_test.cpp:2233
    - EXPECT_EQ(disp.autoSleepQuietMs(), CmdDispatcher::kAutoSleepQuietMinMs)
    + EXPECT_EQ(disp.autoSleepQuietMs(), CmdDispatcherTestPeer::kAutoSleepQuietMinMs)
scenarios/real_cmd_dispatcher_test.cpp:2417
    - CmdDispatcher d{&mot, &sys, &lif, mmux, bmux};
    + CmdDispatcherTestPeer d{&mot, &sys, &lif, mmux, bmux};
scenarios/real_cmd_dispatcher_test.cpp:2426
    - CmdDispatcher d2{&mot, &sys, &lif, mmux, bmux};
    + CmdDispatcherTestPeer d2{&mot, &sys, &lif, mmux, bmux};
scenarios/real_cmd_dispatcher_test.cpp:2456
    - CmdDispatcher d{&mot, &sys, &lif, mmux, bmux};
    + CmdDispatcherTestPeer d{&mot, &sys, &lif, mmux, bmux};
scenarios/real_cmd_dispatcher_test.cpp:2469
    - CmdDispatcher d2{&mot, &sys, &lif, mmux, bmux};
    + CmdDispatcherTestPeer d2{&mot, &sys, &lif, mmux, bmux};
scenarios/real_cmd_dispatcher_test.cpp:2503
    - CmdDispatcher disp{&mot, &sys, &lif, motorMux, buttonMux};
    + CmdDispatcherTestPeer disp{&mot, &sys, &lif, motorMux, buttonMux};
scenarios/real_cmd_dispatcher_test.cpp:2616
    - uint16_t arm_missed_entry(CmdDispatcher &d, SystemCtrl &s,
    + uint16_t arm_missed_entry(CmdDispatcherTestPeer &d, SystemCtrl &s,
scenarios/real_cmd_dispatcher_test.cpp:2648
    - int drain_closes(CmdDispatcher &d) {
    + int drain_closes(CmdDispatcherTestPeer &d) {
scenarios/real_cmd_dispatcher_test.cpp:3294
    - int64_t rxOnMark(const CmdDispatcher &d, size_t len, int64_t near_us, int64_t err_us = 0) {
    + int64_t rxOnMark(const CmdDispatcherTestPeer &d, size_t len, int64_t near_us, int64_t err_us = 0) {
scenarios/real_cmd_dispatcher_test.cpp:3576
    - disp.noteMarkArmed(CmdDispatcher::kNoWindowGen);
    + disp.noteMarkArmed(CmdDispatcherTestPeer::kNoWindowGen);
scenarios/real_cmd_dispatcher_test.cpp:3577
    - disp.noteMarkHit(CmdDispatcher::kNoWindowGen);
    + disp.noteMarkHit(CmdDispatcherTestPeer::kNoWindowGen);
scenarios/real_cmd_dispatcher_test.cpp:3657
    - int drain_acks(CmdDispatcher &d) {
    + int drain_acks(CmdDispatcherTestPeer &d) {
scenarios/real_cmd_dispatcher_test.cpp:4241
    - std::vector<uint8_t> encrypt_op(CmdDispatcher &disp, DownlinkView &inner,
    + std::vector<uint8_t> encrypt_op(CmdDispatcherTestPeer &disp, DownlinkView &inner,
scenarios/real_cmd_dispatcher_test.cpp:4257
    - std::vector<uint8_t> encrypted_sysop(CmdDispatcher &disp, uint32_t msgid, ClientOperation what,
    + std::vector<uint8_t> encrypted_sysop(CmdDispatcherTestPeer &disp, uint32_t msgid, ClientOperation what,
scenarios/real_cmd_dispatcher_test.cpp:4266
    - std::vector<uint8_t> encrypted_mode_test(CmdDispatcher &disp, ModeTest__Mode mode, uint32_t msgid,
    + std::vector<uint8_t> encrypted_mode_test(CmdDispatcherTestPeer &disp, ModeTest__Mode mode, uint32_t msgid,
scenarios/real_cmd_dispatcher_test.cpp:4301
    - std::vector<uint8_t> encrypted_mode_test_default_profile(CmdDispatcher &disp, uint32_t msgid) {
    + std::vector<uint8_t> encrypted_mode_test_default_profile(CmdDispatcherTestPeer &disp, uint32_t msgid) {
scenarios/real_cmd_dispatcher_test.cpp:4316
    - std::vector<uint8_t> encrypted_mode_test_off(CmdDispatcher &disp, uint32_t msgid) {
    + std::vector<uint8_t> encrypted_mode_test_off(CmdDispatcherTestPeer &disp, uint32_t msgid) {
scenarios/real_cmd_dispatcher_test.cpp:4325
    - std::vector<uint8_t> encrypted_grid_sync(CmdDispatcher &disp, uint32_t slot, uint32_t msgid,
    + std::vector<uint8_t> encrypted_grid_sync(CmdDispatcherTestPeer &disp, uint32_t slot, uint32_t msgid,
scenarios/real_cmd_dispatcher_test.cpp:4431
    - void holdSessionAndArmTest(CmdDispatcher &disp, bool counter_on, bool crypto_on = false) {
    + void holdSessionAndArmTest(CmdDispatcherTestPeer &disp, bool counter_on, bool crypto_on = false) {
scenarios/real_cmd_dispatcher_test.cpp:4442
    - std::vector<uint8_t> build_mac_ping_encrypted(CmdDispatcher &disp, uint32_t seq, bool want_echo,
    + std::vector<uint8_t> build_mac_ping_encrypted(CmdDispatcherTestPeer &disp, uint32_t seq, bool want_echo,
scenarios/real_cmd_dispatcher_test.cpp:5116
    - int64_t settle_on_grid(CmdDispatcher &disp, uint32_t slot, uint32_t first_msgid) {
    + int64_t settle_on_grid(CmdDispatcherTestPeer &disp, uint32_t slot, uint32_t first_msgid) {
scenarios/real_cmd_dispatcher_test.cpp:5151
    - const int64_t t0 = cad_start + CmdDispatcher::kUplinkAimLeadUs;
    + const int64_t t0 = cad_start + CmdDispatcherTestPeer::kUplinkAimLeadUs;
scenarios/real_cmd_dispatcher_test.cpp:5829
    - void deliverStamped(CmdDispatcher &disp, const std::vector<uint8_t> &f,
    + void deliverStamped(CmdDispatcherTestPeer &disp, const std::vector<uint8_t> &f,
scenarios/real_cmd_dispatcher_test.cpp:6180
    - void bringNodeToTheEdgeOfPromotion(CmdDispatcher &disp, uint32_t samples = 8) {
    + void bringNodeToTheEdgeOfPromotion(CmdDispatcherTestPeer &disp, uint32_t samples = 8) {
scenarios/real_cmd_dispatcher_test.cpp:6253
    - std::vector<CmdDispatcher::tx_command_t> drainTx(CmdDispatcher &d) {
    + std::vector<CmdDispatcher::tx_command_t> drainTx(CmdDispatcherTestPeer &d) {
scenarios/real_cmd_dispatcher_test.cpp:6259
    - std::vector<CmdDispatcher::tx_command_t> gridSyncRequests(CmdDispatcher &d) {
    + std::vector<CmdDispatcher::tx_command_t> gridSyncRequests(CmdDispatcherTestPeer &d) {
scenarios/real_cmd_dispatcher_test.cpp:6267
    - void framesOnMarks(CmdDispatcher &d, int64_t from_us, uint32_t count, int64_t err_us,
    + void framesOnMarks(CmdDispatcherTestPeer &d, int64_t from_us, uint32_t count, int64_t err_us,
scenarios/real_cmd_dispatcher_test.cpp:6341
    - void promoteAndSettle(CmdDispatcher &disp) {
    + void promoteAndSettle(CmdDispatcherTestPeer &disp) {
scenarios/real_cmd_dispatcher_test.cpp:6386
    - EXPECT_EQ(CmdDispatcher::lastReasonAnnounced(), (uint8_t) timedmode::Demotion::None)
    + EXPECT_EQ(CmdDispatcherTestPeer::lastReasonAnnounced(), (uint8_t) timedmode::Demotion::None)
scenarios/real_cmd_dispatcher_test.cpp:6412
    - ASSERT_EQ(CmdDispatcher::lastReasonAnnounced(), (uint8_t) timedmode::Demotion::None);
    + ASSERT_EQ(CmdDispatcherTestPeer::lastReasonAnnounced(), (uint8_t) timedmode::Demotion::None);
scenarios/real_cmd_dispatcher_test.cpp:6436
    - EXPECT_EQ(CmdDispatcher::lastReasonAnnounced(),
    + EXPECT_EQ(CmdDispatcherTestPeer::lastReasonAnnounced(),
scenarios/real_cmd_dispatcher_test.cpp:6459
    - ASSERT_EQ(CmdDispatcher::lastReasonAnnounced(),
    + ASSERT_EQ(CmdDispatcherTestPeer::lastReasonAnnounced(),
scenarios/real_cmd_dispatcher_test.cpp:6488
    - EXPECT_EQ(CmdDispatcher::lastReasonAnnounced(), settled)
    + EXPECT_EQ(CmdDispatcherTestPeer::lastReasonAnnounced(), settled)
scenarios/real_cmd_dispatcher_test.cpp:6519
    - ASSERT_EQ(CmdDispatcher::lastReasonAnnounced(), (uint8_t) timedmode::Demotion::None)
    + ASSERT_EQ(CmdDispatcherTestPeer::lastReasonAnnounced(), (uint8_t) timedmode::Demotion::None)
scenarios/real_cmd_dispatcher_test.cpp:6544
    - EXPECT_EQ(CmdDispatcher::lastReasonAnnounced(),
    + EXPECT_EQ(CmdDispatcherTestPeer::lastReasonAnnounced(),
scenarios/real_cmd_dispatcher_test.cpp:6604
    - const uint8_t told = CmdDispatcher::lastReasonAnnounced();
    + const uint8_t told = CmdDispatcherTestPeer::lastReasonAnnounced();
scenarios/real_cmd_dispatcher_test.cpp:6647
    - EXPECT_EQ(CmdDispatcher::lastReasonAnnounced(), told)
    + EXPECT_EQ(CmdDispatcherTestPeer::lastReasonAnnounced(), told)
scenarios/real_cmd_dispatcher_test.cpp:7119
    - void deliverStampedGridSync(CmdDispatcher &disp, uint32_t msgid, int64_t err_us) {
    + void deliverStampedGridSync(CmdDispatcherTestPeer &disp, uint32_t msgid, int64_t err_us) {
scenarios/real_cmd_dispatcher_test.cpp:7140
    - gridstate::State truthAt(const CmdDispatcher &disp, int32_t rate_ppb) {
    + gridstate::State truthAt(const CmdDispatcherTestPeer &disp, int32_t rate_ppb) {
scenarios/real_cmd_dispatcher_test.cpp:7235
    - void armModeBWithGrid(CmdDispatcher &disp) {
    + void armModeBWithGrid(CmdDispatcherTestPeer &disp) {
scenarios/real_cmd_dispatcher_test.cpp:7246
    - void feedModeBMarks(CmdDispatcher &disp, const gridstate::State &truth) {
    + void feedModeBMarks(CmdDispatcherTestPeer &disp, const gridstate::State &truth) {
scenarios/real_cmd_dispatcher_test.cpp:7315
    - bool reg_queued(CmdDispatcher &disp, uint32_t *count = nullptr) {
    + bool reg_queued(CmdDispatcherTestPeer &disp, uint32_t *count = nullptr) {
scenarios/real_cmd_dispatcher_test.cpp:7546
    - std::vector<uint8_t> admit_enc_sysop_copy(CmdDispatcher &disp, uint32_t msgid,
    + std::vector<uint8_t> admit_enc_sysop_copy(CmdDispatcherTestPeer &disp, uint32_t msgid,
scenarios/real_cmd_dispatcher_test.cpp:7558
    - void admit_deliver_on_mark(CmdDispatcher &d, const std::vector<uint8_t> &f, int64_t mark) {
    + void admit_deliver_on_mark(CmdDispatcherTestPeer &d, const std::vector<uint8_t> &f, int64_t mark) {
scenarios/real_cmd_dispatcher_test.cpp:7573
    - CmdDispatcher disp(&mot, &sys, &lif, motorMux, buttonMux);
    + CmdDispatcherTestPeer disp(&mot, &sys, &lif, motorMux, buttonMux);
scenarios/real_cmd_dispatcher_test.cpp:7599
    - CmdDispatcher disp(&mot, &sys, &lif, motorMux, buttonMux);
    + CmdDispatcherTestPeer disp(&mot, &sys, &lif, motorMux, buttonMux);
scenarios/real_cmd_dispatcher_test.cpp:7828
    - std::vector<uint8_t> mac_config_frame(CmdDispatcher &disp, uint32_t msgid, bool disable_crypto) {
    + std::vector<uint8_t> mac_config_frame(CmdDispatcherTestPeer &disp, uint32_t msgid, bool disable_crypto) {
scenarios/seam_test.cpp:31
    - #include "CmdDispatcher.h"
    + #include "CmdDispatcher.h"
#include "CmdDispatcherTestPeer.h"   // review N19: private members via the peer
scenarios/seam_test.cpp:84
    - CmdDispatcher disp{&mot, &sys, &lif, motorMux, buttonMux};
    + CmdDispatcherTestPeer disp{&mot, &sys, &lif, motorMux, buttonMux};
../../local_components/lora_client/HubFrames.h:35
    - uint32_t destaddress{0};
    + uint8_t  destaddress{0};
../../local_components/lora_client/HubFrames.h:36
    - uint32_t destsubnet{0};
    + uint8_t  destsubnet{0};
../../local_components/lora_client/HubFrames.h:37
    - uint32_t senderaddress{0};
    + uint8_t  senderaddress{0};
../../local_components/lora_client/HubFrames.h:39
    - uint32_t burstindex{0};
    + uint8_t  burstindex{0};
../../local_components/lora_client/HubFrames.h:40
    - uint32_t burstcount{0};
    + uint8_t  burstcount{0};
total changed lines: 90
```
The script that produced (and applied) this: scratchpad\node_agent\hub_n19_transform.py
`python3 hub_n19_transform.py <your tests/proto_sim> scratchpad\node_agent\n19_private_names.json`
(it also edits ../../local_components/lora_client/HubFrames.h relative to that directory).

## 00. ROUND 3 (2026-10-26..31, on top of pushed 083c0f8) -- DONE FOR NOW, FINAL HEAD **5126acb** (fw 1.1.55)

Image: scratchpad\images\node_review_1.1.55_5126acb.bin (not flashed, not in https_hosted).
Node suite 26/26 at every commit; IDF 6.0 build clean; layer_lint OK (mac1 5, txpath 1, ackpath 2,
everything else 0); MacWireVectors unchanged. Your proto_sim, copied with the mirror overlaid,
shows only your own WIP failures at every step: real_lora_client_test.cpp compile errors
(ModeBProbe `mode_test_`), MacWireV1Listener.{AReplayedRegisterIsRefusedEvenPastTheRateLimit,
ALegacyRegisterAfterASeqIsRefused, ARateLimitedRegisterStillRaisesTheHighestSeq}, and
layer_lint_hub (client.msgid_alloc_sites 2->3, client.mac1_counter_writes 7->9, i.e. your lora_client).
**Shared files:** regen_mirror reports NO differences between the mirror and your tree (stubs incl.
register_seq and MacSap.h are already in), so nothing to copy.

Commits after 1b0ee50 (all node-internal; nothing in your test code needs to change for them):
- **794b2a5 N28:** `set_base_nonce` saves through `savePersistentState()` (now under the session
  lock; reached only via `setBaseNonceForTest`); dead `maybePersist_()` removed.
- **2a9f26f N21:** `SessionManager::seq()` and `LinkSecurity::sublayers()` are const-only. New
  intent methods: `allocateTx()`, `commitRx(fcnt, now)`, `beginNewSession(hub, hub_nonce,
  node_nonce, superseded)`, `abandonSession(peer)`; `armControlSublayers(has_session, bench, cfg)`
  (applies only if no refusal), `setTestSublayers(cfg)`. The SessionManager forwards your
  session_manager_test uses (nextTxId, acceptRxId, resetCounters, ...) are unchanged. If any of your
  code calls `seq().allocate/commit/reset` or assigns `sublayers().x = ...` it must use these (I
  found none).
- **652bf91 N22:** Mac0Access.h no longer includes Mac1Freshness.h (it still arrives via MacSap.h /
  FrameAdmission.h). Comments only otherwise; log tag stays "CmdDispatcher".
- **ee4b165 NodeWallClock** and **ff60aed WakeForensics** (N24): new `main/NodeWallClock.cpp`,
  `main/WakeForensics.cpp` + headers, both in `test/node_dispatcher_sources.cmake` (you include it).
  CmdDispatcher::isClockValid/getUtcOffset/getDstNext/formatLocalTime unchanged (forwards).
- **5126acb fw 1.1.55.**

Still open (NOT done this round): N19 (private: + CmdDispatcherTestPeer -- will need your probes
changed in step), N29 (diag interface), the remaining N27 extractions (SessionEntity, MacManagement,
MacReceiver, AppController, TelemetryBuilder), the mode-announce / auto-mode / battery RTC statics
(rest of N24). The DEPRECATED shims from b925f6c (`modeTestExpiredCb_`, `mode_test_active_`) are
still there; please switch to `expireModeTestForTest()` / `setModeTestActiveForTest(true)` so they
can go.


Stage-5 commits so far (each: node suite 25/25, goldens/vectors unchanged, layer_lint OK, firmware
builds, your node-facing proto_sim green against a copy of your tree with the mirror overlaid):
- **06bb1a7 N26:** `MacSap.h` (shared) loses the unused Mac0DataReq/Ind, SecDataReq/Ind, TxOpts ->
  re-vendor MacSap.h (mirror). `linksec::fitsMacHdr` deleted. MacFrameHdr keeps u32 fields (your
  mirror_tables_test pins hubframes::Hdr to the same layout; narrowing both is a joint change if
  you want it). An uplink target > 0xFF is now refused before FCNT allocation.
- **cfc4eb4 N20:** new `RadioMacPort.h` (interface the radio uses; `RxBuffer` moved there,
  `CmdDispatcher::rx_buffer_t` is an alias). CmdDispatcher implements it (same method names, now
  `override`). `LoraInterface` holds `RadioMacPort *mac_` (was `CmdDispatcher *cmdDispatcher`);
  `setCmdDispatcher(RadioMacPort*)` keeps its name. LoraInterface.cpp no longer includes
  CmdDispatcher.h; it now includes AckCache.h, ClassAWindows.h, <cstring> itself.
  `announceModeChangeIfPending` is still public; the radio calls `onArmingPass()` instead.
- **b925f6c N27 step 1: BenchTools** (`main/BenchTools.cpp`, `main/include/BenchTools.h`, both in
  `test/node_dispatcher_sources.cmake` -- you include it, so nothing to do in your CMake).
  **Hub test code to adapt (compiles today via DEPRECATED shims that I will remove later):**
  - real_cmd_dispatcher_test.cpp:244 `using CmdDispatcher::modeTestExpiredCb_;` and :4663
    `NodeProbe::modeTestExpiredCb_(&disp);` -> `disp.expireModeTestForTest();`
  - frtos_tasks_test.cpp:98 `using CmdDispatcher::mode_test_active_;` and :497
    `disp.mode_test_active_ = true;` -> `disp.setModeTestActiveForTest(true);`
  All other ModeTest/DriftTest accessors you use (modeTestActive, modeTestLastRefusal,
  modeTestReportedMode, modeTest*ForTest, driftTestActive, noteModeTest*) are unchanged.
- Your current tree has its own compile errors in real_lora_client_test.cpp (ModeBProbe
  `mode_test_`, LORAListener) and 3 failing MacWireV1Listener REGISTER tests -- hub-side WIP, not
  caused by the node.

- **1b0ee50 N27 step 2: MacUplink** (`main/MacUplink.cpp`, `main/include/MacUplink.h`, both in
  `test/node_dispatcher_sources.cmake`). The uplink path, framing and the session lock moved out of
  CmdDispatcher. **Removed members your tests might reach:** `SessionTxLock`, `session_tx_mutex_`,
  `pending_session_opening_`, `beacon_tx_pending_`/`beacon_tx_pending_msgid_`, `msgid_relogin_pending_`,
  `requestMsgidRelogin_`, `sealUplinkFrame_`/`plainUplinkFrame_`/`stampUplink_`/`reportUplink_`,
  `noteSessionMutation_`, `resetSessionForRegister_`. Kept as facades: `macDataRequest_`,
  `packUplinkFrame_(msg, hdr, &buf, &len)` (your seal-point test), `msgidReloginPendingForTest`,
  `sessionUnlockedMutationsForTest`, `sessionTxNestedForTest`. Your copy with the mirror overlaid
  built and ran unchanged (only your own WIP failures).

- **82a7658 HB14 node side.** `ClientRegister.register_seq = 4` (uint32, additive; 0 = old node).
  Strictly increasing per node, persisted in NVS key "regseq" before use, continues across
  reboots. **Hub action:** remember the highest register_seq per MAC (RAM, current hub boot) and
  refuse a REGISTER whose value is not higher (treat 0 as legacy: keep today's rate limit only).
  Stubs regenerated: copy `blindsproto/blinds.pb-c.{c,h}` from scratchpad\hubmirror.
  REGISTER goldens on the node re-recorded (+`2001` + new tag). MacWireVectors unchanged.
  Your proto_sim goldens that contain node REGISTER uplinks will change the same way.
- **8b368a1 N18:** comment only (entropy note for H5).

## 0. LATEST (2026-10-26 follow-ups) -- node HEAD **083c0f8**, fw 1.1.54

Image: scratchpad\images\node_review_1.1.54_083c0f8.bin. Commits on top of f4fe9d8:
7820192 (N10 tightened + ACK_ERROR for refused commands), 083c0f8 (fw 1.1.54).

**Shared files to copy (from scratchpad\hubmirror, regenerated from 083c0f8):**
- `blindsproto/blinds.pb-c.h` -- COMMENT changes only (MacAck/AckStatus/GridSync comments from
  blinds.proto); `.c` identical; no struct/API change. Nothing else shared changed.

**Behaviour changes (no wire-format change):**
1. A node WITHOUT a session refuses a plaintext GridSync. Pre-session plaintext set is now
   ClientConfig, CoverConfig (+ LOGIN/K_AUTH, GridBeacon/FLEET, GridDemote). The production hub
   sends GridSync encrypted after confirm_session_, so it is unaffected.
2. ACK_REQ'd commands the node recognises but refuses -> MacAck `ACK_ERROR`: cover target outside
   0..1 / NaN, sysop op 0 (unset) or unknown op, ScheduleConfig without payload. Never executed;
   not entered in the cmd_id guard (a re-pack with the same cmd_id is refused again with
   ACK_ERROR); a same-FCNT duplicate is re-answered ACK_ERROR (<= ackcache::kMaxReAcks), never OK.

**proto_sim fixtures that must adapt: 36 tests**, all because they push a PLAINTEXT GridSync
(cmd_case 21) into a sessionless node (log: `Rejecting PLAINTEXT command (cmd_case=21) ... before
a session only ClientConfig/CoverConfig are accepted`). Fix in the helpers (build_grid_sync /
gridSyncOnMark / seam & irq equivalents): LOGIN first (or setBaseNonceForTest + sealed frame) and
seal the GridSync. Exact list: scratchpad\hub_tests_to_adapt_followup.txt (36 names, e.g.
RealNodeFixture.AnAgreeingGridIsAdoptedAndAnchored, ...TheFleetKeyIsAdoptedOnlyFromAnAuthenticatedGridSync,
Seam.ANodeToldAboutTheGridIsNotToldToStopListening, Irq.AnAdoptedNodeListensAtItsOwnMarkWhileStillInModeA).
Note RealNodeFixture.WithoutASessionTheNodeDoesNotAskForItsGrid also relies on a pre-session
plaintext grid. Plus the stub-header drift gate until you copy the header.
Any of your tests asserting "no ack" for an invalid cover position / unset or unknown sysop now
see an ACK_ERROR MacAck instead.

**docs\mac-wire-format.md** (still uncommitted in your repo): Deviations box, §2.5 (ACK_ERROR for
undispatchable AND refused commands, cmd_id/duplicate rules), §4.5 (GridSync never in the clear;
new "Before a session" paragraph stating the accepted node-side set and that the hub ignores
pre-session plaintext MAC frames: no receive counter, timing, MacAck, PhaseReport or beacon
handling).

---
(Sections below describe the state at f4fe9d8 and remain valid except where section 0 says otherwise.)

Node repo: C:\Development\PlatformIO\PlatformIO\BlindsESP -- local commits only, not pushed.
**FINAL node HEAD: f4fe9d8** (fw 1.1.53). Stages 1-4 complete; stage 5 stopped after step 0
(see section 6). Image: scratchpad\images\node_review_1.1.53_f4fe9d8.bin (not in https_hosted).

**proto_sim against node f4fe9d8 (your tree as of the end of my run): 6 failures**, all expected:
- 5 drift gates (stub_pair_hub_vs_node, schema_drift_hub_vs_esphome, wire_format_drift_hub_vs_node,
  macsap_drift_hub_vs_node, macwire_drift_hub_vs_node): copy the files from scratchpad\hubmirror
  (section 1);
- RealNodeFixture.DriftTestStartsInMeasuringProfileAndEndsInProduction: N15, needs
  `setBenchNode(true)` and a sealed DriftTest after LOGIN (section 2).
(The Seam/Irq failures listed in section 2 are already fixed on your side.)
I never write into the hub repo (except docs\mac-wire-format.md, stage 4, not yet touched).
regen_stubs.sh is run against a scratch mirror; **the ready-to-copy files are in
scratchpad\hubmirror\local_components\{blindsproto,lora_client}\** (regenerated from node 6d83b28,
your banners preserved). Diff of mirror vs your tree = exactly the files below.

## 1. Shared files / stubs you must take

| file | node commit | change |
|---|---|---|
| main/include/Sequencer.h | b5ec9f7 | ADDED `notePersisted(saved_tx, saved_rx)` -- you have already re-vendored this |
| blindsproto/blinds.pb-c.{c,h} | 6d83b28 | regenerated from the restructured blinds.proto. NO wire change and NO C API change: every message/field/number/name kept, field order inside each message kept (identical structs, INIT macros, descriptors); only the order of messages in the file and comments moved (sorted-line multisets of old and new are identical). Your schema drift gates go red until you copy them; so does the node's stubs_node_vs_hub gate. |
| lora_client/MacSap.h | 6d83b28 | comments; CopySealer parameter NAMES ct,ct_len -> frame,frame_len (type unchanged) |
| lora_client/MacWire.h | 6d83b28 | comments only |
| lora_client/FrameCrypto.h | 6d83b28 | comments only |

MacWireVectors.h: unchanged. Recommendation (your call): lora_client.cpp:4058
`self->seq_.notePersisted(written_tx, written_rx)`.

## 1b. docs\mac-wire-format.md (hub repo) -- edited by me, NOT committed

Refreshed to the code (review P9/P23): status line (implemented, live since 2026-10-19),
a "Deviations as built" box, and corrections in §1 (v0 in past tense), §2.4 (allocator
limit), §2.5 (ack iff ACK_REQ, ACK_ERROR, no piggyback, ACKED_FCNT only on MacAck),
§3 (SysopCmd, oneof uniqueness/reserved, PortTable as built), §4.1 (node vs hub receive
order), §4.2 (timing_trusted always true), §4.5 (pre-session plaintext set, FLEET only for
GridBeacon, no sealed broadcast), §5 (CopySealer params), §6 (vector sizes), §7 (cmd_id
scope = one hub boot), §10 (stampCopy bool, buildCtrInitialBlock). Please review and
commit it with your next docs commit (I do not commit in your repo).

## 2. Reconciliation of the failure counts

I re-ran your proto_sim (your tree as of now, read-only, build dir ~/hubps_nodecheck) against node
ae291f4: **1762 tests, 4 fail**. Your earlier "83 against 05696bd" no longer reproduces; most of
the N10 fallout is already adapted on your side. The remaining 4, each with its node cause:

| test | cause (node change) | what the fixture must do |
|---|---|---|
| RealNodeFixture.DriftTestStartsInMeasuringProfileAndEndsInProduction | N15 (ae291f4): DriftTest STARTS only on a bench node from an authenticated frame. Log: `DriftTest: REFUSED (not a bench node)` | `disp.setBenchNode(true)` and send the DriftTest sealed after a LOGIN |
| Seam.AGridAlignedDownlinkArrivesInsideTheNodesWindow | N10 (d3eb776): plaintext cover op (cmd_case 10) to a sessionless node is refused | LOGIN first, deliver the cover op sealed |
| Irq.AModeBNodeArmsItsOwnNextMarkEveryRound | your current `statusOnMark` now sends GridSync frames (allowed in the clear), but every GridSync re-adopts the grid and resets the phase (`phase sample ... n 1` each time), so the node never promotes | use sealed sysops/cover ops after a LOGIN for the 8 mark frames (a plaintext sysop is refused by N10) |
| Irq.PromotionEndsTheTrialAndModeBTakesTheTimer | same as above | same |

## 3. Behaviour changes visible to the hub (all node-side, no wire-format change)

- N10 (d3eb776) node WITHOUT a session accepts in the clear only ClientConfig, CoverConfig and
  GridSync (grid only; fleet key only from an authenticated frame), plus LOGIN/GridBeacon/GridDemote.
  Production hub unaffected (it defers everything else to confirm_session_).
- P3 (05696bd) node acks iff the frame's FLAGS.ACK_REQ is set and the command executed (your
  kRoutes match portmap::ackedWhenExecuted; mirror_tables_test keeps pinning it).
  NEW on the air: an authenticated ACK_REQ'd frame whose body has no case the node knows ->
  MacAck with status ACK_ERROR (1), existing enum value. **Hub action**: MacAck.status is ignored
  today (HubWire.cpp:199 stores it, nobody reads it) -> make dlv_ack_ fail fast on ACK_ERROR.
- P5 (05696bd) FLEET-tagged frame with a non-GridBeacon body is dropped; a SESSION frame must be
  unicast to the node (no sealed broadcast). Hub side of P5 (yours): it accepts a plaintext MacAck
  before session_confirmed_.
- N5 cover position outside 0..1 / NaN: not executed, not acked.
- P4/N12 sysop op 0 (CMD_ENABLE_WIFI == unset) or unknown op: not executed, not acked.
- N6 a LOGIN repeating the live session's nonce gets at most one resend per 4 s (your retry: 5 s).
- N2 ClientConfig sleep_duration: 0 keeps the node's value; else clamped to [60 s, 7 d].
- N3 OTA rollback enabled (needs a serially flashed bootloader to take effect).
- N15 DriftTest needs a bench node + authenticated frame.

## 4. Node API changes your proto_sim compiles against

- `packUplinkFrame_(msg, hdr, &buf, &len)` still exists (malloc test seam, incl. the fresh-session
  request on a spent FCNT); the primary overload packs into a caller buffer, returns `PackStatus`.
- `send_tx_buffer` returns `Mac0Access::TxEnqueue`; `Mac0Access::dataRequest` returns it, no logs.
- `allocTxId_`, `handleNotSet`, members `rxPacket/txPacket/cfgAddress/cfgSubnet/txbuf_mutex/
  rx_pool_mutex` removed. `CmdDispatcher.h` no longer includes psa/crypto.h, <iostream>, <sstream>,
  <queue> (include them yourself if a test relied on that).
- `noteDriftSample()` is a no-op (the RxDone stamp travels with the RX buffer); your tests pass the
  same rx_us to onReceiveNew, so they are unaffected.
- `CmdDispatcher::rx_buffer_t` gained `int16_t rssi`.
- New: `setSessionProvenHook`, `sessionUnlockedMutationsForTest`, `sessionTxNestedForTest`,
  `gridSnapshot()`, `Mac0Access::gridView()/gridSnapshot()`, `ensureOneShotTimer_`.
- DeriveSessionKeys no longer logs "Session keys derived" (handleLogin logs the generation).

## 5. Node commits so far

- b5ec9f7 N4 (Sequencer notePersisted(tx,rx); SessionManager durability flag)
- ecc1652 N1 N2 N5 N6 N7 N8 N12 P4 (dispatcher)
- 091a46a N3 (OTA rollback) + packUplinkFrame_ seam fix
- 96b809d P2 node side (.gitattributes LF, stubs_node_vs_hub gate, loud skips)
- 04abe8c N9 (grid/mark locking, threading doc)
- d3eb776 N10
- 05696bd P3 P5 N16
- ae291f4 stage 4a (N11 N13 N14 N15 N17 N23 N25 N32 N36 N37, comments, layer_lint)
- a7600f5 README (N35)
- 6d83b28 blinds.proto restructure + shared-header comments (P4 P8 P10 P16 P17 P19-P22 doc parts)
- de5a96e N33 N34 (factoryMac64, typos: setCmdDispatcher, taskMotorControl)
- 9e17054 handleLogin comment summary (N30)
- 23e7885 stage 5 step 0: test/node_dispatcher_sources.cmake
- f4fe9d8 fw 1.1.53

## 6. Stage 5 (CmdDispatcher split) -- what you can do to unblock it

Every extraction of CmdDispatcher.cpp into a new translation unit breaks proto_sim's three
hand-written node source lists (CMakeLists.txt ~647-675, ~754, ~848-865). Please `include()`
`${BLINDS_ESP_ROOT}/test/node_dispatcher_sources.cmake` and use NODE_DISPATCHER_SOURCES /
NODE_DISPATCHER_STAGED_HEADERS / NODE_DISPATCHER_PLAIN_SOURCES there. After that the split
(MacReceiver, MacUplink, SessionEntity, MacManagement, BenchTools, ... per review N27) can land
node-side without touching your build. N19 (making CmdDispatcher's members private behind a
test peer) also needs your tests to switch to a friend peer at the same time.

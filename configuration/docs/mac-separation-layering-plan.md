# MAC separation: staged layering plan (S0-S8)

Condensed from `docs/mac-review-2026-10-16.md` §6 (the review that drives this
work; target architecture in its §5 and in `docs/mac-layer.md` §10). Not to be
confused with `docs/mac-separation-implementation-plan.md`, which is the earlier
Tier 1-4 security plan (Encrypt-then-CMAC cutover etc.).

**Rules for every stage before S8:** no wire byte changes (the golden frames in
`tests/proto_sim/golden/` and the node's golden tests must stay identical); no
NVS layout or version change (never touch `kRestoreStateVersion`); do not change
the REGISTER/LOGIN bypass order; no virtual calls, logging or heap in the
retag/verify hot paths, nothing logged under `s_session_keys_mutex`; do not edit
the hub's vendored copies of shared headers (the node copy is the source); the
layering lint counts (`tools/layer_lint.py`, both repos) may only fall.

Status legend: `[x]` done (commit), `[~]` in progress / partial, `[ ]` open,
`[-]` deliberately deferred (with the stage it moved to).

---

## S0 Pin (no behaviour change)

- [x] Hub golden frames: byte-exact output of every hub producer (LOGIN, REGISTER
      config pushes, Control cover op/sysop plaintext + encrypted, Fire
      TimeSync/ClientConfig/CoverConfig, GridSync publish/withdraw, GridDemote,
      ModeTest / MAC ping / drift streams, 17-copy retag, sealBurstCopyTag over
      every timing field) and of the real tracker (signed GridBeacon, per-copy
      restamp). `golden_frames_test`, `golden_tracker_frames_test`; 9/9 mutants
      caught. Hub `ca8879b`.
- [x] Hub ratcheting layering lint `tools/layer_lint.py` + `tools/layer_lint_baseline.json`,
      ctest gate `layer_lint_hub`. Hub `d33d1dc`.
- [x] `docs/mac-layer.md` §1 table, MAC-2 line, §2 crypto bullet, §3 msgId roles
      (7+), new §10 target architecture; this plan; the review saved as
      `docs/mac-review-2026-10-16.md`.
- [x] Node side: node golden uplink frames (real CmdDispatcher, 8 cases / 19
      frames), node `tools/layer_lint.py`, `MacSap.h`. Node `ec35a59`.
- [x] `MacSap.h` (types only) + the `Mac1Freshness.h` it aliases, vendored
      byte-identical into the hub and drift-gated (`macsap_drift_hub_vs_node`,
      `mac1freshness_drift_hub_vs_node`). Hub `8dc0e8c`.

## S1 Findings (small, deliberate behaviour changes, each listed)

- [x] **F3 (hub)** a POSITION report no longer completes the in-flight cover op;
      only the CommandAck (and the node's AckCache re-ack) does. The position
      still reaches the cover entity. See the commit for the history of the
      rule (introduced with the first login/ack work, `a657b03`, before the
      node acked every cover op). Hub `964d686`.
- [x] **F7 (hub)** documented (no behaviour change): an unauthenticated
      encrypted-looking uplink from a registered node without a session makes the
      hub start a LOGIN, before any CMAC check; accepted, bounded by the
      onboarding gate. Hub `964d686`.
- [x] **F4 (hub)** K_enc/K_mac generation pairing per frame, not per
      destination: done in **S4** (hub `ee83ae4`), originally moved there, where the `CopySealer` closure travels with
      the frame in `TxPolicy` (a correct fix needs the per-frame generation in
      the queued frame; a half fix in S1 would add a second mechanism S4 deletes).
- [x] **F1 (node)** retarget `destAddress` only after an encrypted/verified frame
      or a verified LOGIN; fail-closed uplink when a session exists. NOTE: the
      hub-repo test `RealNodeFixture.AdmitFrame_APlaintextPingOnlyReachesTheNodeWhileATestIsArmed`
      (`tests/proto_sim/scenarios/real_cmd_dispatcher_test.cpp`) uses "a
      plaintext ping retargets destAddress" as its witness and must move to
      another witness (e.g. `ping_rx`) in the same change.
- [x] **F2 (node)** forged unicast LOGIN must not move mark-hit / missed-mark
      diagnostics (same root cause as F1).
- [x] **F5 (node)** `noteSessionProven()` only after `admitFrame` succeeds.
- [x] **F6 (node)** sequence authenticated MAC_CONFIG through the session window.
- [x] **F9 (node)** delete dead `LoraInterface::sendPacket` and the unused
      `blinds.pb-c.h` include.
- Node F1/F2/F5/F6/F9: node `5fa7e15`..`5ba6900` (fw 1.1.39). Hub mirror of the
  two affected RealNodeFixture tests: hub `3bd4e1e`.
- F8: no change (accepted: pre-auth burst fields are clamped; jamming does the same).

## S2 MAC-2 extract

- [ ] Node `LinkSecurity` (keys out of CmdDispatcher; LOGIN/REGISTER crypto).
- [x] Hub `mac2::SessionKeyStore` (one owner, replaces the listener members + 5
      static maps) + `mac2::LinkSecurity` (K_root/K_auth, MICs, KDF, seal, retag,
      uplink verify/decrypt) in `local_components/lora_client/HubLinkSecurity.{h,cpp}`;
      API on `mac::MacHdr` + bytes. Mutex rules intact; retag input moved to the
      stack. Characterization `f13ec7f`, extract `89e054e`. Goldens unchanged.
      H-1 not re-measured on hardware (same PSA work and lock span, one heap
      allocation less per copy).
- Gate: golden frames identical; EncUplink / MsgidGuard / seal tests ported;
  H-1 retag latency must not regress.

## S3 MAC-1 extract

- [x] Shared `mac1::Sequencer` (node `ec2a814`..`449b673`), vendored into the hub
      and drift-gated; the hub uses `Config{ForwardRatchet, 1, 1}` with a persist
      hook = save_state_ + notePersisted, so the window (rx < id <= rx+1024) and
      the NVS write cadence (one write per id handed out and per committed
      uplink) are unchanged; check/commit stay two-phase around MAC-2 verify.
      Counters set without a write go through `Sequencer::restore(tx-1, rx-1)`.
      Characterization `f6e8954`, adoption `9528bcb`. No NVS change.

## S4 One data-request path

- [ ] Node: one `macDataRequest` for the 3 uplink builders (closes V10).
- [x] Hub: one framing path `mac_frame_` (header, msgid, pack, seal) for all
      data build sites and one MAC-0 data request `mac0_send_` (the only
      `parent_->send`; LOGIN and GridDemote keep their bootstrap builds but leave
      through it). Lint: raw_parent_send 14->1, msgid_alloc_sites 9->3,
      pack_sites 8->1, app_picks_encryption 7->0. Hub `ee83ae4`.
- [x] `mac::CopySealer` + an opaque 64-bit binding (`mac2::FrameSeal`: peer +
      key epoch) in `TxPolicy` and the queued frame; the tracker calls it per
      copy (`sealCopy_`) and no longer looks up clients or calls the listener
      (lint: seal_upcalls 1->0, client_iteration 8->7, app_cmd_cases 4->2 -- the
      beacon asks for `fire_stamp=false`). **F4 closed** (red-first test).
      Per-copy decode/re-encode for the restamp stays until S5/S8. H-1: host
      sanity test added; bench re-measure still open.
- Gate: golden frames, burst retag tests, H-1 bench re-measure; no heap in the
  closure context.

## S5 MAC-0 boundary

- [ ] Node `Mac0Access` (LoraInterface + frtosTasks RX logic + timing state from
      CmdDispatcher); `RxTicket` + `commitTiming`.
- [x] Hub: beacon build, fleet key, beacon CMAC and the shadow-mask sources moved
      to a MAC management unit (`lora_tracker/mac_mgmt.{h,cpp}`); the tracker
      gets an opaque frame. All per-copy schema work behind `mac0::BurstFrame`
      (`burst_frame.h`); `lora_tracker.cpp` has no protobuf, `__CMD_` or PSA left.
      Lint: app_cmd_cases 2->0, psa_calls 9->0, client_iteration 7->3. Hub
      `84460b0`. Pre-existing: only SleepTrigger objects reach
      register_listener(), so the shadow mask has never seen the real nodes.
- [x] Hub MAC-0 timing: `rx_ticket_` (mac::RxTicket + uncertainty) captured at
      admission, committed by MAC-1 through `mac0_commit_timing_` (the hub's
      commitTiming). Hub `3b4afed`. Per-copy decode/re-encode stays until S8.
- Gate: phase/anchor/WMR tests; bench Mode A/B turnaround vs 7.0/8.2 and
  10.0/10.6 ms; ISR/IRAM rules kept (INT_WDT history).

## S6 Port demux

- [ ] Node: one table oneof -> port; `dispatchCommand` gets APP only; MAC
      management frames go to the management entity (node agent).
- [x] Hub: `PortTable.h` (`porttable::uplinkPort`) is the one uplink table;
      `dispatch_payload_` demultiplexes through it (MAC management frames to
      the MAC handlers). Hub `1abd0ce`.
- [x] Hub children receive decoded `AppIndication`s (MacHdr, payload,
      authentic) for App-port frames only, and an `on_register()` notice; the
      re-pack and the children's own unpack/sender filter are gone (lint:
      raw_bytes_to_children 3->0, children.wire_unpack 2->0,
      children.sender_filter 2->0; the cover's unregistered-sender hole
      closed). Sensor platform now host-tested. Hub `06285e3`, `1abd0ce`.
- Note (pre-existing, unchanged by request): the shadow pending mask's inputs
  come only from register_listener() objects, i.e. SleepTriggers; production
  registers the real nodes with register_client(), so the mask has never seen
  their state.

## S7 ARQ ownership

- [ ] Node: AckCache behind the MAC-0 delivery service; APP handlers stop knowing
      msgid (ack still emitted on execution) (node agent).
- [x] Hub: the delivery service owns msgid, frame, pack-once copy, re-pack
      fallback, attempt logs, ack matching, ladder and timers; the application
      supplies encode (payload via a frame() continuation) and place, and gets
      on_delivered(DeliveryConfirm{tag, log_id}) / on_fail / on_abandon.
      Characterization `c558d53`, service `51a5967`. Lint: new
      client.app_arq_by_msgid 31 -> 0. Also repaired three lint regexes whose
      `\b` had become a backspace (client.psa_key_handles was really 1 after S2;
      now 0).

## Checkpoint S0-S7 on hardware (2026-10-18, hub e0a1fdb, node 1.1.46)

- Correctness and Mode A turnaround clean (7.73/8.59 ms, 10.49/11.11 ms, MIC_FAIL 0).
  The 900 s Mode B+crypto run was not clean: FER_link 10291 ppm, free TX buffers at 1.
- Cause (pre-existing, NOT an S0-S7 regression): ModeTest mark lead ratchet in
  `mode_test_timer_cb_`. Node 1's GridSync publish pushed two node-2 marks two
  rounds out (`dt=4500`), and the "mark after the last" guard kept every later
  mark that far ahead, so 4 of the 5 pool buffers stayed held; marks and beacons
  were dropped (`No free buffers`), and each dropped mark was a seq gap. The 10-06
  and 10-09 baselines show the same ratchet once (a TimeSync, free 3). Pool code
  (send, drain, serviceTxQueue, return paths) and the mark code are textually
  identical to 7a7d7de apart from three plain TxPolicy fields; listener-side
  copies (DeliveryFrame, MacFrame, stored seals) never hold a pool slot.
  Differential: the MarkLead tests produce byte-identical mark traces on
  7a7d7de + node b6f2612 and on HEAD.
- Fix: a tick whose last mark is already a whole round beyond the grid's next
  mark is held (nothing sent, frame kept, no seq gap). Also bounds mode 4's
  1093 ms sweep timer, which ran a round ahead every ~3.7 ticks (12 marks queued
  after 40 ticks in the host test). Tests `MarkLead.*` (failed first), 5/5 mutants.
- Tools: campaign scripts filter REPORTs by node; runbook: Mode A turnaround needs
  the node's Timed Mode OFF.

## S8 Wire (explicit break)

- [x] Spec: `docs/mac-wire-format.md` (byte layout, crypto inputs, restamp, msgId split,
      proto/NVS impact, shared `MacWire.h`, W0-W5 order, cutover, decisions D1-D12).
      User decisions: wire break approved, no backward compatibility / no dual stack.

- [x] W1 hub (`ff1b32f`): stubs regenerated (additive v1 PDUs), prevbeaconfcnt,
      MacWire.h vendored + gated; all 22 vendored files re-verified, no drift.
- [x] W3 hub: v1 behind `MAC_WIRE_V1` (default OFF, v0 untouched, v0 goldens
      identical). `HubWire.{h,cpp}` (port table, HubMacPdu/HubAppPdu bodies,
      cmd_id = the Control slot's generation, uplink decode + v0 view incl.
      MacAck+ACKED_FCNT -> CommandAck), LinkSecurity v1 (sealFrameV1,
      authFrameV1/verifyAuthFrameV1, verifyUplinkV1, retagFrameV1,
      copySealerV1 over raw bytes, K_AUTH copies re-tagged per copy),
      `mac0::WireBurstFrame` (byte writes + one CMAC per copy, STAMP inserted
      once per burst; FLEET beacon verbatim), v1 beacon in mac_mgmt (FLEET tag),
      listener wiring (mac_frame_, LOGIN, GridDemote, set_response, REGISTER,
      encrypted pipeline, FrameCanon bypass). MacWireVectors.h (node 0ce60b6)
      vendored + gated: the hub encodes every downlink vector byte for byte and
      verifies/decodes every uplink vector. proto_sim OFF 1752 pass; ON 1359 pass
      (409 v0-envelope tests listed in tests/proto_sim/mac_wire_v0_only_tests.txt,
      for W4), ON goldens in golden_v1/; seam/e2e (real hub + real node W2, both
      ON) 27/27.
- [x] W4 hub: MAC_WIRE_V1 is the default (`MacWireConfig.h`; the production yml
      needs no flag, `-DMAC_WIRE_V1=0` builds the v0 path until W5). Goldens:
      `golden/` = v1, `golden_v0/` = v0 (OFF build). The 409 ON-excluded tests:
      the listener/cover tests run through `sim/v1_bridge.h` (v0-built uplinks
      become the v1 frames a node sends, hub downlinks are inspected through
      their v0 view, tampered frames stay tampered), the tracker tests through
      wire-aware builders, the node-harness targets build the node's v0 path in
      both configurations; 3 tests that pin v0 envelope bytes stay OFF-only
      (reasons in `mac_wire_v0_only_tests.txt`). Fixed on the way: a dangling
      capture in the `drive_session` test helper (ASan). Reservation of a placed
      v1 frame priced with its STAMP (`9286421`). proto_sim ON 1771 / OFF 1754.
- [x] Cutover LIVE 2026-10-19 (hub 914a09a, both nodes fw 1.1.50): Mode A
      turnaround 6.93/7.50 and 8.81/9.88 ms, Mode B 598/598, FER_link 0,
      MIC_FAIL 0; stack minima send task 1312/4096, esp_timer 1528/3584 free.

### W5 hub: delete v0 (plan, 2026-10-19)

Order: everything that does not need the proto removal first (W5a/b, small
commits, v1 goldens byte-identical, lint baselines lowered); the proto removal
(W5c) last, in step with the node.

W5a -- the switch and the OFF configuration:
- `MacWireConfig.h` and every `MAC_WIRE_V1` / `kMacWireV1` branch in
  lora_client.cpp, burst_frame.h, mac_mgmt.cpp, the shim tracker and the tests;
- proto_sim's `PROTO_SIM_MAC_WIRE_V1` option and OFF build, `golden_v0/`,
  `mac_wire_v0_only_tests.txt` + `proto_sim_discover_tests` and the 3 OFF-only
  tests (padded / unknown-field / wide-header uplinks);
- the node-harness targets (real_cmd_dispatcher_test, real_lora_interface_test)
  keep `-DMAC_WIRE_V1=0` only while the node still has its v0 path; when node
  W5b deletes it they are retired (the node repo covers its v1 path, seam/e2e
  cover hub + node).

W5b -- v0 production code:
- lora_client.cpp: `s_pack_*`, `s_seal_downlink`, `pack_operation_`, the v0
  arms of mac_frame_ / send_login (LOGIN MIC input) / GridDemote / set_response
  unpack / REGISTER MIC / uplink verify / inner unpack, `check_uplink_canonical_`
  + FrameCanon + `noncanonical_frames_`, `retagLikeLastFrameForTest`,
  `packOperationForTest`;
- HubLinkSecurity: `sealDownlink`, `retagCopy`, `copySealer`, the v0
  `verifyUplink`, `authMic` / `verifyAuthMic` (the v1 calls stay);
- MacHdrCodec.h `fromLoraHeader`; tracker `mac0::BurstFrame` (TxFrame becomes
  WireBurstFrame); mac_mgmt `beaconMac` and the v0 beacon pack;
- vendored FrameCanon.h + its drift gate (the hub no longer includes it);
- tests of the deleted internals are deleted with them (their v1 counterparts:
  mac_wire_v1_test, mac_wire_v1_tracker_test).

W5c -- the proto (lands in both repos at once, after the node's commit):
- delete LoraHeader, EncryptedPayload, LoraClientOperationMessage,
  LoraClientResponseMessage, CommandAck (numbers and names reserved);
  LoginMsg.mic / ClientRegister.mic / GridBeacon.mac reserved;
- hub: producers build HubAppPdu / HubMacPdu / LoginMsg directly (no v0
  envelope as the in-memory command); uplink dispatch takes the decoded PDU +
  WireHdr (HubWire's v0 view goes); AppIndication carries the PDU;
- tests build v1 frames natively: sim/v1_bridge, the v0 parts of
  sim/wire_codec + crypto (EtM v0 prefix, LOGIN/REGISTER MIC inputs) and the
  sim models that speak v0 are rewritten or retired; FrameCrypto.h legacy
  helpers go per the node plan (re-vendored).
- Gate: suite green, v1 goldens byte-identical, ESP32 build clean.

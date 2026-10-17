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

- [ ] Shared `Sequencer` with two-phase check/commit (node bitmap window; hub keeps
      its strict ratchet via a policy flag; switching the hub to the bitmap is a
      separate, deliberate change). Persistence bit-identical.

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
- [ ] Hub: tracker gets a framing callback instead of `__CMD_` branching; the
      beacon build moves to the management entity (lint: `tracker.app_cmd_cases`,
      `tracker.psa_calls`).
- Gate: phase/anchor/WMR tests; bench Mode A/B turnaround vs 7.0/8.2 and
  10.0/10.6 ms; ISR/IRAM rules kept (INT_WDT history).

## S6 Port demux

- [ ] One table oneof -> port; `dispatchCommand` gets APP only; MAC management
      frames go to the management entity.
- [ ] Hub children receive decoded indications, not raw bytes (lint:
      `client.raw_bytes_to_children`, `children.wire_unpack`,
      `children.sender_filter` fall to 0; closes the `lora_cover.cpp` sender
      filter hole while unregistered).

## S7 ARQ ownership

- [ ] Delivery ladder and AckCache behind the MAC-0 delivery service; APP handlers
      stop knowing msgid (ack still emitted on execution).

## S8 Wire (explicit break)

- [ ] Fixed header (version byte != 0x0A, hub dual-stack during the node-1 OTA),
      FCnt/cmdId split, MAC ACK bit, CMAC over raw header bytes, delete
      FrameCanon and the legacy GCM helpers. Node 2 by serial first, soak, then
      node 1 by OTA, then remove the dual stack. Re-record the golden frames.
- Gate: full H-2/H-3 campaign, MIC_FAIL 0, FER_link 0, 900 s Mode B+crypto soak.

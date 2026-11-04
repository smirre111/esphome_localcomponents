# Protocol / schema contract review — MAC wire v1 + blinds.proto (2026-10-25)

Read-only review. Nothing was edited, built, flashed or committed.

Scope: node `BlindsESP/proto/blinds.proto`, `proto/regen_stubs.sh`, generated stubs,
`main/include/{MacWire,MacWirePdu,MacWireVectors,MacSap,PortMap,FrameCrypto}.h`, the
receive/transmit glue that uses them (`main/CmdDispatcher.cpp`), the hub's
`local_components/{blindsproto, lora_client/{MacWire.h,HubWire.*,PortTable.h,MacHdrCodec.h,HubLinkSecurity.cpp,lora_client.cpp}}`,
`tests/proto_sim` (golden frames, drift gates, vector tests), the spec
`docs/mac-wire-format.md` and `CLAUDE.md`.

Excluded as already known: H5 (K_root dev literal), Mode B phase-error timing variance.

Paths: `N:` = `C:\Development\PlatformIO\PlatformIO\BlindsESP`, `H:` = `c:\Development\esphome\configuration`.

---

## Summary table

| id | sev | title | effort |
|---|---|---|---|
| P1 | **High** | An encrypted MAC-ping run's reserved FCNT block survives a re-LOGIN: CTR keystream reuse under the new session keys | S |
| P2 | **High** | The contract's drift gates fail open in 5 ways, and `regen_stubs.sh` is checked out CRLF (the script cannot run in WSL) | S |
| P3 | Medium | `FLAGS.ACK_REQ` is not read by the node; two separate "acked" tables have to agree; an ACK_REQ'd frame the node cannot dispatch ends in a session teardown | M |
| P4 | Medium | `SysopCmd.op` has no presence and its zero value is `CMD_ENABLE_WIFI` (several other enums have an action as their zero value too) | S |
| P5 | Medium | The per-frame AUTH policy (spec 4.5) exists only in prose. Each side enforces only part of it | M |
| P6 | Medium | No evolution/versioning story: one VER byte, one spare flag bit, no capability negotiation, an unknown oneof case is dropped silently | M |
| P7 | Medium | Layering leaks: MAC-0/MAC-2 data in App PDUs (TimeSync, NodeWakeBeacon, ClientConfig); the MAC ACK means "the application executed it" | L |
| P8 | Medium | Four structs for one header, plus four converters; v0 names live on in them (`msgid`, `sessionnonce`, u32 one-byte fields) | M |
| P9 | Medium | The spec doc disagrees with the code in 12 places, including its status line ("SPEC, not implemented") and the cmd_id scope | S |
| P10 | Low | cmd_id dedupe: one slot, RAM only, scope not on the wire, reset coupled to `LoginMsg.hub_rebooted` | S–M |
| P11 | Low | The shared vectors start from derived keys: the KDF and the extension order with SESSION_OPENING+ACK are not pinned across implementations | S |
| P12 | Low | Node receive order differs from spec 4.1 and from the hub (MAC-1 check after verify, decrypt and decode) | S |
| P13 | Low | The 2^31 rule has two constants and two vocabularies; the hub address is defined twice | S |
| P14 | Low | `PortTable.h` defaults unknown uplink cases to App; the spec promised a static_assert | S |
| P15 | Low | Telemetry presence (`optional`) still not implemented | S–M |
| P16 | Low | Several uint32 fields carry enums that exist only in C++ headers or prose | S |
| P17 | Low | `AckStatus` ERROR/BUSY are never sent: `MacAck.status` is always 0 (dead field) | S |
| P18 | Low | ACK piggyback is "allowed by the format" but the hub ignores ACKED_FCNT on non-MacAck bodies; misplaced downlink flags are accepted silently | S |
| P19 | Low | Unstated invariant: oneof case numbers must be unique across HubMacPdu and HubAppPdu; gaps are not `reserved` | S |
| P20 | Nit | Stale v0 references in proto comments, shared headers, gates and scripts (list) | S |
| P21 | Nit | Naming inconsistency in blinds.proto; one value has five names (nonce / hub_nonce / base_nonce / session_id / sessionId) | S |
| P22 | Nit | blinds.proto has no header and no entry-point ordering; section references don't name the doc | S |
| P23 | Nit | Spec section 10 signatures and the section 6 size table don't match the code and the vectors | S |

---

## High

### P1 — Encrypted MAC-ping FCNT reservation survives a re-LOGIN → keystream reuse (High, likely; verify with a sim test)

**Location.** `H:\local_components\lora_client\lora_client.cpp:1841-1874` (`start_mac_ping`
reserves a block of the session TX counter: `mac_ping_crypto_next_msgid_ = incrTxMessageId(); … set_counters_no_write_(txId + frames-1, …); mac_ping_crypto_last_msgid_ = txId`),
`:1698-1704` (`encrypt_this_one = mac_ping_crypto_ && session_confirmed_ && next <= last` → `msgid = mac_ping_crypto_next_msgid_++`),
`:4119-4121` (`send_login`: `set_counters_no_write_(0, 0); // a new session: both directions restart`),
`:967` / `:4089` (`clearSessionKeys_()`; neither touches the ping reservation),
`HubLinkSecurity.cpp:399-421` (`sealFrameV1` seals whatever FCNT is in the header under the peer's *current* live keys; it has no check against the Sequencer).

**Why.** The reservation is a range of FCNTs from session S1's counter. A re-LOGIN during the
run (node reboot → REGISTER → `clearSessionKeys_`, op-retry exhaustion, msgid relogin, the
nightly-sleep fallback, …) resets the hub TX FCNT to 0 and installs K(S2). The ping timer keeps
running (≤ 900 s). Once S2 confirms, `session_confirmed_` is true again, so the remaining reserved FCNTs
are sealed under **K(S2)**, while S2's ordinary traffic counts up from 1 through the same numbers.
Example: S1 was young (txId ≈ 5), the run reserves FCNT 6…~800, and the very first S2 frames (TimeSync, the D1
GridSync that carries the **fleet key**, cover ops) use FCNT 2, 3, 4, 6, 7, … Two different plaintexts then go out under the
same `(K_enc, session_id, node_addr, dir, FCNT)` CTR block. The ping body is known plaintext
(`MacControl{kind=PING, seq, pad=0xAB…}`), so an eavesdropper XORs out the keystream and
decrypts the colliding frame. The 'MAC Ping' buttons are in the production YAML (diagnostic
category), so production can reach this path. `tests/proto_sim` has no test that combines
`start_mac_ping(crypto)` with a relogin.

**Fix.** (a) Drop the reservation (`mac_ping_crypto_ = false; next = last = 0`) in `send_login()`'s
counter reset, in `clearSessionKeys_()` and in `installSessionKeys_()`. (b) Defence in depth: bind the reservation to
the key epoch (store `key_epoch` with the range and refuse to seal when it differs), or
have `mac1::Sequencer` own reservations (`reserve(n)` returning a range that is invalidated on reset),
and add `assert(fcnt <= seq_.txId())` before `sealFrameV1`. (c) Add a proto_sim test: start an encrypted ping run,
force `send_login()` and confirm the session, then assert that no FCNT is sealed twice under one key epoch.

### P2 — The contract gates fail open; the regen script is CRLF (High)

**Evidence.**
1. **protoc-c version mismatch passes the gate.** `H:\tests\proto_sim\cmake\check_stub_drift.cmake`:
   `if(_committed_ver AND NOT _committed_ver EQUAL _local_ver) message(STATUS "SKIPPED: …") return()`.
   CTest reports **Passed**, not Skipped.
2. **No protoc-c means no stub gate at all.** `CMakeLists.txt:125-126`: `find_program(PROTOC_C_EXECUTABLE protoc-c)`
   `if(PROTOC_C_EXECUTABLE) …`. The tests are simply never registered. protoc-c is not on the
   Windows PATH (`which protoc-c` → none), only in WSL.
3. **No node tree means every cross-repo gate is skipped.** `CMakeLists.txt:64-67`: "Every node-side target and every cross-repo
   drift gate will be SKIPPED, and the suite will still pass." This is a WARNING only.
4. **A missing vendored file is skipped silently.** `CMakeLists.txt:544`: `if(EXISTS ${_src} AND EXISTS ${_vend})`. If a vendored
   copy is renamed or deleted, its gate disappears. The same pattern guards FrameCrypto and Scheduler. `regen_stubs.sh:112`
   likewise skips silently when `dst` is missing (only FrameCrypto warns).
5. **Semantic mirrors have no gate.** `NodeFrames.h` ↔ `HubFrames.h` ("field for field the
   same names", not gated), `PortMap.h` (node) ↔ `PortTable.h` (hub), and `HubWire.cpp kRoutes[].ack_req` ↔
   `PortMap.h kDownlinkPorts[].acked` (see P3). Only seam/e2e tests would notice if they disagreed.
6. **The regen script is CRLF on disk.** `git ls-files --eol proto/regen_stubs.sh` → `i/lf w/crlf`; both repos have
   `core.autocrlf=true` and no `.gitattributes`. Bash in WSL fails on `set -euo pipefail\r`. This is the
   "CR bug" mentioned in the brief, and it is still present in this working tree. (The hub stubs are also CRLF in the
   worktree while the node's are LF. That is harmless only because both gates normalise line endings.)

**Fix.** (a) Make gates that cannot run report it: `return()` → exit with a distinct code and set
`SKIP_RETURN_CODE` on the test, so CTest shows "Skipped" (or FATAL_ERROR in CI). (b) Add a
**protoc-independent** gate that compares the two *committed* stub pairs with each other
(`compare_files --ignore-eol hub/blinds.pb-c.{c,h} node/components/blinds/...`). It needs no
compiler and catches a half-applied regen. (c) Replace the `EXISTS` guards on the vendored side with FATAL_ERROR. Keep a
single list of shared headers in one file, read by both `regen_stubs.sh` and the CMake foreach (today the two
lists are maintained by hand). (d) Add `.gitattributes` to both repos with `*.sh text eol=lf`, `*.py text eol=lf` and
`*.proto text eol=lf`. (e) Add a static_assert/table test that ties P3's two "acked" tables together.

---

## Medium

### P3 — ACK_REQ is write-only; two "acked" tables (Medium)

**Location.** Hub sets it: `HubWire.cpp:19-32` (`kRoutes[].ack_req`), `lora_client.cpp:3906`
(`withAuth(r.ack_req ? kFlagAckReq : 0, …)`). The node never reads it: `kFlagAckReq`/`ackReq()` appear only
in `MacWire.h`. The node acks from its own table: `PortMap.h:41-58` (`acked`) via
`CmdDispatcher.cpp:4373-4377` `ackExecuted_` → `portmap::ackedWhenExecuted(cmd)`.
Spec 2.5: "the hub sets ACK_REQ on every Acked delivery" (it never says the node obeys it).

**Why.** Two hand-kept tables define one contract. Today they agree (OPERATION, SYSOP,
SCHEDULE and GRIDSYNC are acked on both). If `DeliveryPolicy.h` makes another kind Acked (e.g.
TimeSync), the node never acks: retry ladder → "Command failed after retries" → session teardown.
The same thing happens in a mixed fleet with a **new** oneof case: the old node decodes `pdu_case = NOT_SET`
(`MacWirePdu.h:226-230`), dispatches nothing, and never acks.

**Fix.** The node acks iff `ACK_REQ` is set (the header is authoritative, as the spec intends), and drop
`PortMap::acked`. Add `ACK_UNSUPPORTED = 3` to `AckStatus` and have the node answer an ACK_REQ'd frame it cannot dispatch with it,
so the hub fails fast instead of tearing the session down. Until then, add a test asserting
`routeDownlink(c).ack_req == portmap::ackedWhenExecuted(c)` for every case.

### P4 — `SysopCmd.op` zero value = CMD_ENABLE_WIFI (Medium)

**Location.** `blinds.proto:26-28` (`CMD_ENABLE_WIFI = 0`), `:890-893` (`SysopCmd { ClientOperation op = 1; uint32 cmd_id = 2; }`),
node `CmdDispatcher.cpp:222-223` → `:1811-1813` (`startWifi()`).

**Why.** In v0 the sysop was a oneof member, which has presence. In `SysopCmd` it is a plain proto3 scalar,
so an omitted or zero `op` decodes as ENABLE_WIFI: a ~100 mA radio stays on until something turns it off. Any hub
bug that builds a `SysopCmd` without setting `op` silently becomes "turn WiFi on". The file's own stated
philosophy (GridSync.pendingMaskValid, MacControl negative polarity, `MAC_UNSPEC = 0`, `MODE_UNSPEC = 0`)
is that the zero value must be the safe one. Similar but lower-impact zero values: `AckStatus ACK_OK = 0` (an
unset status reads as success), `SchedAction SCHED_OPEN = 0`, `WakeReason WAKE_BOOT = 0`.

**Fix.** Use `optional ClientOperation op = 1;` (proto3 explicit presence, supported by protoc-c ≥ 1.4;
the committed stubs are 1.5.2) and have the node refuse `!has_op`. At the next flag day, add `CMD_UNSPEC = 0`
and renumber. Same treatment for `AckStatus`, and `UNSPEC` zero values for any new enum.

### P5 — AUTH policy per frame type is prose-only, enforced piecemeal (Medium)

**Evidence.**
- Node `CmdDispatcher.cpp:5188-5194`: a `FLEET` frame on PORT 0 returns true without verification. If its body is
  not a GridBeacon (GridSync, ModeTest, MacControl), `rx_auth_` is Fleet, `was_encrypted` is false, and it goes through the plaintext gate exactly like
  `AUTH=NONE`. It is not refused, although spec 4.5 makes FLEET valid only for GridBeacon.
- Node `CmdDispatcher.cpp:5211` accepts `AUTH=SESSION` with `DST == broadcast` and then decrypts with
  `node_addr = h.dst = 0xFF` (`:5242`). The hub never seals a broadcast, so the two sides would never agree on
  that CTR block. Spec 4.5 has no sealed broadcast.
- Hub `lora_client.cpp:2249-2255`: before `session_confirmed_`, a **plaintext** `MacAck`
  (FLAGS.ACK) is accepted, reaches `dispatch_mac_payload_` → `notePhaseReport_` + `dlv_ack_` (`:1989-1996`).
  Spec 4.5 lists MacAck as SESSION only.
- Broadcast rules ("FCNT = 0", "never STAMPED", "BURST 0/1") are not checked on receive.

**Fix.** Add one dependency-free shared header (`AuthPolicy.h`, node-owned, vendored and gated) with
`allowed(dir, port, case) → bitmask of AUTH` (plus a broadcast flag). Both receivers apply it right after the body
decode, and a table test generated from spec 4.5 covers it. That turns 4.5 from prose into code.

### P6 — No evolution/versioning story (Medium)

**Evidence.** The VER byte is all-or-nothing (`MacWire.h:163`: `if (buf[kOffVer] != kVersion) return false`).
FLAGS has exactly one spare bit (bit 7, "a frame with it set is dropped"), so a second header extension
needs a VER bump. Neither side has capability negotiation other than `NodeWakeBeacon.fwVersion` ("capability gate for the
hub", `blinds.proto:323`), which the hub does not use to choose a body shape. A new oneof case
decodes as NOT_SET and is dropped (which also leads to P3's teardown). New fields inside existing messages are fine
(protobuf-c keeps unknown fields). None of these rules is written down. Spec section 13 shows the cost:
"no dual stack" plus node 1 being OTA-only means every header change repeats the "flash the hub back to the old version,
OTA node 1, flash the new hub" sequence, with node 1 unreachable if the new image is bad.

**Fix.** Add an "Evolution rules" section to the spec and to the top of blinds.proto: (1) new body fields are free and
must have a safe zero value or `optional`; (2) a new oneof case needs a hub-side `fwVersion` gate and the
P3 `ACK_UNSUPPORTED` reply; (3) header changes require VER 0x02. Define now that the hub may hold a per-node
"wire version" learned from REGISTER (add `uint32 wire_caps` to `ClientRegister`, K_AUTH-covered) so a
future VER 0x02 can be rolled out node by node. Consider defining bit 7 as "TLV extension block follows" in
the next break, so later extensions don't need a VER bump.

### P7 — Layering leaks between MAC and APP (Medium)

**Evidence.**
- `TimeSync` (PORT 2, App) carries `sleepOk` (Class A window control) and `inSlotUplinks` (Mode B promotion
  evidence) (`blinds.proto:213,234`). The node strips them out before the application:
  `CmdDispatcher.cpp:4363-4371` "TimeSync's two MAC-0 facts, consumed by the MAC before the
  application sees the frame".
- `NodeWakeBeacon` (PORT 2, App) carries `PhaseReport` (MAC-0), `sessionResume` (MAC-2), and Mode C
  wake-clock and funnel fields 27-35 (MAC-0), next to app telemetry (voltage, position).
- `ClientConfig` (App) assigns the MAC short address and subnet (`addr`, `subnt`) and is the one frame
  that bypasses the address filter and the replay check (`FrameAdmission.h:185-191`).
- `LoginMsg.request_register` (Session) is a config-sync concern.
- The MAC ACK is sent only after the **application executed** the command (`CmdDispatcher.cpp:4357-4358`; spec
  2.5 "status still means executed"). ARQ success therefore depends on the app result. A non-executed command
  is indistinguishable from a lost frame, so it gets retries and eventually a teardown, and `status` carries nothing (P17).
- `GridSync` (MAC-0 mgmt) distributes a MAC-2 key (`netKey`). That is acceptable, but should be stated.

**Fix.** At the next schema break, move the MAC facts into PORT-0 PDUs (e.g. `HubMacPdu.classa{sleepOk,
inSlotUplinks}` piggybacked or sent as its own PDU, and `NodeMacPdu.wakeclock{…}`), keeping App PDUs to app data. Separate
"received" (MAC ACK) from "executed" (an app-level result, or MacAck.status that actually carries ERROR/BUSY). Until then,
document each exception in the proto.

### P8 — Four header structs, four converters (Medium)

**Evidence.** `macwire::WireHdr` (`MacWire.h:103`), `mac::MacHdr` (`MacSap.h:62`; no `auth`, `ack_req`,
`ack` or `acked_fcnt`, and the field is named `session_nonce`), node `MacFrameHdr` (`NodeFrames.h:36`: `uint32_t msgid; // the FCNT`,
u32 for one-byte fields, `sessionnonce`), hub `hubframes::Hdr` (`HubFrames.h:38`, the same). Converters:
node `wireHeader()` (`CmdDispatcher.cpp:5072`), hub `hubwire::toHdr` (`HubWire.cpp:142`),
`machdr::fromWireHdr` and `machdr::fromHdr` (`MacHdrCodec.h:16,36`, the latter still carrying a width check "for hand-built headers").
`UpAck.ack_msg_id` (`HubFrames.h:105`, `NodeFrames.h:103`) holds ACKED_FCNT. `RxTicket.timing_trusted` is documented
as "FrameCanon verdict" (`MacSap.h:105`) but always `true` (`Mac0Access.h:262`). `CopySealer`'s parameters
are named `ct, ct_len` (`MacSap.h:96-97`) but receive the raw copy bytes (`HubLinkSecurity.h:216`).

**Fix.** Make `mac::MacHdr` an alias of (or a thin view over) `macwire::WireHdr`, and delete
`MacFrameHdr`/`hubframes::Hdr` header copies, keeping the case/pointer parts of NodeFrames/HubFrames. Rename `msgid`→`fcnt`, `ack_msg_id`→`acked_fcnt`
and `sessionnonce`→`node_nonce` everywhere. Remove `timing_trusted` and its PhaseGate input, or give it the spec 4.2 meaning.

### P9 — Spec ↔ code/proto discrepancies (Medium)

In `H:\docs\mac-wire-format.md`:
1. Line 3: "Status: **SPEC, not implemented.**" The wire has been live since 2026-10-19 and W5 deleted v0.
2. §1 and §5 are written in the present tense about v0 ("Today addressing … lives in protobuf field 1").
3. §3 table: HubAppPdu lists "ClientOperation (sysop)". The proto has `SysopCmd`.
4. §3: "S6's `PortTable.h` becomes a `static_assert` that each oneof case appears under exactly one port". It
   is a switch with `default: App` (P14). The node keeps a second table (`PortMap.h`).
5. §4.1 receive order "MAC-1 check(FCNT) → verify tag → …". The node verifies, decrypts and decodes first, then runs
   `admitFrame` (check+commit) (`CmdDispatcher.cpp:5088-5146`); the hub checks first (P12).
6. §2.5 "the hub sets ACK_REQ …" says nothing about receivers. In code the node ignores it (P3).
7. §2.5/D12 "An ACK may piggyback on any uplink". The hub only honours ACKED_FCNT on a MacAck body
   (`HubWire.cpp:195-203`) (P18).
8. §4.2 "a frame's length is a T0 input iff AUTH != NONE and its tag verified". Code: `timing_trusted = true`
   always (P8).
9. §7 "once per `cmd_id` **within a session**". Code: one `last_cmd_id_` that survives sessions
   and is cleared only by an authenticated LOGIN with `hub_rebooted` that changed the generation
   (`CmdDispatcher.cpp:5153-5160, 5269-5274`). The code does what the purpose sentence requires ("a re-pack after a session change cannot
   run an OTA twice"), so the spec wording is wrong (P10).
10. §10 signatures: `stampCopy` returns `bool`, not `void`. "`buildCtrBlock(...)`" is `buildCtrInitialBlock`.
    `MacSap.h` "CopySealer … input becomes the raw copy bytes", but the signature still takes a `MacHdr` plus `ct`.
11. §6 sizes: "46-byte cover op" / v1 46 B. The shared vector `cover_op` is 35 B (9-byte body); the table's
    airtime row and the "17 × 8 192 µs saved" claim rest on the 46 B figure.
12. §2.4 "the allocator refuses 2^31 − 1" is ambiguous. Code allocates up to and including 0x7FFFFFFF and refuses the next
    (`FrameCrypto.h:153-156`).

Also in blinds.proto: `ScheduleConfig` (`:265-268`) "Sent SINGLE-SHOT (BURST_CNT = 0), never bursted … only
ever sent in reply to a NodeWakeBeacon". `DeliveryPolicy.h kSchedulePolicy` is `Shape::Auto` (single shot when
earned, otherwise placed or burst) and is also pushed on login. This is very likely stale; check before trusting it. `:178` "P0 note:
these messages are DEFINED but not yet SENT by either side": auto mode has shipped.

**Fix.** Update the status line and §1/§3/§4.1/§7/§10, and add a "Deviations as built" box. Effort S.

---

## Low

### P10 — cmd_id dedupe scope (Low)
`CmdDispatcher.h:846 uint32_t last_cmd_id_{0}` keeps one slot in RAM. A node reboot forgets it: if the node executed an OTA, rebooted
and lost the ack, a hub retry with the same cmd_id runs it again (the hub-side at-most-once logic is the only guard).
The scope "hub boot" is implied by `LoginMsg.hub_rebooted`, a field whose documented purpose is the
demote decision (`blinds.proto:98-107`); changing that field's semantics silently changes dedupe. **Fix:** document the
scope in the proto next to `cmd_id`. Optionally carry a hub boot nonce in the high bits of `cmd_id`, or persist
`last_cmd_id_` with the session blob.

### P11 — Vectors don't pin the KDF or the extension order (Low)
`MacWireVectors.h:19-22` starts from fixed K_auth/K_enc/K_mac/K_fleet. K_root→K_auth and
K_root+session→K_enc/K_mac are pinned only by per-side tests (the node computes the expected value with its own
`cmacRaw`: `test/unit_link_security_test.cpp:53-62`; the hub pins the KDF *input* layout:
`frame_crypto_test.cpp:57-85`). No vector combines `SESSION_OPENING` and `ACK` (extension order NODE_NONCE before
ACKED_FCNT) or `STAMPED` with `SESSION_OPENING`. **Fix:** extend `tools/mac_wire_vectors.py` with K_root (a fixed phrase) →
K_auth, K_enc, K_mac, and one uplink carrying both extensions.

### P12 — Node receive order (Low)
The node decrypts and decodes (protobuf-c over attacker bytes for NONE frames, decrypt for tagged ones) before the read-only
MAC-1 check. This is not a security hole (the tag is verified first), but it differs from spec 4.1 and from the hub, and costs
CPU on replays. **Fix:** check first, or update the spec.

### P13 — Duplicate constants and vocabularies (Low)
`macwire::kFcntLimit` (`MacWire.h:92`) and `framecrypto::kMsgidCtrLimit` / `msgidFitsCtr` / `nextMsgidFitsCtr`
(`FrameCrypto.h:142-156`) express one rule. `hubwire::kHubAddr` (`HubWire.h:30`) duplicates `kHubAddress`
(`lora_client.cpp:3909`). `SecRxStatus::MsgidSpent`, `incrTxMessageId`, `tx_message_id` all mean FCNT. **Fix:** keep one
constant in MacWire.h and rename the rest.

### P14 — PortTable default → App (Low)
`H:\local_components\lora_client\PortTable.h:32-33` `default: return mac::Port::App;`. A new MAC uplink case
added to HubFrames but forgotten here is handed to the loracover children. **Fix:** an exhaustive switch with no
default (`-Wswitch-enum`), or derive the port from the wire PORT the frame came on, which the v1 hub already has.

### P15 — Telemetry presence still missing (Low)
`CoverPosition.position/voltage/current`, `ClientBattery.voltage`, `NodeWakeBeacon.voltage/position` are plain
proto3 scalars (`blinds.proto:804-823, 317-318`), so a real 0.0 (closed, flat battery) cannot be told apart from "not set".
Confirmed not implemented (CLAUDE.md lists this as "not confirmed"). **Fix:** `optional float`.

### P16 — Enums in uint32 (Low)
`PhaseReport.demotionReason`, `.rtcSlowSrc`, `GridSyncRequest.reason` ("timedmode::SyncRequestReason"),
`ScheduleEntry.kind`, `ModeTestReport.mode` (while `ModeTest.Mode` exists), `.armRefusal`,
`NodeWakeBeacon.resetReason`. Their meaning is defined in C++ headers or comments, so a reader of the schema cannot decode them.
**Fix:** proto enums, or at least the value table in the field comment (as `resetReason` already does).

### P17 — `MacAck.status` is dead (Low)
The only producer sets `ACK_OK` (`CmdDispatcher.cpp:518`), and only on `Executed`. ERROR/BUSY are never sent, so the field
is always omitted on the wire. **Fix:** either use it (P3/P7) or document it as reserved.

### P18 — Silent acceptance of misplaced flags (Low)
The hub ignores `FLAGS.ACK` plus ACKED_FCNT unless the body is a MacAck, which contradicts "the format allows piggyback". The node accepts
downlinks with `FLAGS.ACK`/`SESSION_OPENING` and ignores them (`wireHeader`, `CmdDispatcher.cpp:5072-5086`).
**Fix:** either drop such frames (direction-specific shape check) or honour them, and say which in the spec.

### P19 — Oneof numbering invariant unstated; gaps unreserved (Low)
The node casts every PDU case to one `DownlinkCase` enum (`MacWirePdu.h:193,212`), so case numbers must be unique
across `HubMacPdu` (18,20-24) and `HubAppPdu` (10-13,16,17) (and likewise for uplinks). Nothing in the proto says so.
Gaps 14, 15 (`HubAppPdu`), 19 (`HubMacPdu`), 11, 14, 15 (`NodeAppPdu`) and 21 (`NodeMacPdu`) are not `reserved`. **Fix:** a
comment above the four PDUs stating the invariant, plus `reserved` lines naming what each gap used to be.

---

## Nit

### P20 — Stale v0 references (Nit)
- blinds.proto: `:178` "P0 note … not yet SENT"; `:265-268` ScheduleConfig single-shot (see P9); `:321`
  "node kept its AEAD session"; `:359` "matched by msgId"; `:407-411` "before the base-nonce exchange";
  `:425` "not derived from msgid"; `:491-503` GridBeacon "that is what `mac` carries … A deterministic MAC
  rather than the AEAD … Extending AES-GCM …" (field 6 is reserved now); `:543` `s_pack_operation_message`
  (deleted function); `:613-616` "per-node AEAD session"; `:677-679` "msgid is also the AEAD nonce input".
- `MacWire.h:10` "(hub 5787cf0)", `:33` "(FrameCanon goes away in W5)".
- `MacSap.h:32-33` "No wire change: until stage S8 MacHdr is decoded from today's protobuf LoraHeader",
  `:42-47` msgid / "port COMPUTED … never carried", `:60-61`, `:105` "FrameCanon verdict", `:135` MsgidSpent.
- `PortMap.h:11-12` "Until the wire stage (S8) the port is not carried in the frame", `:20` "ENCRYPTED
  envelope", `:23-24` "The hub's equivalent … would be a separate table" (it exists: PortTable.h), `:34` "CommandAck".
- `FrameCrypto.h:111-156` (msgId throughout, "encrypt once per msgId").
- `check_header_drift.cmake` failure text: "agree on the AEAD wire format … psa_aead_decrypt: -149".
- `regen_stubs.sh:56` "FrameCrypto.h is the AEAD wire format (AAD layout, IV derivation …)".
- `CmdDispatcher.cpp:4224-4226` "an unopened ENCRYPTED envelope", `:4815-4819` "the AEAD tag verified",
  `:4833` log "Rejected message ID … my MsgID"; ~72 `msgid` occurrences in CmdDispatcher.cpp and ~121 in lora_client.cpp.

### P21 — Naming (Nit)
Mixed `snake_case` (`mac_addr`, `cmd_id`, `hub_rebooted`, `needs_config`) and `camelCase` (`sleepDuration`,
`txRound`, `pendingMaskValid`). `NodeAppPdu.state` is a `ClientBattery`. `Client*` and `Node*` prefixes are both used for node messages.
`LoginMsg.nonce` is called `hub_nonce`, `base_nonce`, `session_id`/`sessionId` and `pending_login_nonce_` in code. **Fix:** rename at the next
break (names are not on the wire, so the generated code is the only cost): `LoginMsg.session_id`, `NodeAppPdu.battery`.

### P22 — blinds.proto readability (Nit)
Line 1 is blank: there is no header pointing to `docs/mac-wire-format.md`, `regen_stubs.sh`, or the rule that no `package` is declared.
The four entry-point PDUs are at the very bottom (`:847-933`), after ~850 lines of history-rich messages.
Section references don't name their document ("section 4.4", "§4.6", "section 12.3"; the docs are
`implementation-plan.md` / `test-plan.md`). The node README is PlatformIO boilerplate, so the regen workflow can only be found
from the hub docs and the vendored banners. **Fix:** a 15-line header (spec link, regen command, evolution
rules from P6), the PDUs moved to the top, and the doc named in every section reference.

### P23 — Spec section 10 / section 6 details (Nit). See P9 items 10-12.

---

## What is good

- **The v1 header design is sound and small.** Fixed offsets, big-endian, explicit AUTH bits. `decode()`
  does shape checks only, ordered so no read can overrun (`hlen + tlen > len` before any extension read,
  `len > 255` refused), and `encodeHeader`/`stampCopy` refuse values above 24 bits and layout changes between copies.
  Authenticating the raw bytes removes the whole canonical-length class (FrameCanon) structurally.
- **Domain separation is careful.** KDF labels 0x01/0x02/0x03 with "BLS1", a 16-byte single-block input,
  hub and node addresses in K_enc/K_mac. The direction bit is in both the CTR block and the tag prefix, and the session id / fleet
  key id is in the prefix, so a reflected uplink cannot verify as a downlink. Both sides restrict `K_AUTH` to PORT 1
  (the hub requires REGISTER to be exactly K_AUTH on PORT 1, `lora_client.cpp:4016-4018`; the node drops K_AUTH and FLEET on the wrong port,
  `CmdDispatcher.cpp:5180-5194`). The bit-31 check runs after the tag on both sides. Retags are bound to a key epoch
  (a copy for another node or after a relogin is never tagged). `Resend::Rebuild` always takes a fresh FCNT, and pack-once keeps
  retries byte-identical.
- **The vectors are a real cross-implementation check.** `tools/mac_wire_vectors.py` uses pyca/cryptography and a
  header and protobuf encoder written from the spec. It is gated by `mac_wire_vectors_current`, and both repos assert the same 11
  frames (all spec 4.5 types, including a stamped copy 3 of 17 at FIRE_ROUND 0x123456 and a 0xFFFFFE stamp).
- **Schema hygiene where it matters most:** `reserved` number *and* name for `mic`/`mac`/`keepPowerProfile`
  with the reason; negative-polarity booleans; `pendingMaskValid`; `MAC_UNSPEC`/`MODE_UNSPEC`; the
  `PhaseReport` single definition; the deleted v0 messages are listed with their old numbers.
- `MacWirePdu.h` static_asserts every PDU case number against the internal case enum, and the hub
  `HubWire` maps cases explicitly. Both sides refuse a MacAck body without FLAGS.ACK.

## Readability verdict

**Mixed: the wire layer reads well, the schema and the glue do not yet.** `MacWire.h` is excellent for a
newcomer: one screen, the layout table at the top, every constant named, and the spec mirrored in it. `MacWirePdu.h` and
`HubWire.*` are clear. `blinds.proto` is hard: no header, the entry points at the bottom, long design-history essays (some of
them about the deleted v0 construction, e.g. GridBeacon's AES-GCM discussion), naming that mixes four eras,
and section references to unnamed documents. The vocabulary after S8 is half migrated: the spec and MacWire say FCNT /
node_nonce / ACKED_FCNT, while FrameCrypto, MacSap, NodeFrames/HubFrames and both large .cpp files still say msgid /
sessionnonce / ack_msg_id, and four header structs coexist. The spec still calls itself
"not implemented". A newcomer would find the regen/vendoring workflow only through the vendored banners or the hub
docs, and on Windows it fails at the first step (P2.6). Fixing P9, P20 and P22 (all effort S) and the rename in P8 would
make the contract as readable as `MacWire.h` already is.

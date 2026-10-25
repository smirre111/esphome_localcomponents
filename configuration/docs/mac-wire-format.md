# MAC wire format v1 (stage S8) — specification

Status: **IMPLEMENTED — the only wire since 2026-10-19** (S8 W4 cutover, hub `914a09a`,
nodes fw 1.1.50; v0 deleted in W5). Stage S8 of `docs/mac-separation-layering-plan.md`,
derived from `docs/mac-review-2026-10-16.md` §5.5. The shared dependency-free header
**`MacWire.h`** (node copy is the source, hub vendors it byte-identical under a drift
gate) holds every constant, offset and layout helper below, and `MacWireVectors.h`
pins exact frames for both implementations. Written 2026-10-18 against hub `e0a1fdb` /
node `1db0151`; refreshed 2026-10-25 against node `6d83b28` (2026-10-25 review, P9).

> **Deviations as built** (the sections below are corrected; this box lists them):
> - §2.5: the node acks **iff the frame carries `ACK_REQ`** and the command executed
>   (the header is authoritative); an authenticated `ACK_REQ`'d frame with no body case
>   the node knows gets a MacAck with `status = ACK_ERROR`. ACKED_FCNT is honoured by
>   the hub only on a MacAck body; v1 never piggybacks.
> - §3: the sysop is `SysopCmd {op, cmd_id}`; the hub's uplink port table
>   (`PortTable.h`) is a switch, the node keeps `PortMap.h` (case → port, expected ACK_REQ).
> - §4.1: the hub runs the MAC-1 `check` before the tag; the node verifies, decrypts
>   and decodes first and runs check + commit in its admission pipeline (still after the
>   tag, so a forged frame never touches the window).
> - §4.2: `RxTicket.timing_trusted` is always true (the tag covers the length).
> - §4.5: before a session the node accepts in the clear only ClientConfig, CoverConfig
>   and GridSync (grid only, no fleet key), plus LOGIN (K_AUTH), GridBeacon, GridDemote;
>   FLEET is valid only for a GridBeacon body; there is no sealed broadcast.
> - §6: the shared vector `cover_op` is 35 B (9-byte body, unstamped); the 46 B row is a
>   stamped cover op with a larger body.
> - §7: the node's cmd_id guard is one RAM slot whose scope is a **hub boot** (cleared by
>   a LOGIN with `hub_rebooted` that opens a new session), not a session.
> - §10: `stampCopy` returns `bool`; the CTR helper is `buildCtrInitialBlock`.

User decisions this spec is built on: the wire break is approved; **no backward
compatibility and no dual stack** — hub and nodes cut over together (§13).

Notation: all multi-byte integers are **big-endian**. `u8/u16/u24/u32` are unsigned
fixed-width fields. Offsets are from the first byte of the LoRa payload (the radio's
CRC is outside it). "Frame" = the full LoRa payload; "header" = bytes before the body;
"tag" = the trailing 8 bytes when `AUTH != NONE`.

---

## 1. Why (one paragraph)

In v0 (deleted) addressing and per-copy timing lived in protobuf field 1 (`LoraHeader`) of
the application envelope, an encrypted frame carried a second (inner) envelope, and the
CMAC was computed over *decoded* header values. That forces a full decode, mutate,
re-encode and re-MAC per burst copy (H-1 ~1.9 ms p50), created the canonical-length
problem (`FrameCanon.h`, demote-not-drop), and gives `msgId` seven roles. v1 puts a
fixed-offset header on the wire, authenticates the raw bytes, and splits `msgId`.

## 2. Frame layout

```
offset  size  field
0       1     VER          = 0x01 (MAC wire v1). Never 0x0A (every v0 frame starts 0x0A).
1       1     FLAGS        see 2.1
2       1     DST          destination short address (0xFF broadcast, 0x00 unprovisioned)
3       1     SRC          sender short address (hub = 0x01)
4       1     SUBNET       destination subnet (0xFE = any subnet, today's subnetAddressing)
5       4     FCNT         MAC-1 frame counter, u32, bit 31 MUST be 0 (2.4)
9       1     PORT         0 = Mac0Mgmt, 1 = Session, 2 = App; values 3..255 reserved (drop)
10      1     BURST_IDX    copy index within the burst, 0-based
11      1     BURST_CNT    copies in the burst; 0 = not part of a burst (uplinks: 0)
12      ...   EXTENSIONS   in this fixed order, each present only if its flag is set:
                STAMP          6   (FLAGS.STAMPED)   u24 FIRE_ROUND, u24 FIRE_OFFSET_US
                NODE_NONCE     4   (FLAGS.SESSION_OPENING)  u32 node nonce (uplink only)
                ACKED_FCNT     4   (FLAGS.ACK)       u32 the FCNT being acknowledged
H       ...   BODY         protobuf (plaintext or AES-CTR ciphertext), may be empty
L-8     8     TAG          present iff FLAGS.AUTH != NONE (2.1, 4)
```

`H = 12 + 6*STAMPED + 4*SESSION_OPENING + 4*ACK` (12..26). The fixed part is 12 bytes.

### 2.1 FLAGS (byte 1)

| bit | name | meaning |
|---|---|---|
| 0 | STAMPED | STAMP extension present (the copy declares its fire instant on the hub grid) |
| 1 | ON_MARK | copy 0 was placed on the destination's grid mark (phase-sample licence) |
| 2 | ACK_REQ | the sender wants a MAC ACK for this FCNT (2.5) |
| 3 | ACK | ACKED_FCNT extension present: this frame acknowledges that FCNT |
| 4 | SESSION_OPENING | NODE_NONCE extension present (the session-opening uplink) |
| 5-6 | AUTH | 00 NONE, 01 SESSION (body encrypted under K_enc, tag under K_mac), 10 K_AUTH (body plaintext, tag = MIC under K_auth), 11 FLEET (body plaintext, tag under the fleet key) |
| 7 | reserved | MUST be 0; a frame with it set is dropped |

### 2.2 Field mapping from v0 `LoraHeader` and the per-copy timing fields

| v0 field | v1 |
|---|---|
| `destAddress` (u32 varint) | `DST` u8 (values > 255 were never sent; `MacHdrCodec.h` already refuses them) |
| `destSubnet` | `SUBNET` u8 |
| `senderAddress` | `SRC` u8 |
| `msgId` | split (§7): `FCNT` (header) + `cmd_id` (App body, Control commands only) |
| `burstIndex` / `burstCount` | `BURST_IDX` / `BURST_CNT` u8 each (max burst 17) |
| `onMark` | `FLAGS.ON_MARK` |
| `fireStamped` | `FLAGS.STAMPED` |
| `fireRound` (u32) | `STAMP.FIRE_ROUND` u24, compared modulo 2^24 (§2.3) |
| `fireOffsetUs` (u32, < 1 500 000) | `STAMP.FIRE_OFFSET_US` u24 (max 16 777 215 ≥ kRoundUs) |
| `sessionNonce` (fixed32) | `NODE_NONCE` extension + `FLAGS.SESSION_OPENING` |
| `ctLen` (CMAC prefix only) | gone: the CMAC covers the raw bytes, the length is inherent (§4.2) |
| reserved 5 | — |

### 2.3 FIRE_ROUND width

u24 wraps after 2^24 rounds × 1.5 s = 291 days of hub grid uptime. Receivers only ever
compare a stamped round with a round they computed within the last beacon interval, so
both sides compute `(a - b) mod 2^24` as a signed 24-bit difference. `MacWire.h` provides
`roundDelta24(a, b)`. GridBeacon/GridSync keep their full u32 `txRound` in the body; a
node anchors on those, and the stamp only refines.

### 2.4 FCNT

MAC-1 counter of the link, one per direction (hub→node per node, node→hub). Same
allocation, persistence and replay rules as today's msgid (`mac1::Sequencer`: hub
ForwardRatchet, node SlidingWindow). It is also the CTR nonce input (§4.1), so a frame
re-sent byte for byte (pack-once) reuses its FCNT and its ciphertext. Bit 31 MUST be 0
(the CTR block holds 31 bits): the allocator hands out up to and including 0x7FFFFFFF,
refuses the next one and requests a fresh session (the `msgid_relogin_pending_` path); a receiver drops a frame with bit 31
set **after** the tag verified (a forged frame must not be able to cost a session),
and asks for a fresh session if it was authentic.

Broadcasts (GridBeacon, GridDemote) carry FCNT = 0 and are exempt from the replay
window, as today; GridBeacon's replay protection stays its `txRound` monotonicity.

### 2.5 MAC ACK

ARQ lives in the MAC header (v0's App `CommandAck.ack_msg_id` is gone):
- the hub sets `ACK_REQ` on every Acked delivery (Control, Config, Mac channels:
  OPERATION, SYSOP, SCHEDULE, GRIDSYNC — `HubWire.cpp kRoutes`);
- the receiver acks **iff `ACK_REQ` is set**: the node answers an executed command with
  an uplink carrying `FLAGS.ACK` + `ACKED_FCNT` = the request's FCNT and the body
  `MacAck { AckStatus status; PhaseReport phase; }` on PORT 0, `status = ACK_OK`. An
  authenticated `ACK_REQ`'d frame whose body has no case the node knows (a newer peer's
  new command) is answered with `status = ACK_ERROR`, so the sender can fail fast
  instead of retrying into a session teardown. A known command the node did not execute
  is not acked. The node's `PortMap.h` keeps the ACK_REQ it expects per case; the hub's
  `mirror_tables_test` pins it against `kRoutes`;
- a duplicate (same FCNT, already executed) is re-acked from the node's AckCache, keyed
  on FCNT;
- the hub's delivery service matches `ACKED_FCNT` against its slots (S7's `dlv_ack_`).
The format would allow an ACK to piggyback on any uplink, but v1 sends it as its own
frame, and the hub honours `ACKED_FCNT` only on a MacAck body.

## 3. Body

The body is **one protobuf message without any header**; there is no inner envelope
(v0's `EncryptedPayload{tag, ciphertext}` wrapper and the stripped inner `header` are
gone). Which message it is follows from (direction, PORT):

| direction | PORT 0 Mac0Mgmt | PORT 1 Session | PORT 2 App |
|---|---|---|---|
| hub → node | `HubMacPdu` oneof: GridSync, GridBeacon, GridDemote, ModeTest, MacControl, DriftTest | `LoginMsg` | `HubAppPdu` oneof: LoraCoverOperation, SysopCmd, ClientConfig, CoverConfig, TimeSync, ScheduleConfig |
| node → hub | `NodeMacPdu` oneof: MacAck, MacControl (echo), ModeTestReport, GridSyncRequest | `ClientRegister` | `NodeAppPdu` oneof: ClientAvailable, ClientBattery, CoverPosition, NodeWakeBeacon |

The receiver decodes the body with the message the (direction, PORT) pair names and
drops it if it does not decode. Oneof case numbers are unique across the two PDUs of a
direction (the node folds them into one internal enum); `blinds.proto` reserves each
PDU's numbers in its sibling so protoc rejects a collision. As built, the hub maps uplink
cases to ports with a switch (`PortTable.h`) and the node keeps `PortMap.h`; neither is
the static_assert this section once promised. Control commands in `HubAppPdu` carry
`uint32 cmd_id` (§7). A body case unknown to the receiver decodes as "no case" (see §2.5
for the ACK_ERROR answer).

## 4. Authentication and confidentiality

Keys are unchanged: K_root = SHA-256(fleet phrase)[0:16]; K_auth = CMAC(K_root,
`buildKAuthKdfInput`); K_enc / K_mac = CMAC(K_root, `buildSessionKeyKdfInput(1|2,
session_id, node_nonce, hub, node)`); the fleet key is minted with the grid and published
in GridSync. Tags are AES-CMAC truncated to **8 bytes** everywhere.

### 4.1 AUTH = SESSION (Encrypt-then-CMAC)

- CTR initial block (16 B), unchanged in construction, FCNT replacing msgid:
  `session_id u32 || node_addr u32 || (dir<<31 | FCNT) u32 || block_idx u32`, dir = 1
  for hub→node, `node_addr` = the node's short address, block_idx starts at 0.
  Byte-identical on retransmit because FCNT and session do not change.
- Ciphertext = AES-CTR(K_enc, block, body). Length = body length.
- Tag = CMAC(K_mac, MACIN)[0:8] with
  `MACIN = "BW1" || dir u8 || session_id u32 || frame[0 .. L-8)` — an 8-byte prefix that
  is never transmitted (direction and session bind the tag; the hub and node addresses
  are inside the raw header), followed by **every transmitted byte except the tag**:
  header, extensions and ciphertext.
- Receive order: check VER/FLAGS/PORT shape → MAC-1 `check(FCNT)` (read-only) → verify
  tag → bit-31 check → CTR-decrypt → decode body → MAC-1 `commit` → MAC-0 timing commit
  → deliver by port. The hub follows this order. The node checks its address first, then
  tag → bit 31 → decrypt → decode, and runs MAC-1 check + commit inside its admission
  pipeline (`admitFrame`); the window still moves only for a frame whose tag verified.

### 4.2 Canonicality — FrameCanon is deleted

The tag covers the exact transmitted byte string, so the on-air length the T0 formula
uses (`RegRxNbBytes`) is authenticated for every AUTH != NONE frame, and the header has
no varint encodings. `FrameCanon.h`, `noncanonical_frames_` and the demote-not-drop
rule are gone. As built, `RxTicket.timing_trusted` is always true: the length of every
frame that reaches the timing code is exact (fixed-size header, raw-byte tag), and the
MAC-1/MAC-2 gates decide which plaintext frames (bootstrap, GridDemote, MAC-2-off pings)
may feed timing at all.

### 4.3 AUTH = K_AUTH (LOGIN, REGISTER)

Body in the clear; tag = CMAC(K_auth, `"BW1" || dir || 0x00000000 || frame[0 .. L-8)`).
This replaces `buildLoginMicInput` / `buildRegisterMicInput` and the `mic` fields; the
MIC now covers the whole raw frame (header incl. FCNT, and every body field). Today's
layouts list the covered fields one by one; covering the raw bytes means a field added
later is authenticated without a layout change. LOGIN keeps its nonce, request_register and
hub_rebooted in the body. REGISTER keeps its rate limit and its place before the replay
filter.

### 4.4 AUTH = FLEET (GridBeacon)

Body in the clear; tag = CMAC(fleet key, `"BW1" || 1 || net_key_id u32 || frame[0 .. L-8)`).
`netKeyId` stays in the GridBeacon body (so a node can pick the key) and in the prefix.
Replaces `buildBeaconMacInput` and `GridBeacon.mac`; the burst index (today the
"review finding 2" addition) is covered automatically as a raw header byte.

### 4.5 Frame types and their AUTH

| frame | dir | PORT | AUTH | notes |
|---|---|---|---|---|
| LOGIN | ↓ | 1 | K_AUTH | plaintext body; the one session-establishing downlink |
| REGISTER | ↑ | 1 | K_AUTH | bypasses the replay window (bootstrap), time-rate-limited |
| session-opening uplink | ↑ | any | SESSION | `SESSION_OPENING` + NODE_NONCE; hub derives the candidate keys from it |
| GridBeacon | ↓ bcast | 0 | FLEET | FCNT 0, BURST 0/1, never STAMPED (fits kBeaconPayloadBytes); FLEET is valid for this body only — the node drops any other body under FLEET |
| GridDemote | ↓ bcast | 0 | NONE | unauthenticated by design (its only effect is to listen more) |
| GridSync | ↓ | 0 | SESSION | NONE accepted only by a node without a session, and then without the fleet key |
| ModeTest, DriftTest | ↓ | 0 | SESSION | arming requires an authenticated frame (and a bench node) |
| MacControl ping | ↓ | 0 | SESSION, or NONE while MAC-2 is switched off for control during an authenticated ModeTest (bench) | MAC-2-off pings use the dedicated counter |
| MacControl echo, ModeTestReport, GridSyncRequest, MacAck | ↑ | 0 | SESSION | |
| ClientConfig, CoverConfig | ↓ | 2 | SESSION; NONE to a node without a session (provisioning) or with address 0 | |
| other App frames | ↕ | 2 | SESSION | a node without a session refuses them in the clear |

There is no sealed broadcast: a SESSION frame must be unicast (session keys and the CTR
block are per node).

## 5. Per-copy burst restamp

MAC-0 writes, per copy, at fixed offsets and with no decode:
- byte 1 FLAGS: bits STAMPED / ON_MARK (decided once per burst; all copies of a burst
  have the same layout, so the frame length never changes between copies);
- byte 10 BURST_IDX, byte 11 BURST_CNT;
- bytes 12..17 STAMP (when STAMPED): FIRE_ROUND u24, FIRE_OFFSET_US u24;
then one CMAC over `prefix || frame[0 .. L-8)` and 8 bytes written at `L-8`.
The ciphertext is never touched (sealed once at pack time). `MacSap.h`'s `CopySealer`
keeps its shape `(ctx, const MacHdr&, const uint8_t *frame, size_t frame_len, tag)`; the
bytes it tags are the raw copy up to the tag, not a ciphertext.
Cost: ≤ 6 byte writes + one CMAC over (8 + L − 8) bytes — for a 46-byte cover op that is
one AES-CMAC over 46 bytes (3 blocks), replacing today's protobuf unpack + pack per copy
(the dominant part of H-1's ~1.9 ms p50). `BurstFrame` (hub) and the node's restamp shrink
to `MacWire::stampCopy(buf, len, idx, cnt, flags, round, offset)`.

## 6. Sizes and airtime

SX127x payload limit: 255 B (FIFO 256); hub transmit buffer 256 B. Fixed overhead per
frame: v0 encrypted stamped downlink ≈ body + 40 B (outer header ~24 incl. stamp,
envelope tags/lengths 6, tag field 10); v1 = body + 26 B (12 + STAMP 6 + tag 8).
Unstamped encrypted downlink: v0 body + 28, v1 body + 20. Encrypted uplink: v0 body +
25, v1 body + 20 (+4 with ACK).

Airtime, computed with `loratiming::symbolCount` (SF7, BW500, CR 4/8, CRC on, explicit
header, T_sym 256 µs; preamble 12 symbols down / 8 up, +4.25):

| frame | v0 B | v1 B | v0 µs | v1 µs |
|---|---|---|---|---|
| encrypted uplink (e.g. ack/position) | 25 | 20 | 21 568 | 19 520 |
| GridBeacon (fleet-key signed) | 45 | ~40 | 34 880 | 30 784 |
| cover op / sysop, encrypted, stamped | 60 | 46 | 43 072 | 34 880 |
| ScheduleConfig, 8 entries, encrypted, stamped | 152 | 138 | 96 320 | 88 128 |

Gains come in 8-symbol (2 048 µs) steps, so a frame that does not cross a block boundary
gains nothing; the largest frame stays far below 255 B. A 17-copy burst of the cover op
saves 17 × 8 192 µs ≈ 139 ms of shared air. (The rows are the 2026-10-18 estimates; the
shared vector `cover_op` in `MacWireVectors.h` is an unstamped cover op with a 9-byte
body, 35 B on air — use the vectors for exact sizes.)

## 7. msgId split

| role (review §3.2) | v1 home |
|---|---|
| CTR counter-block input | FCNT |
| CMAC prefix field | raw header bytes (FCNT among them) |
| replay-window key | FCNT (`mac1::Sequencer`) |
| ARQ correlation | `FLAGS.ACK` + `ACKED_FCNT`; AckCache keyed on FCNT |
| command identity (pack-once, at-most-once OTA) | `cmd_id` u32 in `HubAppPdu` for LoraCoverOperation / ClientOperation, allocated per HA command by the hub's Control channel (today's `ctl().generation`, which is already per logical command and never reset by a LOGIN) |
| burst-copy identity (CurrentBurstCopy) | FCNT (unchanged) |
| wake-clock pairing | `NodeWakeBeacon.prevBeaconFcnt` (rename of prevBeaconMsgid; same numbers) |
| session exhaustion (2^31) | FCNT bit 31 rule (§2.4) |

The node executes a Control command once per FCNT (replay window + AckCache) and
additionally once per `cmd_id`: it keeps the last executed cmd_id (one slot, RAM) across
sessions, so a hub re-pack after a session change cannot run an OTA twice; a retry keeps
both FCNT and cmd_id. The slot is cleared when a LOGIN with `hub_rebooted` opens a new
session (the hub's ids restart with its boot), so the scope of `cmd_id` is **one hub
boot**. A node reboot also forgets it (the hub's at-most-once delivery is then the only
guard).

## 8. blinds.proto changes (both repos; every stub copy regenerated by `proto/regen_stubs.sh`)

- delete `LoraHeader`, `EncryptedPayload`, `LoraClientOperationMessage`,
  `LoraClientResponseMessage` (field numbers reserved, names reserved);
- add `HubMacPdu`, `HubAppPdu`, `NodeMacPdu`, `NodeAppPdu` (oneofs reusing today's case
  messages and field numbers where possible), `MacAck {AckStatus status=1; PhaseReport phase=2;}`;
- remove `LoginMsg.mic`, `ClientRegister.mic`, `GridBeacon.mac` (reserve 3/3/6);
  delete `CommandAck` (replaced by MacAck + the ACK flag);
- add `uint32 cmd_id` to `LoraCoverOperation` and as a field next to `ClientOperation`
  in `HubAppPdu` (sysop is a bare enum today, so it becomes `SysopCmd {ClientOperation op;
  uint32 cmd_id;}`);
- rename `NodeWakeBeacon.prevBeaconMsgid` -> `prevBeaconFcnt` (same number, same type);
- `ENC_AES_GCM_128` and the `FrameCrypto.h` GCM helpers are deleted with the rest.

## 9. Persistence (NVS)

**No NVS layout change and no version bump.**
- Hub `LORAClientRestoreState {version, rx_message_id, tx_message_id, logged_in,
  last_sleep_epoch, config_hash}`: the two counters keep their meaning (they are the
  FCNTs); field names may be renamed in code only, layout and `kRestoreStateVersion`
  untouched.
- Node `PersistState` (`kRestoreStateVersion`) and the `lsess` session blob (v2: tx/rx
  counters, base/node/prior hub nonces): unchanged semantics, unchanged layout.
  **Never bump `kRestoreStateVersion`** (the documented no-restored-state reboot
  deadlock); nothing here needs it.
- Keys, nonces and counters carry across the cutover: a node resuming its session after
  the update keeps its FCNTs monotonic, so no keystream is reused; the hub's session
  keys are RAM-only and are re-established by the usual LOGIN after the hub flash.
- AckCache is RAM-only (keyed on FCNT now).

## 10. Shared header: `MacWire.h` (node `main/include/`, vendored to the hub)

Dependency-free (`<stdint.h>`, `<stddef.h>`, `<string.h>`), no crypto calls:
- constants: `kVersion`, `kFixedHeaderBytes = 12`, `kStampBytes = 6`, `kNonceBytes = 4`,
  `kAckedFcntBytes = 4`, `kTagBytes = 8`, `kMaxFrameBytes = 255`, flag bits, AUTH values,
  ports, offsets;
- `struct WireHdr` (all §2 fields decoded) and `size_t headerLen(const WireHdr&)`;
- `bool decode(const uint8_t *buf, size_t len, WireHdr *out, size_t *body_off,
  size_t *body_len)` — shape checks only (VER, reserved bits, PORT range, lengths, tag
  presence); never trusts content;
- `size_t encodeHeader(const WireHdr&, uint8_t *out, size_t cap)`;
- `size_t buildMacInputPrefix(uint8_t out[8], bool downlink, uint32_t session_or_key_id)`;
- `bool stampCopy(...)` (refuses values above 24 bits and a layout change between copies;
  see `MacWire.h` for the exact parameters);
- `int32_t roundDelta24(uint32_t a, uint32_t b)`;
- `buildCtrInitialBlock(...)` stays in FrameCrypto.h (unchanged construction).
`MacSap.h` keeps `MacHdr` (decoded from `WireHdr` by one function) and `CopySealer`.

## 11. Test strategy

- `MacWire.h` unit tests on both sides: encode/decode round trip of every flag/extension
  combination; every malformed shape refused (VER 0x0A, reserved bits, short frames,
  extension overrun, bad PORT, AUTH without room for a tag); `stampCopy` touches only the
  documented offsets (byte-diff test); `roundDelta24` across the wrap.
- Shared test vectors (a small `MacWireVectors.h`, node-owned, vendored + drift-gated):
  fixed keys/session/FCNT → exact frame bytes and exact tag for one frame of every type
  in §4.5. Both repos assert the same bytes; this replaces cross-implementation trust in
  the sim.
- Golden frames: S8 is the only stage that re-records `tests/proto_sim/golden/`
  (`PROTO_SIM_GOLDEN_RECORD=1`), in the same commit that flips the hub to v1, with the
  diff reviewed against §6's expected size changes.
- Seam/e2e (`seam_test`, `e2e_test`, real hub + real node in one process) must be green
  on v1 before any flash; the node's golden uplink tests re-recorded likewise.
- Mutation: every new decode/verify gate mutation-checked (refuse-path tests red-first).
- Hardware: §13's acceptance on node 2 before node 1's OTA; H-2/H-3 campaign (MIC_FAIL 0,
  FER_link 0, 900 s Mode B+crypto soak), H-1 re-measured (expected drop).

## 12. Implementation order (each step green in both repos' host suites)

1. **W0 node:** `MacWire.h` + unit tests + `MacWireVectors.h`; nothing uses it yet.
   **Hub:** vendor both + drift gates + run the vectors against hub-side helpers.
2. **W1 both:** additive `blinds.proto` changes (new PDUs, MacAck, cmd_id, SysopCmd) —
   the old messages stay for now; regenerate every stub copy in one pass.
3. **W2 node:** v1 encode/decode/seal/verify/restamp behind a compile-time switch
   `MAC_WIRE_V1` (default OFF); node host tests run both settings.
4. **W3 hub:** the same behind `MAC_WIRE_V1` (default OFF): `mac_frame_`/`mac0_send_`/
   `BurstFrame`/uplink pipeline/MacAck; proto_sim runs both settings; seam/e2e green with
   both sides ON.
5. **W4 both (one coordinated pair of commits):** default ON, golden frames re-recorded,
   node fw version bumped. Build artefacts for §13.
6. **W5 both:** delete the OFF path, `FrameCanon.h`, `LoraHeader`/`EncryptedPayload`/old
   envelopes, `CommandAck`, MIC/MAC body fields, GCM helpers; remove the switch.
A compile-time switch is not a runtime dual stack: no shipped image speaks both formats.

## 13. Cutover (no dual stack)

Constraint: node 1 is reachable only over the air, and its OTA trigger is a LoRa
command — so the **old** hub must trigger node 1's OTA, and node 1 then cannot talk to
the old hub. Sequence:
1. Bench validation (recommended, decision D7): serial-flash node 2 with the v1 image,
   flash the hub with v1, run the acceptance (LOGIN, cover op, sleep/wake, OTA trigger
   over v1, Mode B promotion, 900 s soak). Node 1 is unreachable meanwhile (it keeps
   sleeping/retrying; harmless).
2. Flash the hub back to the last v0 build; trigger node 1's OTA (it fetches the v1 image
   over HTTPS); node 1 goes silent to the v0 hub.
3. Flash the hub with v1. Node 1 and node 2 LOGIN; verify both.
Without step 1 the order is node 1 OTA → node 2 serial → hub, as decided; a v1 node-image
bug would then leave node 1 unreachable until a fixed hub could OTA it (which needs a
working v1 OTA command on that node). Rollback of a node = serial (node 2) / impossible
over the air for node 1 once on v1 with a v0 hub.

## 14. Decision list (picked option and recommendation)

| # | decision | options | recommendation |
|---|---|---|---|
| D1 | burst field width | one byte idx:cnt nibbles (max 15) / two bytes | **two bytes**: bursts are 17 copies |
| D2 | FIRE_ROUND width | u24 + modulo compare / u32 | **u24** (saves 2 B on every stamped copy; only short-range comparisons) |
| D3 | stamp presence | always reserved / per frame flag | **flag, decided once per burst** (all copies same length; beacons never stamped) |
| D4 | where AUTH lives | per-port implicit / explicit 2-bit field | **explicit AUTH bits**: receivers choose the key before decoding anything |
| D5 | body schema | one oneof per direction + port check / one message per (dir, port) | **per (dir, port)**: the port table becomes the schema |
| D6 | ARQ correlation | keep CommandAck.ack_msg_id / MAC ACK + ACKED_FCNT | **MAC ACK**; MacAck keeps status + phase report |
| D7 | cutover validation | decided order only / bench pass on node 2 + v1 hub first | **bench pass first**, then hub back to v0 to OTA node 1, then hub v1 |
| D8 | cmd_id | add now / defer | **add now** (sysop becomes SysopCmd); OTA at-most-once across re-packs needs it |
| D9 | MIC/CMAC prefix | 8-byte "BW1"+dir+id / none (header only) | **prefix**: binds direction and session/key id without spending air |
| D10 | VER value | 0x01 / anything ≠ 0x0A | **0x01**; a v0 frame (0x0A) is dropped by shape check |
| D11 | NVS | rename/re-layout counters / keep | **keep layout and versions**; rename in code only |
| D12 | ACK piggyback | allowed now / later | **format allows it, v1 sends ACKs as own frames** (no timing change) |

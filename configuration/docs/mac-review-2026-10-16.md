> **Status / provenance.** Read-only layering review of hub `7a7d7de` and node
> `b6f2612`, written 2026-10-16. It is the review that drives the MAC separation
> refactor, stages S0-S8; the condensed plan and the live status of each stage
> are in `docs/mac-separation-layering-plan.md`. Line numbers (`CD:`, `LC:`,
> `LT:` ...) refer to those commits and drift as the stages land. Kept verbatim
> below this note; findings fixed later are marked in the plan, not here.

# MAC-0 / MAC-1 / MAC-2 layering review (read-only), 2026-10-16

Scope: hub `c:\Development\esphome\configuration\local_components\` (HEAD 7a7d7de) and node
`C:\Development\PlatformIO\PlatformIO\BlindsESP\main\`. Every claim cites file:line; "INFERRED" marks
reasoning that was not exercised by running code. Nothing was built, flashed or edited.

Path shorthands: `CD` = node `main/CmdDispatcher.cpp`, `LI` = node `main/LoraInterface.cpp`,
`FT` = node `main/frtosTasks.cpp`, `LC` = hub `lora_client/lora_client.cpp`, `LT` = hub
`lora_tracker/lora_tracker.cpp`, `FC` = `FrameCrypto.h`, `FA` = node `include/FrameAdmission.h`.

---------------------------------------------------------------------------------------------------

## 0. Verdict in five lines

1. The **decisions** are well separated: they sit in pure headers (FrameCrypto, Mac1Freshness,
   FrameAdmission, MacSublayers, DeliveryPolicy, TimedGrid, ...). All 13 shared headers are byte-identical
   between hub and node, apart from FrameCrypto's vendoring banner (checked with diff).
2. The **control flow and state** are not separated. There is no service interface between any two
   adjacent layers on either side. `CmdDispatcher` (node) and `LORAListener` (hub) each own state
   from all five layers. The hub's PHY-side burst engine parses the application schema and calls up
   into MAC-2 for every copy it sends.
3. The wire format is the root cause. There is no fixed MAC header, addressing lives in the
   application protobuf, and `msgId` now has at least **seven** roles across four layers (the docs say three).
4. The RX order is **mostly secure** for encrypted traffic: CMAC is checked before decryption, and the
   replay commit happens after authentication. But **MAC-0 and MAC-2 state is committed before checks
   that run later, inside application handlers**. That produced one real defect (F1, below) and several
   minor ones.
5. You can reach a clean target in **seven behaviour-preserving stages plus one explicit wire stage**. The
   key design point: MAC-1 and MAC-2 cannot be stacked naively in numbered order. MAC-1 needs a
   two-phase check/commit primitive that MAC-2 drives (§5.2).

---------------------------------------------------------------------------------------------------

## 1. Layer map (current code)

### 1.1 File level: mac-layer.md §1's table is outdated

| file | PHY | MAC-0 | MAC-1 | MAC-2 | APP | where docs/mac-layer.md:54-64 is wrong |
|---|---|---|---|---|---|---|
| node `components/lora` | ● | | | | | — |
| node `LoraInterface.cpp` | ● (69 `lora_*` calls) | ● windows, CAD, TX queue | | | (includes `blinds.pb-c.h`, LI:19, unused) | dead legacy fixed-header sender `sendPacket` LI:201-250 (no callers) |
| node `frtosTasks.cpp` | ● DIO tasks, `lora_parsePacket/sleep/clearInterrupts` FT:268-604 | ● window-end, mark-missed, Class A hooks FT:327-347,414,554-584 | | | | **missing from the table**: half of MAC-0 RX lives here |
| node `CmdDispatcher.cpp` | ● `lora_packetRssi()` CD:5408 | ● address filter, burst deferral, grid/phase/Class A/drift (87 `grid_./phase_./classA/timedRx` refs), uplink addressing `destAddress` | ● replay check/commit, freshness | ● **K_auth/KDF/CTR/CMAC** (31 PSA/CMAC/CTR calls), LOGIN/REGISTER MIC, session lifecycle | ● cover, schedule, battery, OTA, sysops (85 `motCtrl->/sysCtrl->`) | says "AEAD calls"; the crypto is now EtM plus a KDF; timing is mostly here, not in LoraInterface |
| node `SessionManager.*` | | | ● tx/rx counters, bitmap window, persistence | ● base/node/prior hub nonces | | keys are **not** here (they are in CmdDispatcher `k_enc_key_id_`, CD:263-267) |
| hub `lora.cpp` | ● | | | | | — |
| hub `lora_tracker.cpp` | ● | ● queue, placement, burst, beacon | | ● **beacon CMAC with fleet key** LT:653-699; **per-copy retag up-call** LT:1323-1349 | ● **unpacks/re-packs the APP protobuf for every copy** LT:1237-1355; builds GridBeacon as an APP oneof LT:758-839 | the table claims PHY + MAC-0 only |
| hub `lora_client.cpp` | | ● placement, Class A, single-shot belief, Mode B supervisor, ARQ (`deliver_`) | ● `frame_counter_`, `commit_rx_msgid_` | ● 52 PSA calls, KDF, key maps, EtM | ● cover ops, schedule, sleep model, telemetry, auto mode | says "AEAD"; also spans MAC-0 timing |
| hub `loracover/*` | | ● **own sender-address filter** lora_cover.cpp:162, lora_sensor.cpp:129 | | | ● | re-parses raw wire bytes (lora_cover.cpp:136) |

### 1.2 Worst functions (layers touched)

| function | layers | evidence |
|---|---|---|
| node `CmdDispatcher::onReceiveNew` | PHY-len, MAC-0, MAC-2, APP-schema (4) | CD:6256-6353 (unpack 6274, burst 6296, decrypt 6331, dispatch 6352) |
| node `CmdDispatcher::serviceTxCommand` | APP, MAC-1, MAC-2, MAC-0 (4) | payload CD:431-644; `allocTxId_` 655; sessionNonce 670-674; REGISTER MIC 688-706; seal via `pack_response_message` 714; enqueue 716 |
| node `CmdDispatcher::handleLogin` | MAC-2, MAC-1, MAC-0, APP (4) | MIC 3376-3394; `resetCounters` 3524; `destAddress` 3530; `demoteGrid_` 3630; `sendWakeBeacon`/`sendAvailable` 3636-3642 |
| node `CmdDispatcher::admitFrame` | MAC-0, MAC-1, MAC-2 policy, PHY (RSSI log) (4) | CD:6010-6060 |
| node `CmdDispatcher::handleMacControl` | MAC-0, MAC-1 alloc, MAC-2 seal, TX enqueue (3) | CD:3855-3894 |
| hub `LORATracker::sendPacketBurst` | PHY, MAC-0, APP-schema, MAC-2 up-call (4) | LT:1237, 1301, 1323-1349, 1420 |
| hub `LORAListener::tx_tracked_op_` / `dlv_fire_` | APP, MAC-1, MAC-2, MAC-0 (4) | LC:3507-3579, 3884-3920 |
| hub `LORAListener::uplink_accept_` | MAC-1 commit, MAC-0 timing, MAC-2 install, APP forward (4) | LC:2242-2275 (commit 2246, install 2254, confirm 2264, forward 2274) |
| hub `LORAListener::set_response` | APP-schema, MAC-0, MAC-1, MAC-2, APP (5) | LC:2834-2896 |
| hub `LORAListener::send_login` | MAC-2, MAC-1 reset, MAC-0 raw send (3) | LC:4609 `clearSessionKeys_`, 4640-4641 counter reset, 4742 CMAC, 4760 `parent_->send` |

At class level, `CmdDispatcher` (6781 + 1533 lines) and `LORAListener` (6741 + 1819 lines, about 230 methods,
LC function list) each own state from all five layers.

---------------------------------------------------------------------------------------------------

## 2. Interface audit

### 2.1 Is there a service interface?

| boundary | node | hub |
|---|---|---|
| APP ↔ MAC-2 | **No.** APP handlers and APP payload builders sit in the same class as the crypto. APP picks plaintext or ciphertext implicitly through `get_base_nonce(destAddress)` (CD:977-981). | **No.** Every producer passes `encrypt = this->session_confirmed_` to `pack_operation_` (LC:3545, 3910, 3135, 3225, 5153, 5411, 5581). |
| MAC-2 ↔ MAC-1 | Partial. `SessionManager::wouldAcceptRxId/acceptRxId` is a good two-phase primitive (SessionManager.h:154-162), but MAC-2 state (nonces) is in the same class. | **No.** The counters are raw `frame_counter_` fields (LC:1464, 4640). The check is in `admit_frame_` and the commit is in `commit_rx_msgid_`. |
| MAC-1 ↔ MAC-0 | **No.** MAC-0 timing commits read the MAC-1 freshness class directly (CD:6038-6044). | **No.** MAC-0 timing (`noteAuthenticatedUplink_`) is called from inside the MAC-1 commit (LC:1558). |
| MAC-0 ↔ PHY | Partial. `LoraInterface` wraps the radio, but FT drives the PHY directly (FT:359-445, 529-604). CD reads RSSI (CD:5408). SystemCtrl clears IRQs (SystemCtrl.cpp:524). | Partial. `LORATracker::send(buf,len,TxPolicy)` is effectively the MAC-0 data request, but it takes **APP-schema bytes** and parses them back (LT:1237). |

### 2.2 Violations (layer skipping, foreign data, shared state)

**MAC-0 parsing upper-layer data / calling up**
- V1. Hub burst engine (MAC-0/PHY) unpacks `LoraClientOperationMessage`, branches on
  `CMD_GRIDBEACON`/`CMD_ENCRYPTED`, looks up the destination client by address and calls
  `LORAListener::sealBurstCopyTag` (MAC-2) per copy: LT:1237, 1301, 1323-1341. Tracker refers to
  `__CMD_` cases 4 times and makes 10 PSA calls.
- V2. Hub tracker builds the MAC-0 beacon as an APP oneof case and signs it itself: LT:758-839, 653-699.
- V3. Hub tracker reads per-node app/MAC state for the shadow mask by iterating listeners: LT:742-755.
- V4. Node RX path parses the full APP schema before any MAC decision (CD:6274). FA:43-53 reduces oneof
  cases to `CmdKind` because MAC admission depends on APP message types.

**APP reaching MAC/PHY**
- V5. Hub children (APP) receive **raw wire bytes** and re-run their own address filter. The hub
  re-packs the decrypted inner message with the outer header just for this: LC:2286-2299, 2894-2895;
  lora_cover.cpp:132-181. The filter `senderaddress != short_address_ && registered_` (lora_cover.cpp:162)
  passes every sender while the listener is unregistered.
- V6. APP handlers emit the ARQ acknowledgement keyed on the MAC-1 counter: `sendCommandAck(outer_header->msgid)`
  at CD:2192, 2301, 3331, 4839. MAC-1's duplicate path re-acks through the APP TX queue (CD:5907).
- V7. Hub APP telemetry completes MAC-0 delivery: a POSITION report acks the in-flight cover op
  (LC:2639-2641). See F3.
- V8. Node `CmdDispatcher` reads PHY RSSI (CD:5408). APP-side `MotorCtrl::publishState` → `sendPosition`
  is fine (MotorCtrl.cpp:1332-1336).

**MAC-2 reaching up/down out of turn**
- V9. `handleLogin` (MAC-2) sends APP messages (beacon, available) and demotes MAC-0's grid directly
  (CD:3627-3642). Hub `confirm_session_` (MAC-2) sends deferred APP ops and config (LC:1572-1602, 1604 ff).
- V10. Session-opening state (`pending_session_opening_` → `sessionnonce`) is applied in only one of three
  uplink builders (CD:670-674). `sendModeTestReport_` (CD:3090-3182) and the MAC echo (CD:3855-3894)
  duplicate allocation, seal and enqueue without it. INFERRED low-probability race: an echo sealed under
  the new keys before the opening beacon would fail the hub's CMAC.

**State with more than one owner**
- V11. Hub session keys: listener members `k_enc_key_id_/k_mac_key_id_/session_generation_` AND
  static maps `s_k_enc_map/s_k_mac_map/s_session_generation_map/s_seal_generation_map/s_base_nonce_map`
  (LC:62-109, 1812-1833). Seal reads the map (LC:311); retag reads the member (LC:1978).
- V12. Node keys in CmdDispatcher (CD:263-267). Nonces/counters in SessionManager. `session_proven_`,
  `frame_authenticated_` and `sublayers_` in CmdDispatcher.
- V13. Node RX buffer pool is owned by CmdDispatcher and filled by LoraInterface (LI:534, 575). The TX
  pool is owned by LoraInterface and filled by CmdDispatcher with `memcpy` (CD:762-784).
- V14. MAC-1 is implemented twice and the two behave differently. Node: sliding window + 64-bit bitmap
  (SessionManager.h:123-178). Hub: strict forward ratchet within 1024 (LC:1462-1465). Not a bug today
  (uplinks are never retransmitted byte-identically), but the code is not shared.

**Duplicated "data request" paths**
- V15. Node: 3 uplink build sites (CD:650-726, 3090-3182, 3855-3894). Hub: about 10 sites that hand-build
  header + `incrTxMessageId` + `pack_operation_` (LC:3120, 3215, 3514, 3904, 4663, 5116, 5382, 5535, the
  ping at 2419/2552) and 13 direct `parent_->send` calls. The unified delivery work (`deliver_`) unified the
  **retry policy**, not the **framing path**.

---------------------------------------------------------------------------------------------------

## 3. Wire format and RX order

### 3.1 What the protobuf-header wire format costs

- Addressing is `LoraHeader` = field 1 of the APP envelope (blinds.proto:413-429, 866-867, 978-979).
  MAC-0 cannot filter without the APP decoder (V4). Every hub listener re-parses every frame
  (LT:304-307 → LC:2836).
- **Per-copy stamping needs a full decode, mutate, re-encode and re-MAC for every copy** (LT:1237-1355).
  H-1's measured ~1.9 ms p50 per copy is the price of having no fixed header. The CMAC is computed over
  *decoded* fields (FC:362-403), which created the whole canonicality problem (FrameCanon, demote-not-drop).
- There are two envelopes per encrypted frame (outer header + inner protobuf, CD:5228, LC:2270). The inner
  `header` is stripped by convention (CD:811-814, LC:283-284), not by schema.

### 3.2 `msgId` roles today (the docs count 3)

| # | role | layer | evidence |
|---|---|---|---|
| 1 | CTR counter-block input (31 bits) | MAC-2 | FC:323-332; CD:5209; LC:399 |
| 2 | CMAC prefix field (32 bits) | MAC-2 | FC:394 |
| 3 | replay-window key | MAC-1 | SessionManager.h:154; LC:1463 |
| 4 | ARQ correlation (CommandAck.ack_msg_id, AckCache re-ack, `dlv_ack_`) | MAC-0 | LC:4183-4200; CD:5891, 6568-6583 |
| 5 | command identity (pack-once frame, at-most-once OTA) | APP | LC:3558-3559, 3604-3606 |
| 6 | burst-copy identity (CurrentBurstCopy), phase "same frame" grouping | MAC-0 | Mac1Freshness.h:124-126; CD:5834 |
| 7 | wake-clock pairing of a beacon's TxDone | MAC-0 timing | CD:677-681, 541 |
| (8) | session exhaustion trigger (2^31) | MAC-2 | FC:346-360 |

### 3.3 Node RX order: actual sequence, with a layer and commit verdict for each step

```
 1 PHY      DIO task reads FIFO                                     FT:359
 2 APP-schema  protobuf unpack of untrusted bytes                    CD:6274
 3 MAC-0    canonical-length classify (timing trust only)            CD:6287
 4 MAC-0    COMMIT burst_end_us_ + drift samples  [PRE-AUTH, clamped]  CD:6296, 6189-6229
 5 MAC-0    foreign-encrypted address drop                         CD:6298
 6 MAC-2    base nonce → CMAC verify → msgid-space → CTR → unpack  CD:5256-5301
 7 MAC-2    COMMIT session_proven_, cancel resume/beacon timers [BEFORE replay check]  CD:6342, 1233-1254
 8 MAC-0    address filter (counts)                                 CD:5360
 9 MAC-2    plaintext-command gate (policy)                         CD:5536
10 MAC-1    freshness classify (read-only)                          CD:5586
11 MAC-0    COMMIT mark hit / missed_marks_ (not freshness-gated)   CD:5619-5634
12 MAC-0    COMMIT phase sample / anchor (freshness-gated)          CD:5776-5852
13 MAC-0    maybeRequestGridSync (may queue an uplink, rate-limited) CD:6048
14 MAC-1    COMMIT replay window / ping seq / re-ack                CD:5971-6008
15 MAC-0    COMMIT destAddress/destSubnet (uplink target)            CD:6056-6057
16 APP+MAC  dispatchCommand: LOGIN MIC, GridBeacon CMAC, GridDemote rules are checked HERE,
            i.e. AFTER steps 11-15                                   CD:5086-5147
```

For encrypted frames the order is correct: authenticate (6), then check freshness (10), then commit replay (14).
The design flaw is that **authentication of the three plaintext-exempt frame types (LOGIN, GridBeacon,
GridDemote) runs inside APP-level handlers (16), after MAC-0 has already committed state from them (11, 15).**
Hub order: unpack → canon → REGISTER bypass → sender filter + read-only window → (plaintext LOGIN drop) →
nonce check (may start a LOGIN, LC:2091-2106, pre-auth) → candidate keys → verify → decrypt → MAC-1 commit +
MAC-0 timing (LC:2246, 1545-1559) → key install + session confirm → APP. Correctly ordered except the
pre-auth LOGIN trigger (F7).

---------------------------------------------------------------------------------------------------

## 4. Correctness and security findings (new, ranked)

**F1. MEDIUM. Unauthenticated frames redirect the node's uplinks, and the node then transmits in
plaintext.** `admitFrame` sets `destAddress = outer_header->senderaddress` at CD:6056. That runs after every
gate in admitFrame, but **before** `handleLogin` verifies the LOGIN MIC (CD:3376-3394) and before
`handleGridBeacon` checks its CMAC. LOGIN, GridBeacon and GridDemote are exempt from both the plaintext gate
(FA:66-70) and the sequence check (FA:137-140), so a single forged frame of any of these types, with any
sender address, reaches CD:6056. The next uplink is addressed to that sender. `pack_response_message` finds
no base nonce for it and **falls back to plaintext** (CD:977-981). The hub refuses plaintext from a confirmed
session (LC:2940-2954), so the next ack is lost and the hub retries. Leaked uplink contents: position, battery,
phase report. The link heals at the next admitted hub frame. The comment at CD:6053-6055 claims exactly the
property that is missing. Fix (no wire change): retarget only when `was_encrypted`, or after a verified LOGIN
(handleLogin already sets it at 3530); never from broadcast or unverified frames. Separately, make
`pack_response_message` fail closed when `hasSession_()` and the destination is not the persist peer.

**F2. LOW-MEDIUM. Forged unicast LOGIN moves MAC-0 diagnostics.** A plaintext LOGIN addressed to the node
books a mark hit and resets `missed_marks_` (CD:6041-6042, 5619-5626) before its MIC is checked. These are
WMR diagnostics only today (TimedModePolicy.h:101, 229). Same root cause as F1.

**F3. MEDIUM (correctness). Any authenticated POSITION report completes the in-flight cover command.**
LC:2639-2641 calls `handle_command_ack_(ctl().last_msgid)` whenever a POSITION arrives while a cover op is
awaiting ack. The node publishes POSITION at the end of **every** motor move, including button- and
schedule-driven moves (MotorCtrl.cpp:1332-1336 → CD:1042). INFERRED scenario: a schedule or button move ends
while a hub cover op is lost on air. The hub marks the op delivered, stops retrying, and Home Assistant
shows success. Fix: accept POSITION as confirmation only if it echoes the msgid/cmdId, or drop the rule now
that acks plus AckCache re-ack are reliable.

**F4. LOW-MEDIUM. Pairing of K_enc and K_mac generations is enforced per destination, not per frame.**
Already acknowledged at LC:97-108. Retag checks `s_seal_generation_map[dest]` (LC:1943-1945), which is
**overwritten by the next seal for that destination** (LC:316). A frame still queued from generation g passes
the check once anything is sealed under g+1. The node then verifies the CMAC (new K_mac) and CTR-decrypts
with the new K_enc, producing garbage plaintext that only protobuf parsing stands between and execution
(CD:5228-5235). Rare, because it needs a re-login mid-queue. Fix as part of Stage 4: the seal closure travels
with the frame in `TxPolicy`/`rx_buffer_t` (§5.3, `CopySealer`).

**F5. LOW. A replayed authentic downlink marks the session as proven.** `noteSessionProven()` (CD:6342)
runs before the replay check (CD:6050). After the node restores a session from NVS while the hub has lost it,
a recorded old-session frame cancels the resume fallback (CD:1252-1253). INFERRED effect: re-registration is
delayed until the next fallback path. Fix: move it after `admitFrame` succeeds.

**F6. LOW (bench nodes only). An authenticated MAC_CONFIG is not sequenced.** For non-ping MAC control,
`sequenceRule` returns `None` (FA:146-153), so a captured encrypted MAC_CONFIG can be replayed within the
session to re-disable MAC-1/MAC-2 for control frames for up to 900 s (CD:3734-3786). Non-bench nodes refuse
degrading configs (CD:3740-3742).

**F7. LOW. An unauthenticated encrypted-looking uplink makes the hub start a LOGIN.** This happens when a
registered node has no session (for example after a hub reboot): LC:2091-2106, before any CMAC check. Cost:
one LOGIN exchange (airtime, node battery), bounded by the onboarding gate. Accepting this is reasonable;
document it.

**F8. LOW. Pre-authentication MAC-0 commit from attacker-controlled burst fields.** `burst_end_us_` (clamped
to 17 copies, about 1.5 s of reply deferral per frame) and drift samples (log-only, CD:6104-6108) at
CD:6296. A flood of such frames can delay uplinks; jamming does the same, so this is acceptable.

**F9. INFO. Dead and stale code.** `LoraInterface::sendPacket` (LI:201-250, a legacy fixed header, no
callers) and the unused `#include "blinds.pb-c.h"` (LI:19) are the only protobuf dependency left in the radio
layer, so cutting both makes LI schema-free. The legacy GCM helpers in FC:56-119 are kept "because the hub
pins them in tests" (FC:38-42).

Checked and **not** a finding: CMAC verification before decryption on both sides (CD:5272-5301,
LC:2166-2208). Replay commit only after the tag verifies (CD:6050, LC:2246). Freshness requires
authentication (Mac1Freshness.h:107-119). LOGIN and REGISTER MICs are verified before any state change
(CD:3376-3394). The CMAC prefix covers the timing fields (FC:385-403). The ModeTest arm path is authenticated.

---------------------------------------------------------------------------------------------------

## 5. Target architecture

### 5.1 Principles

- Data plane: request ↓ / confirm ↑ / indication ↑. Management plane: an MLME-style **MAC management
  entity** owns grid, mode and session commands. It receives authenticated management PDUs from MAC-2
  (GridSync, ModeTest, MacControl, LOGIN/REGISTER) and configures lower layers only through *down* calls
  (`mlme_set_*`). This is the 802.15.4/802.11 answer to "MAC control frames that need MAC-2 protection".
- A **port** (LoRaWAN FPort-like) demultiplexes payloads: 0 = MAC-0 management, 1 = MAC-2 session,
  ≥2 = application. Until the wire stage, the port is *computed* from the oneof case by one table, a
  generalised `cmdKindOf` (CD:5327-5339).
- One owner per piece of state. Header-only, dependency-free, vendored and drift-gated like FrameCrypto,
  so hub and node share one implementation.
- No virtual dispatch in the RX hot path. Concrete classes or templates; the cost is negligible against
  the ~10 ms turnaround.

### 5.2 Why MAC-1 and MAC-2 cannot simply be stacked in numbered order

- TX: the CTR nonce **is** the MAC-1 counter (FC:323-332), so MAC-2 needs the counter before it seals.
- RX: the replay window must be committed only **after** MAC-2 authenticates (the lesson behind CD:5413-5419
  and LC:1468-1475). MAC-0's timing commits must wait for the MAC-1 freshness class (CD:5812-5825).

Resolution, using only downward calls and upward indications:
- MAC-1 offers `allocate()`, `check()` (read-only) and `commit()`. MAC-2 calls `allocate` on TX, and
  `check` then `commit` around its verify on RX.
- MAC-0 attaches a `RxTicket` to each indication. MAC-1, after its commit, calls down
  `mac0.commitTiming(ticket, freshness)`.
- This maps 1:1 onto code that already exists: `wouldAcceptRxId/acceptRxId`, `mac1freshness::classify`,
  `commitPhaseSample_`. Equivalent alternative: swap the order (MAC-2 below MAC-1, as in LoRaWAN/IPsec, where
  the integrity check precedes the counter update). It is cleaner on paper but moves more code, so it is not
  recommended for the migration.

### 5.3 Service primitives (sketch, shared `MacSap.h`)

```cpp
namespace mac {
using Addr = uint8_t;  using FCnt = uint32_t;
enum class Port : uint8_t { Mac0Mgmt = 0, Session = 1, App = 2 };
enum class Freshness : uint8_t { New, CurrentBurstCopy, Unsequenced, Stale };   // = mac1freshness::Class

struct MacHdr {              // decoded today from LoraHeader; the fixed wire header in Stage 8
  Addr dst, src; uint8_t subnet; FCnt fcnt; Port port;
  uint8_t burst_idx, burst_cnt; bool on_mark, stamped; uint32_t fire_round, fire_off_us;
  uint32_t session_nonce;    // opening uplink only
};
struct TxOpts { uint8_t copies; uint16_t stride_ms; int64_t earliest_us; bool on_mark,
                expects_reply, ack_request; uint8_t priority; };

// ---- PHY SAP (SX127x) -------------------------------------------------------
struct PhyRxInd { const uint8_t *psdu; uint8_t len; int64_t rxdone_us; uint32_t unc_us;
                  int16_t rssi; int8_t snr; };
class IPhy { public: virtual bool txAt(const uint8_t*, uint8_t, int64_t fire_us)=0;
             virtual void rxWindow(int64_t open_us, uint16_t symtimeout)=0;
             virtual bool cadBusy()=0; virtual void sleep()=0; };
// up-calls: onRxDone(PhyRxInd), onTxDone(int64_t), onRxTimeout()

// ---- MAC-0 SAP (access) -------------------------------------------------------
// Per-copy authentication WITHOUT MAC-0 knowing keys: MAC-2 hands MAC-0 a sealer
// bound to ONE frame's key generation (fixes F4, removes V1's address lookup).
using CopySealer = bool (*)(void *ctx, const MacHdr &copy_hdr,
                            const uint8_t *ct, size_t ct_len, uint8_t tag[8]);
struct RxTicket { int64_t t0_us; uint32_t len; bool timing_trusted; uint32_t window_gen; };
struct Mac0DataReq  { MacHdr hdr; const uint8_t *body; size_t len; TxOpts opts;
                      CopySealer sealer; void *sealer_ctx; };
struct Mac0DataInd  { MacHdr hdr; const uint8_t *body; size_t len; RxTicket ticket; };
class IMac0 { public:
  virtual bool dataRequest(const Mac0DataReq&) = 0;               // -> TxDone confirm
  virtual void commitTiming(const RxTicket&, Freshness, const MacHdr&) = 0;  // MAC-1 calls down
  virtual void retargetUplink(Addr hub, uint8_t subnet) = 0;     // MAC-2 calls after a verified frame (F1)
  // management (MLME-SET), called only by the MAC management entity:
  virtual void setGrid(const gridstate::Params&, int64_t anchor_us) = 0;
  virtual void withdrawGrid() = 0;
};

// ---- MAC-1 SAP (sequencing) -----------------------------------------------------
class Sequencer {            // one class, both sides; node bitmap window, hub ratchet = policy flag
public:
  FCnt      allocate(Addr peer);                                  // 0 = fail closed (N-2)
  Freshness check(Addr peer, FCnt, int64_t now, uint32_t burst_cnt, uint32_t len) const;
  bool      commit(Addr peer, FCnt, int64_t now);                 // only after MAC-2 verified
  void      reset(Addr peer);                                     // on a new session
};

// ---- MAC-2 SAP (security) -------------------------------------------------------
struct SecDataReq { Addr dst; Port port; const uint8_t *pt; size_t len; TxOpts opts; bool require_seal; };
enum class SecRxStatus : uint8_t { Authentic, Plaintext, BadMic, NoSession, MsgidSpent };
struct SecDataInd { MacHdr hdr; Port port; const uint8_t *pt; size_t len;
                    SecRxStatus status; Freshness freshness; };
class LinkSecurity {
public:
  bool dataRequest(const SecDataReq&);    // allocate (MAC-1) -> CTR seal -> mac0.dataRequest + CopySealer
  void onMac0Indication(const Mac0DataInd&); // check -> verify -> decrypt -> commit -> commitTiming -> up
  // session lifecycle (LOGIN/REGISTER become Port::Session PDUs)
  bool hasSession(Addr) const; uint32_t generation(Addr) const;
  // up-calls: onSessionEstablished(peer, hub_rebooted), onSessionLost(peer)
};

// ---- APP SAP ------------------------------------------------------------------
// up-call: onAppIndication(Addr src, const uint8_t *payload, size_t len, bool authentic)
// down:    LinkSecurity::dataRequest({.., Port::App, require_seal=true})
// Reliability (Acked/Fire, retries, FIFO, supersede) = MAC-0 delivery service built on
// DeliveryPolicy.h; APP gets confirm(delivered|failed). CommandAck stays an APP
// "executed" confirm until Stage 8 adds a MAC ACK bit.
}
```

### 5.4 Where the existing pure headers go

| header | layer in the target |
|---|---|
| LoraTiming, TimedGrid, GridState, PhaseTracker, ClassAWindows, TxQueue, TimedModePolicy, ModeTestPolicy, PendingData/PendingShadow, SingleShotAckWait, DeliveryPolicy, AckCache, MacFunnel, DriftEstimator | MAC-0 (AckCache = MAC-0 ARQ re-ack; CurrentBurstCopy is really MAC-0 burst-copy identity) |
| Mac1Freshness, the SessionManager counters half | MAC-1 `Sequencer` |
| FrameCrypto, the SessionManager nonce half, MacSublayers (arming policy) | MAC-2 `LinkSecurity` |
| FrameAdmission | split: `CmdKind` → port table; plaintext gate → MAC-2; `PhaseGate` → MAC-0; `sequenceRule` → MAC-1 |
| FrameCanon | MAC-0 framing; deleted at Stage 8 (raw-byte CMAC over a fixed header) |
| ModeBSupervisor, SleepModel, Scheduler, ScheduleText, AutoModePolicy, OnboardingGate, TimeSyncPolicy | APP or MAC management entity (ModeBSupervisor drives `IMac0::setGrid` via management) |

### 5.5 Minimal clean wire target (Stage 8 only)

```
| ver/flags 1 | dst 1 | src 1 | subnet 1 | FCnt 4 | port 1 | burst idx:cnt 1 | [fireRound 3 | fireOff 3] |
| body (protobuf APP msg or MAC command) | MIC 8 |
flags: encrypted, ack_req, ack, stamped, on_mark, session_opening(+4 B node nonce)
```

- CMAC over the raw header bytes || ciphertext. FrameCanon and the per-copy decode/re-encode go away; per-copy
  stamping becomes fixed-offset byte writes plus one CMAC.
- FCnt is MAC-1 and the nonce, and is reused byte-identically on retransmit. `cmdId` moves into the APP body
  (idempotency, ack attribution).
- The MAC-0 ACK bit and acked FCnt carry ARQ.
- Size is about 13 B + 6 B stamp versus today's varint header of about 12-25 B, so airtime is roughly
  neutral (INFERRED).
- Migration trick: **today's frames always start with 0x0A** (field 1, length-delimited header,
  blinds.proto:867/979). A version byte ≠ 0x0A makes the hub dual-stack cheaply for the node-1 OTA
  transition.

---------------------------------------------------------------------------------------------------

## 6. Staged migration (each stage behaviour-preserving and host-testable, no wire change before Stage 8)

| stage | content | tests / gate | risk |
|---|---|---|---|
| **S0 Pin** | Add `MacSap.h` (types only). Add golden-frame tests: byte-exact wire output for every producer on both sides, captured on today's code. Add a ratcheting layering lint (CI grep counts: `__CMD_`/PSA in `lora_tracker.cpp`, `lora_*` outside PHY files, `blinds.pb-c.h` in LoraInterface). Fix mac-layer.md §1 and §3. | proto_sim, node host tests; lint counts may only fall | none |
| **S1 Findings** | F1 (retarget only on encrypted/verified; fail-closed uplink when a session exists), F3, F5, F6 (sequence MAC_CONFIG through the session window), F9 dead code. | new characterization + mutation tests: forged LOGIN/GridBeacon/GridDemote must not change `destAddress`; spontaneous POSITION must not ack | small behaviour changes, each deliberate and listed |
| **S2 MAC-2 extract** | Node: `LinkSecurity` from CD:164-272, 786-984, 5149-5302, LOGIN/REGISTER crypto. Hub: s_* crypto + `SessionKeyStore` (one owner, replaces members + 5 maps), EncUplink pipeline. API on `MacHdr` + bytes, not protobuf. | golden frames identical; EncUplink/MsgidGuard/seal tests ported; H-1 latency must not regress | key lifetime/mutex rules (LC:295-306, 1926-1935) must move intact |
| **S3 MAC-1 extract** | Shared `Sequencer` (node window; hub keeps the ratchet via a policy flag; switching the hub to the bitmap is a separate, deliberate change). Two-phase check/commit. Persistence stays bit-identical (no NVS version bump). | admitFrame characterization tests; N-1/N-2/N-3 tests | **never touch `kRestoreStateVersion`** (deadlock, CLAUDE.md) |
| **S4 One data-request path** | Node: one `macDataRequest` used by the 3 uplink builders (V10 closes). Hub: one `mac_data_request_` used by all ~10 build sites. `CopySealer` closure travels in `TxPolicy` (F4 closes). Tracker stops looking up clients. | golden frames; burst retag tests; H-1 bench re-measure | hot-path allocation: build the closure ctx without heap allocation |
| **S5 MAC-0 boundary** | Node: `Mac0Access` = LoraInterface + FT RX logic + timing state from CD (grid_, phase_, classA, burst, marks, drift); `RxTicket` + `commitTiming`. Hub: the tracker gets a framing callback instead of `__CMD_` branching; beacon build moves to the management entity. | phase/anchor/WMR tests; ModeTest on bench (Mode A/B turnaround vs 7.0/8.2 and 10.0/10.6 ms baselines) | timing regressions; keep ISR/IRAM rules (INT_WDT history) |
| **S6 Port demux** | One table maps oneof → port. `dispatchCommand` receives APP only; MAC management frames go to the MLME. Hub children receive decoded indications instead of raw bytes (removes LC:2286-2299 re-pack and the lora_cover.cpp:162 hole). | APP tests; cover entity tests | the loracover API changes (internal only) |
| **S7 ARQ ownership** | Delivery ladder and AckCache behind the MAC-0 delivery service; APP handlers stop knowing msgid (ack still emitted on execution, so semantics are unchanged). | DeliveryPolicy, OpRetry, AckPath tests | ack timing (Mode B ack ~1.7 s after the mark): unchanged by construction |
| **S8 Wire (explicit break)** | Fixed header (§5.5), FCnt/cmdId split, MAC ACK bit, raw-byte CMAC, delete FrameCanon and the legacy GCM helpers. Version byte ≠ 0x0A, so the hub is dual-stack during node-1 OTA. Node 2 first over serial, soak, then node 1 by OTA, then remove dual stack. | full H-2/H-3 campaign; MIC_FAIL 0; FER_link 0; 900 s Mode B+crypto soak | the only fleet-migration risk. Justification: removes per-copy decode+re-encode (H-1), the canonicality class of bugs and the 7-role msgid |

**What not to do**
- No big-bang. Do not reorder `admitFrame`'s stages without characterization tests: every ordering comment in
  CD:5304-5324 records a live incident.
- Do not split `msgId` before S8. The overload is made safe by pack-once plus AckCache plus the generation
  checks; changing it piecemeal re-opens nonce reuse.
- No virtual calls, logging or heap allocation inside the retag/verify hot paths (H-1, the 10 ms budget). No
  logging under `s_session_keys_mutex`.
- Do not route MAC control traffic around MAC-2 by default. Keep the MacSublayers safety hinge: switches
  apply to MAC-control only, and arming must be authenticated.
- No NVS layout or version changes in S2/S3. Do not change the REGISTER/LOGIN bypass order (LC:2850-2858,
  CD:3362-3394) during the extraction.
- Do not make the tracker learn the new interfaces by reading `LORAListener` internals (the V3 pattern);
  pass data down with the frame.
- Do not edit the vendored hub copy of shared headers; the node copy is the source (FC:1-16).

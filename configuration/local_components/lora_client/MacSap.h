#pragma once

#include <stddef.h>
#include <stdint.h>

#include "Mac1Freshness.h"

// ---------------------------------------------------------------------------
// MacSap.h -- the SERVICE ACCESS POINT types of the MAC sublayers.
//
// Stage S0 of the MAC-layer separation (mac-review.md sections 5.3 and 6). This
// header holds TYPES ONLY: no function, no class with behaviour, no virtual
// interface. It pins the vocabulary the later stages (S2 MAC-2 extract, S3
// MAC-1 Sequencer, S4 one data-request path, S5 MAC-0 boundary) build on, so
// hub and node agree on the shape of a primitive before any code moves.
//
//   PHY   <- MAC-0 (access: addressing, framing, windows, bursts, timing)
//         <- MAC-1 (sequencing: frame counter, replay window, freshness)
//         <- MAC-2 (security: Encrypt-then-CMAC, session lifecycle)
//         <- APP   (covers, schedules, telemetry)
//
// Data plane: request down, confirm/indication up. MAC-1 sits BETWEEN the two
// halves of MAC-2's receive path (check -> verify -> commit), see mac-review.md
// section 5.2 -- which is why RxTicket exists: MAC-0 hands it up with the frame
// and MAC-1, after its commit, hands it back down with the freshness class.
//
// SHARED, NODE OWNS IT. Dependency-free (<stdint.h>, <stddef.h> and
// Mac1Freshness.h, which itself needs only LoraTiming.h and TimedGrid.h). The
// hub vendors a byte-identical copy into local_components/lora_client/ and a
// drift gate compares the two -- edit THIS file, never the vendored one.
//
// Since S8 the fields of MacHdr are the MAC wire v1 fixed header (MacWire.h,
// docs/mac-wire-format.md). Today the layers call each other through bespoke
// methods (LinkSecurity::sealWireSession, Mac0Access::commitTiming, ...); the
// request/indication structs below name the primitives those methods
// implement (review N26: the SAP is the vocabulary, not yet the call API).
// ---------------------------------------------------------------------------

namespace mac
{

// A short (one-byte) node/hub address. 0xFF is broadcast, 0 is "unprovisioned".
using Addr = uint8_t;

// MAC-1 frame counter: the header's FCNT (31 bits usable: the CTR counter
// block holds 31, see framecrypto::msgidFitsCtr). Older code calls it msgid.
using FCnt = uint32_t;

// Payload demultiplexer (LoRaWAN FPort-like): the header's PORT byte, which
// selects the body PDU (0 HubMacPdu/NodeMacPdu, 1 LoginMsg/ClientRegister,
// 2 HubAppPdu/NodeAppPdu).
enum class Port : uint8_t
{
    Mac0Mgmt = 0,   // grid, beacon, demote, ModeTest, MAC control
    Session  = 1,   // LOGIN / REGISTER (MAC-2 session lifecycle)
    App      = 2,   // covers, schedules, telemetry, sysops
};

// MAC-1's verdict on a received frame, for timing bookkeeping. One type, two
// names: the MAC-0/MAC-1 boundary speaks `mac::Freshness`, the existing policy
// header keeps `mac1freshness::Class`.
using Freshness = mac1freshness::Class;

// The MAC header, decoded (the fields MAC-0 and MAC-1 need; AUTH, ACK_REQ and
// ACKED_FCNT stay in macwire::WireHdr).
struct MacHdr
{
    Addr     dst{0};
    Addr     src{0};
    uint8_t  subnet{0};
    FCnt     fcnt{0};
    Port     port{Port::App};
    // Burst copy position (hub downlinks; node uplinks never burst).
    uint8_t  burst_idx{0};
    uint8_t  burst_cnt{0};
    bool     on_mark{false};
    // The copy declares its own fire instant on the hub grid.
    bool     stamped{false};
    uint32_t fire_round{0};
    uint32_t fire_off_us{0};
    // Session-opening uplink only (the NODE_NONCE extension); 0 on every other frame.
    uint32_t session_nonce{0};
};

// How MAC-0 should put a frame on the air.
struct TxOpts
{
    uint8_t  copies{1};          // 1 = single shot, 17 = Mode A burst
    uint16_t stride_ms{0};       // copy spacing when copies > 1
    int64_t  earliest_us{0};     // 0 = as soon as the channel allows
    bool     on_mark{false};     // placed on the destination's Mode B mark
    bool     expects_reply{false};
    bool     ack_request{false};
    uint8_t  priority{0};
};

// Per-copy authentication WITHOUT MAC-0 knowing keys: MAC-2 hands MAC-0 a
// sealer bound to ONE frame's key generation; MAC-0 calls it once per copy
// after stamping that copy's header bytes. Since S8 the tag covers the raw
// copy: `frame` / `frame_len` are the copy's bytes up to the tag (header
// included), not a ciphertext. Returns false to drop the copy.
using CopySealer = bool (*)(void *ctx, const MacHdr &copy_hdr,
                            const uint8_t *frame, size_t frame_len, uint8_t tag[8]);

// What MAC-0 knows about a received frame's timing. Travels up with the
// indication and back down with MAC-1's freshness verdict (commitTiming).
struct RxTicket
{
    int64_t  t0_us{0};            // SFD-end T0 recovered from RxDone
    uint32_t len{0};              // on-air length
    // May the length be used for T0? Always true on MAC wire v1, whose tag
    // covers every raw byte (the FrameCanon check it replaced is gone).
    bool     timing_trusted{false};
    uint32_t window_gen{0};       // the RX window that heard it (mark hits are window-scoped)
};

// MAC-0 data request (down) and indication (up).
struct Mac0DataReq
{
    MacHdr         hdr{};
    const uint8_t *body{nullptr};
    size_t         len{0};
    TxOpts         opts{};
    CopySealer     sealer{nullptr};
    void          *sealer_ctx{nullptr};
};

struct Mac0DataInd
{
    MacHdr         hdr{};
    const uint8_t *body{nullptr};
    size_t         len{0};
    RxTicket       ticket{};
};

// MAC-2 receive outcome.
enum class SecRxStatus : uint8_t
{
    Authentic,    // CMAC verified, decrypted
    Plaintext,    // arrived unsealed (bootstrap / exempt broadcast)
    BadMic,       // CMAC (or LOGIN/REGISTER MIC) failed
    NoSession,    // sealed, but no keys for this peer
    MsgidSpent,   // the FCNT space is exhausted (2^31); a fresh session is needed
};

// MAC-2 data request (down) and indication (up).
struct SecDataReq
{
    Addr           dst{0};
    Port           port{Port::App};
    const uint8_t *pt{nullptr};
    size_t         len{0};
    TxOpts         opts{};
    bool           require_seal{true};   // fail closed when no session key exists
};

struct SecDataInd
{
    MacHdr         hdr{};
    Port           port{Port::App};
    const uint8_t *pt{nullptr};
    size_t         len{0};
    SecRxStatus    status{SecRxStatus::Plaintext};
    Freshness      freshness{Freshness::Stale};
};

}  // namespace mac

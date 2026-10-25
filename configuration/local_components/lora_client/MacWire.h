// =============================================================================
// VENDORED COPY — DO NOT EDIT HERE.
//
// Source of truth: BlindsESP/main/include/MacWire.h
// Sync with:       BlindsESP/proto/regen_stubs.sh  (copies this header too)
// Guarded by:      ctest `macwire_drift_hub_vs_node`
//
// MAC wire format v1 (docs/mac-wire-format.md). The hub build (ESPHome)
// compiles only local_components/, so it carries this copy; everything from
// include-guard line down must stay byte-identical to the node's file.
// =============================================================================
#pragma once

#include <stddef.h>
#include <stdint.h>
#include <string.h>

// ---------------------------------------------------------------------------
// MacWire.h -- MAC wire format v1 (stage S8), the fixed-offset frame header.
//
// Specification: esphome/configuration docs/mac-wire-format.md.
// This header holds every constant, offset and layout helper of that spec and
// nothing else: no crypto calls, no protobuf, no ESP-IDF. Shared: the node copy
// is the source, the hub vendors it byte-identical under a drift gate.
//
//   offset size field
//   0      1    VER        0x01 (never 0x0A: every v0 frame starts with 0x0A)
//   1      1    FLAGS      STAMPED, ON_MARK, ACK_REQ, ACK, SESSION_OPENING, AUTH(2), reserved
//   2      1    DST        0xFF broadcast, 0x00 unprovisioned
//   3      1    SRC        hub = 0x01
//   4      1    SUBNET     0xFE = any subnet
//   5      4    FCNT       u32 BE, bit 31 must be 0 -- checked by the receiver AFTER the tag
//   9      1    PORT       0 Mac0Mgmt, 1 Session, 2 App; 3..255 dropped
//   10     1    BURST_IDX
//   11     1    BURST_CNT  0 = not a burst
//   12     ...  STAMP 6 (STAMPED: u24 FIRE_ROUND, u24 FIRE_OFFSET_US),
//               NODE_NONCE 4 (SESSION_OPENING), ACKED_FCNT 4 (ACK), in this order
//   H      ...  BODY       one protobuf message, plaintext or AES-CTR ciphertext
//   L-8    8    TAG        iff AUTH != NONE
//
// All multi-byte fields big-endian. The tag is AES-CMAC (truncated to 8) over
// buildMacInputPrefix() || frame[0 .. L-8) -- every transmitted byte except the
// tag, so the on-air length is authenticated and there is no canonical-length
// problem.
//
// decode() makes SHAPE checks only; it never trusts content (FCNT bit 31, burst
// position, stamp values are the receiver's business after the tag verified).
// ---------------------------------------------------------------------------

namespace macwire
{

static constexpr uint8_t kVersion          = 0x01;
static constexpr uint8_t kV0FirstByte      = 0x0A;   // a v0 frame: field 1, length-delimited
static constexpr size_t  kFixedHeaderBytes = 12;
static constexpr size_t  kStampBytes       = 6;
static constexpr size_t  kNonceBytes       = 4;
static constexpr size_t  kAckedFcntBytes   = 4;
static constexpr size_t  kTagBytes         = 8;
static constexpr size_t  kMaxFrameBytes    = 255;   // SX127x payload limit
static constexpr size_t  kMaxHeaderBytes   = kFixedHeaderBytes + kStampBytes + kNonceBytes + kAckedFcntBytes;
static constexpr size_t  kMacInputPrefixBytes = 8;   // "BW1" || dir || id u32 (never transmitted)

// Offsets of the fixed part.
static constexpr size_t kOffVer      = 0;
static constexpr size_t kOffFlags    = 1;
static constexpr size_t kOffDst      = 2;
static constexpr size_t kOffSrc      = 3;
static constexpr size_t kOffSubnet   = 4;
static constexpr size_t kOffFcnt     = 5;
static constexpr size_t kOffPort     = 9;
static constexpr size_t kOffBurstIdx = 10;
static constexpr size_t kOffBurstCnt = 11;
static constexpr size_t kOffExt      = 12;   // the STAMP extension, when present, starts here

// FLAGS (byte 1).
static constexpr uint8_t kFlagStamped        = 0x01;
static constexpr uint8_t kFlagOnMark         = 0x02;
static constexpr uint8_t kFlagAckReq         = 0x04;
static constexpr uint8_t kFlagAck            = 0x08;
static constexpr uint8_t kFlagSessionOpening = 0x10;
static constexpr uint8_t kAuthShift          = 5;
static constexpr uint8_t kAuthMask           = 0x60;
static constexpr uint8_t kFlagReserved       = 0x80;   // must be 0; a frame with it set is dropped

enum class Auth : uint8_t
{
    None    = 0,   // no tag (bootstrap plaintext, GridDemote, MAC-2-off pings)
    Session = 1,   // body AES-CTR under K_enc, tag under K_mac
    KAuth   = 2,   // body plaintext, tag = MIC under K_auth (LOGIN, REGISTER)
    Fleet   = 3,   // body plaintext, tag under the fleet key (GridBeacon)
};

// PORT values (= mac::Port in MacSap.h).
static constexpr uint8_t kPortMac0Mgmt = 0;
static constexpr uint8_t kPortSession  = 1;
static constexpr uint8_t kPortApp      = 2;
static constexpr uint8_t kMaxPort      = kPortApp;

static constexpr uint8_t  kBroadcast     = 0xFF;
static constexpr uint8_t  kUnprovisioned = 0x00;
static constexpr uint8_t  kAnySubnet     = 0xFE;
static constexpr uint32_t kFcntLimit     = 0x80000000u;   // FCNT must stay below (the CTR block holds 31 bits)
static constexpr uint32_t kU24Max        = 0x00FFFFFFu;

inline Auth    authOf(uint8_t flags) { return (Auth) ((flags & kAuthMask) >> kAuthShift); }
inline uint8_t withAuth(uint8_t flags, Auth a)
{
    return (uint8_t) ((flags & ~kAuthMask) | (((uint8_t) a << kAuthShift) & kAuthMask));
}

// Every header field, decoded. `flags` is the raw byte (AUTH included); the
// extension fields are meaningful only when their flag is set.
struct WireHdr
{
    uint8_t  flags{0};
    uint8_t  dst{0};
    uint8_t  src{0};
    uint8_t  subnet{0};
    uint32_t fcnt{0};
    uint8_t  port{0};
    uint8_t  burst_idx{0};
    uint8_t  burst_cnt{0};
    uint32_t fire_round{0};       // u24 (STAMPED)
    uint32_t fire_offset_us{0};   // u24 (STAMPED)
    uint32_t node_nonce{0};       // SESSION_OPENING
    uint32_t acked_fcnt{0};       // ACK

    bool stamped() const        { return (flags & kFlagStamped) != 0; }
    bool onMark() const         { return (flags & kFlagOnMark) != 0; }
    bool ackReq() const         { return (flags & kFlagAckReq) != 0; }
    bool ack() const            { return (flags & kFlagAck) != 0; }
    bool sessionOpening() const { return (flags & kFlagSessionOpening) != 0; }
    Auth auth() const           { return authOf(flags); }
};

// H = 12 + 6*STAMPED + 4*SESSION_OPENING + 4*ACK.
inline size_t headerLenForFlags(uint8_t flags)
{
    return kFixedHeaderBytes + ((flags & kFlagStamped) ? kStampBytes : 0) +
           ((flags & kFlagSessionOpening) ? kNonceBytes : 0) + ((flags & kFlagAck) ? kAckedFcntBytes : 0);
}
inline size_t headerLen(const WireHdr &h) { return headerLenForFlags(h.flags); }
inline size_t tagLen(uint8_t flags) { return authOf(flags) == Auth::None ? 0 : kTagBytes; }

// ---- big-endian helpers ---------------------------------------------------------
inline void putU24(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t) (v >> 16); p[1] = (uint8_t) (v >> 8); p[2] = (uint8_t) v;
}
inline void putU32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t) (v >> 24); p[1] = (uint8_t) (v >> 16); p[2] = (uint8_t) (v >> 8); p[3] = (uint8_t) v;
}
inline uint32_t getU24(const uint8_t *p)
{
    return ((uint32_t) p[0] << 16) | ((uint32_t) p[1] << 8) | (uint32_t) p[2];
}
inline uint32_t getU32(const uint8_t *p)
{
    return ((uint32_t) p[0] << 24) | ((uint32_t) p[1] << 16) | ((uint32_t) p[2] << 8) | (uint32_t) p[3];
}

// ---- decode: shape checks only -----------------------------------------------------
// Refuses: no buffer, shorter than the fixed header, longer than a LoRa payload,
// VER != 0x01 (a v0 frame starts 0x0A), the reserved flag bit, PORT > 2, a
// header whose extensions overrun the frame, and AUTH != NONE without room for
// the tag. On success *body_off / *body_len describe the body (ciphertext for
// AUTH = SESSION), which may be empty.
inline bool decode(const uint8_t *buf, size_t len, WireHdr *out, size_t *body_off, size_t *body_len)
{
    if (buf == nullptr || out == nullptr || len < kFixedHeaderBytes || len > kMaxFrameBytes)
        return false;
    if (buf[kOffVer] != kVersion)
        return false;
    const uint8_t flags = buf[kOffFlags];
    if (flags & kFlagReserved)
        return false;
    if (buf[kOffPort] > kMaxPort)
        return false;
    const size_t hlen = headerLenForFlags(flags);
    const size_t tlen = tagLen(flags);
    if (hlen + tlen > len)
        return false;

    WireHdr h;
    h.flags     = flags;
    h.dst       = buf[kOffDst];
    h.src       = buf[kOffSrc];
    h.subnet    = buf[kOffSubnet];
    h.fcnt      = getU32(buf + kOffFcnt);
    h.port      = buf[kOffPort];
    h.burst_idx = buf[kOffBurstIdx];
    h.burst_cnt = buf[kOffBurstCnt];
    size_t off = kOffExt;
    if (flags & kFlagStamped)
    {
        h.fire_round     = getU24(buf + off);
        h.fire_offset_us = getU24(buf + off + 3);
        off += kStampBytes;
    }
    if (flags & kFlagSessionOpening)
    {
        h.node_nonce = getU32(buf + off);
        off += kNonceBytes;
    }
    if (flags & kFlagAck)
    {
        h.acked_fcnt = getU32(buf + off);
        off += kAckedFcntBytes;
    }
    *out = h;
    if (body_off != nullptr)
        *body_off = off;
    if (body_len != nullptr)
        *body_len = len - off - tlen;
    return true;
}

// ---- encode the header (fixed part + extensions) ------------------------------------------
// Returns the header length written, or 0 if `cap` is too small or the header is
// not encodable: the reserved bit set, PORT > 2, or a STAMP value above 24 bits.
inline size_t encodeHeader(const WireHdr &h, uint8_t *out, size_t cap)
{
    const size_t hlen = headerLen(h);
    if (out == nullptr || cap < hlen)
        return 0;
    if ((h.flags & kFlagReserved) || h.port > kMaxPort)
        return 0;
    if (h.stamped() && (h.fire_round > kU24Max || h.fire_offset_us > kU24Max))
        return 0;
    out[kOffVer]      = kVersion;
    out[kOffFlags]    = h.flags;
    out[kOffDst]      = h.dst;
    out[kOffSrc]      = h.src;
    out[kOffSubnet]   = h.subnet;
    putU32(out + kOffFcnt, h.fcnt);
    out[kOffPort]     = h.port;
    out[kOffBurstIdx] = h.burst_idx;
    out[kOffBurstCnt] = h.burst_cnt;
    size_t off = kOffExt;
    if (h.stamped())
    {
        putU24(out + off, h.fire_round);
        putU24(out + off + 3, h.fire_offset_us);
        off += kStampBytes;
    }
    if (h.sessionOpening())
    {
        putU32(out + off, h.node_nonce);
        off += kNonceBytes;
    }
    if (h.ack())
    {
        putU32(out + off, h.acked_fcnt);
        off += kAckedFcntBytes;
    }
    return off;
}

// ---- the tag's input prefix -------------------------------------------------------------
// "BW1" || dir (1 = hub->node, 0 = node->hub) || id u32, where id is the
// session id for AUTH = SESSION, 0 for AUTH = K_AUTH, the fleet key id for
// AUTH = FLEET. Followed (by the caller) by frame[0 .. L-8).
inline size_t buildMacInputPrefix(uint8_t out[kMacInputPrefixBytes], bool downlink, uint32_t session_or_key_id)
{
    out[0] = 'B'; out[1] = 'W'; out[2] = '1';
    out[3] = downlink ? 1u : 0u;
    putU32(out + 4, session_or_key_id);
    return kMacInputPrefixBytes;
}

// ---- per-copy burst restamp ---------------------------------------------------------------
// Writes, with no decode, exactly: FLAGS bits STAMPED/ON_MARK (all other flag
// bits untouched), BURST_IDX, BURST_CNT, and -- when stamped -- the STAMP
// extension at bytes 12..17. The frame's layout is decided once per burst, so
// the STAMPED bit already in the frame must equal `stamped` (else the length
// would change between copies): false, nothing written. Also false for a frame
// too short for its own header or a stamp value above 24 bits. The tag is the
// caller's (one CMAC per copy over the prefix and frame[0 .. L-8)).
inline bool stampCopy(uint8_t *frame, size_t len, uint8_t idx, uint8_t cnt, bool on_mark, bool stamped,
                      uint32_t round, uint32_t offset)
{
    if (frame == nullptr || len < kFixedHeaderBytes || frame[kOffVer] != kVersion)
        return false;
    const uint8_t flags = frame[kOffFlags];
    if (((flags & kFlagStamped) != 0) != stamped)
        return false;
    if (headerLenForFlags(flags) + tagLen(flags) > len)
        return false;
    if (stamped && (round > kU24Max || offset > kU24Max))
        return false;
    frame[kOffFlags]    = (uint8_t) ((flags & ~kFlagOnMark) | (on_mark ? kFlagOnMark : 0));
    frame[kOffBurstIdx] = idx;
    frame[kOffBurstCnt] = cnt;
    if (stamped)
    {
        putU24(frame + kOffExt, round);
        putU24(frame + kOffExt + 3, offset);
    }
    return true;
}

// ---- FIRE_ROUND modulo 2^24 ---------------------------------------------------------------
// (a - b) mod 2^24 as a signed 24-bit difference: the only comparison a receiver
// makes between a stamped round and one it computed recently (spec 2.3).
inline int32_t roundDelta24(uint32_t a, uint32_t b)
{
    const uint32_t d = (a - b) & kU24Max;
    return (d & 0x00800000u) ? (int32_t) d - (int32_t) 0x01000000 : (int32_t) d;
}

}  // namespace macwire

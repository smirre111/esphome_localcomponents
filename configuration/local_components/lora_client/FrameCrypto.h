// =============================================================================
// VENDORED COPY — DO NOT EDIT HERE.
//
// Source of truth: BlindsESP/main/include/FrameCrypto.h
// Sync with:       BlindsESP/proto/regen_stubs.sh  (copies this header too)
// Guarded by:      ctest `wire_format_drift_hub_vs_node`
//
// The hub build (ESPHome) compiles only local_components/, so it cannot include
// the node's header directly — the same constraint that forces a second copy of
// the protobuf stubs. Vendoring + a drift gate is the established answer here.
//
// Before this existed the AAD/IV layout was written out FOUR times: the node's
// header, the hub's encrypt path, the hub's decrypt path (as lambdas inside
// set_response), and the test sim. Two of those lived in this same file, so the
// hub built its AAD one way to encrypt and another way to decrypt.
// =============================================================================

#pragma once

#include <stdint.h>
#include <stddef.h>

// ---------------------------------------------------------------------------
// FrameCrypto — the AEAD wire-format derivations, as pure functions.
//
// Dependency-free (no PSA, no ESP-IDF), like BootPolicy.h and AutoModePolicy.h,
// so the host harness verifies the byte layouts directly. The PSA calls
// themselves stay in CmdDispatcher: only the parts that decide WHAT bytes go
// into the IV and the AAD move here.
//
// This is the highest-consequence arithmetic in the system and the least
// forgiving to debug. Every failure mode looks identical from the outside —
// `psa_aead_decrypt failed: -149` — whether the cause is a wrong nonce, a
// wrong AAD, a byte-order slip, or a missing direction bit. That single error
// code has stood for at least three different root causes during this project,
// so the layouts are pinned here field by field.
//
// Format, both directions:
//
//   AAD  = destAddress_BE32 || destSubnet_BE32 || senderAddress_BE32 || msgid_BE32
//          (16 bytes, header only — the payload is NOT in the AAD)
//   IV   = baseNonce_BE32 || counter_BE64                    (12 bytes)
//   tag  = 8 bytes (truncated)
//   ciphertext covers the PAYLOAD ONLY
// ---------------------------------------------------------------------------

namespace framecrypto
{

static constexpr size_t kAadBytes   = 16;
static constexpr size_t kIvBytes    = 12;
static constexpr size_t kTagBytes   = 8;   // truncated, to stay slim on air
static constexpr size_t kKeyBytes   = 16;  // AES-128

// Direction separation for the GCM nonce.
//
// Uplink and downlink share the same per-peer base nonce and both derive the IV
// as baseNonce || counter, so without this an uplink and a downlink carrying
// the same msgid would reuse an IV — fatal for AES-GCM, and silent.
//
// msgid is a uint32 and never approaches 2^63, so the top bit is always free.
// Uplink leaves it CLEAR, which keeps uplink IVs byte-identical to the
// pre-direction-bit format: that backward compatibility is deliberate and must
// not be "tidied" by flagging uplink instead.
static constexpr uint64_t kDownlinkFlag = (1ULL << 63);

inline void u32be(uint32_t v, uint8_t *out)
{
    out[0] = static_cast<uint8_t>((v >> 24) & 0xFF);
    out[1] = static_cast<uint8_t>((v >> 16) & 0xFF);
    out[2] = static_cast<uint8_t>((v >> 8) & 0xFF);
    out[3] = static_cast<uint8_t>(v & 0xFF);
}

inline void u64be(uint64_t v, uint8_t *out)
{
    for (int i = 0; i < 8; ++i)
        out[i] = static_cast<uint8_t>((v >> (56 - 8 * i)) & 0xFF);
}

// The frame counter that feeds the IV. Downlink sets the direction bit.
inline uint64_t frameCounter(uint32_t msgid, bool downlink)
{
    const uint64_t c = static_cast<uint64_t>(msgid);
    return downlink ? (c | kDownlinkFlag) : c;
}

// AAD is the 16-byte four-field header. It deliberately does NOT include the
// payload (that is what the ciphertext covers) and no longer includes the old
// `encrypted` header field, which was removed from the wire.
inline void buildAad(uint32_t dest_address, uint32_t dest_subnet,
                     uint32_t sender_address, uint32_t msgid,
                     uint8_t out[kAadBytes])
{
    u32be(dest_address, out);
    u32be(dest_subnet, out + 4);
    u32be(sender_address, out + 8);
    u32be(msgid, out + 12);
}

// IV = baseNonce_BE32 || counter_BE64.
//
// A zero base nonce is not a session — encrypting with it produces frames the
// peer cannot authenticate — so this refuses rather than emitting a usable-
// looking IV.
inline bool deriveIv(uint32_t base_nonce, uint64_t counter, uint8_t out[kIvBytes])
{
    if (base_nonce == 0)
        return false;
    u32be(base_nonce, out);
    u64be(counter, out + 4);
    return true;
}

// ---------------------------------------------------------------------------
// The broadcast beacon's authenticator (section 4.4).
//
// The beacon is one frame for 32 nodes, so it cannot be sealed with a per-node
// session key — but nothing in it is secret either. It needs AUTHENTICITY, not
// confidentiality, and that is a MAC rather than an AEAD.
//
// AES-CMAC under a fleet key, deliberately, and not AES-GCM under a shared base
// nonce. GCM would need a counter that never repeats under the fleet key; the
// hub reboots and restarts its counter while every node still holds the key, so
// it would have to be persisted in NVS, and the penalty for one repeat is total
// rather than partial. Two AAD-only tags under the same nonce yield
// GHASH(H,A1) XOR GHASH(H,A2), a polynomial solvable for the hash subkey H,
// after which an attacker forges beacons freely. CMAC has no nonce at all.
//
// FRESHNESS IS NOT CARRIED HERE. A replayed beacon declares the round it was
// minted for, so the node's own arithmetic already refuses it — the predicted
// mark is rounds in the past, outside the guard band. The MAC stops forgery and
// nothing else, which is why there is no timestamp in the input below.
//
// The input is FIXED LENGTH and starts with a domain tag. Fixed length so there
// is no field-boundary ambiguity to exploit — with variable-length fields, two
// different beacons could otherwise serialise to the same bytes. The domain tag
// so this key can never be made to authenticate anything but a beacon.
//
//   input = "GB1" || netKeyId_BE32 || txRound_BE32 || txSlot_BE32
//           || pendingMask_BE32 || pendingMaskValid_u8            (20 bytes)
// ---------------------------------------------------------------------------

static constexpr size_t kNetKeyBytes     = 16;  // AES-128, like the session key
static constexpr size_t kBeaconMacBytes  = 8;   // truncated, same budget as the
                                                // AEAD tag already on this link
static constexpr size_t kBeaconMacInputBytes = 20;

inline void buildBeaconMacInput(uint32_t net_key_id, uint32_t tx_round,
                                uint32_t tx_slot, uint32_t pending_mask,
                                bool pending_mask_valid,
                                uint8_t out[kBeaconMacInputBytes])
{
    // The domain tag. Three bytes rather than four so the whole input stays a
    // round 20; it is a separator, not a length.
    out[0] = 'G';
    out[1] = 'B';
    out[2] = '1';
    u32be(net_key_id,  out + 3);
    u32be(tx_round,    out + 7);
    u32be(tx_slot,     out + 11);
    u32be(pending_mask, out + 15);
    // The validity flag is covered too. It is the field that decides whether
    // the mask means anything at all, so a MAC that omitted it would let an
    // attacker flip "listen" to "a real all-clear" without touching a signed
    // byte.
    out[19] = pending_mask_valid ? 1u : 0u;
}

// A key of all zeroes is not a key — it is an unset field, or a proto3 default
// — so callers ask this rather than testing bytes themselves. Paired with a
// non-zero id: both must be present before a node believes it holds a key.
inline bool netKeyIsSet(const uint8_t *key, size_t len, uint32_t net_key_id)
{
    if (key == nullptr || len != kNetKeyBytes || net_key_id == 0)
        return false;
    uint8_t acc = 0;
    for (size_t i = 0; i < len; ++i)
        acc = (uint8_t) (acc | key[i]);
    return acc != 0;
}

}  // namespace framecrypto

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
// FrameCrypto -- the link-security byte layouts, as pure functions.
//
// Dependency-free (no PSA, no ESP-IDF), so the host harness verifies the byte
// layouts directly. The PSA calls themselves stay in comm_utils.c /
// LinkSecurity.h (node) and lora_client.cpp (hub): only the parts that decide
// WHAT bytes go into a key derivation or a CTR counter block live here.
//
// What is left after MAC wire v1 (S8 W5b): the fleet-key helper, the key
// hierarchy (K_auth / K_enc / K_mac inputs), the CTR initial counter block and
// the 31-bit counter-space rule. The v1 tag input (the 8-byte "BW1" prefix
// over the raw frame) is MacWire.h's. DELETED with the v0 wire format: the
// AES-GCM AAD / IV helpers, the GridBeacon MAC input ("GB2"), the LOGIN /
// REGISTER MIC inputs ("LG1" / "RG1") and the 45-byte Encrypt-then-CMAC prefix
// ("ETM1") over decoded header fields.
// ---------------------------------------------------------------------------

namespace framecrypto
{

inline void u32be(uint32_t v, uint8_t *out)
{
    out[0] = static_cast<uint8_t>((v >> 24) & 0xFF);
    out[1] = static_cast<uint8_t>((v >> 16) & 0xFF);
    out[2] = static_cast<uint8_t>((v >> 8) & 0xFF);
    out[3] = static_cast<uint8_t>(v & 0xFF);
}

// ---------------------------------------------------------------------------
// The fleet key (section 4.4): the GridBeacon is one frame for 32 nodes, so it
// is authenticated under a key the hub mints with the grid and publishes in
// GridSync, not under a per-node session key. Under MAC wire v1 it is the
// FLEET tag (MacWire.h prefix with the key id) over the raw beacon frame.
// ---------------------------------------------------------------------------

static constexpr size_t kNetKeyBytes = 16;  // AES-128, like the session key

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

// ---------------------------------------------------------------------------
// Tier 3: Encrypt-then-CMAC (docs/mac-separation-implementation-plan.md,
// section 2(b)'s "READ THIS FIRST" synthesis); since S8 the tag covers the raw
// v1 frame (MacWire.h), the keys and the counter block are unchanged.
//
// WHY Encrypt-then-CMAC instead of AES-GCM: GCM requires a per-call nonce
// that never repeats under the key; this link's single-encrypt-many-copies
// burst shape (seal once, stamp+retransmit the same ciphertext across up to
// 17 copies with per-copy timing fields) made that nonce-uniqueness
// invariant fragile across three review rounds. CTR for confidentiality
// (encrypt once per msgId, at allocation time) plus a nonce-free CMAC for
// per-copy authenticity (tag any number of times, since CMAC has no nonce to
// reuse) matches the actual access pattern instead of fighting it.
// ---------------------------------------------------------------------------

static constexpr size_t kSessionCmacTagBytes = 8;   // truncated tag, = macwire::kTagBytes

// ---- Key hierarchy (one shared K_root, not per-node — see the plan's Tier 2
// postponement note: the uniqueness/freshness properties below come from the
// nonce-mixing in session establishment, not from the root key being
// per-node) ----
//
//   K_auth = AES-CMAC(K_root, 0x03 || "BLS1" || 0^11)        (16 bytes in)
//   K_enc  = AES-CMAC(K_root, 0x01 || "BLS1" || sessionId_BE32 ||
//                      nodeNonce_BE32 || hubAddr_u8 || nodeAddr_u8)
//   K_mac  = identical to K_enc's input with 0x02 in place of 0x01
//
// Each input is exactly 16 bytes (one AES block), so the CMAC that derives
// each key is a single-block operation.
static constexpr size_t kKdfInputBytes = 16;

inline void buildKAuthKdfInput(uint8_t out[kKdfInputBytes])
{
    out[0] = 0x03;
    out[1] = 'B'; out[2] = 'L'; out[3] = 'S'; out[4] = '1';
    for (size_t i = 5; i < kKdfInputBytes; ++i) out[i] = 0;
}

// `half` is 0x01 for K_enc, 0x02 for K_mac — the only byte that differs.
inline void buildSessionKeyKdfInput(uint8_t half, uint32_t session_id,
                                    uint32_t node_nonce, uint8_t hub_addr,
                                    uint8_t node_addr, uint8_t out[kKdfInputBytes])
{
    out[0] = half;
    out[1] = 'B'; out[2] = 'L'; out[3] = 'S'; out[4] = '1';
    u32be(session_id, out + 5);
    u32be(node_nonce, out + 9);
    out[13] = hub_addr;
    out[14] = node_addr;
    out[15] = 0;
}

// ---- Encrypt-then-CMAC wire construction ----
//
// CTR initial counter block (16 bytes):
//   sessionId_BE32 || nodeAddr_BE32 || ((dir<<31)|msgId)_BE32 || blockIdx_BE32
//
// `nodeAddr` is whichever address is the non-hub endpoint, in BOTH
// directions — this is what removes the cross-node IV-domain collision a
// per-direction-only split would still have, since two different nodes
// could otherwise share a counter block under the same sessionId.
static constexpr size_t kCtrBlockBytes = 16;

inline void buildCtrInitialBlock(uint32_t session_id, uint32_t node_addr,
                                 bool downlink, uint32_t msgid,
                                 uint32_t block_idx, uint8_t out[kCtrBlockBytes])
{
    u32be(session_id, out);
    u32be(node_addr, out + 4);
    const uint32_t dir_msgid = (downlink ? 0x80000000u : 0u) | (msgid & 0x7FFFFFFFu);
    u32be(dir_msgid, out + 8);
    u32be(block_idx, out + 12);
}

// The counter block has room for only 31 bits of msgid (the top bit is the
// direction bit), but the CMAC prefix covers all 32. So msgid X and
// X + 2^31 get the IDENTICAL keystream under one session key while still
// carrying different tags: a two-time pad. buildCtrInitialBlock() masks
// rather than refuses (its output is the wire format and must stay
// byte-identical on both ends), so every caller must refuse such a msgid
// BEFORE sealing or opening a frame, and move to a fresh session instead,
// whose key derivation resets the counters.
//
// 2^31 frames is years of continuous traffic, which is exactly why this is
// written down rather than left to the arithmetic never being reached: a
// counter restored from NVS, or a corrupt one, gets there at once.
static constexpr uint32_t kMsgidCtrLimit = 0x80000000u;

// May a frame carrying this msgid be sealed (uplink) or opened (downlink)?
inline bool msgidFitsCtr(uint32_t msgid)
{
    return msgid < kMsgidCtrLimit;
}

// Allocation form: given the LAST msgid handed out, is the next one still
// inside the CTR space? Written as a comparison against the limit minus one
// so that last == 0xFFFFFFFF cannot wrap `last + 1` back to 0 and pass.
inline bool nextMsgidFitsCtr(uint32_t last_msgid)
{
    return last_msgid < kMsgidCtrLimit - 1u;
}

}  // namespace framecrypto

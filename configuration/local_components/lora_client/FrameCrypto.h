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
// LEGACY (AES-GCM era). The AAD / IV / direction-bit helpers below (kAadBytes,
// kIvBytes, kTagBytes, kDownlinkFlag, frameCounter, buildAad, deriveIv) are no
// longer used by the node firmware since the Tier 3 Encrypt-then-CMAC cutover;
// they are kept only because the hub vendors this file and still pins them in
// its tests. The live wire format starts at "Tier 3: Encrypt-then-CMAC" below.
//
// Format, both directions (GCM era):
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
//   input = "GB2" || netKeyId_BE32 || txRound_BE32 || txSlot_BE32
//           || pendingMask_BE32 || pendingMaskValid_u8 || burstIndex_BE32
//                                                          (24 bytes)
//
// burstIndex (mac-separation-implementation-plan.md's review-2026-09-15
// finding 2) is an OUTER LoraHeader field, not part of the GridBeacon message
// body — so it sat outside this MAC entirely until now, even though
// handleGridBeacon reads it to back out the copy's offset before computing
// t0_measured. A beacon is always sent as burstIndex 0 (one copy, not a
// burst), so an attacker able to alter just the plaintext outer header of an
// otherwise-genuine beacon frame could nudge t0_measured by any multiple of
// drift::kCopySpacingUs while the inner MAC still verified. Domain tag
// bumped to "GB2" (not "GB1") so an old-firmware verifier cannot be fooled
// into checking a shorter input against a longer one by coincidence.
// ---------------------------------------------------------------------------

static constexpr size_t kNetKeyBytes     = 16;  // AES-128, like the session key
static constexpr size_t kBeaconMacBytes  = 8;   // truncated, same budget as the
                                                // AEAD tag already on this link
static constexpr size_t kBeaconMacInputBytes = 24;

inline void buildBeaconMacInput(uint32_t net_key_id, uint32_t tx_round,
                                uint32_t tx_slot, uint32_t pending_mask,
                                bool pending_mask_valid, uint32_t burst_index,
                                uint8_t out[kBeaconMacInputBytes])
{
    // The domain tag. Three bytes rather than four so the whole input stays a
    // round number; it is a separator, not a length.
    out[0] = 'G';
    out[1] = 'B';
    out[2] = '2';
    u32be(net_key_id,  out + 3);
    u32be(tx_round,    out + 7);
    u32be(tx_slot,     out + 11);
    u32be(pending_mask, out + 15);
    // The validity flag is covered too. It is the field that decides whether
    // the mask means anything at all, so a MAC that omitted it would let an
    // attacker flip "listen" to "a real all-clear" without touching a signed
    // byte.
    out[19] = pending_mask_valid ? 1u : 0u;
    u32be(burst_index, out + 20);
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

// ---------------------------------------------------------------------------
// Tier 3: Encrypt-then-CMAC (docs/mac-separation-implementation-plan.md,
// section 2(b)'s "READ THIS FIRST" synthesis). Replaces AES-GCM above.
//
// As with the GCM derivations and the beacon MAC input above, this file only
// builds the fixed-layout BYTE BUFFERS that get fed to PSA's CMAC/CTR
// primitives — the PSA calls themselves (psa_mac_compute/verify,
// psa_cipher_encrypt/decrypt via the multi-part API) stay in comm_utils.c /
// CmdDispatcher.cpp / lora_client.cpp, same division of responsibility this
// file has always kept.
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

static constexpr size_t kSessionCmacTagBytes = 8;   // truncated, same on-air budget as the old GCM tag

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

// ---- Session establishment MICs (section 2(b), steps 1 and 7) ----
//
//   LOGIN:    CMAC(K_auth, "LG1" || dest || subnet || sender || msgid ||
//                           hub_nonce || request_register || hub_rebooted)[0:8]
//   REGISTER: CMAC(K_auth, "RG1" || mac_addr || needs_config || header)[0:8]
//
// Both verified BEFORE any state change on the receiving side (rate limit,
// resetCounters(), handle_register_'s session teardown) — that ordering
// lives in the caller, not here.

// "LG1"(3) + dest(4) + subnet(4) + sender(4) + msgid(4) + hub_nonce(4) +
// request_register(1) + hub_rebooted(1) = 25 bytes.
static constexpr size_t kLoginMicInputBytes = 3 + 4 + 4 + 4 + 4 + 4 + 1 + 1;

inline void buildLoginMicInput(uint32_t dest, uint32_t subnet, uint32_t sender,
                               uint32_t msgid, uint32_t hub_nonce,
                               bool request_register, bool hub_rebooted,
                               uint8_t out[kLoginMicInputBytes])
{
    out[0] = 'L'; out[1] = 'G'; out[2] = '1';
    u32be(dest,   out + 3);
    u32be(subnet, out + 7);
    u32be(sender, out + 11);
    u32be(msgid,  out + 15);
    u32be(hub_nonce, out + 19);
    out[23] = request_register ? 1u : 0u;
    out[24] = hub_rebooted     ? 1u : 0u;
}

// REGISTER's "header" component is the 4 scalar fields of LoraHeader that
// exist before any session is established (dest/subnet/sender/msgid) — the
// same shape LOGIN's MIC covers, so one helper serves both inputs' header
// portion. mac_addr is 8 bytes (uint64) on the wire; needs_config is 1 byte.
// "RG1"(3) + mac_addr(8) + needs_config(1) + dest(4) + subnet(4) + sender(4)
// + msgid(4) = 28 bytes.
static constexpr size_t kRegisterMicInputBytes = 3 + 8 + 1 + 4 + 4 + 4 + 4;

inline void buildRegisterMicInput(uint64_t mac_addr, bool needs_config,
                                  uint32_t dest, uint32_t subnet,
                                  uint32_t sender, uint32_t msgid,
                                  uint8_t out[kRegisterMicInputBytes])
{
    out[0] = 'R'; out[1] = 'G'; out[2] = '1';
    for (int i = 0; i < 8; ++i)
        out[3 + i] = static_cast<uint8_t>((mac_addr >> (56 - 8 * i)) & 0xFF);
    out[11] = needs_config ? 1u : 0u;
    u32be(dest,   out + 12);
    u32be(subnet, out + 16);
    u32be(sender, out + 20);
    u32be(msgid,  out + 24);
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

// CMAC input: a 45-byte fixed prefix over DECODED header fields, then the
// ciphertext. Computed over decoded values rather than raw wire bytes, so
// protobuf-c's field presence and non-canonical varint encoding can't affect
// it — the receiver's re-decoded values are always what gets MACed, on both
// sides.
static constexpr size_t kEtmPrefixBytes = 4 + 1 + 4 + 4 + 4 + 4 + 4 + 4 + 4 + 1 + 1 + 4 + 4 + 2;  // 45

struct EtmHeaderFields
{
    bool     downlink;
    uint32_t session_id;
    uint32_t dest_address;
    uint32_t dest_subnet;
    uint32_t sender_address;
    uint32_t msgid;
    uint32_t burst_index;
    uint32_t burst_count;
    bool     on_mark;
    bool     fire_stamped;
    uint32_t fire_round;
    uint32_t fire_offset_us;
};

inline void buildEtmCmacPrefix(const EtmHeaderFields &f, uint16_t ct_len,
                               uint8_t out[kEtmPrefixBytes])
{
    out[0] = 'E'; out[1] = 'T'; out[2] = 'M'; out[3] = '1';
    out[4] = f.downlink ? 1u : 0u;
    u32be(f.session_id,     out + 5);
    u32be(f.dest_address,   out + 9);
    u32be(f.dest_subnet,    out + 13);
    u32be(f.sender_address, out + 17);
    u32be(f.msgid,          out + 21);
    u32be(f.burst_index,    out + 25);
    u32be(f.burst_count,    out + 29);
    out[33] = f.on_mark ? 1u : 0u;
    out[34] = f.fire_stamped ? 1u : 0u;
    u32be(f.fire_round,      out + 35);
    u32be(f.fire_offset_us,  out + 39);
    out[43] = static_cast<uint8_t>((ct_len >> 8) & 0xFF);
    out[44] = static_cast<uint8_t>(ct_len & 0xFF);
}

}  // namespace framecrypto

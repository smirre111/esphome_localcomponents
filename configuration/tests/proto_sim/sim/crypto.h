#pragma once

#include "FrameCrypto.h"
#include "MacWire.h"

#include <cstdint>
#include <optional>
#include <vector>

// AES-GCM-128 helpers that match production CmdDispatcher.cpp / lora_client.cpp.
//
// Key:        SHA-256("LoRaHome")[0:16]
// Nonce/IV:   base_nonce_BE[4] ‖ frame_counter_BE[8]   (12 bytes)
// AAD:        destaddress‖destsubnet‖senderaddress‖msgid‖encrypted, each BE u32
//             (20 bytes)
// Tag:        16 bytes
//
// Host-side mbedtls is used here; the on-device firmware uses PSA Crypto
// against the SAME underlying mbedtls library, so the wire format is
// byte-identical.

namespace proto_sim {

// 16-byte derived key (kept across calls). idempotent.
const uint8_t* aes_gcm_key();

// Direction bit OR'd into the GCM nonce counter on DOWNLINK (hub -> node), so
// the two directions can never reuse an IV under the shared per-peer base
// nonce. Must match kDownlinkNonceFlag in BOTH lora_client.cpp and
// CmdDispatcher.cpp. Uplink (node -> hub) uses the bare msgid.
constexpr uint64_t kDownlinkNonceFlag = framecrypto::kDownlinkFlag;

// 12-byte IV = base_nonce[4 BE] || frame_counter[8 BE].
// For a downlink frame the caller must pass `msgid | kDownlinkNonceFlag`;
// prefer derive_gcm_iv_downlink() / derive_gcm_iv_uplink() below, which make
// the direction explicit at the call site.
void derive_gcm_iv(uint32_t base_nonce, uint64_t frame_counter, uint8_t iv_out[12]);

inline void derive_gcm_iv_uplink(uint32_t base_nonce, uint32_t msgid, uint8_t iv_out[12]) {
    derive_gcm_iv(base_nonce, static_cast<uint64_t>(msgid), iv_out);
}
inline void derive_gcm_iv_downlink(uint32_t base_nonce, uint32_t msgid, uint8_t iv_out[12]) {
    derive_gcm_iv(base_nonce, static_cast<uint64_t>(msgid) | kDownlinkNonceFlag, iv_out);
}

// 16-byte AAD = BE concat of (destAddress, destSubnet, senderAddress, msgId).
// The 5th field (the old `encrypted` header flag) was REMOVED from the proto —
// encryption is inferred from the oneof case — so the AAD shrank from 20 to 16
// bytes. Mirrors build_header_aad() in CmdDispatcher.cpp:61.
constexpr size_t kHeaderAadLen = 16;
void build_header_aad(uint32_t dest_addr, uint32_t dest_subnet,
                      uint32_t sender_addr, uint32_t msg_id,
                      uint8_t aad_out[kHeaderAadLen]);

// On-air AES-GCM tag length. Production TRUNCATES the 16-byte tag to 8 bytes
// to keep the envelope slim (lora_client.cpp: kAesGcmTagBytes = 8, and the
// node's encrypt/decrypt calls pass the same). A mismatch here fails every
// AEAD verification, so this must track production exactly.
constexpr size_t kOnAirTagBytes = 8;

// Returns ciphertext (plain_len bytes) + writes the truncated tag.
struct GcmResult {
    std::vector<uint8_t> ciphertext;
    std::vector<uint8_t> tag;       // kOnAirTagBytes
};
GcmResult aes_gcm_encrypt(const uint8_t iv[12], const uint8_t* aad, size_t aad_len,
                          const uint8_t* plain, size_t plain_len);

// Returns plaintext on success, std::nullopt on AEAD authentication failure.
std::optional<std::vector<uint8_t>>
aes_gcm_decrypt(const uint8_t iv[12], const uint8_t* aad, size_t aad_len,
                const uint8_t* cipher, size_t cipher_len,
                const uint8_t* tag,    size_t tag_len);

// ---------------------------------------------------------------------------
// Tier 3 (docs/mac-separation-implementation-plan.md section 2(b)): LOGIN
// and REGISTER are now MIC-authenticated under K_auth, derived from the same
// K_root this file's AES-GCM key comes from. Model-based test scaffolding
// (wire_codec.cpp's sim::ClientRegister/LoginMsg serializers) computes these
// so a simulated node/hub frame still verifies against the REAL production
// code under test.
// ---------------------------------------------------------------------------

void compute_login_mic(uint32_t dest, uint32_t subnet, uint32_t sender,
                       uint32_t msgid, uint32_t hub_nonce, bool request_register,
                       bool hub_rebooted,
                       uint8_t out[framecrypto::kSessionCmacTagBytes]);

void compute_register_mic(uint64_t mac_addr, bool needs_config,
                          uint32_t dest, uint32_t subnet, uint32_t sender,
                          uint32_t msgid, uint8_t out[framecrypto::kSessionCmacTagBytes]);

// ---------------------------------------------------------------------------
// Tier 3: payload confidentiality/authenticity, replacing the AES-GCM
// helpers above. Derives K_enc/K_mac the same way production does — via
// AES-CMAC(K_root, framecrypto::buildSessionKeyKdfInput(...)) — from the
// four values that determine them (session_id/node_nonce/hub_addr/
// node_addr), so test scaffolding that knows those four values can build
// or open a frame the real hub/node under test will actually accept.
// ---------------------------------------------------------------------------

struct EtmResult {
    std::vector<uint8_t> ciphertext;
    uint8_t tag[framecrypto::kSessionCmacTagBytes];
};

// Encrypts once (CTR, blockIdx 0) and tags over the given header fields —
// mirrors Mac2::sealNew conceptually: one ciphertext, one tag, both derived
// from the SAME (session_id, node_nonce, hub_addr, node_addr) the real
// deriveSessionKeys_() on each side would use.
EtmResult encrypt_then_cmac_seal(uint32_t session_id, uint32_t node_nonce,
                                 uint8_t hub_addr, uint8_t node_addr, bool downlink,
                                 const framecrypto::EtmHeaderFields &fields,
                                 const uint8_t *plain, size_t plain_len);

// Verifies the tag (constant-time, via PSA) THEN decrypts. Returns
// std::nullopt on tag mismatch, mirroring aes_gcm_decrypt's shape.
std::optional<std::vector<uint8_t>>
encrypt_then_cmac_open(uint32_t session_id, uint32_t node_nonce,
                       uint8_t hub_addr, uint8_t node_addr, bool downlink,
                       const framecrypto::EtmHeaderFields &fields,
                       const uint8_t *cipher, size_t cipher_len,
                       const uint8_t *tag, size_t tag_len);

// Recomputes ONLY the tag over the given (already-sealed) ciphertext and
// header fields — for tests that build a burst copy the way the real
// tracker's per-copy retagging does (LORAListener::sealBurstCopyTag()).
void encrypt_then_cmac_retag(uint32_t session_id, uint32_t node_nonce,
                             uint8_t hub_addr, uint8_t node_addr, bool downlink,
                             const framecrypto::EtmHeaderFields &fields,
                             const uint8_t *ciphertext, size_t ciphertext_len,
                             uint8_t out_tag[framecrypto::kSessionCmacTagBytes]);

// ---------------------------------------------------------------------------
// MAC wire v1 (S8, docs/mac-wire-format.md section 4): the sim's own
// implementation of the raw-frame tags, on mbedtls (the hub uses PSA). The tag
// is CMAC-8 over "BW1" || dir || id u32 || frame[0 .. L-8). The header itself
// is encoded with the shared MacWire.h (pinned by the node's vectors).
// ---------------------------------------------------------------------------

// Raw-key primitives (the shared vectors use fixed keys).
void v1_tag(const uint8_t key[16], bool downlink, uint32_t id, const uint8_t *bytes, size_t n,
            uint8_t out[8]);
void v1_ctr(const uint8_t key[16], uint32_t session_id, uint8_t node_addr, bool downlink,
            uint32_t fcnt, const uint8_t *in, size_t n, uint8_t *out);
// The K_enc / K_mac a hub and node derive for (session, node nonce, hub, node).
void v1_session_keys(uint32_t session_id, uint32_t node_nonce, uint8_t hub_addr, uint8_t node_addr,
                     uint8_t k_enc[16], uint8_t k_mac[16]);

// Whole frames. `h.flags` must already carry the AUTH the helper implements.
std::vector<uint8_t> v1_session_frame(uint32_t session_id, uint32_t node_nonce, uint8_t hub_addr,
                                      uint8_t node_addr, bool downlink, const macwire::WireHdr &h,
                                      const std::vector<uint8_t> &body);
std::vector<uint8_t> v1_kauth_frame(bool downlink, const macwire::WireHdr &h,
                                    const std::vector<uint8_t> &body);
std::vector<uint8_t> v1_plain_frame(const macwire::WireHdr &h, const std::vector<uint8_t> &body);
// Tag check THEN CTR. nullopt on a bad shape or tag. *hdr_out (optional) is
// the decoded header.
std::optional<std::vector<uint8_t>> v1_open_session_frame(uint32_t session_id, uint32_t node_nonce,
                                                          uint8_t hub_addr, uint8_t node_addr,
                                                          bool downlink,
                                                          const std::vector<uint8_t> &frame,
                                                          macwire::WireHdr *hdr_out = nullptr);
bool v1_verify_kauth_frame(bool downlink, const std::vector<uint8_t> &frame);

} // namespace proto_sim

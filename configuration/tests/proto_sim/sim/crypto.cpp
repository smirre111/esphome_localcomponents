#include "sim/crypto.h"

#include "FrameCrypto.h"

#include <mbedtls/aes.h>
#include <mbedtls/cipher.h>
#include <mbedtls/cmac.h>
#include <mbedtls/gcm.h>
#include <mbedtls/sha256.h>

#include <cstring>
#include <mutex>
#include <optional>
#include <vector>

namespace proto_sim {

namespace {

// Tier-1 (docs/mac-separation-implementation-plan.md): intentionally NOT
// the real fleet key. This string only has to agree with itself here and in
// crypto_test.cpp's KeyDerivationIsStable pin — it exists to catch
// accidental drift in the DERIVATION MECHANISM (SHA-256, truncated to 16
// bytes), not to track the real deployed secret's value. It is fine for
// this to be public, since it never was the real one.
constexpr const char* kKeyString = "ProtoSimTestKeyNotReal";

void u32_be(uint32_t v, uint8_t* out) {
    out[0] = (v >> 24) & 0xFF;
    out[1] = (v >> 16) & 0xFF;
    out[2] = (v >>  8) & 0xFF;
    out[3] = (v >>  0) & 0xFF;
}

void u64_be(uint64_t v, uint8_t* out) {
    for (int i = 7; i >= 0; --i) {
        out[i] = static_cast<uint8_t>(v & 0xFF);
        v >>= 8;
    }
}

} // namespace

const uint8_t* aes_gcm_key() {
    static uint8_t key[16];
    static std::once_flag once;
    std::call_once(once, [] {
        uint8_t hash[32];
        // mbedtls 2.x: third arg "is224" — 0 means SHA-256.
        mbedtls_sha256(reinterpret_cast<const unsigned char*>(kKeyString),
                       std::strlen(kKeyString), hash, 0);
        std::memcpy(key, hash, 16);
    });
    return key;
}

namespace {

// One-shot AES-CMAC (mbedtls 2.x's mbedtls_cipher_cmac always produces the
// full 16-byte block; callers truncate as needed — same convention as PSA's
// TRUNCATED_MAC, which also takes the leading bytes of the full MAC).
void cmac_full(const uint8_t key[16], const uint8_t* input, size_t input_len,
               uint8_t out16[16]) {
    const mbedtls_cipher_info_t* info =
        mbedtls_cipher_info_from_values(MBEDTLS_CIPHER_ID_AES, 128, MBEDTLS_MODE_ECB);
    mbedtls_cipher_cmac(info, key, 128, input, input_len, out16);
}

// K_auth = AES-CMAC(K_root, framecrypto::buildKAuthKdfInput()) — mirrors
// CmdDispatcher::init_k_auth_() / lora_client.cpp's s_init_k_auth_key().
const uint8_t* k_auth_key() {
    static uint8_t key[16];
    static std::once_flag once;
    std::call_once(once, [] {
        uint8_t kdf_input[framecrypto::kKdfInputBytes];
        framecrypto::buildKAuthKdfInput(kdf_input);
        cmac_full(aes_gcm_key(), kdf_input, sizeof(kdf_input), key);
    });
    return key;
}

}  // namespace

namespace {

// K_enc/K_mac derivation, mirroring CmdDispatcher::deriveSessionKeys_() /
// LORAListener::deriveSessionKeys_(): CMAC(K_root, buildSessionKeyKdfInput(
// half, ...)), half 0x01 for K_enc, 0x02 for K_mac.
void derive_session_key(uint8_t half, uint32_t session_id, uint32_t node_nonce,
                        uint8_t hub_addr, uint8_t node_addr, uint8_t out16[16]) {
    uint8_t input[framecrypto::kKdfInputBytes];
    framecrypto::buildSessionKeyKdfInput(half, session_id, node_nonce, hub_addr, node_addr, input);
    cmac_full(aes_gcm_key(), input, sizeof(input), out16);
}

// mbedtls_aes_crypt_ctr starts from nc_off=0 / a fresh stream_block every
// call here — correct for a single one-shot buffer, same as production's
// psa_cipher multi-part API used exactly once per frame.
void ctr_crypt(const uint8_t key[16], const uint8_t iv[16],
               const uint8_t *in, size_t len, uint8_t *out) {
    mbedtls_aes_context ctx;
    mbedtls_aes_init(&ctx);
    mbedtls_aes_setkey_enc(&ctx, key, 128);
    size_t nc_off = 0;
    uint8_t stream_block[16] = {0};
    uint8_t nonce_counter[16];
    std::memcpy(nonce_counter, iv, 16);
    mbedtls_aes_crypt_ctr(&ctx, len, &nc_off, nonce_counter, stream_block, in, out);
    mbedtls_aes_free(&ctx);
}

}  // namespace

// The sim MODELS' own toy AEAD (hub_model / node_model talk to each other, never
// to production). Production is MAC wire v1 (the v1_* helpers below); this GCM
// layout is the models' private convention and no longer mirrors a production
// header.
void derive_gcm_iv(uint32_t base_nonce, uint64_t frame_counter, uint8_t iv_out[12]) {
    u32_be(base_nonce, iv_out);
    u64_be(frame_counter, iv_out + 4);
}

void build_header_aad(uint32_t dest_addr, uint32_t dest_subnet,
                      uint32_t sender_addr, uint32_t msg_id,
                      uint8_t aad_out[kHeaderAadLen]) {
    u32_be(dest_addr, aad_out);
    u32_be(dest_subnet, aad_out + 4);
    u32_be(sender_addr, aad_out + 8);
    u32_be(msg_id, aad_out + 12);
}

GcmResult aes_gcm_encrypt(const uint8_t iv[12], const uint8_t* aad, size_t aad_len,
                          const uint8_t* plain, size_t plain_len) {
    GcmResult out;
    out.ciphertext.resize(plain_len);
    // Production truncates the tag to kOnAirTagBytes for the slim on-air
    // envelope (lora_client.cpp kAesGcmTagBytes / the node's matching
    // constant). mbedtls supports generating a truncated tag directly, so
    // produce exactly what goes on the wire rather than truncating later.
    out.tag.resize(kOnAirTagBytes);

    mbedtls_gcm_context ctx;
    mbedtls_gcm_init(&ctx);
    mbedtls_gcm_setkey(&ctx, MBEDTLS_CIPHER_ID_AES, aes_gcm_key(), 128);
    mbedtls_gcm_crypt_and_tag(&ctx, MBEDTLS_GCM_ENCRYPT,
                              plain_len, iv, 12, aad, aad_len,
                              plain, out.ciphertext.data(),
                              out.tag.size(), out.tag.data());
    mbedtls_gcm_free(&ctx);
    return out;
}

std::optional<std::vector<uint8_t>>
aes_gcm_decrypt(const uint8_t iv[12], const uint8_t* aad, size_t aad_len,
                const uint8_t* cipher, size_t cipher_len,
                const uint8_t* tag,    size_t tag_len) {
    std::vector<uint8_t> plain(cipher_len);

    mbedtls_gcm_context ctx;
    mbedtls_gcm_init(&ctx);
    mbedtls_gcm_setkey(&ctx, MBEDTLS_CIPHER_ID_AES, aes_gcm_key(), 128);
    int rc = mbedtls_gcm_auth_decrypt(&ctx, cipher_len, iv, 12, aad, aad_len,
                                      tag, tag_len, cipher, plain.data());
    mbedtls_gcm_free(&ctx);
    if (rc != 0) return std::nullopt;
    return plain;
}

// ---------------------------------------------------------------------------
// MAC wire v1 (S8)
// ---------------------------------------------------------------------------

void v1_tag(const uint8_t key[16], bool downlink, uint32_t id, const uint8_t *bytes, size_t n,
            uint8_t out[8]) {
    // Written out byte by byte (not via MacWire.h's prefix helper): this is the
    // second implementation the hub's is checked against.
    std::vector<uint8_t> in(8 + n);
    in[0] = 'B'; in[1] = 'W'; in[2] = '1';
    in[3] = downlink ? 1 : 0;
    u32_be(id, in.data() + 4);
    if (n) std::memcpy(in.data() + 8, bytes, n);
    uint8_t full[16];
    cmac_full(key, in.data(), in.size(), full);
    std::memcpy(out, full, 8);
}

void v1_ctr(const uint8_t key[16], uint32_t session_id, uint8_t node_addr, bool downlink,
            uint32_t fcnt, const uint8_t *in, size_t n, uint8_t *out) {
    uint8_t block[16];
    u32_be(session_id, block);
    u32_be(node_addr, block + 4);
    u32_be((downlink ? 0x80000000u : 0u) | (fcnt & 0x7FFFFFFFu), block + 8);
    u32_be(0, block + 12);
    ctr_crypt(key, block, in, n, out);
}

void v1_session_keys(uint32_t session_id, uint32_t node_nonce, uint8_t hub_addr, uint8_t node_addr,
                     uint8_t k_enc[16], uint8_t k_mac[16]) {
    derive_session_key(0x01, session_id, node_nonce, hub_addr, node_addr, k_enc);
    derive_session_key(0x02, session_id, node_nonce, hub_addr, node_addr, k_mac);
}

namespace {
std::vector<uint8_t> v1_layout(const macwire::WireHdr &h, const std::vector<uint8_t> &body) {
    const size_t hlen = macwire::headerLen(h);
    std::vector<uint8_t> f(hlen + body.size() + macwire::tagLen(h.flags), 0);
    macwire::encodeHeader(h, f.data(), f.size());
    if (!body.empty()) std::memcpy(f.data() + hlen, body.data(), body.size());
    return f;
}
}  // namespace

std::vector<uint8_t> v1_session_frame(uint32_t session_id, uint32_t node_nonce, uint8_t hub_addr,
                                      uint8_t node_addr, bool downlink, const macwire::WireHdr &h,
                                      const std::vector<uint8_t> &body) {
    uint8_t k_enc[16], k_mac[16];
    v1_session_keys(session_id, node_nonce, hub_addr, node_addr, k_enc, k_mac);
    std::vector<uint8_t> f = v1_layout(h, body);
    const size_t hlen = macwire::headerLen(h);
    v1_ctr(k_enc, session_id, node_addr, downlink, h.fcnt, body.data(), body.size(), f.data() + hlen);
    v1_tag(k_mac, downlink, session_id, f.data(), f.size() - 8, f.data() + f.size() - 8);
    return f;
}

std::vector<uint8_t> v1_kauth_frame(bool downlink, const macwire::WireHdr &h,
                                    const std::vector<uint8_t> &body) {
    std::vector<uint8_t> f = v1_layout(h, body);
    v1_tag(k_auth_key(), downlink, 0, f.data(), f.size() - 8, f.data() + f.size() - 8);
    return f;
}

std::vector<uint8_t> v1_plain_frame(const macwire::WireHdr &h, const std::vector<uint8_t> &body) {
    return v1_layout(h, body);
}

std::optional<std::vector<uint8_t>> v1_open_session_frame(uint32_t session_id, uint32_t node_nonce,
                                                          uint8_t hub_addr, uint8_t node_addr,
                                                          bool downlink,
                                                          const std::vector<uint8_t> &frame,
                                                          macwire::WireHdr *hdr_out) {
    macwire::WireHdr h;
    size_t off = 0, blen = 0;
    if (!macwire::decode(frame.data(), frame.size(), &h, &off, &blen) ||
        h.auth() != macwire::Auth::Session)
        return std::nullopt;
    uint8_t k_enc[16], k_mac[16];
    v1_session_keys(session_id, node_nonce, hub_addr, node_addr, k_enc, k_mac);
    uint8_t tag[8];
    v1_tag(k_mac, downlink, session_id, frame.data(), frame.size() - 8, tag);
    if (std::memcmp(tag, frame.data() + frame.size() - 8, 8) != 0)
        return std::nullopt;
    std::vector<uint8_t> plain(blen);
    v1_ctr(k_enc, session_id, node_addr, downlink, h.fcnt, frame.data() + off, blen, plain.data());
    if (hdr_out) *hdr_out = h;
    return plain;
}

bool v1_verify_kauth_frame(bool downlink, const std::vector<uint8_t> &frame) {
    macwire::WireHdr h;
    size_t off = 0, blen = 0;
    if (!macwire::decode(frame.data(), frame.size(), &h, &off, &blen) ||
        h.auth() != macwire::Auth::KAuth)
        return false;
    uint8_t tag[8];
    v1_tag(k_auth_key(), downlink, 0, frame.data(), frame.size() - 8, tag);
    return std::memcmp(tag, frame.data() + frame.size() - 8, 8) == 0;
}

} // namespace proto_sim

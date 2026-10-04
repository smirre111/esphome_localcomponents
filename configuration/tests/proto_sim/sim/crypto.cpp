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

void compute_login_mic(uint32_t dest, uint32_t subnet, uint32_t sender,
                       uint32_t msgid, uint32_t hub_nonce, bool request_register,
                       bool hub_rebooted,
                       uint8_t out[framecrypto::kSessionCmacTagBytes]) {
    uint8_t input[framecrypto::kLoginMicInputBytes];
    framecrypto::buildLoginMicInput(dest, subnet, sender, msgid, hub_nonce,
                                    request_register, hub_rebooted, input);
    uint8_t full[16];
    cmac_full(k_auth_key(), input, sizeof(input), full);
    std::memcpy(out, full, framecrypto::kSessionCmacTagBytes);
}

void compute_register_mic(uint64_t mac_addr, bool needs_config,
                          uint32_t dest, uint32_t subnet, uint32_t sender,
                          uint32_t msgid, uint8_t out[framecrypto::kSessionCmacTagBytes]) {
    uint8_t input[framecrypto::kRegisterMicInputBytes];
    framecrypto::buildRegisterMicInput(mac_addr, needs_config, dest, subnet,
                                       sender, msgid, input);
    uint8_t full[16];
    cmac_full(k_auth_key(), input, sizeof(input), full);
    std::memcpy(out, full, framecrypto::kSessionCmacTagBytes);
}

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

EtmResult encrypt_then_cmac_seal(uint32_t session_id, uint32_t node_nonce,
                                 uint8_t hub_addr, uint8_t node_addr, bool downlink,
                                 const framecrypto::EtmHeaderFields &fields,
                                 const uint8_t *plain, size_t plain_len) {
    EtmResult result;
    result.ciphertext.resize(plain_len);

    uint8_t k_enc[16];
    derive_session_key(0x01, session_id, node_nonce, hub_addr, node_addr, k_enc);
    uint8_t ctr_block[framecrypto::kCtrBlockBytes];
    framecrypto::buildCtrInitialBlock(session_id, node_addr, downlink, fields.msgid,
                                      /*block_idx=*/0, ctr_block);
    ctr_crypt(k_enc, ctr_block, plain, plain_len, result.ciphertext.data());

    uint8_t k_mac[16];
    derive_session_key(0x02, session_id, node_nonce, hub_addr, node_addr, k_mac);
    uint8_t prefix[framecrypto::kEtmPrefixBytes];
    framecrypto::buildEtmCmacPrefix(fields, static_cast<uint16_t>(plain_len), prefix);
    std::vector<uint8_t> mac_input(sizeof(prefix) + plain_len);
    std::memcpy(mac_input.data(), prefix, sizeof(prefix));
    std::memcpy(mac_input.data() + sizeof(prefix), result.ciphertext.data(), plain_len);
    uint8_t full_tag[16];
    cmac_full(k_mac, mac_input.data(), mac_input.size(), full_tag);
    std::memcpy(result.tag, full_tag, framecrypto::kSessionCmacTagBytes);

    return result;
}

std::optional<std::vector<uint8_t>>
encrypt_then_cmac_open(uint32_t session_id, uint32_t node_nonce,
                       uint8_t hub_addr, uint8_t node_addr, bool downlink,
                       const framecrypto::EtmHeaderFields &fields,
                       const uint8_t *cipher, size_t cipher_len,
                       const uint8_t *tag, size_t tag_len) {
    uint8_t k_mac[16];
    derive_session_key(0x02, session_id, node_nonce, hub_addr, node_addr, k_mac);
    uint8_t prefix[framecrypto::kEtmPrefixBytes];
    framecrypto::buildEtmCmacPrefix(fields, static_cast<uint16_t>(cipher_len), prefix);
    std::vector<uint8_t> mac_input(sizeof(prefix) + cipher_len);
    std::memcpy(mac_input.data(), prefix, sizeof(prefix));
    std::memcpy(mac_input.data() + sizeof(prefix), cipher, cipher_len);
    uint8_t full_tag[16];
    cmac_full(k_mac, mac_input.data(), mac_input.size(), full_tag);
    if (tag_len > sizeof(full_tag) || std::memcmp(full_tag, tag, tag_len) != 0)
        return std::nullopt;

    uint8_t k_enc[16];
    derive_session_key(0x01, session_id, node_nonce, hub_addr, node_addr, k_enc);
    uint8_t ctr_block[framecrypto::kCtrBlockBytes];
    framecrypto::buildCtrInitialBlock(session_id, node_addr, downlink, fields.msgid,
                                      /*block_idx=*/0, ctr_block);
    std::vector<uint8_t> plain(cipher_len);
    ctr_crypt(k_enc, ctr_block, cipher, cipher_len, plain.data());
    return plain;
}

void encrypt_then_cmac_retag(uint32_t session_id, uint32_t node_nonce,
                             uint8_t hub_addr, uint8_t node_addr, bool downlink,
                             const framecrypto::EtmHeaderFields &fields,
                             const uint8_t *ciphertext, size_t ciphertext_len,
                             uint8_t out_tag[framecrypto::kSessionCmacTagBytes]) {
    uint8_t k_mac[16];
    derive_session_key(0x02, session_id, node_nonce, hub_addr, node_addr, k_mac);
    uint8_t prefix[framecrypto::kEtmPrefixBytes];
    framecrypto::buildEtmCmacPrefix(fields, static_cast<uint16_t>(ciphertext_len), prefix);
    std::vector<uint8_t> mac_input(sizeof(prefix) + ciphertext_len);
    std::memcpy(mac_input.data(), prefix, sizeof(prefix));
    std::memcpy(mac_input.data() + sizeof(prefix), ciphertext, ciphertext_len);
    uint8_t full_tag[16];
    cmac_full(k_mac, mac_input.data(), mac_input.size(), full_tag);
    std::memcpy(out_tag, full_tag, framecrypto::kSessionCmacTagBytes);
}

// These delegate to the production header rather than re-deriving the layout.
//
// The sim is what the tests use to BUILD frames, so a sim that agreed with the
// hub while both differed from the node would leave the suite green and fail
// only on the air. An independent "reference implementation" here is not extra
// assurance — it is a second thing that can be wrong in the same way.
void derive_gcm_iv(uint32_t base_nonce, uint64_t frame_counter, uint8_t iv_out[12]) {
    framecrypto::deriveIv(base_nonce, frame_counter, iv_out);
}

void build_header_aad(uint32_t dest_addr, uint32_t dest_subnet,
                      uint32_t sender_addr, uint32_t msg_id,
                      uint8_t aad_out[kHeaderAadLen]) {
    framecrypto::buildAad(dest_addr, dest_subnet, sender_addr, msg_id, aad_out);
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

} // namespace proto_sim

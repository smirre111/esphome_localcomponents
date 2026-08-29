#include "sim/crypto.h"

#include "FrameCrypto.h"

#include <mbedtls/gcm.h>
#include <mbedtls/sha256.h>

#include <cstring>
#include <mutex>
#include <optional>
#include <vector>

namespace proto_sim {

namespace {

constexpr const char* kKeyString = "LoRaKey1";

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

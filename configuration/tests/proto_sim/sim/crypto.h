#pragma once

#include <cstdint>
#include <optional>
#include <vector>

// AES-GCM-128 helpers that match production CmdDispatcher.cpp / lora_client.cpp.
//
// Key:        SHA-256("LoRaKey1")[0:16]
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

// 12-byte IV = base_nonce[4 BE] || frame_counter[8 BE].
void derive_gcm_iv(uint32_t base_nonce, uint64_t frame_counter, uint8_t iv_out[12]);

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

} // namespace proto_sim

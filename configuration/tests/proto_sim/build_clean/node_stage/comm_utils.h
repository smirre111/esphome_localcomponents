#pragma once

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include "psa/crypto.h"

bool derive_aes_gcm_key(uint8_t key_out[16]);
void u32_to_be(uint32_t value, uint8_t *bytes);
void u64_to_be(uint64_t value, uint8_t *bytes);
uint64_t u64_from_be(const uint8_t *bytes);

/* ---------------------------------------------------------------------------
 * Preferred API: import the key once, reuse for many encrypt/decrypt calls,
 * then destroy when done.  This avoids the per-call PSA slot churn of the
 * legacy helpers below.
 * --------------------------------------------------------------------------- */

/**
 * Import a 16-byte AES key into the PSA key store.
 * The returned key_id is valid for both ENCRYPT and DECRYPT usage with
 * PSA_ALG_GCM.  Call comm_utils_destroy_key() when the key is no longer
 * needed.
 */
bool comm_utils_import_key(const uint8_t *key_material, size_t key_bytes,
                            psa_key_id_t *out_id);

/** Destroy a PSA key previously imported with comm_utils_import_key(). */
void comm_utils_destroy_key(psa_key_id_t key_id);

/** AES-128-GCM encrypt using a pre-imported PSA key ID. */
bool encrypt_gcm_with_key_id(psa_key_id_t key_id,
                              const uint8_t *nonce,
                              const uint8_t *aad, size_t aad_len,
                              const uint8_t *plain, size_t plain_len,
                              uint8_t *cipher, uint8_t *tag, size_t tag_len);

/** AES-128-GCM decrypt using a pre-imported PSA key ID. */
bool decrypt_gcm_with_key_id(psa_key_id_t key_id,
                              const uint8_t *nonce,
                              const uint8_t *aad, size_t aad_len,
                              const uint8_t *cipher, size_t cipher_len,
                              const uint8_t *tag, size_t tag_len,
                              uint8_t *plain_out);

/* ---------------------------------------------------------------------------
 * Legacy API: imports and destroys the PSA key on every call.
 * Kept for backward compatibility with existing tests.
 * Prefer the _with_key_id variants in production firmware.
 * --------------------------------------------------------------------------- */
bool encrypt_gcm(const uint8_t *key, const uint8_t *nonce, const uint8_t *aad, size_t aad_len,
                 const uint8_t *plain, size_t plain_len, uint8_t *cipher, uint8_t *tag, size_t tag_len);

bool decrypt_gcm(const uint8_t *key, const uint8_t *nonce, const uint8_t *aad, size_t aad_len,
                 const uint8_t *cipher, size_t cipher_len, const uint8_t *tag, size_t tag_len, uint8_t *plain_out);

#ifdef __cplusplus
}
#endif

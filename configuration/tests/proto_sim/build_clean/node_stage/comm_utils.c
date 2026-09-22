#include "comm_utils.h"
#include <string.h>
#include <stdlib.h>
#include "psa/crypto.h"

void u32_to_be(uint32_t value, uint8_t *bytes)
{
  for (int i = 3; i >= 0; --i)
  {
    bytes[i] = (uint8_t)(value & 0xFF);
    value >>= 8;
  }
}

void u64_to_be(uint64_t value, uint8_t *bytes)
{
  for (int i = 7; i >= 0; --i)
  {
    bytes[i] = (uint8_t)(value & 0xFF);
    value >>= 8;
  }
}

uint64_t u64_from_be(const uint8_t *bytes)
{
  uint64_t value = 0;
  for (int i = 0; i < 8; ++i)
  {
    value = (value << 8) | bytes[i];
  }
  return value;
}

bool derive_aes_gcm_key(uint8_t key_out[16])
{
  const char *kLoRaAesGcmKey = "LoRaKey1";
  uint8_t hash[32];
  size_t hash_len = 0;
  psa_status_t status = psa_hash_compute(PSA_ALG_SHA_256,
                                         (const uint8_t *)kLoRaAesGcmKey,
                                         strlen(kLoRaAesGcmKey),
                                         hash, sizeof(hash), &hash_len);
  if (status != PSA_SUCCESS)
    return false;

  memcpy(key_out, hash, 16);
  return true;
}

/* ---------------------------------------------------------------------------
 * Preferred API — import once, reuse many times, destroy explicitly.
 * --------------------------------------------------------------------------- */

bool comm_utils_import_key(const uint8_t *key_material, size_t key_bytes,
                            psa_key_id_t *out_id)
{
  psa_key_attributes_t attrs = PSA_KEY_ATTRIBUTES_INIT;
  /* Allow both encrypt and decrypt so one imported key can be reused for
   * both directions (e.g. in the unit test round-trip). */
  psa_set_key_usage_flags(&attrs, PSA_KEY_USAGE_ENCRYPT | PSA_KEY_USAGE_DECRYPT);
  /* Permit truncated tags (production uses an 8-byte GCM tag) — AT_LEAST_THIS_
   * LENGTH lets one imported key serve any tag length the operation requests. */
  psa_set_key_algorithm(&attrs, PSA_ALG_AEAD_WITH_AT_LEAST_THIS_LENGTH_TAG(PSA_ALG_GCM, 4));
  psa_set_key_type(&attrs, PSA_KEY_TYPE_AES);
  psa_set_key_bits(&attrs, (psa_key_bits_t)(key_bytes * 8));

  *out_id = PSA_KEY_ID_NULL;
  return psa_import_key(&attrs, key_material, key_bytes, out_id) == PSA_SUCCESS;
}

void comm_utils_destroy_key(psa_key_id_t key_id)
{
  if (key_id != PSA_KEY_ID_NULL)
    psa_destroy_key(key_id);
}

bool encrypt_gcm_with_key_id(psa_key_id_t key_id,
                              const uint8_t *nonce,
                              const uint8_t *aad, size_t aad_len,
                              const uint8_t *plain, size_t plain_len,
                              uint8_t *cipher, uint8_t *tag, size_t tag_len)
{
  /* PSA AES-GCM: output buffer = ciphertext (plain_len bytes) || tag (tag_len bytes).
   * We use a temporary buffer and split the tag out afterwards to keep the
   * caller-facing API (separate cipher + tag pointers) unchanged. */
  size_t out_size = plain_len + tag_len;
  uint8_t *out_buf = (uint8_t *)malloc(out_size);
  if (!out_buf)
    return false;

  size_t out_len = 0;
  psa_status_t status = psa_aead_encrypt(key_id,
                                          PSA_ALG_AEAD_WITH_SHORTENED_TAG(PSA_ALG_GCM, tag_len),
                                          nonce, 12,
                                          aad, aad_len,
                                          plain, plain_len,
                                          out_buf, out_size, &out_len);

  if (status != PSA_SUCCESS || out_len != out_size) {
    free(out_buf);
    return false;
  }

  memcpy(cipher, out_buf, plain_len);
  memcpy(tag, out_buf + plain_len, tag_len);
  free(out_buf);
  return true;
}

bool decrypt_gcm_with_key_id(psa_key_id_t key_id,
                              const uint8_t *nonce,
                              const uint8_t *aad, size_t aad_len,
                              const uint8_t *cipher, size_t cipher_len,
                              const uint8_t *tag, size_t tag_len,
                              uint8_t *plain_out)
{
  /* PSA AES-GCM: input to psa_aead_decrypt() = ciphertext || tag (concatenated).
   * We build a temporary buffer from the separate cipher + tag pointers. */
  size_t ct_len = cipher_len + tag_len;
  uint8_t *ct_buf = (uint8_t *)malloc(ct_len);
  if (!ct_buf)
    return false;

  memcpy(ct_buf, cipher, cipher_len);
  memcpy(ct_buf + cipher_len, tag, tag_len);

  size_t plain_len = 0;
  psa_status_t status = psa_aead_decrypt(key_id,
                                          PSA_ALG_AEAD_WITH_SHORTENED_TAG(PSA_ALG_GCM, tag_len),
                                          nonce, 12,
                                          aad, aad_len,
                                          ct_buf, ct_len,
                                          plain_out, cipher_len, &plain_len);
  free(ct_buf);
  return status == PSA_SUCCESS;
}

/* ---------------------------------------------------------------------------
 * Legacy API — import + destroy on every call.
 * Kept for backward compatibility; prefer the _with_key_id variants in
 * production firmware to avoid per-call PSA slot churn.
 * --------------------------------------------------------------------------- */

bool encrypt_gcm(const uint8_t *key, const uint8_t *nonce, const uint8_t *aad, size_t aad_len,
                 const uint8_t *plain, size_t plain_len, uint8_t *cipher, uint8_t *tag, size_t tag_len)
{
  psa_key_id_t key_id;
  if (!comm_utils_import_key(key, 16, &key_id))
    return false;
  bool ok = encrypt_gcm_with_key_id(key_id, nonce, aad, aad_len, plain, plain_len, cipher, tag, tag_len);
  comm_utils_destroy_key(key_id);
  return ok;
}

bool decrypt_gcm(const uint8_t *key, const uint8_t *nonce, const uint8_t *aad, size_t aad_len,
                 const uint8_t *cipher, size_t cipher_len, const uint8_t *tag, size_t tag_len, uint8_t *plain_out)
{
  psa_key_id_t key_id;
  if (!comm_utils_import_key(key, 16, &key_id))
    return false;
  bool ok = decrypt_gcm_with_key_id(key_id, nonce, aad, aad_len, cipher, cipher_len, tag, tag_len, plain_out);
  comm_utils_destroy_key(key_id);
  return ok;
}

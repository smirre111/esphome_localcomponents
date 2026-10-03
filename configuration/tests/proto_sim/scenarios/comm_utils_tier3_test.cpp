// comm_utils.c's Tier 3 (docs/mac-separation-implementation-plan.md section
// 2(b)) primitives: AES-CMAC compute/verify and AES-CTR, exercised directly
// against the real production C file (via real_cmd_dispatcher, the same
// library real_cmd_dispatcher_test.cpp links) rather than through a mock.
//
// These are deliberately round-trip tests, unlike FrameCrypto's byte-layout
// pins — the byte LAYOUTS are pinned in frame_crypto_test.cpp; what these
// need to prove is that the PSA calls underneath comm_utils.c actually do
// what their names say (a wrong key policy or algorithm selection fails at
// the psa_import_key/psa_mac_compute call, not at comparison time).

#include <gtest/gtest.h>

#include <string.h>

#include "comm_utils.h"

namespace {

void ensure_psa_ready() { ASSERT_EQ(psa_crypto_init(), PSA_SUCCESS); }

}  // namespace

TEST(CommUtilsTier3, CmacComputeAndVerifyRoundTrip) {
    ensure_psa_ready();
    const uint8_t key[16] = {1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16};
    psa_key_id_t key_id;
    ASSERT_TRUE(comm_utils_import_cmac_key(key, sizeof(key), &key_id));

    const uint8_t input[] = "ETM1 test input, arbitrary length";
    uint8_t tag[8] = {0};
    ASSERT_TRUE(cmac_compute_with_key_id(key_id, input, sizeof(input), tag, sizeof(tag)));
    EXPECT_TRUE(cmac_verify_with_key_id(key_id, input, sizeof(input), tag, sizeof(tag)));

    uint8_t bad_tag[8];
    memcpy(bad_tag, tag, sizeof(tag));
    bad_tag[0] ^= 0x01;
    EXPECT_FALSE(cmac_verify_with_key_id(key_id, input, sizeof(input), bad_tag, sizeof(bad_tag)))
        << "a single flipped bit must fail verification";

    comm_utils_destroy_key(key_id);
}

TEST(CommUtilsTier3, CmacIsDeterministicAndKeyed) {
    ensure_psa_ready();
    const uint8_t key_a[16] = {1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16};
    const uint8_t key_b[16] = {2, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16};
    psa_key_id_t id_a, id_b;
    ASSERT_TRUE(comm_utils_import_cmac_key(key_a, sizeof(key_a), &id_a));
    ASSERT_TRUE(comm_utils_import_cmac_key(key_b, sizeof(key_b), &id_b));

    const uint8_t input[] = "same input, different keys";
    uint8_t tag_a1[8], tag_a2[8], tag_b[8];
    ASSERT_TRUE(cmac_compute_with_key_id(id_a, input, sizeof(input), tag_a1, sizeof(tag_a1)));
    ASSERT_TRUE(cmac_compute_with_key_id(id_a, input, sizeof(input), tag_a2, sizeof(tag_a2)));
    ASSERT_TRUE(cmac_compute_with_key_id(id_b, input, sizeof(input), tag_b, sizeof(tag_b)));

    EXPECT_EQ(memcmp(tag_a1, tag_a2, sizeof(tag_a1)), 0)
        << "CMAC has no nonce — the same (key, input) must always tag identically";
    EXPECT_NE(memcmp(tag_a1, tag_b, sizeof(tag_a1)), 0) << "different keys must tag differently";

    comm_utils_destroy_key(id_a);
    comm_utils_destroy_key(id_b);
}

TEST(CommUtilsTier3, CmacCanProduceAFullSixteenByteKdfOutput) {
    // K_auth/K_enc/K_mac derivation needs the FULL CMAC block (16 bytes), not
    // just the 8-byte truncated MIC tag.
    ensure_psa_ready();
    const uint8_t key[16] = {0};
    psa_key_id_t key_id;
    ASSERT_TRUE(comm_utils_import_cmac_key(key, sizeof(key), &key_id));

    const uint8_t kdf_input[16] = {0x01, 'B', 'L', 'S', '1', 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0};
    uint8_t derived[16] = {0};
    ASSERT_TRUE(cmac_compute_with_key_id(key_id, kdf_input, sizeof(kdf_input), derived, sizeof(derived)));

    bool any_nonzero = false;
    for (uint8_t b : derived) if (b != 0) any_nonzero = true;
    EXPECT_TRUE(any_nonzero) << "a derived key of all zeroes would be a real hazard, not just unlikely";

    comm_utils_destroy_key(key_id);
}

TEST(CommUtilsTier3, CtrEncryptThenDecryptRoundTrips) {
    ensure_psa_ready();
    const uint8_t key[16] = {9, 9, 9, 9, 9, 9, 9, 9, 9, 9, 9, 9, 9, 9, 9, 9};
    psa_key_id_t key_id;
    ASSERT_TRUE(comm_utils_import_ctr_key(key, sizeof(key), &key_id));

    uint8_t iv[16] = {0};
    iv[0] = 0xAA;  // stand-in for sessionId_BE32's first byte

    const uint8_t plain[] = "this is the plaintext payload, > 1 block long!!";
    uint8_t cipher[sizeof(plain)] = {0};
    ASSERT_TRUE(ctr_crypt_with_key_id(key_id, iv, /*encrypt=*/true, plain, sizeof(plain), cipher));
    EXPECT_NE(memcmp(plain, cipher, sizeof(plain)), 0) << "ciphertext must not equal plaintext";

    uint8_t decrypted[sizeof(plain)] = {0};
    ASSERT_TRUE(ctr_crypt_with_key_id(key_id, iv, /*encrypt=*/false, cipher, sizeof(cipher), decrypted));
    EXPECT_EQ(memcmp(plain, decrypted, sizeof(plain)), 0);

    comm_utils_destroy_key(key_id);
}

TEST(CommUtilsTier3, CtrDifferentCounterBlocksProduceDifferentKeystream) {
    ensure_psa_ready();
    const uint8_t key[16] = {5, 5, 5, 5, 5, 5, 5, 5, 5, 5, 5, 5, 5, 5, 5, 5};
    psa_key_id_t key_id;
    ASSERT_TRUE(comm_utils_import_ctr_key(key, sizeof(key), &key_id));

    const uint8_t plain[16] = {0};  // all-zero plaintext: ciphertext IS the keystream
    uint8_t iv1[16] = {0}, iv2[16] = {0};
    iv2[11] = 1;  // differs only in the msgId field's low byte

    uint8_t ks1[16], ks2[16];
    ASSERT_TRUE(ctr_crypt_with_key_id(key_id, iv1, true, plain, sizeof(plain), ks1));
    ASSERT_TRUE(ctr_crypt_with_key_id(key_id, iv2, true, plain, sizeof(plain), ks2));
    EXPECT_NE(memcmp(ks1, ks2, sizeof(ks1)), 0)
        << "two different counter blocks must never produce the same keystream — "
           "this is the invariant the whole msgId/nodeAddr/dir layout exists to hold";

    comm_utils_destroy_key(key_id);
}

TEST(CommUtilsTier3, CtrIsDeterministicUnderTheSameCounterBlock) {
    // The seal-once invariant (I1): the same (key, counter block) must always
    // produce the same ciphertext, since the real firmware seals exactly once
    // per msgId and retransmits the stored bytes verbatim.
    ensure_psa_ready();
    const uint8_t key[16] = {7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7};
    psa_key_id_t key_id;
    ASSERT_TRUE(comm_utils_import_ctr_key(key, sizeof(key), &key_id));

    uint8_t iv[16] = {0};
    const uint8_t plain[32] = "deterministic across two calls";
    uint8_t out1[32], out2[32];
    ASSERT_TRUE(ctr_crypt_with_key_id(key_id, iv, true, plain, sizeof(plain), out1));
    ASSERT_TRUE(ctr_crypt_with_key_id(key_id, iv, true, plain, sizeof(plain), out2));
    EXPECT_EQ(memcmp(out1, out2, sizeof(out1)), 0);

    comm_utils_destroy_key(key_id);
}

// FrameCrypto -- the key-derivation and CTR byte layouts MAC wire v1 still
// shares with the node (the frame tag itself is pinned by MacWireVectors.h in
// mac_wire_v1_test).
//
// These bytes are the least forgiving thing in the system: every mistake in
// them -- a swapped byte order, a missing direction bit -- surfaces as a tag
// that does not verify, with no diagnostic on the air. So these tests do not
// check "encrypt then decrypt round-trips" -- that passes happily with both
// ends wrong in the same way. They pin the literal bytes instead.
//
// S8 / W5: the v0 envelope's AAD, GCM IV, beacon-MAC input, LOGIN / REGISTER
// MIC inputs and Encrypt-then-CMAC prefix are gone from FrameCrypto.h (node
// f393573) together with their tests; v1 authenticates raw frame bytes.

#include <gtest/gtest.h>

#include <string.h>

#include "FrameCrypto.h"
#include "MacWire.h"

using namespace framecrypto;

// ---------------------------------------------------------------------------
// Constants the two ends must agree on
// ---------------------------------------------------------------------------

TEST(FrameCrypto, TagAndKeySizesMatchTheWire) {
    // The frame tag is CMAC truncated to 8 bytes on air, the keys AES-128. A
    // mismatch fails authentication on every frame.
    EXPECT_EQ(kSessionCmacTagBytes, 8u);
    EXPECT_EQ(kSessionCmacTagBytes, macwire::kTagBytes);
    EXPECT_EQ(kNetKeyBytes, 16u) << "AES-128, like the session key";
}

TEST(FrameCrypto, AZeroKeyOrZeroIdIsNotAKey) {
    // Both are proto3 defaults, so "the field was absent" and "the hub sent
    // zeroes" arrive identically. A node that treated either as a key would
    // start refusing every real beacon while believing it was authenticating.
    uint8_t key[kNetKeyBytes];
    memset(key, 0xA5, sizeof(key));
    EXPECT_TRUE(netKeyIsSet(key, sizeof(key), 1));

    EXPECT_FALSE(netKeyIsSet(key, sizeof(key), 0)) << "id zero means unset";
    EXPECT_FALSE(netKeyIsSet(nullptr, kNetKeyBytes, 1));
    EXPECT_FALSE(netKeyIsSet(key, kNetKeyBytes - 1, 1)) << "wrong length";

    memset(key, 0, sizeof(key));
    EXPECT_FALSE(netKeyIsSet(key, sizeof(key), 1)) << "all zeroes is not a key";
}

// ---------------------------------------------------------------------------
// Tier 3 (mac-separation-implementation-plan.md section 2(b)): the KDF and CTR
// byte layouts. Same discipline -- pin the exact bytes, don't just round-trip.
// ---------------------------------------------------------------------------

TEST(FrameCryptoTier3, KAuthKdfInputIsPinned) {
    uint8_t out[kKdfInputBytes];
    buildKAuthKdfInput(out);
    static constexpr uint8_t kExpected[kKdfInputBytes] = {
        0x03, 'B', 'L', 'S', '1', 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    };
    EXPECT_EQ(memcmp(out, kExpected, sizeof(out)), 0);
}

TEST(FrameCryptoTier3, SessionKeyKdfInputDiffersOnlyByTheHalfByte) {
    uint8_t enc[kKdfInputBytes], mac[kKdfInputBytes];
    buildSessionKeyKdfInput(0x01, 100, 200, 1, 2, enc);
    buildSessionKeyKdfInput(0x02, 100, 200, 1, 2, mac);
    EXPECT_EQ(enc[0], 0x01);
    EXPECT_EQ(mac[0], 0x02);
    EXPECT_EQ(memcmp(enc + 1, mac + 1, kKdfInputBytes - 1), 0)
        << "K_enc and K_mac must derive from identical inputs except the "
           "leading domain byte — anything else reuses key material across "
           "the two independent keys the CTR/CMAC split requires";

    // Every other field must actually be covered — not dead padding.
    uint8_t v[kKdfInputBytes];
    buildSessionKeyKdfInput(0x01, 999, 200, 1, 2, v);
    EXPECT_NE(memcmp(enc, v, sizeof(v)), 0) << "session_id";
    buildSessionKeyKdfInput(0x01, 100, 999, 1, 2, v);
    EXPECT_NE(memcmp(enc, v, sizeof(v)), 0) << "node_nonce";
    buildSessionKeyKdfInput(0x01, 100, 200, 9, 2, v);
    EXPECT_NE(memcmp(enc, v, sizeof(v)), 0) << "hub_addr";
    buildSessionKeyKdfInput(0x01, 100, 200, 1, 9, v);
    EXPECT_NE(memcmp(enc, v, sizeof(v)), 0) << "node_addr";
}

TEST(FrameCryptoTier3, CtrInitialBlockSeparatesDirectionAndNodeAndSession) {
    uint8_t base[kCtrBlockBytes];
    buildCtrInitialBlock(/*session_id=*/10, /*node_addr=*/17, /*downlink=*/false,
                         /*msgid=*/5, /*block_idx=*/0, base);

    uint8_t v[kCtrBlockBytes];
    buildCtrInitialBlock(99, 17, false, 5, 0, v);
    EXPECT_NE(memcmp(base, v, sizeof(v)), 0) << "session_id";
    buildCtrInitialBlock(10, 18, false, 5, 0, v);
    EXPECT_NE(memcmp(base, v, sizeof(v)), 0)
        << "node_addr — this is what keeps two different nodes under the "
           "same sessionId from ever sharing a counter block";
    buildCtrInitialBlock(10, 17, true, 5, 0, v);
    EXPECT_NE(memcmp(base, v, sizeof(v)), 0) << "direction bit";
    buildCtrInitialBlock(10, 17, false, 6, 0, v);
    EXPECT_NE(memcmp(base, v, sizeof(v)), 0) << "msgid";
    buildCtrInitialBlock(10, 17, false, 5, 1, v);
    EXPECT_NE(memcmp(base, v, sizeof(v)), 0) << "block_idx";
}

TEST(FrameCryptoTier3, CtrDirectionBitNeverCollidesWithMsgid) {
    // msgId is refused at/above 2^31 by the allocator (the formal bound that
    // keeps this bit and msgId from ever overlapping) — confirm the layout
    // itself keeps them separate for a msgId right at that boundary.
    uint8_t up[kCtrBlockBytes], down[kCtrBlockBytes];
    const uint32_t near_boundary = 0x7FFFFFFFu;
    buildCtrInitialBlock(1, 1, false, near_boundary, 0, up);
    buildCtrInitialBlock(1, 1, true, near_boundary, 0, down);
    EXPECT_NE(memcmp(up, down, sizeof(up)), 0)
        << "direction must still distinguish the two even at the largest "
           "legal msgId";
}

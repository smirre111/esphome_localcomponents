// FrameCrypto — the AEAD wire-format derivations.
//
// These bytes are the least forgiving thing in the system. Every mistake in
// them — a swapped byte order, a field in the wrong AAD slot, a missing
// direction bit — surfaces as exactly one symptom:
//
//     psa_aead_decrypt failed: -149
//
// That single code has stood for at least three unrelated root causes during
// this project, and each time it cost a hardware session to tell them apart.
// So these tests do not check "encrypt then decrypt round-trips" — that passes
// happily with both ends wrong in the same way, which is precisely the bug
// class that hurts. They pin the literal bytes instead.

#include <gtest/gtest.h>

#include <string.h>

#include "FrameCrypto.h"

using namespace framecrypto;

namespace {

// A header whose four fields are all distinct and none of which is a
// palindrome, so a byte-order slip or a field swap cannot pass unnoticed.
constexpr uint32_t kDest       = 0x01020304;
constexpr uint32_t kSubnet     = 0x05060708;
constexpr uint32_t kSender     = 0x090A0B0C;
constexpr uint32_t kMsgId      = 0x0D0E0F10;
constexpr uint32_t kBaseNonce  = 0xA1B2C3D4;

}  // namespace

// ---------------------------------------------------------------------------
// AAD
// ---------------------------------------------------------------------------

TEST(FrameCrypto, AadIsTheFourHeaderFieldsBigEndian) {
    uint8_t aad[kAadBytes];
    memset(aad, 0xEE, sizeof(aad));
    buildAad(kDest, kSubnet, kSender, kMsgId, aad);

    const uint8_t expect[kAadBytes] = {
        0x01, 0x02, 0x03, 0x04,   // destAddress
        0x05, 0x06, 0x07, 0x08,   // destSubnet
        0x09, 0x0A, 0x0B, 0x0C,   // senderAddress
        0x0D, 0x0E, 0x0F, 0x10,   // msgid
    };
    EXPECT_EQ(memcmp(aad, expect, kAadBytes), 0)
        << "the hub builds these 16 bytes independently; both sides must agree "
           "on order and endianness or nothing decrypts";
}

TEST(FrameCrypto, AadIsExactlySixteenBytes) {
    // The payload is deliberately NOT authenticated through the AAD — it is the
    // ciphertext — and the old `encrypted` header field is gone from the wire.
    // Either creeping back in changes the length and breaks every peer.
    EXPECT_EQ(kAadBytes, 16u);
}

TEST(FrameCrypto, EveryHeaderFieldReachesTheAad) {
    // A field dropped or written to the wrong slot still yields 16 plausible
    // bytes. Vary each field alone and require the AAD to move.
    uint8_t base[kAadBytes];
    buildAad(kDest, kSubnet, kSender, kMsgId, base);

    const uint32_t bumped[4][4] = {
        {kDest + 1, kSubnet, kSender, kMsgId},
        {kDest, kSubnet + 1, kSender, kMsgId},
        {kDest, kSubnet, kSender + 1, kMsgId},
        {kDest, kSubnet, kSender, kMsgId + 1},
    };
    for (const auto &f : bumped) {
        uint8_t aad[kAadBytes];
        buildAad(f[0], f[1], f[2], f[3], aad);
        EXPECT_NE(memcmp(aad, base, kAadBytes), 0);
    }
}

TEST(FrameCrypto, TheAadFieldsAreNotInterchangeable) {
    // Swapping two fields keeps the same 16 bytes present but in the wrong
    // places — invisible to any test that only checks "something changed".
    uint8_t normal[kAadBytes], swapped[kAadBytes];
    buildAad(kDest, kSubnet, kSender, kMsgId, normal);
    buildAad(kSubnet, kDest, kSender, kMsgId, swapped);
    EXPECT_NE(memcmp(normal, swapped, kAadBytes), 0);
}

// ---------------------------------------------------------------------------
// IV
// ---------------------------------------------------------------------------

TEST(FrameCrypto, IvIsBaseNonceThenCounterBigEndian) {
    uint8_t iv[kIvBytes];
    memset(iv, 0xEE, sizeof(iv));
    ASSERT_TRUE(deriveIv(kBaseNonce, 0x1122334455667788ULL, iv));

    const uint8_t expect[kIvBytes] = {
        0xA1, 0xB2, 0xC3, 0xD4,                            // base nonce
        0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77, 0x88,    // counter
    };
    EXPECT_EQ(memcmp(iv, expect, kIvBytes), 0);
}

TEST(FrameCrypto, IvIsExactlyTwelveBytes) {
    EXPECT_EQ(kIvBytes, 12u) << "AES-GCM's standard IV length; 12 avoids the "
                                "GHASH-based derivation for other lengths";
}

TEST(FrameCrypto, AZeroBaseNonceIsRefused) {
    // Zero is the cleared/never-set sentinel. Producing a usable-looking IV
    // from it is how a node came to encrypt a resume beacon the hub had no key
    // material for — it looked fine locally and failed only on the hub.
    uint8_t iv[kIvBytes];
    memset(iv, 0xEE, sizeof(iv));
    EXPECT_FALSE(deriveIv(0, 42, iv));
    for (size_t i = 0; i < kIvBytes; i++)
        EXPECT_EQ(iv[i], 0xEE) << "a refused derivation must not write output";
}

TEST(FrameCrypto, DistinctCountersGiveDistinctIvs) {
    uint8_t a[kIvBytes], b[kIvBytes];
    ASSERT_TRUE(deriveIv(kBaseNonce, 1, a));
    ASSERT_TRUE(deriveIv(kBaseNonce, 2, b));
    EXPECT_NE(memcmp(a, b, kIvBytes), 0);
}

TEST(FrameCrypto, TheCounterOccupiesTheFullSixtyFourBits) {
    // Truncating the counter to 32 bits would silently alias the direction bit
    // away, which is the one failure this whole mechanism exists to prevent.
    uint8_t low[kIvBytes], high[kIvBytes];
    ASSERT_TRUE(deriveIv(kBaseNonce, 1ULL, low));
    ASSERT_TRUE(deriveIv(kBaseNonce, 1ULL | (1ULL << 40), high));
    EXPECT_NE(memcmp(low, high, kIvBytes), 0);
}

// ---------------------------------------------------------------------------
// Direction separation
// ---------------------------------------------------------------------------

TEST(FrameCrypto, UplinkAndDownlinkNeverShareAnIv) {
    // Both directions derive the IV from the SAME per-peer base nonce, so
    // without the direction bit an uplink and a downlink carrying the same
    // msgid would reuse an IV. Under AES-GCM that leaks the keystream and
    // forges the authenticator — and it is completely silent on the wire.
    const uint32_t msgid = 7;
    uint8_t up[kIvBytes], down[kIvBytes];
    ASSERT_TRUE(deriveIv(kBaseNonce, frameCounter(msgid, false), up));
    ASSERT_TRUE(deriveIv(kBaseNonce, frameCounter(msgid, true), down));
    EXPECT_NE(memcmp(up, down, kIvBytes), 0);
}

TEST(FrameCrypto, UplinkLeavesTheCounterUntouched) {
    // Deliberate: uplink IVs stay byte-identical to the format from before the
    // direction bit existed. Flagging uplink instead of downlink would look
    // equally "correct" and would break every deployed peer.
    EXPECT_EQ(frameCounter(0, false), 0ULL);
    EXPECT_EQ(frameCounter(12345, false), 12345ULL);
    EXPECT_EQ(frameCounter(0xFFFFFFFFu, false), 0xFFFFFFFFULL);
}

TEST(FrameCrypto, DownlinkSetsTheTopBitAndKeepsTheMsgId) {
    EXPECT_EQ(frameCounter(12345, true), 12345ULL | (1ULL << 63));
    EXPECT_EQ(frameCounter(12345, true) & 0xFFFFFFFFULL, 12345ULL)
        << "the msgid must survive intact — the receiver masks it back out";
}

TEST(FrameCrypto, TheDirectionBitIsAboveEveryReachableMsgId) {
    // msgid is a uint32, so the top bit of a uint64 counter can never collide
    // with a real message id. If the counter ever widens, this is the guard.
    EXPECT_EQ(kDownlinkFlag, 1ULL << 63);
    EXPECT_GT(kDownlinkFlag, static_cast<uint64_t>(UINT32_MAX));
    EXPECT_EQ(frameCounter(UINT32_MAX, false) & kDownlinkFlag, 0ULL);
}

// ---------------------------------------------------------------------------
// Constants the two ends must agree on
// ---------------------------------------------------------------------------

TEST(FrameCrypto, TagAndKeySizesMatchTheHub) {
    // The hub truncates to 8 bytes to stay slim on air, and the key is
    // AES-128 derived as SHA-256("LoRaKey1")[0:16]. A mismatch here fails
    // authentication on every frame with, again, -149.
    EXPECT_EQ(kTagBytes, 8u);
    EXPECT_EQ(kKeyBytes, 16u);
}

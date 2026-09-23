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

// ---------------------------------------------------------------------------
// The broadcast beacon's authenticator (section 4.4)
//
// The one thing that can silently disagree between hub and node is WHICH BYTES
// go into the MAC. Both ends call buildBeaconMacInput, so the layout is pinned
// here byte by byte — a tag computed over a different byte order or a missing
// field fails exactly like a wrong key, and there is no diagnostic on the air
// to tell them apart.
// ---------------------------------------------------------------------------

TEST(FrameCrypto, BeaconMacInputIsFixedLengthAndDomainSeparated) {
    uint8_t in[kBeaconMacInputBytes];
    buildBeaconMacInput(0x11223344u, 0x55667788u, 31u, 0xDEADBEEFu, true, in);

    // The domain tag comes first, so this key can never be made to authenticate
    // anything but a beacon — a fleet key that also signed, say, a config frame
    // would let a compromised node repurpose it.
    EXPECT_EQ(in[0], 'G');
    EXPECT_EQ(in[1], 'B');
    EXPECT_EQ(in[2], '1');

    const uint8_t want[kBeaconMacInputBytes] = {
        'G', 'B', '1',
        0x11, 0x22, 0x33, 0x44,      // netKeyId, big-endian like every other
        0x55, 0x66, 0x77, 0x88,      // txRound
        0x00, 0x00, 0x00, 0x1F,      // txSlot
        0xDE, 0xAD, 0xBE, 0xEF,      // pendingMask
        0x01,                        // pendingMaskValid
    };
    EXPECT_EQ(memcmp(in, want, sizeof(want)), 0);

    // Fixed length is the property, not a convenience. With variable-length
    // fields two different beacons could serialise to the same bytes and share
    // a tag; at 20 bytes there is no boundary to shift.
    EXPECT_EQ(kBeaconMacInputBytes, 20u);
}

TEST(FrameCrypto, TheValidityFlagIsCoveredByTheMac) {
    // It is the field that decides whether the mask MEANS anything, so a MAC
    // that omitted it would let an attacker turn "listen" into a real all-clear
    // without touching a signed byte.
    uint8_t a[kBeaconMacInputBytes], b[kBeaconMacInputBytes];
    buildBeaconMacInput(1, 2, 3, 0, /*valid=*/false, a);
    buildBeaconMacInput(1, 2, 3, 0, /*valid=*/true,  b);
    EXPECT_NE(memcmp(a, b, sizeof(a)), 0);
}

TEST(FrameCrypto, EveryBeaconFieldChangesTheMacInput) {
    // A tag over only the round would not catch a mask substitution, which is
    // the cheapest useful forgery: replay a real beacon's round with an
    // all-clear mask and the fleet stops listening.
    uint8_t base[kBeaconMacInputBytes];
    buildBeaconMacInput(1, 2, 3, 4, true, base);

    uint8_t v[kBeaconMacInputBytes];
    buildBeaconMacInput(9, 2, 3, 4, true, v);
    EXPECT_NE(memcmp(base, v, sizeof(v)), 0) << "netKeyId";
    buildBeaconMacInput(1, 9, 3, 4, true, v);
    EXPECT_NE(memcmp(base, v, sizeof(v)), 0) << "txRound";
    buildBeaconMacInput(1, 2, 9, 4, true, v);
    EXPECT_NE(memcmp(base, v, sizeof(v)), 0) << "txSlot";
    buildBeaconMacInput(1, 2, 3, 9, true, v);
    EXPECT_NE(memcmp(base, v, sizeof(v)), 0) << "pendingMask";
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

TEST(FrameCrypto, TheBeaconTagSharesTheAeadTagBudget) {
    // 8 bytes on the air, like the AEAD tag, because the beacon is priced into
    // the slot geometry at kBeaconPayloadBytes and a wider tag would eat the
    // margin that keeps beaconClearSlots() at one.
    EXPECT_EQ(kBeaconMacBytes, kTagBytes);
    EXPECT_EQ(kNetKeyBytes, kKeyBytes) << "AES-128, like the session key";
}

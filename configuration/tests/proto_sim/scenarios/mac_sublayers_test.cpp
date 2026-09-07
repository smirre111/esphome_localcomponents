// MacSublayers — MAC-1 / MAC-2 as switchable sublayers.
//
// The valuable tests here are the refusals and the scope limit. A switch that
// could disable the replay window or authentication for real commands would be
// a remote-unlock hole wearing a measurement's clothes, so "application traffic
// is never exempt" is asserted directly rather than left to a comment.
//
// See docs/mac-layer.md section 4.

#include <gtest/gtest.h>

#include "MacSublayers.h"

using namespace macsublayers;

// ---------------------------------------------------------------------------
// Defaults are the safe configuration
// ---------------------------------------------------------------------------

TEST(MacSublayers, BothSublayersDefaultOn) {
    // A zeroed Config — proto3 defaults, a cold boot, a corrupted NVS blob —
    // must be the SAFE state, never the permissive one.
    Config c;
    EXPECT_TRUE(c.counter_enabled);
    EXPECT_TRUE(c.crypto_enabled);
}

// ---------------------------------------------------------------------------
// The scope limit — the safety hinge
// ---------------------------------------------------------------------------

TEST(MacSublayers, ApplicationTrafficIsNeverExempt) {
    // Every combination of the switches, against a non-MAC-control frame.
    // All four must still require both checks.
    for (bool ctr : {false, true})
        for (bool cry : {false, true}) {
            Config c{ctr, cry};
            EXPECT_TRUE(counterCheckRequired(c, /*is_mac_control=*/false))
                << "counter " << ctr << " crypto " << cry;
            EXPECT_TRUE(cryptoRequired(c, /*is_mac_control=*/false))
                << "counter " << ctr << " crypto " << cry;
        }
}

TEST(MacSublayers, MacControlFramesFollowTheSwitches) {
    Config on;
    EXPECT_TRUE(counterCheckRequired(on, true));
    EXPECT_TRUE(cryptoRequired(on, true));

    Config off{false, false};
    EXPECT_FALSE(counterCheckRequired(off, true));
    EXPECT_FALSE(cryptoRequired(off, true));
}

TEST(MacSublayers, TheSwitchesAreIndependent) {
    // Attribution needs them separable: MAC-0, then +MAC-1, then +MAC-2, so
    // each delta is one sublayer's cost rather than a lump.
    Config counter_only{true, false};
    EXPECT_TRUE(counterCheckRequired(counter_only, true));
    EXPECT_FALSE(cryptoRequired(counter_only, true));

    Config crypto_only{false, true};
    EXPECT_FALSE(counterCheckRequired(crypto_only, true));
    EXPECT_TRUE(cryptoRequired(crypto_only, true));
}

// ---------------------------------------------------------------------------
// Arming is authenticated even when the traffic is not
// ---------------------------------------------------------------------------

TEST(MacSublayers, NoSessionCannotArm) {
    // Otherwise the unauthenticated bootstrap window every node passes through
    // on every cold boot becomes a way to hold 32 nodes in a test config.
    EXPECT_EQ(armRefusal(/*has_session=*/false, /*authenticated=*/true,
                         /*bench=*/true, /*degrading=*/true),
              ArmRefusal::NoSession);
    EXPECT_FALSE(mayArm(false, true, true, true));
}

TEST(MacSublayers, APlaintextRequestCannotDisableAuthentication) {
    // A plaintext frame asking to turn authentication off answers its own
    // question. This is the assertion that matters most in the file.
    EXPECT_EQ(armRefusal(/*has_session=*/true, /*authenticated=*/false,
                         /*bench=*/true, /*degrading=*/true),
              ArmRefusal::NotAuthenticated);
    EXPECT_FALSE(mayArm(true, false, true, true));
}

TEST(MacSublayers, DegradingModesAreBenchOnly) {
    EXPECT_EQ(armRefusal(true, true, /*bench=*/false, /*degrading=*/true),
              ArmRefusal::NotBenchNode);
    // A non-degrading run (both sublayers left on) is legitimate in the field.
    EXPECT_TRUE(mayArm(true, true, /*bench=*/false, /*degrading=*/false));
}

TEST(MacSublayers, RefusalOrderReportsTheMostFundamentalCauseFirst) {
    // With nothing in place, "no session" is the actionable answer; reporting
    // "not a bench node" would send the operator down the wrong path.
    EXPECT_EQ(armRefusal(false, false, false, true), ArmRefusal::NoSession);
    EXPECT_EQ(armRefusal(true, false, false, true), ArmRefusal::NotAuthenticated);
}

TEST(MacSublayers, AFullyAuthorisedRequestIsAccepted) {
    EXPECT_EQ(armRefusal(true, true, true, true), ArmRefusal::None);
    EXPECT_TRUE(mayArm(true, true, true, true));
}

// ---------------------------------------------------------------------------
// The node owns the deadline
// ---------------------------------------------------------------------------

TEST(MacSublayers, DurationIsCappedByTheNode) {
    EXPECT_EQ(disableDurationS(0), kDefaultDisableS) << "0 means the node default";
    EXPECT_EQ(disableDurationS(60), 60u);
    EXPECT_EQ(disableDurationS(kMaxDisableS), kMaxDisableS);
    EXPECT_EQ(disableDurationS(kMaxDisableS + 1), kMaxDisableS);
    // "Forever" is not expressible — a hub that vanishes mid-test cannot leave
    // a node parked with a sublayer off.
    EXPECT_EQ(disableDurationS(0xFFFFFFFF), kMaxDisableS);
}

TEST(MacSublayers, TheCapIsShortEnoughToMatter) {
    // A cap measured in hours would not be a safety property.
    EXPECT_LE(kMaxDisableS, 3600u);
    EXPECT_LE(kDefaultDisableS, kMaxDisableS);
}

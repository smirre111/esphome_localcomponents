// Scenarios G1, G2, G3 — CoverConfig application and the all-three-non-zero
// geometry guard.
//
// Production guard (CmdDispatcher.cpp:1291): the node applies blindHeightMm,
// axleDiameterMm, blindThicknessMm only when ALL THREE are non-zero. proto3
// scalars default to 0 on the wire, so a partial populate must NOT clobber
// the firmware defaults.
//
// open_time / close_time USED to be applied unconditionally, and this comment
// used to record that as if it were intended. It was not: a zero duration is
// the proto3 encoding of "unset", and accepting it cost two nodes their travel
// times on 2026-09-22 (D1 below). They now go through
// motorpolicy::keptTravelDurationS() like every other field with a 0 = unset
// convention.

#include "sim/hub_model.h"
#include "sim/node_model.h"

#include <gtest/gtest.h>

using namespace proto_sim;

namespace {

constexpr uint64_t kMacRol1 = 0xE08CFE5FB7A4ULL;

struct CoverConfigFixture : public ::testing::Test {
    SimClock        clock;
    SimRadio        radio;
    SharedNonceMap  nonces;
    HubTracker      tracker{&clock, &radio, &nonces};
    HubListener rol_1{"rol_1", 17, 2, kMacRol1, 21600, &tracker, &clock, &nonces};
    NodeModel   node_1{"node_1", kMacRol1, &clock, &radio};

    void SetUp() override {
        tracker.register_listener(&rol_1);
        rol_1.wipe_nvs();
        rol_1.setup(/*time_valid_at_boot=*/false);
        node_1.send_register();
        ASSERT_EQ(node_1.cfg_address(), 17);
        node_1.reboot(/*keep_cfg=*/true);
    }
};

// G1: fully populated CoverConfig applies open/close + all three geometry fields.
TEST_F(CoverConfigFixture, FullGeometryApplied) {
    rol_1.send_cover_config(60, 65, 2000.0f, 60.0f, 8.0f);

    EXPECT_EQ(node_1.open_time_s(),  60u);
    EXPECT_EQ(node_1.close_time_s(), 65u);
    EXPECT_TRUE(node_1.geometry_applied());
    EXPECT_FLOAT_EQ(node_1.height_mm(),    2000.0f);
    EXPECT_FLOAT_EQ(node_1.axle_mm(),        60.0f);
    EXPECT_FLOAT_EQ(node_1.thickness_mm(),    8.0f);
}

// G2: all-zero geometry (proto3 unset) leaves firmware defaults intact.
TEST_F(CoverConfigFixture, AllZeroGeometrySkipped) {
    rol_1.send_cover_config(60, 65, 0.0f, 0.0f, 0.0f);

    EXPECT_EQ(node_1.open_time_s(),  60u);
    EXPECT_EQ(node_1.close_time_s(), 65u);
    EXPECT_FALSE(node_1.geometry_applied())
        << "All-zero geometry is the proto3 'unset' encoding — applying it "
           "would wipe the firmware defaults.";
    EXPECT_FLOAT_EQ(node_1.height_mm(),    0.0f);
    EXPECT_FLOAT_EQ(node_1.axle_mm(),      0.0f);
    EXPECT_FLOAT_EQ(node_1.thickness_mm(), 0.0f);
}

// G3: partial geometry (one field zero) must NOT apply any of the three.
TEST_F(CoverConfigFixture, MixedZeroGeometryRejectedAtomically) {
    rol_1.send_cover_config(60, 65, 2000.0f, 60.0f, /*thickness=*/0.0f);

    EXPECT_EQ(node_1.open_time_s(),  60u);
    EXPECT_EQ(node_1.close_time_s(), 65u);
    EXPECT_FALSE(node_1.geometry_applied())
        << "Partial geometry must be all-or-nothing — applying height+axle "
           "but leaving thickness at the firmware default would silently "
           "produce a position calculation error.";
    EXPECT_FLOAT_EQ(node_1.height_mm(),    0.0f);
    EXPECT_FLOAT_EQ(node_1.axle_mm(),      0.0f);
    EXPECT_FLOAT_EQ(node_1.thickness_mm(), 0.0f);
}

// ---------------------------------------------------------------------------
// D1-D3 — the travel-duration zero guard.
//
// MEASURED 2026-09-22. Node 2 (fw 1.1.4) held
// "motorOpenDuration":0,"motorCloseDuration":0 in its config.txt, and every
// move was instant:
//
//   MOTCMD_FULL_DOWN -> Current position 1.000000 -> 0.000000  (~1 s of motion)
//
// runMsForMove() returns the configured duration for a full move, so a zero
// fires the move timer immediately; a TIMER arriving in a FULLY_* state SNAPS
// the position to the extreme. The blind twitched through the LEDC stop fade
// while the reported position teleported to fully closed. On the other node the
// same zero made a stop at 80 % report "closed".
//
// Note what the existing geometry cases above do NOT cover: every one of them
// passes 60, 65 as the durations. Nothing anywhere sent a zero duration, which
// is exactly why this shipped.
// ---------------------------------------------------------------------------

// D1: a zero duration is "unset" and must not replace a real one.
TEST_F(CoverConfigFixture, ZeroTravelDurationsAreRefused) {
    // The node must first HOLD real durations — "a zero is refused" is
    // meaningless against a node that never had any.
    rol_1.send_cover_config(60, 65, 2000.0f, 60.0f, 8.0f);
    ASSERT_EQ(node_1.open_time_s(),  60u) << "precondition: durations installed";
    ASSERT_EQ(node_1.close_time_s(), 65u) << "precondition: durations installed";

    // Now a CoverConfig that leaves openTime/closeTime unset. On the wire this
    // is indistinguishable from an explicit zero.
    rol_1.send_cover_config(0, 0, 2000.0f, 60.0f, 8.0f);

    EXPECT_EQ(node_1.open_time_s(),  60u)
        << "a zero openTime is the proto3 'unset' encoding, not a duration — "
           "accepting it makes runMsForMove() return 0, fires the move timer "
           "immediately, and the FULLY_* TIMER transition snaps the position "
           "to an extreme after ~1 s of motion";
    EXPECT_EQ(node_1.close_time_s(), 65u)
        << "same for closeTime — this is the one that reported 'closed' from "
           "a blind that had barely moved";
}

// D2: one zero must not drag down the other. Unlike geometry, which is
// all-or-nothing, the durations are independent: a hub that sets only openTime
// should install it and leave closeTime alone.
TEST_F(CoverConfigFixture, AZeroDurationDoesNotClobberItsNonZeroPartner) {
    rol_1.send_cover_config(60, 65, 2000.0f, 60.0f, 8.0f);
    ASSERT_EQ(node_1.open_time_s(),  60u);
    ASSERT_EQ(node_1.close_time_s(), 65u);

    rol_1.send_cover_config(42, 0, 2000.0f, 60.0f, 8.0f);

    EXPECT_EQ(node_1.open_time_s(),  42u)
        << "a real openTime must still be applied — a guard that refused "
           "everything would trade one silent failure for another";
    EXPECT_EQ(node_1.close_time_s(), 65u)
        << "the zero closeTime is unset and keeps the stored value";
}

// D3: the guard must not become "ignore the hub". A node that has durations
// must still be reconfigurable — this is the half a return-current-always
// mutant breaks.
TEST_F(CoverConfigFixture, RealDurationsStillOverwriteStoredOnes) {
    rol_1.send_cover_config(60, 65, 2000.0f, 60.0f, 8.0f);
    ASSERT_EQ(node_1.open_time_s(), 60u);

    rol_1.send_cover_config(38, 36, 2000.0f, 60.0f, 8.0f);

    EXPECT_EQ(node_1.open_time_s(),  38u) << "recalibration must reach the node";
    EXPECT_EQ(node_1.close_time_s(), 36u);
}

} // namespace

// Scenarios G1, G2, G3 — CoverConfig application and the all-three-non-zero
// geometry guard.
//
// Production guard (CmdDispatcher.cpp:1291): the node applies blindHeightMm,
// axleDiameterMm, blindThicknessMm only when ALL THREE are non-zero. proto3
// scalars default to 0 on the wire, so a partial populate must NOT clobber
// the firmware defaults. open_time / close_time are applied unconditionally.

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

} // namespace

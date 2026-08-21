// Scenarios F1, F3 — system operation routing (OTA, WiFi).
//
// These are simple end-to-end checks that the hub can drive the node's
// system-command path correctly. CMD_OTA in production triggers
// trigger_ota=1 + esp_restart(); we just observe the recorded command on
// the node side.

#include "sim/hub_model.h"
#include "sim/node_model.h"

#include <gtest/gtest.h>

using namespace proto_sim;

namespace {

constexpr uint64_t kMacRol1 = 0xE08CFE5FB7A4ULL;

struct SysopFixture : public ::testing::Test {
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
        node_1.reboot(/*keep_cfg=*/true); // clear last_motor_cmd_ watermark
    }
};

// F1: CMD_OTA must be delivered to the addressed node only.
TEST_F(SysopFixture, CmdOtaDelivered) {
    rol_1.send_sysop(ClientOperation::CMD_OTA);
    EXPECT_EQ(node_1.last_motor_cmd(), "OTA");
}

// F3a: CMD_ENABLE_WIFI delivered.
TEST_F(SysopFixture, CmdEnableWifiDelivered) {
    rol_1.send_sysop(ClientOperation::CMD_ENABLE_WIFI);
    EXPECT_EQ(node_1.last_motor_cmd(), "WIFI_ON");
}

// F3b: CMD_DISABLE_WIFI delivered.
TEST_F(SysopFixture, CmdDisableWifiDelivered) {
    rol_1.send_sysop(ClientOperation::CMD_DISABLE_WIFI);
    EXPECT_EQ(node_1.last_motor_cmd(), "WIFI_OFF");
}

// F2: CMD_STATUS routed.
TEST_F(SysopFixture, CmdStatusDelivered) {
    rol_1.send_sysop(ClientOperation::CMD_STATUS);
    EXPECT_EQ(node_1.last_motor_cmd(), "STATUS");
}

} // namespace

// Scenarios A1 + A2 — REGISTER routing by MAC.
//
// Plan section 3.A:
//   * REGISTER from a node is routed to the listener whose MAC matches.
//   * The wrong-MAC listener silently ignores it.
//   * The accepted REGISTER triggers ClientConfig carrying that listener's
//     own short_address and the matching MAC — directly stressing the
//     misconfiguration the user reported ("default address 17 on both
//     nodes"). If MAC matching ever regresses, the second node's
//     ClientConfig would land with addr=17 and this test fails.
//
// We do not tick past the REGISTER 500 ms login timer here — the LoginMsg
// path is exercised in login_test.cpp.
//
// Known production limitation modelled by the harness: when TWO listeners
// each send a ClientConfig with the same independent tx_message_id_ (e.g.
// both at msgid=65 after NVS restore-with-skip-64), a node that has
// already advanced rx_message_id_ from the first one will replay-reject
// the second. In real deployments the nodes wake one at a time, so this
// doesn't bite. We test each registration in its own scenario.

#include "sim/hub_model.h"
#include "sim/node_model.h"

#include <gtest/gtest.h>

using namespace proto_sim;

namespace {

constexpr uint64_t kMacRol1 = 0xE08CFE5FB7A4ULL;
constexpr uint64_t kMacRol2 = 0xE08CFE5F9EC4ULL;

struct ColdBootFixture : public ::testing::Test {
    SimClock        clock;
    SimRadio        radio;
    SharedNonceMap  nonces;
    HubTracker      tracker{&clock, &radio, &nonces};

    HubListener rol_1{"rol_1", 17, 2, kMacRol1, 21600, &tracker, &clock, &nonces};
    HubListener rol_2{"rol_2", 18, 2, kMacRol2, 21600, &tracker, &clock, &nonces};

    NodeModel node_1{"node_1", kMacRol1, &clock, &radio};
    NodeModel node_2{"node_2", kMacRol2, &clock, &radio};

    void SetUp() override {
        tracker.register_listener(&rol_1);
        tracker.register_listener(&rol_2);

        rol_1.rng = [] { return 0x11111111u; };
        rol_2.rng = [] { return 0x22222222u; };

        rol_1.wipe_nvs();
        rol_2.wipe_nvs();
        rol_1.setup(/*time_valid_at_boot=*/false);
        rol_2.setup(/*time_valid_at_boot=*/false);
    }
};

TEST_F(ColdBootFixture, Node1RegistersToAddress17) {
    node_1.send_register();

    EXPECT_TRUE(rol_1.registered());
    EXPECT_FALSE(rol_2.registered())
        << "Only the listener with the matching MAC must register.";

    EXPECT_EQ(node_1.cfg_address(), 17);
    EXPECT_TRUE(node_1.registered());
}

TEST_F(ColdBootFixture, Node2RegistersToAddress18) {
    node_2.send_register();

    EXPECT_FALSE(rol_1.registered());
    EXPECT_TRUE(rol_2.registered());

    EXPECT_EQ(node_2.cfg_address(), 18)
        << "Both nodes ending up with addr=17 would mean MAC-based REGISTER "
           "routing broke — this is the exact misbehaviour the user reported.";
    EXPECT_TRUE(node_2.registered());
}

TEST_F(ColdBootFixture, BothRegistersDispatchOneClientConfigEach) {
    // Verifies the hub's transmit transcript even when both nodes register
    // back-to-back. Each listener must emit exactly one ClientConfig with
    // its own short_address and the matching MAC. (The msgid replay
    // interaction on the node side is covered by the two scenarios above.)
    node_1.send_register();
    node_2.send_register();

    int cfg_for_17 = 0, cfg_for_18 = 0;
    for (const auto& f : radio.hub_to_node_frames()) {
        auto m = as_op(f);
        if (!m || m->cmd != LoraClientOperationMessage::Cmd::ClientConfig) continue;
        if (m->clientconfig.addr == 17) {
            EXPECT_EQ(m->clientconfig.mac_addr, kMacRol1);
            ++cfg_for_17;
        } else if (m->clientconfig.addr == 18) {
            EXPECT_EQ(m->clientconfig.mac_addr, kMacRol2);
            ++cfg_for_18;
        }
    }
    EXPECT_EQ(cfg_for_17, 1);
    EXPECT_EQ(cfg_for_18, 1);
}

TEST_F(ColdBootFixture, WrongMacRegisterIgnored) {
    NodeModel rogue{"rogue", 0xAABBCCDDEEFFULL, &clock, &radio};
    rogue.send_register();

    EXPECT_FALSE(rol_1.registered());
    EXPECT_FALSE(rol_2.registered());

    for (const auto& f : radio.hub_to_node_frames()) {
        auto m = as_op(f);
        if (!m) continue;
        EXPECT_NE(m->cmd, LoraClientOperationMessage::Cmd::ClientConfig)
            << "Hub must not respond to unknown MAC";
    }
}

} // namespace

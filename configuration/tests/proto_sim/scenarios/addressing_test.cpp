// Scenarios D1–D5 — addressing and dispatch isolation.
//
// These are the regression tests for the user's ORIGINAL suspicion:
// "every node is addressed with the same address (e.g. 17)". The cause turned
// out to be a different node-side misconfiguration, but the invariants here
// (cover OPEN destAddress, listener filter by senderaddress, broadcast/0xFF
// acceptance, unconfigured-node accept-only-CLIENTCONFIG/LOGIN) are exactly
// what would break if the dispatch ever regressed.

#include "sim/hub_model.h"
#include "sim/node_model.h"

#include <gtest/gtest.h>

using namespace proto_sim;

namespace {

constexpr uint64_t kMacRol1 = 0xE08CFE5FB7A4ULL;
constexpr uint64_t kMacRol2 = 0xE08CFE5F9EC4ULL;

struct AddressingFixture : public ::testing::Test {
    SimClock        clock;
    SimRadio        radio;
    SharedNonceMap  nonces;
    HubTracker      tracker{&clock, &radio, &nonces};

    HubListener rol_1{"rol_1", 17, 2, kMacRol1, 21600, &tracker, &clock, &nonces};
    HubListener rol_2{"rol_2", 18, 2, kMacRol2, 21600, &tracker, &clock, &nonces};

    NodeModel node_1{"node_1", kMacRol1, &clock, &radio};
    NodeModel node_2{"node_2", kMacRol2, &clock, &radio};

    // Bring both hub listeners + both nodes to a fully-registered steady
    // state so the addressing tests can focus solely on routing.
    void SetUp() override {
        tracker.register_listener(&rol_1);
        tracker.register_listener(&rol_2);
        rol_1.wipe_nvs();
        rol_2.wipe_nvs();
        rol_1.setup(/*time_valid_at_boot=*/false);
        rol_2.setup(/*time_valid_at_boot=*/false);

        // Register one node at a time to avoid the documented msgid-collision
        // window when two listeners independently send ClientConfig with the
        // same tx counter (see register_test.cpp commentary).
        node_1.send_register();
        ASSERT_EQ(node_1.cfg_address(), 17);

        node_2.send_register();
        ASSERT_EQ(node_2.cfg_address(), 18);

        // Reset motor-cmd watermark so each test starts with a clean slate.
        node_1.reboot(/*keep_cfg=*/true);
        node_2.reboot(/*keep_cfg=*/true);
    }
};

// D1: rol_2 OPEN must land on addr 18 only.
TEST_F(AddressingFixture, CoverOpenForRol2RoutedToAddr18Only) {
    rol_2.send_cover_op(CovOperation::CMD_OPEN);

    EXPECT_EQ(node_2.last_motor_cmd(), "OPEN");
    EXPECT_EQ(node_1.last_motor_cmd(), "")
        << "Node with cfg_address=17 must reject a packet destined for 18";

    auto hub_frames = radio.hub_to_node_frames();
    bool found = false;
    for (const auto& f : hub_frames) {
        auto m = as_op(f);
        if (!m || m->cmd != LoraClientOperationMessage::Cmd::Operation) continue;
        if (m->operation.kind != LoraCoverOperation::Kind::Operation) continue;
        if (m->operation.operation != CovOperation::CMD_OPEN) continue;
        EXPECT_EQ(m->header.destAddress, 18u)
            << "rol_2.send_cover_op must put destAddress=18 on the wire — "
               "this is the wire-level guard against the user's original bug.";
        found = true;
    }
    EXPECT_TRUE(found);
}

// D1 sibling: rol_1 OPEN routes to addr 17 only.
TEST_F(AddressingFixture, CoverOpenForRol1RoutedToAddr17Only) {
    rol_1.send_cover_op(CovOperation::CMD_OPEN);

    EXPECT_EQ(node_1.last_motor_cmd(), "OPEN");
    EXPECT_EQ(node_2.last_motor_cmd(), "");
}

// D2: a CoverPosition reply from addr 18 must be accepted only by rol_2.
// rol_1 must reject it on the senderaddress filter.
TEST_F(AddressingFixture, CoverPositionReplyRoutedByListener) {
    // Simulate node_2 publishing a position update.
    node_2.set_cfg_address(18, 2);
    node_2.set_registered(true);
    node_2.send_position(0.42f);

    // rol_2 must have advanced its rx counter and flipped login_acked_.
    // rol_1 must NOT have advanced — proves the cross-listener filter holds.
    EXPECT_GT(rol_2.rx_message_id(), 0u);
    EXPECT_EQ(rol_1.rx_message_id(), 0u)
        << "rol_1's set_response must reject senderAddress=18 (not equal to "
           "its short_address_=17). If this fails, BOTH listeners would race "
           "for state — exactly the cross-talk symptom the user described.";
}

// D3: broadcast destAddress (0xFF) is accepted by all nodes regardless of cfg.
TEST_F(AddressingFixture, BroadcastOperationAcceptedByAll) {
    // Craft a CMD_OPEN with destAddress=0xFF directly via the tracker.
    LoraClientOperationMessage m;
    m.header.destAddress   = 0xFF;
    m.header.destSubnet    = 2;
    m.header.senderAddress = 0xFF;
    m.header.msgId         = 9999;        // > both nodes' rx (=0 after reboot)
    m.cmd                  = LoraClientOperationMessage::Cmd::Operation;
    m.operation.kind       = LoraCoverOperation::Kind::Operation;
    m.operation.operation  = CovOperation::CMD_STOP;
    tracker.send(std::move(m));

    EXPECT_EQ(node_1.last_motor_cmd(), "STOP");
    EXPECT_EQ(node_2.last_motor_cmd(), "STOP");
}

// D5: a node with cfg_address=0 (never received CLIENTCONFIG) must accept
// CLIENTCONFIG and LOGIN regardless of destAddress, but must reject every
// other command targeted at a specific address.
TEST_F(AddressingFixture, UnconfiguredNodeOnlyAcceptsConfigAndLogin) {
    NodeModel fresh{"fresh", 0xDEADBEEFULL, &clock, &radio};
    // No cfg, no registered_=true. cfg_address_ is 0.

    // 1) CMD_OPERATION addressed to 17 — must be ignored.
    LoraClientOperationMessage op_to_17;
    op_to_17.header.destAddress = 17;
    op_to_17.header.senderAddress = 0xFF;
    op_to_17.header.msgId = 1;
    op_to_17.cmd = LoraClientOperationMessage::Cmd::Operation;
    op_to_17.operation.kind = LoraCoverOperation::Kind::Operation;
    op_to_17.operation.operation = CovOperation::CMD_OPEN;
    tracker.send(std::move(op_to_17));
    EXPECT_EQ(fresh.last_motor_cmd(), "")
        << "Unconfigured node must not act on a packet not addressed to it.";

    // 2) CMD_CLIENTCONFIG addressed to 17 BUT carrying fresh's MAC — must apply.
    LoraClientOperationMessage cfg;
    cfg.header.destAddress = 17;       // does not match fresh.cfg_address_=0
    cfg.header.senderAddress = 0xFF;
    cfg.header.msgId = 2;
    cfg.cmd = LoraClientOperationMessage::Cmd::ClientConfig;
    cfg.clientconfig.mac_addr = 0xDEADBEEFULL;
    cfg.clientconfig.addr     = 42;
    cfg.clientconfig.subnt    = 9;
    cfg.clientconfig.name     = "fresh";
    tracker.send(std::move(cfg));

    EXPECT_EQ(fresh.cfg_address(), 42u)
        << "CLIENTCONFIG must bypass the destAddress filter so a fresh node "
           "can be provisioned. Its MAC is the gate.";
    EXPECT_TRUE(fresh.registered());
}

} // namespace

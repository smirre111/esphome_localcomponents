// Scenarios H1, H2, H3 — boundary inputs to the wire decoder.
//
// In production these correspond to:
//   * H1: a 255-byte packet (SX1278 max) must parse cleanly.
//   * H2: truncated bytes ("Could not read protobuf" path in
//         CmdDispatcher.cpp / lora_client.cpp).
//   * H3: random garbage bytes likewise rejected, no crash.

#include "sim/hub_model.h"
#include "sim/node_model.h"
#include "sim/wire_codec.h"

#include <gtest/gtest.h>

#include <random>

using namespace proto_sim;

namespace {

constexpr uint64_t kMacRol2 = 0xE08CFE5F9EC4ULL;

// H1: a maximally-populated ClientConfig (with a long name) packs and
// round-trips correctly, well within the 255-byte SX1278 limit.
TEST(Garbage, LargeClientConfigRoundTrips) {
    LoraClientOperationMessage m;
    m.header.destAddress   = 18;
    m.header.destSubnet    = 2;
    m.header.senderAddress = 0xFF;
    m.header.msgId         = 42;
    m.cmd                  = LoraClientOperationMessage::Cmd::ClientConfig;
    m.clientconfig.mac_addr      = kMacRol2;
    m.clientconfig.addr          = 18;
    m.clientconfig.subnt         = 2;
    m.clientconfig.name          = std::string(200, 'X'); // big name
    m.clientconfig.sleepDuration = 21600;

    auto bytes = serialize_op(m);
    EXPECT_LE(bytes.size(), 255u)
        << "Packed size " << bytes.size() << " must fit in one SX1278 packet";

    auto round = deserialize_op(bytes.data(), bytes.size());
    ASSERT_TRUE(round);
    EXPECT_EQ(round->clientconfig.addr, 18u);
    EXPECT_EQ(round->clientconfig.name.size(), 200u);
    EXPECT_EQ(round->clientconfig.sleepDuration, 21600u);
}

// H2: a truncated packet must return nullopt from deserialize, and the
// node must silently drop it without crashing.
TEST(Garbage, TruncatedPacketRejected) {
    // Build a real CMD_OPEN, then chop the last 5 bytes.
    LoraClientOperationMessage m;
    m.header.destAddress = 17;
    m.header.senderAddress = 0xFF;
    m.header.msgId = 1;
    m.cmd = LoraClientOperationMessage::Cmd::Operation;
    m.operation.kind = LoraCoverOperation::Kind::Operation;
    m.operation.operation = CovOperation::CMD_OPEN;
    auto bytes = serialize_op(m);
    ASSERT_GT(bytes.size(), 6u);
    bytes.resize(bytes.size() - 5);

    auto parsed = deserialize_op(bytes.data(), bytes.size());
    EXPECT_FALSE(parsed) << "Truncated bytes must return std::nullopt from "
                            "the deserializer (production 'Could not read "
                            "protobuf' path).";

    // End-to-end through a node: must not crash, must not act.
    SimClock        clock;
    SimRadio        radio;
    SharedNonceMap  nonces;
    HubTracker      tracker{&clock, &radio, &nonces};
    HubListener rol_1{"rol_1", 17, 2, 0xABCDEF, 21600, &tracker, &clock, &nonces};
    tracker.register_listener(&rol_1);
    NodeModel node_1{"node_1", 0xABCDEF, &clock, &radio};
    node_1.set_cfg_address(17, 2); node_1.set_registered(true);

    AirFrame f{AirFrame::Dir::HubToNode, bytes};
    radio.send(f);
    EXPECT_EQ(node_1.last_motor_cmd(), "")
        << "Truncated packet must not produce any motor action.";
}

// H3: pure random bytes — must not crash, no side effects.
TEST(Garbage, RandomBytesRejected) {
    std::mt19937 rng(0xC0FFEE);
    std::uniform_int_distribution<int> byte_dist(0, 255);

    SimClock        clock;
    SimRadio        radio;
    SharedNonceMap  nonces;
    HubTracker      tracker{&clock, &radio, &nonces};
    HubListener rol_1{"rol_1", 17, 2, 0xABCDEF, 21600, &tracker, &clock, &nonces};
    tracker.register_listener(&rol_1);
    NodeModel node_1{"node_1", 0xABCDEF, &clock, &radio};
    node_1.set_cfg_address(17, 2); node_1.set_registered(true);

    // 200 garbage packets, ranging from 1 to 64 bytes each.
    for (int trial = 0; trial < 200; ++trial) {
        std::uniform_int_distribution<int> len_dist(1, 64);
        size_t len = len_dist(rng);
        std::vector<uint8_t> garbage(len);
        for (auto& b : garbage) b = static_cast<uint8_t>(byte_dist(rng));
        radio.send(AirFrame{AirFrame::Dir::HubToNode, garbage});
        radio.send(AirFrame{AirFrame::Dir::NodeToHub, garbage});
    }

    // Survival check: no crash; node never executed a motor command from
    // garbage; rol_1's registered_ state unchanged.
    EXPECT_EQ(node_1.last_motor_cmd(), "");
    EXPECT_FALSE(rol_1.registered())
        << "Garbage bytes must never push the hub into a registered state.";
}

} // namespace

// Scenarios H4 — msgid wraparound behaviour.
//
// Production uses a strict `msgid > rx_message_id_` check (CmdDispatcher.cpp
// line 1044, lora_client.cpp line 605). If rx ever reaches UINT32_MAX, the
// next msgid (which would wrap to 0) is silently rejected as replay.
//
// In normal operation the login challenge resets counters periodically so
// wraparound is unreachable — but the test pins the conservative behaviour
// so a future "wrap-aware" change is a deliberate decision, not an
// accident.

#include "sim/hub_model.h"
#include "sim/node_model.h"

#include <gtest/gtest.h>

#include <cstdint>
#include <limits>

using namespace proto_sim;

namespace {

constexpr uint64_t kMacRol1 = 0xE08CFE5FB7A4ULL;

AirFrame make_op(uint32_t dest_addr, uint32_t msg_id, CovOperation op) {
    LoraClientOperationMessage m;
    m.header.destAddress   = dest_addr;
    m.header.senderAddress = 0xFF;
    m.header.msgId         = msg_id;
    m.cmd                  = LoraClientOperationMessage::Cmd::Operation;
    m.operation.kind       = LoraCoverOperation::Kind::Operation;
    m.operation.operation  = op;
    return make_op_frame(m);
}

TEST(BoundaryMsgId, WraparoundIsRejectedAsReplay) {
    SimClock        clock;
    SimRadio        radio;
    SharedNonceMap  nonces;
    HubTracker      tracker{&clock, &radio, &nonces};

    HubListener rol_1{"rol_1", 17, 2, kMacRol1, 21600, &tracker, &clock, &nonces};
    tracker.register_listener(&rol_1);

    NodeModel node_1{"node_1", kMacRol1, &clock, &radio};
    node_1.set_cfg_address(17, 2);
    node_1.set_registered(true);

    // Climb close to the cap.
    radio.send(make_op(17, std::numeric_limits<uint32_t>::max() - 1, CovOperation::CMD_OPEN));
    EXPECT_EQ(node_1.last_motor_cmd(), "OPEN");
    EXPECT_EQ(node_1.rx_message_id(), std::numeric_limits<uint32_t>::max() - 1);

    radio.send(make_op(17, std::numeric_limits<uint32_t>::max(), CovOperation::CMD_CLOSE));
    EXPECT_EQ(node_1.last_motor_cmd(), "CLOSE");
    EXPECT_EQ(node_1.rx_message_id(), std::numeric_limits<uint32_t>::max());

    // Wraparound: msgid=0 must be rejected (NOT > UINT32_MAX).
    node_1.reboot(/*keep_cfg=*/true); // clear last_motor_cmd_ watermark
    // reboot() also zeros rx; manually push it back up via one more valid msg.
    // Simpler: build the assertion around the wire effect — node ignores the
    // wrapped packet. Re-arm rx by replaying the cap.
    radio.send(make_op(17, std::numeric_limits<uint32_t>::max(), CovOperation::CMD_STOP));
    EXPECT_EQ(node_1.last_motor_cmd(), "STOP");

    radio.send(make_op(17, 0u, CovOperation::CMD_OPEN));
    EXPECT_EQ(node_1.last_motor_cmd(), "STOP")
        << "Wrapped msgid=0 must NOT overwrite the prior STOP — replay "
           "rejection is the documented behaviour at the uint32 boundary.";
}

// H5 — burst-dedupe + NVS write throttle.
//
// Production sendPacketBurst() in lora_tracker.cpp transmits each packet
// up to 7 times back-to-back to defeat air noise. The receiver's msgid
// replay check is what makes that safe: the first copy advances
// rx_message_id_; the rest are silently dropped.
//
// The NVS write side is then bounded by the same check: lora_client.cpp's
// setRxMessageId() calls save_state_() only on accepted messages, so a
// 7-copy burst produces at most one flash write per unique msgid. This
// test pins that invariant — a 10-copy burst increments nvs_write_count_
// by exactly 1 (the single accepted msg), not 10.

#include "sim/wire_codec.h"

TEST(Boundary, BurstOfIdenticalEncryptedMessagesProducesOneAcceptance) {
    SimClock        clock;
    SimRadio        radio;
    SharedNonceMap  nonces;
    HubTracker      tracker{&clock, &radio, &nonces};
    HubListener rol_2{"rol_2", 18, 2, /*mac=*/0xE08CFE5F9EC4ULL, 21600,
                      &tracker, &clock, &nonces};
    tracker.register_listener(&rol_2);
    NodeModel node_2{"node_2", 0xE08CFE5F9EC4ULL, &clock, &radio};

    // Bring everything to a fully-logged-in state.
    rol_2.wipe_nvs();
    rol_2.setup(/*time_valid_at_boot=*/false);
    node_2.send_register();
    clock.tick(600);
    ASSERT_TRUE(rol_2.login_acked());

    // Reset the NVS write counter so we measure ONLY what the burst does.
    rol_2.reset_nvs_write_count();
    const uint32_t rx_before = rol_2.rx_message_id();

    // Capture one encrypted CoverPosition reply, then replay it 9 more
    // times — same bytes, same msgid. This is exactly what a real
    // 10-copy burst looks like over the air.
    node_2.send_position(0.5f);
    auto burst_frame = radio.node_to_hub_frames().back();
    for (int i = 0; i < 9; ++i) {
        radio.send(burst_frame);
    }

    // First copy was processed; rx advanced by exactly 1 step.
    EXPECT_EQ(rol_2.rx_message_id(), rx_before + 1u);

    // NVS write count for the burst: exactly 1.
    EXPECT_EQ(rol_2.nvs_write_count(), 1u)
        << "10-copy radio burst must produce exactly one NVS write — the "
           "replay filter is what bounds flash wear when production sends "
           "packets 7x for noise tolerance.";
}

} // namespace

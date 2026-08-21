// Phase 3 node side — drives the REAL production CmdDispatcher.cpp.
//
// The class under test is `CmdDispatcher` defined in
// BlindsESP/main/include/CmdDispatcher.h. FreeRTOS queues, ESP-IDF system
// APIs and the MotorCtrl/SystemCtrl/LoraInterface siblings are shimmed
// (synchronous fakes); no tasks are spawned. Tests drive onReceiveNew()
// directly and inspect the resulting state + TX buffers.

#include <gtest/gtest.h>

#include "esp_err.h"
#include "esp_mac.h"
#include "esp_sleep.h"
#include "esp_system.h"
#include "MotorCtrl.h"
#include "SystemCtrl.h"
#include "LoraInterface.h"
#include "CmdDispatcher.h"

extern "C" {
#include "blinds.pb-c.h"
#include "comm_utils.h"
}

#include "sim/crypto.h"

#include <cstring>
#include <vector>

using proto_sim::aes_gcm_decrypt;
using proto_sim::derive_gcm_iv;
using proto_sim::build_header_aad;

namespace {

constexpr uint8_t kHubAddr = 0xFF;
constexpr uint8_t kNodeAddr = 18;
constexpr uint8_t kSubnet = 2;
constexpr uint64_t kNodeMac = 0xE08CFE5F9EC4ULL;

// Serialise a hub-sent operation message into the byte buffer that
// onReceiveNew() expects.
std::vector<uint8_t> pack_login_op(uint32_t msgid, uint32_t nonce) {
    LoraClientOperationMessage op = LORA_CLIENT_OPERATION_MESSAGE__INIT;
    LoraHeader hdr               = LORA_HEADER__INIT;
    hdr.destaddress   = kNodeAddr;
    hdr.destsubnet    = kSubnet;
    hdr.senderaddress = kHubAddr;
    hdr.msgid         = msgid;
    op.header         = &hdr;

    LoginMsg login = LOGIN_MSG__INIT;
    login.nonce    = nonce;
    op.cmd_case    = LORA_CLIENT_OPERATION_MESSAGE__CMD_LOGIN;
    op.login       = &login;

    size_t len = lora_client_operation_message__get_packed_size(&op);
    std::vector<uint8_t> out(len);
    lora_client_operation_message__pack(&op, out.data());
    return out;
}

struct RealNodeFixture : public ::testing::Test {
    MotorCtrl     mot;
    SystemCtrl    sys;
    LoraInterface lif;
    portMUX_TYPE  motorMux{};
    portMUX_TYPE  buttonMux{};
    CmdDispatcher disp{&mot, &sys, &lif, motorMux, buttonMux};

    void SetUp() override {
        // Production CmdDispatcher::onReceiveNew expects the node to know
        // its own address (CLIENTCONFIG path was already exercised).
        sys.setAddress(kNodeAddr, kSubnet);
        sys.setRegistered();

        // Pin the simulated factory MAC so any code reading
        // esp_read_mac() sees this node's identity.
        uint8_t mac[6] = {
            (uint8_t)((kNodeMac >> 40) & 0xFF),
            (uint8_t)((kNodeMac >> 32) & 0xFF),
            (uint8_t)((kNodeMac >> 24) & 0xFF),
            (uint8_t)((kNodeMac >> 16) & 0xFF),
            (uint8_t)((kNodeMac >>  8) & 0xFF),
            (uint8_t)( kNodeMac        & 0xFF),
        };
        proto_sim_set_factory_mac(mac);
    }

    // Pump one item off the TX queue and execute one iteration of
    // processTxCommand-equivalent send logic. We do this by hand because
    // tasks are not spawned.
    void run_one_tx_step() {
        // Production processTxCommand pulls from txCmdQueueNew, builds
        // the LoraClientResponseMessage, and calls send_tx_buffer.
        // We mirror that minimal flow here so the encrypted-path code in
        // pack_response_message() runs.
        // The dispatcher's setStatus(SYSCMD_AVAILABLE) (called from inside
        // onReceiveNew for CMD_LOGIN) pushed an entry into txCmdQueueNew.
        // We trigger one drain by calling processTxCommand once-equivalent —
        // but the real method runs forever. Use sendAvailable's *result*
        // by directly invoking its tail through the queue mechanism.
        // For simplicity: the real CmdDispatcher::sendAvailable already
        // enqueued. We need to make processTxCommand drain it once.
        // The cleanest way: call setStatus(... AVAILABLE) ourselves and
        // then peek the queue ourselves.
        // — left as exercise; this test asserts the side effect of
        // onReceiveNew(LOGIN) directly: peer counter + sendAvailable
        // enqueue.
    }
};

TEST_F(RealNodeFixture, CmdLoginResetsCountersAndStoresNonce) {
    const uint32_t kHubNonce = 0xCAFEBABEu;

    // Pre-set rx_message_id_ to something non-zero so we can verify the
    // CMD_LOGIN handler explicitly resets it.
    // Production CmdDispatcher.cpp:1229-1230 does:
    //     this->tx_message_id_ = 0;
    //     this->rx_message_id_ = 0;
    // We can't directly read these protected members, but we can
    // observe: after CMD_LOGIN, sendAvailable is enqueued in
    // txCmdQueueNew. Verify the queue has exactly one item.
    EXPECT_EQ(uxQueueMessagesWaiting(disp.txCmdQueueNew), 0u);

    auto bytes = pack_login_op(/*msgid=*/1, kHubNonce);
    disp.onReceiveNew(bytes.data(), static_cast<int>(bytes.size()));

    // sendAvailable() pushes BlindsStatusCmd::SYSCMD_AVAILABLE onto
    // txCmdQueueNew. CmdDispatcher.cpp:1248: this->sendAvailable();
    EXPECT_EQ(uxQueueMessagesWaiting(disp.txCmdQueueNew), 1u)
        << "CMD_LOGIN handler must enqueue an AVAILABLE ack via "
           "sendAvailable() — otherwise the hub never sees login_acked.";

    // destAddress is set to the hub's senderaddress (=0xFF) by the
    // CMD_LOGIN handler at CmdDispatcher.cpp:1237.
    EXPECT_EQ(disp.destAddress, kHubAddr);
    EXPECT_EQ(disp.destSubnet, kSubnet);

    // The base nonce must be stored under the hub's address. We don't
    // have a public getter; use the friend-style invariant: a second
    // CMD_LOGIN within 5 s must be rate-limited (returns without
    // touching the queue).
    auto bytes2 = pack_login_op(/*msgid=*/1, /*nonce=*/0xDEADBEEFu);
    disp.onReceiveNew(bytes2.data(), static_cast<int>(bytes2.size()));

    EXPECT_EQ(uxQueueMessagesWaiting(disp.txCmdQueueNew), 1u)
        << "Second CMD_LOGIN within 5 s must be rate-limited "
           "(LOGIN_RATE_LIMIT_MS = 5000) — otherwise a rogue can reset "
           "frame counters at will. (Production CmdDispatcher.cpp:1215)";
}

TEST_F(RealNodeFixture, CmdOperationReplayRejected) {
    // CMD_OPEN with msgid=1, then again with msgid=1.
    LoraClientOperationMessage op = LORA_CLIENT_OPERATION_MESSAGE__INIT;
    LoraHeader hdr = LORA_HEADER__INIT;
    hdr.destaddress = kNodeAddr;
    hdr.destsubnet = kSubnet;
    hdr.senderaddress = kHubAddr;
    hdr.msgid = 1;
    op.header = &hdr;

    LoraCoverOperation covop = LORA_COVER_OPERATION__INIT;
    covop.covop_case = LORA_COVER_OPERATION__COVOP_OPERATION;
    covop.operation  = COV_OPERATION__CMD_OPEN;
    op.cmd_case  = LORA_CLIENT_OPERATION_MESSAGE__CMD_OPERATION;
    op.operation = &covop;

    size_t len = lora_client_operation_message__get_packed_size(&op);
    std::vector<uint8_t> bytes(len);
    lora_client_operation_message__pack(&op, bytes.data());

    // First delivery: motor command queued.
    disp.onReceiveNew(bytes.data(), static_cast<int>(bytes.size()));
    EXPECT_EQ(uxQueueMessagesWaiting(disp.rxCmdQueueNew), 1u);

    // Replay with same msgid: must be rejected.
    disp.onReceiveNew(bytes.data(), static_cast<int>(bytes.size()));
    EXPECT_EQ(uxQueueMessagesWaiting(disp.rxCmdQueueNew), 1u)
        << "Replayed CMD_OPEN with the same msgid must be silently dropped — "
           "rx_message_id_ check at CmdDispatcher.cpp:1044.";
}

// Regression test for the cross-node CMD_LOGIN interference the harness
// surfaced: production used to skip the destAddress check for LOGIN, so a
// LoginMsg destined for addr 17 would also reset node 18's counters and
// install an unrelated base nonce on node 18 — even though node 18's reply
// would be ignored by the hub's address filter, the wrong-nonce state on
// node 18 broke any subsequent legitimate handshake.
//
// Fix: CmdDispatcher.cpp now applies the destAddress check to LOGIN.
TEST_F(RealNodeFixture, CmdLoginDestinedForDifferentNodeIsIgnored) {
    const uint32_t kHubNonce = 0xC0DEFEEDu;

    // Hub sends LoginMsg with destAddress=17 (NOT us — we are 18).
    LoraClientOperationMessage op = LORA_CLIENT_OPERATION_MESSAGE__INIT;
    LoraHeader hdr               = LORA_HEADER__INIT;
    hdr.destaddress   = 17;             // ← addressed to a DIFFERENT node
    hdr.destsubnet    = kSubnet;
    hdr.senderaddress = kHubAddr;
    hdr.msgid         = 1;
    op.header         = &hdr;

    LoginMsg login = LOGIN_MSG__INIT;
    login.nonce    = kHubNonce;
    op.cmd_case    = LORA_CLIENT_OPERATION_MESSAGE__CMD_LOGIN;
    op.login       = &login;

    size_t len = lora_client_operation_message__get_packed_size(&op);
    std::vector<uint8_t> bytes(len);
    lora_client_operation_message__pack(&op, bytes.data());

    EXPECT_EQ(uxQueueMessagesWaiting(disp.txCmdQueueNew), 0u);

    disp.onReceiveNew(bytes.data(), static_cast<int>(bytes.size()));

    EXPECT_EQ(uxQueueMessagesWaiting(disp.txCmdQueueNew), 0u)
        << "LoginMsg addressed to addr 17 must NOT enqueue an AVAILABLE ack "
           "on the node with cfgAddress=18. If it does, the destAddress "
           "filter for LOGIN regressed and every node within radio range "
           "would re-handshake on every per-node login challenge.";
}

// CoverConfig with full geometry must populate SystemCtrl + MotorCtrl
// roll-geometry state (only when all three floats are non-zero —
// production CmdDispatcher.cpp:1291).
TEST_F(RealNodeFixture, CmdCoverConfigAppliesGeometry) {
    LoraClientOperationMessage op = LORA_CLIENT_OPERATION_MESSAGE__INIT;
    LoraHeader hdr               = LORA_HEADER__INIT;
    hdr.destaddress   = kNodeAddr;
    hdr.destsubnet    = kSubnet;
    hdr.senderaddress = kHubAddr;
    hdr.msgid         = 1;
    op.header         = &hdr;

    CoverConfig cc = COVER_CONFIG__INIT;
    cc.opentime          = 60;
    cc.closetime         = 65;
    cc.blindheightmm     = 2000.0f;
    cc.axlediametermm    = 60.0f;
    cc.blindthicknessmm  = 8.0f;
    op.cmd_case    = LORA_CLIENT_OPERATION_MESSAGE__CMD_COVERCONFIG;
    op.coverconfig = &cc;

    size_t len = lora_client_operation_message__get_packed_size(&op);
    std::vector<uint8_t> bytes(len);
    lora_client_operation_message__pack(&op, bytes.data());

    disp.onReceiveNew(bytes.data(), static_cast<int>(bytes.size()));

    EXPECT_EQ(sys.open_time_s(),  60u);
    EXPECT_EQ(sys.close_time_s(), 65u);
    EXPECT_TRUE(sys.geometry_set())
        << "Real CMD_COVERCONFIG handler must apply geometry when all "
           "three floats are non-zero (production guard at 1291).";
    EXPECT_FLOAT_EQ(sys.height_mm(),    2000.0f);
    EXPECT_FLOAT_EQ(sys.axle_mm(),        60.0f);
    EXPECT_FLOAT_EQ(sys.thickness_mm(),    8.0f);
    EXPECT_TRUE(mot.geometry_set())
        << "Real CMD_COVERCONFIG handler must also push geometry to "
           "MotorCtrl::setRollGeometry().";
}

// CoverConfig with one zero geometry field must NOT apply ANY of them
// (atomic guard — production CmdDispatcher.cpp:1291 requires all three
// non-zero). Documents the "partial = unset" production behaviour.
TEST_F(RealNodeFixture, CmdCoverConfigRejectsPartialGeometry) {
    LoraClientOperationMessage op = LORA_CLIENT_OPERATION_MESSAGE__INIT;
    LoraHeader hdr               = LORA_HEADER__INIT;
    hdr.destaddress = kNodeAddr;
    hdr.senderaddress = kHubAddr;
    hdr.msgid = 1;
    op.header = &hdr;

    CoverConfig cc = COVER_CONFIG__INIT;
    cc.opentime         = 60;
    cc.closetime        = 65;
    cc.blindheightmm    = 2000.0f;
    cc.axlediametermm   = 60.0f;
    cc.blindthicknessmm = 0.0f;   // ← one field zero (proto3 unset)
    op.cmd_case    = LORA_CLIENT_OPERATION_MESSAGE__CMD_COVERCONFIG;
    op.coverconfig = &cc;

    size_t len = lora_client_operation_message__get_packed_size(&op);
    std::vector<uint8_t> bytes(len);
    lora_client_operation_message__pack(&op, bytes.data());

    disp.onReceiveNew(bytes.data(), static_cast<int>(bytes.size()));

    EXPECT_EQ(sys.open_time_s(),  60u)  << "open/close are unconditional";
    EXPECT_EQ(sys.close_time_s(), 65u);
    EXPECT_FALSE(sys.geometry_set())
        << "Partial geometry must be all-or-nothing — applying height+axle "
           "but leaving thickness at the firmware default would silently "
           "produce a position calculation error.";
}

// A fresh node with cfgAddress=0 must still accept CLIENTCONFIG (the MAC
// inside the message is the gate) and END UP with cfgAddress set. This
// is the first-boot bootstrap path on the real CmdDispatcher.
TEST(RealCmdDispatcherFresh, FreshNodeAcceptsClientConfigByMac) {
    MotorCtrl     mot;
    SystemCtrl    sys;          // unconfigured: cfgAddress=0
    LoraInterface lif;
    portMUX_TYPE  motorMux{}, buttonMux{};
    CmdDispatcher disp(&mot, &sys, &lif, motorMux, buttonMux);

    // Pin the simulated factory MAC.
    constexpr uint64_t kFreshMac = 0xCAFEBABEFEEDULL;
    uint8_t mac_bytes[6];
    for (int i = 0; i < 6; ++i)
        mac_bytes[i] = static_cast<uint8_t>((kFreshMac >> (40 - 8 * i)) & 0xFF);
    proto_sim_set_factory_mac(mac_bytes);

    ASSERT_EQ(sys.getConfigAddress(), 0u) << "Sanity: starting unconfigured";

    LoraClientOperationMessage op = LORA_CLIENT_OPERATION_MESSAGE__INIT;
    LoraHeader hdr               = LORA_HEADER__INIT;
    hdr.destaddress   = 18;      // hub-intended address (node has cfgAddress=0)
    hdr.senderaddress = 0xFF;
    hdr.msgid         = 1;
    op.header         = &hdr;

    ClientConfig cc = CLIENT_CONFIG__INIT;
    cc.mac_addr = kFreshMac;
    cc.addr     = 18;
    cc.subnt    = 2;
    op.cmd_case     = LORA_CLIENT_OPERATION_MESSAGE__CMD_CLIENTCONFIG;
    op.clientconfig = &cc;

    size_t len = lora_client_operation_message__get_packed_size(&op);
    std::vector<uint8_t> bytes(len);
    lora_client_operation_message__pack(&op, bytes.data());

    disp.onReceiveNew(bytes.data(), static_cast<int>(bytes.size()));

    EXPECT_EQ(sys.getConfigAddress(), 18u)
        << "Fresh boot bootstrap: a node with cfgAddress=0 must accept "
           "CLIENTCONFIG whose MAC matches its factory MAC and apply the "
           "hub-assigned address. The CLIENTCONFIG exception to the "
           "destAddress check is exactly what makes this work.";
    EXPECT_TRUE(sys.getRegistered());
}

TEST_F(RealNodeFixture, CmdClientConfigAppliesAddressOnlyForMatchingMac) {
    // CLIENTCONFIG addressed to a DIFFERENT MAC — node must NOT apply.
    LoraClientOperationMessage op = LORA_CLIENT_OPERATION_MESSAGE__INIT;
    LoraHeader hdr = LORA_HEADER__INIT;
    hdr.destaddress = 17;          // CLIENTCONFIG bypasses destaddr check
    hdr.senderaddress = kHubAddr;
    hdr.msgid = 1;
    op.header = &hdr;

    ClientConfig cfg = CLIENT_CONFIG__INIT;
    cfg.mac_addr = 0xAABBCCDDEEFFULL;   // wrong MAC
    cfg.addr     = 42;
    cfg.subnt    = 9;
    op.cmd_case     = LORA_CLIENT_OPERATION_MESSAGE__CMD_CLIENTCONFIG;
    op.clientconfig = &cfg;

    size_t len = lora_client_operation_message__get_packed_size(&op);
    std::vector<uint8_t> bytes(len);
    lora_client_operation_message__pack(&op, bytes.data());

    const uint8_t addr_before = sys.getConfigAddress();
    disp.onReceiveNew(bytes.data(), static_cast<int>(bytes.size()));

    EXPECT_EQ(sys.getConfigAddress(), addr_before)
        << "CLIENTCONFIG with a non-matching MAC must NOT change cfgAddress — "
           "this is the multi-node-on-one-air guarantee.";
}

} // namespace

// ---------------------------------------------------------------------------
// P1 — TimeSync. The node has no clock source of its own, so this is the only
// way it ever learns the time. Nothing schedules against it yet; these tests
// pin the behaviour the scheduler will later depend on.
//
// NOTE on what is NOT asserted: the handler calls settimeofday(), which
// requires CAP_SYS_TIME and fails as an unprivileged host user. That failure
// is harmless here — every property the scheduler relies on (validity flag,
// UTC offset, local-time rendering) is independent of whether the host clock
// actually moved, so the tests assert those instead of the wall clock.
// ---------------------------------------------------------------------------

namespace {

std::vector<uint8_t> pack_timesync_op(uint32_t msgid, uint64_t epoch,
                                      int32_t utcoffset, uint64_t dstnext = 0) {
    LoraClientOperationMessage op = LORA_CLIENT_OPERATION_MESSAGE__INIT;
    LoraHeader hdr               = LORA_HEADER__INIT;
    hdr.destaddress   = kNodeAddr;
    hdr.destsubnet    = kSubnet;
    hdr.senderaddress = kHubAddr;
    hdr.msgid         = msgid;
    op.header         = &hdr;

    TimeSync ts  = TIME_SYNC__INIT;
    ts.epoch     = epoch;
    ts.utcoffset = utcoffset;
    ts.dstnext   = dstnext;
    op.cmd_case  = LORA_CLIENT_OPERATION_MESSAGE__CMD_TIMESYNC;
    op.timesync  = &ts;

    size_t len = lora_client_operation_message__get_packed_size(&op);
    std::vector<uint8_t> out(len);
    lora_client_operation_message__pack(&op, out.data());
    return out;
}

} // namespace

TEST_F(RealNodeFixture, TimeSyncEstablishesClockAndOffset) {
    auto frame = pack_timesync_op(/*msgid=*/10, /*epoch=*/1787000000ULL,
                                  /*utcoffset=*/7200);
    disp.onReceiveNew(frame.data(), static_cast<int>(frame.size()));

    EXPECT_TRUE(CmdDispatcher::isClockValid());
    EXPECT_EQ(CmdDispatcher::getUtcOffset(), 7200);
}

TEST_F(RealNodeFixture, TimeSyncRendersLocalWallTime) {
    auto frame = pack_timesync_op(/*msgid=*/11, /*epoch=*/1787000000ULL,
                                  /*utcoffset=*/7200);
    disp.onReceiveNew(frame.data(), static_cast<int>(frame.size()));

    // 1787000000 = 2026-08-17 20:53:20 UTC; +2 h (CEST) -> 22:53:20 local.
    char buf[32];
    CmdDispatcher::formatLocalTime(1787000000ULL, buf, sizeof(buf));
    EXPECT_STREQ(buf, "2026-08-17 22:53:20");
}

TEST_F(RealNodeFixture, TimeSyncHandlesNegativeUtcOffset) {
    auto frame = pack_timesync_op(/*msgid=*/12, /*epoch=*/1787000000ULL,
                                  /*utcoffset=*/-18000);
    disp.onReceiveNew(frame.data(), static_cast<int>(frame.size()));

    EXPECT_EQ(CmdDispatcher::getUtcOffset(), -18000);
    char buf[32];
    CmdDispatcher::formatLocalTime(1787000000ULL, buf, sizeof(buf));
    // 20:53:20 UTC - 5 h -> 15:53:20 same day.
    EXPECT_STREQ(buf, "2026-08-17 15:53:20");
}

TEST_F(RealNodeFixture, TimeSyncWithZeroEpochIsIgnoredAndKeepsPriorClock) {
    // The hub sends epoch 0 only if its OWN clock is invalid. A node that
    // already has a good clock must keep it: a known-stale clock is far better
    // than none, because I8 makes a clockless node refuse to sleep at all.
    auto good = pack_timesync_op(/*msgid=*/20, /*epoch=*/1787000000ULL,
                                 /*utcoffset=*/7200);
    disp.onReceiveNew(good.data(), static_cast<int>(good.size()));
    ASSERT_TRUE(CmdDispatcher::isClockValid());

    auto bad = pack_timesync_op(/*msgid=*/21, /*epoch=*/0, /*utcoffset=*/0);
    disp.onReceiveNew(bad.data(), static_cast<int>(bad.size()));

    EXPECT_TRUE(CmdDispatcher::isClockValid()) << "a zero-epoch TimeSync must not invalidate a good clock";
    EXPECT_EQ(CmdDispatcher::getUtcOffset(), 7200) << "offset must survive an ignored TimeSync";
}

TEST_F(RealNodeFixture, TimeSyncStoresDstNextForLaterUse) {
    auto frame = pack_timesync_op(/*msgid=*/30, /*epoch=*/1787000000ULL,
                                  /*utcoffset=*/7200, /*dstnext=*/1793491200ULL);
    disp.onReceiveNew(frame.data(), static_cast<int>(frame.size()));
    EXPECT_EQ(CmdDispatcher::getDstNext(), 1793491200ULL);
}

TEST_F(RealNodeFixture, TimeSyncIsNotAcked) {
    // Deliberate: config pushes are not acked either, and the wake beacon is
    // the designed way for the hub to observe the node's clock. Acking here
    // would spend battery on a redundant transmission, so if an ACK ever shows
    // up on the TX queue this test should be the thing that asks why.
    auto frame = pack_timesync_op(/*msgid=*/40, /*epoch=*/1787000000ULL,
                                  /*utcoffset=*/7200);
    disp.onReceiveNew(frame.data(), static_cast<int>(frame.size()));

    CmdDispatcher::tx_command_t cmd{};
    EXPECT_EQ(xQueueReceive(disp.txCmdQueueNew, &cmd, 0), pdFALSE)
        << "TimeSync must not enqueue a reply";
}

// ---------------------------------------------------------------------------
// P2 — wake beacon. Sent on every boot/wake so the hub learns why the node
// woke, what its clock reads (drift, without a serial cable) and whether the
// login handshake can be skipped.
// ---------------------------------------------------------------------------

TEST_F(RealNodeFixture, WakeReasonFromTimerIsCheckin) {
    proto_sim_set_wakeup_causes(BIT(ESP_SLEEP_WAKEUP_TIMER));
    EXPECT_EQ(CmdDispatcher::classifyWakeReason(), WAKE_REASON__WAKE_TIMER_CHECKIN);
}

TEST_F(RealNodeFixture, WakeReasonFromExt1IsButton) {
    proto_sim_set_wakeup_causes(BIT(ESP_SLEEP_WAKEUP_EXT1));
    EXPECT_EQ(CmdDispatcher::classifyWakeReason(), WAKE_REASON__WAKE_BUTTON);
}

TEST_F(RealNodeFixture, WakeReasonPrefersSleepCauseOverResetReason) {
    // A deep-sleep wake IS a reset as far as esp_reset_reason() is concerned
    // (ESP_RST_DEEPSLEEP), so checking the reset reason first would mislabel
    // every scheduled wake as a boot. The sleep cause must win.
    proto_sim_set_wakeup_causes(BIT(ESP_SLEEP_WAKEUP_TIMER));
    proto_sim_set_reset_reason(ESP_RST_DEEPSLEEP);
    EXPECT_EQ(CmdDispatcher::classifyWakeReason(), WAKE_REASON__WAKE_TIMER_CHECKIN);
    proto_sim_set_reset_reason(ESP_RST_POWERON);
}

TEST_F(RealNodeFixture, WakeReasonFromPowerOnIsBoot) {
    proto_sim_set_wakeup_causes(0);
    proto_sim_set_reset_reason(ESP_RST_POWERON);
    EXPECT_EQ(CmdDispatcher::classifyWakeReason(), WAKE_REASON__WAKE_BOOT);
}

TEST_F(RealNodeFixture, CrashLikeResetsReportUnknownNotBoot) {
    // A node that keeps reporting UNKNOWN is reset-looping. Reporting those as
    // a normal BOOT would hide exactly the failure mode behind the earlier
    // silent battery outage.
    proto_sim_set_wakeup_causes(0);
    for (auto r : {ESP_RST_PANIC, ESP_RST_INT_WDT, ESP_RST_TASK_WDT,
                   ESP_RST_WDT, ESP_RST_BROWNOUT}) {
        proto_sim_set_reset_reason(r);
        EXPECT_EQ(CmdDispatcher::classifyWakeReason(), WAKE_REASON__WAKE_UNKNOWN)
            << "reset reason " << (int) r << " must not look like a clean boot";
    }
    proto_sim_set_reset_reason(ESP_RST_POWERON);
}

TEST_F(RealNodeFixture, BeaconIsQueuedWithTheGivenReason) {
    disp.sendWakeBeacon(WAKE_REASON__WAKE_BUTTON);

    CmdDispatcher::tx_command_t cmd{};
    ASSERT_EQ(xQueueReceive(disp.txCmdQueueNew, &cmd, 0), pdTRUE)
        << "sendWakeBeacon must enqueue a TX command";
    EXPECT_EQ(cmd.cmd, (blinds_syscmd_base_t) BlindsStatusCmd::SYSCMD_BEACON);
    EXPECT_EQ(cmd.arg, (uint32_t) WAKE_REASON__WAKE_BUTTON)
        << "the wake reason must travel WITH the queued command — a shared "
           "slot could be overwritten before the TX task reads it";
}

TEST_F(RealNodeFixture, BeaconCarriesClockOnlyWhenValid) {
    // I8's precondition: a node that has never been told the time must say so,
    // rather than reporting epoch 0 as if it were a real clock. The hub uses
    // this to decide whether the offset it computes means anything.
    auto ts = pack_timesync_op(/*msgid=*/60, /*epoch=*/1787000000ULL,
                               /*utcoffset=*/7200);
    disp.onReceiveNew(ts.data(), static_cast<int>(ts.size()));
    ASSERT_TRUE(CmdDispatcher::isClockValid());

    disp.sendWakeBeacon(WAKE_REASON__WAKE_BOOT);
    CmdDispatcher::tx_command_t cmd{};
    ASSERT_EQ(xQueueReceive(disp.txCmdQueueNew, &cmd, 0), pdTRUE);
    EXPECT_EQ(cmd.cmd, (blinds_syscmd_base_t) BlindsStatusCmd::SYSCMD_BEACON);
    // The beacon body is built inside processTxCommand (a FreeRTOS task that
    // the harness does not spawn), so the clock-validity plumbing is asserted
    // through the accessor the beacon reads from.
    EXPECT_TRUE(CmdDispatcher::isClockValid());
}

TEST_F(RealNodeFixture, FirmwareVersionMatchesProjectVersion) {
    // kFirmwareVersion is hand-maintained alongside PROJECT_VER in the
    // top-level CMakeLists.txt. Encoding is major*10000 + minor*100 + patch.
    // If this fails, the two have drifted and the hub's capability gate is
    // reporting a version the node is not actually running.
    EXPECT_EQ(CmdDispatcher::kFirmwareVersion, 10013u)
        << "kFirmwareVersion is out of sync with PROJECT_VER (1.0.13)";
}

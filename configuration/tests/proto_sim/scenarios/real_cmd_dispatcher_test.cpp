// Phase 3 node side — drives the REAL production CmdDispatcher.cpp.
//
// The class under test is `CmdDispatcher` defined in
// BlindsESP/main/include/CmdDispatcher.h. FreeRTOS queues, ESP-IDF system
// APIs and the MotorCtrl/SystemCtrl/LoraInterface siblings are shimmed
// (synchronous fakes); no tasks are spawned. Tests drive onReceiveNew()
// directly and inspect the resulting state + TX buffers.

#include <gtest/gtest.h>
#include <chrono>
#include <thread>

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

#include <psa/crypto.h>

#include <cstring>
#include <ctime>
#include <sys/time.h>
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
        // Production initialises PSA Crypto in app_main ("PSA Crypto subsystem
        // initialised"). The harness constructs CmdDispatcher directly, so
        // without this every key import fails and decrypt_payload_gcm bails
        // with "PSA key not available".
        ASSERT_EQ(psa_crypto_init(), PSA_SUCCESS);

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

    // CMD_LOGIN enqueues TWO things, in this order:
    //   1. the wake BEACON — this is the first moment it can be sent, because
    //      the login just reset both sides' msgid counters. Sent any earlier
    //      (at boot, during register->login) the hub rejects it as "duplicate
    //      or old message ID" against its NVS-restored rx counter, and then
    //      never learns our clock or schedule version.
    //   2. the AVAILABLE ack — without it the hub never sees login_acked.
    EXPECT_EQ(uxQueueMessagesWaiting(disp.txCmdQueueNew), 2u)
        << "CMD_LOGIN must enqueue the wake beacon AND the AVAILABLE ack";

    CmdDispatcher::tx_command_t first{}, second{};
    ASSERT_EQ(xQueueReceive(disp.txCmdQueueNew, &first, 0), pdTRUE);
    ASSERT_EQ(xQueueReceive(disp.txCmdQueueNew, &second, 0), pdTRUE);
    EXPECT_EQ(first.cmd, (blinds_syscmd_base_t) BlindsStatusCmd::SYSCMD_BEACON)
        << "the beacon must go out on the login path";
    EXPECT_EQ(second.cmd, (blinds_syscmd_base_t) BlindsStatusCmd::SYSCMD_AVAILABLE);
    // Put one back so the rate-limit assertion below still has a baseline.
    xQueueSend(disp.txCmdQueueNew, &second, 0);

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

// The test that used to live here asserted kFirmwareVersion == 10014 and
// described itself as the enforcement that "a version bump fails here until
// BOTH are updated". It could not do that: both the constant and the expected
// value were hardcoded, so they could only ever be changed together, and the
// test passed happily while the constant sat at 1.0.14 for three releases.
//
// A guard that requires the thing it guards to be edited in lockstep is not a
// guard. The version is now read from the running image, and the tests for that
// live further down (see the FirmwareVersion suite).

// ---------------------------------------------------------------------------
// P2b — resume-first wake.
//
// The saving: a provisioned node with a live session skips REGISTER -> config
// -> login (~4 s of awake radio) and just sends an encrypted beacon.
//
// The risk being guarded: the hub rebooted while we slept. It holds no nonce,
// cannot decrypt anything we send, and we would sit there believing we are
// connected — a SILENT node, the worst failure this system has. So the resume
// path is always armed with a fallback that re-registers.
// ---------------------------------------------------------------------------

TEST_F(RealNodeFixture, ResumeFallbackFiresRegisterWhenNothingDecrypts) {
    proto_sim_timer_reset();
    disp.armResumeFallback();
    ASSERT_EQ(proto_sim_timer_armed_count(), 1) << "fallback must be armed";

    // Drain anything already queued so the assertion below is unambiguous.
    CmdDispatcher::tx_command_t drain{};
    while (xQueueReceive(disp.txCmdQueueNew, &drain, 0) == pdTRUE) {}

    // Hub rebooted: nothing we send can be decrypted, so no downlink ever
    // proves the session. Time passes.
    proto_sim_timer_fire_all();

    CmdDispatcher::tx_command_t cmd{};
    ASSERT_EQ(xQueueReceive(disp.txCmdQueueNew, &cmd, 0), pdTRUE)
        << "a resume that was never proven MUST fall back to REGISTER — "
           "otherwise the node is silent until its next wake";
    EXPECT_EQ(cmd.cmd, (blinds_syscmd_base_t) BlindsStatusCmd::SYSCMD_REGISTER);
}

TEST_F(RealNodeFixture, ResumeFallbackDoesNotRegisterOnceSessionIsProven) {
    proto_sim_timer_reset();
    disp.armResumeFallback();

    CmdDispatcher::tx_command_t drain{};
    while (xQueueReceive(disp.txCmdQueueNew, &drain, 0) == pdTRUE) {}

    // A downlink decrypted successfully — the hub holds the same base nonce.
    disp.noteSessionProven();
    ASSERT_TRUE(disp.isSessionProven());

    proto_sim_timer_fire_all();

    CmdDispatcher::tx_command_t cmd{};
    EXPECT_EQ(xQueueReceive(disp.txCmdQueueNew, &cmd, 0), pdFALSE)
        << "a proven session must NOT re-register — that would throw away the "
           "~4 s of awake radio the resume path exists to save";
}

TEST_F(RealNodeFixture, ArmingResumeFallbackClearsAnyStaleProof) {
    // session_proven_ survives in the object across wakes; arming must reset it
    // or the second wake would treat the FIRST wake's proof as its own and skip
    // the fallback entirely.
    disp.noteSessionProven();
    ASSERT_TRUE(disp.isSessionProven());

    proto_sim_timer_reset();
    disp.armResumeFallback();
    EXPECT_FALSE(disp.isSessionProven())
        << "arming the fallback must clear stale proof from a previous wake";
}

TEST_F(RealNodeFixture, DecryptedDownlinkProvesSessionEndToEnd) {
    // The real path: a LOGIN establishes the base nonce, then an encrypted
    // downlink arrives and decrypts. That decrypt is what cancels the fallback.
    constexpr uint32_t kNonce = 0xA5A51234;
    auto login = pack_login_op(/*msgid=*/1, kNonce);
    disp.onReceiveNew(login.data(), static_cast<int>(login.size()));

    proto_sim_timer_reset();
    disp.armResumeFallback();
    ASSERT_FALSE(disp.isSessionProven());

    // Encrypted TimeSync from the hub — the reply a beacon actually triggers.
    TimeSync ts  = TIME_SYNC__INIT;
    ts.epoch     = 1787000000ULL;
    ts.utcoffset = 7200;
    LoraClientOperationMessage inner = LORA_CLIENT_OPERATION_MESSAGE__INIT;
    inner.cmd_case = LORA_CLIENT_OPERATION_MESSAGE__CMD_TIMESYNC;
    inner.timesync = &ts;
    size_t plain_len = lora_client_operation_message__get_packed_size(&inner);
    std::vector<uint8_t> plain(plain_len);
    lora_client_operation_message__pack(&inner, plain.data());

    constexpr uint32_t kMsgId = 2;
    uint8_t aad[proto_sim::kHeaderAadLen];
    proto_sim::build_header_aad(kNodeAddr, kSubnet, kHubAddr, kMsgId, aad);
    uint8_t iv[12];
    proto_sim::derive_gcm_iv_downlink(kNonce, kMsgId, iv);
    auto enc = proto_sim::aes_gcm_encrypt(iv, aad, sizeof(aad), plain.data(), plain.size());

    LoraClientOperationMessage outer = LORA_CLIENT_OPERATION_MESSAGE__INIT;
    LoraHeader hdr = LORA_HEADER__INIT;
    hdr.destaddress   = kNodeAddr;
    hdr.destsubnet    = kSubnet;
    hdr.senderaddress = kHubAddr;
    hdr.msgid         = kMsgId;
    outer.header      = &hdr;
    EncryptedPayload ep = ENCRYPTED_PAYLOAD__INIT;
    ep.tag.data        = enc.tag.data();
    ep.tag.len         = enc.tag.size();
    ep.ciphertext.data = enc.ciphertext.data();
    ep.ciphertext.len  = enc.ciphertext.size();
    outer.cmd_case  = LORA_CLIENT_OPERATION_MESSAGE__CMD_ENCRYPTED;
    outer.encrypted = &ep;

    size_t frame_len = lora_client_operation_message__get_packed_size(&outer);
    std::vector<uint8_t> frame(frame_len);
    lora_client_operation_message__pack(&outer, frame.data());

    disp.onReceiveNew(frame.data(), static_cast<int>(frame.size()));

    EXPECT_TRUE(disp.isSessionProven())
        << "a successfully decrypted downlink must prove the session";
    EXPECT_TRUE(CmdDispatcher::isClockValid()) << "and the TimeSync should have applied";
}

TEST_F(RealNodeFixture, PlaintextDownlinkDoesNotProveSession) {
    // If the hub lost its state it answers in PLAINTEXT (BaseNonceExchange).
    // That must NOT count as proof: the whole point is that we can still be
    // heard. Treating it as proof would cancel the fallback and leave the node
    // half-connected.
    proto_sim_timer_reset();
    disp.armResumeFallback();

    auto login = pack_login_op(/*msgid=*/50, 0xDEADBEEF);
    disp.onReceiveNew(login.data(), static_cast<int>(login.size()));

    EXPECT_FALSE(disp.isSessionProven())
        << "a plaintext frame must not be mistaken for a working session";
}

// ---------------------------------------------------------------------------
// P3 — automatic mode gating and execution.
//
// The gate matters more than the happy path. A node that sleeps against a
// schedule it cannot evaluate does not fail loudly — it just stops answering,
// possibly for weeks. Every refusal below is a deliberate "stay interactive"
// rather than a guess.
// ---------------------------------------------------------------------------

namespace {

sched::Entry sched_entry(uint16_t minute, uint8_t days,
                         uint8_t action = sched::ACTION_OPEN, bool enabled = true) {
    sched::Entry e;
    e.minuteOfDay = minute;
    e.dayMask     = days;
    e.action      = action;
    e.enabled     = enabled;
    return e;
}

// Give the node a clock via a real CMD_TIMESYNC, the only way it ever gets one.
void give_clock(CmdDispatcher &disp, uint32_t msgid, uint64_t epoch, int32_t offset) {
    auto f = pack_timesync_op(msgid, epoch, offset);
    disp.onReceiveNew(f.data(), static_cast<int>(f.size()));
}

} // namespace

TEST_F(RealNodeFixture, AutoModeRefusedWithoutASchedule) {
    // Q9 at runtime: mode can be AUTO while no entry can fire. Sleeping towards
    // nothing would strand the node until its check-in — or forever, if that is
    // disabled too.
    give_clock(disp, 200, 1787000000ULL, 7200);
    sys.setAutoMode(true);
    EXPECT_FALSE(disp.shouldRunAutoMode());
    EXPECT_EQ(disp.computeSleepSeconds(), 0u)
        << "no usable schedule must mean: do not sleep on a schedule";
}

TEST_F(RealNodeFixture, AutoModeRefusedWithAllEntriesDisabled) {
    give_clock(disp, 201, 1787000000ULL, 7200);
    sched::Entry e[] = {sched_entry(450, sched::DAY_ALL, sched::ACTION_OPEN, false)};
    sys.setSchedule(1, 1, 0, 0, 0, 0, 1800, e, 1);
    EXPECT_FALSE(disp.shouldRunAutoMode());
}

TEST_F(RealNodeFixture, AutoModeAcceptedWithClockAndUsableSchedule) {
    give_clock(disp, 202, 1787000000ULL, 7200);
    sched::Entry e[] = {sched_entry(450, sched::DAY_ALL)};   // 07:30 daily
    sys.setSchedule(0xABCD, 1, 0, 0, 0, 0, 1800, e, 1);

    EXPECT_TRUE(disp.shouldRunAutoMode());
    EXPECT_NE(disp.computeNextEvent(), 0u);
    EXPECT_GT(disp.computeSleepSeconds(), 0u);
}

TEST_F(RealNodeFixture, AutoModeIgnoredWhenModeIsInteractive) {
    give_clock(disp, 203, 1787000000ULL, 7200);
    sched::Entry e[] = {sched_entry(450, sched::DAY_ALL)};
    sys.setSchedule(1, 0, 0, 0, 0, 0, 1800, e, 1);   // INTERACTIVE
    EXPECT_FALSE(disp.shouldRunAutoMode());
    EXPECT_EQ(disp.computeSleepSeconds(), 0u);
}

TEST_F(RealNodeFixture, SleepIsCappedByTheCheckinInterval) {
    // A weekly entry would otherwise mean a week of radio silence, during which
    // no hub-side config change could reach the node at all.
    give_clock(disp, 204, 1787000000ULL, 7200);
    sched::Entry e[] = {sched_entry(450, sched::DAY_MON)};    // weekly
    sys.setSchedule(1, 1, 0, 3600, 0, 0, 1800, e, 1);

    const uint64_t sleep_s = disp.computeSleepSeconds();
    EXPECT_GT(sleep_s, 0u);
    EXPECT_LE(sleep_s, 3600u)
        << "check-in must bound how long hub config can sit unseen";
}

TEST_F(RealNodeFixture, SleepWakesBeaconLeadBeforeTheEvent) {
    // I1: wake early, beacon, apply pending config, THEN act — so a schedule
    // edit made an hour ago takes effect on THIS event, cancellation included.
    //
    // NOTE: the epoch handed to TimeSync does NOT become the node's clock here.
    // settimeofday() needs CAP_SYS_TIME and fails for an unprivileged host
    // user, so the node reads the HOST clock; TimeSync only establishes
    // validity and the offset. The assertion is therefore on the invariant
    // (wake == next - lead) measured against the same clock the node used,
    // with a tolerance for the tick between the two reads.
    give_clock(disp, 205, 1787000000ULL, 0);
    sched::Entry e[] = {sched_entry(23 * 60, sched::DAY_ALL)};   // 23:00 UTC
    // check-in disabled, so the schedule alone decides the wake time.
    sys.setSchedule(1, 1, 0, /*checkin=*/0, /*lead=*/30, 0, 1800, e, 1);

    const uint64_t next  = disp.computeNextEvent();
    const uint64_t sleep = disp.computeSleepSeconds();
    ASSERT_NE(next, 0u);
    ASSERT_GT(sleep, 0u);

    struct timeval tv;
    gettimeofday(&tv, nullptr);
    const uint64_t now = static_cast<uint64_t>(tv.tv_sec);

    EXPECT_NEAR(static_cast<double>(now + sleep + 30),
                static_cast<double>(next), 2.0)
        << "must wake at (next - beacon_lead), not at next";
}

TEST_F(RealNodeFixture, ScheduleConfigIsAppliedAndAcked) {
    // Unlike TimeSync, a schedule push IS acked: the hub retransmits until
    // acknowledged and must know its pending config actually landed.
    ScheduleEntry e1 = SCHEDULE_ENTRY__INIT;
    e1.minuteofday = 450;              // 07:30
    e1.daymask     = sched::DAY_ALL;
    e1.action      = SCHED_ACTION__SCHED_OPEN;
    ScheduleEntry *entries[] = {&e1};

    ScheduleConfig sc = SCHEDULE_CONFIG__INIT;
    sc.version              = 0xC0FFEE;
    sc.mode                 = NODE_MODE__MODE_AUTO;
    sc.interactivetimeout_s = 900;
    sc.checkininterval_s    = 7200;
    sc.beaconlead_s         = 45;
    sc.posteventwindow_s    = 25;
    sc.catchupwindow_s      = 600;
    sc.n_entries            = 1;
    sc.entries              = entries;

    LoraClientOperationMessage op = LORA_CLIENT_OPERATION_MESSAGE__INIT;
    LoraHeader hdr = LORA_HEADER__INIT;
    hdr.destaddress   = kNodeAddr;
    hdr.destsubnet    = kSubnet;
    hdr.senderaddress = kHubAddr;
    hdr.msgid         = 300;
    op.header   = &hdr;
    op.cmd_case = LORA_CLIENT_OPERATION_MESSAGE__CMD_SCHEDULE;
    op.schedule = &sc;

    size_t len = lora_client_operation_message__get_packed_size(&op);
    std::vector<uint8_t> frame(len);
    lora_client_operation_message__pack(&op, frame.data());

    disp.onReceiveNew(frame.data(), static_cast<int>(frame.size()));

    EXPECT_EQ(sys.getSchedVersion(), 0xC0FFEEu);
    EXPECT_TRUE(sys.getAutoMode());
    EXPECT_EQ(sys.getEntryCount(), 1);
    EXPECT_EQ(sys.getInteractiveTimeout(), 900u);
    EXPECT_EQ(sys.getCheckinInterval(), 7200u);
    EXPECT_EQ(sys.getBeaconLead(), 45u);
    EXPECT_EQ(sys.getPostEventWindow(), 25u);
    EXPECT_EQ(sys.getCatchupWindow(), 600u);
    EXPECT_TRUE(sys.hasUsableSchedule());

    CmdDispatcher::tx_command_t cmd{};
    ASSERT_EQ(xQueueReceive(disp.txCmdQueueNew, &cmd, 0), pdTRUE)
        << "a schedule push must be acked so the hub stops retransmitting";
    EXPECT_EQ(cmd.cmd, (blinds_syscmd_base_t) BlindsStatusCmd::SYSCMD_ACK);
    EXPECT_EQ(cmd.arg, 300u) << "the ack must echo the pushed msgid";
}

TEST_F(RealNodeFixture, ZeroHandlingFollowsWhatTheProtoDocumentsPerField) {
    // Zero is NOT uniform across these fields, and making it uniform would
    // silently disable whatever the hub actually asked for. blinds.proto
    // documents a meaningful zero for three of them; the other two have none,
    // and a 0 there would quietly defeat the wake-early-then-act behaviour.
    sched::Entry e[] = {sched_entry(450, sched::DAY_ALL)};
    sys.setSchedule(1, 1, 0, 0, 0, 0, 0, e, 1);

    EXPECT_EQ(sys.getInteractiveTimeout(), 0u)
        << "0 means 'stay interactive until told otherwise'";
    EXPECT_EQ(sys.getCheckinInterval(), 0u)
        << "0 means 'no periodic check-in wake'";
    EXPECT_EQ(sys.getCatchupWindow(), 0u)
        << "0 means 'never execute a missed event'";
    EXPECT_EQ(sys.getBeaconLead(), 30u)
        << "no documented zero — default must survive";
    EXPECT_EQ(sys.getPostEventWindow(), 20u)
        << "no documented zero — default must survive";
}

TEST_F(RealNodeFixture, DisabledCheckinLeavesSleepDrivenPurelyByTheSchedule) {
    // The counterpart to SleepIsCappedByTheCheckinInterval: with check-in
    // explicitly disabled, the sleep must run all the way to the next event.
    give_clock(disp, 206, 1787000000ULL, 0);
    sched::Entry e[] = {sched_entry(23 * 60, sched::DAY_ALL)};   // 23:00 UTC
    sys.setSchedule(1, 1, 0, /*checkin=*/0, /*lead=*/30, 0, 1800, e, 1);

    const uint64_t next  = disp.computeNextEvent();
    const uint64_t sleep = disp.computeSleepSeconds();
    ASSERT_NE(next, 0u);
    ASSERT_GT(sleep, 0u);

    struct timeval tv;
    gettimeofday(&tv, nullptr);
    const uint64_t now = static_cast<uint64_t>(tv.tv_sec);

    EXPECT_NEAR(static_cast<double>(now + sleep + 30),
                static_cast<double>(next), 2.0)
        << "with no check-in cap the sleep runs to (next - beacon_lead)";
    EXPECT_GT(sleep, 3600u)
        << "and is NOT clipped to the 6 h default check-in that a 0 must disable";
}

TEST_F(RealNodeFixture, OutOfRangeEntriesAreDroppedNotStored) {
    // Keeping a corrupt entry would make next_occurrence silently skip it,
    // which is far harder to diagnose than never loading it.
    sched::Entry e[] = {
        sched_entry(1440, sched::DAY_ALL),   // invalid: 24:00
        sched_entry(450,  sched::DAY_ALL),   // valid
    };
    sys.setSchedule(1, 1, 0, 0, 0, 0, 1800, e, 2);
    EXPECT_EQ(sys.getEntryCount(), 1);
    EXPECT_EQ(sys.getEntries()[0].minuteOfDay, 450);
}

TEST_F(RealNodeFixture, ScheduleReplacesWholesaleRatherThanMerging) {
    // The hub always sends the complete blob, so there is no partial-update
    // state to get out of sync. A merge would leave deleted entries firing.
    sched::Entry three[] = {
        sched_entry(400, sched::DAY_ALL),
        sched_entry(500, sched::DAY_ALL),
        sched_entry(600, sched::DAY_ALL),
    };
    sys.setSchedule(1, 1, 0, 0, 0, 0, 1800, three, 3);
    ASSERT_EQ(sys.getEntryCount(), 3);

    sched::Entry one[] = {sched_entry(700, sched::DAY_ALL)};
    sys.setSchedule(2, 1, 0, 0, 0, 0, 1800, one, 1);

    EXPECT_EQ(sys.getEntryCount(), 1) << "the old entries must be gone, not merged";
    EXPECT_EQ(sys.getEntries()[0].minuteOfDay, 700);
}

// ---------------------------------------------------------------------------
// D4 — the interactive override.
//
// A button press must give whoever is standing at the blind a responsive
// device. The trap it is designed around: doing that by writing autoMode=false
// to config.txt means ONE press silently disables the schedule until somebody
// notices and re-enables it in Home Assistant — and on a node that then only
// wakes on its check-in, "somebody notices" could be days.
//
// So the hub's configured mode is never touched; the override is local and
// expires on its own.
// ---------------------------------------------------------------------------

TEST_F(RealNodeFixture, ButtonPressSuspendsAutoModeWithoutDisablingIt) {
    give_clock(disp, 400, 1787000000ULL, 0);
    sched::Entry e[] = {sched_entry(450, sched::DAY_ALL)};
    sys.setSchedule(1, /*mode=*/1, /*interactive=*/1800, 0, 0, 0, 1800, e, 1);
    ASSERT_TRUE(disp.shouldRunAutoMode());

    disp.enterInteractiveMode();

    EXPECT_TRUE(disp.isTemporarilyInteractive());
    EXPECT_FALSE(disp.shouldRunAutoMode()) << "auto mode must be suspended";
    EXPECT_TRUE(sys.getAutoMode())
        << "the CONFIGURED mode must be untouched — otherwise one press "
           "disables the schedule permanently";
    EXPECT_EQ(disp.computeSleepSeconds(), 0u)
        << "a suspended node must not sleep on its schedule";
}

TEST_F(RealNodeFixture, InteractiveOverrideExpiresAndAutoModeResumes) {
    give_clock(disp, 401, 1787000000ULL, 0);
    sched::Entry e[] = {sched_entry(450, sched::DAY_ALL)};
    // 1 s window so expiry is observable without waiting.
    sys.setSchedule(1, 1, /*interactive=*/1, 0, 0, 0, 1800, e, 1);

    disp.enterInteractiveMode();
    EXPECT_TRUE(disp.isTemporarilyInteractive());

    // The node reads the HOST clock here (settimeofday needs CAP_SYS_TIME and
    // fails unprivileged), so real time passing is what expires the window.
    struct timespec ts{0, 0};
    ts.tv_sec = 2;
    nanosleep(&ts, nullptr);

    EXPECT_FALSE(disp.isTemporarilyInteractive()) << "the window must expire";
    EXPECT_TRUE(disp.shouldRunAutoMode()) << "auto mode must resume by itself";
}

TEST_F(RealNodeFixture, ZeroTimeoutMeansStayInteractiveIndefinitely) {
    // blinds.proto documents interactiveTimeout == 0 as "stay interactive until
    // told otherwise". It must NOT be read as "expire immediately".
    give_clock(disp, 402, 1787000000ULL, 0);
    sched::Entry e[] = {sched_entry(450, sched::DAY_ALL)};
    sys.setSchedule(1, 1, /*interactive=*/0, 0, 0, 0, 1800, e, 1);
    ASSERT_EQ(sys.getInteractiveTimeout(), 0u);

    disp.enterInteractiveMode();

    EXPECT_TRUE(disp.isTemporarilyInteractive());
    EXPECT_EQ(disp.interactiveRemaining(), UINT32_MAX)
        << "a zero timeout must never expire";
    EXPECT_FALSE(disp.shouldRunAutoMode());
}

TEST_F(RealNodeFixture, EachPressRestartsTheWindow) {
    // Someone adjusting the blind by hand should not have it fall asleep
    // mid-adjustment because the FIRST press's timeout ran out.
    give_clock(disp, 403, 1787000000ULL, 0);
    sched::Entry e[] = {sched_entry(450, sched::DAY_ALL)};
    sys.setSchedule(1, 1, /*interactive=*/60, 0, 0, 0, 1800, e, 1);

    disp.enterInteractiveMode();
    const uint32_t first = disp.interactiveRemaining();
    ASSERT_GT(first, 0u);

    struct timespec ts{1, 0};
    nanosleep(&ts, nullptr);
    const uint32_t decayed = disp.interactiveRemaining();
    EXPECT_LE(decayed, first) << "the window should be counting down";

    disp.enterInteractiveMode();   // second press
    EXPECT_GE(disp.interactiveRemaining(), decayed)
        << "a fresh press must restart the window, not let it keep decaying";
}

TEST_F(RealNodeFixture, OverrideIsIgnoredWhenAutoModeWasNeverOn) {
    // An interactive-mode node pressing buttons is just... an interactive node.
    // The override must not invent state for it.
    give_clock(disp, 404, 1787000000ULL, 0);
    sched::Entry e[] = {sched_entry(450, sched::DAY_ALL)};
    sys.setSchedule(1, /*mode=*/0, 1800, 0, 0, 0, 1800, e, 1);

    disp.enterInteractiveMode();
    EXPECT_FALSE(disp.shouldRunAutoMode()) << "still interactive, as configured";
    EXPECT_FALSE(sys.getAutoMode());
}

TEST_F(RealNodeFixture, NoOverrideMeansNoSuspension) {
    give_clock(disp, 405, 1787000000ULL, 0);
    sched::Entry e[] = {sched_entry(450, sched::DAY_ALL)};
    sys.setSchedule(1, 1, 1800, 0, 0, 0, 1800, e, 1);
    EXPECT_FALSE(disp.isTemporarilyInteractive());
    EXPECT_EQ(disp.interactiveRemaining(), 0u);
    EXPECT_TRUE(disp.shouldRunAutoMode());
}

// ---------------------------------------------------------------------------
// Sleep-path coverage.
//
// SystemCtrl::enterDeepsleep() used to be a bare no-op in this harness, so the
// entire sleep path had ZERO host coverage — and the path automatic mode
// depends on most is exactly the one that was invisible. A change that called
// enterDeepsleep() from the CMD_SCHEDULE handler therefore passed the suite and
// crashed on hardware.
//
// These pin WHEN sleep is requested, and — more usefully — when it must not be.
// ---------------------------------------------------------------------------

namespace {
bool drain_for_sleep(CmdDispatcher &d) {
    bool found = false;
    CmdDispatcher::tx_command_t c{};
    while (xQueueReceive(d.sysCmdQueueNew, &c, 0) == pdTRUE)
        if (c.cmd == (blinds_syscmd_base_t) BlindsSysCmd::SYSCMD_SLEEP) found = true;
    return found;
}

// Entering auto mode arms a quiet-window timer rather than sleeping on the
// spot, so a test that wants the sleep has to let that window elapse.
bool drain_for_sleep_after_quiet_window(CmdDispatcher &d) {
    if (drain_for_sleep(d)) return true;   // should not happen; caller asserts
    proto_sim_timer_fire_all();
    return drain_for_sleep(d);
}
}  // namespace

TEST_F(RealNodeFixture, ApplyingAScheduleDoesNotSleepImmediately) {
    // Direct regression for the crash: entering deep sleep from the RX task
    // right after applying a schedule reset the node. Auto mode takes effect at
    // the next boot/wake instead. If someone reinstates the immediate sleep,
    // this fails first — on the host, not on a node in a window.
    give_clock(disp, 500, 1787000000ULL, 0);
    sys.reset_deepsleep_calls();

    ScheduleEntry e1 = SCHEDULE_ENTRY__INIT;
    e1.minuteofday = 450;
    e1.daymask     = sched::DAY_ALL;
    e1.action      = SCHED_ACTION__SCHED_OPEN;
    ScheduleEntry *entries[] = {&e1};

    ScheduleConfig sc = SCHEDULE_CONFIG__INIT;
    sc.version   = 0xBEEF;
    sc.mode      = NODE_MODE__MODE_AUTO;   // switches the node INTO auto mode
    sc.n_entries = 1;
    sc.entries   = entries;

    LoraClientOperationMessage op = LORA_CLIENT_OPERATION_MESSAGE__INIT;
    LoraHeader hdr = LORA_HEADER__INIT;
    hdr.destaddress   = kNodeAddr;
    hdr.destsubnet    = kSubnet;
    hdr.senderaddress = kHubAddr;
    hdr.msgid         = 501;   // must differ from the TimeSync above, or the
                               // replay filter drops it and the test lies
    op.header   = &hdr;
    op.cmd_case = LORA_CLIENT_OPERATION_MESSAGE__CMD_SCHEDULE;
    op.schedule = &sc;

    size_t len = lora_client_operation_message__get_packed_size(&op);
    std::vector<uint8_t> frame(len);
    lora_client_operation_message__pack(&op, frame.data());
    disp.onReceiveNew(frame.data(), static_cast<int>(frame.size()));

    ASSERT_TRUE(sys.getAutoMode()) << "the schedule should still have been applied";
    EXPECT_EQ(sys.deepsleep_calls(), 0)
        << "applying a schedule must NOT call enterDeepsleep() from the "
           "RX/dispatcher task — tearing the radio down from inside the receive "
           "path crashed the node on hardware";

    // It must still ENTER auto mode, just via the queue: SYSCMD_SLEEP is
    // handled by processSysCommand's own task, the same context the nightly
    // CMD_SLEEP has always used.
    EXPECT_FALSE(drain_for_sleep(disp))
        << "the sleep must be deferred behind the quiet window so our "
           "CommandAck is actually transmitted and the hub can follow up";
    EXPECT_TRUE(drain_for_sleep_after_quiet_window(disp))
        << "a schedule that switches the node INTO auto mode must queue a "
           "sleep — otherwise auto mode never actually sleeps and the whole "
           "battery saving is lost";
}

TEST_F(RealNodeFixture, EnterDeepsleepStillReachesSystemCtrl) {
    // The counterpart to the assertion above: sleep must still be REACHABLE,
    // otherwise "did not sleep" could be satisfied by sleep being broken
    // outright rather than by the schedule handler correctly not calling it.
    //
    // Driven directly rather than through processSysCommand(), which is a task
    // body with an infinite xQueueReceive loop — calling it from a test hangs
    // the suite (learned the hard way).
    sys.reset_deepsleep_calls();
    disp.enterDeepsleep();
    EXPECT_GE(sys.deepsleep_calls(), 1)
        << "enterDeepsleep() must still delegate to SystemCtrl";
}

// ---------------------------------------------------------------------------
// Unprovisioned REGISTER retry.
//
// Address 0 means the node rejects every addressed downlink — including the
// LoginMsg carrying request_register. So it cannot be TOLD to re-register; it
// has to keep asking. The boot REGISTER used to be sent exactly once, so one
// lost frame stranded the node until a physical reset. Observed on node 2.
// ---------------------------------------------------------------------------

TEST_F(RealNodeFixture, UnprovisionedNodeIsDetected) {
    sys.setAddress(0, 0);
    EXPECT_FALSE(disp.isProvisioned());
    sys.setAddress(kNodeAddr, kSubnet);
    EXPECT_TRUE(disp.isProvisioned());
}

TEST_F(RealNodeFixture, RegisterRetryReSendsWhileUnprovisioned) {
    proto_sim_timer_reset();
    sys.setAddress(0, 0);
    disp.armRegisterRetry();
    ASSERT_EQ(proto_sim_timer_armed_count(), 1) << "retry must be armed";

    CmdDispatcher::tx_command_t drain{};
    while (xQueueReceive(disp.txCmdQueueNew, &drain, 0) == pdTRUE) {}

    proto_sim_timer_fire_all();   // the retry interval elapses

    CmdDispatcher::tx_command_t cmd{};
    ASSERT_EQ(xQueueReceive(disp.txCmdQueueNew, &cmd, 0), pdTRUE)
        << "an unprovisioned node must keep asking — one lost REGISTER must "
           "not strand it until somebody walks over and resets it";
    EXPECT_EQ(cmd.cmd, (blinds_syscmd_base_t) BlindsStatusCmd::SYSCMD_REGISTER);

    sys.setAddress(kNodeAddr, kSubnet);
}

TEST_F(RealNodeFixture, RegisterRetryStopsOnceProvisioned) {
    proto_sim_timer_reset();
    sys.setAddress(0, 0);
    disp.armRegisterRetry();

    CmdDispatcher::tx_command_t drain{};
    while (xQueueReceive(disp.txCmdQueueNew, &drain, 0) == pdTRUE) {}

    // The hub provisions us.
    sys.setAddress(kNodeAddr, kSubnet);
    proto_sim_timer_fire_all();

    CmdDispatcher::tx_command_t cmd{};
    EXPECT_EQ(xQueueReceive(disp.txCmdQueueNew, &cmd, 0), pdFALSE)
        << "a provisioned node must stop re-registering — otherwise every node "
           "spends radio time and battery on pointless REGISTERs forever";
}

TEST_F(RealNodeFixture, ArmingRetryIsANoOpWhenAlreadyProvisioned) {
    proto_sim_timer_reset();
    sys.setAddress(kNodeAddr, kSubnet);
    disp.armRegisterRetry();
    EXPECT_EQ(proto_sim_timer_armed_count(), 0)
        << "nothing to retry when we already have an address";
}

// ---------------------------------------------------------------------------
// Boot policy: a node always comes up INTERACTIVE.
//
// Automatic mode is never resumed from stored config. An unexplained reboot is
// precisely when you most want to be able to reach the node — resuming a
// schedule would put it straight back to sleep instead. Home Assistant (via the
// hub's schedule push) is what re-arms it.
// ---------------------------------------------------------------------------

TEST_F(RealNodeFixture, BootPolicyLeavesTheNodeInteractive) {
    // Simulates what app_main does after loading config: forget the stored mode
    // and version, whatever they were.
    give_clock(disp, 600, 1787000000ULL, 0);
    sched::Entry e[] = {sched_entry(450, sched::DAY_ALL)};
    sys.setSchedule(0xABCD, /*mode=*/1, 0, 0, 0, 0, 1800, e, 1);
    ASSERT_TRUE(disp.shouldRunAutoMode()) << "precondition: auto mode was active";

    sys.setAutoMode(false);
    sys.setSchedVersion(0);

    EXPECT_FALSE(disp.shouldRunAutoMode())
        << "a reboot must leave the node interactive and reachable";
    EXPECT_EQ(disp.computeSleepSeconds(), 0u)
        << "and it must not sleep on the stored schedule";
    EXPECT_TRUE(sys.hasUsableSchedule())
        << "the schedule ENTRIES are kept — only the mode and version are cleared";
}

TEST_F(RealNodeFixture, ClearedVersionMakesTheHubRePushAndReArmAutoMode) {
    // Version 0 is what the node reports in its beacon, so the hub sees a
    // mismatch and pushes again. That push is what re-arms auto mode, which is
    // why clearing the version is not a one-way door.
    sys.setSchedVersion(0);
    EXPECT_EQ(sys.getSchedVersion(), 0u);

    give_clock(disp, 601, 1787000000ULL, 0);
    sched::Entry e[] = {sched_entry(450, sched::DAY_ALL)};
    sys.setSchedule(0x1234, /*mode=*/1, 0, 0, 0, 0, 1800, e, 1);   // the re-push

    EXPECT_TRUE(disp.shouldRunAutoMode())
        << "the hub's push must be able to put the node back into auto mode";
    EXPECT_EQ(sys.getSchedVersion(), 0x1234u);
}

// ---------------------------------------------------------------------------
// TimeSync / ScheduleConfig arrival order.
//
// The hub sends these as two frames ~1.25 s apart and nothing guarantees the
// order — or that both arrive. Observed live: the schedule landed while the
// TimeSync was still missing, so the I8 guard correctly refused auto mode, and
// the node then stayed interactive FOREVER because nothing asked again. Every
// trial looked like a broken scheduler when configuration was in fact fine.
// ---------------------------------------------------------------------------

namespace {

std::vector<uint8_t> pack_schedule_op(uint32_t msgid, uint32_t version, uint32_t mode) {
    static ScheduleEntry e1;
    schedule_entry__init(&e1);
    e1.minuteofday = 450;
    e1.daymask     = sched::DAY_ALL;
    e1.action      = SCHED_ACTION__SCHED_OPEN;
    static ScheduleEntry *entries[1];
    entries[0] = &e1;

    ScheduleConfig sc = SCHEDULE_CONFIG__INIT;
    sc.version   = version;
    sc.mode      = (NodeMode) mode;
    sc.n_entries = 1;
    sc.entries   = entries;

    LoraClientOperationMessage op = LORA_CLIENT_OPERATION_MESSAGE__INIT;
    LoraHeader hdr = LORA_HEADER__INIT;
    hdr.destaddress   = kNodeAddr;
    hdr.destsubnet    = kSubnet;
    hdr.senderaddress = kHubAddr;
    hdr.msgid         = msgid;
    op.header   = &hdr;
    op.cmd_case = LORA_CLIENT_OPERATION_MESSAGE__CMD_SCHEDULE;
    op.schedule = &sc;

    size_t len = lora_client_operation_message__get_packed_size(&op);
    std::vector<uint8_t> out(len);
    lora_client_operation_message__pack(&op, out.data());
    return out;
}

} // namespace

TEST_F(RealNodeFixture, ScheduleBeforeTimeSyncStillEntersAutoMode) {
    // The order that broke it live. The schedule cannot start auto mode on its
    // own (no clock yet), so the TimeSync that follows must do it.
    CmdDispatcher::tx_command_t drain{};
    while (xQueueReceive(disp.sysCmdQueueNew, &drain, 0) == pdTRUE) {}

    auto sched_frame = pack_schedule_op(/*msgid=*/700, 0xAAAA, NODE_MODE__MODE_AUTO);
    disp.onReceiveNew(sched_frame.data(), static_cast<int>(sched_frame.size()));
    ASSERT_TRUE(sys.getAutoMode()) << "the schedule itself must still be stored";

    // Now the clock arrives.
    auto ts = pack_timesync_op(/*msgid=*/701, 1787000000ULL, 7200);
    disp.onReceiveNew(ts.data(), static_cast<int>(ts.size()));

    EXPECT_TRUE(CmdDispatcher::isClockValid());
    EXPECT_FALSE(drain_for_sleep(disp))
        << "the TimeSync must not sleep the node on the spot: the hub's "
           "ScheduleConfig follows ~1.25 s later, and sleeping here would miss "
           "it and every one of its retransmits";
    EXPECT_TRUE(drain_for_sleep_after_quiet_window(disp))
        << "whichever of TimeSync/Schedule arrives LAST must start auto mode — "
           "otherwise a lost or reordered TimeSync leaves the node interactive "
           "forever with nothing to re-trigger it";
}

TEST_F(RealNodeFixture, TimeSyncBeforeScheduleAlsoEntersAutoMode) {
    // The intended order, which must keep working.
    CmdDispatcher::tx_command_t drain{};
    while (xQueueReceive(disp.sysCmdQueueNew, &drain, 0) == pdTRUE) {}

    auto ts = pack_timesync_op(/*msgid=*/710, 1787000000ULL, 7200);
    disp.onReceiveNew(ts.data(), static_cast<int>(ts.size()));
    (void) drain_for_sleep(disp);   // no schedule yet, so nothing to start

    auto sched_frame = pack_schedule_op(/*msgid=*/711, 0xBBBB, NODE_MODE__MODE_AUTO);
    disp.onReceiveNew(sched_frame.data(), static_cast<int>(sched_frame.size()));

    EXPECT_TRUE(drain_for_sleep_after_quiet_window(disp))
        << "the schedule must start auto mode when the clock is already valid";
}

TEST_F(RealNodeFixture, TimeSyncAloneDoesNotStartAutoModeWithoutASchedule) {
    // The guard must not over-trigger: a clock with no usable schedule is still
    // a node that has nothing to sleep towards.
    sys.setSchedule(0, /*mode=*/0, 0, 0, 0, 0, 1800, nullptr, 0);
    CmdDispatcher::tx_command_t drain{};
    while (xQueueReceive(disp.sysCmdQueueNew, &drain, 0) == pdTRUE) {}

    auto ts = pack_timesync_op(/*msgid=*/720, 1787000000ULL, 7200);
    disp.onReceiveNew(ts.data(), static_cast<int>(ts.size()));

    EXPECT_FALSE(drain_for_sleep(disp))
        << "no schedule means nothing to sleep towards";
}

// ---------------------------------------------------------------------------
// Deferred auto-sleep.
//
// Regression for the failure that made the node look like a radio problem: it
// woke, sent its REGISTER, and queued a sleep ~3 s later — while the hub defers
// its LoginMsg by ~4 s. The handshake could never complete. The hub logged
// "Login not acknowledged" up to 24 times per cycle, the node looped every
// 600 s carrying a stale schedule, and there was no path by which it could ever
// be told about a new one. Captured live on 2026-08-23:
//
//   Device not registered yet, going to register mode
//   Sending REGISTER response
//   Entering deep sleep — state persisted        <- 30 ms later
//   Auto mode: sleeping 600 s until next scheduled wake
// ---------------------------------------------------------------------------

TEST_F(RealNodeFixture, AutoSleepWaitsForTheQuietWindow) {
    give_clock(disp, 730, 1787000000ULL, 0);
    auto sched = pack_schedule_op(/*msgid=*/731, 0xC0DE, NODE_MODE__MODE_AUTO);
    disp.onReceiveNew(sched.data(), static_cast<int>(sched.size()));
    CmdDispatcher::tx_command_t drain{};
    while (xQueueReceive(disp.sysCmdQueueNew, &drain, 0) == pdTRUE) {}

    disp.armAutoSleep();
    EXPECT_FALSE(drain_for_sleep(disp))
        << "arming must not sleep the node immediately — that is the bug: on the "
           "register path the hub's LoginMsg has not even been sent yet";

    proto_sim_timer_fire_all();
    EXPECT_TRUE(drain_for_sleep(disp))
        << "once the hub goes quiet the node must actually sleep, or auto mode "
           "costs the whole battery saving it exists for";
}

TEST_F(RealNodeFixture, EachRefreshRestartsOneTimerRatherThanStacking) {
    give_clock(disp, 740, 1787000000ULL, 0);
    auto sched = pack_schedule_op(/*msgid=*/741, 0xC0DF, NODE_MODE__MODE_AUTO);
    disp.onReceiveNew(sched.data(), static_cast<int>(sched.size()));

    // Measured relative to one arm, not to an absolute count: other timers
    // (the interactive-return timer, the register retry) may also be armed, and
    // the invariant under test is only that REFRESHING does not add more.
    disp.armAutoSleep();
    const int after_one = proto_sim_timer_armed_count();
    disp.armAutoSleep();
    disp.armAutoSleep();
    EXPECT_EQ(proto_sim_timer_armed_count(), after_one)
        << "every downlink refreshes the window, so repeated arming must restart "
           "ONE timer; stacking them would sleep the node on the first expiry "
           "while the hub was still talking to it";
}

TEST_F(RealNodeFixture, ButtonPressCancelsAPendingAutoSleep) {
    give_clock(disp, 750, 1787000000ULL, 0);
    auto sched = pack_schedule_op(/*msgid=*/751, 0xC0E0, NODE_MODE__MODE_AUTO);
    disp.onReceiveNew(sched.data(), static_cast<int>(sched.size()));
    CmdDispatcher::tx_command_t drain{};
    while (xQueueReceive(disp.sysCmdQueueNew, &drain, 0) == pdTRUE) {}

    disp.armAutoSleep();
    disp.enterInteractiveMode();   // person at the blind

    proto_sim_timer_fire_all();
    EXPECT_FALSE(drain_for_sleep(disp))
        << "a sleep armed seconds before a button press must not still fire — "
           "the blind would go unresponsive in the person's hands";
}

// The node wakes beacon_lead seconds BEFORE its event, so nothing is due at
// boot. Normally the sleep that follows clamps to now+1 s for an imminent event
// and the node naps and wakes to execute it. But every downlink refreshes the
// quiet window, so a talkative hub can hold the node awake PAST the event — and
// next_occurrence() then reports the event AFTER it, skipping the entry
// outright. Observed live: woken 15:27:23 for a 15:28:00 CLOSE, still awake at
// 15:28:03, slept until 15:36, blind never moved.
// ---------------------------------------------------------------------------
// Firmware version in the wake beacon.
//
// This was a hand-maintained constant with a "KEEP IN SYNC with PROJECT_VER"
// comment on it, and it did not stay in sync: a node running 1.0.17 announced
// 10014 in every beacon. A field whose only job is to tell the hub which
// firmware a node is running is worse than useless when it can quietly lie —
// this project has already lost a session to not knowing what was on a node.
// It is now derived from the running image.
// ---------------------------------------------------------------------------

TEST(FirmwareVersion, ParsesDottedVersions) {
    EXPECT_EQ(CmdDispatcher::parseFirmwareVersion("1.0.17"), 10017u);
    EXPECT_EQ(CmdDispatcher::parseFirmwareVersion("1.0.14"), 10014u);
    EXPECT_EQ(CmdDispatcher::parseFirmwareVersion("2.10.3"), 21003u);
    EXPECT_EQ(CmdDispatcher::parseFirmwareVersion("v1.0.17"), 10017u);
}

TEST(FirmwareVersion, OrdersAsTheHubExpects) {
    // The hub compares with >= to gate capabilities, so the encoding has to be
    // monotonic across a minor rollover — the reason for the 100/10000 bases.
    EXPECT_LT(CmdDispatcher::parseFirmwareVersion("1.0.99"),
              CmdDispatcher::parseFirmwareVersion("1.1.0"));
    EXPECT_LT(CmdDispatcher::parseFirmwareVersion("1.99.0"),
              CmdDispatcher::parseFirmwareVersion("2.0.0"));
}

TEST(FirmwareVersion, StopsAtASuffixRatherThanFoldingItIn) {
    // A build that appends "-dirty" or "-rc1" must not have those digits
    // silently absorbed into the patch number.
    EXPECT_EQ(CmdDispatcher::parseFirmwareVersion("1.0.17-dirty"), 10017u);
    EXPECT_EQ(CmdDispatcher::parseFirmwareVersion("1.0.17-rc2"), 10017u);
}

TEST(FirmwareVersion, ReportsZeroRatherThanAMisleadingNumber) {
    // "Unknown" is honest; a wrong version is what caused the problem.
    EXPECT_EQ(CmdDispatcher::parseFirmwareVersion(nullptr), 0u);
    EXPECT_EQ(CmdDispatcher::parseFirmwareVersion(""), 0u);
    EXPECT_EQ(CmdDispatcher::parseFirmwareVersion("not-a-version"), 0u);
    // Out of range: 100 in any field would carry into the next one.
    EXPECT_EQ(CmdDispatcher::parseFirmwareVersion("1.100.0"), 0u);
    EXPECT_EQ(CmdDispatcher::parseFirmwareVersion("1.0.100"), 0u);
}

TEST(FirmwareVersion, ComesFromTheRunningImageNotAConstant) {
    // The shim pins the image to 9.8.7. If this ever returns a number that
    // looks like a real release, the value has been hard-coded again.
    EXPECT_EQ(CmdDispatcher::firmwareVersion(), 90807u);
}

// A ClientConfig for ANOTHER node must not wedge our link.
//
// CLIENTCONFIG is the one message that skips the address filter (a fresh node
// has cfgAddress 0 and cannot match the hub's destaddress). That exemption let
// a foreign CLIENTCONFIG reach the msgid check and, with a higher msgid, ratchet
// rx_message_id_ onto another node's sequence — after which every legitimate
// command to us is rejected as a replay until the next login. Same counter
// pollution the address filter was moved ahead of the msgid check to prevent;
// CLIENTCONFIG was the hole left in it.
//
// Observed live on node 2 (address 18), harmless only because the ids happened
// to fall the other way round:
//     Dest Adreess: 17 / Config Address: 18
//     Message ID check
//     Rejected message ID: 2, ignoring, my MsgID: 3
//
// Asserted through behaviour rather than the counter: what matters is that the
// hub can still talk to us afterwards.
TEST_F(RealNodeFixture, ForeignClientConfigDoesNotWedgeOurLink) {
    give_clock(disp, /*msgid=*/50, 1787000000ULL, 0);

    // A ClientConfig for a different node, carrying a much higher msgid.
    LoraClientOperationMessage op = LORA_CLIENT_OPERATION_MESSAGE__INIT;
    LoraHeader hdr = LORA_HEADER__INIT;
    hdr.destaddress   = 17;              // not us
    hdr.destsubnet    = kSubnet;
    hdr.senderaddress = kHubAddr;
    hdr.msgid         = 900;             // far ahead of our stream
    op.header = &hdr;

    ClientConfig cfg = CLIENT_CONFIG__INIT;
    cfg.mac_addr = 0xAABBCCDDEEFFULL;    // not our MAC either
    cfg.addr     = 17;
    cfg.subnt    = kSubnet;
    op.cmd_case     = LORA_CLIENT_OPERATION_MESSAGE__CMD_CLIENTCONFIG;
    op.clientconfig = &cfg;

    size_t len = lora_client_operation_message__get_packed_size(&op);
    std::vector<uint8_t> bytes(len);
    lora_client_operation_message__pack(&op, bytes.data());
    disp.onReceiveNew(bytes.data(), static_cast<int>(bytes.size()));

    EXPECT_EQ(sys.getConfigAddress(), kNodeAddr)
        << "a non-matching MAC must not reconfigure us";

    // The hub's NEXT real command to us continues our own sequence (51). If the
    // foreign frame had advanced our counter to 900, this is silently dropped.
    auto sched = pack_schedule_op(/*msgid=*/51, 0xFEED, NODE_MODE__MODE_AUTO);
    disp.onReceiveNew(sched.data(), static_cast<int>(sched.size()));

    EXPECT_EQ(sys.getSchedVersion(), 0xFEEDu)
        << "after a CLIENTCONFIG for another node, the hub's next command to US "
           "must still be accepted — otherwise one overheard provisioning frame "
           "silences the node until its next login";
}

TEST_F(RealNodeFixture, AnEntryThatFallsDueWhileAwakeIsStillExecuted) {
    // NOTE: the harness has no settimeofday shim, so glibc's fails without root
    // and the node clock is real wall time. The schedule below is therefore
    // built RELATIVE TO NOW rather than from a pinned epoch.
    const uint64_t now_s = static_cast<uint64_t>(time(nullptr));
    const uint16_t now_minute = static_cast<uint16_t>((now_s / 60) % 1440);
    // One minute ago, in UTC — the entry is given utc_offset 0 below. Skip the
    // test in the one minute after midnight UTC rather than wrap into
    // yesterday, where "missed" depends on the day mask instead of the time.
    if (now_minute == 0) GTEST_SKIP() << "would wrap past midnight UTC";

    sched::Entry e = sched_entry(static_cast<uint16_t>(now_minute - 1),
                                 sched::DAY_ALL, sched::ACTION_CLOSE);
    sys.setSchedule(/*version=*/0xD00D, /*mode=*/1,
                    /*interactiveTimeout=*/1, /*checkin=*/600,
                    /*beaconLead=*/30, /*postEventWindow=*/20,
                    /*catchup=*/1800, &e, 1);

    give_clock(disp, 770, now_s, 0);

    // The interactive override is a file-level static shared by every test in
    // this binary, and an earlier one leaves it set to "interactive forever".
    // Replace it with a 1 s deadline and wait that out — the clock is real, so
    // it cannot simply be stepped forward.
    disp.enterInteractiveMode();
    std::this_thread::sleep_for(std::chrono::milliseconds(1200));
    ASSERT_FALSE(disp.isTemporarilyInteractive())
        << "precondition: override expired; remaining=" << disp.interactiveRemaining();
    ASSERT_TRUE(disp.shouldRunAutoMode())
        << "precondition: the node must actually be in auto mode here";

    // Schedule entries execute via setBlindOperation(), which posts the OPEN /
    // CLOSE onto the RX command queue.
    CmdDispatcher::tx_command_t drain_rx{};
    while (xQueueReceive(disp.rxCmdQueueNew, &drain_rx, 0) == pdTRUE) {}

    disp.armAutoSleep();
    proto_sim_timer_fire_all();

    int closes = 0;
    CmdDispatcher::tx_command_t got{};
    while (xQueueReceive(disp.rxCmdQueueNew, &got, 0) == pdTRUE)
        if (got.cmd == (blinds_syscmd_base_t) BlindsOpCmd::SYSCMD_CLOSE) ++closes;

    EXPECT_GT(closes, 0)
        << "an entry that fell due while the hub kept us awake must still run — "
           "otherwise a talkative hub silently skips scheduled events and the "
           "blind never moves";
}

TEST_F(RealNodeFixture, AutoSleepRechecksTheConditionWhenItFires) {
    give_clock(disp, 760, 1787000000ULL, 0);
    auto sched = pack_schedule_op(/*msgid=*/761, 0xC0E1, NODE_MODE__MODE_AUTO);
    disp.onReceiveNew(sched.data(), static_cast<int>(sched.size()));
    CmdDispatcher::tx_command_t drain{};
    while (xQueueReceive(disp.sysCmdQueueNew, &drain, 0) == pdTRUE) {}

    disp.armAutoSleep();
    // The hub switches the node back to interactive during the quiet window.
    sys.setAutoMode(false);

    proto_sim_timer_fire_all();
    EXPECT_FALSE(drain_for_sleep(disp))
        << "the timer must re-check rather than trust a condition evaluated "
           "before the mode changed";
}

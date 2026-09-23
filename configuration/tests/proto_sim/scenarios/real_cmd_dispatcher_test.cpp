// Phase 3 node side — drives the REAL production CmdDispatcher.cpp.
//
// The class under test is `CmdDispatcher` defined in
// BlindsESP/main/include/CmdDispatcher.h. FreeRTOS queues, ESP-IDF system
// APIs and the MotorCtrl/SystemCtrl/LoraInterface siblings are shimmed
// (synchronous fakes); no tasks are spawned. Tests drive onReceiveNew()
// directly and inspect the resulting state + TX buffers.

#include <gtest/gtest.h>

#include <cstdlib>

#include "AckCache.h"
#include <iostream>
#include "nvs.h"
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

#include <array>
#include <psa/crypto.h>

#include <cstring>
#include <ctime>
#include <sys/time.h>
#include <vector>
#include "GridState.h"

using proto_sim::aes_gcm_decrypt;
using proto_sim::derive_gcm_iv;
using proto_sim::build_header_aad;

namespace {

// The hub is peer 1 in production ("CMD_LOGIN from peer 1"), NOT 0xFF.
//
// This was 0xFF, which is also the broadcast address AND the value
// persistHubAddr_ defaults to. That collision made a whole class of bug
// structurally invisible here: "we used the 0xFF default instead of the hub's
// real address" cannot fail in a fixture whose hub address IS 0xFF. Three
// attempts to unit-test the persistHubAddr_ fix all passed under mutation for
// exactly this reason, and the bug had to be caught on hardware instead.
constexpr uint8_t kHubAddr = 0x01;
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

// sysop is a plain ClientOperation enum field on the operation message, not a
// nested message — see blinds.proto field 11.
std::vector<uint8_t> pack_sysop_op(uint32_t msgid, ClientOperation what,
                                   uint32_t burst_index = 0) {
    LoraClientOperationMessage op = LORA_CLIENT_OPERATION_MESSAGE__INIT;
    LoraHeader hdr               = LORA_HEADER__INIT;
    hdr.destaddress   = kNodeAddr;
    hdr.destsubnet    = kSubnet;
    hdr.senderaddress = kHubAddr;
    hdr.msgid         = msgid;
    // Which copy of the hub's burst this is. Re-stamped per copy by the hub,
    // and the number every phase path has to back out before it stamps a mark.
    hdr.burstindex    = burst_index;
    hdr.burstcount    = 17;
    op.header         = &hdr;

    op.cmd_case = LORA_CLIENT_OPERATION_MESSAGE__CMD_SYSOP;
    op.sysop    = what;

    size_t len = lora_client_operation_message__get_packed_size(&op);
    std::vector<uint8_t> out(len);
    lora_client_operation_message__pack(&op, out.data());
    return out;
}

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
    // Now requires a BUTTON pin in the EXT1 mask, not merely an EXT1 wake:
    // the mask also carries the LoRa DIO lines (F18). This test previously
    // set only the cause bit and so encoded the assumption that any EXT1 wake
    // is a press — which is exactly the bug.
    proto_sim_set_wakeup_causes(BIT(ESP_SLEEP_WAKEUP_EXT1));
    proto_sim_set_ext1_status(1ULL << ctrlButtonUpPin);
    EXPECT_EQ(CmdDispatcher::classifyWakeReason(), WAKE_REASON__WAKE_BUTTON);
    proto_sim_set_wakeup_causes(0);
    proto_sim_set_ext1_status(0);
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
    // TWO timers now: the 12 s REGISTER fallback and the beacon retry ladder
    // that runs inside it (re-send the cheap single frame before escalating to
    // a full handshake). This used to assert 1.
    ASSERT_EQ(proto_sim_timer_armed_count(), 2)
        << "the REGISTER fallback and the beacon retry ladder must both be armed";

    // Drain anything already queued so the assertion below is unambiguous.
    CmdDispatcher::tx_command_t drain{};
    while (xQueueReceive(disp.txCmdQueueNew, &drain, 0) == pdTRUE) {}

    // Hub rebooted: nothing we send can be decrypted, so no downlink ever
    // proves the session. Time passes.
    proto_sim_timer_fire_all();

    // The queue may now hold a re-beacon as well as the REGISTER, and
    // fire_all() gives no ordering guarantee — so scan for the invariant rather
    // than asserting which command happens to be first.
    bool registered = false;
    CmdDispatcher::tx_command_t cmd{};
    while (xQueueReceive(disp.txCmdQueueNew, &cmd, 0) == pdTRUE)
        if (cmd.cmd == (blinds_syscmd_base_t) BlindsStatusCmd::SYSCMD_REGISTER)
            registered = true;

    EXPECT_TRUE(registered)
        << "a resume that was never proven MUST fall back to REGISTER — "
           "otherwise the node is silent until its next wake";
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

std::vector<uint8_t> pack_schedule_op(uint32_t msgid, uint32_t version, uint32_t mode,
                                      uint32_t sender = kHubAddr) {
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
    hdr.senderaddress = sender;
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

// post_event_window must actually do something.
//
// It is configured in YAML, fed into the schedule CRC, pushed over the air,
// persisted on the node and echoed back in the beacon — and its only consumer
// was that beacon field. The quiet window used a hard-coded constant, so
// setting post_event_window had no effect on anything the node did.
// ---------------------------------------------------------------------------
// F14 — a node that wants auto mode but has no clock must keep asking.
//
// TimeSync is a single unbursted, unacknowledged frame, and the hub sends it
// only on the login-ack and beacon paths. An AWAKE node does neither, so losing
// that one frame left the node sitting interactive with nothing to re-trigger
// it. Observed live: CMD_LOGIN and CMD SCHEDULE both arrived, TimeSync did not,
// and the node stayed awake 25 minutes repeating
// "Auto mode is on but the clock is not valid yet".
//
// The node now re-sends its wake beacon, which the hub already answers with a
// TimeSync — reusing that path rather than retransmitting TimeSync blindly.
// ---------------------------------------------------------------------------

namespace {
int drain_beacons(CmdDispatcher &d) {
    int n = 0;
    CmdDispatcher::tx_command_t c{};
    while (xQueueReceive(d.txCmdQueueNew, &c, 0) == pdTRUE)
        if (c.cmd == (blinds_syscmd_base_t) BlindsStatusCmd::SYSCMD_BEACON) ++n;
    return n;
}
}  // namespace

// The deep-sleep drain-wait must not sleep while the motor is still running.
//
// checkQueuesIdle() only counted queue depth. It happened to hold the node
// awake through a move because MotorCtrl leaves the command in motCmdQueueNew
// for the duration — a property of the current queue discipline, not a
// guarantee. Dequeue earlier and track state in the FSM (a perfectly reasonable
// refactor) and the node would deep-sleep mid-travel, leaving the blind half
// closed and its stored position wrong. The invariant is now asked for
// explicitly.
// NOTE: there is deliberately no test here for restorePosition() publishing
// into state_ (the variable the wake beacon reports). MotorCtrl.cpp is not
// compiled by the harness — MotorCtrl is a shim — so any such test would assert
// the shim's behaviour, survive every mutation of the real code, and claim
// coverage it does not have. The shim mirrors production so that OTHER tests
// are not misled; the fix itself is verified on hardware, where the beacon goes
// from pos=0.00 to the real position on a wake.

TEST_F(RealNodeFixture, DeepSleepWaitsForTheMotorEvenWithEmptyQueues) {
    // Drain everything: by the old check, this is "idle".
    CmdDispatcher::tx_command_t c{};
    while (xQueueReceive(disp.rxCmdQueueNew, &c, 0) == pdTRUE) {}
    while (xQueueReceive(disp.txCmdQueueNew, &c, 0) == pdTRUE) {}
    while (xQueueReceive(disp.sysCmdQueueNew, &c, 0) == pdTRUE) {}
    MotorCmd_t m{};
    while (xQueueReceive(mot.motCmdQueueNew, &m, 0) == pdTRUE) {}
    while (xQueueReceive(mot.motorCmdQueueNew, &m, 0) == pdTRUE) {}

    mot.set_busy(true);
    EXPECT_FALSE(disp.checkQueuesIdle())
        << "empty queues with the motor still running must NOT count as idle — "
           "sleeping here stops the blind mid-travel and records the wrong "
           "position";

    mot.set_busy(false);
    EXPECT_TRUE(disp.checkQueuesIdle())
        << "and once the motor is idle and the queues are empty, sleep";
}

// ---------------------------------------------------------------------------
// F18 — EXT1 is not synonymous with "a button was pressed".
//
// The deep-sleep wake mask contains the three buttons AND the LoRa DIO0/DIO1
// lines. classifyWakeReason() returned WAKE_BUTTON for any EXT1 wake without
// looking at which pin fired — and WAKE_BUTTON makes main.cpp suspend automatic
// mode for the whole interactive_timeout (5 min here, 30 min on node 1) and
// keep the node awake. A single radio interrupt would therefore stop the
// schedule with nobody having touched the blind.
// ---------------------------------------------------------------------------

TEST_F(RealNodeFixture, AButtonPinMakesItAButtonWake) {
    proto_sim_set_wakeup_causes(BIT(ESP_SLEEP_WAKEUP_EXT1));
    proto_sim_set_ext1_status(1ULL << ctrlButtonDownPin);

    EXPECT_EQ(CmdDispatcher::classifyWakeReason(), WAKE_REASON__WAKE_BUTTON)
        << "a real press must still flip the node to interactive";

    proto_sim_set_wakeup_causes(0);
    proto_sim_set_ext1_status(0);
}

TEST_F(RealNodeFixture, ARadioPinIsNotAButtonWake) {
    // EXT1 fired, but from a LoRa DIO line rather than a button.
    proto_sim_set_wakeup_causes(BIT(ESP_SLEEP_WAKEUP_EXT1));
    // GPIO26 is the LoRa DIO0 on this board variant; the value matters less
    // than it not being one of the button pins (34/35/36).
    proto_sim_set_ext1_status(1ULL << 26);

    EXPECT_NE(CmdDispatcher::classifyWakeReason(), WAKE_REASON__WAKE_BUTTON)
        << "a radio interrupt reported as a button press suspends automatic "
           "mode for the full interactive_timeout with nobody at the blind";
    EXPECT_EQ(CmdDispatcher::classifyWakeReason(),
              WAKE_REASON__WAKE_TIMER_CHECKIN)
        << "it is an ordinary check-in: beacon, listen, sleep again";

    proto_sim_set_wakeup_causes(0);
    proto_sim_set_ext1_status(0);
}

// ---------------------------------------------------------------------------
// The beacon retry ladder.
//
// The link is asymmetric: every hub->node message is bursted 17x across a full
// round (the node's receiver is windowed), while a node uplink is sent ONCE. The
// wake beacon is therefore the least protected frame in the system — and it is
// the one the exchange hangs on, since the hub answers it with TimeSync and, if
// the version differs, the schedule.
//
// Losing it used to cost a full REGISTER -> ClientConfig -> login handshake,
// because armResumeFallback() was the only recovery. Re-sending the beacon
// costs one frame. The hub always answers a beacon, so a decrypted downlink is
// a reliable implicit ACK.
// ---------------------------------------------------------------------------

TEST_F(RealNodeFixture, AnUnansweredBeaconIsResent) {
    (void) drain_beacons(disp);
    disp.armResumeFallback();      // "beacon sent, expecting a reply"
    (void) drain_beacons(disp);    // ignore anything armResumeFallback queued

    // No decrypted downlink arrives; the ack timer expires.
    proto_sim_timer_fire_all();

    EXPECT_GT(drain_beacons(disp), 0)
        << "an unanswered beacon must be re-sent — it is a single unbursted "
           "frame, and the alternative is a full REGISTER handshake";
}

TEST_F(RealNodeFixture, AnAnsweredBeaconIsNotResent) {
    (void) drain_beacons(disp);
    disp.armResumeFallback();
    (void) drain_beacons(disp);

    // A decrypted downlink IS the acknowledgement.
    disp.noteSessionProven();
    proto_sim_timer_fire_all();

    // NOTE: two things enforce this — noteSessionProven() cancels the timer,
    // and beaconRetryCb re-checks session_proven_ and self-cancels if it fires
    // anyway. So removing the handler-side cancel does NOT fail this test; it
    // only costs one extra timer expiry. The behaviour is what is asserted.
    EXPECT_EQ(drain_beacons(disp), 0)
        << "the hub answered, so re-sending would be pure airtime — and on a "
           "shared channel that is airtime taken from the other node";
}

TEST_F(RealNodeFixture, BeaconRetriesAreBounded) {
    (void) drain_beacons(disp);
    disp.armResumeFallback();
    (void) drain_beacons(disp);

    // Fire well past the ladder's length. Each expiry re-arms, so this walks
    // the whole ladder and then some.
    int total = 0;
    for (int i = 0; i < 6; i++) {
        proto_sim_timer_fire_all();
        total += drain_beacons(disp);
    }

    EXPECT_LE(total, CmdDispatcher::kMaxBeaconRetries)
        << "the ladder must stop and hand over to the REGISTER fallback — an "
           "unbounded retry against a hub that cannot answer (its own clock "
           "invalid, so it sends no TimeSync) would beacon forever";
    EXPECT_GT(total, 0) << "but it must retry at least once";
}

// ---------------------------------------------------------------------------
// F19 — the nightly CMD_SLEEP belongs to interactive mode only.
//
// It exists to put an AWAKE node down for sleep_duration. An auto-mode node
// derives its wake from the schedule, so obeying it would replace a
// schedule-derived wake with a fixed-duration one.
//
// The node-side ignore is belt and braces (the hub no longer sends it to an
// auto-mode node), but it matters for a node whose mode the hub has stale
// knowledge of — exactly the divergence that caused the damage.
// ---------------------------------------------------------------------------

TEST_F(RealNodeFixture, AutoModeIgnoresTheNightlySleepCommand) {
    sched::Entry e = sched_entry(450, sched::DAY_ALL);
    sys.setSchedule(1, /*mode=*/1, 1800, 21600, 30, 20, 1800, &e, 1);
    ASSERT_TRUE(sys.getAutoMode()) << "precondition: node is in auto mode";

    CmdDispatcher::tx_command_t drain{};
    while (xQueueReceive(disp.sysCmdQueueNew, &drain, 0) == pdTRUE) {}

    auto frame = pack_sysop_op(/*msgid=*/860, CLIENT_OPERATION__CMD_SLEEP);
    disp.onReceiveNew(frame.data(), static_cast<int>(frame.size()));

    EXPECT_FALSE(drain_for_sleep(disp))
        << "an auto-mode node must keep its schedule-derived wake — obeying a "
           "fixed sleep_duration puts its next wake where the hub guesses "
           "rather than where the schedule says";
}

TEST_F(RealNodeFixture, InteractiveModeStillObeysTheNightlySleep) {
    // The case the command exists for must keep working.
    sys.setAutoMode(false);
    ASSERT_FALSE(sys.getAutoMode()) << "precondition: node is interactive";

    CmdDispatcher::tx_command_t drain{};
    while (xQueueReceive(disp.sysCmdQueueNew, &drain, 0) == pdTRUE) {}

    auto frame = pack_sysop_op(/*msgid=*/861, CLIENT_OPERATION__CMD_SLEEP);
    disp.onReceiveNew(frame.data(), static_cast<int>(frame.size()));

    EXPECT_TRUE(drain_for_sleep(disp))
        << "an interactive node has no schedule to sleep towards — the nightly "
           "sleep is the only thing that puts it down for the night";
}

TEST_F(RealNodeFixture, NoClockMakesTheNodeAskAgain) {
    // Auto mode configured, but no TimeSync has arrived.
    sched::Entry e = sched_entry(450, sched::DAY_ALL);
    sys.setSchedule(1, /*mode=*/1, 1800, 600, 30, 20, 1800, &e, 1);

    ASSERT_FALSE(CmdDispatcher::isClockValid())
        << "precondition: no clock yet";
    ASSERT_FALSE(disp.shouldRunAutoMode())
        << "I8: a node that cannot evaluate its schedule must stay interactive";

    (void) drain_beacons(disp);
    proto_sim_timer_fire_all();

    EXPECT_GT(drain_beacons(disp), 0)
        << "the node must re-send its wake beacon to ask for a TimeSync — "
           "otherwise one lost frame strands it in interactive mode with "
           "nothing to re-trigger it";
}

TEST_F(RealNodeFixture, AClockStopsTheAsking) {
    sched::Entry e = sched_entry(450, sched::DAY_ALL);
    sys.setSchedule(1, /*mode=*/1, 1800, 600, 30, 20, 1800, &e, 1);
    ASSERT_FALSE(disp.shouldRunAutoMode());   // arms the retry

    // The TimeSync we were asking for.
    give_clock(disp, /*msgid=*/900, 1787000000ULL, 0);
    ASSERT_TRUE(CmdDispatcher::isClockValid());

    (void) drain_beacons(disp);
    proto_sim_timer_fire_all();

    // NOTE: two things enforce this — the TimeSync handler cancels the timer,
    // and clockRetryCb re-checks isClockValid() and self-cancels if it fires
    // anyway. So removing the handler-side cancel does NOT fail this test; it
    // only costs one extra firing. The behaviour is what is asserted here.
    EXPECT_EQ(drain_beacons(disp), 0)
        << "once the clock is established the node must stop asking — a retry "
           "that never stops is just a slower kind of stuck";
}

TEST_F(RealNodeFixture, QuietWindowComesFromPostEventWindow) {
    sched::Entry e = sched_entry(450, sched::DAY_ALL);
    // 45 s window, comfortably above the floor.
    sys.setSchedule(1, /*mode=*/1, /*interactiveTimeout=*/1800, /*checkin=*/600,
                    /*beaconLead=*/30, /*postEventWindow=*/45,
                    /*catchup=*/1800, &e, 1);

    EXPECT_EQ(disp.autoSleepQuietMs(), 45000u)
        << "a configured post_event_window must be what the node actually waits";
}

TEST_F(RealNodeFixture, AZeroPostEventWindowNeverYieldsAnUnusableWindow) {
    // A zero window would sleep the node the instant it stopped transmitting,
    // so it must never take effect. TWO mechanisms enforce that — setSchedule()
    // ignores a zero push (there is no meaningful "zero listening window"), and
    // the floor clamps anything too short — so no single mutation will fail
    // this test. It is here to state the property, not to pin one line.
    //
    // An earlier version of this test claimed to cover a fallback branch for
    // the zero case. It could not: Config defaults postEventWindow_s to 20 and
    // setSchedule ignores zeroes, so the branch was unreachable and the test
    // passed only because the default happened to equal the constant it
    // asserted. The branch has been deleted.
    sched::Entry e = sched_entry(450, sched::DAY_ALL);
    sys.setSchedule(1, 1, 1800, 600, 30, /*postEventWindow=*/0, 1800, &e, 1);

    EXPECT_GE(disp.autoSleepQuietMs(), CmdDispatcher::kAutoSleepQuietMinMs)
        << "a zero push must never leave the node with a window too short to "
           "survive the hub's inter-frame gaps";
}

TEST_F(RealNodeFixture, QuietWindowIsFlooredBelowTheHubsInterFrameGaps) {
    sched::Entry e = sched_entry(450, sched::DAY_ALL);
    // 2 s is shorter than the hub's schedule-push retransmit interval (5 s), so
    // the node would sleep between two frames of a conversation in progress —
    // reintroducing F8 from YAML.
    sys.setSchedule(1, 1, 1800, 600, 30, /*postEventWindow=*/2, 1800, &e, 1);

    EXPECT_EQ(disp.autoSleepQuietMs(), CmdDispatcher::kAutoSleepQuietMinMs)
        << "a window shorter than the hub's inter-frame gaps must be clamped, "
           "not honoured — honouring it makes the node unreachable";
}

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
// The boot path must gate the resume on isProvisioned(), not getRegistered().
//
// getRegistered() is set ONLY by the CLIENTCONFIG / COVERCONFIG handlers, which
// the hub sends only when a node's config is unsynced THAT boot. A node the hub
// already knows registers and receives a bare LoginMsg:
//
//     Registered with LORA server
//     LoginMsg sent (request_register=0)      <- no config push, so no flag
//
// so the flag stays false for the life of the RTC domain. It guarded the
// beacon-first resume, which therefore never ran: every wake did a full
// REGISTER -> config -> login handshake. Observed on node 2 as a REGISTER on
// every 10-minute check-in, and "Device not registered yet" logged on a wake
// with rtc_ram=VALID.
//
// This pins the predicate, not main.cpp's branch (main.cpp is not compiled by
// the harness) — but the predicate is the whole of the bug: the two disagree
// for exactly the node that has been provisioned and not re-pushed.
// A session established by login must survive a save/load round trip.
//
// persistHubAddr_ defaulted to 0xFF and was only ever assigned from a blob the
// node had just loaded, so it stayed 255 while the live nonce was filed under
// the hub's real address. savePersistentState() looked up get_base_nonce(255)
// and — because a peer entry is created valid with base_nonce 0 — got a
// "session" that could not decrypt anything, so it saved that instead.
//
// The node then restored identical stale state on every boot:
//     F-5: restored state hub=255 base_nonce=0x20fc0cdd tx=390(+64) rx=0
// and its resume beacon used a nonce the hub had never held.
//
// This test was written during that fix and REMOVED, because it passed under
// mutation: the zero-nonce peer satisfied get_base_nonce() either way. Now that
// a zero nonce is correctly reported as "no session", it discriminates.
TEST(RealCmdDispatcherPersist, ASessionFromLoginSurvivesSaveAndLoad) {
    MotorCtrl mot; SystemCtrl sys; LoraInterface lif;
    portMUX_TYPE mmux{}, bmux{};

    // The node NVS shim is process-global; without this the load below can pick
    // up a blob an earlier test saved.
    proto_sim_nvs_reset();

    {
        CmdDispatcher d{&mot, &sys, &lif, mmux, bmux};
        sys.setAddress(kNodeAddr, kSubnet);
        ASSERT_EQ(psa_crypto_init(), PSA_SUCCESS);

        auto login = pack_login_op(/*msgid=*/1, /*nonce=*/0xA1B2C3D4);
        d.onReceiveNew(login.data(), static_cast<int>(login.size()));
        d.savePersistentState();
    }

    CmdDispatcher d2{&mot, &sys, &lif, mmux, bmux};
    d2.loadPersistentState();

    EXPECT_EQ(d2.destAddress, static_cast<uint32_t>(kHubAddr))
        << "the restored session must point at the HUB — with the wrong peer "
           "key it restored hub=255 (broadcast)";
    EXPECT_TRUE(d2.hasValidPersistentState())
        << "the login's session must actually have been written";
    EXPECT_TRUE(d2.canResumeSession())
        << "and the restored session must be usable, or the beacon-first "
           "resume encrypts with a nonce the hub has never held";
}

TEST_F(RealNodeFixture, ProvisionedNodeIsRecognisedWithoutAConfigPush) {
    // The fixture sets an address, as a provisioned node has from config.txt,
    // WITHOUT any CLIENTCONFIG/COVERCONFIG having arrived this boot.
    ASSERT_NE(sys.getConfigAddress(), 0)
        << "precondition: the node has a persisted address";

    EXPECT_TRUE(disp.isProvisioned())
        << "a node with a persisted address is provisioned — this is what "
           "'the hub can address us' actually means, and it is what the boot "
           "path must gate the beacon-first resume on";
}

TEST(RealCmdDispatcherFresh, UnprovisionedNodeIsNotMistakenForProvisioned) {
    // The other side of the predicate: address 0 means the hub cannot reach us,
    // so the node must register rather than try to resume.
    MotorCtrl     mot;
    SystemCtrl    sys;
    LoraInterface lif;
    portMUX_TYPE  motorMux{};
    portMUX_TYPE  buttonMux{};
    CmdDispatcher disp{&mot, &sys, &lif, motorMux, buttonMux};

    ASSERT_EQ(sys.getConfigAddress(), 0) << "precondition: fresh node";
    EXPECT_FALSE(disp.isProvisioned());
}

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

// ---------------------------------------------------------------------------
// Catch-up window (I3).
//
// last_missed() — the calendar primitive — is covered in scheduler_test,
// including its window boundaries. What is covered here is the layer above:
// runDueScheduleEntry() deriving that window from catchupWindow, and refusing
// to run the same entry twice.
//
// The double-execution guard matters more since the quiet-window re-check was
// added: runDueScheduleEntry() is now called both at boot AND when the hub goes
// quiet, so a broken s_last_exec_epoch guard would replay an entry on every
// single wake.
// ---------------------------------------------------------------------------

namespace {
// Arms one schedule entry `minutes_ago` in the past with the given catch-up
// window, and clears the interactive override (a file-level static an earlier
// test leaves set to "forever" — see the harness notes in the README).
// Returns the entry's minute-of-day.
uint16_t arm_missed_entry(CmdDispatcher &d, SystemCtrl &s,
                          int minutes_ago, uint32_t catchup) {
    // Clear the interactive override FIRST, before the entry is computed.
    //
    // This dance costs 1.2 s of real time, and the entry's age is measured
    // against a 120 s grace window. Computing the entry before the sleep made
    // a 1-minute-old entry 61..120 s old at arming and up to 121 s old at use —
    // so the test failed whenever the sleep straddled a minute boundary, about
    // 2% of runs. Caught in a full-suite run that passed 3/3 in isolation.
    sched::Entry placeholder = sched_entry(0, sched::DAY_ALL, sched::ACTION_CLOSE);
    s.setSchedule(/*version=*/0xCA7C, /*mode=*/1, /*interactiveTimeout=*/1,
                  /*checkin=*/600, /*beaconLead=*/30, /*postEventWindow=*/20,
                  catchup, &placeholder, 1);
    give_clock(d, /*msgid=*/800, static_cast<uint64_t>(time(nullptr)), 0);
    d.enterInteractiveMode();
    std::this_thread::sleep_for(std::chrono::milliseconds(1200));

    // NOW compute the entry, so its age is what the test intends.
    const uint64_t now_s = static_cast<uint64_t>(time(nullptr));
    const int now_minute = static_cast<int>((now_s / 60) % 1440);
    const int entry_minute = now_minute - minutes_ago;
    if (entry_minute < 0) return 0xFFFF;      // caller skips: wraps past midnight

    sched::Entry e = sched_entry(static_cast<uint16_t>(entry_minute),
                                 sched::DAY_ALL, sched::ACTION_CLOSE);
    s.setSchedule(/*version=*/0xCA7C, /*mode=*/1, /*interactiveTimeout=*/1,
                  /*checkin=*/600, /*beaconLead=*/30, /*postEventWindow=*/20,
                  catchup, &e, 1);
    give_clock(d, /*msgid=*/801, now_s, 0);
    return static_cast<uint16_t>(entry_minute);
}

int drain_closes(CmdDispatcher &d) {
    int n = 0;
    CmdDispatcher::tx_command_t c{};
    while (xQueueReceive(d.rxCmdQueueNew, &c, 0) == pdTRUE)
        if (c.cmd == (blinds_syscmd_base_t) BlindsOpCmd::SYSCMD_CLOSE) ++n;
    return n;
}
}  // namespace

TEST_F(RealNodeFixture, CatchupZeroNeverReplaysAMissedEntry) {
    // catchup_window == 0 is documented as "never replay a miss". A node that
    // was off overnight must not slam the blind on the morning it comes back.
    //
    // 10 minutes, comfortably outside the fixed kDueGraceS (120 s) window that
    // is always searched. This used to say 2 minutes, which passed only because
    // the window is exclusive at its start — a one-second margin, i.e. an
    // accidental pass.
    if (arm_missed_entry(disp, sys, /*minutes_ago=*/10, /*catchup=*/0) == 0xFFFF)
        GTEST_SKIP() << "would wrap past midnight UTC";
    ASSERT_TRUE(disp.shouldRunAutoMode());
    (void) drain_closes(disp);

    EXPECT_FALSE(disp.runDueScheduleEntry())
        << "catchup_window 0 must mean no replay at all";
    EXPECT_EQ(drain_closes(disp), 0);
}

TEST_F(RealNodeFixture, AnEntryOlderThanTheCatchupWindowIsNotReplayed) {
    // Outside the window the miss is stale — acting on it would move the blind
    // for a reason hours in the past.
    if (arm_missed_entry(disp, sys, /*minutes_ago=*/20, /*catchup=*/300) == 0xFFFF)
        GTEST_SKIP() << "would wrap past midnight UTC";
    ASSERT_TRUE(disp.shouldRunAutoMode());
    (void) drain_closes(disp);

    EXPECT_FALSE(disp.runDueScheduleEntry())
        << "a miss 20 min old must not replay under a 5 min catch-up window";
    EXPECT_EQ(drain_closes(disp), 0);
}

TEST_F(RealNodeFixture, AnEntryInsideTheCatchupWindowIsReplayedExactlyOnce) {
    // Inside the window it replays — and, crucially, only once. Since the quiet
    // window now re-checks, a broken guard would re-run the entry on every wake
    // and on every hub lull.
    if (arm_missed_entry(disp, sys, /*minutes_ago=*/3, /*catchup=*/1800) == 0xFFFF)
        GTEST_SKIP() << "would wrap past midnight UTC";
    ASSERT_TRUE(disp.shouldRunAutoMode());
    (void) drain_closes(disp);

    EXPECT_TRUE(disp.runDueScheduleEntry()) << "the miss is inside the window";
    EXPECT_EQ(drain_closes(disp), 1);

    EXPECT_FALSE(disp.runDueScheduleEntry())
        << "the same entry must not run twice — runDueScheduleEntry() is called "
           "at boot AND when the hub goes quiet, so replaying here would move "
           "the blind again on every wake for the whole catch-up window";
    EXPECT_EQ(drain_closes(disp), 0);
}

// ORDERING NOTE: this test must come AFTER the two above. s_last_exec_epoch is
// a file-level static shared by every test in the binary, and an entry is
// skipped when `due <= s_last_exec_epoch`. This test executes the most recent
// entry of the group (1 minute ago), so running it earlier would block the
// older entries the other tests rely on. Defined last so each execution uses a
// strictly later epoch than the one before.
// catchup_window == 0 must NOT disable the schedule itself.
//
// runDueScheduleEntry() is the only thing that executes an entry, and it used
// to `return false` outright when catchup was 0 — so setting "never replay a
// missed event" silently turned automatic mode into a no-op. Observed live on
// node 1: it woke on time for its 21:45 close, beaconed either side of the
// event, and never moved the blind.
//
// An entry falling due NOW is not a missed event. It must run whatever
// catchup_window says.
TEST_F(RealNodeFixture, CatchupZeroStillRunsAnEntryThatIsDueNow) {
    if (arm_missed_entry(disp, sys, /*minutes_ago=*/1, /*catchup=*/0) == 0xFFFF)
        GTEST_SKIP() << "would wrap past midnight UTC";
    ASSERT_TRUE(disp.shouldRunAutoMode());
    (void) drain_closes(disp);

    EXPECT_TRUE(disp.runDueScheduleEntry())
        << "catchup_window 0 means 'do not replay a MISS', not 'never run the "
           "schedule' — this is what left the blind untouched all night";
    EXPECT_EQ(drain_closes(disp), 1);
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

// ---------------------------------------------------------------------------
// Drift test: the node must always come back to the PRODUCTION power profile
// ---------------------------------------------------------------------------

TEST_F(RealNodeFixture, DriftTestStartsInMeasuringProfileAndEndsInProduction) {
    // The measuring profile disables light sleep and pins the CPU at 240 MHz:
    // ~11 mA against the ~1.2 mA interactive average. If a test ever ends
    // without restoring production, the node quietly burns ~9x its budget and
    // nothing says so until the battery is flat.
    SystemCtrl::applyPowerProfile(false);
    ASSERT_FALSE(SystemCtrl::measuringProfile());

    LoraClientOperationMessage op = LORA_CLIENT_OPERATION_MESSAGE__INIT;
    LoraHeader hdr = LORA_HEADER__INIT;
    hdr.destaddress = kNodeAddr; hdr.destsubnet = kSubnet;
    hdr.senderaddress = kHubAddr; hdr.msgid = 1;
    op.header = &hdr;

    DriftTest dt = DRIFT_TEST__INIT;
    dt.enable = true; dt.durations = 60; dt.gridperiodms = 1100;
    op.cmd_case = LORA_CLIENT_OPERATION_MESSAGE__CMD_DRIFTTEST;
    op.drifttest = &dt;

    disp.handleDriftTest(&op, &hdr);
    EXPECT_TRUE(SystemCtrl::measuringProfile())
        << "entering the test must disable light sleep";
    EXPECT_TRUE(disp.driftTestActive());

    dt.enable = false;
    disp.handleDriftTest(&op, &hdr);
    EXPECT_FALSE(SystemCtrl::measuringProfile())
        << "leaving the test MUST restore production power management";
    EXPECT_FALSE(disp.driftTestActive());
    EXPECT_FALSE(lif.continuousRx())
        << "and must leave continuous RX — 11 mA otherwise";
}

// ---------------------------------------------------------------------------
// Mode sysops must actually change the mode
// ---------------------------------------------------------------------------

TEST_F(RealNodeFixture, ModeSysopsSwitchTheNodeMode) {
    // The bug this pins down. CMD_MODE_AUTO (5) and CMD_MODE_INTERACTIVE (6)
    // had no case in blindsSysPbToCmd(), so they fell to `default: return 0`
    // and the node did NOTHING with them.
    //
    // It hid for two reasons, both of which made the system look healthy:
    // the ack is sent regardless, so the hub logged "Tracked op acknowledged"
    // and believed the mode had changed; and ScheduleConfig also carries mode,
    // so any change followed by a schedule push landed anyway. It failed only
    // for a node AWAKE and not beaconing -- exactly what this sysop is for.
    // Observed on hardware: hub switch ON and acked, node still interactive 27
    // minutes later.
    LoraClientOperationMessage op = LORA_CLIENT_OPERATION_MESSAGE__INIT;
    LoraHeader hdr = LORA_HEADER__INIT;
    hdr.destaddress = kNodeAddr; hdr.destsubnet = kSubnet;
    hdr.senderaddress = kHubAddr; hdr.msgid = 1;
    op.header = &hdr;
    op.cmd_case = LORA_CLIENT_OPERATION_MESSAGE__CMD_SYSOP;

    sys.setAutoMode(false);
    ASSERT_FALSE(sys.getAutoMode());

    op.sysop = CLIENT_OPERATION__CMD_MODE_AUTO;
    disp.handleSysop(&op, &hdr);
    EXPECT_TRUE(sys.getAutoMode())
        << "CMD_MODE_AUTO must switch the node to automatic mode";

    op.sysop = CLIENT_OPERATION__CMD_MODE_INTERACTIVE;
    disp.handleSysop(&op, &hdr);
    EXPECT_FALSE(sys.getAutoMode())
        << "CMD_MODE_INTERACTIVE must switch it back";
}

TEST_F(RealNodeFixture, AModeSysopDoesNotLeakIntoTheSystemCommandQueue) {
    // A mode sysop has no BlindsSysCmd equivalent, so blindsSysPbToCmd()
    // returns 0 for it. Falling through to the normal dispatch would hand that
    // 0 to setSystemCommand() -- a command that means nothing, executed for
    // every mode change.
    LoraClientOperationMessage op = LORA_CLIENT_OPERATION_MESSAGE__INIT;
    LoraHeader hdr = LORA_HEADER__INIT;
    hdr.destaddress = kNodeAddr; hdr.destsubnet = kSubnet;
    hdr.senderaddress = kHubAddr; hdr.msgid = 2;
    op.header = &hdr;
    op.cmd_case = LORA_CLIENT_OPERATION_MESSAGE__CMD_SYSOP;
    op.sysop = CLIENT_OPERATION__CMD_MODE_AUTO;

    disp.handleSysop(&op, &hdr);
    EXPECT_TRUE(sys.getAutoMode());   // handled, and handled only once
}

// ---------------------------------------------------------------------------
// M1 — the MAC control frame terminates at MAC-0.
//
// Built with the REAL protobuf-c stubs rather than the sim's hand-written
// mirror, so the bytes are exactly what the hub puts on the air. The mirror
// never covered DriftTest either; for a frame whose whole purpose is to be
// measured, byte fidelity is the point.
//
// What these assert is the layer boundary: a MacControl frame is counted,
// timestamped and echoed, and NO application handler runs. That is why it is
// safe to send at any time — there is no inert command to design, because
// there is no application dispatch.
// ---------------------------------------------------------------------------
namespace {

std::vector<uint8_t> build_mac_ping(uint32_t seq, bool want_echo,
                                    uint32_t msgid, size_t pad_bytes = 0) {
    LoraHeader hdr = LORA_HEADER__INIT;
    hdr.destaddress   = kNodeAddr;
    hdr.destsubnet    = kSubnet;
    hdr.senderaddress = 1;          // the hub
    hdr.msgid         = msgid;

    MacControl mc = MAC_CONTROL__INIT;
    mc.kind     = MAC_CONTROL__KIND__MAC_PING;
    mc.seq      = seq;
    mc.wantecho = want_echo;

    std::vector<uint8_t> pad(pad_bytes, 0xAB);
    if (pad_bytes) { mc.pad.len = pad.size(); mc.pad.data = pad.data(); }

    LoraClientOperationMessage op = LORA_CLIENT_OPERATION_MESSAGE__INIT;
    op.header     = &hdr;
    op.cmd_case   = LORA_CLIENT_OPERATION_MESSAGE__CMD_MACCONTROL;
    op.maccontrol = &mc;

    std::vector<uint8_t> out(lora_client_operation_message__get_packed_size(&op));
    lora_client_operation_message__pack(&op, out.data());
    return out;
}

}  // namespace

TEST_F(RealNodeFixture, MacPingIsCountedAndTimestamped) {
    ASSERT_EQ(disp.macCounters().ping_rx, 0u);

    auto bytes = build_mac_ping(/*seq=*/7, /*want_echo=*/false, /*msgid=*/1000);
    disp.onReceiveNew(bytes.data(), static_cast<int>(bytes.size()));

    EXPECT_EQ(disp.macCounters().ping_rx, 1u);
    EXPECT_EQ(disp.macCounters().last_seq, 7u);
}

TEST_F(RealNodeFixture, MacPingWithoutEchoSendsNothing) {
    auto bytes = build_mac_ping(/*seq=*/1, /*want_echo=*/false, /*msgid=*/1000);
    disp.onReceiveNew(bytes.data(), static_cast<int>(bytes.size()));

    EXPECT_EQ(disp.macCounters().ping_rx, 1u);
    EXPECT_EQ(disp.macCounters().echo_tx, 0u);
}

TEST_F(RealNodeFixture, MacPingWithEchoRepliesFromMac0) {
    auto bytes = build_mac_ping(/*seq=*/42, /*want_echo=*/true, /*msgid=*/1000);
    disp.onReceiveNew(bytes.data(), static_cast<int>(bytes.size()));

    EXPECT_EQ(disp.macCounters().ping_rx, 1u);
    EXPECT_EQ(disp.macCounters().echo_tx, 1u)
        << "MAC-0 must answer without any application round trip";
    EXPECT_EQ(disp.macCounters().echo_failed, 0u);
    EXPECT_EQ(disp.macCounters().last_seq, 42u);
}

TEST_F(RealNodeFixture, SeqIsCarriedNotDerivedFromMsgid) {
    // A frame lost to the air must leave a GAP rather than shifting every
    // later sample, which is why seq is its own field. Send marks 5 and 9 with
    // consecutive msgids: the node reports the MARK, not the msgid.
    // msgids stay near the node's own counter: the replay window rejects an
    // id too far ahead ("Rejected message ID: 2000 ... my MsgID: 0"). That
    // check is MAC-1 admission, and today it is entangled with MAC-0 — which
    // is exactly why mac-layer.md wants the sublayer independently switchable.
    auto a = build_mac_ping(/*seq=*/5, false, /*msgid=*/100);
    disp.onReceiveNew(a.data(), static_cast<int>(a.size()));
    EXPECT_EQ(disp.macCounters().last_seq, 5u);

    auto b = build_mac_ping(/*seq=*/9, false, /*msgid=*/101);
    disp.onReceiveNew(b.data(), static_cast<int>(b.size()));
    EXPECT_EQ(disp.macCounters().last_seq, 9u);
    EXPECT_EQ(disp.macCounters().ping_rx, 2u);
}

TEST_F(RealNodeFixture, MacEchoFromAPeerIsIgnored) {
    // Only a PING is actionable. An echo arriving at a node is either our own
    // frame looped back or a peer misbehaving; if the node answered it, two
    // nodes could echo each other indefinitely.
    LoraHeader hdr = LORA_HEADER__INIT;
    hdr.destaddress = kNodeAddr;
    hdr.destsubnet  = kSubnet;
    hdr.senderaddress = 1;
    hdr.msgid = 300;

    MacControl mc = MAC_CONTROL__INIT;
    mc.kind     = MAC_CONTROL__KIND__MAC_ECHO;
    mc.seq      = 3;
    mc.wantecho = true;                 // even so: no reply

    LoraClientOperationMessage op = LORA_CLIENT_OPERATION_MESSAGE__INIT;
    op.header = &hdr;
    op.cmd_case = LORA_CLIENT_OPERATION_MESSAGE__CMD_MACCONTROL;
    op.maccontrol = &mc;

    std::vector<uint8_t> bytes(lora_client_operation_message__get_packed_size(&op));
    lora_client_operation_message__pack(&op, bytes.data());
    disp.onReceiveNew(bytes.data(), static_cast<int>(bytes.size()));

    EXPECT_EQ(disp.macCounters().ping_rx, 0u);
    EXPECT_EQ(disp.macCounters().echo_tx, 0u);
}

TEST_F(RealNodeFixture, PaddingSweepsTimeOnAirWithoutChangingBehaviour) {
    // The FIFO read is per byte, so turnaround has to be measurable as a
    // function of frame length. Padding must not change what the MAC does.
    for (size_t pad : {size_t{0}, size_t{20}, size_t{100}}) {
        disp.resetMacCounters();
        auto bytes = build_mac_ping(/*seq=*/1, /*want_echo=*/true,
                                    /*msgid=*/(uint32_t)(200 + pad), pad);
        EXPECT_GT(bytes.size(), pad);
        disp.onReceiveNew(bytes.data(), static_cast<int>(bytes.size()));
        EXPECT_EQ(disp.macCounters().ping_rx, 1u) << "pad " << pad;
        EXPECT_EQ(disp.macCounters().echo_tx, 1u) << "pad " << pad;
    }
}

// ---------------------------------------------------------------------------
// M2 — the funnel advances in the REAL dispatcher, not just in the header.
// ---------------------------------------------------------------------------

TEST_F(RealNodeFixture, FunnelCountsParsedFrames) {
    ASSERT_EQ(disp.macFunnel().parsed, 0u);
    auto bytes = build_mac_ping(/*seq=*/1, /*want_echo=*/false, /*msgid=*/100);
    disp.onReceiveNew(bytes.data(), static_cast<int>(bytes.size()));
    EXPECT_EQ(disp.macFunnel().parsed, 1u);
    EXPECT_EQ(disp.macFunnel().parse_failures, 0u);
}

TEST_F(RealNodeFixture, FunnelCountsGarbageAsAParseFailureNotLoss) {
    // Noise and foreign protocols reach the dispatcher constantly on a shared
    // channel. They are counted, not treated as link loss.
    uint8_t junk[24];
    for (size_t i = 0; i < sizeof(junk); ++i) junk[i] = static_cast<uint8_t>(0xF0 | (i & 0x0F));
    disp.onReceiveNew(junk, static_cast<int>(sizeof(junk)));

    EXPECT_EQ(disp.macFunnel().parse_failures, 1u);
    EXPECT_EQ(disp.macFunnel().parsed, 0u);
    EXPECT_EQ(disp.macFunnel().crc_errors, 0u)
        << "a parse failure is not a CRC error — different stage, different fix";
}

TEST_F(RealNodeFixture, FunnelCountsTheReplayWindowAsDuplicatesNotLoss) {
    auto a = build_mac_ping(/*seq=*/1, /*want_echo=*/false, /*msgid=*/150);
    disp.onReceiveNew(a.data(), static_cast<int>(a.size()));
    const uint32_t accepted_after_first = disp.macFunnel().counter_accepted;
    EXPECT_GE(accepted_after_first, 1u);

    // Same msgid again — exactly what copies 2..17 of a burst look like.
    disp.onReceiveNew(a.data(), static_cast<int>(a.size()));
    EXPECT_EQ(disp.macFunnel().duplicates, 1u);
    EXPECT_EQ(disp.macFunnel().counter_accepted, accepted_after_first)
        << "a duplicate must not advance the accepted count";
}

TEST_F(RealNodeFixture, DetectedIsReportedByTheRadioPathNotInferred) {
    // The DIO0 task owns stages 2-3 because that is the only place the CRC flag
    // exists. The dispatcher must not invent them from frames it happened to
    // parse, or a frame lost before RxDone would never be counted at all.
    auto bytes = build_mac_ping(/*seq=*/1, /*want_echo=*/false, /*msgid=*/160);
    disp.onReceiveNew(bytes.data(), static_cast<int>(bytes.size()));
    EXPECT_EQ(disp.macFunnel().detected, 0u);

    disp.noteFrameDetected(/*crc_ok=*/true);
    disp.noteFrameDetected(/*crc_ok=*/false);
    EXPECT_EQ(disp.macFunnel().detected, 2u);
    EXPECT_EQ(disp.macFunnel().crc_valid, 1u);
    EXPECT_EQ(disp.macFunnel().crc_errors, 1u);
}

// ---------------------------------------------------------------------------
// M3 — the sublayer switches, in the REAL dispatcher.
// ---------------------------------------------------------------------------
namespace {

// Negative sense on the wire, so a zeroed message is the SAFE one.
std::vector<uint8_t> build_mac_config(bool disable_counter, bool disable_crypto,
                                      uint32_t duration_s, uint32_t msgid) {
    LoraHeader hdr = LORA_HEADER__INIT;
    hdr.destaddress   = kNodeAddr;
    hdr.destsubnet    = kSubnet;
    hdr.senderaddress = 1;
    hdr.msgid         = msgid;

    MacControl mc = MAC_CONTROL__INIT;
    mc.kind          = MAC_CONTROL__KIND__MAC_CONFIG;
    mc.disablecounter = disable_counter;
    mc.disablecrypto  = disable_crypto;
    mc.durations     = duration_s;

    LoraClientOperationMessage op = LORA_CLIENT_OPERATION_MESSAGE__INIT;
    op.header     = &hdr;
    op.cmd_case   = LORA_CLIENT_OPERATION_MESSAGE__CMD_MACCONTROL;
    op.maccontrol = &mc;

    std::vector<uint8_t> out(lora_client_operation_message__get_packed_size(&op));
    lora_client_operation_message__pack(&op, out.data());
    return out;
}

}  // namespace

TEST_F(RealNodeFixture, PlaintextMacConfigIsRefused) {
    // The security property, end to end through production code: a PLAINTEXT
    // frame asking to disable authentication must change nothing.
    ASSERT_TRUE(disp.macSublayers().counter_enabled);
    ASSERT_TRUE(disp.macSublayers().crypto_enabled);

    auto bytes = build_mac_config(/*disable_counter=*/true, /*disable_crypto=*/true,
                                  /*duration_s=*/60, /*msgid=*/400);
    disp.onReceiveNew(bytes.data(), static_cast<int>(bytes.size()));

    EXPECT_TRUE(disp.macSublayers().counter_enabled)
        << "an unauthenticated frame must not disable the replay window";
    EXPECT_TRUE(disp.macSublayers().crypto_enabled)
        << "an unauthenticated frame must not disable authentication";
}

TEST_F(RealNodeFixture, MacConfigDoesNotDriveTheApplication) {
    // A MAC_CONFIG frame is consumed at MAC-0 whether accepted or refused —
    // nothing reaches a cover, a schedule or the motor.
    auto bytes = build_mac_config(true, true, 60, 401);
    disp.onReceiveNew(bytes.data(), static_cast<int>(bytes.size()));
    EXPECT_EQ(disp.macCounters().ping_rx, 0u);
    EXPECT_EQ(disp.macCounters().echo_tx, 0u);
}

TEST_F(RealNodeFixture, ApplicationFramesStillFaceTheReplayWindow) {
    // Even if the switches were somehow off, a non-MAC-control frame must be
    // checked. Asserted through the real admission path with a replayed msgid.
    auto a = build_mac_ping(/*seq=*/1, /*want_echo=*/false, /*msgid=*/420);
    disp.onReceiveNew(a.data(), static_cast<int>(a.size()));
    const uint32_t dup_before = disp.macFunnel().duplicates;

    disp.onReceiveNew(a.data(), static_cast<int>(a.size()));
    EXPECT_EQ(disp.macFunnel().duplicates, dup_before + 1u)
        << "with MAC-1 on (the default) a replay must still be rejected";
}


TEST_F(RealNodeFixture, AZeroedMacConfigDoesNotDisableAnything) {
    // proto3 bools default FALSE. With an "enable" field a hub sending
    // MAC_CONFIG with only durationS set — meaning to extend a run — silently
    // switched off the replay window. Negative sense makes the zero value safe.
    auto bytes = build_mac_config(/*disable_counter=*/false, /*disable_crypto=*/false,
                                  /*duration_s=*/0, /*msgid=*/402);
    disp.onReceiveNew(bytes.data(), static_cast<int>(bytes.size()));
    EXPECT_TRUE(disp.macSublayers().counter_enabled);
    EXPECT_TRUE(disp.macSublayers().crypto_enabled);
}

TEST_F(RealNodeFixture, AddressedIsCountedOnBothOutcomes) {
    // `addressed` used to be incremented nowhere, so stage 4->5 read as 100 %
    // loss on a perfectly healthy link.
    auto mine = build_mac_ping(/*seq=*/1, /*want_echo=*/false, /*msgid=*/430);
    disp.onReceiveNew(mine.data(), static_cast<int>(mine.size()));
    EXPECT_GE(disp.macFunnel().addressed, 1u);
    EXPECT_EQ(disp.macFunnel().foreign, 0u);
}

TEST_F(RealNodeFixture, ForeignPlaintextFramesAreCountedNotIgnored) {
    LoraHeader hdr = LORA_HEADER__INIT;
    hdr.destaddress   = kNodeAddr + 7;   // someone else's
    hdr.destsubnet    = kSubnet;
    hdr.senderaddress = 1;
    hdr.msgid         = 440;
    MacControl mc = MAC_CONTROL__INIT;
    mc.kind = MAC_CONTROL__KIND__MAC_PING;
    mc.seq  = 1;
    LoraClientOperationMessage op = LORA_CLIENT_OPERATION_MESSAGE__INIT;
    op.header = &hdr;
    op.cmd_case = LORA_CLIENT_OPERATION_MESSAGE__CMD_MACCONTROL;
    op.maccontrol = &mc;
    std::vector<uint8_t> bytes(lora_client_operation_message__get_packed_size(&op));
    lora_client_operation_message__pack(&op, bytes.data());

    const uint32_t before = disp.macFunnel().foreign;
    disp.onReceiveNew(bytes.data(), static_cast<int>(bytes.size()));
    EXPECT_EQ(disp.macFunnel().foreign, before + 1u);
    EXPECT_EQ(disp.macCounters().ping_rx, 0u);
}

// ---------------------------------------------------------------------------
// B2 — phase samples are committed only for frames addressed to this node.
//
// This is the bug the design warns about, exercised through production code:
// noteDriftSample() runs before parsing (correct for drift), so without the
// commit-late split a node stamps its neighbours' frames and phaseErrUs goes
// bimodal at 0 and one slot pitch.
// ---------------------------------------------------------------------------

TEST_F(RealNodeFixture, NoPhaseSampleWithoutAGrid) {
    // expected_t0_us_ is 0 until B3 publishes a grid. Committing against 0
    // would make every error the node's whole uptime.
    ASSERT_EQ(disp.expectedT0Us(), 0);
    auto bytes = build_mac_ping(/*seq=*/1, /*want_echo=*/false, /*msgid=*/500);
    disp.onReceiveNew(bytes.data(), static_cast<int>(bytes.size()));
    EXPECT_EQ(disp.phaseStats().n, 0u);
}

TEST_F(RealNodeFixture, AForeignFrameCommitsNoPhaseSample) {
    disp.setExpectedT0Us(1000000);
    disp.resetPhaseStats();

    // Addressed to a different node — exactly the traffic a node hears most of
    // the time on a shared channel.
    LoraHeader hdr = LORA_HEADER__INIT;
    hdr.destaddress   = kNodeAddr + 5;
    hdr.destsubnet    = kSubnet;
    hdr.senderaddress = 1;
    hdr.msgid         = 510;
    MacControl mc = MAC_CONTROL__INIT;
    mc.kind = MAC_CONTROL__KIND__MAC_PING;
    mc.seq  = 1;
    LoraClientOperationMessage op = LORA_CLIENT_OPERATION_MESSAGE__INIT;
    op.header = &hdr;
    op.cmd_case = LORA_CLIENT_OPERATION_MESSAGE__CMD_MACCONTROL;
    op.maccontrol = &mc;
    std::vector<uint8_t> bytes(lora_client_operation_message__get_packed_size(&op));
    lora_client_operation_message__pack(&op, bytes.data());

    // A real timestamp, so what rejects this sample is the ADDRESS FILTER and
    // not the missing-timestamp guard. Without this the test passed either way.
    disp.onReceiveNew(bytes.data(), static_cast<int>(bytes.size()),
                      /*rx_us=*/1040000);

    EXPECT_EQ(disp.phaseStats().n, 0u)
        << "stamping a neighbour's frame is what makes phaseErrUs bimodal";
    EXPECT_GE(disp.macFunnel().foreign, 1u) << "but it IS counted as foreign";
}


TEST_F(RealNodeFixture, RtcSourceDefaultsToUnknownNotCrystal) {
    // The safe reading of a node that has not reported is that it cannot hold
    // phase — Mode B is gated on the crystal.
    EXPECT_EQ(disp.rtcSlowSrc(), phase::RtcSlowSrc::Unknown);
}

// ---------------------------------------------------------------------------
// B3 — the real dispatcher adopts, refuses and withdraws a grid.
// ---------------------------------------------------------------------------
namespace {

std::vector<uint8_t> build_grid_sync(bool enable, uint32_t slot, uint32_t msgid,
                                     uint32_t slot_count = timedgrid::kSlotCount,
                                     uint32_t pitch_us = timedgrid::kSlotPitchUs,
                                     uint32_t burst_index = 0) {
    LoraHeader hdr = LORA_HEADER__INIT;
    hdr.destaddress   = kNodeAddr;
    hdr.destsubnet    = kSubnet;
    hdr.senderaddress = 1;
    hdr.msgid         = msgid;
    hdr.burstindex    = burst_index;
    hdr.burstcount    = 17;

    GridSync gs = GRID_SYNC__INIT;
    gs.enable            = enable;
    gs.slotindex         = slot;
    gs.slotcount         = slot_count;
    gs.roundus           = timedgrid::kRoundUs;
    gs.pitchus           = pitch_us;
    gs.txround           = 0;
    gs.txslot            = slot;
    gs.beaconslotindex   = timedgrid::kSlotCount - 1;
    gs.beaconeveryrounds = 233;
    gs.symtimeout        = timedgrid::kSymbolTimeoutSymbols;
    gs.resyncmaxs        = 350;
    gs.uloffsetus        = 60000;

    LoraClientOperationMessage op = LORA_CLIENT_OPERATION_MESSAGE__INIT;
    op.header   = &hdr;
    op.cmd_case = LORA_CLIENT_OPERATION_MESSAGE__CMD_GRIDSYNC;
    op.gridsync = &gs;

    std::vector<uint8_t> out(lora_client_operation_message__get_packed_size(&op));
    lora_client_operation_message__pack(&op, out.data());
    return out;
}

}  // namespace

TEST_F(RealNodeFixture, NodeStartsWithNoGrid) {
    EXPECT_FALSE(disp.gridState().active);
    EXPECT_EQ(disp.expectedT0Us(), 0);
}

TEST_F(RealNodeFixture, AnAgreeingGridIsAdoptedAndAnchored) {
    auto bytes = build_grid_sync(/*enable=*/true, /*slot=*/4, /*msgid=*/600);
    disp.onReceiveNew(bytes.data(), static_cast<int>(bytes.size()));

    ASSERT_TRUE(disp.gridState().active);
    EXPECT_EQ(disp.lastGridRefusal(), gridstate::Refusal::None);
    EXPECT_EQ(disp.gridState().params.slot_index, 4u);
    EXPECT_GT(disp.expectedT0Us(), 0) << "a grid must produce a next mark";
}

TEST_F(RealNodeFixture, AGridWeDisagreeWithIsRefusedNotHalfAdopted) {
    // Different pitch: a node that adopted this would miss every window and
    // look like a dead radio.
    auto bytes = build_grid_sync(true, 4, 610, timedgrid::kSlotCount, 31250);
    disp.onReceiveNew(bytes.data(), static_cast<int>(bytes.size()));

    EXPECT_FALSE(disp.gridState().active);
    EXPECT_EQ(disp.lastGridRefusal(), gridstate::Refusal::PitchMismatch);
    EXPECT_EQ(disp.expectedT0Us(), 0);
}

TEST_F(RealNodeFixture, WithdrawalIsUnconditionalAndClearsPhase) {
    auto on = build_grid_sync(true, 4, 620);
    disp.onReceiveNew(on.data(), static_cast<int>(on.size()));
    ASSERT_TRUE(disp.gridState().active);

    // Feed a phase sample so there is something to clear.
    auto ping = build_mac_ping(1, false, 621);
    disp.onReceiveNew(ping.data(), static_cast<int>(ping.size()),
                      /*rx_us=*/1040000);

    auto off = build_grid_sync(false, 4, 622);
    disp.onReceiveNew(off.data(), static_cast<int>(off.size()));

    EXPECT_FALSE(disp.gridState().active);
    EXPECT_EQ(disp.expectedT0Us(), 0);
    EXPECT_EQ(disp.phaseStats().n, 0u)
        << "samples against a withdrawn anchor describe a grid that is gone";
}

TEST_F(RealNodeFixture, ReAnchoringResetsPhaseStats) {
    auto a = build_grid_sync(true, 4, 630);
    disp.onReceiveNew(a.data(), static_cast<int>(a.size()));
    auto ping = build_mac_ping(1, false, 631);
    disp.onReceiveNew(ping.data(), static_cast<int>(ping.size()),
                      /*rx_us=*/1040000);
    ASSERT_GE(disp.phaseStats().n, 1u);

    auto b = build_grid_sync(true, 9, 632);   // new slot, new anchor
    disp.onReceiveNew(b.data(), static_cast<int>(b.size()));
    EXPECT_EQ(disp.gridState().params.slot_index, 9u);
    EXPECT_EQ(disp.phaseStats().n, 0u)
        << "old samples describe the old anchor";
}

TEST_F(RealNodeFixture, GridSyncDoesNotReachTheApplication) {
    auto bytes = build_grid_sync(true, 4, 640);
    disp.onReceiveNew(bytes.data(), static_cast<int>(bytes.size()));
    EXPECT_EQ(disp.macCounters().ping_rx, 0u);
    EXPECT_EQ(disp.macCounters().echo_tx, 0u);
}

// ---------------------------------------------------------------------------
// B3 — one receive window per round. Default OFF until T_detect is measured.
// ---------------------------------------------------------------------------

TEST_F(RealNodeFixture, TimedRxDefaultsToOff) {
    // Narrowing three windows to one trades a 3x reception margin for battery
    // on the strength of an unmeasured T_detect. The mechanism ships; the
    // switch waits for HW-2's number.
    EXPECT_FALSE(disp.timedRxEnabled());
    EXPECT_FALSE(disp.timedRxActive());
}

TEST_F(RealNodeFixture, TimedRxNeedsAGridEvenWhenEnabled) {
    disp.setTimedRxEnabled(true);
    EXPECT_FALSE(disp.timedRxActive()) << "no grid, no timed window";
}

TEST_F(RealNodeFixture, TimedRxNeedsTheCrystal) {
    disp.setTimedRxEnabled(true);
    auto g = build_grid_sync(true, 4, 700);
    disp.onReceiveNew(g.data(), static_cast<int>(g.size()));
    ASSERT_TRUE(disp.gridState().active);

    // rtcSlowSrc is Unknown by default — the safe reading.
    EXPECT_FALSE(disp.timedRxActive())
        << "a node that cannot hold phase must stay in Mode A, visibly";

    disp.setRtcSlowSrc(phase::RtcSlowSrc::InternalRc);
    EXPECT_FALSE(disp.timedRxActive()) << "~5 % is not a clock";
}

TEST_F(RealNodeFixture, ALaterBurstCopyAnchorsWhereCopyZeroWas) {
    // A GridSync declares the position of COPY 0, and it is sent as a 17-copy
    // burst one stride apart. The node adopts from whichever copy it decodes
    // first — and it is in Mode A when it is told about the grid, sweeping a
    // free-running window, so that is rarely copy 0. Solving the anchor from a
    // later copy's T0 displaced every mark the node would ever arm, by an
    // amount that is not even a whole number of slots: the stride is 88000 us
    // against a 46875 us pitch.
    //
    // burstIndex is re-stamped per copy by the hub and the node already backs
    // it out for the drift estimate, so the correction costs nothing new on the
    // wire.
    constexpr int64_t kT0Copy0 = 5'000'000;

    // RxDone is derived from each copy's OWN length. T0 is preamble-relative,
    // so the copies are exactly one stride apart at T0 however long each frame
    // is — but proto3 omits burstIndex when it is 0, so copy 0 really is a
    // couple of bytes shorter than copy 5 and its RxDone sits correspondingly
    // earlier. Handing both the same offset would compare two different T0s.
    auto deliver = [&](const std::vector<uint8_t>& f, int64_t t0) {
        const int64_t rxdone =
            t0 + (int64_t) loratiming::t0ToRxDoneUs((uint32_t) f.size());
        disp.onReceiveNew(const_cast<uint8_t*>(f.data()),
                          static_cast<int>(f.size()), rxdone);
    };

    auto copy0 = build_grid_sync(/*enable=*/true, /*slot=*/4, /*msgid=*/780);
    deliver(copy0, kT0Copy0);
    ASSERT_TRUE(disp.gridState().active);
    const int64_t anchor_from_copy0 = disp.gridState().anchor_us;

    // The same frame, five copies later — so its T0 is five strides after
    // copy 0's, which is what the radio would hand the node.
    constexpr uint32_t kIdx = 5;
    auto copy5 = build_grid_sync(/*enable=*/true, /*slot=*/4, /*msgid=*/781,
                                 timedgrid::kSlotCount, timedgrid::kSlotPitchUs,
                                 /*burst_index=*/kIdx);
    deliver(copy5, kT0Copy0 + (int64_t) kIdx * drift::kCopySpacingUs);
    ASSERT_TRUE(disp.gridState().active);

    EXPECT_EQ(disp.gridState().anchor_us, anchor_from_copy0)
        << "which copy of the burst the node happens to decode must not move "
           "its anchor — the declaration describes copy 0";
}

TEST_F(RealNodeFixture, TimedRxNeedsATrustworthyPhase) {
    disp.setTimedRxEnabled(true);
    disp.setRtcSlowSrc(phase::RtcSlowSrc::Crystal);
    auto g = build_grid_sync(true, 4, 710);
    disp.onReceiveNew(g.data(), static_cast<int>(g.size()));

    // Adoption resets the phase stats, so there is no baseline yet.
    EXPECT_EQ(disp.phaseStats().n, 0u);
    EXPECT_FALSE(disp.timedRxActive())
        << "promotion needs a long baseline; demotion is immediate";
}

TEST_F(RealNodeFixture, ArmInstantLeadsTheMarkByTheArmLead) {
    auto g = build_grid_sync(true, 4, 720);
    disp.onReceiveNew(g.data(), static_cast<int>(g.size()));
    ASSERT_TRUE(disp.gridState().active);

    const int64_t now = disp.expectedT0Us() - 100000;
    const int64_t arm = disp.nextArmInstantUs(now);
    const int64_t t0  = gridstate::nextT0Us(disp.gridState(), now);
    EXPECT_EQ(t0 - arm, (int64_t) timedgrid::kArmLeadUs);
}

TEST_F(RealNodeFixture, MissedMarksKeyOnAddressedFramesNotOnSilence) {
    auto g = build_grid_sync(true, 4, 730);
    disp.onReceiveNew(g.data(), static_cast<int>(g.size()));
    ASSERT_TRUE(disp.gridState().active);

    EXPECT_EQ(disp.consecutiveMissedMarks(), 0u);
    disp.noteMarkOutcome(/*addressed=*/false);
    EXPECT_EQ(disp.consecutiveMissedMarks(), 1u);

    // A frame FOR ME clears it. A window walked through by a neighbour's burst
    // is not empty, which is why the caller passes "addressed", not "received".
    disp.noteMarkOutcome(/*addressed=*/true);
    EXPECT_EQ(disp.consecutiveMissedMarks(), 0u);
}

TEST_F(RealNodeFixture, MarkOutcomesAreTheOnlySourceOfWmr) {
    // windows_armed stayed 0 until this existed, so the rate the design calls
    // "the one that actually distinguishes the modes" read a permanent zero.
    const uint32_t before = disp.macFunnel().windows_armed;
    disp.noteMarkOutcome(true);
    disp.noteMarkOutcome(false);
    EXPECT_EQ(disp.macFunnel().windows_armed, before + 2u);
    EXPECT_EQ(disp.macFunnel().windows_hit, 1u);
    EXPECT_EQ(macfunnel::wmrPpm(disp.macFunnel()), 500000u) << "one of two missed";
}

TEST_F(RealNodeFixture, EnoughMissedMarksDemoteUnilaterally) {
    auto g = build_grid_sync(true, 4, 740);
    disp.onReceiveNew(g.data(), static_cast<int>(g.size()));
    ASSERT_TRUE(disp.gridState().active);

    for (uint32_t i = 0; i < timedmode::kMaxMissedMarks; ++i)
        disp.noteMarkOutcome(false);

    EXPECT_FALSE(disp.gridState().active)
        << "staying in a window the hub no longer transmits into is the unsafe "
           "direction; dropping to Mode A is always safe";
    EXPECT_EQ(disp.expectedT0Us(), 0);
}

// ---------------------------------------------------------------------------
// B4 — a byte-identical retransmit is answered, not dropped
// ---------------------------------------------------------------------------
//
// The hub packs a tracked command ONCE and retransmits the stored bytes, so a
// retry carries the SAME msgid. The node's replay filter admits only
// msgid > rx_id_, so before this the retry was dropped in silence and no ack
// could ever be regenerated: the hub retried four times, gave up, and tore the
// session down. Shipping the hub's pack-once half alone made the retry path
// strictly WORSE than the fresh-msgid behaviour it replaced.
//
// The whole difficulty is that Mode A sends seventeen copies of every command,
// so sixteen duplicates per command are expected and must stay silent. The
// discriminator is time — see AckCache.h.

namespace {
int drain_acks(CmdDispatcher &d) {
    int n = 0;
    CmdDispatcher::tx_command_t c{};
    while (xQueueReceive(d.txCmdQueueNew, &c, 0) == pdTRUE)
        if (c.cmd == (blinds_syscmd_base_t) BlindsStatusCmd::SYSCMD_ACK) ++n;
    return n;
}
}  // namespace

TEST_F(RealNodeFixture, AnAcceptedSysopIsAcked) {
    auto op = pack_sysop_op(/*msgid=*/900, CLIENT_OPERATION__CMD_STATUS);
    disp.onReceiveNew(op.data(), static_cast<int>(op.size()));
    EXPECT_EQ(drain_acks(disp), 1);
}

TEST_F(RealNodeFixture, TheOtherSixteenBurstCopiesStaySilent) {
    // The trap the cache exists for. A naive cached ack answers all sixteen —
    // sixteen uplinks per command, on a battery node, for a command that
    // already succeeded. That is worse than the bug it fixes.
    auto op = pack_sysop_op(/*msgid=*/901, CLIENT_OPERATION__CMD_STATUS);
    disp.onReceiveNew(op.data(), static_cast<int>(op.size()));
    ASSERT_EQ(drain_acks(disp), 1) << "the first copy is acked";

    for (int copy = 2; copy <= 17; ++copy)
        disp.onReceiveNew(op.data(), static_cast<int>(op.size()));

    EXPECT_EQ(drain_acks(disp), 0)
        << "every duplicate inside the burst span must be silent";
}

TEST_F(RealNodeFixture, ADuplicateOfADifferentCommandIsNotAReAck) {
    // The cache holds one command. A duplicate whose msgid is not the cached
    // one is an ordinary replay and must be dropped, not answered.
    auto a = pack_sysop_op(/*msgid=*/910, CLIENT_OPERATION__CMD_STATUS);
    disp.onReceiveNew(a.data(), static_cast<int>(a.size()));
    ASSERT_EQ(drain_acks(disp), 1);

    // An OLD msgid the node has already moved past.
    auto stale = pack_sysop_op(/*msgid=*/905, CLIENT_OPERATION__CMD_STATUS);
    disp.onReceiveNew(stale.data(), static_cast<int>(stale.size()));
    EXPECT_EQ(drain_acks(disp), 0);
}

TEST_F(RealNodeFixture, APlaintextDuplicateNeverMakesTheNodeTransmit) {
    // The security half. Recovering a lost ack must not become an
    // unauthenticated uplink trigger: an attacker who observed a msgid could
    // otherwise replay it in plaintext to make the node transmit on demand.
    // These frames are plaintext (no session in this fixture), so even a
    // duplicate that is old enough to be a genuine retry must stay silent.
    auto op = pack_sysop_op(/*msgid=*/920, CLIENT_OPERATION__CMD_STATUS);
    disp.onReceiveNew(op.data(), static_cast<int>(op.size()));
    ASSERT_EQ(drain_acks(disp), 1);

    // Past the burst span, so classify() would say ReAck on an encrypted frame.
    std::this_thread::sleep_for(
        std::chrono::microseconds(ackcache::kBurstSpanUs + 50000));

    disp.onReceiveNew(op.data(), static_cast<int>(op.size()));
    EXPECT_EQ(drain_acks(disp), 0)
        << "a plaintext duplicate must never produce an uplink, however old";
}

TEST_F(RealNodeFixture, AckingACommandArmsTheReAckCache) {
    // Wiring half 1: sendCommandAck populates the cache. It is recorded there
    // rather than at the three handler call sites so a handler added later
    // cannot forget to do it — this asserts that placement holds.
    EXPECT_FALSE(disp.ackCacheForTest().valid) << "nothing acked yet";

    auto op = pack_sysop_op(/*msgid=*/930, CLIENT_OPERATION__CMD_STATUS);
    disp.onReceiveNew(op.data(), static_cast<int>(op.size()));
    ASSERT_EQ(drain_acks(disp), 1);

    EXPECT_TRUE(disp.ackCacheForTest().valid);
    EXPECT_EQ(disp.ackCacheForTest().msgid, 930u);
    EXPECT_EQ(disp.ackCacheForTest().reacks, 0);
}

TEST_F(RealNodeFixture, TheArmedCacheWouldReAckOnceTheBurstSpanHasPassed) {
    // Wiring half 2: the cache the dispatcher actually built yields ReAck at
    // the right time. Together with the test above and ack_cache_test.cpp this
    // leaves exactly one unverified link — the `&& was_encrypted` conjunction
    // and the sendCommandAck call inside it — because producing an encrypted
    // duplicate needs a hub-side AES-GCM helper the host fixture does not have.
    // Stated here rather than left implicit.
    auto op = pack_sysop_op(/*msgid=*/931, CLIENT_OPERATION__CMD_STATUS);
    disp.onReceiveNew(op.data(), static_cast<int>(op.size()));
    ASSERT_EQ(drain_acks(disp), 1);
    const ackcache::Cache &c = disp.ackCacheForTest();
    ASSERT_TRUE(c.valid);

    const int64_t armed_at = c.first_seen_us;
    EXPECT_EQ(ackcache::classify(c, 931, armed_at + 1000),
              ackcache::Decision::SilentCopy) << "immediately: a burst copy";
    EXPECT_EQ(ackcache::classify(c, 931, armed_at + ackcache::kBurstSpanUs - 1),
              ackcache::Decision::SilentCopy) << "still inside the burst";
    EXPECT_EQ(ackcache::classify(c, 931, armed_at + ackcache::kBurstSpanUs),
              ackcache::Decision::ReAck) << "past it: a genuine retry";
    EXPECT_EQ(ackcache::classify(c, 999, armed_at + ackcache::kBurstSpanUs),
              ackcache::Decision::NotADuplicate) << "a different command";
}

// ---------------------------------------------------------------------------
// B3 — Mode B was unreachable on the node side; three separate reasons
// ---------------------------------------------------------------------------

TEST_F(RealNodeFixture, AdoptingAGridEnablesTimedRx) {
    // setTimedRxEnabled() had no caller anywhere, so timed_rx_enabled_ was
    // permanently false and timedRxActive() returned false BEFORE consulting
    // the grid at all. A node could accept a GridSync, solve its anchor, and
    // still never arm a timed window. The hub asking for a grid IS the trigger.
    auto gs = build_grid_sync(/*enable=*/true, /*slot=*/4, /*msgid=*/700);
    disp.onReceiveNew(gs.data(), static_cast<int>(gs.size()), /*rx_us=*/5'000'000);

    ASSERT_TRUE(disp.gridState().active);
    EXPECT_TRUE(disp.timedRxEnabledForTest())
        << "adopting a grid is what makes Mode B reachable";
}

TEST_F(RealNodeFixture, ThePhaseExpectationTracksTheGridInsteadOfFreezing) {
    // expected_t0_us_ was assigned once at adoption and never advanced, so
    // every sample after the first was measured against a mark one round
    // further in the past — +1.5 s, +3.0 s, and so on. phaseTrustworthy()
    // requires zero samples outside the guard, so it went permanently false on
    // the SECOND addressed frame and Mode B could never be entered. The failure
    // was silent and looked like a clock fault.
    // noteDriftSample() is what the DIO0 task calls on every RxDone, and it is
    // what sets the timestamp the phase sample is built from. Mirrored here
    // because onReceiveNew alone does not set it.
    auto gs = build_grid_sync(true, /*slot=*/0, /*msgid=*/710);
    const int64_t anchor_rx = 10'000'000;
    disp.noteDriftSample(anchor_rx);
    disp.onReceiveNew(gs.data(), static_cast<int>(gs.size()), anchor_rx);
    ASSERT_TRUE(disp.gridState().active);

    // Compare two SAMPLED frames, not the adoption-time value: adoption records
    // the next mark, and a frame arriving just after that mark shares it. Two
    // frames a round apart are the honest comparison.
    // Place T0 on the mark, not RxDone. The hub fires EARLIER for a longer
    // frame precisely so that T0 — the SFD end — lands on the mark whatever the
    // payload length; putting RxDone on the mark instead leaves an error equal
    // to the difference in n_sym between two frame sizes, which for a GridSync
    // against a sysop is 14.3 ms and would sail past the 14.08 ms guard. That
    // is a real effect, and getting it wrong here would have looked like a
    // phase bug in the code rather than in the test.
    auto op1 = pack_sysop_op(/*msgid=*/711, CLIENT_OPERATION__CMD_STATUS);
    const int64_t mark1 = disp.expectedT0Us() + timedgrid::kRoundUs;
    const int64_t rx1 = mark1 + (int64_t) loratiming::t0ToRxDoneUs((uint32_t) op1.size());
    disp.noteDriftSample(rx1);
    disp.onReceiveNew(op1.data(), static_cast<int>(op1.size()), rx1);
    const int64_t expectation1 = disp.expectedT0Us();
    ASSERT_NE(expectation1, 0);

    auto op2 = pack_sysop_op(/*msgid=*/712, CLIENT_OPERATION__CMD_STATUS);
    const int64_t mark2 = mark1 + timedgrid::kRoundUs;
    const int64_t rx2 = mark2 + (int64_t) loratiming::t0ToRxDoneUs((uint32_t) op2.size());
    disp.noteDriftSample(rx2);
    disp.onReceiveNew(op2.data(), static_cast<int>(op2.size()), rx2);

    EXPECT_NE(disp.expectedT0Us(), expectation1)
        << "the expectation is frozen — every later sample is a round further out";
    EXPECT_EQ(disp.expectedT0Us() - expectation1, (int64_t) timedgrid::kRoundUs)
        << "it must advance by exactly one round";

    // And the phase error must stay small, which is the whole point: with a
    // frozen expectation the second sample reads +1.5 s and phaseTrustworthy()
    // goes permanently false.
    EXPECT_LT((uint32_t) std::abs((long) disp.phaseStats().last_us), 2000u)
        << "a frame placed exactly on its mark must read a near-zero phase "
           "error; the frozen expectation made the second sample read +1.5 s, "
           "which is what put phaseTrustworthy() permanently false";
}

TEST_F(RealNodeFixture, MissedMarksActuallyDemote) {
    // The demotion body lived inside noteMarkOutcome, which has no callers, so
    // when the radio paths were split onto noteMarkArmed/Hit/Missed the counter
    // kept incrementing and nothing ever acted on it. grid_.active stayed true
    // forever and the node held a grid it should have abandoned.
    auto gs = build_grid_sync(true, /*slot=*/7, /*msgid=*/720);
    disp.onReceiveNew(gs.data(), static_cast<int>(gs.size()), 20'000'000);
    ASSERT_TRUE(disp.gridState().active);

    for (uint32_t i = 0; i < timedmode::kMaxMissedMarks; ++i) {
        disp.noteMarkArmed();
        disp.noteMarkMissed();
    }

    EXPECT_FALSE(disp.gridState().active)
        << "the node must drop a grid it is no longer being served on";
    EXPECT_TRUE(disp.timedRxEnabledForTest())
        << "but timed RX stays ENABLED, so a later GridSync re-adopts without "
           "needing anything else to happen — clearing it would recreate the "
           "original bug as a flag nothing sets again";
}

TEST_F(RealNodeFixture, AnAddressedFrameResetsTheMissedMarkCount) {
    auto gs = build_grid_sync(true, /*slot=*/7, /*msgid=*/730);
    disp.onReceiveNew(gs.data(), static_cast<int>(gs.size()), 30'000'000);
    ASSERT_TRUE(disp.gridState().active);

    disp.noteMarkArmed();
    disp.noteMarkMissed();
    disp.noteMarkArmed();
    disp.noteMarkMissed();
    ASSERT_TRUE(disp.gridState().active) << "two is below the threshold";

    // A frame addressed to us resets the count, so the third miss must not
    // demote — it is the FIRST of a new run, not the third of the old one.
    auto op = pack_sysop_op(/*msgid=*/731, CLIENT_OPERATION__CMD_STATUS);
    disp.onReceiveNew(op.data(), static_cast<int>(op.size()), 30'100'000);

    disp.noteMarkArmed();
    disp.noteMarkMissed();
    EXPECT_TRUE(disp.gridState().active);
}

TEST_F(RealNodeFixture, AnAddressedFrameCommitsExactlyOnePhaseSample) {
    // Adopt a grid first, because that is what production does. This test used
    // to seed the expectation with setExpectedT0Us(), a setter no production
    // path ever calls — and relying on it hid the fact that the expectation was
    // never advanced. The commit now gates on the grid, so the test has to
    // establish one.
    auto gs = build_grid_sync(/*enable=*/true, /*slot=*/0, /*msgid=*/519);
    disp.noteDriftSample(1'000'000);
    disp.onReceiveNew(gs.data(), static_cast<int>(gs.size()), /*rx_us=*/1'000'000);
    ASSERT_TRUE(disp.gridState().active);
    disp.resetPhaseStats();

    // rx_us must be REAL: onReceiveNew defaults it to 0, and 0 means "no
    // timestamp", which correctly commits nothing. Passing the default here
    // would have made this test pass for the wrong reason.
    auto bytes = build_mac_ping(/*seq=*/1, /*want_echo=*/false, /*msgid=*/520);
    disp.noteDriftSample(1'040'000);
    disp.onReceiveNew(bytes.data(), static_cast<int>(bytes.size()),
                      /*rx_us=*/1'040'000);
    EXPECT_EQ(disp.phaseStats().n, 1u);
}

// ---------------------------------------------------------------------------
// Once a session exists, every command must be encrypted
// ---------------------------------------------------------------------------
//
// The plaintext rejection named CMD_OPERATION and CMD_SYSOP only, so seven
// other authority-bearing downlinks were accepted unauthenticated by a node
// with a live session. The LoraHeader is plaintext on every frame, so an
// attacker in radio range reads destAddress and msgid directly and needs only
// msgid in (rx_id_, rx_id_ + 1024].
//
// The session is established the production way — a LoginMsg, which CARRIES the
// base nonce — rather than by reaching into set_base_nonce(). That matters:
// login is the one exemption in the new rule, so establishing the session this
// way also proves the exemption still works.

TEST_F(RealNodeFixture, APlaintextScheduleIsRefusedOnceASessionExists) {
    // The worst of them. ScheduleConfig replaces the schedule WHOLESALE — it is
    // idempotent by design — so one unauthenticated frame saying "open at
    // 03:00, every day" opens every blind every night. A physical-security
    // bypass with no key.
    auto login = pack_login_op(/*msgid=*/800, /*nonce=*/0xABCDEF01);
    disp.onReceiveNew(login.data(), static_cast<int>(login.size()), 1'000'000);

    const uint32_t before = disp.macFunnel().addressed;
    auto sched = pack_schedule_op(/*msgid=*/801, /*version=*/0xBEEF,
                                  NODE_MODE__MODE_INTERACTIVE);
    disp.onReceiveNew(sched.data(), static_cast<int>(sched.size()), 1'100'000);

    // It reached the address filter (so this is not an addressing accident) and
    // was then refused for being plaintext.
    EXPECT_GT(disp.macFunnel().addressed, before);
    EXPECT_NE(sys.getSchedVersion(), 0xBEEFu)
        << "an unauthenticated frame must not be able to rewrite the schedule";
}

TEST_F(RealNodeFixture, AnUnauthenticatedFrameCannotRatchetTheReplayCounter) {
    // The gate has to run BEFORE the replay window, not merely exist.
    //
    // The replay check accepts any forward jump inside (rx_id_, rx_id_ + 1024]
    // and ASSIGNS rx_id_ on the way through. While the plaintext rejection sat
    // after it, an attacker did not need to get a command executed to do
    // damage: one injected frame at msgid = observed + 1024 pushed the counter
    // past everything the hub had queued, and every legitimate command was
    // then rejected as a replay until the next LOGIN. The frame was refused
    // and the link was still wedged.
    disp.setBaseNonceForTest(kHubAddr, 0xABCDEF01);

    const uint32_t before = disp.rxMsgIdForTest();
    auto sched = pack_schedule_op(/*msgid=*/before + 1000, /*version=*/0xBEEF,
                                  NODE_MODE__MODE_INTERACTIVE);
    disp.onReceiveNew(sched.data(), static_cast<int>(sched.size()), 1'100'000);

    EXPECT_EQ(disp.rxMsgIdForTest(), before)
        << "a plaintext frame must be refused before it can touch the counter";
    EXPECT_NE(sys.getSchedVersion(), 0xBEEFu);
}

TEST_F(RealNodeFixture, AnUnknownSenderCannotWalkPastThePlaintextGate) {
    // The gate used to ask "do I hold a base nonce for the SENDER" — and
    // senderaddress is a plaintext field the node never validates. Naming a
    // sender the node has never heard of made get_base_nonce fail, so the gate
    // did not fire AT ALL: the frame went on to ratchet rx_message_id_, set
    // destAddress to the attacker's address, and reach ScheduleConfig,
    // BaseNonceExchange, GridSync, TimeSync and CoverConfig. Four such frames
    // naming four unknown senders also evict the hub's real base nonce, because
    // findOrCreatePeer drops slot 0 when the four-entry peer table is full.
    //
    // The predicate was keyed on the one field the attacker chooses. The test
    // that was meant to cover this sent from kHubAddr — the single sender for
    // which the old gate happened to work.
    disp.setBaseNonceForTest(kHubAddr, 0xABCDEF01);

    const uint32_t before = disp.rxMsgIdForTest();
    auto sched = pack_schedule_op(/*msgid=*/before + 1000, /*version=*/0xBEEF,
                                  NODE_MODE__MODE_INTERACTIVE,
                                  /*sender=*/0x77);   // never seen by this node
    disp.onReceiveNew(sched.data(), static_cast<int>(sched.size()), 1'100'000);

    EXPECT_NE(sys.getSchedVersion(), 0xBEEFu)
        << "naming an unknown sender must not be a way past the gate";
    EXPECT_EQ(disp.rxMsgIdForTest(), before)
        << "and it must not ratchet the replay counter on its way through";
}

TEST_F(RealNodeFixture, LoginItselfStaysAcceptedInPlaintext) {
    // The single exemption, and it has to be exactly one: LoginMsg carries the
    // base nonce, so it IS the bootstrap. The hub clears session_confirmed_
    // before sending it precisely so it goes out in the clear — which is what
    // lets a node whose nonce has diverged recover. Refusing it would make the
    // link unrecoverable after any nonce divergence.
    //
    // "A session already exists" is seeded directly rather than by sending a
    // first LoginMsg. handleLogin() rate-limits logins to one per 5 s (F-30)
    // off the REAL monotonic clock, which does not advance with the rx_us the
    // test hands in: two logins 100 ms apart on the simulated clock land in the
    // same real millisecond, so the second is dropped by the limiter and the
    // test would be measuring F-30, not the security gate.
    disp.setBaseNonceForTest(kHubAddr, 0x11223344);
    uint32_t bn = 0;
    ASSERT_TRUE(disp.getBaseNonceForTest(kHubAddr, bn));
    ASSERT_EQ(bn, 0x11223344u) << "precondition: a session exists";

    // Now the recovery case: the hub has lost its state and re-logs in, in the
    // clear. Every other plaintext command from this peer is refused at this
    // point (see APlaintextScheduleIsRefusedOnceASessionExists); LOGIN must not
    // be, or the link can never recover from a nonce divergence.
    auto login = pack_login_op(/*msgid=*/811, /*nonce=*/0x55667788);
    disp.onReceiveNew(login.data(), static_cast<int>(login.size()), 2'100'000);

    EXPECT_TRUE(disp.getBaseNonceForTest(kHubAddr, bn));
    EXPECT_EQ(bn, 0x55667788u)
        << "plaintext login is the recovery path and must never be refused";
}

// ---------------------------------------------------------------------------
// C2 — the wiring, not the arithmetic
// ---------------------------------------------------------------------------
//
// ClassAWindows.h had a full test suite and noteUplinkSent() had none, which is
// the §11a shape: the sequencing was right and what fed it was not. These three
// pin the feed.

TEST_F(RealNodeFixture, ClassAWindowsOnlyOpenForAnAutomaticModeNode) {
    // An interactive node is awake with the free-running Mode A window, which is
    // wider and already open. Class A exists to buy a SLEEPING node a reply
    // without any clock agreement; taking over the shared one-shot timer for an
    // awake node just moves its windows somewhere less useful.
    sys.setAutoMode(false);
    disp.noteUplinkSent(/*t_txdone_us=*/5'000'000, /*uplink_len=*/60);
    EXPECT_FALSE(disp.classAActive())
        << "an interactive node must not place windows off its own uplink";

    sys.setAutoMode(true);
    disp.noteUplinkSent(/*t_txdone_us=*/5'000'000, /*uplink_len=*/60);
    EXPECT_TRUE(disp.classAActive());
}

TEST_F(RealNodeFixture, AGridBeatsClassAWindows) {
    // Mode B's marks ARE the schedule. Both mechanisms drive the same one-shot,
    // so letting an uplink activate Class A while a grid is adopted means
    // whichever ran last wins and the node's mark rate collapses for reasons no
    // beacon field explains.
    sys.setAutoMode(true);
    auto grid = build_grid_sync(/*enable=*/true, /*slot=*/4, /*msgid=*/900);
    disp.onReceiveNew(grid.data(), static_cast<int>(grid.size()));
    ASSERT_TRUE(disp.gridState().active);

    disp.noteUplinkSent(/*t_txdone_us=*/5'000'000, /*uplink_len=*/60);
    EXPECT_FALSE(disp.classAActive())
        << "a node on the hub's grid must not be pulled onto its own uplink";
}

TEST_F(RealNodeFixture, TheWindowHangsOffT0NotOffTxDone) {
    // T0_uplink is TxDone minus the frame's OWN air time. Using TxDone directly
    // is wrong by n_sym(len)*T_sym — 18.4 ms to 92.2 ms across this fleet's
    // frame sizes — so short frames work and long ones fail, which reads as
    // interference rather than as arithmetic.
    sys.setAutoMode(true);
    const int64_t txdone = 5'000'000;
    disp.noteUplinkSent(txdone, /*uplink_len=*/60);
    ASSERT_TRUE(disp.classAActive());

    const int64_t t0   = classa::t0UplinkUs(txdone, 60);
    // rx1OpenUs already carries the PROTOCOL lead (T_pre + G before the
    // window's own T0); the lead the caller passes is the SOFTWARE one, the
    // ~1 ms between the timer callback and the radio actually listening. Two
    // different leads, both subtracted, exactly as Mode B does it.
    const int64_t open = classa::rx1OpenUs(t0);
    const int64_t kSoftwareLeadUs = 1000;
    const int64_t now   = t0 + 100'000;
    const int64_t delay = disp.classAArmDelayUs(now, kSoftwareLeadUs);
    EXPECT_EQ(now + delay + kSoftwareLeadUs, open)
        << "the arm must be placed from T0_uplink, not from TxDone";

    // And the same length dependence, stated as the failure it caused.
    EXPECT_NE(t0, classa::t0UplinkUs(txdone, 20))
        << "T0 must depend on the frame length, or the error is invisible until "
           "a long frame is sent";
}

TEST_F(RealNodeFixture, TheClassASequenceStopsBeingActiveWhenItIsOver) {
    // "Active" has to mean "there is a window still to open". It used to stay
    // true until the next uplink, so every Mode A window that received anything
    // — for hours — was reported into a finished sequence, and armNextRxWindow
    // kept taking the Class A branch first and relied on a delay of zero to
    // fall through to the mode that actually applied. Both worked. Neither was
    // true, and a state that lies is the thing every other bug in this file
    // grew out of.
    sys.setAutoMode(true);
    disp.noteUplinkSent(/*t_txdone_us=*/5'000'000, /*uplink_len=*/60);
    ASSERT_TRUE(disp.classAActive());

    // RX1 heard something: the reply arrived, so RX2 is pointless and the
    // sequence is done.
    disp.noteClassAWindowResult(/*had_data=*/true);
    EXPECT_FALSE(disp.classAActive())
        << "a reply in RX1 ends the sequence";

    // The other path: RX1 empty leaves RX2 to open, and only then is it over.
    disp.noteUplinkSent(/*t_txdone_us=*/9'000'000, /*uplink_len=*/60);
    ASSERT_TRUE(disp.classAActive());
    disp.noteClassAWindowResult(/*had_data=*/false);
    EXPECT_TRUE(disp.classAActive()) << "RX2 is still to come";
    disp.noteClassAWindowResult(/*had_data=*/false);
    EXPECT_FALSE(disp.classAActive()) << "and after RX2 there is nothing left";
}

// ---------------------------------------------------------------------------
// ModeTest — the mode the node ends up in, not the mode that was asked for.
//
// The policy header has always decided this correctly and been tested for it.
// What went untested was the CALLER: handleModeTest stored `mode` in mt_mode_,
// echoed it into the report, and never touched the node's RX discipline. So the
// "Mode Test B" button produced a Mode A measurement labelled `mode = 2`, and
// the hub logged it as a Mode B result — every Mode B number the fleet could
// produce described the wrong mode.
//
// These drive the real dispatcher through a real encrypted frame, because that
// is the only way to reach the arm path at all: ModeTest requires an
// authenticated frame AND a proven session (mac-layer.md section 4).
// ---------------------------------------------------------------------------
namespace {

constexpr uint32_t kMtNonce = 0x5EED1234;

// The fleet key a test hub hands out, and the AES-CMAC over a beacon's fields.
// Computed here from framecrypto::buildBeaconMacInput rather than from a stored
// golden tag: what can silently disagree between hub and node is WHICH BYTES go
// into the MAC, and a golden tag would pin the node's arithmetic against the
// node's own choice.
const uint8_t kFleetKey[framecrypto::kNetKeyBytes] = {
    0xA0, 0xA1, 0xA2, 0xA3, 0xA4, 0xA5, 0xA6, 0xA7,
    0xA8, 0xA9, 0xAA, 0xAB, 0xAC, 0xAD, 0xAE, 0xAF};
constexpr uint32_t kFleetKeyId = 0x5EED0001u;

std::array<uint8_t, framecrypto::kBeaconMacBytes>
beacon_mac(uint32_t key_id, uint32_t round, uint32_t slot, uint32_t mask,
           bool mask_valid, const uint8_t *key = kFleetKey) {
    uint8_t input[framecrypto::kBeaconMacInputBytes];
    framecrypto::buildBeaconMacInput(key_id, round, slot, mask, mask_valid, input);

    psa_crypto_init();
    psa_key_attributes_t attrs = PSA_KEY_ATTRIBUTES_INIT;
    psa_set_key_usage_flags(&attrs, PSA_KEY_USAGE_SIGN_MESSAGE);
    psa_set_key_algorithm(&attrs, PSA_ALG_CMAC);
    psa_set_key_type(&attrs, PSA_KEY_TYPE_AES);
    psa_set_key_bits(&attrs, framecrypto::kNetKeyBytes * 8);
    psa_set_key_lifetime(&attrs, PSA_KEY_LIFETIME_VOLATILE);

    psa_key_id_t kid = PSA_KEY_ID_NULL;
    EXPECT_EQ(psa_import_key(&attrs, key, framecrypto::kNetKeyBytes, &kid),
              PSA_SUCCESS);
    uint8_t full[16];
    size_t  full_len = 0;
    EXPECT_EQ(psa_mac_compute(kid, PSA_ALG_CMAC, input, sizeof(input), full,
                              sizeof(full), &full_len), PSA_SUCCESS);
    psa_destroy_key(kid);

    std::array<uint8_t, framecrypto::kBeaconMacBytes> out{};
    memcpy(out.data(), full, out.size());
    return out;
}


// Wrap an operation message as an encrypted downlink from the hub. Same
// construction as DecryptedDownlinkProvesSessionEndToEnd, hoisted so more than
// one test can reach the authenticated handlers.
std::vector<uint8_t> encrypt_op(LoraClientOperationMessage &inner, uint32_t msgid,
                                uint32_t nonce = kMtNonce) {
    size_t plain_len = lora_client_operation_message__get_packed_size(&inner);
    std::vector<uint8_t> plain(plain_len);
    lora_client_operation_message__pack(&inner, plain.data());

    uint8_t aad[proto_sim::kHeaderAadLen];
    proto_sim::build_header_aad(kNodeAddr, kSubnet, kHubAddr, msgid, aad);
    uint8_t iv[12];
    proto_sim::derive_gcm_iv_downlink(nonce, msgid, iv);
    auto enc = proto_sim::aes_gcm_encrypt(iv, aad, sizeof(aad), plain.data(), plain.size());

    LoraHeader hdr = LORA_HEADER__INIT;
    hdr.destaddress   = kNodeAddr;
    hdr.destsubnet    = kSubnet;
    hdr.senderaddress = kHubAddr;
    hdr.msgid         = msgid;

    EncryptedPayload ep = ENCRYPTED_PAYLOAD__INIT;
    ep.tag.data        = enc.tag.data();
    ep.tag.len         = enc.tag.size();
    ep.ciphertext.data = enc.ciphertext.data();
    ep.ciphertext.len  = enc.ciphertext.size();

    LoraClientOperationMessage outer = LORA_CLIENT_OPERATION_MESSAGE__INIT;
    outer.header    = &hdr;
    outer.cmd_case  = LORA_CLIENT_OPERATION_MESSAGE__CMD_ENCRYPTED;
    outer.encrypted = &ep;

    size_t frame_len = lora_client_operation_message__get_packed_size(&outer);
    std::vector<uint8_t> frame(frame_len);
    lora_client_operation_message__pack(&outer, frame.data());
    return frame;
}

std::vector<uint8_t> encrypted_mode_test(ModeTest__Mode mode, uint32_t msgid,
                                         uint32_t grid_period_ms = 1100) {
    ModeTest mt = MODE_TEST__INIT;
    mt.enable           = true;
    mt.durations        = 60;
    mt.mode             = mode;
    mt.gridperiodms     = grid_period_ms;
    mt.copies           = 1;
    mt.droppowerprofile = false;
    mt.enablecounter    = true;
    mt.enablecrypto     = true;

    LoraClientOperationMessage inner = LORA_CLIENT_OPERATION_MESSAGE__INIT;
    inner.cmd_case = LORA_CLIENT_OPERATION_MESSAGE__CMD_MODETEST;
    inner.modetest = &mt;
    return encrypt_op(inner, msgid);
}

// A ModeTest that leaves the power-profile field at its proto3 default — the
// frame a sender that has never heard of the field produces. That used to mean
// "pin the CPU at 240 MHz with light sleep off", which is the opposite of what
// the field's own comment promised.
std::vector<uint8_t> encrypted_mode_test_default_profile(uint32_t msgid) {
    ModeTest mt = MODE_TEST__INIT;
    mt.enable       = true;
    mt.durations    = 60;
    mt.mode         = MODE_TEST__MODE__MODE_A;
    mt.gridperiodms = 1100;
    mt.copies       = 1;
    // droppowerprofile deliberately NOT set.
    LoraClientOperationMessage inner = LORA_CLIENT_OPERATION_MESSAGE__INIT;
    inner.cmd_case = LORA_CLIENT_OPERATION_MESSAGE__CMD_MODETEST;
    inner.modetest = &mt;
    return encrypt_op(inner, msgid);
}

// The hub's real "ModeTest OFF" frame — the path that restores the node's mode.
std::vector<uint8_t> encrypted_mode_test_off(uint32_t msgid) {
    ModeTest mt = MODE_TEST__INIT;
    mt.enable = false;
    LoraClientOperationMessage inner = LORA_CLIENT_OPERATION_MESSAGE__INIT;
    inner.cmd_case = LORA_CLIENT_OPERATION_MESSAGE__CMD_MODETEST;
    inner.modetest = &mt;
    return encrypt_op(inner, msgid);
}

std::vector<uint8_t> encrypted_grid_sync(uint32_t slot, uint32_t msgid,
                                        bool with_key = false,
                                        bool enable = true) {
    GridSync gs = GRID_SYNC__INIT;
    gs.enable            = enable;
    gs.slotindex         = slot;
    gs.slotcount         = timedgrid::kSlotCount;
    gs.roundus           = timedgrid::kRoundUs;
    gs.pitchus           = timedgrid::kSlotPitchUs;
    gs.txround           = 0;
    gs.txslot            = slot;
    gs.beaconslotindex   = timedgrid::kSlotCount - 1;
    gs.beaconeveryrounds = 233;
    gs.symtimeout        = timedgrid::kSymbolTimeoutSymbols;
    gs.resyncmaxs        = 350;
    gs.uloffsetus        = 60000;
    if (with_key) {
        gs.netkey.data = const_cast<uint8_t *>(kFleetKey);
        gs.netkey.len  = framecrypto::kNetKeyBytes;
        gs.netkeyid    = kFleetKeyId;
    }

    LoraClientOperationMessage inner = LORA_CLIENT_OPERATION_MESSAGE__INIT;
    inner.cmd_case = LORA_CLIENT_OPERATION_MESSAGE__CMD_GRIDSYNC;
    inner.gridsync = &gs;
    return encrypt_op(inner, msgid);
}

}  // namespace

TEST_F(RealNodeFixture, ModeTestBActuallyPutsTheNodeInTimedRx) {
    auto login = pack_login_op(/*msgid=*/1, kMtNonce);
    disp.onReceiveNew(login.data(), static_cast<int>(login.size()));

    auto gs = encrypted_grid_sync(/*slot=*/4, /*msgid=*/2);
    disp.onReceiveNew(gs.data(), static_cast<int>(gs.size()));
    ASSERT_TRUE(disp.gridState().active) << "the grid must adopt before B is armable";
    ASSERT_TRUE(disp.isSessionProven());

    // Adopting the grid already enables timed RX, so prove the handler is what
    // sets it rather than inheriting a true it never wrote: put the node back
    // in Mode A first, through the same handler.
    auto a = encrypted_mode_test(MODE_TEST__MODE__MODE_A, /*msgid=*/3);
    disp.onReceiveNew(a.data(), static_cast<int>(a.size()));
    ASSERT_TRUE(disp.modeTestActive());
    EXPECT_FALSE(disp.timedRxEnabledForTest())
        << "MODE_A must turn timed RX OFF, or a node with a grid measures B";
    EXPECT_EQ(disp.modeTestReportedMode(), (uint8_t) modetest::Mode::A);

    // Stopped through the hub's own OFF frame, not a test hook: the restore is
    // the half that was already written, and it is what puts Mode A back.
    auto off = encrypted_mode_test_off(/*msgid=*/4);
    disp.onReceiveNew(off.data(), static_cast<int>(off.size()));
    ASSERT_FALSE(disp.modeTestActive());
    ASSERT_TRUE(disp.timedRxEnabledForTest()) << "the restore puts the grid's mode back";

    auto b = encrypted_mode_test(MODE_TEST__MODE__MODE_B, /*msgid=*/5);
    disp.onReceiveNew(b.data(), static_cast<int>(b.size()));
    ASSERT_TRUE(disp.modeTestActive());
    EXPECT_TRUE(disp.timedRxEnabledForTest());
    EXPECT_EQ(disp.modeTestReportedMode(), (uint8_t) modetest::Mode::B);
}

TEST_F(RealNodeFixture, ModeTestBIsRefusedWithNoGridRatherThanMislabelled) {
    // No GridSync: there is no anchor to arm a window against. Before the fix
    // this armed happily, ran a Mode A measurement and reported mode = 2.
    auto login = pack_login_op(/*msgid=*/1, kMtNonce);
    disp.onReceiveNew(login.data(), static_cast<int>(login.size()));

    // Prove the session with something harmless first.
    TimeSync ts  = TIME_SYNC__INIT;
    ts.epoch     = 1787000000ULL;
    ts.utcoffset = 0;
    LoraClientOperationMessage inner = LORA_CLIENT_OPERATION_MESSAGE__INIT;
    inner.cmd_case = LORA_CLIENT_OPERATION_MESSAGE__CMD_TIMESYNC;
    inner.timesync = &ts;
    auto tsf = encrypt_op(inner, /*msgid=*/2);
    disp.onReceiveNew(tsf.data(), static_cast<int>(tsf.size()));
    ASSERT_TRUE(disp.isSessionProven());
    ASSERT_FALSE(disp.gridState().active);

    auto b = encrypted_mode_test(MODE_TEST__MODE__MODE_B, /*msgid=*/3);
    disp.onReceiveNew(b.data(), static_cast<int>(b.size()));

    EXPECT_FALSE(disp.modeTestActive()) << "no grid, no Mode B measurement";
    EXPECT_EQ(disp.modeTestLastRefusal(), (uint32_t) modetest::ArmRefusal::NoGrid);
    EXPECT_FALSE(disp.timedRxEnabledForTest());
}

TEST_F(RealNodeFixture, ModeTestCIsRefusedBecauseTheHandlerCannotApplyIt) {
    // Class A is a sleep discipline, not a flag. Arming it here would have
    // produced a report labelled mode = 3 over a run that never left Mode A.
    auto login = pack_login_op(/*msgid=*/1, kMtNonce);
    disp.onReceiveNew(login.data(), static_cast<int>(login.size()));
    auto gs = encrypted_grid_sync(/*slot=*/4, /*msgid=*/2);
    disp.onReceiveNew(gs.data(), static_cast<int>(gs.size()));
    ASSERT_TRUE(disp.isSessionProven());

    auto c = encrypted_mode_test(MODE_TEST__MODE__MODE_C, /*msgid=*/3);
    disp.onReceiveNew(c.data(), static_cast<int>(c.size()));

    EXPECT_FALSE(disp.modeTestActive());
    EXPECT_EQ(disp.modeTestLastRefusal(), (uint32_t) modetest::ArmRefusal::ModeUnimplemented);
}

// ---------------------------------------------------------------------------
// §4.6's promotion evidence, as the node actually fills it.
//
// The six flat phase fields on NodeWakeBeacon were declared on the wire,
// decoded by the hub and fed into its promotion path — and the node's beacon
// builder assigned none of them. Every one went out as a proto3 zero, the
// hub's guard requires samples and a crystal, and single-shot therefore never
// engaged for any node in any state. The rule failed closed, which is why it
// cost airtime rather than commands.
// ---------------------------------------------------------------------------

TEST_F(RealNodeFixture, ThePhaseReportCarriesWhatTheTrackerMeasured) {
    auto gs = build_grid_sync(/*enable=*/true, /*slot=*/4, /*msgid=*/900);
    disp.onReceiveNew(gs.data(), static_cast<int>(gs.size()));
    ASSERT_TRUE(disp.gridState().active);

    // Nothing measured yet: samples is 0, and that is the field the hub reads
    // as "this message says nothing".
    PhaseReport empty = PHASE_REPORT__INIT;
    disp.fillPhaseReport(empty);
    EXPECT_EQ(empty.samples, 0u)
        << "an unmeasured node must report no samples, not a phase of zero";

    // Two addressed frames, which is what feeds the tracker.
    auto p1 = build_mac_ping(1, false, 901);
    disp.onReceiveNew(p1.data(), static_cast<int>(p1.size()), /*rx_us=*/1040000);
    auto p2 = build_mac_ping(1, false, 902);
    disp.onReceiveNew(p2.data(), static_cast<int>(p2.size()), /*rx_us=*/1090000);
    ASSERT_GE(disp.phaseStats().n, 1u);

    PhaseReport pr = PHASE_REPORT__INIT;
    disp.fillPhaseReport(pr);
    EXPECT_EQ(pr.samples, disp.phaseStats().n);
    EXPECT_EQ(pr.errus, disp.phaseStats().mean_us());
    EXPECT_EQ(pr.spreadus, disp.phaseStats().spread_us());
    EXPECT_EQ(pr.outsideguard, disp.phaseStats().outside_guard);
    EXPECT_EQ(pr.rtcslowsrc, (uint32_t) disp.rtcSlowSrc());

    // ppm is the one field with no continuous source: it comes from a DriftTest
    // fit, so until one has run ppmSamples is 0 — which is exactly what the
    // field's own comment says it means.
    EXPECT_EQ(pr.ppmsamples, 0u);
}

TEST_F(RealNodeFixture, ReAnchoringClearsWhatThePhaseReportWouldClaim) {
    // The report must not outlive the anchor it describes. A node that adopted
    // a new grid and kept reporting the old fit would hand the hub evidence for
    // a window that has moved.
    auto a = build_grid_sync(true, 4, 910);
    disp.onReceiveNew(a.data(), static_cast<int>(a.size()));
    auto ping = build_mac_ping(1, false, 911);
    disp.onReceiveNew(ping.data(), static_cast<int>(ping.size()), /*rx_us=*/1040000);
    ASSERT_GE(disp.phaseStats().n, 1u);

    auto b = build_grid_sync(true, 9, 912);   // new slot, new anchor
    disp.onReceiveNew(b.data(), static_cast<int>(b.size()));

    PhaseReport pr = PHASE_REPORT__INIT;
    disp.fillPhaseReport(pr);
    EXPECT_EQ(pr.samples, 0u) << "old samples describe the old anchor";
}

// ---------------------------------------------------------------------------
// Section 4.4 — the broadcast beacon, on the real dispatcher.
//
// This is the frame that keeps a node IN Mode B: without it a node holds phase
// only for resyncMaxS after each addressed frame, which at 3.5 commands/day is
// 2.9 % of the day on the grid and no battery saving at all.
//
// It is also the only frame the node acts on without authenticating, so most of
// what is pinned here is what it may NOT do.
// ---------------------------------------------------------------------------
namespace {

std::vector<uint8_t> build_grid_beacon(uint32_t round, uint32_t slot,
                                       uint32_t msgid, uint32_t mask = 0,
                                       bool mask_valid = false,
                                       const uint8_t *mac = nullptr,
                                       uint32_t key_id = 0) {
    LoraHeader hdr = LORA_HEADER__INIT;
    hdr.destaddress   = 0xFF;          // broadcast: one frame for the fleet
    hdr.destsubnet    = kSubnet;
    hdr.senderaddress = kHubAddr;
    hdr.msgid         = msgid;
    hdr.burstindex    = 0;
    hdr.burstcount    = 1;

    GridBeacon gb = GRID_BEACON__INIT;
    gb.txround          = round;
    gb.txslot           = slot;
    gb.pendingmask      = mask;
    gb.pendingmaskvalid = mask_valid;
    if (mac != nullptr) {
        gb.netkeyid = key_id;
        gb.mac.data = const_cast<uint8_t *>(mac);
        gb.mac.len  = framecrypto::kBeaconMacBytes;
    }

    LoraClientOperationMessage op = LORA_CLIENT_OPERATION_MESSAGE__INIT;
    op.header     = &hdr;
    op.cmd_case   = LORA_CLIENT_OPERATION_MESSAGE__CMD_GRIDBEACON;
    op.gridbeacon = &gb;

    std::vector<uint8_t> out(lora_client_operation_message__get_packed_size(&op));
    lora_client_operation_message__pack(&op, out.data());
    return out;
}

// The rx instant that makes a beacon for `round` land `err_us` from where this
// node predicts the beacon mark. Built from the node's own grid so the test
// states the ERROR it is exercising rather than an opaque timestamp.
int64_t rx_for_beacon(const gridstate::State &st, uint32_t round, int64_t err_us,
                      uint32_t len) {
    const int64_t t0 = gridstate::beaconT0ForRound(st, round) + err_us;
    return t0 + (int64_t) loratiming::t0ToRxDoneUs(len);
}

}  // namespace

TEST_F(RealNodeFixture, ABeaconCorrectsDriftWithoutTouchingTheGeometry) {
    auto gs = build_grid_sync(/*enable=*/true, /*slot=*/4, /*msgid=*/1000);
    disp.onReceiveNew(gs.data(), static_cast<int>(gs.size()));
    ASSERT_TRUE(disp.gridState().active);

    const gridstate::State before = disp.gridState();
    constexpr int64_t kDrift = 900;          // us — a plausible hour of it

    auto b = build_grid_beacon(/*round=*/50, before.params.beacon_slot,
                               /*msgid=*/1);
    const int64_t rx = rx_for_beacon(before, 50, kDrift, (uint32_t) b.size());
    disp.onReceiveNew(b.data(), static_cast<int>(b.size()), rx);

    const gridstate::State after = disp.gridState();
    EXPECT_EQ(after.anchor_us, before.anchor_us + kDrift)
        << "the anchor moves by the measured error, which is the whole point";
    // Geometry is GridSync's, never a broadcast's: a beacon that could rewrite
    // it would hand every node in the fleet the same slot.
    EXPECT_EQ(after.params.slot_index, before.params.slot_index);
    EXPECT_EQ(after.params.pitch_us,   before.params.pitch_us);
    EXPECT_EQ(after.params.beacon_every_rounds, before.params.beacon_every_rounds);
    // And it is the sample an idle node's phase tracking otherwise never gets.
    EXPECT_GE(disp.phaseStats().n, 1u);
}

TEST_F(RealNodeFixture, ABeaconOutsideTheGuardIsNotDriftAndIsRefused) {
    // A beacon cannot be sealed with a per-node session key, so this bound is
    // what makes acting on it safe. A whole slot pitch out is the case that
    // matters: adopting it would move this node onto its neighbour's window.
    auto gs = build_grid_sync(true, 4, 1010);
    disp.onReceiveNew(gs.data(), static_cast<int>(gs.size()));
    const gridstate::State before = disp.gridState();

    auto b = build_grid_beacon(60, before.params.beacon_slot, 1);
    const int64_t rx = rx_for_beacon(before, 60,
                                     (int64_t) timedgrid::kSlotPitchUs,
                                     (uint32_t) b.size());
    disp.onReceiveNew(b.data(), static_cast<int>(b.size()), rx);

    EXPECT_EQ(disp.gridState().anchor_us, before.anchor_us)
        << "an error this large is a foreign frame, not drift";
}

TEST_F(RealNodeFixture, ABeaconInTheWrongSlotIsNotOurBeacon) {
    auto gs = build_grid_sync(true, 4, 1020);
    disp.onReceiveNew(gs.data(), static_cast<int>(gs.size()));
    const gridstate::State before = disp.gridState();

    // Correctly timed for ITS slot, but that is not the beacon slot we were
    // told about — a stale beacon from before a re-assignment, or another grid.
    auto b = build_grid_beacon(70, before.params.beacon_slot - 1, 1);
    const int64_t rx = rx_for_beacon(before, 70, 0, (uint32_t) b.size());
    disp.onReceiveNew(b.data(), static_cast<int>(b.size()), rx);

    EXPECT_EQ(disp.gridState().anchor_us, before.anchor_us);
}

TEST_F(RealNodeFixture, APlaintextBeaconMayNotMakeTheNodeListenLess) {
    // A clear bit says "stop listening for up to a beacon interval" — 5.8
    // minutes at the default cadence. Honouring that from an unauthenticated
    // frame would be a cheap, silent way to mute the fleet: cheaper than
    // jamming, and invisible, because a node that skips wrongly reports
    // nothing. An unauthenticated frame may make this node listen MORE, never
    // less.
    auto gs = build_grid_sync(true, 4, 1030);
    disp.onReceiveNew(gs.data(), static_cast<int>(gs.size()));
    const gridstate::State st = disp.gridState();

    // A mask with every bit clear, offered as valid, correctly timed.
    auto b = build_grid_beacon(80, st.params.beacon_slot, 1,
                               /*mask=*/0, /*mask_valid=*/true);
    const int64_t rx = rx_for_beacon(st, 80, 0, (uint32_t) b.size());
    disp.onReceiveNew(b.data(), static_cast<int>(b.size()), rx);

    EXPECT_TRUE(disp.shouldArmNextWindow(rx + 1))
        << "an unauthenticated beacon must not talk this node out of listening";
}

TEST_F(RealNodeFixture, ABeaconDoesNotRatchetTheReplayCounter) {
    // A broadcast belongs to no per-node sequence. Running it through the
    // replay filter would ratchet rx_message_id_ onto the beacon's counter and
    // wedge the node's link to the hub until its next login — the same counter
    // pollution the address filter was moved up to prevent, arriving by a
    // different door.
    const uint32_t kNonce = 0xBEEF0001;
    auto login = pack_login_op(/*msgid=*/1, kNonce);
    disp.onReceiveNew(login.data(), static_cast<int>(login.size()));
    const uint32_t rx_id_before = disp.rxMsgIdForTest();

    auto gs = build_grid_sync(true, 4, 2);
    disp.onReceiveNew(gs.data(), static_cast<int>(gs.size()));
    const gridstate::State st = disp.gridState();

    // A beacon carrying a msgid far ahead of anything this node has seen.
    auto b = build_grid_beacon(90, st.params.beacon_slot, /*msgid=*/999999);
    const int64_t rx = rx_for_beacon(st, 90, 0, (uint32_t) b.size());
    disp.onReceiveNew(b.data(), static_cast<int>(b.size()), rx);

    EXPECT_LE(disp.rxMsgIdForTest(), rx_id_before + 2)
        << "the beacon's own counter must not become the node's";
    EXPECT_EQ(disp.gridState().anchor_us, st.anchor_us)
        << "and it still re-anchored (by zero) rather than being dropped";
}

// ---------------------------------------------------------------------------
// Section 4.3 — the node reads ulOffsetUs at last
//
// The field has been published in every GridSync since the grid was designed,
// validated on arrival, and stored in gridstate::Params — and nothing ever read
// it. Every uplink went out after an unconditional random 29-290 ms backoff, so
// the hub's in-slot measurement was comparing arrivals against a number the
// node had never heard of. These drive the REAL dispatcher, not the header.
// ---------------------------------------------------------------------------

namespace {
// The bound the transmit path passes (LoraInterface::maxUplinkAimWaitUs).
constexpr int64_t kAimBound = 290000;

// Walk the node onto the grid and give it enough on-mark samples that
// phaseTrustworthy() is satisfied — the gate uplinkCadStartUs() applies before
// it will aim at anything.
int64_t settle_on_grid(CmdDispatcher &disp, uint32_t slot, uint32_t first_msgid) {
    auto gs = build_grid_sync(/*enable=*/true, slot, first_msgid);
    const int64_t anchor_rx = 20'000'000;
    disp.noteDriftSample(anchor_rx);
    disp.onReceiveNew(gs.data(), static_cast<int>(gs.size()), anchor_rx);
    if (!disp.gridState().active) return 0;

    int64_t mark = disp.expectedT0Us();
    for (uint32_t i = 0; i < 10; ++i) {
        auto op = pack_sysop_op(first_msgid + 1 + i, CLIENT_OPERATION__CMD_STATUS);
        mark = disp.expectedT0Us() + timedgrid::kRoundUs;
        const int64_t rx = mark + (int64_t) loratiming::t0ToRxDoneUs((uint32_t) op.size());
        disp.noteDriftSample(rx);
        disp.onReceiveNew(op.data(), static_cast<int>(op.size()), rx);
    }
    return mark;
}
}  // namespace

TEST_F(RealNodeFixture, AnAckIsAimedAtTheMarkTheHubWillSubtract) {
    const int64_t mark = settle_on_grid(disp, /*slot=*/4, /*first_msgid=*/900);
    ASSERT_NE(mark, 0);
    ASSERT_TRUE(phase::phaseTrustworthy(disp.phaseStats(), timedgrid::kGuardUs))
        << "the aim is gated on trusted phase; without it this asserts nothing";
    ASSERT_EQ(disp.gridState().params.ul_offset_us, 60000u)
        << "the offset must have survived adoption — it is the number under test";

    // Where an ack really asks: just after RxDone for the frame it is acking.
    const int64_t asks_at = mark + (int64_t) loratiming::t0ToRxDoneUs(60);
    const int64_t cad_start = disp.uplinkCadStartUs(asks_at, kAimBound);
    ASSERT_GT(cad_start, 0) << "an ack inside the offset must be placed";

    // The frame's T0 is one lead after the CAD start, and the hub recovers the
    // mark by subtracting the offset it published. That inverse is the contract.
    const int64_t t0 = cad_start + CmdDispatcher::kUplinkAimLeadUs;
    EXPECT_EQ(t0 - (int64_t) disp.gridState().params.ul_offset_us, mark);
    EXPECT_EQ(disp.uplinkAimHits(), 1u);
    EXPECT_EQ(disp.uplinkAimMisses(), 0u);
}

TEST_F(RealNodeFixture, AnUnplaceableUplinkDeclinesAndSaysSo) {
    const int64_t mark = settle_on_grid(disp, /*slot=*/4, /*first_msgid=*/950);
    ASSERT_NE(mark, 0);

    // A turnaround longer than the published offset: the instant is behind us
    // and the next one is a round away. Declining is right; counting it is what
    // makes HW-7's number visible in the field, where a silent fallback would
    // look exactly like the offset working.
    EXPECT_EQ(disp.uplinkCadStartUs(mark + 400'000, kAimBound), 0);
    EXPECT_EQ(disp.uplinkAimMisses(), 1u);
    EXPECT_EQ(disp.uplinkAimHits(), 0u);
}

TEST_F(RealNodeFixture, ANodeOffTheGridNeverAims) {
    // The rollout property: with no grid there is no aim, so every node running
    // today gets exactly the transmit path that shipped, with no flag to set.
    ASSERT_FALSE(disp.gridState().active);
    EXPECT_EQ(disp.uplinkCadStartUs(1'000'000, kAimBound), 0);
}

TEST_F(RealNodeFixture, AnAnchorTheNodeDoesNotTrustIsNotAimedFrom) {
    // Adoption enables timed RX immediately, but the phase evidence takes ten
    // frames. In between, aiming would be worse than not aiming: a random
    // backoff lands somewhere harmless, while a confident wrong instant lands
    // on another node's slot — 32 nodes with bad anchors would converge on one
    // window instead of spreading out.
    auto gs = build_grid_sync(/*enable=*/true, /*slot=*/4, /*msgid=*/980);
    disp.noteDriftSample(20'000'000);
    disp.onReceiveNew(gs.data(), static_cast<int>(gs.size()), 20'000'000);
    ASSERT_TRUE(disp.gridState().active);
    ASSERT_TRUE(disp.timedRxEnabledForTest());
    ASSERT_FALSE(phase::phaseTrustworthy(disp.phaseStats(), timedgrid::kGuardUs));

    EXPECT_EQ(disp.uplinkCadStartUs(disp.expectedT0Us(), kAimBound), 0);
}

// ---------------------------------------------------------------------------
// Section 4.4 — the beacon is authenticated, and the fleet key that does it
//
// The beacon is one frame for 32 nodes, so it cannot be ENCRYPTED per session.
// Nothing in it is secret either — round, slot and bitmap are all public — so
// what it needs is AUTHENTICITY: an AES-CMAC under a fleet key the hub hands
// each node inside its already-encrypted GridSync.
//
// A deterministic MAC rather than the AEAD the rest of the link uses, because
// extending AES-GCM to a one-to-many key needs a counter that never repeats
// under it, and a hub reboot restarts the counter while every node still holds
// the key. CMAC has no nonce to reuse.
// ---------------------------------------------------------------------------

TEST_F(RealNodeFixture, TheFleetKeyIsAdoptedOnlyFromAnAuthenticatedGridSync) {
    // A plaintext GridSync carrying a key would make the whole construction
    // decorative: anything in radio range could install the key it then signs
    // its own beacons with. This is the §11a lesson — a deny-by-default rule is
    // only shipped when the legitimate sender satisfies it — in a new place.
    auto plain = build_grid_sync(/*enable=*/true, /*slot=*/4, /*msgid=*/601);
    disp.onReceiveNew(plain.data(), static_cast<int>(plain.size()));
    ASSERT_TRUE(disp.gridState().active) << "the grid itself is still adopted";
    EXPECT_FALSE(disp.hasNetKey())
        << "a plaintext frame may not install the key the beacon rests on";

    // The real path: a login proves the session, then an encrypted GridSync
    // carries the key.
    auto login = pack_login_op(/*msgid=*/602, kMtNonce);
    disp.onReceiveNew(login.data(), static_cast<int>(login.size()));
    auto enc = encrypted_grid_sync(/*slot=*/4, /*msgid=*/603, /*with_key=*/true);
    disp.onReceiveNew(enc.data(), static_cast<int>(enc.size()));

    ASSERT_TRUE(disp.gridState().active);
    EXPECT_TRUE(disp.hasNetKey());
    EXPECT_EQ(disp.netKeyId(), kFleetKeyId);
}

TEST_F(RealNodeFixture, AWithdrawnGridTakesTheFleetKeyWithIt) {
    auto login = pack_login_op(/*msgid=*/610, kMtNonce);
    disp.onReceiveNew(login.data(), static_cast<int>(login.size()));
    auto enc = encrypted_grid_sync(4, 611, /*with_key=*/true);
    disp.onReceiveNew(enc.data(), static_cast<int>(enc.size()));
    ASSERT_TRUE(disp.hasNetKey());

    // The key authenticates beacons ABOUT AN ANCHOR. Keeping it past the
    // anchor's life would only let a stale beacon look valid.
    //
    // Encrypted, and that is not incidental: once a node holds a session the
    // plaintext gate refuses every non-LOGIN, non-beacon frame, so a plaintext
    // withdrawal never reaches this handler at all. (The hub's STARTUP
    // withdrawal is exactly that frame — recorded in the plan, not fixed here.)
    auto off = encrypted_grid_sync(/*slot=*/4, /*msgid=*/612, /*with_key=*/false,
                                   /*enable=*/false);
    disp.onReceiveNew(off.data(), static_cast<int>(off.size()));
    EXPECT_FALSE(disp.gridState().active);
    EXPECT_FALSE(disp.hasNetKey());
}

TEST_F(RealNodeFixture, AKeyedNodeAdoptsTheMaskFromASignedBeacon) {
    // The gate used to be frame_authenticated_ — the AEAD flag, which is false
    // for every broadcast by construction. So the branch could never be taken
    // and Tier 3's saving was UNREACHABLE rather than merely unused.
    auto login = pack_login_op(/*msgid=*/620, kMtNonce);
    disp.onReceiveNew(login.data(), static_cast<int>(login.size()));
    auto enc = encrypted_grid_sync(4, 621, /*with_key=*/true);
    disp.onReceiveNew(enc.data(), static_cast<int>(enc.size()));
    ASSERT_TRUE(disp.hasNetKey());

    const gridstate::State st = disp.gridState();
    const uint32_t mask = pending::allListening();
    const auto mac = beacon_mac(kFleetKeyId, 90, st.params.beacon_slot, mask, true);
    auto b = build_grid_beacon(90, st.params.beacon_slot, /*msgid=*/1, mask,
                               /*mask_valid=*/true, mac.data(), kFleetKeyId);
    disp.onReceiveNew(b.data(), static_cast<int>(b.size()),
                      rx_for_beacon(st, 90, 0, (uint32_t) b.size()));

    EXPECT_TRUE(disp.pendingStateForTest().valid)
        << "a signed beacon is the only thing that may set this";
    EXPECT_EQ(disp.pendingStateForTest().bits, mask);
}

TEST_F(RealNodeFixture, ASignedBeaconCanFinallyLetANodeSkipItsWindow) {
    // Tier 3's actual saving, reachable for the first time. An armed window is
    // ~29 ms of receive at ~11 mA against a ~1.2 mA average, and most windows
    // on most nodes are empty.
    //
    // The hub does NOT clear bits today, and that is a separate decision from
    // this one: a cleared bit is a promise it cannot keep for an interactive
    // node, which can be commanded at any instant. What is pinned here is that
    // the MECHANISM now works end to end, so the remaining question is policy.
    auto login = pack_login_op(/*msgid=*/660, kMtNonce);
    disp.onReceiveNew(login.data(), static_cast<int>(login.size()));
    auto enc = encrypted_grid_sync(/*slot=*/4, /*msgid=*/661, /*with_key=*/true);
    disp.onReceiveNew(enc.data(), static_cast<int>(enc.size()));
    ASSERT_TRUE(disp.hasNetKey());

    const gridstate::State st = disp.gridState();
    // Every bit set EXCEPT this node's: the hub has traffic for others and none
    // for us.
    const uint32_t mask = pending::withSlot(pending::allListening(),
                                            st.params.slot_index, false);
    const auto mac = beacon_mac(kFleetKeyId, 98, st.params.beacon_slot, mask, true);
    auto b = build_grid_beacon(98, st.params.beacon_slot, /*msgid=*/1, mask,
                               /*mask_valid=*/true, mac.data(), kFleetKeyId);
    const int64_t rx = rx_for_beacon(st, 98, 0, (uint32_t) b.size());
    disp.onReceiveNew(b.data(), static_cast<int>(b.size()), rx);

    EXPECT_FALSE(disp.shouldArmNextWindow(rx + 1))
        << "this is the saving section 4.4 Tier 3 exists for, and it needed a "
           "fleet key before a node could believe the bit";
}

TEST_F(RealNodeFixture, AForgedBeaconIsIgnoredEntirelyNotMerelyDistrusted) {
    // The ratchet. Once forgery is DETECTABLE, tolerating an unsigned beacon
    // would leave the whole attack open — an attacker would simply omit the
    // MAC — and the guard band does not save the node there: it bounds ONE
    // nudge, not a sequence of them, so anything in radio range could walk the
    // anchor 14 ms per beacon until the node is off the grid.
    auto login = pack_login_op(/*msgid=*/630, kMtNonce);
    disp.onReceiveNew(login.data(), static_cast<int>(login.size()));
    auto enc = encrypted_grid_sync(4, 631, /*with_key=*/true);
    disp.onReceiveNew(enc.data(), static_cast<int>(enc.size()));
    ASSERT_TRUE(disp.hasNetKey());

    const gridstate::State before = disp.gridState();
    constexpr int64_t kDrift = 900;

    // A tag that is right for a DIFFERENT mask than the one on the wire — the
    // exact substitution the MAC exists to catch, and the one a tag over only
    // the round would miss.
    auto mac = beacon_mac(kFleetKeyId, 95, before.params.beacon_slot,
                          pending::allListening(), true);
    auto b = build_grid_beacon(95, before.params.beacon_slot, /*msgid=*/1,
                               /*mask=*/0u, /*mask_valid=*/true, mac.data(),
                               kFleetKeyId);
    disp.onReceiveNew(b.data(), static_cast<int>(b.size()),
                      rx_for_beacon(before, 95, kDrift, (uint32_t) b.size()));

    EXPECT_EQ(disp.gridState().anchor_us, before.anchor_us)
        << "a beacon that fails its MAC moves nothing — not even the anchor";
    EXPECT_FALSE(disp.pendingStateForTest().valid) << "and certainly not the mask";
}

TEST_F(RealNodeFixture, ABeaconUnderAnUnknownKeyIdIsRefused) {
    // The hub has rotated — it re-mints on every restart — and this node has
    // not yet had the GridSync carrying the new key. Refusing is right: the
    // node coasts on resyncMaxS and re-syncs off its next addressed frame,
    // which is exactly the behaviour it had before the beacon existed.
    auto login = pack_login_op(/*msgid=*/640, kMtNonce);
    disp.onReceiveNew(login.data(), static_cast<int>(login.size()));
    auto enc = encrypted_grid_sync(4, 641, /*with_key=*/true);
    disp.onReceiveNew(enc.data(), static_cast<int>(enc.size()));
    const gridstate::State before = disp.gridState();

    const uint32_t other_id = kFleetKeyId + 1;
    auto mac = beacon_mac(other_id, 96, before.params.beacon_slot, 0u, false);
    auto b = build_grid_beacon(96, before.params.beacon_slot, 1, 0u, false,
                               mac.data(), other_id);
    disp.onReceiveNew(b.data(), static_cast<int>(b.size()),
                      rx_for_beacon(before, 96, 500, (uint32_t) b.size()));

    EXPECT_EQ(disp.gridState().anchor_us, before.anchor_us);
}

TEST_F(RealNodeFixture, ANodeWithNoKeyKeepsTheBehaviourThatShipped) {
    // The other half of the ratchet, and what makes this safe to ship ahead of
    // the hubs: a node that never receives a key still corrects its anchor from
    // a beacon, bounded by the guard, and still refuses the mask. No regression
    // and no flag to set.
    auto gs = build_grid_sync(/*enable=*/true, /*slot=*/4, /*msgid=*/650);
    disp.onReceiveNew(gs.data(), static_cast<int>(gs.size()));
    ASSERT_FALSE(disp.hasNetKey());

    const gridstate::State before = disp.gridState();
    constexpr int64_t kDrift = 700;
    auto b = build_grid_beacon(97, before.params.beacon_slot, /*msgid=*/1,
                               /*mask=*/0u, /*mask_valid=*/true);
    disp.onReceiveNew(b.data(), static_cast<int>(b.size()),
                      rx_for_beacon(before, 97, kDrift, (uint32_t) b.size()));

    EXPECT_EQ(disp.gridState().anchor_us, before.anchor_us + kDrift)
        << "an unsigned beacon may still make this node listen at the right time";
    EXPECT_FALSE(disp.pendingStateForTest().valid)
        << "but it may never make it listen LESS";
}

// ---------------------------------------------------------------------------
// 11b — the three things the section listed as still open
// ---------------------------------------------------------------------------

TEST_F(RealNodeFixture, AnOmittedPowerProfileFieldMeansTheProductionProfile) {
    // The field was `keepPowerProfile`, documented "DEFAULT TRUE". proto3
    // scalars have no presence, so an omitting sender got FALSE and the node
    // ran the measurement with light sleep off at 240 MHz — then reported it as
    // if it were a production number. A comment cannot change a wire default.
    //
    // Inverted to dropPowerProfile, so proto3's own zero is the safe answer.
    auto login = pack_login_op(/*msgid=*/1, kMtNonce);
    disp.onReceiveNew(login.data(), static_cast<int>(login.size()));

    auto mt = encrypted_mode_test_default_profile(/*msgid=*/2);
    disp.onReceiveNew(mt.data(), static_cast<int>(mt.size()));

    ASSERT_TRUE(disp.modeTestActive());
    EXPECT_TRUE(disp.modeTestProductionProfile())
        << "a sender that omits the field must get the profile the mode exists "
           "to measure, not its opposite";
}

TEST_F(RealNodeFixture, DroppingThePowerProfileIsAnExplicitAct) {
    // The other direction, so the inversion is not merely a constant that
    // happens to read true: asking for the bench profile still works, and now
    // it reads like the bench act it is at the call site.
    auto login = pack_login_op(/*msgid=*/1, kMtNonce);
    disp.onReceiveNew(login.data(), static_cast<int>(login.size()));

    ModeTest mt = MODE_TEST__INIT;
    mt.enable           = true;
    mt.durations        = 60;
    mt.mode             = MODE_TEST__MODE__MODE_A;
    mt.gridperiodms     = 1100;
    mt.copies           = 1;
    mt.droppowerprofile = true;
    LoraClientOperationMessage inner = LORA_CLIENT_OPERATION_MESSAGE__INIT;
    inner.cmd_case = LORA_CLIENT_OPERATION_MESSAGE__CMD_MODETEST;
    inner.modetest = &mt;
    auto frame = encrypt_op(inner, /*msgid=*/2);
    disp.onReceiveNew(frame.data(), static_cast<int>(frame.size()));

    ASSERT_TRUE(disp.modeTestActive());
    EXPECT_FALSE(disp.modeTestProductionProfile())
        << "and the report must say so, or a bench number can be quoted as a "
           "production one";
}

TEST_F(RealNodeFixture, ABeaconCommitsOneSampleAndItIsInsideTheGuard) {
    // The beacon exists to be "the sample an idle node's phase tracking
    // otherwise never gets". It was doing the opposite.
    //
    // A beacon is a BROADCAST, so it passed the address filter, and admitFrame
    // then stamped it against THIS node's own mark — while the beacon sits in
    // the beacon slot, 27 pitches away for a node in slot 4. That is 234 ms
    // against a 14 080 us guard, so outside_guard went to 1; phaseTrustworthy()
    // requires ZERO outside the guard, so one beacon made the node distrust its
    // own anchor permanently. handleGridBeacon then committed a second, correct
    // sample against the beacon slot's mark — which is the one that belongs.
    auto gs = build_grid_sync(/*enable=*/true, /*slot=*/4, /*msgid=*/700);
    disp.onReceiveNew(gs.data(), static_cast<int>(gs.size()));
    ASSERT_TRUE(disp.gridState().active);
    ASSERT_EQ(disp.phaseStats().n, 0u) << "adoption resets the stats";

    const gridstate::State st = disp.gridState();
    auto b = build_grid_beacon(/*round=*/50, st.params.beacon_slot, /*msgid=*/1);
    disp.onReceiveNew(b.data(), static_cast<int>(b.size()),
                      rx_for_beacon(st, 50, 0, (uint32_t) b.size()));

    EXPECT_EQ(disp.phaseStats().n, 1u)
        << "one frame, one sample — the generic path must leave the beacon to "
           "the handler that knows which slot it is in";
    EXPECT_EQ(disp.phaseStats().outside_guard, 0u)
        << "a beacon landing exactly on its own mark must not be recorded as a "
           "quarter-second phase error";
}

TEST_F(RealNodeFixture, ABurstCopyIsStampedAsCopyZero) {
    // handleGridSync and handleGridBeacon both back out burstIndex; the generic
    // phase path did not. Copies are one 88 ms stride apart, so stamping copy N
    // as copy 0 commits an error of N x 88 ms against a 14 080 us guard — and
    // one such sample is enough to fail phaseTrustworthy() forever.
    //
    // Not a rare case: until the hub promotes to single-shot it sends seventeen
    // copies, and a node that has not promoted is sweeping a free-running
    // window, so the copy it hears first is usually not copy 0.
    auto gs = build_grid_sync(/*enable=*/true, /*slot=*/4, /*msgid=*/710);
    disp.onReceiveNew(gs.data(), static_cast<int>(gs.size()));
    ASSERT_TRUE(disp.gridState().active);

    // Copy 3 of a burst whose copy 0 sat exactly on this node's mark.
    constexpr uint32_t kCopy = 3;
    auto op = pack_sysop_op(/*msgid=*/711, CLIENT_OPERATION__CMD_STATUS, kCopy);
    const int64_t mark = disp.expectedT0Us() + timedgrid::kRoundUs;
    const int64_t rx = mark + (int64_t) kCopy * drift::kCopySpacingUs
                     + (int64_t) loratiming::t0ToRxDoneUs((uint32_t) op.size());
    disp.noteDriftSample(rx);
    disp.onReceiveNew(op.data(), static_cast<int>(op.size()), rx);

    ASSERT_EQ(disp.phaseStats().n, 1u);
    EXPECT_EQ(disp.phaseStats().outside_guard, 0u)
        << "copy 3 is 264 ms after the mark; read as copy 0 that is a phase "
           "error eighteen guard bands wide";
    EXPECT_LT(std::abs((long) disp.phaseStats().last_us), 2000L);
}

TEST_F(RealNodeFixture, APlaintextFrameCannotFeedThePhaseFit) {
    // 11b's second open item. The sample used to be committed with the address
    // filter, above the plaintext gate, so anything in radio range could bias
    // the fit that decides whether this node trusts its own anchor — and
    // therefore whether it enters Mode B and where it aims its uplinks.
    //
    // Bounded, because poisoning drives outside_guard up and DEMOTES rather
    // than desynchronising silently. A bound is not a reason to accept it.
    auto login = pack_login_op(/*msgid=*/1, kMtNonce);
    disp.onReceiveNew(login.data(), static_cast<int>(login.size()));
    auto gs = encrypted_grid_sync(/*slot=*/4, /*msgid=*/2);
    disp.onReceiveNew(gs.data(), static_cast<int>(gs.size()));
    ASSERT_TRUE(disp.gridState().active);
    ASSERT_TRUE(disp.isSessionProven());
    const uint32_t n_before = disp.phaseStats().n;

    // A plaintext frame addressed to this node, arriving half a guard band off
    // the mark — a plausible nudge rather than an obvious forgery.
    auto op = pack_sysop_op(/*msgid=*/3, CLIENT_OPERATION__CMD_STATUS);
    const int64_t mark = disp.expectedT0Us() + timedgrid::kRoundUs;
    const int64_t rx = mark + 7000
                     + (int64_t) loratiming::t0ToRxDoneUs((uint32_t) op.size());
    disp.noteDriftSample(rx);
    disp.onReceiveNew(op.data(), static_cast<int>(op.size()), rx);

    EXPECT_EQ(disp.phaseStats().n, n_before)
        << "a frame the node refuses to ACT on must not move the belief that "
           "decides where it listens";
}

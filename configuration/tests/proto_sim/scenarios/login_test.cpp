// Scenario B2 — login retry MUST reuse the same base nonce.
//
// Regression test for the bug captured in this session's logs:
//   * Hub schedules a LoginMsg via the REGISTER 500 ms timer.
//   * A stale login_retry interval fires ~230 ms later and calls
//     send_login() again.
//   * Before the fix, the second call generated a NEW esp_random() base
//     nonce. The node accepted the first LoginMsg and rate-limited the
//     second, so the hub stored nonce_B and the node stored nonce_A.
//     Every subsequent encrypted reply triggered "IV mismatch for peer N".
//
// The tests here are hub-only and use a synthesized ACK frame instead of a
// live NodeModel — the synchronous SimRadio would otherwise let the node
// reply during the same call, clearing pending_login_nonce_ before the
// retry-path assertions can run.

#include "sim/hub_model.h"

#include <gtest/gtest.h>

using namespace proto_sim;

namespace {

constexpr uint64_t kMacRol2 = 0xE08CFE5F9EC4ULL;

AirFrame make_node_ack(uint8_t sender_addr, uint32_t msg_id) {
    LoraClientResponseMessage m;
    m.header.senderAddress = sender_addr;
    m.header.msgId         = msg_id;
    m.proto                = LoraClientResponseMessage::Proto::Avail;
    m.avail.available      = true;
    return make_resp_frame(m);
}

struct HubOnlyFixture : public ::testing::Test {
    SimClock        clock;
    SimRadio        radio;
    SharedNonceMap  nonces;
    HubTracker      tracker{&clock, &radio, &nonces};
    HubListener     rol_2{"rol_2", 18, 2, kMacRol2, 21600, &tracker, &clock, &nonces};

    int rng_calls = 0;
    void SetUp() override {
        tracker.register_listener(&rol_2);
        rol_2.rng = [this] {
            ++rng_calls;
            return 0xA0000000u + static_cast<uint32_t>(rng_calls);
        };
        // Force registered_=true via NVS preload so send_login() is meaningful
        // without driving the full REGISTER path (we want to isolate the
        // login-retry invariant from REGISTER side effects).
        HubNvsBlob blob;
        blob.version       = 2;
        blob.rx_message_id = 0;
        blob.tx_message_id = 0;
        blob.logged_in     = true;
        rol_2.preload_nvs(blob);
        rol_2.setup(/*time_valid_at_boot=*/false); // suppress startup-login timer
    }
};

TEST_F(HubOnlyFixture, RetryReusesPendingNonce) {
    // Three rapid send_login() calls modelling the production race:
    // first from the REGISTER 500 ms timer, second from a stale login_retry
    // interval, third for good measure.
    rol_2.send_login();
    const uint32_t first = rol_2.pending_login_nonce();
    ASSERT_NE(first, 0u);
    EXPECT_EQ(rng_calls, 1);

    rol_2.send_login();
    EXPECT_EQ(rng_calls, 1)
        << "Second send_login while pending must NOT call rng() again";
    EXPECT_EQ(rol_2.pending_login_nonce(), first);

    rol_2.send_login();
    EXPECT_EQ(rng_calls, 1);
    EXPECT_EQ(rol_2.pending_login_nonce(), first);

    // Every LoginMsg on the wire must carry the SAME nonce — this is the
    // invariant the production bug violated.
    int login_count = 0;
    for (const auto& f : radio.hub_to_node_frames()) {
        auto m = as_op(f);
        if (!m || m->cmd != LoraClientOperationMessage::Cmd::Login) continue;
        EXPECT_EQ(m->login.nonce, first)
            << "All retried LoginMsgs must carry the same base nonce";
        ++login_count;
    }
    EXPECT_EQ(login_count, 3);

    // Hub's nonce map must hold the SAME nonce the node would have stored
    // (the first one it ever sees, since later duplicates are rate-limited).
    EXPECT_EQ(nonces.get(18), first);
}

TEST_F(HubOnlyFixture, AckClearsPendingForNextChallenge) {
    rol_2.send_login();
    const uint32_t first = rol_2.pending_login_nonce();
    ASSERT_NE(first, 0u);

    // Synthesize the node's ACK (Available, msgid=1) the way it would arrive
    // after CMD_LOGIN handling on the node side.
    radio.send(make_node_ack(/*sender_addr=*/18, /*msg_id=*/1));

    EXPECT_TRUE(rol_2.login_acked());
    EXPECT_EQ(rol_2.pending_login_nonce(), 0u);

    // A subsequent login challenge (e.g. after enter_sleep) must mint fresh.
    rol_2.send_login();
    EXPECT_EQ(rng_calls, 2);
    EXPECT_NE(rol_2.pending_login_nonce(), first);
}

TEST_F(HubOnlyFixture, EnterSleepClearsPending) {
    rol_2.send_login();
    const uint32_t first = rol_2.pending_login_nonce();
    ASSERT_NE(first, 0u);

    rol_2.enter_sleep();
    EXPECT_EQ(rol_2.pending_login_nonce(), 0u)
        << "enter_sleep() ends the session; pending nonce must reset so "
           "the next wake-cycle login mints fresh.";
}

// B5: after kMaxLoginRetries (24) consecutive unacknowledged retries the hub
// must stop sending LoginMsgs. The interval is 1 h in production; we drive it
// virtually here.
TEST_F(HubOnlyFixture, MaxLoginRetriesExhaustsAndStops) {
    rol_2.send_login();   // initial send via the do_login_and_arm_retry path
    // The fixture preloaded NVS so registered_=true; arm the retry interval
    // by going through the same path the production REGISTER timer does.
    // We can't call do_login_and_arm_retry_ directly (private), so re-enter
    // through enter_sleep + wake fallback which uses the same machinery.

    // Easier: replicate the retry loop by repeatedly calling send_login on
    // the assumption that pending_login_nonce_ stays set (no ACK arrives).
    // The interval-based count cap is asserted indirectly: after 24
    // retries, no further send_login() should add to the wire.

    // Simpler still: drive the interval directly via clock.tick.
    // First fire the startup-style login at sleep wake to register a
    // real interval.
    rol_2.enter_sleep();
    // sleep_duration_s_=21600 (6 h) + boot margin + stagger ≈ 6 h before the
    // fallback fires; then 24 retries × 1 h. Tick 32 h to clear the cap.
    clock.tick(60u * 60u * 1000u * 32u);

    int login_count = 0;
    for (const auto& f : radio.hub_to_node_frames()) {
        auto m = as_op(f);
        if (m && m->cmd == LoraClientOperationMessage::Cmd::Login) ++login_count;
    }

    // 1 initial pre-sleep send + 1 fallback at wake + up to 24 retries = 26 max.
    EXPECT_LE(login_count, 26)
        << "Hub must give up after kMaxLoginRetries (24) — sending forever "
           "would burn battery on the node and air time on the band.";
    EXPECT_EQ(rol_2.login_retry_count(), HubListener::kMaxLoginRetries)
        << "Retry counter must be pinned at the cap; loop must have exited.";
}

TEST_F(HubOnlyFixture, AckRequiresMatchingSender) {
    rol_2.send_login();
    const uint32_t first = rol_2.pending_login_nonce();
    ASSERT_NE(first, 0u);

    // ACK with the wrong sender address (17 instead of 18) must NOT flip
    // login_acked_ on rol_2 — that's the cross-listener isolation the user
    // depends on for multi-node deployments.
    radio.send(make_node_ack(/*sender_addr=*/17, /*msg_id=*/1));
    EXPECT_FALSE(rol_2.login_acked());
    EXPECT_EQ(rol_2.pending_login_nonce(), first);
}

// A4: a fresh REGISTER while a previous login challenge is still pending
// must wipe pending_login_nonce_ so the post-REGISTER login mints a new one.
// (Production: REGISTER means the node just rebooted and lost any base nonce
// it had — keeping the hub's pending would create a guaranteed mismatch.)
TEST(LoginNoFixture, RegisterClearsPendingNonce) {
    SimClock        clock;
    SimRadio        radio;
    SharedNonceMap  nonces;
    HubTracker      tracker{&clock, &radio, &nonces};
    HubListener rol_2{"rol_2", 18, 2, kMacRol2, 21600, &tracker, &clock, &nonces};
    tracker.register_listener(&rol_2);

    int rng_calls = 0;
    rol_2.rng = [&] { ++rng_calls; return 0xC0000000u + rng_calls; };

    rol_2.wipe_nvs();
    rol_2.setup(/*time_valid_at_boot=*/false);

    // First login challenge is pending.
    rol_2.send_login();
    const uint32_t pending_before = rol_2.pending_login_nonce();
    ASSERT_NE(pending_before, 0u);

    // Synthesize a REGISTER from a (rebooted) node.
    LoraClientResponseMessage reg_msg;
    reg_msg.proto        = LoraClientResponseMessage::Proto::Register;
    reg_msg.reg.mac_addr = kMacRol2;
    radio.send(make_resp_frame(reg_msg));

    EXPECT_EQ(rol_2.pending_login_nonce(), 0u)
        << "REGISTER must clear pending_login_nonce_ so the next login "
           "challenge mints a fresh nonce — required when the node has "
           "rebooted and lost its peer_base store.";
}

} // namespace

// Scenarios B4 and B7 — recovery paths that need deferred-delivery
// timing on the SimRadio to model the production async TX → propagation →
// RX latency.
//
// B4: a node's first Available ACK is "lost" (dropped from the air). The
//     hub's retry interval fires, a second LoginMsg goes out, and a
//     successful ACK comes back. login_acked_ flips. With the pending-
//     nonce fix in place, both LoginMsgs carry the same nonce so the
//     node accepts the first and rate-limits the second.
//
// B7: hub-side base-nonce map is wiped (simulating reboot). A stale
//     encrypted reply from the node lands at the hub. Hub auto-sends
//     CMD_BASENONCE to re-provision; node receives it and stores the
//     new base. Subsequent encrypted replies decrypt cleanly.

#include "sim/hub_model.h"
#include "sim/node_model.h"

#include <gtest/gtest.h>

using namespace proto_sim;

namespace {

constexpr uint64_t kMacRol2 = 0xE08CFE5F9EC4ULL;

// ---------------------------------------------------------------------------
// B4 — lost ACK → retry → eventual ack
// ---------------------------------------------------------------------------
TEST(Recovery, LostFirstAckRetriesAndEventuallyAcked) {
    SimClock        clock;
    SimRadio        radio;
    SharedNonceMap  nonces;
    HubTracker      tracker{&clock, &radio, &nonces};
    HubListener rol_2{"rol_2", 18, 2, kMacRol2, 21600, &tracker, &clock, &nonces};
    tracker.register_listener(&rol_2);
    NodeModel node_2{"node_2", kMacRol2, &clock, &radio};

    int rng_calls = 0;
    rol_2.rng = [&] { ++rng_calls; return 0xE0000000u + rng_calls; };

    rol_2.wipe_nvs();
    rol_2.setup(/*time_valid_at_boot=*/false);

    // Switch to deferred delivery so we can drop the first ACK in flight.
    radio.set_deferred(true);

    // REGISTER from node → hub schedules 500ms login timer.
    node_2.send_register();
    radio.deliver_pending();   // REGISTER → hub (which queues ClientConfig).
    radio.deliver_pending();   // ClientConfig → node.
    clock.tick(500);           // login_startup fires → hub queues LoginMsg
    radio.deliver_pending();   // LoginMsg → node (node queues Available).

    // Now drop the node's Available ACK before delivering it to the hub.
    // The Available is wrapped in an EncryptedPayload (outer proto =
    // Encrypted) once the node has a peer base nonce, so we just drop
    // any NodeToHub frame currently pending.
    size_t dropped = radio.drop_pending([](const AirFrame& f) {
        return f.dir == AirFrame::Dir::NodeToHub;
    });
    ASSERT_EQ(dropped, 1u) << "Sanity: exactly one ACK frame was queued";

    EXPECT_FALSE(rol_2.login_acked())
        << "ACK was dropped; hub must still be waiting.";

    const uint32_t pending_after_first = rol_2.pending_login_nonce();

    // Hourly retry fires. send_login() reuses pending nonce.
    clock.tick(60u * 60u * 1000u + 1);
    // After the retry, LoginMsg #2 is queued. Deliver it to the node.
    radio.deliver_pending();
    // Node rate-limits the duplicate (5s window long since elapsed → accepts),
    // resets counters, stores the SAME nonce again, sends Available.
    radio.deliver_pending();   // Available → hub

    EXPECT_TRUE(rol_2.login_acked())
        << "After the retry round, the second Available ACK must reach "
           "the hub and flip login_acked_ to true.";
    EXPECT_EQ(rol_2.pending_login_nonce(), 0u)
        << "Successful ACK must clear pending_login_nonce_ so the next "
           "challenge mints fresh.";
    // Nonce on both sides must still match (regression of B2 in the
    // lost-ACK scenario).
    EXPECT_EQ(nonces.get(18), pending_after_first);
    EXPECT_EQ(node_2.base_nonce_for(0xFF), pending_after_first);
    EXPECT_EQ(rng_calls, 1)
        << "Lost-ACK retry must not regenerate the nonce (B2 invariant).";
}

// ---------------------------------------------------------------------------
// B7 — BaseNonceExchange recovery path
// ---------------------------------------------------------------------------
TEST(Recovery, BaseNonceExchangeReprovisionsAfterHubWipe) {
    SimClock        clock;
    SimRadio        radio;
    SharedNonceMap  nonces;
    HubTracker      tracker{&clock, &radio, &nonces};
    HubListener rol_2{"rol_2", 18, 2, kMacRol2, 21600, &tracker, &clock, &nonces};
    tracker.register_listener(&rol_2);
    NodeModel node_2{"node_2", kMacRol2, &clock, &radio};

    int rng_calls = 0;
    rol_2.rng = [&] { ++rng_calls; return 0xF0000000u + rng_calls; };

    rol_2.wipe_nvs();
    rol_2.setup(/*time_valid_at_boot=*/false);
    node_2.send_register();
    clock.tick(600);
    ASSERT_TRUE(rol_2.login_acked());
    const uint32_t login_nonce = nonces.get(18);
    ASSERT_NE(login_nonce, 0u);

    // Simulate hub-side BSS clear (file-scope s_base_nonce_map gone).
    nonces.clear();
    EXPECT_FALSE(nonces.contains(18));

    // Hub explicitly re-provisions via the BaseNonceExchange recovery path.
    rol_2.send_base_nonce_exchange();
    ASSERT_TRUE(nonces.contains(18))
        << "send_base_nonce_exchange must store the fresh nonce locally.";
    const uint32_t fresh = nonces.get(18);
    EXPECT_NE(fresh, login_nonce)
        << "BaseNonceExchange must mint a new value, not reuse the old one.";
    EXPECT_EQ(node_2.base_nonce_for(0xFF), fresh)
        << "Node must have stored the fresh nonce — peer_base_[0xFF] now "
           "matches the hub's nonces[18]. End-to-end recovery without a "
           "full LoginMsg challenge.";

    // Subsequent encrypted reply from the node decrypts on the hub side
    // (the integrating test for the recovery being end-to-end functional).
    node_2.send_position(0.7f);
    EXPECT_GT(rol_2.rx_message_id(), 0u)
        << "Encrypted CoverPosition reply must decrypt with the fresh "
           "base nonce — proves the recovery is functional end-to-end.";
}

} // namespace

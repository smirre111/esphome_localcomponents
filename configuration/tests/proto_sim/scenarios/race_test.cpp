// Scenarios B3, C5, D4 — race conditions and multi-node interactions.
//
// B3 models the exact production race that produced today's nonce
// divergence: a stale login_retry interval was about to fire when REGISTER
// arrived. cancel_interval (in production) didn't catch the already-queued
// callback, so it fired ~230 ms after the REGISTER-triggered LoginMsg.
// With the pending-nonce fix the second send_login() reuses the first
// nonce; both LoginMsgs on the wire are identical and the node's
// rate-limiter silently drops the duplicate.
//
// We can't directly reproduce ESPHome's exact scheduler bug in SimClock
// (cancel_interval here is well-behaved), but we model the equivalent:
// a manually-armed stale interval that fires shortly after REGISTER.

#include "sim/hub_model.h"
#include "sim/node_model.h"

#include <gtest/gtest.h>

using namespace proto_sim;

namespace {

constexpr uint64_t kMacRol1 = 0xE08CFE5FB7A4ULL;
constexpr uint64_t kMacRol2 = 0xE08CFE5F9EC4ULL;

// B3: two send_login() invocations in rapid succession (modelling the
// REGISTER-timer + stale-interval double-fire) must result in identical
// nonces on the wire and a single rng draw — the invariant violated by
// the production bug.
//
// Hub-only by design: we model the IN-FLIGHT retry window (before any ACK
// arrives). The synchronous SimRadio would otherwise let the node ACK
// the first LoginMsg and clear pending_login_nonce_ before the phantom
// retry fires; that's a harness artefact, not the production behaviour.
// Phase 2 introduces a deferred-delivery SimRadio knob and re-runs this
// scenario end-to-end with a real node.
TEST(Race, StaleRetryDoubleFireKeepsNoncesAligned) {
    SimClock        clock;
    SimRadio        radio;
    SharedNonceMap  nonces;
    HubTracker      tracker{&clock, &radio, &nonces};
    HubListener rol_2{"rol_2", 18, 2, kMacRol2, 21600, &tracker, &clock, &nonces};
    tracker.register_listener(&rol_2);

    int rng_calls = 0;
    rol_2.rng = [&] { ++rng_calls; return 0xD0000000u + rng_calls; };

    HubNvsBlob blob; blob.version = 2; blob.logged_in = true;
    rol_2.preload_nvs(blob);
    rol_2.setup(/*time_valid_at_boot=*/true);
    // setup() armed login_startup_rol_2 at 5000 + 18*3000 = 59000 ms.

    // Phantom stale-retry fires 230 ms after the startup login — same delta
    // observed in production logs (21:55:11.727 → 21:55:11.957).
    clock.set_timeout("phantom_stale_retry",
                      59000u + 230u,
                      [&] { rol_2.send_login(); });

    clock.tick(60000);

    EXPECT_EQ(rng_calls, 1)
        << "Only the first send_login() may consume an rng draw; the "
           "stale-retry send_login() must reuse pending_login_nonce_.";

    // Both LoginMsgs on the wire must carry the same nonce.
    uint32_t observed_nonce = 0;
    int login_count = 0;
    for (const auto& f : radio.hub_to_node_frames()) {
        auto m = as_op(f);
        if (!m || m->cmd != LoraClientOperationMessage::Cmd::Login) continue;
        if (login_count == 0) observed_nonce = m->login.nonce;
        else EXPECT_EQ(m->login.nonce, observed_nonce)
            << "Both LoginMsgs in the race must carry the same nonce.";
        ++login_count;
    }
    EXPECT_EQ(login_count, 2)
        << "Test should have fired both the startup timer AND the phantom "
           "stale retry — got " << login_count;

    EXPECT_EQ(nonces.get(18), observed_nonce)
        << "Hub's stored base nonce must match what's on the wire — "
           "regenerating on retry would leave the map storing nonce_B "
           "while the node accepted nonce_A. (Today's production bug.)";
}

// C5: per-listener stagger (= short_address * 3 s) must produce distinct
// login_startup fire times for rol_1 and rol_2 — guarding against the
// burst-collision scenario where two LoginMsgs go out within the same
// transmission window.
TEST(Race, StaggerProducesDistinctFireTimes) {
    SimClock        clock;
    SimRadio        radio;
    SharedNonceMap  nonces;
    HubTracker      tracker{&clock, &radio, &nonces};

    HubListener rol_1{"rol_1", 17, 2, kMacRol1, 21600, &tracker, &clock, &nonces};
    HubListener rol_2{"rol_2", 18, 2, kMacRol2, 21600, &tracker, &clock, &nonces};
    tracker.register_listener(&rol_1);
    tracker.register_listener(&rol_2);

    // Preload registered state on both so setup() arms the startup timer.
    HubNvsBlob blob; blob.version = 2; blob.logged_in = true;
    rol_1.preload_nvs(blob);
    rol_2.preload_nvs(blob);
    rol_1.setup(/*time_valid_at_boot=*/true);
    rol_2.setup(/*time_valid_at_boot=*/true);

    // rol_1 fallback = 5000 + 17*3000 = 56000 ms
    // rol_2 fallback = 5000 + 18*3000 = 59000 ms
    // Tick just into the rol_1 window.
    clock.tick(56500);
    int after_rol1 = 0;
    for (const auto& f : radio.hub_to_node_frames())
        if (auto m = as_op(f); m && m->cmd == LoraClientOperationMessage::Cmd::Login) ++after_rol1;
    EXPECT_EQ(after_rol1, 1) << "rol_1 must have fired its login by now";

    // Tick to past the rol_2 window.
    clock.tick(3000);
    int after_rol2 = 0;
    for (const auto& f : radio.hub_to_node_frames())
        if (auto m = as_op(f); m && m->cmd == LoraClientOperationMessage::Cmd::Login) ++after_rol2;
    EXPECT_EQ(after_rol2, 2) << "rol_2 must also have fired now";

    // The two LoginMsgs must have left at distinct virtual times implied by
    // the 3-second stagger gap. The transcript order pins ordering — rol_1
    // first.
    int idx = 0;
    int rol_1_login_idx = -1, rol_2_login_idx = -1;
    for (const auto& f : radio.hub_to_node_frames()) {
        if (auto m = as_op(f); m && m->cmd == LoraClientOperationMessage::Cmd::Login) {
            if (m->header.destAddress == 17 && rol_1_login_idx < 0) rol_1_login_idx = idx;
            if (m->header.destAddress == 18 && rol_2_login_idx < 0) rol_2_login_idx = idx;
        }
        ++idx;
    }
    EXPECT_GE(rol_1_login_idx, 0);
    EXPECT_GE(rol_2_login_idx, 0);
    EXPECT_LT(rol_1_login_idx, rol_2_login_idx)
        << "Stagger must order rol_1 before rol_2 by short_address.";
}

// D4: two nodes mis-provisioned with the same address (e.g. operator pasted
// the same short_address into both YAML entries). Document the failure
// mode: BOTH nodes act on a CMD_OPEN destined for that shared address.
// This test exists to make the failure visible — there's no in-protocol
// way to detect it; the cure is at the config layer.
TEST(Race, BothNodesActOnSharedAddressIsDocumentedBehaviour) {
    SimClock        clock;
    SimRadio        radio;
    SharedNonceMap  nonces;
    HubTracker      tracker{&clock, &radio, &nonces};
    HubListener rol_1{"rol_1", 17, 2, kMacRol1, 21600, &tracker, &clock, &nonces};
    tracker.register_listener(&rol_1);

    NodeModel node_a{"node_a", kMacRol1, &clock, &radio};
    NodeModel node_b{"node_b", kMacRol2, &clock, &radio};
    // Force the misconfiguration: both nodes carry address 17.
    node_a.set_cfg_address(17, 2); node_a.set_registered(true);
    node_b.set_cfg_address(17, 2); node_b.set_registered(true);

    rol_1.send_cover_op(CovOperation::CMD_OPEN);

    EXPECT_EQ(node_a.last_motor_cmd(), "OPEN");
    EXPECT_EQ(node_b.last_motor_cmd(), "OPEN")
        << "Both nodes act when they share an address — there's no "
           "address-collision detection in the wire protocol; this is a "
           "config-layer correctness obligation, not a runtime invariant.";
}

} // namespace

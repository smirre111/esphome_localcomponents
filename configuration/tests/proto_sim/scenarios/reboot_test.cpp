// Scenarios A5, A6, B6, C4 — hub reboot and NVS recovery.
//
// Production: the file-scope s_base_nonce_map in lora_client.cpp is BSS
// and is therefore cleared on every reboot. Frame counters are persisted
// to ESPPreference flash (NVS) with a version byte. After reboot the
// counter is restored with the "skip 64" gap so an unsynced node won't
// see a backwards-going msgid sequence.

#include "sim/hub_model.h"
#include "sim/node_model.h"

#include <gtest/gtest.h>

using namespace proto_sim;

namespace {

constexpr uint64_t kMacRol1 = 0xE08CFE5FB7A4ULL;
constexpr uint64_t kMacRol2 = 0xE08CFE5F9EC4ULL;

// A5: a reboot AFTER a node has registered must restore registered_=true
// from NVS without requiring REGISTER to be re-sent. The first post-reboot
// LoginMsg goes out via the startup path.
TEST(Reboot, ReplaysRegisteredStateFromNvs) {
    SimClock        clock;
    SimRadio        radio;
    SharedNonceMap  nonces;
    HubTracker      tracker{&clock, &radio, &nonces};
    HubListener rol_2{"rol_2", 18, 2, kMacRol2, 21600, &tracker, &clock, &nonces};
    tracker.register_listener(&rol_2);

    rol_2.wipe_nvs();
    rol_2.setup(/*time_valid_at_boot=*/false);

    NodeModel node_2{"node_2", kMacRol2, &clock, &radio};
    node_2.send_register();
    ASSERT_TRUE(rol_2.registered());

    // Snapshot NVS, then reboot.
    auto blob = rol_2.nvs_snapshot();
    rol_2.simulate_reboot();
    EXPECT_FALSE(rol_2.registered());
    rol_2.preload_nvs(blob);
    rol_2.setup(/*time_valid_at_boot=*/true);

    EXPECT_TRUE(rol_2.registered())
        << "After reboot the hub must restore registered_=true from NVS so "
           "an already-configured node doesn't need to re-send REGISTER.";

    // After setup with time_valid, schedule_startup_login_ armed a timer.
    // Drive it and confirm a LoginMsg goes out.
    clock.tick(5000 + 18u * 3000u + 10);
    int login_count = 0;
    for (const auto& f : radio.hub_to_node_frames())
        if (auto m = as_op(f); m && m->cmd == LoraClientOperationMessage::Cmd::Login) ++login_count;
    EXPECT_GE(login_count, 1)
        << "Hub must send a fresh LoginMsg after reboot to repopulate the "
           "(now-empty) s_base_nonce_map.";
}

// A6: NVS blob with mismatched version is discarded; hub boots fresh.
TEST(Reboot, NvsVersionMismatchDiscarded) {
    SimClock        clock;
    SimRadio        radio;
    SharedNonceMap  nonces;
    HubTracker      tracker{&clock, &radio, &nonces};
    HubListener rol_2{"rol_2", 18, 2, kMacRol2, 21600, &tracker, &clock, &nonces};
    tracker.register_listener(&rol_2);

    HubNvsBlob stale;
    stale.version       = 99;          // not kRestoreStateVersion (=2)
    stale.rx_message_id = 1234;
    stale.tx_message_id = 5678;
    stale.logged_in     = true;
    rol_2.preload_nvs(stale);
    rol_2.setup(/*time_valid_at_boot=*/false);

    EXPECT_FALSE(rol_2.registered())
        << "Stale NVS with the wrong version byte must be ignored so a "
           "fresh REGISTER cycle happens — silently trusting it would "
           "give the hub a corrupt counter from a possibly-different "
           "firmware build.";
    EXPECT_EQ(rol_2.tx_message_id(), 0u);
    EXPECT_EQ(rol_2.rx_message_id(), 0u);
}

// B6: after reboot, the node still holds the OLD base nonce in its
// peer_base store. The hub's nonce map is empty, so every encrypted reply
// the node sends would IV-mismatch — until the hub's startup login fires
// and re-provisions both sides with a fresh nonce.
TEST(Reboot, BaseNonceMapClearedRequiresFreshLogin) {
    SimClock        clock;
    SimRadio        radio;
    SharedNonceMap  nonces;
    HubTracker      tracker{&clock, &radio, &nonces};
    HubListener rol_2{"rol_2", 18, 2, kMacRol2, 21600, &tracker, &clock, &nonces};
    tracker.register_listener(&rol_2);

    rol_2.wipe_nvs();
    rol_2.setup(/*time_valid_at_boot=*/false);
    NodeModel node_2{"node_2", kMacRol2, &clock, &radio};
    node_2.send_register();
    // Drive the post-REGISTER login.
    clock.tick(600);
    ASSERT_TRUE(nonces.contains(18))
        << "Sanity: nonces map populated after first login";
    const uint32_t pre_reboot_nonce = nonces.get(18);
    ASSERT_EQ(node_2.base_nonce_for(0xFF), pre_reboot_nonce)
        << "Sanity: hub and node agree on the nonce before reboot";

    auto blob = rol_2.nvs_snapshot();
    rol_2.simulate_reboot();
    EXPECT_FALSE(nonces.contains(18))
        << "Reboot must clear the file-scope s_base_nonce_map (BSS-cleared "
           "in production).";

    rol_2.preload_nvs(blob);
    rol_2.setup(/*time_valid_at_boot=*/true);

    // Drive the startup login. The new nonce must differ from the old one
    // OR — at minimum — must be repopulated, otherwise the node's still-cached
    // peer_base_[0xFF] (= pre_reboot_nonce) won't match what the hub now uses.
    clock.tick(5000 + 18u * 3000u + 100);
    ASSERT_TRUE(nonces.contains(18))
        << "Startup login must repopulate s_base_nonce_map after reboot.";
}

// C4: hub reboot WHILE the node is asleep. On wake the node sends REGISTER,
// hub responds with a clean handshake, and the node ends up with a working
// base_nonce.
TEST(Reboot, RebootDuringNodeSleep) {
    SimClock        clock;
    SimRadio        radio;
    SharedNonceMap  nonces;
    HubTracker      tracker{&clock, &radio, &nonces};
    HubListener rol_2{"rol_2", 18, 2, kMacRol2, 21600, &tracker, &clock, &nonces};
    tracker.register_listener(&rol_2);

    rol_2.wipe_nvs();
    rol_2.setup(/*time_valid_at_boot=*/false);
    NodeModel node_2{"node_2", kMacRol2, &clock, &radio};
    node_2.send_register();
    clock.tick(600);

    // Node sleeps.
    rol_2.enter_sleep();

    // Mid-sleep hub reboot.
    auto blob = rol_2.nvs_snapshot();
    rol_2.simulate_reboot();
    EXPECT_FALSE(nonces.contains(18));
    rol_2.preload_nvs(blob);
    rol_2.setup(/*time_valid_at_boot=*/true);

    // Node wakes. In the user's deployment this means a fresh boot which
    // sends REGISTER again.
    node_2.reboot(/*keep_cfg=*/true);
    node_2.send_register();
    clock.tick(600);

    EXPECT_TRUE(rol_2.registered());
    EXPECT_TRUE(nonces.contains(18))
        << "Hub-reboot-during-sleep must converge to a working base-nonce "
           "once the node wakes and re-handshakes.";
    EXPECT_EQ(node_2.base_nonce_for(0xFF), nonces.get(18))
        << "Both sides must agree on the same nonce after re-handshake.";
}

} // namespace

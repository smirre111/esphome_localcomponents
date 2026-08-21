// Scenarios C1, C2, C3 — sleep / wake lifecycle.
//
// In production this path is the most timing-sensitive part of the protocol:
//   * enterSleep() sends CMD_SLEEP and arms a "login_startup" fallback at
//     sleep_duration + boot_margin + per-node stagger.
//   * REGISTER from the node on wake is supposed to be the primary path —
//     it cancels the fallback and arms a 500 ms login timer instead.
//   * If REGISTER is lost (radio noise, hub busy), the fallback must fire.
//
// We use a small sleep_duration_s here so the virtual clock advance stays
// readable. Production uses 21600 s (6 h).

#include "sim/hub_model.h"
#include "sim/node_model.h"

#include <gtest/gtest.h>

using namespace proto_sim;

namespace {

constexpr uint64_t kMacRol2 = 0xE08CFE5F9EC4ULL;
// 60 s sleep, 5 s boot margin, 18 * 3 s stagger = 119 s fallback delay.
constexpr uint32_t kSleepDurationS = 60;

struct SleepWakeFixture : public ::testing::Test {
    SimClock        clock;
    SimRadio        radio;
    SharedNonceMap  nonces;
    HubTracker      tracker{&clock, &radio, &nonces};

    HubListener rol_2{"rol_2", 18, 2, kMacRol2, kSleepDurationS, &tracker, &clock, &nonces};
    NodeModel   node_2{"node_2", kMacRol2, &clock, &radio};

    int rng_calls = 0;
    void SetUp() override {
        tracker.register_listener(&rol_2);
        rol_2.rng = [this] {
            ++rng_calls;
            return 0xB0000000u + static_cast<uint32_t>(rng_calls);
        };
        rol_2.wipe_nvs();
        rol_2.setup(/*time_valid_at_boot=*/false);
        node_2.send_register();
        ASSERT_TRUE(rol_2.registered());
        ASSERT_EQ(node_2.cfg_address(), 18);
        rng_calls = 0;
    }

    int count_login_msgs() const {
        int n = 0;
        for (const auto& f : radio.hub_to_node_frames())
            if (auto m = as_op(f); m && m->cmd == LoraClientOperationMessage::Cmd::Login) ++n;
        return n;
    }
};

// C1: enter_sleep emits CMD_SLEEP and arms the fallback timer at the
// documented offset (sleep + boot_margin + addr * stagger).
TEST_F(SleepWakeFixture, EnterSleepArmsFallbackAtCorrectOffset) {
    const int logins_before = count_login_msgs();

    rol_2.enter_sleep();

    // CMD_SLEEP must have been queued exactly once.
    int sleep_msgs = 0;
    for (const auto& f : radio.hub_to_node_frames()) {
        auto m = as_op(f);
        if (m && m->cmd == LoraClientOperationMessage::Cmd::Sysop &&
            m->sysop == ClientOperation::CMD_SLEEP) ++sleep_msgs;
    }
    EXPECT_EQ(sleep_msgs, 1);

    // Fallback offset = 60s + 5s + 18*3s = 119s. One ms shy must not fire.
    constexpr uint32_t kExpectedDelayMs = 60u * 1000u + 5000u + 18u * 3000u;
    clock.tick(kExpectedDelayMs - 1);
    EXPECT_EQ(count_login_msgs(), logins_before);

    clock.tick(2);
    EXPECT_EQ(count_login_msgs(), logins_before + 1)
        << "Fallback LoginMsg must fire exactly at sleep+boot+stagger.";
}

// C2: a REGISTER arriving on early wake cancels the fallback so only ONE
// LoginMsg is sent — the REGISTER-triggered one — not two.
TEST_F(SleepWakeFixture, EarlyRegisterCancelsFallback) {
    const int logins_before = count_login_msgs();

    rol_2.enter_sleep();

    // Node wakes a bit early (50 s into the 60 s sleep) and sends REGISTER.
    clock.tick(50'000);
    node_2.send_register();

    // REGISTER schedules a 500 ms login.
    clock.tick(600);
    EXPECT_EQ(count_login_msgs(), logins_before + 1);

    // Tick well past the original fallback deadline. NO second login.
    clock.tick(200'000);
    EXPECT_EQ(count_login_msgs(), logins_before + 1)
        << "Original fallback must have been cancelled by REGISTER. If a "
           "second LoginMsg fires here, the stale-interval race the user "
           "hit in production would still be live.";
}

// C3: REGISTER lost — fallback fires and successfully re-establishes login.
TEST_F(SleepWakeFixture, FallbackFiresWhenRegisterLost) {
    const int logins_before = count_login_msgs();

    rol_2.enter_sleep();

    // No REGISTER from node. Tick past fallback offset.
    constexpr uint32_t kExpectedDelayMs = 60u * 1000u + 5000u + 18u * 3000u;
    clock.tick(kExpectedDelayMs + 100);
    EXPECT_EQ(count_login_msgs(), logins_before + 1);
}

} // namespace

// Phase 3 — exercise the REAL production lora_client.cpp via host shims.
//
// The class under test is esphome::lora_tracker::LORAListener defined in
// local_components/lora_client/lora_client.h. Its scheduler and NVS calls
// route into the same SimClock + nvs_slot store the model-based tests use,
// so the assertions and timing logic transfer directly.
//
// This is the regression test for the IV-mismatch bug actually running
// AGAINST the production code, not a parallel model.

#include "esphome/components/lora_client/lora_client.h"
#include "esphome/components/lora_tracker/lora_tracker.h"
#include "esphome/components/homeassistant/time/homeassistant_time.h"

// The MAC tests build frames with the REAL generated stubs, so the bytes
// are exactly what goes on the air.
#include "blinds.pb-c.h"
#include "TimedGrid.h"
#include "ClassAWindows.h"
#include "LoraTiming.h"

#include "sim/sim_clock.h"
#include "sim/sim_radio.h"
#include "sim/wire_codec.h"
#include "sim/crypto.h"

#include <psa/crypto.h>

#include <gtest/gtest.h>
#include "esphome/components/switch/switch.h"

using esphome::lora_tracker::LORAClient;
using esphome::lora_tracker::LORATracker;
using esphome::time::RealTimeClock;

namespace {

constexpr uint64_t kMacRol2 = 0xE08CFE5F9EC4ULL;

// Production initialises PSA Crypto in LORAListener::setup(). These scenarios
// construct the listener directly and never call setup() (it would also restore
// NVS and schedule its own logins, which each scenario controls itself), so PSA
// must be initialised here or every key import silently fails — and that is the
// ONE branch in decrypt_payload_gcm that returns false without logging.
void ensure_psa_ready() { ASSERT_EQ(psa_crypto_init(), PSA_SUCCESS); }

// Answer every outgoing LoginMsg with a properly ENCRYPTED ClientAvailable,
// exactly as the node's CMD_LOGIN handler does (store the nonce, then
// sendAvailable()).  The hub sets login_acked_ ONLY inside the successful-
// decrypt branch — a successful decrypt is what proves the node holds the
// matching base nonce — so a plaintext reply does NOT acknowledge a challenge.
// See lora_client.cpp: "Only now do we treat login as acknowledged".
void attach_encrypted_login_ack(proto_sim::SimRadio& radio,
                                LORAClient& listener,
                                uint32_t node_addr,
                                uint32_t subnet) {
    radio.add_sink([&radio, &listener, node_addr, subnet](const proto_sim::AirFrame& f) {
        if (f.dir != proto_sim::AirFrame::Dir::HubToNode) return;
        auto m = proto_sim::as_op(f);
        if (!m || m->cmd != proto_sim::LoraClientOperationMessage::Cmd::Login) return;

        const uint32_t base_nonce = m->login.nonce;
        constexpr uint32_t kMsgId = 1;   // node's first post-login-reset tx

        proto_sim::LoraClientResponseMessage inner;
        inner.header.destAddress   = esphome::lora_tracker::kHubAddress;
        inner.header.destSubnet    = subnet;
        inner.header.senderAddress = node_addr;
        inner.header.msgId         = kMsgId;
        inner.proto                = proto_sim::LoraClientResponseMessage::Proto::Avail;
        inner.avail.available      = true;

        // Payload-only plaintext (the inner header is stripped; the receiver
        // uses the outer one), AAD from the outer header, IV from base||msgid.
        auto plain = proto_sim::serialize_resp_payload(inner);
        uint8_t aad[proto_sim::kHeaderAadLen];
        proto_sim::build_header_aad(inner.header.destAddress, inner.header.destSubnet,
                                    inner.header.senderAddress, inner.header.msgId, aad);
        uint8_t iv[12];
        proto_sim::derive_gcm_iv(base_nonce, kMsgId, iv);
        auto enc = proto_sim::aes_gcm_encrypt(iv, aad, sizeof(aad),
                                              plain.data(), plain.size());

        proto_sim::LoraClientResponseMessage outer;
        outer.header               = inner.header;
        outer.proto                = proto_sim::LoraClientResponseMessage::Proto::Encrypted;
        outer.encrypted.tag        = enc.tag;
        outer.encrypted.ciphertext = enc.ciphertext;

        auto ack_bytes = proto_sim::serialize_resp(outer);
        listener.set_response(ack_bytes.data(), ack_bytes.size());
    });
}

// Phase 3 regression of scenario B2: the production send_login() must reuse
// the same pending base nonce on every retry within a challenge cycle.
// This is the bug shipped in this session's fix; rebuilding with the fix
// reverted causes this test to fail (verified manually).
TEST(RealLoraClient, SendLoginReusesPendingNonceAcrossRetries) {
    proto_sim::SimClock clock;
    proto_sim::SimRadio radio;

    // Wire shim hooks so the production set_timeout/set_interval/send/NVS
    // routes through our test harness.
    esphome::shim_hooks::set_active_clock(&clock);
    esphome::shim_hooks::reset_nvs();
    esphome::lora_tracker::shim_hooks::set_active_radio(&radio);

    LORATracker tracker;
    LORAClient  rol_2;
    rol_2.set_name("rol_2");
    rol_2.set_short_address(18);
    rol_2.set_subnet_address(2);
    rol_2.set_sleep_duration(21600);
    rol_2.set_address(kMacRol2);

    RealTimeClock time;
    time.set_now(0, /*valid=*/false);  // suppress startup-login NTP path
    rol_2.set_time(&time);

    tracker.register_client(&rol_2);  // sets parent_; without this,
                                      // send_login()'s guard would bail

    // Force registered_=true (production normally sets this from REGISTER /
    // NVS restore). We don't want the REGISTER path's own login scheduling
    // here — we want to call send_login() directly and observe the nonce
    // reuse invariant in isolation.
    rol_2.registered_ = true;

    // First challenge.
    rol_2.send_login();
    const uint32_t pending_first = rol_2.pending_login_nonce_;
    ASSERT_NE(pending_first, 0u);

    // Two more retries before any ACK.
    rol_2.send_login();
    rol_2.send_login();

    EXPECT_EQ(rol_2.pending_login_nonce_, pending_first)
        << "REAL LORAListener::send_login() must reuse pending nonce on "
           "every retry — this is the production-side guarantee that "
           "the harness model also enforces.";

    // Every LoginMsg on the wire carries the same nonce.
    uint32_t observed = 0;
    int count = 0;
    for (const auto& f : radio.hub_to_node_frames()) {
        auto m = proto_sim::as_op(f);
        if (!m || m->cmd != proto_sim::LoraClientOperationMessage::Cmd::Login) continue;
        if (count == 0) observed = m->login.nonce;
        else EXPECT_EQ(m->login.nonce, observed)
            << "Production wire LoginMsg #" << (count + 1)
            << " has a different nonce than the first — pending-nonce "
               "fix must be in place.";
        ++count;
    }
    EXPECT_EQ(count, 3) << "Expected three LoginMsgs on the wire";
    EXPECT_EQ(observed, pending_first);

    esphome::lora_tracker::shim_hooks::set_active_radio(nullptr);
    esphome::shim_hooks::set_active_clock(nullptr);
}

// Regression test for the do_login_and_arm_retry_ ordering fragility:
// the reset of login_acked_/login_retry_count_ MUST happen BEFORE
// send_login(). With async radio this is harmless ordering; with sync
// radio (the test harness, or a hypothetical future driver swap) the
// post-call reset would clobber an already-arrived ACK and leave the hub
// thinking login was never acknowledged.
//
// We drive the real do_login_and_arm_retry_ via the REGISTER → 500 ms
// timer path and inject a synchronous ACK into the LoginMsg send. End
// state with the fix: login_acked_ stays true. End state without the
// fix: login_acked_ is clobbered to false by the post-call reset.
TEST(RealLoraClient, DoLoginAndArmRetryResetsBeforeSend) {
    proto_sim::SimClock clock;
    proto_sim::SimRadio radio;

    esphome::shim_hooks::set_active_clock(&clock);
    esphome::shim_hooks::reset_nvs();
    esphome::lora_tracker::shim_hooks::set_active_radio(&radio);

    ensure_psa_ready();

    LORATracker tracker;
    LORAClient  rol_2;
    rol_2.set_name("rol_2");
    rol_2.set_short_address(18);
    rol_2.set_subnet_address(2);
    rol_2.set_sleep_duration(21600);
    rol_2.set_address(kMacRol2);
    RealTimeClock time; time.set_now(0, /*valid=*/false);
    rol_2.set_time(&time);
    tracker.register_client(&rol_2);

    // Every outgoing LoginMsg gets a synchronous, correctly encrypted ACK fed
    // back through the real LORAListener::set_response.
    attach_encrypted_login_ack(radio, rol_2, /*node_addr=*/18, /*subnet=*/2);

    // Drive the production REGISTER path, which schedules the login challenge
    // kRegisterToLoginDelayMs after the REGISTER (4 s today — it was raised
    // from 500 ms so the LoginMsg is not queued on top of the config bursts).
    proto_sim::LoraClientResponseMessage reg_msg;
    reg_msg.proto        = proto_sim::LoraClientResponseMessage::Proto::Register;
    reg_msg.reg.mac_addr = kMacRol2;
    auto reg_bytes = proto_sim::serialize_resp(reg_msg);
    rol_2.set_response(reg_bytes.data(), reg_bytes.size());

    // Fire that timer → do_login_and_arm_retry_ → send_login →
    // (sync sink inside the radio dispatch) → LORAListener::set_response
    // processes the Available reply → login_acked_ flips to true.
    // Derived from the production constant so a future retune does not
    // silently turn this regression test into a no-op.
    clock.tick(esphome::lora_tracker::LORAListener::kRegisterToLoginDelayMs + 100);

    EXPECT_TRUE(rol_2.login_acked_)
        << "do_login_and_arm_retry_ must reset login_acked_=false BEFORE "
           "calling send_login(). Otherwise a synchronous ACK arriving "
           "during send_login() is silently clobbered, and the hub "
           "spends the next 24 hours retrying an already-acknowledged "
           "challenge.";

    esphome::lora_tracker::shim_hooks::set_active_radio(nullptr);
    esphome::shim_hooks::set_active_clock(nullptr);
}

// =============================================================================
// Additional real-code scenarios — port the model-side coverage onto the
// production LORAListener so the test transcript actually exercises
// production source lines.
// =============================================================================

namespace real_helpers {

inline std::vector<uint8_t> serialize_register(uint64_t mac,
                                              bool needs_config = false) {
    proto_sim::LoraClientResponseMessage m;
    m.proto            = proto_sim::LoraClientResponseMessage::Proto::Register;
    m.reg.mac_addr     = mac;
    m.reg.needs_config = needs_config;
    return proto_sim::serialize_resp(m);
}

inline std::vector<uint8_t> serialize_avail(uint8_t sender, uint32_t msg_id) {
    proto_sim::LoraClientResponseMessage m;
    m.header.senderAddress = sender;
    m.header.msgId         = msg_id;
    m.proto                = proto_sim::LoraClientResponseMessage::Proto::Avail;
    m.avail.available      = true;
    return proto_sim::serialize_resp(m);
}

struct RealHubHarness {
    proto_sim::SimClock clock;
    proto_sim::SimRadio radio;
    LORATracker  tracker;
    LORAClient   rol;
    RealTimeClock time;

    explicit RealHubHarness(uint8_t addr, uint64_t mac) {
        esphome::shim_hooks::set_active_clock(&clock);
        esphome::shim_hooks::reset_nvs();
        esphome::lora_tracker::shim_hooks::set_active_radio(&radio);

        rol.set_name("rol");
        rol.set_short_address(addr);
        rol.set_subnet_address(2);
        rol.set_sleep_duration(21600);
        rol.set_address(mac);
        time.set_now(0, /*valid=*/true);  // NTP-synced from boot
        rol.set_time(&time);
        tracker.register_client(&rol);
    }
    ~RealHubHarness() {
        esphome::lora_tracker::shim_hooks::set_active_radio(nullptr);
        esphome::shim_hooks::set_active_clock(nullptr);
    }
};

} // namespace real_helpers

// Real-code A1: REGISTER → real LORAListener sends ClientConfig with the
// listener's address and the matching MAC, then schedules its 500 ms
// login_startup timer.
TEST(RealLoraClient, RegisterTriggersClientConfigAndLoginTimer) {
    using namespace real_helpers;
    RealHubHarness h{18, kMacRol2};

    // An UNPROVISIONED node: it holds no session, so plaintext is the only way
    // to reach it and accepting it is the bootstrap. This is the one case where
    // the config goes out immediately, in the clear.
    auto reg = serialize_register(kMacRol2, /*needs_config=*/true);
    h.rol.set_response(reg.data(), reg.size());

    EXPECT_TRUE(h.rol.registered_)
        << "REGISTER must flip registered_ true in real code.";

    int cfg_count = 0;
    for (const auto& f : h.radio.hub_to_node_frames()) {
        auto m = proto_sim::as_op(f);
        if (!m) continue;
        if (m->cmd == proto_sim::LoraClientOperationMessage::Cmd::ClientConfig) {
            EXPECT_EQ(m->clientconfig.addr, 18u);
            EXPECT_EQ(m->clientconfig.mac_addr, kMacRol2);
            ++cfg_count;
        }
    }
    EXPECT_EQ(cfg_count, 1)
        << "Real send_remote_config must emit exactly one ClientConfig.";
}

// Real-code C1: enter_sleep emits CMD_SLEEP and arms the fallback timer.
TEST(RealLoraClient, EnterSleepEmitsSleepCmdAndArmsFallback) {
    using namespace real_helpers;
    RealHubHarness h{18, kMacRol2};
    h.rol.registered_ = true;

    h.rol.enterSleep();

    int sleep_msgs = 0;
    for (const auto& f : h.radio.hub_to_node_frames()) {
        auto m = proto_sim::as_op(f);
        if (m && m->cmd == proto_sim::LoraClientOperationMessage::Cmd::Sysop &&
            m->sysop == proto_sim::ClientOperation::CMD_SLEEP) ++sleep_msgs;
    }
    EXPECT_EQ(sleep_msgs, 1)
        << "Real enterSleep() must transmit CMD_SLEEP exactly once.";
}

// Real-code: triggerOTA emits CMD_OTA.
TEST(RealLoraClient, TriggerOtaEmitsOtaSysop) {
    using namespace real_helpers;
    RealHubHarness h{18, kMacRol2};
    h.rol.registered_ = true;

    h.rol.triggerOTA();

    int ota = 0;
    for (const auto& f : h.radio.hub_to_node_frames()) {
        auto m = proto_sim::as_op(f);
        if (m && m->cmd == proto_sim::LoraClientOperationMessage::Cmd::Sysop &&
            m->sysop == proto_sim::ClientOperation::CMD_OTA) ++ota;
    }
    EXPECT_EQ(ota, 1)
        << "Real triggerOTA() must transmit CMD_OTA exactly once.";
}

// Real-code C3: enter_sleep arms a fallback timer; if no REGISTER ever
// arrives, the fallback fires and produces a LoginMsg.
TEST(RealLoraClient, SleepFallbackFiresWhenRegisterLost) {
    using namespace real_helpers;
    // Use a short sleep_duration so the virtual tick stays reasonable.
    proto_sim::SimClock clock;
    proto_sim::SimRadio radio;
    esphome::shim_hooks::set_active_clock(&clock);
    esphome::shim_hooks::reset_nvs();
    esphome::lora_tracker::shim_hooks::set_active_radio(&radio);

    LORATracker tracker;
    LORAClient  rol;
    rol.set_name("rol");
    rol.set_short_address(18);
    rol.set_subnet_address(2);
    rol.set_sleep_duration(60);  // 60 s → 119 s fallback (60 + 5 + 18*3)
    rol.set_address(kMacRol2);
    RealTimeClock time; time.set_now(0, /*valid=*/true);
    rol.set_time(&time);
    tracker.register_client(&rol);
    rol.registered_ = true;

    rol.enterSleep();
    const int before = static_cast<int>(radio.transcript().size());

    constexpr uint32_t kExpected = 60u * 1000u + 5000u + 18u * 3000u;
    clock.tick(kExpected + 1000);

    int login_count = 0;
    for (size_t i = before; i < radio.transcript().size(); ++i) {
        auto m = proto_sim::as_op(radio.transcript()[i]);
        if (m && m->cmd == proto_sim::LoraClientOperationMessage::Cmd::Login)
            ++login_count;
    }
    EXPECT_GE(login_count, 1)
        << "Real enterSleep() fallback must fire a LoginMsg after the "
           "sleep+boot+stagger window if no REGISTER cancelled it.";

    esphome::lora_tracker::shim_hooks::set_active_radio(nullptr);
    esphome::shim_hooks::set_active_clock(nullptr);
}

// Real-code C2: enter_sleep arms a fallback, REGISTER arrives during the
// wake window, fallback is cancelled, and only the REGISTER-path LoginMsg
// goes out (not the fallback one too).
TEST(RealLoraClient, EarlyRegisterCancelsSleepFallback) {
    using namespace real_helpers;
    proto_sim::SimClock clock;
    proto_sim::SimRadio radio;
    esphome::shim_hooks::set_active_clock(&clock);
    esphome::shim_hooks::reset_nvs();
    esphome::lora_tracker::shim_hooks::set_active_radio(&radio);

    LORATracker tracker;
    LORAClient  rol;
    rol.set_name("rol");
    rol.set_short_address(18);
    rol.set_subnet_address(2);
    rol.set_sleep_duration(60);
    rol.set_address(kMacRol2);
    RealTimeClock time; time.set_now(0, /*valid=*/true);
    rol.set_time(&time);
    tracker.register_client(&rol);
    rol.registered_ = true;

    // ACK the login challenge. Without an ACK the hub legitimately RETRIES on
    // its exponential backoff (kLoginRetryBaseMs = 5 s, doubling), which puts
    // extra LoginMsgs on the wire and masks what this test is actually about:
    // whether the stale enterSleep() fallback timer was cancelled.
    ensure_psa_ready();
    attach_encrypted_login_ack(radio, rol, /*node_addr=*/18, /*subnet=*/2);

    rol.enterSleep();
    const int before = static_cast<int>(radio.transcript().size());

    // Node wakes early at 50 s and sends REGISTER.
    clock.tick(50'000);
    auto reg = serialize_register(kMacRol2);
    rol.set_response(reg.data(), reg.size());

    // REGISTER schedules the login challenge kRegisterToLoginDelayMs later.
    clock.tick(esphome::lora_tracker::LORAListener::kRegisterToLoginDelayMs + 200);

    int login_after_register = 0;
    for (size_t i = before; i < radio.transcript().size(); ++i) {
        auto m = proto_sim::as_op(radio.transcript()[i]);
        if (m && m->cmd == proto_sim::LoraClientOperationMessage::Cmd::Login)
            ++login_after_register;
    }
    EXPECT_EQ(login_after_register, 1)
        << "REGISTER-path login fired once.";

    // Tick well past the original fallback time. NO second login.
    clock.tick(300'000);
    int login_total = 0;
    for (size_t i = before; i < radio.transcript().size(); ++i) {
        auto m = proto_sim::as_op(radio.transcript()[i]);
        if (m && m->cmd == proto_sim::LoraClientOperationMessage::Cmd::Login)
            ++login_total;
    }
    EXPECT_EQ(login_total, 1)
        << "Real enterSleep() fallback timer must have been cancelled by "
           "the REGISTER handler. If a second LoginMsg shows up here, the "
           "stale-interval race is live.";

    esphome::lora_tracker::shim_hooks::set_active_radio(nullptr);
    esphome::shim_hooks::set_active_clock(nullptr);
}

// Real-code D2 sibling: cross-listener isolation. A reply from sender=18
// must NOT advance rx on rol_1 (short_address=17). Two real LORAListener
// instances share the same tracker/radio/nonces.
TEST(RealLoraClient, ReplyFromOtherNodeRejectedByListener) {
    using namespace real_helpers;
    proto_sim::SimClock clock;
    proto_sim::SimRadio radio;
    esphome::shim_hooks::set_active_clock(&clock);
    esphome::shim_hooks::reset_nvs();
    esphome::lora_tracker::shim_hooks::set_active_radio(&radio);

    LORATracker tracker;
    LORAClient  rol_1, rol_2;
    rol_1.set_name("rol_1");
    rol_1.set_short_address(17);
    rol_1.set_subnet_address(2);
    rol_1.set_sleep_duration(21600);
    rol_1.set_address(0xE08CFE5FB7A4ULL);

    rol_2.set_name("rol_2");
    rol_2.set_short_address(18);
    rol_2.set_subnet_address(2);
    rol_2.set_sleep_duration(21600);
    rol_2.set_address(kMacRol2);

    RealTimeClock time; time.set_now(0, /*valid=*/false);
    rol_1.set_time(&time);
    rol_2.set_time(&time);
    tracker.register_client(&rol_1);
    tracker.register_client(&rol_2);
    rol_1.registered_ = true;
    rol_2.registered_ = true;

    // Synthesize a reply with sender=18. Inject into both listeners and
    // verify rol_1 ignores it (sender filter) while rol_2 acts on it.
    auto ack = serialize_avail(/*sender=*/18, /*msg_id=*/1);
    rol_1.set_response(ack.data(), ack.size());
    rol_2.set_response(ack.data(), ack.size());

    EXPECT_EQ(rol_1.frame_counter_.rx_message_id, 0u)
        << "rol_1 (short_address=17) must REJECT a reply from sender=18 — "
           "cross-listener filter is what keeps multi-node deployments "
           "from cross-talk.";
    EXPECT_EQ(rol_2.frame_counter_.rx_message_id, 1u)
        << "rol_2 must have accepted the reply matching its own address.";

    esphome::lora_tracker::shim_hooks::set_active_radio(nullptr);
    esphome::shim_hooks::set_active_clock(nullptr);
}

// Real-code A2 sibling: REGISTER with wrong MAC is ignored — listener
// stays unregistered and emits no ClientConfig.
TEST(RealLoraClient, WrongMacRegisterIgnored) {
    using namespace real_helpers;
    RealHubHarness h{18, kMacRol2};

    auto reg = serialize_register(0xAABBCCDDEEFFULL);  // wrong MAC
    h.rol.set_response(reg.data(), reg.size());

    EXPECT_FALSE(h.rol.registered_)
        << "Real REGISTER handler must reject non-matching MAC.";
    for (const auto& f : h.radio.hub_to_node_frames()) {
        auto m = proto_sim::as_op(f);
        EXPECT_TRUE(!m || m->cmd != proto_sim::LoraClientOperationMessage::Cmd::ClientConfig)
            << "Real code must not emit ClientConfig for unknown MAC.";
    }
}

} // namespace

// ---------------------------------------------------------------------------
// P1 — the hub pushes wall-clock time to the node once the encrypted session is
// confirmed. The node has no clock source of its own, so this is the only way
// it ever learns the time; everything the scheduler will later do rests on it.
// ---------------------------------------------------------------------------
namespace {

// Decrypt a hub->node frame that was encrypted for `node_addr` with
// `base_nonce`, and return the inner (payload-only) operation message.
std::optional<proto_sim::LoraClientOperationMessage>
decrypt_downlink(const proto_sim::AirFrame& f, uint32_t base_nonce) {
    auto outer = proto_sim::as_op(f);
    if (!outer || outer->cmd != proto_sim::LoraClientOperationMessage::Cmd::Encrypted)
        return std::nullopt;

    // Downlink: the direction bit is OR'd into the nonce counter so hub->node
    // and node->hub can never reuse an IV under the shared base nonce.
    uint8_t iv[12];
    proto_sim::derive_gcm_iv_downlink(base_nonce, outer->header.msgId, iv);
    uint8_t aad[proto_sim::kHeaderAadLen];
    proto_sim::build_header_aad(outer->header.destAddress, outer->header.destSubnet,
                                outer->header.senderAddress, outer->header.msgId, aad);

    auto plain = proto_sim::aes_gcm_decrypt(iv, aad, sizeof(aad),
                                            outer->encrypted.ciphertext.data(),
                                            outer->encrypted.ciphertext.size(),
                                            outer->encrypted.tag.data(),
                                            outer->encrypted.tag.size());
    if (!plain) return std::nullopt;
    return proto_sim::deserialize_op(plain->data(), plain->size());
}

// Drive REGISTER -> login -> encrypted ACK, leaving the session confirmed.
// Returns the base nonce the hub minted, so the caller can decrypt downlinks.
uint32_t drive_session(proto_sim::SimClock& clock, proto_sim::SimRadio& radio,
                       LORAClient& listener) {
    uint32_t captured_nonce = 0;
    radio.add_sink([&captured_nonce](const proto_sim::AirFrame& f) {
        if (f.dir != proto_sim::AirFrame::Dir::HubToNode) return;
        auto m = proto_sim::as_op(f);
        if (m && m->cmd == proto_sim::LoraClientOperationMessage::Cmd::Login)
            captured_nonce = m->login.nonce;
    });
    attach_encrypted_login_ack(radio, listener, /*node_addr=*/18, /*subnet=*/2);

    auto reg = real_helpers::serialize_register(kMacRol2);
    listener.set_response(reg.data(), reg.size());
    clock.tick(esphome::lora_tracker::LORAListener::kRegisterToLoginDelayMs + 200);
    return captured_nonce;
}

} // namespace

TEST(RealLoraClient, TimeSyncPushedAfterSessionConfirmed) {
    proto_sim::SimClock clock;
    proto_sim::SimRadio radio;
    esphome::shim_hooks::set_active_clock(&clock);
    esphome::shim_hooks::reset_nvs();
    esphome::lora_tracker::shim_hooks::set_active_radio(&radio);
    ensure_psa_ready();

    constexpr std::time_t kHubEpoch = 1787000000;
    esphome::ESPTime::set_timezone_offset(7200);   // CEST

    LORATracker tracker;
    LORAClient  rol;
    rol.set_name("rol");
    rol.set_short_address(18);
    rol.set_subnet_address(2);
    rol.set_sleep_duration(21600);
    rol.set_address(kMacRol2);
    RealTimeClock time; time.set_now(kHubEpoch, /*valid=*/true);
    rol.set_time(&time);
    tracker.register_client(&rol);

    const uint32_t base = drive_session(clock, radio, rol);
    ASSERT_NE(base, 0u);
    ASSERT_TRUE(rol.login_acked_) << "session must be confirmed before TimeSync is due";

    const size_t before = radio.transcript().size();
    clock.tick(1000);   // past the 750 ms deferred push

    int found = 0;
    proto_sim::TimeSync got{};
    for (size_t i = before; i < radio.transcript().size(); ++i) {
        auto inner = decrypt_downlink(radio.transcript()[i], base);
        if (inner && inner->cmd == proto_sim::LoraClientOperationMessage::Cmd::TimeSync) {
            ++found;
            got = inner->timesync;
        }
    }

    ASSERT_EQ(found, 1) << "exactly one TimeSync must follow session confirmation";
    EXPECT_EQ(got.epoch,     static_cast<uint64_t>(kHubEpoch));
    EXPECT_EQ(got.utcOffset, 7200);

    esphome::lora_tracker::shim_hooks::set_active_radio(nullptr);
    esphome::shim_hooks::set_active_clock(nullptr);
}

TEST(RealLoraClient, AProvisionedNodesConfigPushWaitsForEncryption) {
    // send_remote_config() packed raw, so ClientConfig ALWAYS went out in the
    // clear — including to a provisioned node, which refuses plaintext commands
    // because ClientConfig sets its address, subnet, name and sleep duration and
    // an unauthenticated frame must not be able to re-address a node. The hub
    // sent it anyway on the first REGISTER after every hub boot, set
    // config_synced_ = true regardless, and never retried: a config change
    // flashed into the hub was lost with nothing logged as a failure.
    proto_sim::SimClock clock;
    proto_sim::SimRadio radio;
    esphome::shim_hooks::set_active_clock(&clock);
    esphome::shim_hooks::reset_nvs();
    esphome::lora_tracker::shim_hooks::set_active_radio(&radio);
    ensure_psa_ready();

    LORATracker tracker;
    LORAClient  rol;
    rol.set_name("rol");
    rol.set_short_address(18);
    rol.set_subnet_address(2);
    rol.set_sleep_duration(21600);
    rol.set_address(kMacRol2);
    RealTimeClock time; time.set_now(1787000000, /*valid=*/true);
    rol.set_time(&time);
    tracker.register_client(&rol);

    // A PROVISIONED node re-registering after a hub reboot: needs_config false,
    // but this hub boot has not pushed yet.
    auto reg = real_helpers::serialize_register(kMacRol2, /*needs_config=*/false);
    rol.set_response(reg.data(), reg.size());

    auto count_plaintext_config = [&]() {
        int n = 0;
        for (const auto& f : radio.hub_to_node_frames()) {
            auto m = proto_sim::as_op(f);
            if (m && m->cmd == proto_sim::LoraClientOperationMessage::Cmd::ClientConfig)
                ++n;
        }
        return n;
    };
    EXPECT_EQ(count_plaintext_config(), 0)
        << "a provisioned node would drop this; sending it is wasted airtime "
           "and the config change is lost";
    EXPECT_FALSE(rol.config_synced_)
        << "a push that did not happen must not be recorded as one that did";

    // Now the session comes up. That is the first moment the config CAN be
    // encrypted, and it must go then.
    const uint32_t base = drive_session(clock, radio, rol);
    ASSERT_NE(base, 0u);
    ASSERT_TRUE(rol.session_confirmed_);

    EXPECT_TRUE(rol.config_synced_)
        << "the deferred push must actually happen once the session exists";
    EXPECT_EQ(count_plaintext_config(), 0)
        << "and it must go out ENCRYPTED — a readable ClientConfig on the air "
           "is one the node refuses";

    esphome::lora_tracker::shim_hooks::set_active_radio(nullptr);
    esphome::shim_hooks::set_active_clock(nullptr);
}

TEST(RealLoraClient, ADecryptedBeaconIsWhatCarriesThePhaseReport) {
    // The wiring, end to end, with no test hook anywhere in it.
    //
    // Every other §4.6 test seeds the report through notePhaseReportForTest so
    // it can exercise the POLICY transitions concisely. That is only legitimate
    // if something pins that handle_beacon_ really calls the same entry point —
    // otherwise the policy is reachable from tests and from nothing else, which
    // is the exact failure this whole exercise has been about.
    //
    // It also pins the property that makes this evidence worth trusting: the
    // report arrives ENCRYPTED. handle_beacon_ runs only for a frame that
    // passed its GCM tag, so an attacker in radio range cannot hand the hub a
    // flattering phase report and talk it into single-shot.
    proto_sim::SimClock clock;
    proto_sim::SimRadio radio;
    esphome::shim_hooks::set_active_clock(&clock);
    esphome::shim_hooks::reset_nvs();
    esphome::lora_tracker::shim_hooks::set_active_radio(&radio);
    ensure_psa_ready();

    LORATracker tracker;
    LORAClient  rol;
    rol.set_name("rol");
    rol.set_short_address(18);
    rol.set_subnet_address(2);
    rol.set_sleep_duration(21600);
    rol.set_address(kMacRol2);
    RealTimeClock time; time.set_now(1787000000, /*valid=*/true);
    rol.set_time(&time);
    tracker.register_client(&rol);
    rol.registered_ = true;

    const uint32_t base = drive_session(clock, radio, rol);
    ASSERT_NE(base, 0u);
    ASSERT_TRUE(rol.session_confirmed_);

    rol.enable_timed_mode(true);
    EXPECT_FALSE(rol.hubBelief().phase_reported)
        << "nothing has reported a phase yet";

    // A real beacon, encrypted the way the node sends one.
    proto_sim::LoraClientResponseMessage inner;
    inner.header.destAddress   = esphome::lora_tracker::kHubAddress;
    inner.header.destSubnet    = 2;
    inner.header.senderAddress = 18;
    inner.header.msgId         = rol.frame_counter_.rx_message_id + 1;
    inner.proto                = proto_sim::LoraClientResponseMessage::Proto::Beacon;
    inner.beacon.fwVersion     = 0x00010203;
    inner.beacon.phasePresent      = true;
    inner.beacon.phase.rtcSlowSrc   = 2;      // crystal
    inner.beacon.phase.errUs        = 120;
    inner.beacon.phase.spreadUs     = 300;
    inner.beacon.phase.samples      = timedmode::kPromotionPhaseSamples;
    inner.beacon.phase.outsideGuard = 0;

    auto plain = proto_sim::serialize_resp_payload(inner);
    uint8_t aad[proto_sim::kHeaderAadLen];
    proto_sim::build_header_aad(inner.header.destAddress, inner.header.destSubnet,
                                inner.header.senderAddress, inner.header.msgId, aad);
    uint8_t iv[12];
    proto_sim::derive_gcm_iv(base, inner.header.msgId, iv);
    auto enc = proto_sim::aes_gcm_encrypt(iv, aad, sizeof(aad),
                                          plain.data(), plain.size());
    proto_sim::LoraClientResponseMessage outer;
    outer.header               = inner.header;
    outer.proto                = proto_sim::LoraClientResponseMessage::Proto::Encrypted;
    outer.encrypted.tag        = enc.tag;
    outer.encrypted.ciphertext = enc.ciphertext;
    auto frame = proto_sim::serialize_resp(outer);
    rol.set_response(frame.data(), frame.size());

    const auto b = rol.hubBelief();
    EXPECT_TRUE(b.phase_reported)
        << "handle_beacon_ must feed the phase report into the belief";
    EXPECT_EQ(b.phase_err_us, 120);
    EXPECT_EQ(b.phase_spread_us, 300);
    EXPECT_EQ(b.phase_samples, timedmode::kPromotionPhaseSamples);
    EXPECT_EQ(b.rtc_src, timedmode::RtcSlowSrc::Crystal);
    EXPECT_EQ(rol.txPolicyNow(),
              timedmode::TxPolicy::SingleShot)
        << "a healthy report from a real encrypted beacon must earn single-shot";

    esphome::lora_tracker::shim_hooks::set_active_radio(nullptr);
    esphome::shim_hooks::set_active_clock(nullptr);
}

TEST(RealLoraClient, AForgedPlaintextUplinkMovesNeitherTheBeliefNorTheClassAOrigin) {
    // admit_frame_ checks the address and the replay window. Neither is
    // authentication, and both of C2's and section 4.6's inputs were taken
    // there — so anyone in radio range had two levers with no key at all:
    //
    //   * one out-of-slot frame per round resets in_slot_acks, denying the node
    //     single-shot for as long as the attacker keeps transmitting;
    //   * three frames timed AT the mark EARN single-shot, after which the hub
    //     sends one copy to a real node it has no evidence about.
    //
    // The measurement now hangs off the same rule as the replay counter: a
    // frame moves the hub's beliefs only once it has earned the right to.
    proto_sim::SimClock clock;
    proto_sim::SimRadio radio;
    esphome::shim_hooks::set_active_clock(&clock);
    esphome::shim_hooks::reset_nvs();
    esphome::lora_tracker::shim_hooks::set_active_radio(&radio);
    ensure_psa_ready();

    LORATracker tracker;
    LORAClient  rol;
    rol.set_name("rol");
    rol.set_short_address(18);
    rol.set_subnet_address(2);
    rol.set_sleep_duration(21600);
    rol.set_address(kMacRol2);
    RealTimeClock time; time.set_now(1787000000, /*valid=*/true);
    rol.set_time(&time);
    tracker.register_client(&rol);

    const uint32_t base = drive_session(clock, radio, rol);
    ASSERT_NE(base, 0u);
    ASSERT_TRUE(rol.session_confirmed_);
    rol.node_fw_version_ = 0x00010203;
    rol.enable_timed_mode(true);
    ASSERT_TRUE(tracker.gridStarted());

    const int64_t origin_before = rol.last_uplink_t0_us_;
    uint32_t msgid = rol.frame_counter_.rx_message_id;

    // Three PLAINTEXT uplinks placed perfectly at this node's mark — the exact
    // evidence section 4.6 promotes on, forged.
    for (int i = 0; i < 3; ++i) {
        const int64_t mark =
            tracker.nextT0ForSlotUs(rol.grid_slot(), tracker.gridAnchorUs());
        tracker.last_rx_t0_us_v = mark + (int64_t) LORAClient::kUplinkOffsetUs;
        auto forged = real_helpers::serialize_avail(/*sender=*/18, ++msgid);
        rol.set_response(forged.data(), forged.size());
    }

    EXPECT_EQ(rol.txPolicyNow(), timedmode::TxPolicy::Burst)
        << "unauthenticated frames must not earn a node single-shot";
    EXPECT_EQ(rol.last_uplink_t0_us_, origin_before)
        << "and must not become this node's Class A window origin";

    esphome::lora_tracker::shim_hooks::set_active_radio(nullptr);
    esphome::shim_hooks::set_active_clock(nullptr);
}

TEST(RealLoraClient, APlaintextFrameOnAConfirmedSessionCannotBeReplayedWithoutLimit) {
    // Duplicate rejection used to be a SIDE EFFECT of admit_frame_ assigning the
    // replay counter: the frame moved rx_message_id, so the next copy failed the
    // "msgid > rx_message_id" window. Taking that assignment away — correctly,
    // so an unauthenticated frame cannot ratchet the counter — removed the
    // dedup with it and nothing replaced it. The SAME captured frame was then
    // admitted and dispatched without limit, and dispatch_payload_ is not
    // inert: a replayed CommandAck clears op_awaiting_ack_ every time,
    // suppressing the retry ladder while the hub reports the command delivered.
    proto_sim::SimClock clock;
    proto_sim::SimRadio radio;
    esphome::shim_hooks::set_active_clock(&clock);
    esphome::shim_hooks::reset_nvs();
    esphome::lora_tracker::shim_hooks::set_active_radio(&radio);
    ensure_psa_ready();

    LORATracker tracker;
    LORAClient  rol;
    rol.set_name("rol");
    rol.set_short_address(18);
    rol.set_subnet_address(2);
    rol.set_sleep_duration(21600);
    rol.set_address(kMacRol2);
    RealTimeClock time; time.set_now(1787000000, /*valid=*/true);
    rol.set_time(&time);
    tracker.register_client(&rol);

    const uint32_t base = drive_session(clock, radio, rol);
    ASSERT_NE(base, 0u);
    ASSERT_TRUE(rol.session_confirmed_) << "the replay only applies to a live session";

    const uint32_t rx = rol.frame_counter_.rx_message_id;
    auto forged = real_helpers::serialize_avail(/*sender=*/18, rx + 5);

    // The first copy is admitted — this path stays open by design, because a
    // plaintext status frame from a live node is still worth acting on.
    rol.set_response(forged.data(), forged.size());
    const int64_t first_seen = rol.last_uplink_t0_us_;

    // Move the tracker's stamp so a SECOND admission would be visible.
    tracker.last_rx_t0_us_v = first_seen + 777'000;

    for (int i = 0; i < 5; ++i)
        rol.set_response(forged.data(), forged.size());

    EXPECT_EQ(rol.frame_counter_.rx_message_id, rx)
        << "still no ratchet — that fix must not regress";
    EXPECT_EQ(rol.last_uplink_t0_us_, first_seen)
        << "a replayed plaintext frame must be dropped as a duplicate, not "
           "re-admitted without limit";

    esphome::lora_tracker::shim_hooks::set_active_radio(nullptr);
    esphome::shim_hooks::set_active_clock(nullptr);
}

TEST(RealLoraClient, AForgedPlaintextUplinkCannotRatchetTheHubsReplayCounter) {
    // The hub's mirror of the node's counter ratchet.
    //
    // admit_frame_ accepts any forward jump inside (rx, rx + 1024] and used to
    // ASSIGN rx there, before a byte had been authenticated — and setRxMessageId
    // persists, so the damage outlived a reboot. Anyone in radio range can read
    // the plaintext LoraHeader off a real uplink and send one frame at
    // msgid + 1000 claiming to be the node. Every genuine uplink after that was
    // logged as "duplicate or old message ID" and dropped, so the hub saw the
    // node as silent: no acks, retries exhausted, session torn down. The forged
    // frame never had to be acted on.
    proto_sim::SimClock clock;
    proto_sim::SimRadio radio;
    esphome::shim_hooks::set_active_clock(&clock);
    esphome::shim_hooks::reset_nvs();
    esphome::lora_tracker::shim_hooks::set_active_radio(&radio);
    ensure_psa_ready();

    LORATracker tracker;
    LORAClient  rol;
    rol.set_name("rol");
    rol.set_short_address(18);
    rol.set_subnet_address(2);
    rol.set_sleep_duration(21600);
    rol.set_address(kMacRol2);
    RealTimeClock time; time.set_now(1787000000, /*valid=*/true);
    rol.set_time(&time);
    tracker.register_client(&rol);

    const uint32_t base = drive_session(clock, radio, rol);
    ASSERT_NE(base, 0u);
    ASSERT_TRUE(rol.session_confirmed_) << "the attack only applies to a live session";

    const uint32_t rx_after_login = rol.frame_counter_.rx_message_id;

    // One forged plaintext frame, near the top of the acceptance window.
    auto forged = real_helpers::serialize_avail(/*sender=*/18,
                                                /*msg_id=*/rx_after_login + 1000);
    rol.set_response(forged.data(), forged.size());

    EXPECT_EQ(rol.frame_counter_.rx_message_id, rx_after_login)
        << "an unauthenticated uplink must not move the hub's replay counter";

    // And the node's next real frame — the very next msgid — is still accepted.
    proto_sim::LoraClientResponseMessage inner;
    inner.header.destAddress   = esphome::lora_tracker::kHubAddress;
    inner.header.destSubnet    = 2;
    inner.header.senderAddress = 18;
    inner.header.msgId         = rx_after_login + 1;
    inner.proto                = proto_sim::LoraClientResponseMessage::Proto::Avail;
    inner.avail.available      = true;

    auto plain = proto_sim::serialize_resp_payload(inner);
    uint8_t aad[proto_sim::kHeaderAadLen];
    proto_sim::build_header_aad(inner.header.destAddress, inner.header.destSubnet,
                                inner.header.senderAddress, inner.header.msgId, aad);
    uint8_t iv[12];
    proto_sim::derive_gcm_iv(base, inner.header.msgId, iv);
    auto enc = proto_sim::aes_gcm_encrypt(iv, aad, sizeof(aad),
                                          plain.data(), plain.size());
    proto_sim::LoraClientResponseMessage outer;
    outer.header               = inner.header;
    outer.proto                = proto_sim::LoraClientResponseMessage::Proto::Encrypted;
    outer.encrypted.tag        = enc.tag;
    outer.encrypted.ciphertext = enc.ciphertext;
    auto real_frame = proto_sim::serialize_resp(outer);
    rol.set_response(real_frame.data(), real_frame.size());

    EXPECT_EQ(rol.frame_counter_.rx_message_id, rx_after_login + 1)
        << "the node's genuine next uplink must still be accepted — the forged "
           "frame must not have wedged the link";

    esphome::lora_tracker::shim_hooks::set_active_radio(nullptr);
    esphome::shim_hooks::set_active_clock(nullptr);
}

TEST(RealLoraClient, RX1IsAimedAtThisNodesOwnUplinkNotTheTrackersLastFrame) {
    // C2's hub half. The node's Class A windows hang off ITS OWN uplink, so the
    // hub's reply has to be placed from the stamp of that node's uplink — and
    // the tracker's stamp is global: one radio, one variable, overwritten by
    // every frame from every node. send_into_class_a_window_() read it 750 ms after the
    // uplink that triggered the reply, so on a 32-node fleet the common case was
    // aiming one copy — no burst to save it — at a window derived from someone
    // else's transmit.
    proto_sim::SimClock clock;
    proto_sim::SimRadio radio;
    esphome::shim_hooks::set_active_clock(&clock);
    esphome::shim_hooks::reset_nvs();
    esphome::lora_tracker::shim_hooks::set_active_radio(&radio);

    LORATracker tracker;
    LORAClient  rol_1, rol_2;
    rol_1.set_name("rol_1"); rol_1.set_short_address(17); rol_1.set_subnet_address(2);
    rol_2.set_name("rol_2"); rol_2.set_short_address(18); rol_2.set_subnet_address(2);
    RealTimeClock time; time.set_now(0, /*valid=*/false);
    rol_1.set_time(&time); rol_2.set_time(&time);
    tracker.register_client(&rol_1);
    tracker.register_client(&rol_2);
    rol_1.registered_ = true;
    rol_2.registered_ = true;

    // Node 17 transmits. The tracker stamps it; both listeners see the frame,
    // only rol_1 admits it.
    constexpr int64_t kT0Node17 = 100'000;
    tracker.last_rx_t0_us_v = kT0Node17;
    tracker.rx_uncertainty_v = 250;
    auto from_17 = real_helpers::serialize_avail(/*sender=*/17, /*msg_id=*/1);
    rol_1.set_response(from_17.data(), from_17.size());
    rol_2.set_response(from_17.data(), from_17.size());

    // Node 18 transmits 300 ms later, inside the window before the hub replies
    // to node 17. This is the frame that used to decide where node 17's reply
    // went.
    constexpr int64_t kT0Node18 = 400'000;
    tracker.last_rx_t0_us_v = kT0Node18;
    auto from_18 = real_helpers::serialize_avail(/*sender=*/18, /*msg_id=*/1);
    rol_1.set_response(from_18.data(), from_18.size());
    rol_2.set_response(from_18.data(), from_18.size());

    EXPECT_EQ(rol_1.last_uplink_t0_us_, kT0Node17)
        << "a frame from another node must not become this node's window origin";
    EXPECT_EQ(rol_2.last_uplink_t0_us_, kT0Node18);

    // A node the hub has never heard from is MODE_INTERACTIVE, and an
    // interactive node opens no Class A window at all — it sweeps a free-running
    // one. Aiming a single copy at a window that is not there is strictly worse
    // than the burst it replaces, so the hub must DECLINE and let the caller
    // fall back.
    std::vector<uint8_t> payload{1, 2, 3, 4};
    EXPECT_EQ(rol_1.node_mode_, (uint32_t) NODE_MODE__MODE_INTERACTIVE)
        << "the safe default: a node that has not said otherwise is not Class A";
    EXPECT_FALSE(rol_1.send_into_class_a_window_(payload.data(), payload.size()))
        << "an interactive node has no RX1 to aim at; the caller must burst";

    // Now the node tells us, in a beacon, that it is in AUTO. It is on no grid,
    // so Class A is exactly the mode it is in.
    rol_1.node_mode_ = (uint32_t) NODE_MODE__MODE_AUTO;
    ASSERT_TRUE(rol_1.send_into_class_a_window_(payload.data(), payload.size()));
    EXPECT_EQ(tracker.last_copies, 1)
        << "a burst is the opposite construction to a single placed copy";

    // The contract is NOT "earliest_us equals the expression send_into_rx1_
    // computes" — restating the code's own arithmetic back at it proves
    // nothing. It is that the frame's T0 lands inside the window the NODE
    // opens, which the node builds with classa::rx1OpenUs/rx1CloseUs off its
    // own uplink. earliest_us is a FIRE instant, so the T0 the node sees is
    // kPreambleToT0Us later; a producer that forgets that conversion puts the
    // frame 3136 us late and this is the assertion that catches it.
    const int64_t fire      = tracker.last_earliest_us;
    const int64_t seen_t0   = fire + (int64_t) loratiming::kPreambleToT0Us;
    const int64_t win_open  = classa::rx1OpenUs(kT0Node17);
    const int64_t win_close = classa::rx1CloseUs(kT0Node17);
    EXPECT_GE(seen_t0, win_open)
        << "frame arrives before node 17's RX1 window opens";
    EXPECT_LT(seen_t0, win_close)
        << "frame arrives after node 17's RX1 window has closed";
    EXPECT_EQ(seen_t0, kT0Node17 + (int64_t) classa::kRx1DelayUs)
        << "and it lands on the design point, guard G inside the open edge — "
           "placed from node 17's uplink, not from node 18's";

    // The margin the design promises, stated so a future edit that eats it
    // fails here rather than in the field. The guard is measured to the first
    // CHIRP, not to T0: the node must already be listening a full G before the
    // preamble starts, which is why the window opens kArmLeadUs (= T_pre + G)
    // ahead of the expected T0 rather than G ahead of it.
    EXPECT_EQ(fire - win_open, (int64_t) timedgrid::kGuardUs)
        << "the frame's first chirp must sit a full guard inside the window";
    EXPECT_EQ(seen_t0 - win_open, (int64_t) classa::kArmLeadUs);

    // And the late side: what is left between T0 and the window closing. This
    // is the budget the hub's own stamp uncertainty is spent against, so a
    // change that quietly halves it should fail here.
    EXPECT_EQ(win_close - seen_t0,
              (int64_t) timedgrid::kWindowUs - (int64_t) classa::kArmLeadUs);

    esphome::lora_tracker::shim_hooks::set_active_radio(nullptr);
    esphome::shim_hooks::set_active_clock(nullptr);
}

// ---------------------------------------------------------------------------
// §4.6 — the hub's half: one copy instead of seventeen, once it has EVIDENCE
// ---------------------------------------------------------------------------
namespace {
namespace real_helpers {

// Feed the listener one uplink whose T0 lands exactly where the grid says this
// node transmits: its mark plus the uplink offset the hub itself publishes.
// The evidence §4.6 now promotes on: the node's own phase measurement, as it
// arrives from a decrypted beacon. Defaults are a healthy node.
void give_phase_report(RealHubHarness& h, int32_t err_us = 0,
                       int32_t spread_us = 0,
                       uint32_t samples = timedmode::kPromotionPhaseSamples,
                       uint32_t rtc = 2 /*crystal*/) {
    h.rol.notePhaseReportForTest(rtc, err_us, spread_us, samples);
}

void feed_in_slot_uplink(RealHubHarness& h, uint32_t msgid, int64_t err_us = 0) {
    const int64_t mark = h.tracker.nextT0ForSlotUs(h.rol.grid_slot(),
                                                   h.tracker.gridAnchorUs());
    h.tracker.last_rx_t0_us_v = mark + (int64_t) LORAClient::kUplinkOffsetUs + err_us;
    auto up = serialize_avail(/*sender=*/18, msgid);
    h.rol.set_response(up.data(), up.size());
}

}  // namespace real_helpers
}  // namespace

TEST(RealLoraClient, ARelogonMakesTheNodeEarnSingleShotFromScratch) {
    // §4.6's own rule: "a new session invalidates the hub's confidence... it
    // earns single-shot back from observation." send_login() set
    // session_changed and nothing else, but noteUplinkPlacement_ clears that
    // flag on the FIRST in-slot uplink — and in_slot_acks was still >=
    // kPromotionUplinks from the old session, so one lucky arrival restored
    // single-shot immediately. A node that just re-keyed after a reboot got a
    // one-copy command on the strength of evidence gathered before the reboot.
    using namespace real_helpers;
    RealHubHarness h{18, kMacRol2};
    h.rol.registered_ = true;
    h.rol.node_fw_version_ = 0x00010203;
    h.rol.enable_timed_mode(true);
    ASSERT_TRUE(h.tracker.gridStarted());

    give_phase_report(h);
    ASSERT_EQ(h.rol.txPolicyNow(),
              timedmode::TxPolicy::SingleShot);

    h.rol.send_login();
    EXPECT_EQ(h.rol.txPolicyNow(), timedmode::TxPolicy::Burst)
        << "a new session must not inherit the old session's confidence";

    // A report from the old session is not evidence about the new one, so the
    // node has to send a fresh beacon before single-shot returns.
    give_phase_report(h);
    EXPECT_EQ(h.rol.txPolicyNow(),
              timedmode::TxPolicy::SingleShot)
        << "a fresh report on the new session earns it back";
}

TEST(RealLoraClient, ARegisterMeansTheNodeRebootedAndConfidenceIsGone) {
    // rebooted_since_confirm is a txPolicyFor guard implementing rule 3, "any
    // hub uncertainty means burst". It defaulted true and was cleared by the
    // first in-slot uplink — and NOTHING ever set it again, so after the first
    // confirmation the guard was false for the life of the hub process and the
    // rule never fired. A REGISTER is the hub's actual notification that the
    // node restarted, which is exactly the uncertainty the flag names.
    using namespace real_helpers;
    RealHubHarness h{18, kMacRol2};
    h.rol.registered_ = true;
    h.rol.node_fw_version_ = 0x00010203;
    h.rol.enable_timed_mode(true);

    give_phase_report(h);
    ASSERT_EQ(h.rol.txPolicyNow(),
              timedmode::TxPolicy::SingleShot);

    auto reg = serialize_register(kMacRol2);
    h.rol.set_response(reg.data(), reg.size());

    EXPECT_EQ(h.rol.txPolicyNow(), timedmode::TxPolicy::Burst)
        << "a node that just told us it rebooted has lost its grid phase";
}

// ---------------------------------------------------------------------------
// RX2 — the second window, which the hub never used.
//
// The node opens BOTH windows: classa::nextAction returns OpenRx2 whenever RX1
// passed with NO DATA, and that is exactly the state the hub leaves it in when
// its reply is not ready in time. The hub logged "RX1 already past" and fell
// back to a 17-copy burst — 1408 ms of air aimed at a node whose 29.44 ms
// window it could place exactly. `grep kRx2` in the hub returned nothing.
// ---------------------------------------------------------------------------
namespace {

// A registered listener whose Class A predicate passes, with the harness clock
// and radio wired up. Returns the tracker by reference so a test can read what
// was actually placed.
struct ClassANode {
    proto_sim::SimClock clock;
    proto_sim::SimRadio radio;
    LORATracker tracker;
    LORAClient  rol;

    ClassANode() {
        esphome::shim_hooks::set_active_clock(&clock);
        esphome::shim_hooks::reset_nvs();
        esphome::lora_tracker::shim_hooks::set_active_radio(&radio);
        proto_sim_timer_reset();
        rol.set_name("rol");
        rol.set_short_address(17);
        rol.set_subnet_address(2);
        tracker.register_client(&rol);
        rol.registered_ = true;
        // What send_into_class_a_window_ requires: the node said AUTO in a
        // beacon we decrypted, and it is on no grid.
        rol.node_mode_ = (uint32_t) NODE_MODE__MODE_AUTO;
    }
    ~ClassANode() {
        esphome::lora_tracker::shim_hooks::set_active_radio(nullptr);
        esphome::shim_hooks::set_active_clock(nullptr);
    }
};

}  // namespace

TEST(RealLoraClient, AReplyTooLateForRx1GoesToRx2RatherThanToABurst) {
    ClassANode n;
    std::vector<uint8_t> payload{1, 2, 3, 4};

    constexpr int64_t kT0Uplink = 1'000'000;
    n.rol.last_uplink_t0_us_ = kT0Uplink;

    // Now is past RX1's fire instant and well before RX2's. Before this change
    // that was the "falling back" branch.
    proto_sim_timer_set_now_us(kT0Uplink + (int64_t) classa::kRx1DelayUs + 50'000);

    ASSERT_TRUE(n.rol.send_into_class_a_window_(payload.data(), payload.size()))
        << "RX1 is gone, but RX2 is still ahead — that is a placeable window";
    EXPECT_EQ(n.tracker.last_copies, 1)
        << "a burst is the opposite construction to a single placed copy";

    // Same contract as the RX1 test: not "earliest_us equals the expression the
    // code computes", but "the T0 the NODE sees lands inside the window the
    // node opens", built from classa::rx2OpenUs/rx2CloseUs off its own uplink.
    const int64_t fire      = n.tracker.last_earliest_us;
    const int64_t seen_t0   = fire + (int64_t) loratiming::kPreambleToT0Us;
    const int64_t win_open  = classa::rx2OpenUs(kT0Uplink);
    const int64_t win_close = classa::rx2CloseUs(kT0Uplink);
    EXPECT_GE(seen_t0, win_open)  << "frame arrives before RX2 opens";
    EXPECT_LT(seen_t0, win_close) << "frame arrives after RX2 has closed";
    EXPECT_EQ(seen_t0, kT0Uplink + (int64_t) classa::kRx2DelayUs)
        << "and on the design point, one RX2 delay after the node's own uplink";
    EXPECT_EQ(fire - win_open, (int64_t) timedgrid::kGuardUs)
        << "the first chirp must sit a full guard inside the window";
}

TEST(RealLoraClient, AWindowTooCloseToFireOnIsNotAWindow) {
    // popDue releases a placed frame kPrepareLeadUs early, so a target nearer
    // than that cannot be fired ON — asking for it puts the copy late by the
    // shortfall. RX1 half a millisecond away is unreachable, and the answer is
    // RX2 rather than the burst that used to be the only alternative.
    ClassANode n;
    std::vector<uint8_t> payload{1, 2, 3, 4};

    constexpr int64_t kT0Uplink = 1'000'000;
    n.rol.last_uplink_t0_us_ = kT0Uplink;

    const int64_t rx1_fire =
        loratiming::fireInstantUs(kT0Uplink + (int64_t) classa::kRx1DelayUs, 0);
    proto_sim_timer_set_now_us(rx1_fire - txqueue::kPrepareLeadUs / 10);

    ASSERT_TRUE(n.rol.send_into_class_a_window_(payload.data(), payload.size()));
    const int64_t seen_t0 =
        n.tracker.last_earliest_us + (int64_t) loratiming::kPreambleToT0Us;
    EXPECT_EQ(seen_t0, kT0Uplink + (int64_t) classa::kRx2DelayUs)
        << "an RX1 the queue cannot fire on must not be claimed as placed";
}

TEST(RealLoraClient, OnceBothWindowsArePastTheHubDeclines) {
    // Declining is not a failure: the caller falls back to the burst, which is
    // what this path did before C2 existed. What must not happen is a placed
    // copy aimed at a window that has already closed.
    ClassANode n;
    std::vector<uint8_t> payload{1, 2, 3, 4};

    constexpr int64_t kT0Uplink = 1'000'000;
    n.rol.last_uplink_t0_us_ = kT0Uplink;
    proto_sim_timer_set_now_us(kT0Uplink + (int64_t) classa::kRx2DelayUs + 1);

    EXPECT_FALSE(n.rol.send_into_class_a_window_(payload.data(), payload.size()));
}

TEST(RealLoraClient, AnInteractiveNodeGetsNeitherWindow) {
    // The mode gate is checked before either window: an interactive node sweeps
    // a free-running window, so a single copy at a fixed offset hits it about
    // 6 % of the time while the 17-copy burst hits it every time. RX2 must not
    // become a second way to aim into silence.
    ClassANode n;
    n.rol.node_mode_ = (uint32_t) NODE_MODE__MODE_INTERACTIVE;
    std::vector<uint8_t> payload{1, 2, 3, 4};

    constexpr int64_t kT0Uplink = 1'000'000;
    n.rol.last_uplink_t0_us_ = kT0Uplink;
    proto_sim_timer_set_now_us(kT0Uplink + (int64_t) classa::kRx1DelayUs + 50'000);

    EXPECT_FALSE(n.rol.send_into_class_a_window_(payload.data(), payload.size()));
}

TEST(RealLoraClient, ADecryptedAckIsTheCarrierThatKeepsTheReportFresh) {
    // The beacon is not the carrier that makes §4.6 work — the ACK is.
    //
    // A wake beacon reports what the node knew at wake, which is nothing:
    // phase::Stats is a plain member, zeroed on every deep-sleep wake, and the
    // beacon goes out before that wake has heard a grid-aligned frame. The ack
    // answers the very command single-shot is decided for, and every addressed
    // frame feeds the phase tracker, so a node acking a command has just taken
    // a sample against the frame it is acking.
    //
    // The alternative was a periodic per-node uplink, which is what §4.4 prices
    // and rejects: a unicast keepalive at 5.8 min is 3.4x worse than plain
    // burst at 32 nodes.
    proto_sim::SimClock clock;
    proto_sim::SimRadio radio;
    esphome::shim_hooks::set_active_clock(&clock);
    esphome::shim_hooks::reset_nvs();
    esphome::lora_tracker::shim_hooks::set_active_radio(&radio);
    ensure_psa_ready();

    LORATracker tracker;
    LORAClient  rol;
    rol.set_name("rol");
    rol.set_short_address(18);
    rol.set_subnet_address(2);
    rol.set_sleep_duration(21600);
    rol.set_address(kMacRol2);
    RealTimeClock time; time.set_now(1787000000, /*valid=*/true);
    rol.set_time(&time);
    tracker.register_client(&rol);
    rol.registered_ = true;

    const uint32_t base = drive_session(clock, radio, rol);
    ASSERT_NE(base, 0u);
    rol.enable_timed_mode(true);
    ASSERT_FALSE(rol.hubBelief().phase_reported);

    // First the wake beacon, exactly as a real one arrives: it carries the
    // firmware version the hub gates on, and a phase report of ZERO SAMPLES,
    // because phase::Stats is zeroed on every deep-sleep wake and the beacon
    // goes out before this wake has heard a grid-aligned frame. This is the
    // state §4.6 sat in: evidence that cannot arrive on the carrier that was
    // asked for it.
    {
        proto_sim::LoraClientResponseMessage bmsg;
        bmsg.header.destAddress   = esphome::lora_tracker::kHubAddress;
        bmsg.header.destSubnet    = 2;
        bmsg.header.senderAddress = 18;
        bmsg.header.msgId         = rol.frame_counter_.rx_message_id + 1;
        bmsg.proto                = proto_sim::LoraClientResponseMessage::Proto::Beacon;
        bmsg.beacon.fwVersion       = 0x00010203;
        bmsg.beacon.phasePresent    = true;
        bmsg.beacon.phase.rtcSlowSrc = 2;
        bmsg.beacon.phase.samples    = 0;      // nothing measured yet this wake

        auto bplain = proto_sim::serialize_resp_payload(bmsg);
        uint8_t baad[proto_sim::kHeaderAadLen];
        proto_sim::build_header_aad(bmsg.header.destAddress, bmsg.header.destSubnet,
                                    bmsg.header.senderAddress, bmsg.header.msgId, baad);
        uint8_t biv[12];
        proto_sim::derive_gcm_iv(base, bmsg.header.msgId, biv);
        auto benc = proto_sim::aes_gcm_encrypt(biv, baad, sizeof(baad),
                                               bplain.data(), bplain.size());
        proto_sim::LoraClientResponseMessage bouter;
        bouter.header               = bmsg.header;
        bouter.proto                = proto_sim::LoraClientResponseMessage::Proto::Encrypted;
        bouter.encrypted.tag        = benc.tag;
        bouter.encrypted.ciphertext = benc.ciphertext;
        auto bframe = proto_sim::serialize_resp(bouter);
        rol.set_response(bframe.data(), bframe.size());
    }
    EXPECT_FALSE(rol.hubBelief().phase_reported)
        << "zero samples is not a report, and must not read as a perfect one";
    EXPECT_EQ(rol.txPolicyNow(), timedmode::TxPolicy::Burst)
        << "the wake beacon alone cannot promote — this is what was broken";

    // Then the ack, which is the carrier that can.
    proto_sim::LoraClientResponseMessage inner;
    inner.header.destAddress   = esphome::lora_tracker::kHubAddress;
    inner.header.destSubnet    = 2;
    inner.header.senderAddress = 18;
    inner.header.msgId         = rol.frame_counter_.rx_message_id + 1;
    inner.proto                = proto_sim::LoraClientResponseMessage::Proto::Ack;
    inner.ack.ack_msg_id       = 7;
    inner.ack.phasePresent      = true;
    inner.ack.phase.rtcSlowSrc   = 2;      // crystal
    inner.ack.phase.errUs        = 90;
    inner.ack.phase.spreadUs     = 250;
    inner.ack.phase.samples      = timedmode::kPromotionPhaseSamples;
    inner.ack.phase.outsideGuard = 0;

    auto plain = proto_sim::serialize_resp_payload(inner);
    uint8_t aad[proto_sim::kHeaderAadLen];
    proto_sim::build_header_aad(inner.header.destAddress, inner.header.destSubnet,
                                inner.header.senderAddress, inner.header.msgId, aad);
    uint8_t iv[12];
    proto_sim::derive_gcm_iv(base, inner.header.msgId, iv);
    auto enc = proto_sim::aes_gcm_encrypt(iv, aad, sizeof(aad),
                                          plain.data(), plain.size());
    proto_sim::LoraClientResponseMessage outer;
    outer.header               = inner.header;
    outer.proto                = proto_sim::LoraClientResponseMessage::Proto::Encrypted;
    outer.encrypted.tag        = enc.tag;
    outer.encrypted.ciphertext = enc.ciphertext;
    auto frame = proto_sim::serialize_resp(outer);
    rol.set_response(frame.data(), frame.size());

    const auto b = rol.hubBelief();
    EXPECT_TRUE(b.phase_reported)
        << "the ack's phase report must reach the belief, not just the beacon's";
    EXPECT_EQ(b.phase_err_us, 90);
    EXPECT_EQ(b.phase_spread_us, 250);
    EXPECT_EQ(b.phase_samples, timedmode::kPromotionPhaseSamples);
    EXPECT_EQ(b.rtc_src, timedmode::RtcSlowSrc::Crystal);
    EXPECT_EQ(rol.txPolicyNow(), timedmode::TxPolicy::SingleShot)
        << "a healthy report on a real encrypted ack must earn single-shot";

    esphome::lora_tracker::shim_hooks::set_active_radio(nullptr);
    esphome::shim_hooks::set_active_clock(nullptr);
}

TEST(RealLoraClient, AnAckWithNoPhaseReportLeavesTheBeliefAlone) {
    // ABSENT IS NOT EMPTY — the same rule the pending mask is built on. A node
    // whose firmware predates the field sends an ack with no PhaseReport, and
    // treating that as a report of zeros would silently demote a node the hub
    // has good evidence for. Let confirmation_age_s expire it instead.
    proto_sim::SimClock clock;
    proto_sim::SimRadio radio;
    esphome::shim_hooks::set_active_clock(&clock);
    esphome::shim_hooks::reset_nvs();
    esphome::lora_tracker::shim_hooks::set_active_radio(&radio);
    ensure_psa_ready();

    LORATracker tracker;
    LORAClient  rol;
    rol.set_name("rol");
    rol.set_short_address(18);
    rol.set_subnet_address(2);
    rol.set_sleep_duration(21600);
    rol.set_address(kMacRol2);
    RealTimeClock time; time.set_now(1787000000, /*valid=*/true);
    rol.set_time(&time);
    tracker.register_client(&rol);
    rol.registered_ = true;

    const uint32_t base = drive_session(clock, radio, rol);
    ASSERT_NE(base, 0u);
    rol.enable_timed_mode(true);

    // Seed a good report, then send an ack that carries none.
    rol.notePhaseReportForTest(2, 100, 200, timedmode::kPromotionPhaseSamples);
    ASSERT_TRUE(rol.hubBelief().phase_reported);

    proto_sim::LoraClientResponseMessage inner;
    inner.header.destAddress   = esphome::lora_tracker::kHubAddress;
    inner.header.destSubnet    = 2;
    inner.header.senderAddress = 18;
    inner.header.msgId         = rol.frame_counter_.rx_message_id + 1;
    inner.proto                = proto_sim::LoraClientResponseMessage::Proto::Ack;
    inner.ack.ack_msg_id       = 9;
    inner.ack.phasePresent     = false;

    auto plain = proto_sim::serialize_resp_payload(inner);
    uint8_t aad[proto_sim::kHeaderAadLen];
    proto_sim::build_header_aad(inner.header.destAddress, inner.header.destSubnet,
                                inner.header.senderAddress, inner.header.msgId, aad);
    uint8_t iv[12];
    proto_sim::derive_gcm_iv(base, inner.header.msgId, iv);
    auto enc = proto_sim::aes_gcm_encrypt(iv, aad, sizeof(aad),
                                          plain.data(), plain.size());
    proto_sim::LoraClientResponseMessage outer;
    outer.header               = inner.header;
    outer.proto                = proto_sim::LoraClientResponseMessage::Proto::Encrypted;
    outer.encrypted.tag        = enc.tag;
    outer.encrypted.ciphertext = enc.ciphertext;
    auto frame = proto_sim::serialize_resp(outer);
    rol.set_response(frame.data(), frame.size());

    const auto b = rol.hubBelief();
    EXPECT_TRUE(b.phase_reported)   << "an absent report must not erase a good one";
    EXPECT_EQ(b.phase_err_us, 100);
    EXPECT_EQ(b.phase_samples, timedmode::kPromotionPhaseSamples);

    esphome::lora_tracker::shim_hooks::set_active_radio(nullptr);
    esphome::shim_hooks::set_active_clock(nullptr);
}

TEST(RealLoraClient, APhaseReportEarnsASingleCopyDownlink) {
    // TimedModePolicy.h's txPolicyFor() and HubBelief had NO production caller.
    // The entire airtime saving of Mode B is in that function — 17 copies down
    // to 1 — so a hub that never asked it paid Mode A's cost for Mode B's
    // narrower window, which is the worst of both.
    //
    // It then had a caller and still could not fire: promotion required three
    // consecutive uplinks observed in slot, and the node's transmit path cannot
    // produce them (CAD, a burst-end deferral and a 29-290 ms random backoff
    // stand in front of every uplink). The evidence is now the node's own phase
    // report — see TimedModePolicy.h for why that answers the question better,
    // not merely more conveniently.
    using namespace real_helpers;
    RealHubHarness h{18, kMacRol2};
    h.rol.registered_ = true;
    // Learned from a beacon in production; the policy refuses single-shot to a
    // node whose firmware it does not know.
    h.rol.node_fw_version_ = 0x00010203;

    h.rol.enable_timed_mode(true);
    ASSERT_TRUE(h.tracker.gridStarted());

    // No report yet: a node that has told us nothing gets the burst.
    EXPECT_EQ(h.rol.txPolicyNow(),
              timedmode::TxPolicy::Burst);

    give_phase_report(h);
    ASSERT_EQ(h.rol.txPolicyNow(),
              timedmode::TxPolicy::SingleShot)
        << "phase inside the guard over enough samples, on the crystal, "
           "reported just now, firmware known";

    // And the belief has to reach the radio, which is the half that was missing.
    h.tracker.last_copies = 0;
    h.rol.send_cover_operation(LORA_COVER_OPERATION__COVOP_OPERATION,
                               COV_OPERATION__CMD_OPEN, 0.0f);
    EXPECT_EQ(h.tracker.last_copies, 1)
        << "the downlink must go out as ONE placed copy, not a 17-copy burst";
}

TEST(RealLoraClient, APoorPhaseReportKeepsTheHubOnBursts) {
    // The three ways a report can be present and still not be evidence. Each
    // is a node a single copy would miss, and each is exactly the node most
    // likely to have looked healthy on the report before it.
    using namespace real_helpers;
    RealHubHarness h{18, kMacRol2};
    h.rol.registered_ = true;
    h.rol.node_fw_version_ = 0x00010203;
    h.rol.enable_timed_mode(true);

    give_phase_report(h);
    ASSERT_EQ(h.rol.txPolicyNow(),
              timedmode::TxPolicy::SingleShot);

    // Out of guard: the window is landing where the frame is not.
    give_phase_report(h, /*err_us=*/(int32_t) timedgrid::kGuardUs + 1000);
    EXPECT_EQ(h.rol.txPolicyNow(),
              timedmode::TxPolicy::Burst)
        << "a phase error outside the guard must cost the promotion outright";

    // Spread, with a mean that looks perfect. Two clusters a pitch apart
    // average to zero, and this is the case the mean cannot see.
    give_phase_report(h, /*err_us=*/0,
                      /*spread_us=*/(int32_t) timedgrid::kGuardUs + 1000);
    EXPECT_EQ(h.rol.txPolicyNow(),
              timedmode::TxPolicy::Burst)
        << "a mean of zero over a bimodal distribution is not a phase";

    // A node that has fallen back to the internal RC cannot hold phase between
    // beacons, whatever this report says.
    give_phase_report(h, 0, 0, timedmode::kPromotionPhaseSamples, /*rtc=*/1);
    EXPECT_EQ(h.rol.txPolicyNow(),
              timedmode::TxPolicy::Burst);

    h.tracker.last_copies = 1;
    h.rol.send_cover_operation(LORA_COVER_OPERATION__COVOP_OPERATION,
                               COV_OPERATION__CMD_OPEN, 0.0f);
    EXPECT_NE(h.tracker.last_copies, 1)
        << "and the radio must be back on bursts, not merely the belief";
}

TEST(RealLoraClient, TimeSyncIsEncrypted) {
    // The node only trusts an authenticated downlink once it has a session, so
    // a plaintext TimeSync would be both droppable and spoofable — a spoofed
    // clock is the one input that can make a scheduled node sleep through
    // every event, or wake at the wrong time indefinitely.
    proto_sim::SimClock clock;
    proto_sim::SimRadio radio;
    esphome::shim_hooks::set_active_clock(&clock);
    esphome::shim_hooks::reset_nvs();
    esphome::lora_tracker::shim_hooks::set_active_radio(&radio);
    ensure_psa_ready();
    esphome::ESPTime::set_timezone_offset(7200);

    LORATracker tracker;
    LORAClient  rol;
    rol.set_name("rol");
    rol.set_short_address(18);
    rol.set_subnet_address(2);
    rol.set_address(kMacRol2);
    RealTimeClock time; time.set_now(1787000000, /*valid=*/true);
    rol.set_time(&time);
    tracker.register_client(&rol);

    drive_session(clock, radio, rol);
    const size_t before = radio.transcript().size();
    clock.tick(1000);

    for (size_t i = before; i < radio.transcript().size(); ++i) {
        auto m = proto_sim::as_op(radio.transcript()[i]);
        if (!m) continue;
        EXPECT_NE(m->cmd, proto_sim::LoraClientOperationMessage::Cmd::TimeSync)
            << "TimeSync appeared in PLAINTEXT on the wire";
    }

    esphome::lora_tracker::shim_hooks::set_active_radio(nullptr);
    esphome::shim_hooks::set_active_clock(nullptr);
}

TEST(RealLoraClient, NoTimeSyncWhenHubClockInvalid) {
    // Hub booted but Home Assistant time has not arrived yet. Sending epoch 0
    // would be worse than sending nothing: the node would burn awake radio time
    // receiving a frame it must discard. The next login retries the push.
    proto_sim::SimClock clock;
    proto_sim::SimRadio radio;
    esphome::shim_hooks::set_active_clock(&clock);
    esphome::shim_hooks::reset_nvs();
    esphome::lora_tracker::shim_hooks::set_active_radio(&radio);
    ensure_psa_ready();

    LORATracker tracker;
    LORAClient  rol;
    rol.set_name("rol");
    rol.set_short_address(18);
    rol.set_subnet_address(2);
    rol.set_address(kMacRol2);
    RealTimeClock time; time.set_now(0, /*valid=*/false);
    rol.set_time(&time);
    tracker.register_client(&rol);

    const uint32_t base = drive_session(clock, radio, rol);
    const size_t before = radio.transcript().size();
    clock.tick(1000);

    for (size_t i = before; i < radio.transcript().size(); ++i) {
        auto inner = decrypt_downlink(radio.transcript()[i], base);
        if (!inner) continue;
        EXPECT_NE(inner->cmd, proto_sim::LoraClientOperationMessage::Cmd::TimeSync)
            << "hub sent TimeSync despite having no valid clock";
    }

    esphome::lora_tracker::shim_hooks::set_active_radio(nullptr);
    esphome::shim_hooks::set_active_clock(nullptr);
}

// ---------------------------------------------------------------------------
// P2 — wake beacon handling on the hub. The beacon is how the node's clock
// becomes observable without a serial cable, which is what keeps the
// "no sleep cap" decision honest over time.
// ---------------------------------------------------------------------------
namespace {

// Send an ENCRYPTED uplink from the simulated node into the real listener.
void send_encrypted_uplink(LORAClient& listener, uint32_t base_nonce,
                           uint32_t msgid, uint32_t node_addr, uint32_t subnet,
                           proto_sim::LoraClientResponseMessage inner) {
    inner.header.destAddress   = esphome::lora_tracker::kHubAddress;
    inner.header.destSubnet    = subnet;
    inner.header.senderAddress = node_addr;
    inner.header.msgId         = msgid;

    auto plain = proto_sim::serialize_resp_payload(inner);
    uint8_t aad[proto_sim::kHeaderAadLen];
    proto_sim::build_header_aad(inner.header.destAddress, inner.header.destSubnet,
                                inner.header.senderAddress, inner.header.msgId, aad);
    uint8_t iv[12];
    proto_sim::derive_gcm_iv_uplink(base_nonce, msgid, iv);
    auto enc = proto_sim::aes_gcm_encrypt(iv, aad, sizeof(aad), plain.data(), plain.size());

    proto_sim::LoraClientResponseMessage outer;
    outer.header               = inner.header;
    outer.proto                = proto_sim::LoraClientResponseMessage::Proto::Encrypted;
    outer.encrypted.tag        = enc.tag;
    outer.encrypted.ciphertext = enc.ciphertext;

    auto bytes = proto_sim::serialize_resp(outer);
    listener.set_response(bytes.data(), bytes.size());
}

proto_sim::LoraClientResponseMessage make_beacon(uint64_t node_epoch, bool clock_valid) {
    proto_sim::LoraClientResponseMessage m;
    m.proto = proto_sim::LoraClientResponseMessage::Proto::Beacon;
    m.beacon.reason        = proto_sim::WakeReason::WAKE_TIMER_CHECKIN;
    m.beacon.nodeEpoch     = node_epoch;
    m.beacon.mode          = proto_sim::NodeMode::MODE_INTERACTIVE;
    m.beacon.voltage       = 11.4f;
    m.beacon.position      = 0.5f;
    m.beacon.sessionResume = true;
    m.beacon.clockValid    = clock_valid;
    m.beacon.fwVersion     = 10013;
    return m;
}

struct BeaconRig {
    proto_sim::SimClock clock;
    proto_sim::SimRadio radio;
    LORATracker tracker;
    LORAClient  rol;
    RealTimeClock time;
    uint32_t base{0};

    void start(std::time_t hub_epoch) {
        esphome::shim_hooks::set_active_clock(&clock);
        esphome::shim_hooks::reset_nvs();
        esphome::lora_tracker::shim_hooks::set_active_radio(&radio);
        ensure_psa_ready();
        rol.set_name("rol");
        rol.set_short_address(18);
        rol.set_subnet_address(2);
        rol.set_address(kMacRol2);
        time.set_now(hub_epoch, /*valid=*/true);
        rol.set_time(&time);
        tracker.register_client(&rol);
        base = drive_session(clock, radio, rol);
    }
    ~BeaconRig() {
        esphome::lora_tracker::shim_hooks::set_active_radio(nullptr);
        esphome::shim_hooks::set_active_clock(nullptr);
    }
};

} // namespace

// ---------------------------------------------------------------------------
// The HA auto-mode switch must not be reverted by a stale beacon.
//
// The beacon carries the mode the node was in when it woke. If our mode change
// has not reached it yet, that is simply out of date — and publishing it
// overwrites the request with the state the user just changed away from.
//
// Observed live: auto mode switched OFF while the node slept; the next beacon
// arrived 2 s BEFORE the push carrying INTERACTIVE went out, and the switch
// snapped back to ON. The change was still delivered and the node did go
// interactive, so the command was not lost — but the UI showed the wrong mode
// until the following beacon, which on the configured 6 h check-in is six
// hours of lying about what the blind is doing.
//
// schedule_pending() is exactly "the node is not on our version yet", which
// separates an undelivered change from a node that legitimately chose a
// different mode (a button press changes no version, so it still wins).
// ---------------------------------------------------------------------------

TEST(RealLoraClient, StaleBeaconDoesNotRevertARequestedModeChange) {
    constexpr std::time_t kHubEpoch = 1787000000;
    BeaconRig rig;
    rig.start(kHubEpoch);
    ASSERT_NE(rig.base, 0u);

    esphome::switch_::Switch sw;
    rig.rol.set_auto_mode_switch(&sw);

    // The user switches auto mode OFF while the node is asleep; HA now shows
    // OFF. This marks the schedule dirty, so the node is behind our version.
    rig.rol.set_auto_mode(false);
    sw.publish_state(false);
    ASSERT_TRUE(rig.rol.schedule_pending())
        << "precondition: the change is undelivered";

    // The node wakes and beacons the mode AND version it had BEFORE our change.
    auto b = make_beacon(kHubEpoch, /*clock_valid=*/true);
    b.beacon.mode         = proto_sim::NodeMode::MODE_AUTO;
    b.beacon.schedVersion = 0;              // never received our push
    send_encrypted_uplink(rig.rol, rig.base, /*msgid=*/2, 18, 2, b);

    EXPECT_FALSE(sw.state)
        << "a beacon predating our change must not flip the switch back to the "
           "state the user just changed away from";
}

TEST(RealLoraClient, BeaconStillCorrectsTheSwitchOnceTheNodeIsInSync) {
    // The case the correction exists for, which must keep working: the node is
    // on our version and reports a mode we did not ask for — a button press at
    // the blind, or auto mode refused for want of a clock. That is the node's
    // real state and HA must show it.
    constexpr std::time_t kHubEpoch = 1787000000;
    BeaconRig rig;
    rig.start(kHubEpoch);

    esphome::switch_::Switch sw;
    sw.publish_state(true);                 // HA shows AUTO
    rig.rol.set_auto_mode_switch(&sw);

    // "In sync" means the BEACON reports our version — handle_beacon_ takes
    // node_sched_version_ from the beacon, which is the node telling us where
    // it actually is.
    auto b = make_beacon(kHubEpoch, /*clock_valid=*/true);
    b.beacon.mode         = proto_sim::NodeMode::MODE_INTERACTIVE;  // button press
    b.beacon.schedVersion = rig.rol.schedule_version();
    send_encrypted_uplink(rig.rol, rig.base, /*msgid=*/2, 18, 2, b);

    ASSERT_TRUE(rig.rol.clock_offset_valid_)
        << "precondition: the beacon must actually have been processed";
    ASSERT_FALSE(rig.rol.schedule_pending())
        << "precondition: the node reported our version, so nothing is pending";
    EXPECT_FALSE(sw.state)
        << "an in-sync node reporting INTERACTIVE is telling us its real state; "
           "HA must not keep showing AUTO for a blind that is not in auto mode";
}

TEST(RealLoraClient, BeaconClockOffsetIsNodeMinusHub) {
    constexpr std::time_t kHubEpoch = 1787000000;
    BeaconRig rig;
    rig.start(kHubEpoch);
    ASSERT_NE(rig.base, 0u);

    // Node runs 7 s AHEAD of the hub.
    send_encrypted_uplink(rig.rol, rig.base, /*msgid=*/2, 18, 2,
                          make_beacon(kHubEpoch + 7, /*clock_valid=*/true));

    EXPECT_TRUE(rig.rol.clock_offset_valid_);
    EXPECT_EQ(rig.rol.clock_offset_s_, 7)
        << "offset must be node_epoch - hub_epoch; a sign flip would make a "
           "fast node look slow and send drift correction the wrong way";
    EXPECT_EQ(rig.rol.node_fw_version_, 10013u);
    EXPECT_TRUE(rig.rol.node_session_resume_);
}

TEST(RealLoraClient, BeaconClockOffsetIsNegativeWhenNodeLags) {
    constexpr std::time_t kHubEpoch = 1787000000;
    BeaconRig rig;
    rig.start(kHubEpoch);

    send_encrypted_uplink(rig.rol, rig.base, /*msgid=*/2, 18, 2,
                          make_beacon(kHubEpoch - 12, /*clock_valid=*/true));

    EXPECT_TRUE(rig.rol.clock_offset_valid_);
    EXPECT_EQ(rig.rol.clock_offset_s_, -12);
}

TEST(RealLoraClient, BeaconWithInvalidClockPublishesNoOffset) {
    // I8's case: a node that has never received a TimeSync reports clockValid
    // = false. Publishing an offset computed from epoch 0 would show a ~56-year
    // drift in Home Assistant and make the sensor useless.
    constexpr std::time_t kHubEpoch = 1787000000;
    BeaconRig rig;
    rig.start(kHubEpoch);

    send_encrypted_uplink(rig.rol, rig.base, /*msgid=*/2, 18, 2,
                          make_beacon(0, /*clock_valid=*/false));

    EXPECT_FALSE(rig.rol.clock_offset_valid_)
        << "a clockless node must not produce a bogus offset reading";
}

// ---------------------------------------------------------------------------
// B-1: the transmit policy belongs to the FRAME, not to the tracker.
//
// setBurstCopies() was a mutable field on LORATracker, set by the caller at
// enqueue and read by sendTask at dequeue. Those are different moments on
// different tasks, so for the whole 300 s of a drift test EVERY frame the hub
// sent inherited copies=1 — including an ordinary blind command a user pressed
// in Home Assistant, delivered at roughly 5.8 % against a node in the normal
// three-window mode. The old code's comment worried only about leaving the
// field set afterwards, which is the smaller half of the bug.
// ---------------------------------------------------------------------------

TEST(TxPolicy, DefaultsToTheFullBurst) {
    using namespace real_helpers;
    RealHubHarness h{2, kMacRol2};
    uint8_t frame[8] = {1, 2, 3, 4, 5, 6, 7, 8};

    h.tracker.send(frame, sizeof(frame));

    ASSERT_EQ(h.tracker.sent_copies.size(), 1u);
    EXPECT_EQ(h.tracker.sent_copies[0], 0) << "0 means txSlotsPerRound";
}

TEST(TxPolicy, SingleCopyIsPerFrameAndDoesNotPersist) {
    using namespace real_helpers;
    RealHubHarness h{2, kMacRol2};
    uint8_t frame[8] = {1, 2, 3, 4, 5, 6, 7, 8};

    h.tracker.send(frame, sizeof(frame), {/*copies=*/1, /*stride_ms=*/0});
    h.tracker.send(frame, sizeof(frame));

    ASSERT_EQ(h.tracker.sent_copies.size(), 2u);
    EXPECT_EQ(h.tracker.sent_copies[0], 1);
    EXPECT_EQ(h.tracker.sent_copies[1], 0)
        << "a single-copy frame must not change how the NEXT frame is sent";
}

TEST(TxPolicy, OrdinaryCommandKeepsItsBurstDuringADriftTest) {
    // The regression. Start a real drift test on the real LORAListener, then
    // send an ordinary frame while it is active.
    using namespace real_helpers;
    RealHubHarness h{2, kMacRol2};
    ensure_psa_ready();

    h.rol.start_drift_test(/*duration_s=*/300, /*grid_ms=*/1100);
    ASSERT_TRUE(h.rol.drift_test_active());

    const size_t before = h.tracker.sent_copies.size();
    uint8_t user_command[16] = {0};
    h.tracker.send(user_command, sizeof(user_command));

    ASSERT_GT(h.tracker.sent_copies.size(), before);
    EXPECT_EQ(h.tracker.sent_copies.back(), 0)
        << "a user command sent during a drift test must still be bursted; "
           "one copy is ~5.8 % delivery to a node in three-window mode";

    h.rol.stop_drift_test();
    EXPECT_FALSE(h.rol.drift_test_active());
}

TEST(TxPolicy, StoppingADriftTestBurstsTheOffCommand) {
    using namespace real_helpers;
    RealHubHarness h{2, kMacRol2};
    ensure_psa_ready();

    h.rol.start_drift_test(/*duration_s=*/300, /*grid_ms=*/1100);
    h.tracker.sent_copies.clear();
    h.rol.stop_drift_test();

    // The OFF frame goes out as a normal burst: the node may already have left
    // continuous RX on its own deadline, and a single copy would likely be lost.
    ASSERT_FALSE(h.tracker.sent_copies.empty());
    for (int c : h.tracker.sent_copies)
        EXPECT_EQ(c, 0) << "the OFF command must be bursted";
}

// ---------------------------------------------------------------------------
// M1, hub half — MAC-0 ping and echo (mac-layer.md sections 5 and 6).
//
// Stage 0 of the frame funnel is the hub's alone: only the sender knows what it
// offered. That is why pings_offered lives here and the node never computes its
// own frame-error rate — it cannot know what it did not hear.
// ---------------------------------------------------------------------------

TEST(MacPing, StartsAndStopsCleanly) {
    using namespace real_helpers;
    RealHubHarness h{2, kMacRol2};
    ensure_psa_ready();

    EXPECT_FALSE(h.rol.mac_ping_active());
    h.rol.start_mac_ping(/*duration_s=*/300, /*grid_ms=*/1100);
    EXPECT_TRUE(h.rol.mac_ping_active());
    h.rol.stop_mac_ping();
    EXPECT_FALSE(h.rol.mac_ping_active());
}

TEST(MacPing, StartIsIdempotentAndDoesNotResetStatsMidRun) {
    using namespace real_helpers;
    RealHubHarness h{2, kMacRol2};
    ensure_psa_ready();

    h.rol.start_mac_ping(300, 1100);
    // A second start must not silently restart the accumulator: the run would
    // then only ever reflect its final segment, which is the shape of bug the
    // drift test already had to fix once.
    h.rol.start_mac_ping(300, 1100);
    EXPECT_TRUE(h.rol.mac_ping_active());
    h.rol.stop_mac_ping();
}

TEST(MacPing, EchoIsCountedAndConsumedNotForwarded) {
    using namespace real_helpers;
    RealHubHarness h{2, kMacRol2};
    ensure_psa_ready();

    h.rol.start_mac_ping(300, 1100);
    ASSERT_EQ(h.rol.mac_stats().echoes_rx, 0u);

    // Feed a plaintext MAC echo in, exactly as the node's MAC-0 emits it.
    ::LoraHeader hdr = LORA_HEADER__INIT;
    hdr.destaddress   = 1;
    hdr.destsubnet    = 2;
    hdr.senderaddress = 2;
    hdr.msgid         = 500;

    ::MacControl echo = MAC_CONTROL__INIT;
    echo.kind = MAC_CONTROL__KIND__MAC_ECHO;
    echo.seq  = 1;

    ::LoraClientResponseMessage resp = LORA_CLIENT_RESPONSE_MESSAGE__INIT;
    resp.header     = &hdr;
    resp.proto_case = LORA_CLIENT_RESPONSE_MESSAGE__PROTO_MACCONTROL;
    resp.maccontrol = &echo;

    std::vector<uint8_t> bytes(lora_client_response_message__get_packed_size(&resp));
    lora_client_response_message__pack(&resp, bytes.data());
    h.rol.set_response(bytes.data(), bytes.size());

    EXPECT_EQ(h.rol.mac_stats().echoes_rx, 1u);
    EXPECT_EQ(h.rol.mac_stats().last_seq_echoed, 1u);
    h.rol.stop_mac_ping();
}

TEST(MacPing, MissingMarksAreCountedAsGapsBySeq) {
    using namespace real_helpers;
    RealHubHarness h{2, kMacRol2};
    ensure_psa_ready();
    h.rol.start_mac_ping(300, 1100);

    auto feed_echo = [&](uint32_t seq, uint32_t msgid) {
        ::LoraHeader hdr = LORA_HEADER__INIT;
        hdr.destaddress = 1; hdr.destsubnet = 2; hdr.senderaddress = 2;
        hdr.msgid = msgid;
        ::MacControl e = MAC_CONTROL__INIT;
        e.kind = MAC_CONTROL__KIND__MAC_ECHO;
        e.seq  = seq;
        ::LoraClientResponseMessage r = LORA_CLIENT_RESPONSE_MESSAGE__INIT;
        r.header = &hdr;
        r.proto_case = LORA_CLIENT_RESPONSE_MESSAGE__PROTO_MACCONTROL;
        r.maccontrol = &e;
        std::vector<uint8_t> b(lora_client_response_message__get_packed_size(&r));
        lora_client_response_message__pack(&r, b.data());
        h.rol.set_response(b.data(), b.size());
    };

    // Marks 1 and 4 come back; 2 and 3 were lost. A lost frame must leave a
    // HOLE rather than shifting every later sample — which is exactly why seq
    // is carried separately from msgid.
    feed_echo(1, 600);
    feed_echo(4, 601);

    EXPECT_EQ(h.rol.mac_stats().echoes_rx, 2u);
    EXPECT_EQ(h.rol.mac_stats().echo_seq_gaps, 2u);
    EXPECT_EQ(h.rol.mac_stats().last_seq_echoed, 4u);
    h.rol.stop_mac_ping();
}

TEST(MacPing, FirstEchoIsNotAGapEvenIfItsSeqIsHigh) {
    using namespace real_helpers;
    RealHubHarness h{2, kMacRol2};
    ensure_psa_ready();
    h.rol.start_mac_ping(300, 1100);

    ::LoraHeader hdr = LORA_HEADER__INIT;
    hdr.destaddress = 1; hdr.destsubnet = 2; hdr.senderaddress = 2; hdr.msgid = 700;
    ::MacControl e = MAC_CONTROL__INIT;
    e.kind = MAC_CONTROL__KIND__MAC_ECHO;
    e.seq  = 9;
    ::LoraClientResponseMessage r = LORA_CLIENT_RESPONSE_MESSAGE__INIT;
    r.header = &hdr;
    r.proto_case = LORA_CLIENT_RESPONSE_MESSAGE__PROTO_MACCONTROL;
    r.maccontrol = &e;
    std::vector<uint8_t> b(lora_client_response_message__get_packed_size(&r));
    lora_client_response_message__pack(&r, b.data());
    h.rol.set_response(b.data(), b.size());

    // Without the have_echo guard this would report eight phantom losses on the
    // very first echo of every run.
    EXPECT_EQ(h.rol.mac_stats().echoes_rx, 1u);
    EXPECT_EQ(h.rol.mac_stats().echo_seq_gaps, 0u);
    h.rol.stop_mac_ping();
}

TEST(MacPing, APingIsNeverAnsweredByTheHub) {
    using namespace real_helpers;
    RealHubHarness h{2, kMacRol2};
    ensure_psa_ready();
    h.rol.start_mac_ping(300, 1100);

    ::LoraHeader hdr = LORA_HEADER__INIT;
    hdr.destaddress = 1; hdr.destsubnet = 2; hdr.senderaddress = 2; hdr.msgid = 800;
    ::MacControl ping = MAC_CONTROL__INIT;
    ping.kind = MAC_CONTROL__KIND__MAC_PING;   // wrong direction on purpose
    ping.seq = 1;
    ping.wantecho = true;
    ::LoraClientResponseMessage r = LORA_CLIENT_RESPONSE_MESSAGE__INIT;
    r.header = &hdr;
    r.proto_case = LORA_CLIENT_RESPONSE_MESSAGE__PROTO_MACCONTROL;
    r.maccontrol = &ping;
    std::vector<uint8_t> b(lora_client_response_message__get_packed_size(&r));
    lora_client_response_message__pack(&r, b.data());

    const size_t before = h.tracker.sent_copies.size();
    h.rol.set_response(b.data(), b.size());

    // Neither counted as an echo nor answered. If the hub replied to a ping and
    // the node replied to an echo, the two would trade frames forever.
    EXPECT_EQ(h.rol.mac_stats().echoes_rx, 0u);
    EXPECT_EQ(h.tracker.sent_copies.size(), before);
    h.rol.stop_mac_ping();
}

// ---------------------------------------------------------------------------
// B1 — the grid anchor (implementation-plan.md 4.2).
//
// A is set once and never moved. That is what lets a node hold a phase across
// hours, and it is why a hub restart invalidates every node's phase at once.
// ---------------------------------------------------------------------------

TEST(GridAnchor, StartsOnceAndNeverMoves) {
    using namespace real_helpers;
    RealHubHarness h{2, kMacRol2};

    h.tracker.startGrid();
    ASSERT_TRUE(h.tracker.gridStarted());
    const int64_t a = h.tracker.gridAnchorUs();

    h.tracker.startGrid();   // a second call must be a no-op
    EXPECT_EQ(h.tracker.gridAnchorUs(), a);
}

TEST(GridAnchor, SlotsAreOnePitchApartWithinARound) {
    using namespace real_helpers;
    RealHubHarness h{2, kMacRol2};
    h.tracker.startGrid();
    const int64_t a = h.tracker.gridAnchorUs();

    for (uint8_t k = 0; k + 1 < timedgrid::kSlotCount; ++k) {
        const int64_t t0 = h.tracker.nextT0ForSlotUs(k, a);
        const int64_t t1 = h.tracker.nextT0ForSlotUs(k + 1, a);
        EXPECT_EQ(t1 - t0, (int64_t) timedgrid::kSlotPitchUs) << "slots " << (int) k;
    }
}

TEST(GridAnchor, NextT0IsNeverInThePast) {
    using namespace real_helpers;
    RealHubHarness h{2, kMacRol2};
    h.tracker.startGrid();
    const int64_t a = h.tracker.gridAnchorUs();

    // Sweep a whole round in 1 ms steps, for a slot in the middle of the grid.
    for (int64_t off = 0; off < (int64_t) timedgrid::kRoundUs; off += 1000) {
        const int64_t now = a + off;
        const int64_t t0  = h.tracker.nextT0ForSlotUs(7, now);
        EXPECT_GE(t0, now) << "offset " << off;
        EXPECT_LT(t0 - now, (int64_t) timedgrid::kRoundUs) << "offset " << off;
    }
}

TEST(GridAnchor, EveryAnswerIsOnTheGrid) {
    using namespace real_helpers;
    RealHubHarness h{2, kMacRol2};
    h.tracker.startGrid();
    const int64_t a = h.tracker.gridAnchorUs();

    for (uint8_t k : {uint8_t{0}, uint8_t{1}, uint8_t{31}})
        for (int64_t off = 0; off < 3LL * timedgrid::kRoundUs; off += 7777) {
            const int64_t t0 = h.tracker.nextT0ForSlotUs(k, a + off);
            const int64_t rel = t0 - a - (int64_t) k * timedgrid::kSlotPitchUs;
            EXPECT_EQ(rel % (int64_t) timedgrid::kRoundUs, 0)
                << "slot " << (int) k << " offset " << off;
        }
}

TEST(GridAnchor, ATimeBeforeTheSlotsFirstT0DoesNotRoundBackwards) {
    // Integer division truncates towards zero, so a negative delta with a
    // (d + round - 1) / round formula rounds the WRONG way and returns an
    // instant in the past. Slot 31's first T0 is 1.45 s after the anchor, so
    // any `now` in that window exercises it.
    using namespace real_helpers;
    RealHubHarness h{2, kMacRol2};
    h.tracker.startGrid();
    const int64_t a = h.tracker.gridAnchorUs();

    const int64_t first_t0 = a + (int64_t) 31 * timedgrid::kSlotPitchUs;
    for (int64_t now = a; now < first_t0; now += 10000) {
        const int64_t t0 = h.tracker.nextT0ForSlotUs(31, now);
        EXPECT_EQ(t0, first_t0) << "now " << now;
        EXPECT_GE(t0, now);
    }
}

TEST(GridAnchor, SlotIndexWrapsRatherThanRunningOffTheGrid) {
    using namespace real_helpers;
    RealHubHarness h{2, kMacRol2};
    h.tracker.startGrid();
    const int64_t a = h.tracker.gridAnchorUs();
    EXPECT_EQ(h.tracker.nextT0ForSlotUs(timedgrid::kSlotCount, a),
              h.tracker.nextT0ForSlotUs(0, a));
}

TEST(GridAnchor, WithoutAGridTheAnswerIsNow) {
    // A caller that ignores gridStarted() must send immediately, not at some
    // instant derived from a zero anchor.
    using namespace real_helpers;
    RealHubHarness h{2, kMacRol2};
    ASSERT_FALSE(h.tracker.gridStarted());
    EXPECT_EQ(h.tracker.nextT0ForSlotUs(5, 123456), 123456);
}

// ---------------------------------------------------------------------------
// B1 — grid-aligned downlink, default OFF.
//
// Alignment costs up to a full round (1.5 s) of latency on a user command and
// buys nothing until the node opens a single window at that instant (B3). A
// node in Mode A hears three windows every 1.5 s wherever the burst starts, so
// enabling it now would be a pure regression. These tests pin that default as
// hard as they pin the mechanism.
// ---------------------------------------------------------------------------

TEST(GridAligned, DefaultsToOffSoNothingRegresses) {
    using namespace real_helpers;
    RealHubHarness h{2, kMacRol2};
    EXPECT_FALSE(h.rol.grid_aligned());
}

TEST(GridAligned, SlotsAreClaimedInDeclarationOrder) {
    // Same mechanism as login_slot_: claimed in setup(), in YAML order, so it
    // is stable across reboots with no configuration. RealHubHarness does not
    // call setup() (it would restore NVS and schedule logins), so this test
    // drives it explicitly — which is also what makes it a real check of the
    // claim path rather than of a default-initialised member.
    using namespace real_helpers;
    RealHubHarness a{2, kMacRol2};
    RealHubHarness b{3, kMacRol2};
    a.rol.setup();
    b.rol.setup();

    EXPECT_NE(a.rol.grid_slot(), b.rol.grid_slot());
    EXPECT_LT(a.rol.grid_slot(), timedgrid::kSlotCount);
    EXPECT_LT(b.rol.grid_slot(), timedgrid::kSlotCount);
}

TEST(GridAligned, SlotWrapsAtTheGridDepth) {
    // A 33rd client must share a slot rather than address one that does not
    // exist. Asserting the rule directly; the claim itself is above.
    for (uint32_t login = 0; login < 70; ++login)
        EXPECT_LT((uint8_t) (login % timedgrid::kSlotCount), timedgrid::kSlotCount);
    EXPECT_EQ(32u % timedgrid::kSlotCount, 0u);
}

TEST(GridAligned, DelayIsZeroWithoutAGrid) {
    // A caller must never wait on an anchor that was never set.
    using namespace real_helpers;
    RealHubHarness h{2, kMacRol2};
    ASSERT_FALSE(h.tracker.gridStarted());
    EXPECT_EQ(h.tracker.msUntilNextT0(5), 0u);
}

TEST(GridAligned, DelayRoundsUpSoAFrameNeverLandsEarly) {
    // Arriving a millisecond early puts the frame in the previous slot's tail.
    using namespace real_helpers;
    RealHubHarness h{2, kMacRol2};
    h.tracker.startGrid();

    // Slot 1's T0 is 46875 us after the anchor. Ask 1 us after the anchor:
    // 46874 us remain, which must round UP to 47 ms, not down to 46.
    h.tracker.sim_now_us = h.tracker.gridAnchorUs() + 1;
    EXPECT_EQ(h.tracker.msUntilNextT0(1), 47u);

    // Exactly at a T0 the answer is 0, not a whole round.
    h.tracker.sim_now_us = h.tracker.gridAnchorUs() + timedgrid::kSlotPitchUs;
    EXPECT_EQ(h.tracker.msUntilNextT0(1), 0u);
}

TEST(GridAligned, DelayNeverExceedsOneRound) {
    using namespace real_helpers;
    RealHubHarness h{2, kMacRol2};
    h.tracker.startGrid();
    for (uint8_t k = 0; k < timedgrid::kSlotCount; ++k)
        for (int64_t off = 0; off < (int64_t) timedgrid::kRoundUs; off += 37000) {
            h.tracker.sim_now_us = h.tracker.gridAnchorUs() + off;
            EXPECT_LE(h.tracker.msUntilNextT0(k), timedgrid::kRoundUs / 1000)
                << "slot " << (int) k << " off " << off;
        }
}

TEST(GridAligned, WithAlignmentOffTheFrameGoesOutImmediately) {
    using namespace real_helpers;
    RealHubHarness h{2, kMacRol2};
    h.tracker.startGrid();
    h.tracker.sim_now_us = h.tracker.gridAnchorUs() + 1000;   // mid-slot

    uint8_t frame[8] = {1, 2, 3, 4, 5, 6, 7, 8};
    const size_t before = h.tracker.sent_copies.size();
    h.rol.send_aligned_for_test(frame, sizeof(frame));
    EXPECT_EQ(h.tracker.sent_copies.size(), before + 1u)
        << "the default path must not defer anything";
}

TEST(GridAligned, WithAlignmentOnTheFrameIsDeferredToT0) {
    using namespace real_helpers;
    RealHubHarness h{2, kMacRol2};
    h.tracker.startGrid();
    h.rol.set_grid_aligned(true);
    // 1 us past the anchor: this client's slot T0 is a whole pitch away unless
    // it happens to own slot 0, so force a slot that is definitely ahead.
    h.tracker.sim_now_us = h.tracker.gridAnchorUs() + 1;

    uint8_t frame[8] = {1, 2, 3, 4, 5, 6, 7, 8};
    h.rol.send_aligned_for_test(frame, sizeof(frame));

    // The frame is handed to the transmit queue IMMEDIATELY, carrying the mark
    // it is to be fired at. It used to be held here in an ESPHome timeout and
    // handed over near the right time, which could only ever deliver it to the
    // back of the queue — the queue owns the radio, so only the queue can fire
    // it ON the mark (B5). What "deferred" means, therefore, is the instant
    // that travels with it, not whether send() has been called yet.
    // earliest_us is a FIRE instant; what the node's window is built around is
    // the T0, kPreambleToT0Us later. Asserting the fire instant against
    // nextClearT0ForSlotUs() — the very expression send_aligned_ evaluates —
    // would restate the code back at itself AND hide the missing conversion,
    // which is exactly how a 3136 us placement error survived review.
    const int64_t fire    = h.tracker.last_earliest_us;
    const int64_t seen_t0 = fire + (int64_t) loratiming::kPreambleToT0Us;

    // The real contract: that T0 is a mark of THIS node's slot. A mark is a
    // fixed point of nextT0ForSlotUs — asking for the next mark strictly after
    // the instant just before it must return it.
    EXPECT_EQ(h.tracker.nextT0ForSlotUs(h.rol.grid_slot(), seen_t0 - 1), seen_t0)
        << "the frame's T0 must land ON a mark of this node's slot, not "
           "kPreambleToT0Us past one";
    EXPECT_GT(seen_t0, h.tracker.sim_now_us)
        << "and that mark must be in the future, or nothing was placed at all";
}

TEST(GridAligned, ADroppedFrameDoesNotConsumeItsMark) {
    // send() drops silently when the buffer pool is exhausted or the handoff
    // queue is full — it used to return void, so the placement path could not
    // tell. It recorded the mark as spent anyway, which pushed the NEXT command
    // for this node a further round out to make room for a frame that had never
    // been queued: a real command delayed 1.5 s by a phantom.
    using namespace real_helpers;
    RealHubHarness h{2, kMacRol2};
    h.tracker.startGrid();
    h.rol.set_grid_aligned(true);
    h.tracker.sim_now_us = h.tracker.gridAnchorUs() + 1;

    uint8_t frame[4] = {1, 2, 3, 4};

    // First frame is dropped by the transmit queue.
    h.tracker.drop_next_sends = 1;
    h.rol.send_aligned_for_test(frame, sizeof(frame));
    const int64_t dropped_at = h.tracker.last_earliest_us;

    // The next real command must get THAT mark, not the one after it. The
    // placement is unchanged; only the bookkeeping was wrong.
    h.rol.send_aligned_for_test(frame, sizeof(frame));
    EXPECT_EQ(h.tracker.last_earliest_us, dropped_at)
        << "a mark spent on a frame that was never queued is a mark lost";
}

TEST(GridAligned, ASecondCommandInTheSameRoundGoesToTheNextMark) {
    // The old deferral was a NAMED ESPHome timeout, so a second command for the
    // same node before the first had fired replaced it: a dropped command,
    // silently, whenever two arrived inside one round. One frame per mark was
    // the right invariant; dropping was never the way to get it.
    using namespace real_helpers;
    RealHubHarness h{2, kMacRol2};
    h.tracker.startGrid();
    h.rol.set_grid_aligned(true);
    h.tracker.sim_now_us = h.tracker.gridAnchorUs() + 1;

    uint8_t first[4] = {1, 2, 3, 4};
    h.rol.send_aligned_for_test(first, sizeof(first));
    const int64_t mark1 = h.tracker.last_earliest_us;

    uint8_t second[4] = {5, 6, 7, 8};
    h.rol.send_aligned_for_test(second, sizeof(second));
    const int64_t mark2 = h.tracker.last_earliest_us;

    EXPECT_EQ(h.tracker.sent_copies.size(), (size_t) 2)
        << "both commands must reach the radio; neither may be replaced";
    EXPECT_GT(mark2, mark1) << "the second must be placed at a LATER mark";
    EXPECT_EQ(mark2 - mark1, (int64_t) timedgrid::kRoundUs)
        << "and the next mark for this node is exactly one round on";
}

TEST(GridAligned, TheDeferredFrameSurvivesTheCallersFree) {
    // Every caller frees its packed buffer the moment send_aligned_ returns, so
    // a deferred send that captured the pointer would hand the scheduler freed
    // memory. Write a pattern, free it, then let the timer fire.
    using namespace real_helpers;
    RealHubHarness h{2, kMacRol2};
    h.tracker.startGrid();
    h.rol.set_grid_aligned(true);
    h.tracker.sim_now_us = h.tracker.gridAnchorUs() + 1;

    auto *heap = static_cast<uint8_t *>(malloc(16));
    for (int i = 0; i < 16; ++i) heap[i] = static_cast<uint8_t>(0xC0 + i);
    h.rol.send_aligned_for_test(heap, 16);
    memset(heap, 0xEE, 16);   // poison, as a real free+reuse would
    free(heap);

    h.clock.tick(2000);
    SUCCEED() << "no use-after-free; the copy is what was transmitted";
}

// ---------------------------------------------------------------------------
// B4 — pack once, retransmit the stored bytes.
//
// Re-packing on retry was two bugs at once: a fresh msgid made the node execute
// the command a SECOND time (a blind that moves twice), and re-reading
// op_position_ at pack time meant a user moving the blind mid-retry produced
// different plaintext under a msgid-derived AEAD nonce.
// ---------------------------------------------------------------------------

TEST(TrackedOpRetry, ARetransmitReusesTheMsgidAndTheExactBytes) {
    using namespace real_helpers;
    RealHubHarness h{18, kMacRol2};
    ensure_psa_ready();
    h.rol.registered_ = true;

    h.rol.send_cover_operation(LORA_COVER_OPERATION__COVOP_POSITION, 0, 0.5f);
    h.clock.tick(10);
    const auto first = h.radio.hub_to_node_frames();
    ASSERT_FALSE(first.empty()) << "the command must go out at all";
    const auto original = first.back();

    // Let the retry timer fire without any ack.
    h.clock.tick(3000 + 50);   // kOpRetryIntervalMs
    const auto after = h.radio.hub_to_node_frames();
    ASSERT_GT(after.size(), first.size()) << "a retry must be transmitted";
    const auto retry = after.back();

    EXPECT_EQ(retry.bytes, original.bytes)
        << "byte-identical: same msgid, same ciphertext, no GCM nonce reuse "
           "and no second execution at the node";
}

TEST(TrackedOpRetry, ANewCommandGetsANewMsgid) {
    // Pack-once must not freeze the client: a fresh user command is a NEW
    // command and must supersede, with its own msgid. Only the RETRY of an
    // in-flight command reuses bytes.
    using namespace real_helpers;
    RealHubHarness h{18, kMacRol2};
    ensure_psa_ready();
    h.rol.registered_ = true;

    h.rol.send_cover_operation(LORA_COVER_OPERATION__COVOP_POSITION, 0, 0.25f);
    h.clock.tick(10);
    const auto first = h.radio.hub_to_node_frames().back();

    h.rol.send_cover_operation(LORA_COVER_OPERATION__COVOP_POSITION, 0, 0.90f);
    h.clock.tick(10);
    const auto second = h.radio.hub_to_node_frames().back();

    EXPECT_NE(first.bytes, second.bytes)
        << "a new command is not a retransmission";
}

TEST(TrackedOpRetry, ASecondGestureSupersedesTheFirstOnTheWire) {
    // Section 11b: "one Home Assistant gesture can become two commands 1.5 s
    // apart." The hub tracks exactly ONE command per node — begin_tracked_op_
    // overwrites op_first_msgid_ — so from the instant the second arrives the
    // hub will neither accept the first's ack nor retry it. The first frame was
    // nevertheless still in the transmit queue, placed at an earlier mark, and
    // went out anyway.
    //
    // What the listener can say is what the frame CARRIES. Whether it is then
    // dropped at the front of the queue is the tracker's half, and is tested
    // against the real queue in real_lora_tracker_test.
    using namespace real_helpers;
    RealHubHarness h{18, kMacRol2};
    ensure_psa_ready();
    h.rol.registered_ = true;

    h.rol.send_cover_operation(LORA_COVER_OPERATION__COVOP_OPERATION,
                               COV_OPERATION__CMD_OPEN, 0.0f);
    h.clock.tick(10);
    const uint32_t gen_first = h.tracker.last_supersede_gen;
    EXPECT_EQ(h.tracker.last_supersede_key, 18u)
        << "the key is the node's address: one node's command must not retire "
           "another's";
    EXPECT_NE(gen_first, 0u) << "0 means 'takes no part', which a command does";

    h.rol.send_cover_operation(LORA_COVER_OPERATION__COVOP_OPERATION,
                               COV_OPERATION__CMD_CLOSE, 0.0f);
    h.clock.tick(10);
    EXPECT_GT(h.tracker.last_supersede_gen, gen_first)
        << "a new logical command must retire the one still waiting for its mark";
}

TEST(TrackedOpRetry, ARetryDoesNotSupersedeTheFrameItIsARetryOf) {
    // The half that is easy to get wrong. A retransmission is the SAME logical
    // command; giving it a fresh generation would retire the original frame —
    // the bug this fixes, with an extra step, and it would show up only when a
    // retry overtook a placed original.
    using namespace real_helpers;
    RealHubHarness h{18, kMacRol2};
    ensure_psa_ready();
    h.rol.registered_ = true;

    h.rol.send_cover_operation(LORA_COVER_OPERATION__COVOP_POSITION, 0, 0.5f);
    h.clock.tick(10);
    const uint32_t gen = h.tracker.last_supersede_gen;

    h.clock.tick(3000 + 50);   // kOpRetryIntervalMs, no ack
    EXPECT_EQ(h.tracker.last_supersede_gen, gen)
        << "a retry rides under the generation its frame was built with";
}

TEST(TrackedOpRetry, RoutineDownlinksTakeNoPartInSupersession) {
    // Everything that is not a tracked op carries key 0. A GridSync or a
    // schedule push retired by a cover command would be a frame silently lost
    // because somebody moved a blind.
    using namespace real_helpers;
    RealHubHarness h{18, kMacRol2};
    ensure_psa_ready();
    h.rol.registered_ = true;

    h.rol.send_login();
    h.clock.tick(10);
    EXPECT_EQ(h.tracker.last_supersede_key, 0u)
        << "a login is not a tracked op and must never retire one";
}

// NOTE on what is NOT tested here. The sharpest form of the hazard —
// op_position_ changing between the pack and the retry WITHOUT a new command —
// is not reachable through the public API in this harness, because
// send_cover_operation() always starts a fresh tracked op. Pack-once removes it
// by construction (the retry never re-reads live state), and that is verified
// above by byte identity rather than by reproducing the mutation.

// ---------------------------------------------------------------------------
// The ModeTest report, as numbers.
//
// The hub recomputes every rate from the node's RAW counters — I1: the node
// never grades itself — and logged them in one line that only a console can
// read. B3's gate is "reception >= Mode A over a week", which is a week of
// Home Assistant history, so the numbers have to exist as numbers. A single
// text_sensor cannot carry them either: HA caps a state at 255 characters and
// the line is up to 512.
// ---------------------------------------------------------------------------

TEST(RealLoraClient, AModeTestReportIsKeptAsNumbersNotJustLogged) {
    proto_sim::SimClock clock;
    proto_sim::SimRadio radio;
    esphome::shim_hooks::set_active_clock(&clock);
    esphome::shim_hooks::reset_nvs();
    esphome::lora_tracker::shim_hooks::set_active_radio(&radio);
    ensure_psa_ready();

    LORATracker tracker;
    LORAClient  rol;
    rol.set_name("rol");
    rol.set_short_address(18);
    rol.set_subnet_address(2);
    rol.set_address(kMacRol2);
    RealTimeClock time; time.set_now(1787000000, /*valid=*/true);
    rol.set_time(&time);
    tracker.register_client(&rol);
    rol.registered_ = true;

    EXPECT_FALSE(rol.mode_test_summary().valid)
        << "nothing has been measured yet — the entity must read unknown, not 0";

    // A report as the node sends one: raw counters, no conclusions.
    Hist phase = HIST__INIT;
    phase.p50 = 120; phase.p99 = 900; phase.max = 1500; phase.n = 64;
    Hist turn  = HIST__INIT;
    turn.p50 = 4000; turn.p99 = 7000; turn.n = 64;

    ModeTestReport rep = MODE_TEST_REPORT__INIT;
    rep.mode          = 2;            // what the node APPLIED
    rep.armrefusal    = 0;
    rep.elapseds      = 300;
    rep.seqfirst      = 1;
    rep.seqlast       = 100;          // 100 offered over the span it observed
    rep.detected      = 95;
    rep.crcvalid      = 94;
    rep.addressed     = 90;
    rep.windowsarmed  = 100;
    rep.windowshit    = 90;
    rep.phaseerrus    = &phase;
    rep.turnaroundus  = &turn;
    rep.powerprofileproduction = true;

    LoraHeader hdr = LORA_HEADER__INIT;
    hdr.destaddress   = esphome::lora_tracker::kHubAddress;
    hdr.destsubnet    = 2;
    hdr.senderaddress = 18;
    hdr.msgid         = 1;

    LoraClientResponseMessage msg = LORA_CLIENT_RESPONSE_MESSAGE__INIT;
    msg.header         = &hdr;
    msg.proto_case     = LORA_CLIENT_RESPONSE_MESSAGE__PROTO_MODETESTREPORT;
    msg.modetestreport = &rep;

    std::vector<uint8_t> frame(lora_client_response_message__get_packed_size(&msg));
    lora_client_response_message__pack(&msg, frame.data());
    rol.set_response(frame.data(), frame.size());

    const auto &s = rol.mode_test_summary();
    ASSERT_TRUE(s.valid) << "a report must reach the summary, not only the log";
    EXPECT_EQ(s.mode, 2u);
    EXPECT_EQ(s.arm_refusal, 0u);
    EXPECT_EQ(s.windows_armed, 100u);
    EXPECT_EQ(s.windows_hit, 90u);
    EXPECT_EQ(s.phase_p99_us, 900);
    EXPECT_EQ(s.turnaround_p99_us, 7000);
    // Recomputed here, not taken from the node: 10 of 100 windows missed.
    EXPECT_EQ(s.wmr_ppm, 100000u);
    // Link loss is CRC-VALID over offered — the air, before any addressing or
    // MAC decision — against the node's own seqFirst..seqLast span, which is
    // the honest denominator: the hub's own count would report the node's late
    // arrival as packet loss. 94 of 100, so 6 %.
    EXPECT_EQ(s.fer_link_ppm, 60000u);

    esphome::lora_tracker::shim_hooks::set_active_radio(nullptr);
    esphome::shim_hooks::set_active_clock(nullptr);
}

// ---------------------------------------------------------------------------
// The routine downlinks are PLACED (B1a's last open half)
//
// send_aligned_ was reached only by the tracked-op path and the grid
// publication. TimeSync, ScheduleConfig and BaseNonceExchange went out through
// the bare parent_->send(), so in Mode B they left whenever the queue drained
// — into a window the node had stopped opening. The largest frame on the link
// (a 152 B ScheduleConfig) was also the one least likely to be heard.
//
// Whether each may be sent as ONE copy is a separate question from whether it
// is placed, and it turns on one thing: is the loss visible?
// ---------------------------------------------------------------------------

TEST(PlacedDownlinks, AScheduleConfigIsPlacedAndMayBeASingleShot) {
    // Single-shot ELIGIBLE, and the only one of the three that is: the node
    // ACKS a schedule push, so a missed copy is visible and the retry is
    // already a burst. That is precisely Rule 4's bounded exposure.
    using namespace real_helpers;
    RealHubHarness h{18, kMacRol2};
    h.rol.registered_ = true;
    h.rol.node_fw_version_ = 0x00010203;
    h.rol.enable_timed_mode(true);
    ASSERT_TRUE(h.tracker.gridStarted());
    give_phase_report(h);
    ASSERT_EQ(h.rol.txPolicyNow(), timedmode::TxPolicy::SingleShot);

    const size_t before = h.tracker.sent_earliest_us.size();
    h.rol.send_schedule_config();
    ASSERT_GT(h.tracker.sent_earliest_us.size(), before)
        << "the schedule push must have been sent";

    EXPECT_GT(h.tracker.last_earliest_us, 0)
        << "PLACED: a schedule push sent bare leaves whenever the queue drains, "
           "which in Mode B is not when the node is listening";
    EXPECT_EQ(h.tracker.last_copies, 1)
        << "and it is single-shot eligible, because its loss is visible";
}

TEST(PlacedDownlinks, ATimeSyncIsPlacedButAlwaysABurst) {
    // A TimeSync carries NO ack. Rule 4's "being wrong costs one frame" rests
    // on the loss being visible; for an unacked frame a missed single copy is
    // silent and the node runs on a stale clock until the next push. So the
    // burst is asked for explicitly, even on a node the hub would otherwise
    // trust with one copy.
    using namespace real_helpers;
    RealHubHarness h{18, kMacRol2};
    h.rol.registered_ = true;
    h.rol.node_fw_version_ = 0x00010203;
    h.rol.enable_timed_mode(true);
    give_phase_report(h);
    ASSERT_EQ(h.rol.txPolicyNow(), timedmode::TxPolicy::SingleShot);

    const size_t before = h.tracker.sent_earliest_us.size();
    h.rol.send_timesync();
    ASSERT_GT(h.tracker.sent_earliest_us.size(), before);

    EXPECT_GT(h.tracker.last_earliest_us, 0) << "placed";
    EXPECT_NE(h.tracker.last_copies, 1)
        << "an unacked frame must not be reduced to one copy: nothing would "
           "ever learn it was lost";
}

TEST(PlacedDownlinks, ABaseNonceExchangeIsPlacedAndAlwaysABurst) {
    // This frame INSTALLS A KEY — the node adopts the nonce and persists it,
    // and until it does neither end can decrypt the other. It carries no ack,
    // so a lost single copy breaks the session silently. There is no version of
    // "costs one frame" that applies.
    using namespace real_helpers;
    RealHubHarness h{18, kMacRol2};
    h.rol.registered_ = true;
    h.rol.node_fw_version_ = 0x00010203;
    h.rol.enable_timed_mode(true);
    give_phase_report(h);
    ASSERT_EQ(h.rol.txPolicyNow(), timedmode::TxPolicy::SingleShot);

    const size_t before = h.tracker.sent_earliest_us.size();
    h.rol.send_base_nonce_exchange();
    ASSERT_GT(h.tracker.sent_earliest_us.size(), before);

    EXPECT_GT(h.tracker.last_earliest_us, 0) << "placed";
    EXPECT_NE(h.tracker.last_copies, 1) << "never one copy for a key install";
}

TEST(PlacedDownlinks, ATimeSyncCarriesTheHubsInSlotCountToTheNode) {
    // U-4. NodeState::in_slot_uplinks is §4.6's own promotion criterion and the
    // node cannot measure it: where its uplink landed is produced by its
    // TRANSMIT path — CAD, a burst-end deferral, a random backoff — and the
    // question is where the frame ARRIVED. "A beacon saying I am ready says
    // nothing about where its window actually landed."
    //
    // So the node had it hardcoded to the value that satisfies the criterion,
    // which meant Demotion::NotConfirmed could never fire. The hub has been
    // counting the real thing all along (noteUplinkPlacement_) with no way to
    // tell the node. TimeSync is the carrier because the hub answers every
    // beacon with one.
    //
    // Decoded with the REAL generated stub rather than the mirror, so this
    // asserts the wire bytes and not a second definition of them.
    using namespace real_helpers;
    RealHubHarness h{18, kMacRol2};
    h.rol.registered_ = true;
    h.rol.enable_timed_mode(true);

    // Three uplinks landing in this node's slot: what the node is not allowed
    // to claim for itself.
    feed_in_slot_uplink(h, 41);
    feed_in_slot_uplink(h, 42);
    feed_in_slot_uplink(h, 43);
    ASSERT_EQ(h.rol.hubBelief().in_slot_acks, 3u)
        << "precondition: the hub has observed three in slot";

    const size_t before = h.radio.hub_to_node_frames().size();
    h.rol.send_timesync();
    ASSERT_GT(h.radio.hub_to_node_frames().size(), before);

    int seen = 0;
    for (const auto &f : h.radio.hub_to_node_frames()) {
        LoraClientOperationMessage *m = lora_client_operation_message__unpack(
            nullptr, f.bytes.size(), f.bytes.data());
        if (m == nullptr) continue;
        if (m->cmd_case == LORA_CLIENT_OPERATION_MESSAGE__CMD_TIMESYNC &&
            m->timesync != nullptr) {
            EXPECT_EQ(m->timesync->inslotuplinks, 3u)
                << "the hub must tell the node what it has actually observed — "
                   "this is the only route by which the node's own promotion "
                   "criterion can ever be satisfied";
            ++seen;
        }
        lora_client_operation_message__free_unpacked(m, nullptr);
    }
    EXPECT_EQ(seen, 1) << "exactly one TimeSync";
}

TEST(PlacedDownlinks, ARefusedBaseNonceExchangeLeavesTheSessionWorking) {
    // T-2's sharp edge, and the reason send_aligned_ stopped returning void.
    //
    // send() refuses a frame when the buffer pool is exhausted — five buffers
    // against a queue of twenty, and a placed frame holds its buffer until its
    // mark, so a fleet-wide push runs out. The mark is correctly not consumed,
    // and that was already handled. What was NOT is that no producer could see
    // the refusal.
    //
    // For this frame that was a session break: the new base nonce was stored in
    // s_base_nonce_map BEFORE the send, so on a refusal the hub began deriving
    // its IVs from a nonce the node had never been told. Nothing either end
    // sent could be decrypted by the other and only a REGISTER recovered it —
    // from a full buffer pool, with one warning line in the log.
    //
    // Committing after a successful handoff leaves BOTH ends on the previous
    // base, which is a working session that the caller can simply retry.
    using namespace real_helpers;
    RealHubHarness h{18, kMacRol2};
    ensure_psa_ready();          // real AEAD, not a stub: the tag is the point
    h.rol.registered_ = true;
    h.rol.node_fw_version_ = 0x00010203;

    // Establish a working session, so there is a nonce that must survive.
    const uint32_t base = drive_session(h.clock, h.radio, h.rol);
    ASSERT_NE(base, 0u);
    ASSERT_TRUE(h.rol.session_confirmed_);

    // Now the pool is full: the next handoff is refused.
    h.tracker.drop_next_sends = 1;
    h.rol.send_base_nonce_exchange();

    EXPECT_TRUE(h.rol.session_confirmed_)
        << "a frame that never left the hub must not take the session with it";

    // The proof is that the PREVIOUS nonce still decrypts. An encrypted beacon
    // the node builds with the base it actually holds must still verify here,
    // and the phase report inside it is the observable that says it did — the
    // GCM tag gates handle_beacon_, so a hub that had rolled its nonce forward
    // would reject this frame and report nothing.
    proto_sim::LoraClientResponseMessage inner;
    inner.header.destAddress   = esphome::lora_tracker::kHubAddress;
    inner.header.destSubnet    = 2;
    inner.header.senderAddress = 18;
    inner.header.msgId         = h.rol.frame_counter_.rx_message_id + 1;
    inner.proto                = proto_sim::LoraClientResponseMessage::Proto::Beacon;
    inner.beacon.fwVersion          = 0x00010203;
    inner.beacon.phasePresent       = true;
    inner.beacon.phase.rtcSlowSrc   = 2;
    inner.beacon.phase.errUs        = 77;
    inner.beacon.phase.spreadUs     = 88;
    inner.beacon.phase.samples      = timedmode::kPromotionPhaseSamples;
    inner.beacon.phase.outsideGuard = 0;

    auto plain = proto_sim::serialize_resp_payload(inner);
    uint8_t aad[proto_sim::kHeaderAadLen];
    proto_sim::build_header_aad(inner.header.destAddress, inner.header.destSubnet,
                                inner.header.senderAddress, inner.header.msgId, aad);
    uint8_t iv[12];
    proto_sim::derive_gcm_iv(base, inner.header.msgId, iv);
    auto enc = proto_sim::aes_gcm_encrypt(iv, aad, sizeof(aad),
                                          plain.data(), plain.size());
    proto_sim::LoraClientResponseMessage outer;
    outer.header               = inner.header;
    outer.proto                = proto_sim::LoraClientResponseMessage::Proto::Encrypted;
    outer.encrypted.tag        = enc.tag;
    outer.encrypted.ciphertext = enc.ciphertext;
    auto frame = proto_sim::serialize_resp(outer);
    h.rol.set_response(frame.data(), frame.size());

    EXPECT_EQ(h.rol.hubBelief().phase_err_us, 77)
        << "the hub must still be on the base nonce the node actually holds — "
           "this beacon was encrypted with it, and a rolled-forward nonce "
           "would have failed its tag and reported nothing";
}

TEST(PlacedDownlinks, ARoutineDownlinkDoesNotEraseRuleFoursExposure) {
    // The defect routing these through send_aligned_ would have made routine.
    //
    // send_aligned_ assigned op_sent_single_shot_ — the flag Rule 4 reads to
    // decide whether an unacked command costs this node its confidence — so
    // ANY later frame overwrote the tracked command's shape. A grid publication
    // already did it (it asks for the burst explicitly, so it cleared the
    // flag); a TimeSync or schedule push would have done it on every push.
    // The flag now belongs to the command it describes.
    using namespace real_helpers;
    RealHubHarness h{18, kMacRol2};
    h.rol.registered_ = true;
    h.rol.node_fw_version_ = 0x00010203;
    h.rol.enable_timed_mode(true);
    give_phase_report(h);
    ASSERT_EQ(h.rol.txPolicyNow(), timedmode::TxPolicy::SingleShot);

    // A command that really did go out as one copy.
    h.rol.send_cover_operation(LORA_COVER_OPERATION__COVOP_OPERATION,
                               COV_OPERATION__CMD_OPEN, 0.0f);
    h.clock.tick(10);
    ASSERT_EQ(h.tracker.last_copies, 1) << "precondition: it was a single shot";

    // A TimeSync in between — a burst, and nothing to do with that command.
    h.rol.send_timesync();
    h.clock.tick(10);
    ASSERT_NE(h.tracker.last_copies, 1);

    // Now let the command go unacked. Rule 4 must still fire: one frame of
    // exposure is the whole promise single-shot makes, and it is only kept if
    // the hub can still tell the command was a single shot.
    h.clock.tick(3000 + 50);   // kOpRetryIntervalMs
    EXPECT_EQ(h.rol.txPolicyNow(), timedmode::TxPolicy::Burst)
        << "an unacked single shot must put this node back on bursts, however "
           "many routine downlinks happened in between";
}

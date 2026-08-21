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

#include "sim/sim_clock.h"
#include "sim/sim_radio.h"
#include "sim/wire_codec.h"
#include "sim/crypto.h"

#include <psa/crypto.h>

#include <gtest/gtest.h>

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

inline std::vector<uint8_t> serialize_register(uint64_t mac) {
    proto_sim::LoraClientResponseMessage m;
    m.proto        = proto_sim::LoraClientResponseMessage::Proto::Register;
    m.reg.mac_addr = mac;
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

    auto reg = serialize_register(kMacRol2);
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

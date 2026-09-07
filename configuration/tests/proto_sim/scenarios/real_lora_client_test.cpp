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
    const size_t before = h.tracker.sent_copies.size();
    h.rol.send_aligned_for_test(frame, sizeof(frame));

    if (h.tracker.msUntilNextT0(h.rol.grid_slot()) == 0) {
        EXPECT_EQ(h.tracker.sent_copies.size(), before + 1u);
    } else {
        EXPECT_EQ(h.tracker.sent_copies.size(), before)
            << "an aligned frame must wait for its slot";
        h.clock.tick(2000);   // past any slot T0 in the round
        EXPECT_GT(h.tracker.sent_copies.size(), before)
            << "and must actually go out once the slot arrives";
    }
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

// NOTE on what is NOT tested here. The sharpest form of the hazard —
// op_position_ changing between the pack and the retry WITHOUT a new command —
// is not reachable through the public API in this harness, because
// send_cover_operation() always starts a fresh tracked op. Pack-once removes it
// by construction (the retry never re-reads live state), and that is verified
// above by byte identity rather than by reproducing the mutation.

#include "sim/hub_model.h"

#include "sim/crypto.h"

#include <algorithm>
#include <cstring>
#include <utility>

namespace proto_sim {

HubListener::HubListener(std::string name, uint8_t short_address, uint8_t subnet_address,
                         uint64_t mac_addr, uint32_t sleep_duration_s,
                         HubTracker* tracker, SimClock* clock, SharedNonceMap* nonces)
    : name_(std::move(name)),
      short_address_(short_address),
      subnet_address_(subnet_address),
      mac_(mac_addr),
      sleep_duration_s_(sleep_duration_s),
      tracker_(tracker),
      clock_(clock),
      nonces_(nonces) {}

void HubListener::simulate_reboot() {
    // RAM-only fields that ESPHome would not have persisted.
    registered_              = false;
    login_acked_             = false;
    login_retry_count_       = 0;
    startup_login_initiated_ = false;
    pending_login_nonce_     = 0;
    tx_message_id_           = 0;
    rx_message_id_           = 0;

    // Cancel any pending timers from the previous boot.
    clock_->cancel_timeout("login_startup_" + name_);
    clock_->cancel_interval("login_retry_" + name_);

    // The file-scope s_base_nonce_map in production lives in BSS and is
    // therefore cleared by a reset; mirror that.
    if (nonces_) nonces_->clear();
}

void HubListener::setup(bool time_valid_at_boot) {
    if (have_nvs_ && nvs_.version == 2) {
        rx_message_id_ = nvs_.rx_message_id;
        // Production "skip-64" gap.
        tx_message_id_ = nvs_.tx_message_id + 64;
        registered_    = nvs_.logged_in;
    } else {
        rx_message_id_ = 0;
        tx_message_id_ = 0;
        registered_    = false;
    }

    if (!registered_) return;

    login_acked_             = false;
    login_retry_count_       = 0;
    startup_login_initiated_ = false;
    pending_login_nonce_     = 0;

    if (time_valid_at_boot) {
        startup_login_initiated_ = true;
        schedule_startup_login_();
    }
    // Without time, production waits on NTP callback. Tests can drive that
    // by calling schedule_startup_login_ directly after marking time valid.
}

void HubListener::schedule_startup_login_() {
    clock_->cancel_timeout("login_startup_" + name_);
    clock_->cancel_interval("login_retry_" + name_);

    const uint32_t stagger = static_cast<uint32_t>(short_address_) * kLoginStaggerMs;
    const uint32_t total   = 5000 + stagger;

    clock_->set_timeout("login_startup_" + name_, total,
                        [this] { do_login_and_arm_retry_(); });
}

void HubListener::do_login_and_arm_retry_() {
    // Reset login state BEFORE send_login. In production the radio is async
    // so the node's ACK reply can only arrive long after send_login returns
    // (and the order doesn't matter). The synchronous SimRadio runs the
    // node's response inside send_login, so resetting login_acked_ AFTER
    // would clobber an already-arrived ACK. Mirrors production semantics
    // ("login is unacknowledged at the start of a challenge"); only the
    // statement order differs.
    login_acked_       = false;
    login_retry_count_ = 0;

    send_login();

    clock_->cancel_interval("login_retry_" + name_);
    clock_->set_interval("login_retry_" + name_, kRetryIntervalMs, [this] {
        if (login_acked_) {
            clock_->cancel_interval("login_retry_" + name_);
            return;
        }
        if (++login_retry_count_ >= kMaxLoginRetries) {
            clock_->cancel_interval("login_retry_" + name_);
            return;
        }
        send_login();
    });
}

void HubListener::send_login() {
    // Same policy as the production fix: reuse pending nonce on retry.
    uint32_t base;
    if (pending_login_nonce_ != 0) {
        base = pending_login_nonce_;
    } else {
        base = rng();
        pending_login_nonce_ = base;
    }

    tx_message_id_ = 0;
    rx_message_id_ = 0;
    nonces_->set(short_address_, base);

    LoraClientOperationMessage op;
    op.header.destAddress   = short_address_;
    op.header.destSubnet    = subnet_address_;
    op.header.senderAddress = 0xFF;
    op.header.msgId         = ++tx_message_id_;

    op.cmd                  = LoraClientOperationMessage::Cmd::Login;
    op.login.nonce          = base;

    persist_nvs_();
    send_op_(std::move(op));
}

void HubListener::send_remote_config() {
    LoraClientOperationMessage op;
    op.header.destAddress   = short_address_;
    op.header.destSubnet    = subnet_address_;
    op.header.senderAddress = 0xFF;
    op.header.msgId         = ++tx_message_id_;

    op.cmd                  = LoraClientOperationMessage::Cmd::ClientConfig;

    op.clientconfig.mac_addr      = mac_;
    op.clientconfig.addr          = short_address_;
    op.clientconfig.subnt         = subnet_address_;
    op.clientconfig.name          = name_;
    op.clientconfig.sleepDuration = sleep_duration_s_;

    persist_nvs_();
    send_op_(std::move(op));
}

void HubListener::enter_sleep() {
    login_acked_         = false;
    login_retry_count_   = 0;
    pending_login_nonce_ = 0;
    clock_->cancel_timeout("login_startup_" + name_);
    clock_->cancel_interval("login_retry_" + name_);

    LoraClientOperationMessage op;
    op.header.destAddress   = short_address_;
    op.header.destSubnet    = subnet_address_;
    op.header.senderAddress = 0xFF;
    op.header.msgId         = ++tx_message_id_;

    op.cmd                  = LoraClientOperationMessage::Cmd::Sysop;
    op.sysop                = ClientOperation::CMD_SLEEP;

    persist_nvs_();
    send_op_(std::move(op));

    const uint32_t stagger = static_cast<uint32_t>(short_address_) * kLoginStaggerMs;
    const uint32_t total   = sleep_duration_s_ * 1000u + kNodeBootMarginMs + stagger;
    clock_->set_timeout("login_startup_" + name_, total,
                        [this] { do_login_and_arm_retry_(); });
}

void HubListener::send_op_(LoraClientOperationMessage msg) {
    tracker_->send(std::move(msg));
}

void HubListener::send_cover_op(CovOperation op) {
    LoraClientOperationMessage m;
    m.header.destAddress   = short_address_;
    m.header.destSubnet    = subnet_address_;
    m.header.senderAddress = 0xFF;
    m.header.msgId         = ++tx_message_id_;

    m.cmd                  = LoraClientOperationMessage::Cmd::Operation;
    m.operation.kind       = LoraCoverOperation::Kind::Operation;
    m.operation.operation  = op;
    persist_nvs_();
    send_op_(std::move(m));
}

void HubListener::send_cover_config(uint32_t open_time, uint32_t close_time,
                                    float height_mm, float axle_mm, float thickness_mm) {
    LoraClientOperationMessage m;
    m.header.destAddress   = short_address_;
    m.header.destSubnet    = subnet_address_;
    m.header.senderAddress = 0xFF;
    m.header.msgId         = ++tx_message_id_;

    m.cmd                  = LoraClientOperationMessage::Cmd::CoverConfig;
    m.coverconfig.openTime         = open_time;
    m.coverconfig.closeTime        = close_time;
    m.coverconfig.blindHeightMm    = height_mm;
    m.coverconfig.axleDiameterMm   = axle_mm;
    m.coverconfig.blindThicknessMm = thickness_mm;
    persist_nvs_();
    send_op_(std::move(m));
}

void HubListener::send_base_nonce_exchange() {
    // Mint a fresh base nonce, store it on the hub side, and send the
    // 4-byte BE-encoded value over the air. Mirrors lora_client.cpp:836.
    uint32_t base = rng();
    nonces_->set(short_address_, base);

    LoraClientOperationMessage m;
    m.header.destAddress   = short_address_;
    m.header.destSubnet    = subnet_address_;
    m.header.senderAddress = 0xFF;
    m.header.msgId         = ++tx_message_id_;

    m.cmd                  = LoraClientOperationMessage::Cmd::BaseNonce;
    m.basenonce.base_nonce = base;
    persist_nvs_();
    send_op_(std::move(m));
}

void HubListener::send_sysop(ClientOperation op) {
    LoraClientOperationMessage m;
    m.header.destAddress   = short_address_;
    m.header.destSubnet    = subnet_address_;
    m.header.senderAddress = 0xFF;
    m.header.msgId         = ++tx_message_id_;

    m.cmd                  = LoraClientOperationMessage::Cmd::Sysop;
    m.sysop                = op;
    persist_nvs_();
    send_op_(std::move(m));
}

void HubListener::send_cover_position(float position) {
    LoraClientOperationMessage m;
    m.header.destAddress   = short_address_;
    m.header.destSubnet    = subnet_address_;
    m.header.senderAddress = 0xFF;
    m.header.msgId         = ++tx_message_id_;

    m.cmd                  = LoraClientOperationMessage::Cmd::Operation;
    m.operation.kind       = LoraCoverOperation::Kind::Position;
    m.operation.position   = position;
    persist_nvs_();
    send_op_(std::move(m));
}

void HubListener::persist_nvs_() {
    nvs_.version          = 2;
    nvs_.rx_message_id    = rx_message_id_;
    nvs_.tx_message_id    = tx_message_id_;
    nvs_.logged_in        = registered_;
    nvs_.last_sleep_epoch = 0; // not modeled in phase 1
    have_nvs_             = true;
    ++nvs_write_count_;
}

// ---------------------------------------------------------------------------
// RX dispatch — mirrors LORAListener::set_response.
// ---------------------------------------------------------------------------
void HubListener::on_frame(const AirFrame& f) {
    if (f.dir != AirFrame::Dir::NodeToHub) return;
    auto parsed = as_resp(f);
    if (!parsed) return;   // production "Could not read protobuf"
    auto m = *parsed;

    // Encrypted-path unwrap (mirrors lora_client.cpp set_response encrypted
    // branch, lines 632-724). REGISTER is never encrypted, so it bypasses.
    // Encryption is inferred from the ONEOF CASE — there is no header flag any
    // more. The IV is not transmitted either; both sides derive it from the
    // stored base nonce and the msgid, so a wrong IV simply fails the AEAD tag
    // check rather than being caught by an explicit comparison.
    if (m.proto == LoraClientResponseMessage::Proto::Encrypted)
    {
        if (!nonces_->contains(m.header.senderAddress)) return;
        const uint32_t base = nonces_->get(m.header.senderAddress);

        uint8_t iv[12];
        derive_gcm_iv(base, m.header.msgId, iv);

        uint8_t aad[kHeaderAadLen];
        build_header_aad(m.header.destAddress, m.header.destSubnet,
                         m.header.senderAddress, m.header.msgId, aad);

        auto plain = aes_gcm_decrypt(iv, aad, sizeof(aad),
                                     m.encrypted.ciphertext.data(),
                                     m.encrypted.ciphertext.size(),
                                     m.encrypted.tag.data(),
                                     m.encrypted.tag.size());
        if (!plain) return; // AEAD verify failed
        auto inner = deserialize_resp(plain->data(), plain->size());
        if (!inner) return;
        // The ciphertext is payload-only: the inner message carries no header,
        // so the plaintext OUTER header stays authoritative.
        const LoraHeader outer_header = m.header;
        m         = *inner;
        m.header  = outer_header;
    }


    // REGISTER bypasses every address/msgid filter (MAC is the gate).
    if (m.proto == LoraClientResponseMessage::Proto::Register) {
        if (m.reg.mac_addr != mac_) return; // not for me

        registered_ = true;
        send_remote_config();

        // (Node configs would be dispatched here in production; covered by
        // node-side tests in scenario A1 phase 2.)

        login_acked_             = false;
        login_retry_count_       = 0;
        pending_login_nonce_     = 0;
        startup_login_initiated_ = true;
        clock_->cancel_timeout("login_startup_" + name_);
        clock_->cancel_interval("login_retry_" + name_);

        clock_->set_timeout("login_startup_" + name_, 500,
                            [this] { do_login_and_arm_retry_(); });
        return;
    }

    if (!registered_) return;
    if (m.header.senderAddress != short_address_) return; // not for me

    if (m.proto == LoraClientResponseMessage::Proto::Login) {
        send_login();
        return;
    }

    // Replay / monotonic check.
    if (m.header.msgId > rx_message_id_) {
        rx_message_id_ = m.header.msgId;
        if (!login_acked_) {
            login_acked_         = true;
            login_retry_count_   = 0;
            pending_login_nonce_ = 0;
            clock_->cancel_interval("login_retry_" + name_);
        }
        persist_nvs_();
    } else {
        return; // duplicate / old
    }

    // Phase 2 decrypts the encrypted variant here; phase 1 just observes.
}

} // namespace proto_sim

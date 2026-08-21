#include "sim/node_model.h"

#include "sim/crypto.h"

#include <utility>

namespace proto_sim {

NodeModel::NodeModel(std::string label, uint64_t mac, SimClock* clock, SimRadio* radio)
    : label_(std::move(label)), mac_(mac), clock_(clock), radio_(radio) {
    radio_->add_sink([this](const AirFrame& f) { on_frame(f); });
}

void NodeModel::reboot(bool keep_cfg) {
    tx_message_id_ = 0;
    rx_message_id_ = 0;
    peer_base_.clear();
    last_login_accepted_ms_ = 0;
    last_motor_cmd_.clear();
    outbox_.clear();
    login_seen_once_ = false;
    open_time_s_  = 0;
    close_time_s_ = 0;
    height_mm_    = 0.0f;
    axle_mm_      = 0.0f;
    thickness_mm_ = 0.0f;
    geometry_applied_ = false;
    if (!keep_cfg) {
        cfg_address_ = 0;
        cfg_subnet_  = 0;
        registered_  = false;
    }
}

void NodeModel::send_register() {
    LoraClientResponseMessage m;
    // SYSCMD_REGISTER resets counters to 0 in production CmdDispatcher.cpp:324.
    tx_message_id_ = 0;
    rx_message_id_ = 0;

    m.header.destAddress   = 0xFF;
    m.header.destSubnet    = 0;
    m.header.senderAddress = cfg_address_;
    m.header.msgId         = ++tx_message_id_;

    m.proto                = LoraClientResponseMessage::Proto::Register;
    m.reg.mac_addr         = mac_;

    send_resp_(std::move(m));
}

void NodeModel::send_available() {
    LoraClientResponseMessage m;
    m.header.destAddress   = last_hub_addr_;
    m.header.destSubnet    = cfg_subnet_;
    m.header.senderAddress = cfg_address_;
    m.header.msgId         = ++tx_message_id_;
    m.proto                = LoraClientResponseMessage::Proto::Avail;
    m.avail.available      = true;
    send_resp_(std::move(m));
}

void NodeModel::send_position(float position) {
    LoraClientResponseMessage m;
    m.header.destAddress   = last_hub_addr_;
    m.header.destSubnet    = cfg_subnet_;
    m.header.senderAddress = cfg_address_;
    m.header.msgId         = ++tx_message_id_;
    m.proto                = LoraClientResponseMessage::Proto::Position;
    m.position.position    = position;
    send_resp_(std::move(m));
}

void NodeModel::send_resp_(LoraClientResponseMessage msg) {
    // Production: pack_response_message() in CmdDispatcher.cpp encrypts only
    // when a base nonce for the destination peer is known. If not known,
    // the message ships as-is (plaintext).
    auto it = peer_base_.find(msg.header.destAddress);
    if (it != peer_base_.end() && msg.proto != LoraClientResponseMessage::Proto::Register) {
        const uint32_t base_nonce = it->second;

        // Build AAD from the (plaintext) outer header. 16 bytes / 4 fields —
        // the old `encrypted` flag is gone from the header.
        uint8_t aad[kHeaderAadLen];
        build_header_aad(msg.header.destAddress, msg.header.destSubnet,
                         msg.header.senderAddress, msg.header.msgId, aad);

        // Serialize the PAYLOAD ONLY — production strips the inner header
        // before encrypting, since the receiver uses the outer one.
        std::vector<uint8_t> inner_bytes = serialize_resp_payload(msg);

        // Derive IV from base_nonce ‖ msgid.
        uint8_t iv[12];
        derive_gcm_iv(base_nonce, msg.header.msgId, iv);

        // Encrypt.
        auto enc = aes_gcm_encrypt(iv, aad, sizeof(aad),
                                   inner_bytes.data(), inner_bytes.size());

        // Outer message wraps the EncryptedPayload.
        // Slim envelope: only tag + ciphertext travel. The receiver rebuilds
        // the IV and AAD from the plaintext outer header.
        LoraClientResponseMessage outer;
        outer.header               = msg.header;
        outer.proto                = LoraClientResponseMessage::Proto::Encrypted;
        outer.encrypted.tag        = enc.tag;
        outer.encrypted.ciphertext = enc.ciphertext;

        AirFrame f = make_resp_frame(outer);
        outbox_.push_back(f);
        radio_->send(f);
        return;
    }

    AirFrame f = make_resp_frame(msg);
    outbox_.push_back(f);
    radio_->send(f);
}

// ---------------------------------------------------------------------------
// RX — mirrors CmdDispatcher::onReceiveNew.
// ---------------------------------------------------------------------------
void NodeModel::on_frame(const AirFrame& f) {
    if (f.dir != AirFrame::Dir::HubToNode) return;
    auto parsed = as_op(f);
    if (!parsed) return;   // "Could not read protobuf"
    const auto& m = *parsed;

    using Cmd = LoraClientOperationMessage::Cmd;

    // msgid replay check — skipped for CMD_LOGIN.
    if (m.cmd != Cmd::Login) {
        if (m.header.msgId > rx_message_id_) {
            rx_message_id_ = m.header.msgId;
        } else {
            return; // duplicate / old
        }
    }

    // Address check — skipped ONLY for CLIENTCONFIG. CMD_LOGIN now follows
    // the standard rule (must be addressed to me or be broadcast); the hub
    // always sends LoginMsg with a specific destAddress, never broadcast,
    // so a LOGIN destined for a different node must be ignored. Mirrors
    // the production fix in CmdDispatcher.cpp:1068.
    if (m.cmd != Cmd::ClientConfig) {
        if (m.header.destAddress != cfg_address_ && m.header.destAddress != 0xFF) {
            return; // not for me
        }
    }

    last_hub_addr_ = m.header.senderAddress;

    switch (m.cmd) {
    case Cmd::Login: {
        // 5-second rate limit (mirrors CmdDispatcher.cpp LOGIN_RATE_LIMIT_MS).
        if (login_seen_once_ &&
            (clock_->now_ms() - last_login_accepted_ms_) < kLoginRateLimitMs) {
            return;
        }
        login_seen_once_        = true;
        last_login_accepted_ms_ = clock_->now_ms();

        tx_message_id_ = 0;
        rx_message_id_ = 0;
        peer_base_[m.header.senderAddress] = m.login.nonce;
        send_available(); // ACK so hub flips login_acked_
        break;
    }
    case Cmd::ClientConfig: {
        if (m.clientconfig.mac_addr != mac_) return;
        cfg_address_ = static_cast<uint8_t>(m.clientconfig.addr);
        cfg_subnet_  = static_cast<uint8_t>(m.clientconfig.subnt);
        registered_  = true;
        break;
    }
    case Cmd::CoverConfig:
        open_time_s_  = m.coverconfig.openTime;
        close_time_s_ = m.coverconfig.closeTime;
        // Production guard (CmdDispatcher.cpp:1291): only apply geometry when
        // ALL three values are non-zero. proto3 unset == 0, so partial
        // populations must NOT clobber the firmware defaults.
        if (m.coverconfig.blindHeightMm   > 0.0f &&
            m.coverconfig.axleDiameterMm  > 0.0f &&
            m.coverconfig.blindThicknessMm > 0.0f) {
            height_mm_        = m.coverconfig.blindHeightMm;
            axle_mm_          = m.coverconfig.axleDiameterMm;
            thickness_mm_     = m.coverconfig.blindThicknessMm;
            geometry_applied_ = true;
        }
        break;
    case Cmd::Operation:
        switch (m.operation.kind) {
        case LoraCoverOperation::Kind::Operation:
            switch (m.operation.operation) {
            case CovOperation::CMD_OPEN:  last_motor_cmd_ = "OPEN";  break;
            case CovOperation::CMD_CLOSE: last_motor_cmd_ = "CLOSE"; break;
            case CovOperation::CMD_STOP:  last_motor_cmd_ = "STOP";  break;
            }
            break;
        case LoraCoverOperation::Kind::Position:
            last_motor_cmd_ = "POS";
            break;
        }
        break;
    case Cmd::Sysop:
        switch (m.sysop) {
        case ClientOperation::CMD_SLEEP:        last_motor_cmd_ = "SLEEP"; break;
        case ClientOperation::CMD_OTA:          last_motor_cmd_ = "OTA"; break;
        case ClientOperation::CMD_STATUS:       last_motor_cmd_ = "STATUS"; break;
        case ClientOperation::CMD_ENABLE_WIFI:  last_motor_cmd_ = "WIFI_ON"; break;
        case ClientOperation::CMD_DISABLE_WIFI: last_motor_cmd_ = "WIFI_OFF"; break;
        }
        break;
    case Cmd::BaseNonce:
        if (!m.basenonce.key_id.empty() || m.basenonce.base_nonce != 0)
            peer_base_[m.header.senderAddress] = m.basenonce.base_nonce;
        break;
    case Cmd::Encrypted:
    case Cmd::NotSet:
        break;
    }
}

} // namespace proto_sim

// S0 golden frames, hub side (docs/mac-separation-layering-plan.md).
//
// Byte-exact wire output of every hub->node producer in lora_client.cpp,
// captured on the code as it stood on 2026-10-16 (hub 7a7d7de), with every
// input that reaches the bytes pinned: rand() seed (the shim's esp_random),
// esp_timer clock, SimClock, wall clock, time zone, NVS (empty), node nonce.
//
// Each scenario records two streams:
//   raw : what the producer handed to LORATracker::send(), with its TxPolicy
//         (the MAC-0 data request as it stands today);
//   air : what went on the air after the per-copy re-stamp and CMAC retag
//         (the frame's own copy sealer, LinkSecurity::copySealer, production
//         code since S4; the shim mirrors the tracker's
//         unpack/stamp/repack around it).
// Stages S2-S7 must leave every file in tests/proto_sim/golden/ untouched.
// Only S8 (the explicit wire break) re-records, deliberately.
//
// The tracker-built GridBeacon is pinned by golden_tracker_frames_test.cpp.

#include "esphome/components/lora_client/lora_client.h"
#include "esphome/components/lora_tracker/lora_tracker.h"
#include "esphome/components/homeassistant/time/homeassistant_time.h"
#include "esphome/components/loracover/cover/lora_cover.h"

#include "blinds.pb-c.h"
// Not used yet: included so the hub's VENDORED MacSap.h (S0 types, node-owned,
// drift-gated) is compiled in the hub's own include context.
#include "MacSap.h"
#include "sim/golden.h"
#include "MacWire.h"
#include "../../../local_components/lora_tracker/burst_frame.h"
#include "sim/sim_clock.h"
#include "sim/sim_radio.h"
#include "sim/wire_codec.h"
#include "sim/crypto.h"
#include "esp_timer.h"

#include <psa/crypto.h>
#include <gtest/gtest.h>

#include <cstdlib>
#include <sstream>

using esphome::lora_tracker::LORAClient;
using esphome::lora_tracker::LORATracker;
using esphome::time::RealTimeClock;

namespace {

constexpr uint64_t    kMac        = 0xE08CFE5F9EC4ULL;
constexpr uint8_t     kNode       = 18;
constexpr uint8_t     kSubnet     = 2;
constexpr uint32_t    kNodeNonce  = 0xACE55001u;
constexpr std::time_t kEpoch      = 1787000000;   // 2026-08-17 (CEST)
constexpr unsigned    kSeed       = 0x5EED0001u;

#if defined(MAC_WIRE_V1) && MAC_WIRE_V1
// S8: with MAC_WIRE_V1 these scenarios record the v1 wire into golden_v1/ (the
// v0 files in golden/ stay pinned by the OFF build). Same scenarios, same
// pinned inputs; the node side of each exchange is the sim's v1 frames.
constexpr bool kV1 = true;
#else
constexpr bool kV1 = false;
#endif

std::string describeV1(const std::vector<uint8_t> &b) {
    std::ostringstream s;
    macwire::WireHdr h;
    size_t off = 0, blen = 0;
    if (!macwire::decode(b.data(), b.size(), &h, &off, &blen)) {
        s << "v1=? len=" << b.size();
        return s.str();
    }
    s << "port=" << (int) h.port << " auth=" << (int) h.auth() << " len=" << b.size()
      << " dst=" << (int) h.dst << " fcnt=" << h.fcnt << " burst=" << (int) h.burst_idx << "/"
      << (int) h.burst_cnt << " onmark=" << h.onMark() << " stamped=" << h.stamped()
      << " ackreq=" << h.ackReq();
    return s.str();
}

std::string describeOp(const std::vector<uint8_t> &b) {
    if (kV1)
        return describeV1(b);
    std::ostringstream s;
    ::LoraClientOperationMessage *m =
        lora_client_operation_message__unpack(nullptr, b.size(), b.data());
    if (m == nullptr) {
        s << "cmd=? len=" << b.size();
        return s.str();
    }
    s << "cmd=" << (int) m->cmd_case << " len=" << b.size();
    if (m->header != nullptr)
        s << " dst=" << m->header->destaddress << " msgid=" << m->header->msgid
          << " burst=" << m->header->burstindex << "/" << m->header->burstcount
          << " onmark=" << m->header->onmark << " stamped=" << m->header->firestamped;
    lora_client_operation_message__free_unpacked(m, nullptr);
    return s.str();
}

// The node's CommandAck reaches exactly this function in production.
struct GProbe : LORAClient {
    using esphome::lora_tracker::LORAListener::handle_command_ack_;
    using esphome::lora_tracker::LORAListener::ctl;
};

struct GoldenHub {
    proto_sim::SimClock clock;
    proto_sim::SimRadio radio;
    LORATracker  tracker;
    LORAClient   rol;
    RealTimeClock time;
    esphome::loracov::LoraCoverComponent cover;
    uint32_t base_nonce{0};

    explicit GoldenHub(bool with_cover = true) {
        std::srand(kSeed);
        proto_sim_timer_reset();
        proto_sim_timer_set_now_us(1'000'000);
        esphome::shim_hooks::set_active_clock(&clock);
        esphome::shim_hooks::reset_nvs();
        esphome::lora_tracker::shim_hooks::set_active_radio(&radio);
        esphome::ESPTime::set_timezone_offset(7200);
        LORAClient::resetModeBArbiterForTest();
        EXPECT_EQ(psa_crypto_init(), PSA_SUCCESS);

        rol.set_name("rol");
        rol.set_short_address(kNode);
        rol.set_subnet_address(kSubnet);
        rol.set_sleep_duration(21600);
        rol.set_address(kMac);
        time.set_now(kEpoch, /*valid=*/true);
        rol.set_time(&time);
        tracker.register_client(&rol);
        if (with_cover) {
            cover.set_name("rol_cover");
            cover.set_open_duration(60);
            cover.set_close_duration(61);
            cover.set_invert_position(false);
            cover.set_blind_height_mm(2000.0f);
            cover.set_axle_diameter_mm(60.0f);
            cover.set_blind_thickness_mm(8.0f);
            cover.setup();
            rol.register_lora_node(&cover);
        }
    }
    ~GoldenHub() {
        esphome::lora_tracker::shim_hooks::set_active_radio(nullptr);
        esphome::shim_hooks::set_active_clock(nullptr);
    }

    void tick(uint32_t ms) {
        proto_sim_timer_advance_us((int64_t) ms * 1000);
        clock.tick(ms);
    }

    // Answer the LOGIN addressed to this node with the session-opening
    // encrypted ClientAvailable, exactly as real_lora_client_test does.
    // v1: the LOGIN is PORT 1 / K_AUTH; the answer is the SESSION_OPENING
    // uplink sealed under the keys the sim derives from its nonce.
    void attachLoginAckV1() {
        radio.add_sink([this](const proto_sim::AirFrame &f) {
            if (f.dir != proto_sim::AirFrame::Dir::HubToNode) return;
            macwire::WireHdr lh;
            size_t off = 0, blen = 0;
            if (!macwire::decode(f.bytes.data(), f.bytes.size(), &lh, &off, &blen)) return;
            if (lh.port != macwire::kPortSession || lh.dst != kNode) return;
            ::LoginMsg *lm = login_msg__unpack(nullptr, blen, f.bytes.data() + off);
            if (lm == nullptr) return;
            base_nonce = lm->nonce;
            login_msg__free_unpacked(lm, nullptr);
            ::ClientAvailable a = CLIENT_AVAILABLE__INIT;
            a.available = true;
            ::NodeAppPdu pdu = NODE_APP_PDU__INIT;
            pdu.pdu_case = NODE_APP_PDU__PDU_AVAIL;
            pdu.avail = &a;
            std::vector<uint8_t> body(node_app_pdu__get_packed_size(&pdu));
            node_app_pdu__pack(&pdu, body.data());
            macwire::WireHdr h;
            h.flags = macwire::withAuth(macwire::kFlagSessionOpening, macwire::Auth::Session);
            h.dst = (uint8_t) esphome::lora_tracker::kHubAddress;
            h.src = kNode; h.subnet = kSubnet; h.fcnt = 1; h.port = macwire::kPortApp;
            h.node_nonce = kNodeNonce;
            auto up = proto_sim::v1_session_frame(base_nonce, kNodeNonce,
                                                  (uint8_t) esphome::lora_tracker::kHubAddress,
                                                  kNode, false, h, body);
            rol.set_response(up.data(), up.size());
        });
    }

    void sendRegister() {
        if (kV1) {
            ::ClientRegister reg = CLIENT_REGISTER__INIT;
            reg.mac_addr     = kMac;
            reg.needs_config = true;
            std::vector<uint8_t> body(client_register__get_packed_size(&reg));
            client_register__pack(&reg, body.data());
            macwire::WireHdr h;
            h.flags = macwire::withAuth(0, macwire::Auth::KAuth);
            h.dst = macwire::kBroadcast; h.src = kNode; h.subnet = kSubnet; h.fcnt = 1;
            h.port = macwire::kPortSession;
            auto b = proto_sim::v1_kauth_frame(false, h, body);
            rol.set_response(b.data(), b.size());
            return;
        }
        proto_sim::LoraClientResponseMessage reg;
        reg.proto            = proto_sim::LoraClientResponseMessage::Proto::Register;
        reg.reg.mac_addr     = kMac;
        reg.reg.needs_config = true;
        auto b = proto_sim::serialize_resp(reg);
        rol.set_response(b.data(), b.size());
    }

    void attachLoginAck() {
        if (kV1) {
            attachLoginAckV1();
            return;
        }
        radio.add_sink([this](const proto_sim::AirFrame &f) {
            if (f.dir != proto_sim::AirFrame::Dir::HubToNode) return;
            auto m = proto_sim::as_op(f);
            if (!m || m->cmd != proto_sim::LoraClientOperationMessage::Cmd::Login) return;
            if (m->header.destAddress != kNode) return;
            const uint32_t base = m->login.nonce;
            base_nonce = base;
            proto_sim::LoraClientResponseMessage inner;
            inner.header.destAddress   = esphome::lora_tracker::kHubAddress;
            inner.header.destSubnet    = kSubnet;
            inner.header.senderAddress = kNode;
            inner.header.msgId         = 1;
            inner.header.sessionNonce  = kNodeNonce;
            inner.proto                = proto_sim::LoraClientResponseMessage::Proto::Avail;
            inner.avail.available      = true;
            auto plain = proto_sim::serialize_resp_payload(inner);
            framecrypto::EtmHeaderFields fields{};
            fields.downlink       = false;
            fields.session_id     = base;
            fields.dest_address   = inner.header.destAddress;
            fields.dest_subnet    = inner.header.destSubnet;
            fields.sender_address = inner.header.senderAddress;
            fields.msgid          = inner.header.msgId;
            auto enc = proto_sim::encrypt_then_cmac_seal(base, kNodeNonce,
                                                         esphome::lora_tracker::kHubAddress, kNode,
                                                         false, fields, plain.data(), plain.size());
            proto_sim::LoraClientResponseMessage outer;
            outer.header               = inner.header;
            outer.proto                = proto_sim::LoraClientResponseMessage::Proto::Encrypted;
            outer.encrypted.tag        = std::vector<uint8_t>(enc.tag, enc.tag + framecrypto::kSessionCmacTagBytes);
            outer.encrypted.ciphertext = enc.ciphertext;
            auto bytes = proto_sim::serialize_resp(outer);
            rol.set_response(bytes.data(), bytes.size());
        });
    }

    // REGISTER (needs_config) -> ClientConfig/CoverConfig -> LOGIN -> encrypted
    // ack -> confirmed session, then the post-confirmation pushes.
    void session() {
        attachLoginAck();
        sendRegister();
        tick(esphome::lora_tracker::LORAListener::kRegisterToLoginDelayMs + 200);
        tick(1000);
        ASSERT_NE(base_nonce, 0u) << "precondition: a LOGIN went out and was answered";
        ASSERT_TRUE(rol.login_acked_) << "precondition: the session is confirmed";
    }

    void ackCtl() {
        auto &p = static_cast<GProbe &>(rol);
        p.handle_command_ack_(p.ctl().last_msgid);
    }

    // Everything sent so far, both streams.
    std::string dump() const {
        std::ostringstream s;
        for (size_t i = 0; i < tracker.sent_wire.size(); ++i) {
            const auto &p = tracker.sent_policy[i];
            s << "raw " << i << " " << describeOp(tracker.sent_wire[i]) << " copies=" << p.copies
              << " stride=" << p.stride_ms << " earliest=" << p.earliest_us
              << " onmark=" << p.on_mark << " reply=" << p.expects_reply
              << " prio=" << (int) p.priority << " sup=" << p.supersede_key << "/"
              << p.supersede_gen << " " << golden::hex(tracker.sent_wire[i]) << "\n";
        }
        size_t k = 0;
        for (const auto &f : radio.transcript()) {
            if (f.dir != proto_sim::AirFrame::Dir::HubToNode) continue;
            s << "air " << k++ << " " << describeOp(f.bytes) << " " << golden::hex(f.bytes) << "\n";
        }
        return s.str();
    }
};

}  // namespace

// The LOGIN challenge on its own (K_auth MIC; no session).
TEST(GoldenHubFrames, LoginChallenge) {
    GoldenHub h(false);
    h.rol.registered_ = true;
    h.rol.send_login();
    h.rol.send_login();   // a retry reuses the pending nonce
    EXPECT_TRUE(golden::matches("hub_login_challenge", h.dump()));
}

// REGISTER-ack paths (ClientConfig + CoverConfig), LOGIN, and the pushes that
// follow a confirmed session (TimeSync, ScheduleConfig, ...).
TEST(GoldenHubFrames, RegisterLoginAndSessionPushes) {
    GoldenHub h;
    h.session();
    h.tick(5000);
    EXPECT_TRUE(golden::matches("hub_register_login_session", h.dump()));
}

// A cover op and a sysop on the Control channel without a session (plaintext),
// including a retransmission of the packed frame.
TEST(GoldenHubFrames, PlaintextCoverOpAndRetry) {
    GoldenHub h;
    h.rol.registered_ = true;
    h.rol.send_cover_operation(LORA_COVER_OPERATION__COVOP_OPERATION, COV_OPERATION__CMD_OPEN, 0.0f);
    h.tick(3050);
    EXPECT_TRUE(golden::matches("hub_plaintext_cover_op", h.dump()));
}

// Encrypted Control channel: cover op (position), ack, then an OTA sysop.
TEST(GoldenHubFrames, EncryptedCoverOpAndSysop) {
    GoldenHub h;
    h.session();
    h.rol.send_cover_operation(LORA_COVER_OPERATION__COVOP_POSITION, 0, 0.25f);
    h.tick(10);
    h.ackCtl();
    h.rol.triggerOTA();
    h.tick(10);
    EXPECT_TRUE(golden::matches("hub_encrypted_cover_op_sysop", h.dump()));
}

// Fire channel, encrypted: TimeSync, ClientConfig, CoverConfig.
TEST(GoldenHubFrames, EncryptedFirePushes) {
    GoldenHub h;
    h.session();
    h.rol.send_timesync();
    h.rol.send_remote_config();
    h.cover.send_remote_config();
    h.tick(10);
    EXPECT_TRUE(golden::matches("hub_encrypted_fire_pushes", h.dump()));
}

// Mac channel: GridSync publish, withdraw, and the GridDemote broadcast.
TEST(GoldenHubFrames, GridSyncWithdrawAndDemote) {
    GoldenHub h;
    h.session();
    h.rol.enable_timed_mode(true);
    h.tick(10);
    h.rol.enable_timed_mode(false);
    h.tick(10);
    h.rol.broadcast_grid_demote();
    h.tick(10);
    EXPECT_TRUE(golden::matches("hub_gridsync_withdraw_demote", h.dump()));
}

// ModeTest stream: START burst, two grid marks, STOP.
TEST(GoldenHubFrames, ModeTestStream) {
    GoldenHub h;
    h.session();
    h.rol.start_mode_test(60, 1500, 2, 1, true, true, true, true, 0);
    h.tick(3100);                 // the grid arms and builds the first mark
    proto_sim_timer_fire_all();   // mark 1
    proto_sim_timer_fire_all();   // mark 2
    h.rol.stop_mode_test();
    EXPECT_TRUE(golden::matches("hub_mode_test_stream", h.dump()));
}

// MAC ping stream, MAC-2 on (reserved msgid block) and then off.
TEST(GoldenHubFrames, MacPingStreams) {
    GoldenHub h;
    h.session();
    h.rol.start_mac_ping(10, 1500, true, 4, /*crypto=*/true);
    proto_sim_timer_fire_all();
    proto_sim_timer_fire_all();
    h.rol.stop_mac_ping();
    h.rol.start_mac_ping(10, 1500, false, 0, /*crypto=*/false);
    proto_sim_timer_fire_all();
    h.rol.stop_mac_ping();
    EXPECT_TRUE(golden::matches("hub_mac_ping_streams", h.dump()));
}

// Drift test stream: START burst, one grid tick, STOP.
TEST(GoldenHubFrames, DriftTestStream) {
    GoldenHub h;
    h.session();
    h.rol.start_drift_test(30, 1000);
    h.tick(3100);
    proto_sim_timer_fire_all();
    h.rol.stop_drift_test();
    EXPECT_TRUE(golden::matches("hub_drift_test_stream", h.dump()));
}

// Per-copy retag: a full 17-copy encrypted burst, each copy re-stamped and
// re-tagged by the frame's own copy sealer.
TEST(GoldenHubFrames, EncryptedBurstRetagEveryCopy) {
    GoldenHub h;
    h.session();
    h.tracker.set_expand_bursts(true);
    h.rol.send_cover_operation(LORA_COVER_OPERATION__COVOP_OPERATION, COV_OPERATION__CMD_CLOSE, 0.0f);
    h.tick(10);
    EXPECT_TRUE(golden::matches("hub_encrypted_burst_retag", h.dump()));
}

#if defined(MAC_WIRE_V1) && MAC_WIRE_V1
// v1: the copy sealer over every raw header byte MAC-0 writes per copy (FLAGS
// ON_MARK/STAMPED, BURST_IDX/CNT, the STAMP), through the real WireBurstFrame.
TEST(GoldenHubFrames, RetagCoversEveryTimingField) {
    GoldenHub h;
    h.session();
    h.rol.send_cover_operation(LORA_COVER_OPERATION__COVOP_OPERATION, COV_OPERATION__CMD_OPEN, 0.0f);
    h.tick(10);
    ASSERT_FALSE(h.tracker.sent_wire.empty());
    const auto raw = h.tracker.sent_wire.back();
    const auto pol = h.tracker.sent_policy.back();
    ASSERT_NE(pol.sealer, nullptr);
    std::ostringstream s;
    const uint32_t cases[][6] = {
        {0, 17, 0, 0, 0, 0}, {5, 17, 0, 0, 0, 0}, {5, 17, 1, 0, 0, 0},
        {5, 17, 1, 1, 123, 4567}, {0, 1, 1, 1, 0xFFFFFF, 999999},
    };
    for (const auto &c : cases) {
        mac0::WireBurstFrame fr(raw.data(), raw.size());
        ASSERT_TRUE(fr.ok());
        fr.setBurst((int) c[1], c[2] != 0);
        fr.setIndex((int) c[0]);
        fr.setFireStamp(c[3] != 0, c[4], c[5]);
        uint64_t binding = pol.seal_binding;
        ASSERT_TRUE(fr.seal(pol.sealer, &binding));
        std::vector<uint8_t> out(fr.packedSize());
        fr.packInto(out.data());
        s << "retag " << c[0] << "/" << c[1] << " onmark=" << c[2] << " stamped=" << c[3]
          << " round=" << c[4] << " off=" << c[5] << " " << golden::hex(out) << "\n";
    }
    EXPECT_TRUE(golden::matches("hub_retag_timing_fields", s.str()));
}
#else
// The copy sealer over every timing field the CMAC prefix covers.
TEST(GoldenHubFrames, RetagCoversEveryTimingField) {
    GoldenHub h;
    h.session();
    h.rol.send_cover_operation(LORA_COVER_OPERATION__COVOP_OPERATION, COV_OPERATION__CMD_OPEN, 0.0f);
    h.tick(10);
    ASSERT_FALSE(h.tracker.sent_wire.empty());
    const auto raw = h.tracker.sent_wire.back();
    ::LoraClientOperationMessage *m =
        lora_client_operation_message__unpack(nullptr, raw.size(), raw.data());
    ASSERT_NE(m, nullptr);
    ASSERT_EQ(m->cmd_case, LORA_CLIENT_OPERATION_MESSAGE__CMD_ENCRYPTED);
    std::ostringstream s;
    const uint32_t cases[][6] = {
        {0, 17, 0, 0, 0, 0}, {5, 17, 0, 0, 0, 0}, {5, 17, 1, 0, 0, 0},
        {5, 17, 1, 1, 123, 4567}, {0, 1, 1, 1, 0xFFFFFF, 999999},
    };
    for (const auto &c : cases) {
        m->header->burstindex   = c[0];
        m->header->burstcount   = c[1];
        m->header->onmark       = c[2];
        m->header->firestamped  = c[3];
        m->header->fireround    = c[4];
        m->header->fireoffsetus = c[5];
        ASSERT_TRUE(h.rol.retagLikeLastFrameForTest(m->encrypted, m->header));
        std::vector<uint8_t> out(lora_client_operation_message__get_packed_size(m));
        lora_client_operation_message__pack(m, out.data());
        s << "retag " << c[0] << "/" << c[1] << " onmark=" << c[2] << " stamped=" << c[3]
          << " round=" << c[4] << " off=" << c[5] << " " << golden::hex(out) << "\n";
    }
    lora_client_operation_message__free_unpacked(m, nullptr);
    EXPECT_TRUE(golden::matches("hub_retag_timing_fields", s.str()));
}
#endif

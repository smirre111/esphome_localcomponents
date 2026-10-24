// S8: the hub side of MAC wire format v1 (docs/mac-wire-format.md).
//
// Part 1:
//   * HubWire: routing, body packing, cmd_id, the uplink decode into the
//     hub-internal view (HubFrames.h);
//   * conformance with the node-owned shared vectors (MacWireVectors.h): the
//     hub's own framing + MAC-0 restamp + copy sealer produce the vector bytes
//     and tags, and the hub's verify/decrypt/decode accept the vector uplinks;
//   * the sim's independent mbedtls implementation agrees with the vectors too.
// Part 2 drives the real listener end to end over v1.

#include "esphome/components/lora_client/lora_client.h"
#include "esphome/components/lora_tracker/lora_tracker.h"
#include "esphome/components/homeassistant/time/homeassistant_time.h"

#include "blinds.pb-c.h"
#include "HubWire.h"
#include "HubLinkSecurity.h"
#include "MacHdrCodec.h"
#include "MacWire.h"
#include "MacWireVectors.h"
#include "DeliveryPolicy.h"
#include "../../../local_components/lora_tracker/burst_frame.h"

#include "sim/sim_clock.h"
#include "sim/sim_radio.h"
#include "sim/crypto.h"

#include <psa/crypto.h>
#include <gtest/gtest.h>

#include <cstring>
#include <string>
#include <vector>

using esphome::lora_tracker::LORAClient;
using esphome::lora_tracker::LORATracker;
using esphome::time::RealTimeClock;
namespace mv = macwire_vectors;

namespace {

constexpr uint64_t kMac = 0xE08CFE5F9EC4ULL;

using Bytes = std::vector<uint8_t>;
Bytes B(const uint8_t *p, size_t n) { return Bytes(p, p + n); }

const mv::Vector &vec(const char *name) {
    for (size_t i = 0; i < mv::kVectorCount; ++i)
        if (std::string(mv::kVectors[i].name) == name)
            return mv::kVectors[i];
    ADD_FAILURE() << "no vector " << name;
    return mv::kVectors[0];
}

psa_key_id_t importKey(const uint8_t k[16], bool cmac) {
    EXPECT_EQ(psa_crypto_init(), PSA_SUCCESS);
    psa_key_attributes_t a = PSA_KEY_ATTRIBUTES_INIT;
    if (cmac) {
        psa_set_key_usage_flags(&a, PSA_KEY_USAGE_SIGN_MESSAGE | PSA_KEY_USAGE_VERIFY_MESSAGE);
        psa_set_key_algorithm(&a, PSA_ALG_AT_LEAST_THIS_LENGTH_MAC(PSA_ALG_CMAC, 8));
    } else {
        psa_set_key_usage_flags(&a, PSA_KEY_USAGE_ENCRYPT | PSA_KEY_USAGE_DECRYPT);
        psa_set_key_algorithm(&a, PSA_ALG_CTR);
    }
    psa_set_key_type(&a, PSA_KEY_TYPE_AES);
    psa_set_key_bits(&a, 128);
    psa_key_id_t id = PSA_KEY_ID_NULL;
    EXPECT_EQ(psa_import_key(&a, k, 16, &id), PSA_SUCCESS);
    return id;
}

mac2::KeyPair vectorKeys() { return {importKey(mv::kKEnc, false), importKey(mv::kKMac, true)}; }

// The vector session (K_enc/K_mac, session id) installed for the node, as a
// LOGIN + session-opening uplink would leave it. Forgotten on destruction.
struct VectorSession {
    explicit VectorSession(uint8_t peer = mv::kNodeAddr) : peer_(peer) {
        auto &st = mac2::LinkSecurity::hub().store();
        st.setSessionId(peer, mv::kSessionId);
        st.install(peer, mv::kSessionId, vectorKeys());
    }
    ~VectorSession() { mac2::LinkSecurity::hub().store().forget(peer_); }
    uint8_t peer_;
};

// The vector K_auth swapped in for the hub's derived one, swapped back after.
struct VectorAuthKey {
    VectorAuthKey() : mine_(importKey(mv::kKAuth, true)) {
        old_ = mac2::LinkSecurity::hub().swapAuthKeyForTest(mine_);
    }
    ~VectorAuthKey() {
        mac2::LinkSecurity::hub().swapAuthKeyForTest(old_);
        psa_destroy_key(mine_);
    }
    psa_key_id_t mine_, old_{PSA_KEY_ID_NULL};
};

// The protected v1 framing of the listener, and the Control slot (cmd_id).
struct V1Probe : LORAClient {
    using esphome::lora_tracker::LORAListener::v1_frame_;
    using esphome::lora_tracker::LORAListener::v1_login_frame_;
    using esphome::lora_tracker::LORAListener::v1_demote_frame_;
    using esphome::lora_tracker::LORAListener::ctl;
};
V1Probe &P(LORAClient &r) { return static_cast<V1Probe &>(r); }

struct Hub {
    proto_sim::SimClock clock;
    proto_sim::SimRadio radio;
    LORATracker tracker;
    LORAClient rol;
    RealTimeClock time;
    Hub(uint8_t addr = mv::kNodeAddr, uint8_t subnet = 0x02) {
        EXPECT_EQ(psa_crypto_init(), PSA_SUCCESS);
        esphome::shim_hooks::set_active_clock(&clock);
        esphome::shim_hooks::reset_nvs();
        esphome::lora_tracker::shim_hooks::set_active_radio(&radio);
        rol.set_name("rol");
        rol.set_short_address(addr);
        rol.set_subnet_address(subnet);
        rol.set_sleep_duration(21600);
        rol.set_address(kMac);
        time.set_now(0, true);
        rol.set_time(&time);
        tracker.register_client(&rol);
    }
    ~Hub() {
        esphome::lora_tracker::shim_hooks::set_active_radio(nullptr);
        esphome::shim_hooks::set_active_clock(nullptr);
    }
};

// MAC-0's per-copy work, with the vector's copy position and stamp: the real
// WireBurstFrame and the frame's own sealer (exactly what sendPacketBurst does).
Bytes restampLike(const Bytes &frame, const mv::Vector &v, const mac2::FrameSeal &seal) {
    mac0::WireBurstFrame fr(frame.data(), frame.size());
    EXPECT_TRUE(fr.ok());
    fr.setBurst(v.burst_cnt, (v.flags & macwire::kFlagOnMark) != 0);
    fr.setIndex(v.burst_idx);
    if (v.flags & macwire::kFlagStamped)
        fr.setFireStamp(true, v.fire_round, v.fire_offset_us);
    uint64_t binding = seal.binding();
    if (fr.encrypted())
        EXPECT_TRUE(fr.seal(&mac2::LinkSecurity::copySealerV1, &binding));
    Bytes out(fr.packedSize());
    fr.packInto(out.data());
    return out;
}

} // namespace

// ===========================================================================
// HubWire
// ===========================================================================

TEST(HubWireRoute, EveryAckedDeliveryAndOnlyThoseAskForAMacAck) {
    // Spec 2.5: ACK_REQ on every Acked delivery -- Control (cover op, sysop),
    // Config (ScheduleConfig), Mac (GridSync); never on a Fire push or a stream.
    struct Row { hubframes::DownCase c; uint8_t port; bool ack; };
    const Row rows[] = {
        {hubframes::DL_OPERATION,    macwire::kPortApp,      true},
        {hubframes::DL_SYSOP,        macwire::kPortApp,      true},
        {hubframes::DL_SCHEDULE,     macwire::kPortApp,      true},
        {hubframes::DL_CLIENTCONFIG, macwire::kPortApp,      false},
        {hubframes::DL_COVERCONFIG,  macwire::kPortApp,      false},
        {hubframes::DL_TIMESYNC,     macwire::kPortApp,      false},
        {hubframes::DL_GRIDSYNC,     macwire::kPortMac0Mgmt, true},
        {hubframes::DL_MODETEST,     macwire::kPortMac0Mgmt, false},
        {hubframes::DL_MACCONTROL,   macwire::kPortMac0Mgmt, false},
        {hubframes::DL_DRIFTTEST,    macwire::kPortMac0Mgmt, false},
        {hubframes::DL_GRIDBEACON,   macwire::kPortMac0Mgmt, false},
        {hubframes::DL_GRIDDEMOTE,   macwire::kPortMac0Mgmt, false},
    };
    for (const Row &r : rows) {
        const hubwire::Route got = hubwire::routeDownlink(r.c);
        EXPECT_TRUE(got.ok) << r.c;
        EXPECT_EQ(got.port, r.port) << r.c;
        EXPECT_EQ(got.ack_req, r.ack) << r.c;
    }
    // ...which is exactly what the delivery policies say (DeliveryPolicy.h).
    using deliverypolicy::Mode;
    struct P2 { hubframes::DownCase c; const deliverypolicy::Policy &p; };
    const P2 pol[] = {
        {hubframes::DL_OPERATION,    deliverypolicy::kCoverPolicy},
        {hubframes::DL_SYSOP,        deliverypolicy::kSysopPolicy},
        {hubframes::DL_SCHEDULE,     deliverypolicy::kSchedulePolicy},
        {hubframes::DL_GRIDSYNC,     deliverypolicy::kGridSyncPolicy},
        {hubframes::DL_TIMESYNC,     deliverypolicy::kTimeSyncPolicy},
        {hubframes::DL_CLIENTCONFIG, deliverypolicy::kConfigPushPolicy},
        {hubframes::DL_COVERCONFIG,  deliverypolicy::kConfigPushPolicy},
    };
    for (const P2 &x : pol)
        EXPECT_EQ(hubwire::routeDownlink(x.c).ack_req, x.p.mode == Mode::Acked) << x.c;
}

TEST(HubWireRoute, LoginAndNoCommandHaveNoAppOrMacBody) {
    EXPECT_FALSE(hubwire::routeDownlink(hubframes::DL_LOGIN).ok);
    EXPECT_FALSE(hubwire::routeDownlink(hubframes::DL_NOT_SET).ok);
}

TEST(HubWireBody, ACoverOpCarriesItsCmdIdAndTheCallersMessageIsUntouched) {
    hubframes::Down op{};
    LoraCoverOperation covop = LORA_COVER_OPERATION__INIT;
    covop.covop_case = LORA_COVER_OPERATION__COVOP_POSITION;
    covop.position   = 0.25f;
    op.cmd_case  = hubframes::DL_OPERATION;
    op.operation = &covop;
    uint8_t buf[64];
    size_t len = 0;
    ASSERT_TRUE(hubwire::packDownlinkBody(&op, 7, buf, sizeof(buf), &len));
    const mv::Vector &v = vec("cover_op");
    EXPECT_EQ(B(buf, len), B(v.body, v.body_len))
        << "the vector's body: HubAppPdu.operation{position 0.25, cmd_id 7}";
    EXPECT_EQ(covop.cmd_id, 0u) << "the caller's message must not be modified";
}

TEST(HubWireBody, ASysopBecomesSysopCmdWithItsCmdId) {
    hubframes::Down op{};
    op.cmd_case = hubframes::DL_SYSOP;
    op.sysop    = CLIENT_OPERATION__CMD_OTA;
    uint8_t buf[32];
    size_t len = 0;
    ASSERT_TRUE(hubwire::packDownlinkBody(&op, 300, buf, sizeof(buf), &len));
    const mv::Vector &v = vec("sysop_ota");
    EXPECT_EQ(B(buf, len), B(v.body, v.body_len));
}

TEST(HubWireBody, AMacCommandIsAHubMacPduAndAFireCommandHasNoCmdId) {
    hubframes::Down op{};
    MacControl mc = MAC_CONTROL__INIT;
    mc.kind = MAC_CONTROL__KIND__MAC_PING;
    mc.seq = 75;
    mc.wantecho = true;
    op.cmd_case   = hubframes::DL_MACCONTROL;
    op.maccontrol = &mc;
    uint8_t buf[32];
    size_t len = 0;
    ASSERT_TRUE(hubwire::packDownlinkBody(&op, 999, buf, sizeof(buf), &len));
    const mv::Vector &v = vec("mac_ping_plain");
    EXPECT_EQ(B(buf, len), B(v.body, v.body_len)) << "cmd_id is ignored outside Control";

    TimeSync ts = TIME_SYNC__INIT;
    op.cmd_case = hubframes::DL_TIMESYNC;
    op.timesync = &ts;
    ASSERT_TRUE(hubwire::packDownlinkBody(&op, 999, buf, sizeof(buf), &len));
    HubAppPdu *app = hub_app_pdu__unpack(nullptr, len, buf);
    ASSERT_NE(app, nullptr);
    EXPECT_EQ(app->pdu_case, HUB_APP_PDU__PDU_TIMESYNC);
    hub_app_pdu__free_unpacked(app, nullptr);
}

TEST(HubWireBody, RefusesWhatHasNoBodyOrDoesNotFit) {
    hubframes::Down op{};
    uint8_t buf[4];
    size_t len = 0;
    EXPECT_FALSE(hubwire::packDownlinkBody(&op, 0, buf, sizeof(buf), &len)) << "no command";
    LoraCoverOperation covop = LORA_COVER_OPERATION__INIT;
    covop.covop_case = LORA_COVER_OPERATION__COVOP_POSITION;
    covop.position   = 0.5f;
    op.cmd_case  = hubframes::DL_OPERATION;
    op.operation = &covop;
    EXPECT_FALSE(hubwire::packDownlinkBody(&op, 7, buf, sizeof(buf), &len)) << "4 B is too small";
    op.operation = nullptr;
    uint8_t big[64];
    EXPECT_FALSE(hubwire::packDownlinkBody(&op, 7, big, sizeof(big), &len)) << "no operation";
}

TEST(HubWireAssemble, HeaderBodyAndTagRoomOrNothing) {
    macwire::WireHdr h;
    h.flags = macwire::withAuth(macwire::kFlagAckReq, macwire::Auth::Session);
    h.dst = 0x12; h.src = 1; h.subnet = 2; h.fcnt = 9; h.port = macwire::kPortApp;
    const uint8_t body[3] = {1, 2, 3};
    Bytes f;
    ASSERT_TRUE(hubwire::assemble(h, body, sizeof(body), &f));
    ASSERT_EQ(f.size(), 12u + 3u + 8u);
    EXPECT_EQ(f[0], macwire::kVersion);
    EXPECT_EQ(Bytes(f.begin() + 12, f.begin() + 15), B(body, 3));
    EXPECT_EQ(Bytes(f.end() - 8, f.end()), Bytes(8, 0)) << "tag room, zero until sealed";
    Bytes big(250, 0xAB);
    EXPECT_FALSE(hubwire::assemble(h, big.data(), big.size(), &f)) << "past 255 B";
    h.port = 9;
    EXPECT_FALSE(hubwire::assemble(h, body, 3, &f)) << "not encodable";
}

TEST(HubWireUplink, EachPortDecodesAsItsOwnMessage) {
    {   // PORT 2: NodeAppPdu.position
        const mv::Vector &v = vec("position_uplink");
        macwire::WireHdr h;
        h.port = macwire::kPortApp; h.src = 0x12; h.fcnt = 10;
        hubwire::Uplink u;
        ASSERT_TRUE(u.parse(h, v.body, v.body_len));
        ASSERT_EQ(u.view()->proto_case, hubframes::UL_POSITION);
        EXPECT_FLOAT_EQ(u.view()->position->position, 1.0f);
        EXPECT_EQ(u.view()->header->senderaddress, 0x12u);
        EXPECT_EQ(u.view()->header->msgid, 10u) << "FCNT is the msgid";
    }
    {   // PORT 0: MacAck needs FLAGS.ACK; ACKED_FCNT becomes ack_msg_id
        const mv::Vector &v = vec("mac_ack");
        macwire::WireHdr h;
        h.port = macwire::kPortMac0Mgmt;
        hubwire::Uplink u;
        EXPECT_FALSE(u.parse(h, v.body, v.body_len)) << "a MacAck without FLAGS.ACK";
        h.flags = macwire::kFlagAck;
        h.acked_fcnt = 7;
        ASSERT_TRUE(u.parse(h, v.body, v.body_len));
        ASSERT_EQ(u.view()->proto_case, hubframes::UL_ACK);
        EXPECT_EQ(u.view()->ack->ack_msg_id, 7u);
        ASSERT_NE(u.view()->ack->phase, nullptr) << "the phase report still rides on the ack";
    }
    {   // PORT 1: ClientRegister
        const mv::Vector &v = vec("register");
        macwire::WireHdr h;
        h.port = macwire::kPortSession;
        hubwire::Uplink u;
        ASSERT_TRUE(u.parse(h, v.body, v.body_len));
        ASSERT_EQ(u.view()->proto_case, hubframes::UL_REGISTER);
        EXPECT_TRUE(u.view()->register_->needs_config);
    }
}

TEST(HubWireUplink, AHubBodyOrGarbageOrAnEmptyCaseIsRefused) {
    macwire::WireHdr h;
    h.port = macwire::kPortApp;
    hubwire::Uplink u;
    const uint8_t garbage[] = {0xFF, 0xFF, 0xFF};
    EXPECT_FALSE(u.parse(h, garbage, sizeof(garbage)));
    EXPECT_FALSE(u.parse(h, nullptr, 0)) << "an empty NodeAppPdu has no case";
    h.port = macwire::kPortMac0Mgmt;
    const mv::Vector &gs = vec("grid_sync_copy3");   // a HubMacPdu: case 21 is not a node case
    EXPECT_FALSE(u.parse(h, gs.body, gs.body_len));
}

TEST(HubWireUplink, TheSessionOpeningNonceReachesTheHeaderView) {
    macwire::WireHdr h;
    h.flags = macwire::kFlagSessionOpening;
    h.node_nonce = 0x0BADCAFE;
    hubframes::Hdr lh;
    hubwire::toHdr(h, &lh);
    EXPECT_EQ(lh.sessionnonce, 0x0BADCAFEu);
    h.flags = 0;
    hubwire::toHdr(h, &lh);
    EXPECT_EQ(lh.sessionnonce, 0u) << "only a SESSION_OPENING frame carries it";
}

// ===========================================================================
// Shared vectors: the hub produces the same bytes and tags
// ===========================================================================

TEST(MacWireVectorsHub, LoginIsFramedAndKAuthTaggedPerCopy) {
    VectorAuthKey auth;
    Hub hub;
    const mv::Vector &v = vec("login");
    LoginMsg *login = login_msg__unpack(nullptr, v.body_len, v.body);
    ASSERT_NE(login, nullptr);
    esphome::lora_tracker::DeliveryFrame f;
    ASSERT_TRUE(P(hub.rol).v1_login_frame_(*login, v.fcnt, &f));
    login_msg__free_unpacked(login, nullptr);
    EXPECT_TRUE(f.seal.sealed && f.seal.kauth) << "MAC-0 must recompute the tag per copy";
    EXPECT_EQ(restampLike(f.bytes, v, f.seal), B(v.frame, v.frame_len));
}

TEST(MacWireVectorsHub, CoverOpSealedStampedOnMark) {
    VectorSession s;
    Hub hub;
    const mv::Vector &v = vec("cover_op");
    P(hub.rol).ctl().generation = 7;   // cmd_id = the Control slot's generation
    hubframes::Down op{};
    LoraCoverOperation covop = LORA_COVER_OPERATION__INIT;
    covop.covop_case = LORA_COVER_OPERATION__COVOP_POSITION;
    covop.position   = 0.25f;
    op.cmd_case  = hubframes::DL_OPERATION;
    op.operation = &covop;
    esphome::lora_tracker::DeliveryFrame f;
    ASSERT_TRUE(P(hub.rol).v1_frame_(&op, v.fcnt, /*encrypt=*/true, &f));
    EXPECT_TRUE(f.seal.sealed);
    EXPECT_FALSE(f.seal.kauth);
    // As packed (before any restamp) the frame already carries a valid tag.
    uint8_t tag[8];
    proto_sim::v1_tag(mv::kKMac, true, mv::kSessionId, f.bytes.data(), f.bytes.size() - 8, tag);
    EXPECT_EQ(B(tag, 8), Bytes(f.bytes.end() - 8, f.bytes.end()));
    EXPECT_EQ(restampLike(f.bytes, v, f.seal), B(v.frame, v.frame_len));
}

TEST(MacWireVectorsHub, SysopOtaCarriesItsCmdId) {
    VectorSession s;
    Hub hub;
    const mv::Vector &v = vec("sysop_ota");
    P(hub.rol).ctl().generation = 300;
    hubframes::Down op{};
    op.cmd_case = hubframes::DL_SYSOP;
    op.sysop    = CLIENT_OPERATION__CMD_OTA;
    esphome::lora_tracker::DeliveryFrame f;
    ASSERT_TRUE(P(hub.rol).v1_frame_(&op, v.fcnt, true, &f));
    EXPECT_EQ(restampLike(f.bytes, v, f.seal), B(v.frame, v.frame_len));
}

TEST(MacWireVectorsHub, GridSyncCopyThreeOfABurst) {
    VectorSession s;
    Hub hub;
    const mv::Vector &v = vec("grid_sync_copy3");
    HubMacPdu *pdu = hub_mac_pdu__unpack(nullptr, v.body_len, v.body);
    ASSERT_NE(pdu, nullptr);
    ASSERT_EQ(pdu->pdu_case, HUB_MAC_PDU__PDU_GRIDSYNC);
    hubframes::Down op{};
    op.cmd_case = hubframes::DL_GRIDSYNC;
    op.gridsync = pdu->gridsync;
    esphome::lora_tracker::DeliveryFrame f;
    ASSERT_TRUE(P(hub.rol).v1_frame_(&op, v.fcnt, true, &f));
    hub_mac_pdu__free_unpacked(pdu, nullptr);
    EXPECT_EQ(restampLike(f.bytes, v, f.seal), B(v.frame, v.frame_len));
}

TEST(MacWireVectorsHub, PlaintextPingAndDemoteAreUntagged) {
    Hub hub;
    {
        const mv::Vector &v = vec("mac_ping_plain");
        hubframes::Down op{};
        MacControl mc = MAC_CONTROL__INIT;
        mc.kind = MAC_CONTROL__KIND__MAC_PING;
        mc.seq = 75;
        mc.wantecho = true;
        op.cmd_case   = hubframes::DL_MACCONTROL;
        op.maccontrol = &mc;
        esphome::lora_tracker::DeliveryFrame f;
        ASSERT_TRUE(P(hub.rol).v1_frame_(&op, v.fcnt, /*encrypt=*/false, &f));
        EXPECT_FALSE(f.seal.sealed);
        EXPECT_EQ(f.bytes, B(v.frame, v.frame_len)) << "as packed: burst 0/0, no tag";
    }
    {
        const mv::Vector &v = vec("grid_demote");
        esphome::lora_tracker::DeliveryFrame f;
        ASSERT_TRUE(P(hub.rol).v1_demote_frame_(&f));
        EXPECT_EQ(restampLike(f.bytes, v, f.seal), B(v.frame, v.frame_len));
    }
}

TEST(MacWireVectorsHub, NoSessionMeansPlaintextAndASpentFcntIsRefused) {
    Hub hub;
    hubframes::Down op{};
    TimeSync ts = TIME_SYNC__INIT;
    op.cmd_case = hubframes::DL_TIMESYNC;
    op.timesync = &ts;
    esphome::lora_tracker::DeliveryFrame f;
    ASSERT_TRUE(P(hub.rol).v1_frame_(&op, 5, /*encrypt=*/true, &f));
    EXPECT_EQ(macwire::authOf(f.bytes[1]), macwire::Auth::None)
        << "no session id for the node: plaintext, the pre-session rule";

    VectorSession s;
    hub.rol.clearMsgidReloginForTest();
    EXPECT_FALSE(P(hub.rol).v1_frame_(&op, macwire::kFcntLimit, true, &f));
    EXPECT_TRUE(f.bytes.empty());
    EXPECT_TRUE(hub.rol.msgidReloginPendingForTest()) << "FCNT bit 31: a fresh session";
}

TEST(MacWireVectorsHub, UplinksVerifyDecryptAndDecode) {
    auto &sec = mac2::LinkSecurity::hub();
    mac2::KeyPair kp = vectorKeys();
    for (const char *name : {"session_opening_beacon", "mac_ack", "position_uplink"}) {
        const mv::Vector &v = vec(name);
        ASSERT_EQ(sec.verifyUplinkV1(mv::kSessionId, kp, v.frame, v.frame_len),
                  mac2::LinkSecurity::Verify::Ok) << name;
        macwire::WireHdr h;
        size_t off = 0, blen = 0;
        ASSERT_TRUE(macwire::decode(v.frame, v.frame_len, &h, &off, &blen));
        mac::MacHdr mh;
        machdr::fromWireHdr(h, &mh);
        Bytes pt(blen);
        ASSERT_TRUE(sec.decryptUplink(mh, mv::kSessionId, kp, v.frame + off, blen, pt.data()));
        EXPECT_EQ(pt, B(v.body, v.body_len)) << name;
        hubwire::Uplink u;
        EXPECT_TRUE(u.parse(h, pt.data(), pt.size())) << name;

        Bytes bad = B(v.frame, v.frame_len);
        bad[macwire::kOffFcnt + 3] ^= 1;   // any header byte is covered
        EXPECT_EQ(sec.verifyUplinkV1(mv::kSessionId, kp, bad.data(), bad.size()),
                  mac2::LinkSecurity::Verify::BadTag) << name;
        EXPECT_EQ(sec.verifyUplinkV1(mv::kSessionId + 1, kp, v.frame, v.frame_len),
                  mac2::LinkSecurity::Verify::BadTag) << "the session id binds the tag";
    }
    mac2::LinkSecurity::destroy(kp);
}

TEST(MacWireVectorsHub, RegisterVerifiesUnderKAuthAndOnlyAsAnUplink) {
    VectorAuthKey auth;
    auto &sec = mac2::LinkSecurity::hub();
    const mv::Vector &v = vec("register");
    EXPECT_TRUE(sec.verifyAuthFrameV1(/*downlink=*/false, v.frame, v.frame_len));
    EXPECT_FALSE(sec.verifyAuthFrameV1(/*downlink=*/true, v.frame, v.frame_len))
        << "the direction byte binds the tag";
    Bytes bad = B(v.frame, v.frame_len);
    bad[12] ^= 0x80;   // a body byte
    EXPECT_FALSE(sec.verifyAuthFrameV1(false, bad.data(), bad.size()));
    const mv::Vector &l = vec("login");
    EXPECT_TRUE(sec.verifyAuthFrameV1(true, l.frame, l.frame_len));
    const mv::Vector &p = vec("position_uplink");
    EXPECT_FALSE(sec.verifyAuthFrameV1(false, p.frame, p.frame_len)) << "AUTH = SESSION is not K_AUTH";
}

TEST(MacWireVectorsHub, ACopyForAnotherNodeOrAfterReloginIsNotTagged) {
    VectorSession s;
    Hub hub;
    const mv::Vector &v = vec("cover_op");
    hubframes::Down op{};
    LoraCoverOperation covop = LORA_COVER_OPERATION__INIT;
    covop.covop_case = LORA_COVER_OPERATION__COVOP_OPERATION;
    op.cmd_case  = hubframes::DL_OPERATION;
    op.operation = &covop;
    esphome::lora_tracker::DeliveryFrame f;
    ASSERT_TRUE(P(hub.rol).v1_frame_(&op, v.fcnt, true, &f));
    uint64_t binding = f.seal.binding();
    mac::MacHdr copy;
    copy.dst = 0x13;
    copy.fcnt = v.fcnt;
    uint8_t tag[8];
    EXPECT_FALSE(mac2::LinkSecurity::copySealerV1(&binding, copy, f.bytes.data(), f.bytes.size() - 8, tag))
        << "a copy addressed elsewhere is never this frame's";
    copy.dst = mv::kNodeAddr;
    EXPECT_TRUE(mac2::LinkSecurity::copySealerV1(&binding, copy, f.bytes.data(), f.bytes.size() - 8, tag));
    mac2::LinkSecurity::hub().store().install(mv::kNodeAddr, mv::kSessionId, vectorKeys());
    EXPECT_FALSE(mac2::LinkSecurity::copySealerV1(&binding, copy, f.bytes.data(), f.bytes.size() - 8, tag))
        << "keys moved on (F4): the frame is stale";
    EXPECT_FALSE(mac2::LinkSecurity::copySealerV1(nullptr, copy, f.bytes.data(), 1, tag));
}

// The seal and retag refusals, ported from the v0 characterization (S8 / W5:
// packOperationForTest / retagLikeLastFrameForTest went with the v0 sealer).
namespace {
hubframes::Down timeSyncOp(TimeSync *ts) {
    *ts = TIME_SYNC__INIT;
    ts->epoch = 1787000000;
    hubframes::Down op{};
    op.cmd_case = hubframes::DL_TIMESYNC;
    op.timesync = ts;
    return op;
}
// A sealed frame for the vector node and the binding MAC-0 hands back per copy.
struct SealedFrame {
    esphome::lora_tracker::DeliveryFrame f;
    mac::MacHdr copy;
    uint8_t tag[8];
    explicit SealedFrame(LORAClient &rol, uint32_t fcnt = 999) {
        TimeSync ts;
        hubframes::Down op = timeSyncOp(&ts);
        EXPECT_TRUE(P(rol).v1_frame_(&op, fcnt, true, &f));
        copy.dst  = mv::kNodeAddr;
        copy.fcnt = fcnt;
        memset(tag, 0xAA, sizeof(tag));
    }
    mac2::LinkSecurity::Retag retag() {
        return mac2::LinkSecurity::hub().retagFrameV1(f.seal, copy, f.bytes.data(),
                                                      f.bytes.size() - 8, tag);
    }
    bool untouched() const {
        for (uint8_t b : tag) if (b != 0xAA) return false;
        return true;
    }
};
}  // namespace

TEST(MacWireV1Seal, ASpentFcntIsRefusedOnlyWhenSealing) {
    VectorSession s;
    Hub hub;
    TimeSync ts;
    hubframes::Down op = timeSyncOp(&ts);
    esphome::lora_tracker::DeliveryFrame f;
    hub.rol.clearMsgidReloginForTest();
    EXPECT_FALSE(P(hub.rol).v1_frame_(&op, 0x80000005u, true, &f)) << "2^31 + 5 would reuse FCNT 5's keystream";
    EXPECT_TRUE(f.bytes.empty());
    EXPECT_TRUE(hub.rol.msgidReloginPendingForTest());

    // The same FCNT in the clear is not a CTR problem (no keystream).
    hub.rol.clearMsgidReloginForTest();
    ASSERT_TRUE(P(hub.rol).v1_frame_(&op, 0x80000005u, /*encrypt=*/false, &f));
    EXPECT_EQ(macwire::authOf(f.bytes[1]), macwire::Auth::None);
    EXPECT_FALSE(hub.rol.msgidReloginPendingForTest());
}

TEST(MacWireV1Seal, NoKeysOrASessionIdOfZeroSealsNothingAndAsksForNothing) {
    Hub hub;
    TimeSync ts;
    hubframes::Down op = timeSyncOp(&ts);
    esphome::lora_tracker::DeliveryFrame f;
    auto &st = mac2::LinkSecurity::hub().store();
    {
        VectorSession s;
        st.clearKeys(mv::kNodeAddr);   // the session id stays; K_enc/K_mac are gone
        hub.rol.clearMsgidReloginForTest();
        EXPECT_FALSE(P(hub.rol).v1_frame_(&op, 5, true, &f));
        EXPECT_TRUE(f.bytes.empty()) << "never sent half-sealed, never downgraded";
        EXPECT_FALSE(hub.rol.msgidReloginPendingForTest()) << "not an FCNT-space refusal";
    }
    {
        VectorSession s;
        const psa_key_id_t saved = st.liveKeys(mv::kNodeAddr).mac;
        st.setMacKeyForTest(mv::kNodeAddr, PSA_KEY_ID_NULL);   // keys held, but not a whole pair
        EXPECT_FALSE(P(hub.rol).v1_frame_(&op, 6, true, &f));
        EXPECT_TRUE(f.bytes.empty()) << "half a key pair seals nothing";
        st.setMacKeyForTest(mv::kNodeAddr, saved);
    }
    {
        VectorSession s;
        st.setSessionId(mv::kNodeAddr, 0);   // an entry, but zero: found, then refused
        EXPECT_FALSE(P(hub.rol).v1_frame_(&op, 5, true, &f));
        EXPECT_TRUE(f.bytes.empty());
    }
}

TEST(MacWireV1Seal, TheCopyTagIsExactlyTheCmacOverTheCopysBytes) {
    VectorSession s;
    Hub hub;
    SealedFrame sf(hub.rol);
    ASSERT_TRUE(sf.f.seal.sealed);
    ASSERT_EQ(sf.retag(), mac2::LinkSecurity::Retag::Ok);
    uint8_t want[8];
    proto_sim::v1_tag(mv::kKMac, true, mv::kSessionId, sf.f.bytes.data(), sf.f.bytes.size() - 8, want);
    EXPECT_EQ(B(sf.tag, 8), B(want, 8));
    // Every byte is covered: one header byte and one body byte move the tag.
    for (size_t i : {(size_t) macwire::kOffBurstIdx, sf.f.bytes.size() - 9}) {
        SealedFrame c(hub.rol);
        c.f.bytes[i] ^= 1;
        ASSERT_EQ(c.retag(), mac2::LinkSecurity::Retag::Ok);
        EXPECT_NE(B(c.tag, 8), B(want, 8)) << "byte " << i;
    }
}

TEST(MacWireV1Seal, EveryRetagRefusalLeavesTheTagBytesAlone) {
    using R = mac2::LinkSecurity::Retag;
    VectorSession s;
    Hub hub;
    auto &st = mac2::LinkSecurity::hub().store();
    {
        SealedFrame c(hub.rol);
        c.copy.fcnt = macwire::kFcntLimit;
        const uint32_t before = st.staleDrops(mv::kNodeAddr);
        EXPECT_EQ(c.retag(), R::MsgidSpent);
        EXPECT_TRUE(c.untouched());
        EXPECT_EQ(st.staleDrops(mv::kNodeAddr), before) << "refused for the FCNT, not as stale";
    }
    {   // no MAC key installed: not stale, not tagged, not counted
        SealedFrame c(hub.rol);
        const psa_key_id_t saved = st.liveKeys(mv::kNodeAddr).mac;
        const uint32_t before = st.staleDrops(mv::kNodeAddr);
        st.setMacKeyForTest(mv::kNodeAddr, PSA_KEY_ID_NULL);
        EXPECT_EQ(c.retag(), R::Failed);
        st.setMacKeyForTest(mv::kNodeAddr, saved);
        EXPECT_TRUE(c.untouched());
        EXPECT_EQ(st.staleDrops(mv::kNodeAddr), before);
    }
    {   // a MAC key id PSA does not know: psa_mac_compute fails, nothing is written
        SealedFrame c(hub.rol);
        const psa_key_id_t saved = st.liveKeys(mv::kNodeAddr).mac;
        st.setMacKeyForTest(mv::kNodeAddr, static_cast<psa_key_id_t>(0x7FFFFFF0u));
        EXPECT_EQ(c.retag(), R::Failed);
        st.setMacKeyForTest(mv::kNodeAddr, saved);
        EXPECT_TRUE(c.untouched());
    }
    {   // keys moved on since the seal (a re-login mid-burst): stale, counted
        SealedFrame c(hub.rol);
        const uint32_t before = st.staleDrops(mv::kNodeAddr);
        st.install(mv::kNodeAddr, mv::kSessionId, vectorKeys());
        EXPECT_EQ(c.retag(), R::Stale);
        EXPECT_TRUE(c.untouched());
        EXPECT_EQ(st.staleDrops(mv::kNodeAddr), before + 1) << "counted as a stale-session drop";
        EXPECT_EQ(hub.rol.staleSessionDropsForTest(), before + 1) << "and visible on the listener";
    }
    {   // the peer's session is gone altogether
        SealedFrame c(hub.rol);
        st.forget(mv::kNodeAddr);
        EXPECT_EQ(c.retag(), R::NoSession);
        EXPECT_TRUE(c.untouched());
    }
}

// ===========================================================================
// WireBurstFrame (MAC-0's per-copy work)
// ===========================================================================

TEST(WireBurstFrame, TouchesOnlyTheDocumentedOffsets) {
    const mv::Vector &v = vec("sysop_ota");   // unstamped, SESSION
    Bytes in = B(v.frame, v.frame_len);
    mac0::WireBurstFrame fr(in.data(), in.size());
    ASSERT_TRUE(fr.ok());
    ASSERT_TRUE(fr.encrypted());
    EXPECT_EQ(fr.msgid(), v.fcnt);
    fr.setBurst(9, true);
    fr.setIndex(4);
    Bytes out(fr.packedSize());
    fr.packInto(out.data());
    ASSERT_EQ(out.size(), in.size());
    for (size_t i = 0; i < in.size(); ++i) {
        if (i == macwire::kOffFlags || i == macwire::kOffBurstIdx || i == macwire::kOffBurstCnt)
            continue;
        EXPECT_EQ(out[i], in[i]) << "byte " << i;
    }
    EXPECT_EQ(out[macwire::kOffFlags], in[macwire::kOffFlags] | macwire::kFlagOnMark);
    EXPECT_EQ(out[macwire::kOffBurstIdx], 4);
    EXPECT_EQ(out[macwire::kOffBurstCnt], 9);
}

TEST(WireBurstFrame, TheStampIsInsertedOnceBeforeTheBodyAndCanBeTakenOut) {
    const mv::Vector &v = vec("sysop_ota");
    Bytes in = B(v.frame, v.frame_len);
    mac0::WireBurstFrame fr(in.data(), in.size());
    fr.setFireStamp(true, 0x01ABCDEF, 140625);   // the round is compared modulo 2^24
    ASSERT_TRUE(fr.ok());
    Bytes out(fr.packedSize());
    fr.packInto(out.data());
    ASSERT_EQ(out.size(), in.size() + macwire::kStampBytes);
    macwire::WireHdr h;
    size_t off = 0, blen = 0;
    ASSERT_TRUE(macwire::decode(out.data(), out.size(), &h, &off, &blen));
    EXPECT_TRUE(h.stamped());
    EXPECT_EQ(h.fire_round, 0xABCDEFu);
    EXPECT_EQ(h.fire_offset_us, 140625u);
    EXPECT_EQ(Bytes(out.begin() + off, out.end()), Bytes(in.begin() + 12, in.end()))
        << "body and tag move unchanged";
    fr.setFireStamp(true, 5, 6);   // same layout: no second insertion
    EXPECT_EQ(fr.packedSize(), in.size() + macwire::kStampBytes);
    fr.setFireStamp(false, 0, 0);
    Bytes back(fr.packedSize());
    fr.packInto(back.data());
    EXPECT_EQ(back, in);
}

TEST(WireBurstFrame, AFleetOrNonV1FrameIsNotTouched) {
    const mv::Vector &b = vec("grid_beacon");
    mac0::WireBurstFrame fleet(b.frame, b.frame_len);
    EXPECT_FALSE(fleet.ok()) << "MAC-0 holds no fleet key: sent verbatim";
    const uint8_t not_v1[] = {0x0A, 0x02, 0x08, 0x01};   // VER is not 0x01
    mac0::WireBurstFrame old(not_v1, sizeof(not_v1));
    EXPECT_FALSE(old.ok());
    Bytes big(256, 0x01);
    mac0::WireBurstFrame toolong(big.data(), big.size());
    EXPECT_FALSE(toolong.ok());
}

TEST(WireBurstFrame, AFrameWithNoRoomForTheStampIsDropped) {
    macwire::WireHdr h;
    h.dst = 0x12; h.src = 1; h.port = macwire::kPortApp;
    Bytes body(255 - 12, 0x55);
    Bytes f;
    ASSERT_TRUE(hubwire::assemble(h, body.data(), body.size(), &f));
    mac0::WireBurstFrame fr(f.data(), f.size());
    ASSERT_TRUE(fr.ok());
    fr.setFireStamp(true, 1, 2);
    EXPECT_FALSE(fr.ok());
}

TEST(WireBurstFrame, ASealerRefusalDropsTheCopy) {
    const mv::Vector &v = vec("sysop_ota");
    mac0::WireBurstFrame fr(v.frame, v.frame_len);
    uint64_t binding = 0;
    EXPECT_FALSE(fr.seal(nullptr, &binding));
    EXPECT_FALSE(fr.seal(&mac2::LinkSecurity::copySealerV1, &binding)) << "an unsealed binding";
}

// ===========================================================================
// The sim's independent implementation agrees with the vectors
// ===========================================================================

TEST(MacWireVectorsSim, EveryTagFromAnIndependentImplementation) {
    for (size_t i = 0; i < mv::kVectorCount; ++i) {
        const mv::Vector &v = mv::kVectors[i];
        if (v.auth == 0)
            continue;
        uint8_t tag[8];
        proto_sim::v1_tag(v.key, v.downlink, v.mac_id, v.frame, v.frame_len - 8, tag);
        EXPECT_EQ(B(tag, 8), B(v.tag, v.tag_len)) << v.name;
        if (v.auth != 1)
            continue;
        Bytes ct(v.body_len);
        proto_sim::v1_ctr(mv::kKEnc, mv::kSessionId, mv::kNodeAddr, v.downlink, v.fcnt, v.body,
                          v.body_len, ct.data());
        EXPECT_EQ(ct, B(v.frame + v.header_len, v.body_len)) << v.name;
    }
}

// ===========================================================================
// Part 2: the real listener, end to end over v1
// ===========================================================================

namespace {

constexpr uint32_t kNodeNonce = 0xACE55001u;
constexpr uint8_t  kNode = 0x12;
constexpr uint8_t  kHub  = 0x01;

// The last hub->node frame on the air.
Bytes lastDown(const proto_sim::SimRadio &r) {
    for (auto it = r.transcript().rbegin(); it != r.transcript().rend(); ++it)
        if (it->dir == proto_sim::AirFrame::Dir::HubToNode)
            return it->bytes;
    return {};
}

macwire::WireHdr hdrOf(const Bytes &f) {
    macwire::WireHdr h;
    size_t off = 0, blen = 0;
    EXPECT_TRUE(macwire::decode(f.data(), f.size(), &h, &off, &blen));
    return h;
}

Bytes availBody() {
    ClientAvailable a = CLIENT_AVAILABLE__INIT;
    a.available = true;
    NodeAppPdu pdu = NODE_APP_PDU__INIT;
    pdu.pdu_case = NODE_APP_PDU__PDU_AVAIL;
    pdu.avail = &a;
    Bytes b(node_app_pdu__get_packed_size(&pdu));
    node_app_pdu__pack(&pdu, b.data());
    return b;
}

Bytes ackBody() {
    MacAck ack = MAC_ACK__INIT;
    NodeMacPdu pdu = NODE_MAC_PDU__INIT;
    pdu.pdu_case = NODE_MAC_PDU__PDU_ACK;
    pdu.ack = &ack;
    Bytes b(node_mac_pdu__get_packed_size(&pdu));
    node_mac_pdu__pack(&pdu, b.data());
    return b;
}

macwire::WireHdr uplinkHdr(uint32_t fcnt, uint8_t port, macwire::Auth a, uint8_t extra = 0) {
    macwire::WireHdr h;
    h.flags = macwire::withAuth(extra, a);
    h.dst = kHub; h.src = kNode; h.subnet = 0x02; h.fcnt = fcnt; h.port = port;
    return h;
}

// LOGIN, then the node's session-opening uplink under the keys the sim derives
// from the LOGIN's nonce: the hub's session is then confirmed.
uint32_t openSession(Hub &hub) {
    hub.rol.registered_    = true;   // a provisioned node (REGISTER is its own test)
    hub.rol.config_synced_ = true;   // ...whose config was pushed (no follow-up re-login)
    hub.rol.send_login();
    const Bytes login = lastDown(hub.radio);
    LoginMsg *lm = nullptr;
    {
        macwire::WireHdr h;
        size_t off = 0, blen = 0;
        EXPECT_TRUE(macwire::decode(login.data(), login.size(), &h, &off, &blen));
        lm = login_msg__unpack(nullptr, blen, login.data() + off);
    }
    EXPECT_NE(lm, nullptr);
    const uint32_t session = lm ? lm->nonce : 0;
    if (lm) login_msg__free_unpacked(lm, nullptr);
    macwire::WireHdr h = uplinkHdr(1, macwire::kPortApp, macwire::Auth::Session,
                                   macwire::kFlagSessionOpening);
    h.node_nonce = kNodeNonce;
    Bytes up = proto_sim::v1_session_frame(session, kNodeNonce, kHub, kNode, false, h, availBody());
    hub.rol.set_response(up.data(), up.size());
    return session;
}

} // namespace

TEST(MacWireV1Listener, LoginIsKAuthOnTheSessionPortAndVerifiesAtTheNode) {
    Hub hub;
    hub.rol.send_login();
    const Bytes f = lastDown(hub.radio);
    ASSERT_FALSE(f.empty());
    const macwire::WireHdr h = hdrOf(f);
    EXPECT_EQ(h.auth(), macwire::Auth::KAuth);
    EXPECT_EQ(h.port, macwire::kPortSession);
    EXPECT_EQ(h.dst, kNode);
    EXPECT_TRUE(proto_sim::v1_verify_kauth_frame(/*downlink=*/true, f))
        << "the sim's own K_auth implementation accepts the hub's tag";
}

TEST(MacWireV1Listener, TheSessionOpeningUplinkConfirmsTheSession) {
    Hub hub;
    openSession(hub);
    EXPECT_TRUE(hub.rol.session_confirmed_);
    EXPECT_TRUE(hub.rol.login_acked_);
}

TEST(MacWireV1Listener, ATamperedOrV0UplinkConfirmsNothing) {
    Hub hub;
    hub.rol.registered_ = true;
    hub.rol.send_login();
    const Bytes login = lastDown(hub.radio);
    macwire::WireHdr lh;
    size_t off = 0, blen = 0;
    ASSERT_TRUE(macwire::decode(login.data(), login.size(), &lh, &off, &blen));
    LoginMsg *lm = login_msg__unpack(nullptr, blen, login.data() + off);
    ASSERT_NE(lm, nullptr);
    const uint32_t session = lm->nonce;
    login_msg__free_unpacked(lm, nullptr);

    macwire::WireHdr h = uplinkHdr(1, macwire::kPortApp, macwire::Auth::Session,
                                   macwire::kFlagSessionOpening);
    h.node_nonce = kNodeNonce;
    Bytes up = proto_sim::v1_session_frame(session, kNodeNonce, kHub, kNode, false, h, availBody());
    up[up.size() - 1] ^= 0x01;
    hub.rol.set_response(up.data(), up.size());
    EXPECT_FALSE(hub.rol.session_confirmed_) << "a bad tag";

    Bytes v0 = {0x0A, 0x04, 0x18, 0x12, 0x20, 0x01, 0x52, 0x02, 0x08, 0x01};
    hub.rol.set_response(v0.data(), v0.size());
    EXPECT_FALSE(hub.rol.session_confirmed_) << "a v0 frame is not a v1 frame";
}

TEST(MacWireV1Listener, ACoverOpIsSealedAsksForAnAckAndCarriesItsCmdId) {
    Hub hub;
    const uint32_t session = openSession(hub);
    ASSERT_TRUE(hub.rol.session_confirmed_);
    ASSERT_TRUE(hub.rol.send_cover_operation(LORA_COVER_OPERATION__COVOP_POSITION, 0, 0.75f));
    const Bytes f = lastDown(hub.radio);
    macwire::WireHdr h;
    auto pt = proto_sim::v1_open_session_frame(session, kNodeNonce, kHub, kNode, true, f, &h);
    ASSERT_TRUE(pt.has_value()) << "the node's keys open the hub's frame";
    EXPECT_TRUE(h.ackReq());
    EXPECT_EQ(h.port, macwire::kPortApp);
    HubAppPdu *app = hub_app_pdu__unpack(nullptr, pt->size(), pt->data());
    ASSERT_NE(app, nullptr);
    ASSERT_EQ(app->pdu_case, HUB_APP_PDU__PDU_OPERATION);
    EXPECT_FLOAT_EQ(app->operation->position, 0.75f);
    EXPECT_EQ(app->operation->cmd_id, P(hub.rol).ctl().generation);
    EXPECT_NE(app->operation->cmd_id, 0u);
    hub_app_pdu__free_unpacked(app, nullptr);
    EXPECT_TRUE(hub.rol.awaitingAck());

    // The MAC ACK: FLAGS.ACK + ACKED_FCNT = the command's FCNT, MacAck body.
    macwire::WireHdr ah = uplinkHdr(2, macwire::kPortMac0Mgmt, macwire::Auth::Session, macwire::kFlagAck);
    ah.acked_fcnt = h.fcnt;
    Bytes ack = proto_sim::v1_session_frame(session, kNodeNonce, kHub, kNode, false, ah, ackBody());
    hub.rol.set_response(ack.data(), ack.size());
    EXPECT_FALSE(hub.rol.awaitingAck()) << "the delivery service matched ACKED_FCNT";
}

TEST(MacWireV1Listener, AnAckNamingAnotherFcntCompletesNothing) {
    Hub hub;
    const uint32_t session = openSession(hub);
    ASSERT_TRUE(hub.rol.send_cover_operation(LORA_COVER_OPERATION__COVOP_POSITION, 0, 0.5f));
    const macwire::WireHdr h = hdrOf(lastDown(hub.radio));
    macwire::WireHdr ah = uplinkHdr(2, macwire::kPortMac0Mgmt, macwire::Auth::Session, macwire::kFlagAck);
    ah.acked_fcnt = h.fcnt + 100;
    Bytes ack = proto_sim::v1_session_frame(session, kNodeNonce, kHub, kNode, false, ah, ackBody());
    hub.rol.set_response(ack.data(), ack.size());
    EXPECT_TRUE(hub.rol.awaitingAck());
}

TEST(MacWireV1Listener, AnAuthenticUplinkPastTheFcntSpaceAsksForAFreshSession) {
    Hub hub;
    const uint32_t session = openSession(hub);
    hub.rol.clearMsgidReloginForTest();
    // The node's counter is at the edge (the replay window admits the step).
    hub.rol.setCountersForTest(3, macwire::kFcntLimit - 1);
    macwire::WireHdr h = uplinkHdr(macwire::kFcntLimit + 5, macwire::kPortApp, macwire::Auth::Session);
    Bytes up = proto_sim::v1_session_frame(session, kNodeNonce, kHub, kNode, false, h, availBody());
    hub.rol.set_response(up.data(), up.size());
    EXPECT_TRUE(hub.rol.msgidReloginPendingForTest());
}

TEST(MacWireV1Listener, RegisterIsAcceptedOnlyUnderKAuth) {
    Hub hub;
    ClientRegister reg = CLIENT_REGISTER__INIT;
    reg.mac_addr = kMac;
    reg.needs_config = true;
    Bytes body(client_register__get_packed_size(&reg));
    client_register__pack(&reg, body.data());
    macwire::WireHdr h = uplinkHdr(1, macwire::kPortSession, macwire::Auth::KAuth);
    h.dst = macwire::kBroadcast;

    Bytes forged = proto_sim::v1_kauth_frame(false, h, body);
    forged[forged.size() - 2] ^= 0x10;
    hub.rol.set_response(forged.data(), forged.size());
    EXPECT_FALSE(hub.rol.registered_) << "a forged REGISTER";

    macwire::WireHdr plain = uplinkHdr(1, macwire::kPortSession, macwire::Auth::None);
    Bytes unauth = proto_sim::v1_plain_frame(plain, body);
    hub.rol.set_response(unauth.data(), unauth.size());
    EXPECT_FALSE(hub.rol.registered_) << "PORT 1 without K_AUTH";

    Bytes good = proto_sim::v1_kauth_frame(false, h, body);
    hub.rol.set_response(good.data(), good.size());
    EXPECT_TRUE(hub.rol.registered_);
}

TEST(MacWireV1Listener, APlaintextUplinkBeforeTheSessionIsHeard) {
    Hub hub;
    const uint32_t before = hub.rol.last_heard_epoch_;
    hub.time.set_now(1000, true);
    Bytes up = proto_sim::v1_plain_frame(uplinkHdr(1, macwire::kPortApp, macwire::Auth::None), availBody());
    hub.rol.set_response(up.data(), up.size());
    EXPECT_FALSE(hub.rol.session_confirmed_);
    (void) before;
}

namespace {
struct CaptureNode : esphome::lora_tracker::LORAClientNode {
    void send_remote_config() override {}
    void on_app_indication(const esphome::lora_tracker::AppIndication &ind) override {
        ++count;
        last_case = ind.msg ? ind.msg->proto_case : hubframes::UL_NOT_SET;
        if (ind.msg && ind.msg->proto_case == hubframes::UL_POSITION)
            position = ind.msg->position->position;
        authentic = ind.authentic;
        src = ind.hdr.src;
    }
    int count{0};
    hubframes::UpCase last_case{hubframes::UL_NOT_SET};
    float position{-1};
    bool authentic{false};
    uint8_t src{0};
};

Bytes positionBody(float pos) {
    CoverPosition p = COVER_POSITION__INIT;
    p.position = pos;
    NodeAppPdu pdu = NODE_APP_PDU__INIT;
    pdu.pdu_case = NODE_APP_PDU__PDU_POSITION;
    pdu.position = &p;
    Bytes b(node_app_pdu__get_packed_size(&pdu));
    node_app_pdu__pack(&pdu, b.data());
    return b;
}
}  // namespace

TEST(MacWireV1Listener, AnAuthenticPositionReachesTheChildAsAnAppIndication) {
    Hub hub;
    CaptureNode child;
    hub.rol.register_lora_node(&child);
    const uint32_t session = openSession(hub);
    child.count = 0;
    macwire::WireHdr h = uplinkHdr(2, macwire::kPortApp, macwire::Auth::Session);
    Bytes up = proto_sim::v1_session_frame(session, kNodeNonce, kHub, kNode, false, h, positionBody(0.4f));
    hub.rol.set_response(up.data(), up.size());
    ASSERT_EQ(child.count, 1);
    EXPECT_EQ(child.last_case, hubframes::UL_POSITION);
    EXPECT_FLOAT_EQ(child.position, 0.4f);
    EXPECT_TRUE(child.authentic);
    EXPECT_EQ(child.src, kNode);

    // A MAC frame never reaches the application.
    macwire::WireHdr ah = uplinkHdr(3, macwire::kPortMac0Mgmt, macwire::Auth::Session, macwire::kFlagAck);
    ah.acked_fcnt = 77;
    Bytes ack = proto_sim::v1_session_frame(session, kNodeNonce, kHub, kNode, false, ah, ackBody());
    hub.rol.set_response(ack.data(), ack.size());
    EXPECT_EQ(child.count, 1);

    // A session frame on PORT 1 means nothing and is dropped after the tag.
    macwire::WireHdr sh = uplinkHdr(4, macwire::kPortSession, macwire::Auth::Session);
    Bytes s1 = proto_sim::v1_session_frame(session, kNodeNonce, kHub, kNode, false, sh, positionBody(0.9f));
    hub.rol.set_response(s1.data(), s1.size());
    EXPECT_EQ(child.count, 1);
}

TEST(MacWireV1Listener, ARetryResendsTheSameFcntAndCmdId) {
    Hub hub;
    const uint32_t session = openSession(hub);
    ASSERT_TRUE(hub.rol.send_cover_operation(LORA_COVER_OPERATION__COVOP_POSITION, 0, 0.3f));
    const Bytes first = lastDown(hub.radio);
    macwire::WireHdr h1;
    auto p1 = proto_sim::v1_open_session_frame(session, kNodeNonce, kHub, kNode, true, first, &h1);
    ASSERT_TRUE(p1);
    const size_t n_before = hub.radio.transcript().size();
    hub.clock.tick(6000);   // past the first ack wait: the delivery service retries
    // Other traffic (a TimeSync) may interleave; the retry is the frame that
    // carries the command's FCNT again.
    int retries = 0;
    for (size_t i = n_before; i < hub.radio.transcript().size(); ++i) {
        macwire::WireHdr h2;
        auto p2 = proto_sim::v1_open_session_frame(session, kNodeNonce, kHub, kNode, true,
                                                   hub.radio.transcript()[i].bytes, &h2);
        if (!p2 || h2.fcnt != h1.fcnt)
            continue;
        ++retries;
        EXPECT_EQ(*p2, *p1) << "pack once: same FCNT, same body (and so the same cmd_id)";
    }
    EXPECT_GE(retries, 1) << "a retry went out under the command's FCNT";
}

TEST(MacWireV1Listener, TwoControlCommandsNeverShareACmdId) {
    // The node executes a cmd_id once (it remembers the last one until a LOGIN
    // with hub_rebooted): a second, different command under the same cmd_id
    // would be acked and silently NOT executed.
    Hub hub;
    const uint32_t session = openSession(hub);
    auto cmdIdOfLast = [&](HubAppPdu__PduCase want) -> uint32_t {
        auto pt = proto_sim::v1_open_session_frame(session, kNodeNonce, kHub, kNode, true,
                                                   lastDown(hub.radio));
        if (!pt) return 0;
        HubAppPdu *app = hub_app_pdu__unpack(nullptr, pt->size(), pt->data());
        uint32_t id = 0;
        if (app && app->pdu_case == want)
            id = want == HUB_APP_PDU__PDU_SYSOP ? app->sysop->cmd_id : app->operation->cmd_id;
        if (app) hub_app_pdu__free_unpacked(app, nullptr);
        return id;
    };
    ASSERT_TRUE(hub.rol.send_cover_operation(LORA_COVER_OPERATION__COVOP_POSITION, 0, 0.1f));
    const uint32_t a = cmdIdOfLast(HUB_APP_PDU__PDU_OPERATION);
    macwire::WireHdr ah = uplinkHdr(2, macwire::kPortMac0Mgmt, macwire::Auth::Session, macwire::kFlagAck);
    ah.acked_fcnt = hdrOf(lastDown(hub.radio)).fcnt;
    Bytes ack = proto_sim::v1_session_frame(session, kNodeNonce, kHub, kNode, false, ah, ackBody());
    hub.rol.set_response(ack.data(), ack.size());
    ASSERT_FALSE(hub.rol.awaitingAck());
    hub.rol.triggerOTA();
    const uint32_t b = cmdIdOfLast(HUB_APP_PDU__PDU_SYSOP);
    EXPECT_NE(a, 0u);
    EXPECT_NE(b, 0u);
    EXPECT_NE(a, b) << "a different command, a different cmd_id";
}

TEST(MacWireV1Listener, AConfigPushAfterRegisterIsAV1AppFrame) {
    Hub hub;
    ClientRegister reg = CLIENT_REGISTER__INIT;
    reg.mac_addr = kMac;
    reg.needs_config = true;
    Bytes body(client_register__get_packed_size(&reg));
    client_register__pack(&reg, body.data());
    macwire::WireHdr h = uplinkHdr(1, macwire::kPortSession, macwire::Auth::KAuth);
    h.dst = macwire::kBroadcast;
    const size_t n_before = hub.radio.transcript().size();
    Bytes good = proto_sim::v1_kauth_frame(false, h, body);
    hub.rol.set_response(good.data(), good.size());
    ASSERT_GT(hub.radio.transcript().size(), n_before);
    bool saw_config = false;
    for (size_t i = n_before; i < hub.radio.transcript().size(); ++i) {
        const Bytes &f = hub.radio.transcript()[i].bytes;
        macwire::WireHdr fh;
        size_t off = 0, blen = 0;
        ASSERT_TRUE(macwire::decode(f.data(), f.size(), &fh, &off, &blen)) << "every downlink is v1";
        if (fh.port != macwire::kPortApp || fh.auth() != macwire::Auth::None)
            continue;
        HubAppPdu *app = hub_app_pdu__unpack(nullptr, blen, f.data() + off);
        ASSERT_NE(app, nullptr);
        saw_config |= app->pdu_case == HUB_APP_PDU__PDU_CLIENTCONFIG;
        EXPECT_FALSE(fh.ackReq()) << "a config push is Fire";
        hub_app_pdu__free_unpacked(app, nullptr);
    }
    EXPECT_TRUE(saw_config) << "unprovisioned: ClientConfig in the clear";
}

TEST(MacWireV1Listener, AnAuthenticV1FramesStampIsTheNodesUplinkOrigin) {
    // Spec 4.2: the fixed header has no varints and the tag covers the raw
    // bytes, so the on-air length is a T0 input as it stands (the MAC-1/MAC-2
    // gates on timing use are unchanged); there is no re-encode check to pass.
    Hub hub;
    const uint32_t session = openSession(hub);
    hub.tracker.last_rx_t0_us_v = 123'456;
    macwire::WireHdr h = uplinkHdr(2, macwire::kPortApp, macwire::Auth::Session);
    Bytes up = proto_sim::v1_session_frame(session, kNodeNonce, kHub, kNode, false, h, positionBody(0.2f));
    hub.rol.set_response(up.data(), up.size());
    EXPECT_EQ(hub.rol.last_uplink_t0_us_, 123'456) << "the frame's T0 stamp is committed as is";
}

TEST(MacWireV1Listener, TheStartupDemoteIsAPlainBroadcastOnPortZero) {
    Hub hub;
    hub.rol.broadcast_grid_demote();
    const macwire::WireHdr h = hdrOf(lastDown(hub.radio));
    EXPECT_EQ(h.auth(), macwire::Auth::None);
    EXPECT_EQ(h.dst, macwire::kBroadcast);
    EXPECT_EQ(h.port, macwire::kPortMac0Mgmt);
    EXPECT_EQ(h.fcnt, 0u);
}

// The sim codec: proto_sim::* (the tests' plain structs, messages.h) to and from
// MAC wire v1 frames (docs/mac-wire-format.md), built with the shared MacWire.h
// header and the generated body PDUs the production code compiles.
//
//   header        LoraHeader fields -> the fixed v1 header (FCNT = msgId, burst,
//                 ON_MARK, STAMP, SESSION_OPENING carries sessionNonce)
//   body          the one PDU (direction, PORT) names: HubAppPdu / LoginMsg
//                 downlink, NodeAppPdu / NodeMacPdu / ClientRegister uplink
//   Login / Register
//                 AUTH = K_AUTH, tagged with the sim's K_auth
//   Encrypted     AUTH = SESSION with the struct's ciphertext and tag as they
//                 are (the sim models' own AEAD); a frame the production code
//                 must accept is built with seal_resp / opened with open_op
//   everything else
//                 AUTH = NONE
//
// Schema drift still fails here first: every body goes through blinds.pb-c.

#include "sim/wire_codec.h"
#include "sim/crypto.h"

extern "C" {
#include "blinds.pb-c.h"
}

#include "sim/messages.h"
#include "MacWire.h"

#include <cstring>

namespace {

using Bytes = std::vector<uint8_t>;

void fill_phase(::PhaseReport &pb, const proto_sim::PhaseReport &m)
{
    phase_report__init(&pb);
    pb.samples        = m.samples;
    pb.errus          = m.errUs;
    pb.spreadus       = m.spreadUs;
    pb.outsideguard   = m.outsideGuard;
    pb.rtcslowsrc     = m.rtcSlowSrc;
    pb.ppmestimate    = m.ppmEstimate;
    pb.ppmsamples     = m.ppmSamples;
    pb.timedrxactive  = m.timedRxActive;
    pb.demotionreason = m.demotionReason;
}

void read_phase(proto_sim::PhaseReport &m, const ::PhaseReport *pb)
{
    m.samples        = pb->samples;
    m.errUs          = pb->errus;
    m.spreadUs       = pb->spreadus;
    m.outsideGuard   = pb->outsideguard;
    m.rtcSlowSrc     = pb->rtcslowsrc;
    m.ppmEstimate    = pb->ppmestimate;
    m.ppmSamples     = pb->ppmsamples;
    m.timedRxActive  = pb->timedrxactive;
    m.demotionReason = pb->demotionreason;
}

template <typename M, typename F, typename G>
Bytes pack(const M *m, F size_fn, G pack_fn)
{
    Bytes b(size_fn(m));
    if (!b.empty())
        pack_fn(m, b.data());
    return b;
}

macwire::WireHdr hdr_of(const proto_sim::LoraHeader &h)
{
    macwire::WireHdr w;
    w.dst       = (uint8_t) h.destAddress;
    w.src       = (uint8_t) h.senderAddress;
    w.subnet    = (uint8_t) h.destSubnet;
    w.fcnt      = h.msgId;
    w.burst_idx = (uint8_t) h.burstIndex;
    w.burst_cnt = (uint8_t) h.burstCount;
    if (h.onMark)
        w.flags |= macwire::kFlagOnMark;
    if (h.fireStamped)
    {
        w.flags |= macwire::kFlagStamped;
        w.fire_round     = h.fireRound & macwire::kU24Max;
        w.fire_offset_us = h.fireOffsetUs & macwire::kU24Max;
    }
    return w;
}

void hdr_from(const macwire::WireHdr &w, proto_sim::LoraHeader &h)
{
    h.destAddress   = w.dst;
    h.destSubnet    = w.subnet;
    h.senderAddress = w.src;
    h.msgId         = w.fcnt;
    h.burstIndex    = w.burst_idx;
    h.burstCount    = w.burst_cnt;
    h.onMark        = w.onMark();
    h.fireStamped   = w.stamped();
    h.fireRound     = w.fire_round;
    h.fireOffsetUs  = w.fire_offset_us;
    h.sessionNonce  = w.sessionOpening() ? w.node_nonce : 0;
}

// header || ciphertext || tag, as they are (the PORT / ACK the payload names).
Bytes raw_session_frame(macwire::WireHdr w, const proto_sim::EncryptedPayload &e)
{
    w.port = e.port;
    if (e.ack)
    {
        w.flags     |= macwire::kFlagAck;
        w.acked_fcnt = e.ackedFcnt;
    }
    w.flags = macwire::withAuth(w.flags, macwire::Auth::Session);
    Bytes f = proto_sim::v1_plain_frame(w, e.ciphertext);   // header + body, tag room after
    Bytes tag = e.tag;
    tag.resize(macwire::kTagBytes, 0);
    f.resize(macwire::headerLen(w) + e.ciphertext.size());
    f.insert(f.end(), tag.begin(), tag.end());
    return f;
}

// ---- downlink bodies ------------------------------------------------------------
struct OpStorage
{
    ::LoraCoverOperation          covop;
    ::SysopCmd                    sysop;
    ::ClientConfig                cc;
    ::CoverConfig                 cv;
    ::TimeSync                    ts;
    ::ScheduleConfig              sched;
    Bytes                         name;
    std::vector<::ScheduleEntry>  entries;
    std::vector<::ScheduleEntry*> ptrs;
};

bool app_pdu(const proto_sim::LoraClientOperationMessage &m, ::HubAppPdu &pdu, OpStorage &st)
{
    using Cmd = proto_sim::LoraClientOperationMessage::Cmd;
    hub_app_pdu__init(&pdu);
    switch (m.cmd)
    {
    case Cmd::Operation:
        lora_cover_operation__init(&st.covop);
        if (m.operation.kind == proto_sim::LoraCoverOperation::Kind::Operation)
        {
            st.covop.covop_case = LORA_COVER_OPERATION__COVOP_OPERATION;
            st.covop.operation  = static_cast<::CovOperation>(m.operation.operation);
        }
        else
        {
            st.covop.covop_case = LORA_COVER_OPERATION__COVOP_POSITION;
            st.covop.position   = m.operation.position;
        }
        pdu.pdu_case  = HUB_APP_PDU__PDU_OPERATION;
        pdu.operation = &st.covop;
        return true;
    case Cmd::Sysop:
        sysop_cmd__init(&st.sysop);
        st.sysop.op  = static_cast<::ClientOperation>(m.sysop);
        pdu.pdu_case = HUB_APP_PDU__PDU_SYSOP;
        pdu.sysop    = &st.sysop;
        return true;
    case Cmd::ClientConfig:
        client_config__init(&st.cc);
        st.cc.mac_addr = m.clientconfig.mac_addr;
        st.cc.addr     = m.clientconfig.addr;
        st.cc.subnt    = m.clientconfig.subnt;
        st.name.assign(m.clientconfig.name.begin(), m.clientconfig.name.end());
        st.cc.name.data       = st.name.data();
        st.cc.name.len        = st.name.size();
        st.cc.sleepduration   = m.clientconfig.sleepDuration;
        st.cc.batteryinterval = m.clientconfig.batteryInterval;
        pdu.pdu_case     = HUB_APP_PDU__PDU_CLIENTCONFIG;
        pdu.clientconfig = &st.cc;
        return true;
    case Cmd::CoverConfig:
        cover_config__init(&st.cv);
        st.cv.opentime         = m.coverconfig.openTime;
        st.cv.closetime        = m.coverconfig.closeTime;
        st.cv.blindheightmm    = m.coverconfig.blindHeightMm;
        st.cv.axlediametermm   = m.coverconfig.axleDiameterMm;
        st.cv.blindthicknessmm = m.coverconfig.blindThicknessMm;
        st.cv.openslack        = m.coverconfig.openSlack;
        st.cv.closeslack       = m.coverconfig.closeSlack;
        pdu.pdu_case    = HUB_APP_PDU__PDU_COVERCONFIG;
        pdu.coverconfig = &st.cv;
        return true;
    case Cmd::TimeSync:
        time_sync__init(&st.ts);
        st.ts.epoch     = m.timesync.epoch;
        st.ts.utcoffset = m.timesync.utcOffset;
        st.ts.dstnext   = m.timesync.dstNext;
        pdu.pdu_case = HUB_APP_PDU__PDU_TIMESYNC;
        pdu.timesync = &st.ts;
        return true;
    case Cmd::Schedule:
        schedule_config__init(&st.sched);
        st.sched.version              = m.schedule.version;
        st.sched.mode                 = static_cast<::NodeMode>(m.schedule.mode);
        st.sched.interactivetimeout_s = m.schedule.interactiveTimeout_s;
        st.sched.checkininterval_s    = m.schedule.checkinInterval_s;
        st.sched.beaconlead_s         = m.schedule.beaconLead_s;
        st.sched.posteventwindow_s    = m.schedule.postEventWindow_s;
        st.sched.catchupwindow_s      = m.schedule.catchupWindow_s;
        st.entries.resize(m.schedule.entries.size());
        st.ptrs.resize(m.schedule.entries.size());
        for (size_t i = 0; i < m.schedule.entries.size(); ++i)
        {
            schedule_entry__init(&st.entries[i]);
            st.entries[i].minuteofday = m.schedule.entries[i].minuteOfDay;
            st.entries[i].daymask     = m.schedule.entries[i].dayMask;
            st.entries[i].action      = static_cast<::SchedAction>(m.schedule.entries[i].action);
            st.entries[i].positionpct = m.schedule.entries[i].positionPct;
            st.entries[i].kind        = m.schedule.entries[i].kind;
            st.ptrs[i] = &st.entries[i];
        }
        st.sched.n_entries = st.ptrs.size();
        st.sched.entries   = st.ptrs.empty() ? nullptr : st.ptrs.data();
        pdu.pdu_case = HUB_APP_PDU__PDU_SCHEDULE;
        pdu.schedule = &st.sched;
        return true;
    default:
        return false;
    }
}

Bytes login_body(const proto_sim::LoginMsg &l)
{
    ::LoginMsg pb;
    login_msg__init(&pb);
    pb.nonce            = l.nonce;
    pb.request_register = l.request_register;
    pb.hub_rebooted     = l.hub_rebooted;
    return pack(&pb, login_msg__get_packed_size, login_msg__pack);
}

// ---- uplink bodies -----------------------------------------------------------------
// The body for `m`, and its PORT / ACK flag in *w. False: no v1 body.
bool resp_body(const proto_sim::LoraClientResponseMessage &m, macwire::WireHdr *w, Bytes *out)
{
    using Proto = proto_sim::LoraClientResponseMessage::Proto;
    ::NodeAppPdu app;
    node_app_pdu__init(&app);
    ::ClientAvailable av;
    ::ClientBattery   bat;
    ::CoverPosition   pos;
    ::NodeWakeBeacon  bc;
    ::PhaseReport     ph;
    switch (m.proto)
    {
    case Proto::Avail:
        client_available__init(&av);
        av.available = m.avail.available;
        app.pdu_case = NODE_APP_PDU__PDU_AVAIL;
        app.avail    = &av;
        break;
    case Proto::State:
        client_battery__init(&bat);
        bat.voltage  = m.state.voltage;
        app.pdu_case = NODE_APP_PDU__PDU_STATE;
        app.state    = &bat;
        break;
    case Proto::Position:
        cover_position__init(&pos);
        pos.position = m.position.position;
        pos.voltage  = m.position.voltage;
        pos.current  = m.position.current;
        app.pdu_case = NODE_APP_PDU__PDU_POSITION;
        app.position = &pos;
        break;
    case Proto::Beacon:
        node_wake_beacon__init(&bc);
        bc.reason         = static_cast<::WakeReason>(m.beacon.reason);
        bc.schedversion   = m.beacon.schedVersion;
        bc.nodeepoch      = m.beacon.nodeEpoch;
        bc.mode           = static_cast<::NodeMode>(m.beacon.mode);
        bc.voltage        = m.beacon.voltage;
        bc.position       = m.beacon.position;
        bc.awakewindow_ms = m.beacon.awakeWindow_ms;
        bc.nexteventepoch = m.beacon.nextEventEpoch;
        bc.sessionresume  = m.beacon.sessionResume;
        bc.clockvalid     = m.beacon.clockValid;
        bc.fwversion      = m.beacon.fwVersion;
        if (m.beacon.phasePresent)
        {
            fill_phase(ph, m.beacon.phase);
            bc.phase = &ph;
        }
        app.pdu_case = NODE_APP_PDU__PDU_BEACON;
        app.beacon   = &bc;
        break;
    case Proto::Ack:
    {
        ::MacAck ack;
        mac_ack__init(&ack);
        ack.status = static_cast<::AckStatus>(m.ack.status);
        if (m.ack.phasePresent)
        {
            fill_phase(ph, m.ack.phase);
            ack.phase = &ph;
        }
        ::NodeMacPdu mp;
        node_mac_pdu__init(&mp);
        mp.pdu_case = NODE_MAC_PDU__PDU_ACK;
        mp.ack      = &ack;
        w->port       = macwire::kPortMac0Mgmt;
        w->flags     |= macwire::kFlagAck;
        w->acked_fcnt = m.ack.ack_msg_id;
        *out = pack(&mp, node_mac_pdu__get_packed_size, node_mac_pdu__pack);
        return true;
    }
    case Proto::Register:
    {
        ::ClientRegister r;
        client_register__init(&r);
        r.mac_addr     = m.reg.mac_addr;
        r.needs_config = m.reg.needs_config;
        w->port = macwire::kPortSession;
        *out = pack(&r, client_register__get_packed_size, client_register__pack);
        return true;
    }
    case Proto::Login:
        // No such uplink in v1: a LoginMsg body on PORT 1 without K_AUTH, which
        // the hub refuses before decoding (the old "PROTO_LOGIN uplink").
        w->port = macwire::kPortSession;
        *out = login_body(m.login);
        return true;
    default:
        return false;
    }
    w->port = macwire::kPortApp;
    *out = pack(&app, node_app_pdu__get_packed_size, node_app_pdu__pack);
    return true;
}

}  // namespace

namespace proto_sim {

std::vector<uint8_t> serialize_op_payload(const LoraClientOperationMessage &m)
{
    if (m.cmd == LoraClientOperationMessage::Cmd::Login)
        return login_body(m.login);
    ::HubAppPdu pdu;
    OpStorage   st;
    if (!app_pdu(m, pdu, st))
        return {};
    return pack(&pdu, hub_app_pdu__get_packed_size, hub_app_pdu__pack);
}

std::vector<uint8_t> serialize_op(const LoraClientOperationMessage &m)
{
    using Cmd = LoraClientOperationMessage::Cmd;
    macwire::WireHdr w = hdr_of(m.header);
    if (m.cmd == Cmd::Encrypted)
    {
        return raw_session_frame(w, m.encrypted);
    }
    if (m.cmd == Cmd::Login)
    {
        w.flags = macwire::withAuth(w.flags, macwire::Auth::KAuth);
        w.port  = macwire::kPortSession;
        return v1_kauth_frame(/*downlink=*/true, w, login_body(m.login));
    }
    const bool ack_req = m.cmd == Cmd::Operation || m.cmd == Cmd::Sysop || m.cmd == Cmd::Schedule;
    if (ack_req)
        w.flags |= macwire::kFlagAckReq;
    w.port = macwire::kPortApp;
    return v1_plain_frame(w, serialize_op_payload(m));
}

std::vector<uint8_t> serialize_resp_payload(const LoraClientResponseMessage &m)
{
    macwire::WireHdr w;
    Bytes body;
    resp_body(m, &w, &body);
    return body;
}

std::vector<uint8_t> serialize_resp(const LoraClientResponseMessage &m)
{
    using Proto = LoraClientResponseMessage::Proto;
    macwire::WireHdr w = hdr_of(m.header);
    if (m.proto == Proto::Encrypted)
    {
        if (m.header.sessionNonce != 0)
        {
            w.flags |= macwire::kFlagSessionOpening;
            w.node_nonce = m.header.sessionNonce;
        }
        return raw_session_frame(w, m.encrypted);
    }
    Bytes body;
    if (!resp_body(m, &w, &body))
        w.port = macwire::kPortApp;   // NotSet: an empty application body
    if (m.proto == Proto::Register)
    {
        w.flags = macwire::withAuth(w.flags, macwire::Auth::KAuth);
        return v1_kauth_frame(/*downlink=*/false, w, body);
    }
    return v1_plain_frame(w, body);
}

namespace {
// An uplink frame sealed like a real node's: AUTH = SESSION, SESSION_OPENING
// when the header carries a sessionNonce.
Bytes sealed_uplink(macwire::WireHdr w, const LoraHeader &h, const Bytes &body,
                    uint32_t session_id, uint32_t node_nonce)
{
    if (h.sessionNonce != 0)
    {
        w.flags |= macwire::kFlagSessionOpening;
        w.node_nonce = h.sessionNonce;
    }
    w.flags = macwire::withAuth(w.flags, macwire::Auth::Session);
    return v1_session_frame(session_id, node_nonce, (uint8_t) h.destAddress,
                            (uint8_t) h.senderAddress, /*downlink=*/false, w, body);
}

EncryptedPayload split_sealed(const Bytes &f)
{
    macwire::WireHdr w;
    size_t off = 0, blen = 0;
    EncryptedPayload e;
    if (!macwire::decode(f.data(), f.size(), &w, &off, &blen))
        return e;
    e.ciphertext.assign(f.begin() + off, f.begin() + off + blen);
    e.tag.assign(f.end() - macwire::kTagBytes, f.end());
    e.port      = w.port;
    e.ack       = w.ack();
    e.ackedFcnt = w.acked_fcnt;
    return e;
}
}  // namespace

std::vector<uint8_t> seal_resp(const LoraClientResponseMessage &m, uint32_t session_id,
                               uint32_t node_nonce)
{
    macwire::WireHdr w = hdr_of(m.header);
    Bytes body;
    resp_body(m, &w, &body);
    return sealed_uplink(w, m.header, body, session_id, node_nonce);
}

EncryptedPayload seal_resp_payload(const LoraClientResponseMessage &m, uint32_t session_id,
                                   uint32_t node_nonce)
{
    return split_sealed(seal_resp(m, session_id, node_nonce));
}

EncryptedPayload seal_resp_body(const LoraHeader &h, const std::vector<uint8_t> &body, uint8_t port,
                                uint32_t session_id, uint32_t node_nonce)
{
    macwire::WireHdr w = hdr_of(h);
    w.port = port;
    return split_sealed(sealed_uplink(w, h, body, session_id, node_nonce));
}

// ---------------------------------------------------------------------------
// frame -> struct
// ---------------------------------------------------------------------------
namespace {
void op_from_app(const ::HubAppPdu *p, LoraClientOperationMessage &out)
{
    using Cmd = LoraClientOperationMessage::Cmd;
    switch (p->pdu_case)
    {
    case HUB_APP_PDU__PDU_OPERATION:
        out.cmd = Cmd::Operation;
        if (p->operation && p->operation->covop_case == LORA_COVER_OPERATION__COVOP_OPERATION)
        {
            out.operation.kind      = LoraCoverOperation::Kind::Operation;
            out.operation.operation = static_cast<CovOperation>(p->operation->operation);
        }
        else if (p->operation && p->operation->covop_case == LORA_COVER_OPERATION__COVOP_POSITION)
        {
            out.operation.kind     = LoraCoverOperation::Kind::Position;
            out.operation.position = p->operation->position;
        }
        break;
    case HUB_APP_PDU__PDU_SYSOP:
        out.cmd = Cmd::Sysop;
        if (p->sysop) out.sysop = static_cast<ClientOperation>(p->sysop->op);
        break;
    case HUB_APP_PDU__PDU_CLIENTCONFIG:
        out.cmd = Cmd::ClientConfig;
        if (p->clientconfig)
        {
            out.clientconfig.mac_addr = p->clientconfig->mac_addr;
            out.clientconfig.addr     = p->clientconfig->addr;
            out.clientconfig.subnt    = p->clientconfig->subnt;
            out.clientconfig.name.assign(reinterpret_cast<const char *>(p->clientconfig->name.data),
                                         p->clientconfig->name.len);
            out.clientconfig.sleepDuration   = p->clientconfig->sleepduration;
            out.clientconfig.batteryInterval = p->clientconfig->batteryinterval;
        }
        break;
    case HUB_APP_PDU__PDU_COVERCONFIG:
        out.cmd = Cmd::CoverConfig;
        if (p->coverconfig)
        {
            out.coverconfig.openTime         = p->coverconfig->opentime;
            out.coverconfig.closeTime        = p->coverconfig->closetime;
            out.coverconfig.blindHeightMm    = p->coverconfig->blindheightmm;
            out.coverconfig.axleDiameterMm   = p->coverconfig->axlediametermm;
            out.coverconfig.blindThicknessMm = p->coverconfig->blindthicknessmm;
            out.coverconfig.openSlack        = p->coverconfig->openslack;
            out.coverconfig.closeSlack       = p->coverconfig->closeslack;
        }
        break;
    case HUB_APP_PDU__PDU_TIMESYNC:
        out.cmd = Cmd::TimeSync;
        if (p->timesync)
        {
            out.timesync.epoch     = p->timesync->epoch;
            out.timesync.utcOffset = p->timesync->utcoffset;
            out.timesync.dstNext   = p->timesync->dstnext;
        }
        break;
    case HUB_APP_PDU__PDU_SCHEDULE:
        out.cmd = Cmd::Schedule;
        if (p->schedule)
        {
            out.schedule.version              = p->schedule->version;
            out.schedule.mode                 = static_cast<NodeMode>(p->schedule->mode);
            out.schedule.interactiveTimeout_s = p->schedule->interactivetimeout_s;
            out.schedule.checkinInterval_s    = p->schedule->checkininterval_s;
            out.schedule.beaconLead_s         = p->schedule->beaconlead_s;
            out.schedule.postEventWindow_s    = p->schedule->posteventwindow_s;
            out.schedule.catchupWindow_s      = p->schedule->catchupwindow_s;
            for (size_t i = 0; i < p->schedule->n_entries; ++i)
            {
                const ::ScheduleEntry *e = p->schedule->entries[i];
                ScheduleEntry se;
                se.minuteOfDay = e->minuteofday;
                se.dayMask     = e->daymask;
                se.action      = static_cast<SchedAction>(e->action);
                se.positionPct = e->positionpct;
                se.kind        = e->kind;
                out.schedule.entries.push_back(se);
            }
        }
        break;
    default:
        out.cmd = Cmd::NotSet;   // incl. a newer peer's case: a quiet fallthrough
        break;
    }
}

bool op_from_body(const macwire::WireHdr &w, const uint8_t *body, size_t len,
                  LoraClientOperationMessage &out)
{
    using Cmd = LoraClientOperationMessage::Cmd;
    if (w.port == macwire::kPortSession)
    {
        ::LoginMsg *l = login_msg__unpack(nullptr, len, body);
        if (!l) return false;
        out.cmd                    = Cmd::Login;
        out.login.nonce            = l->nonce;
        out.login.request_register = l->request_register;
        out.login.hub_rebooted     = l->hub_rebooted;
        login_msg__free_unpacked(l, nullptr);
        return true;
    }
    if (w.port == macwire::kPortApp)
    {
        ::HubAppPdu *p = hub_app_pdu__unpack(nullptr, len, body);
        if (!p) return false;
        op_from_app(p, out);
        hub_app_pdu__free_unpacked(p, nullptr);
        return true;
    }
    out.cmd = Cmd::NotSet;   // MAC management (GridSync, beacon, ...): not modelled
    return true;
}
}  // namespace

std::optional<LoraClientOperationMessage> deserialize_op(const uint8_t *data, size_t len)
{
    macwire::WireHdr w;
    size_t off = 0, blen = 0;
    if (data == nullptr || !macwire::decode(data, len, &w, &off, &blen))
        return std::nullopt;
    LoraClientOperationMessage out;
    hdr_from(w, out.header);
    if (w.auth() == macwire::Auth::Session)
    {
        out.cmd = LoraClientOperationMessage::Cmd::Encrypted;
        out.encrypted.port      = w.port;
        out.encrypted.ack       = w.ack();
        out.encrypted.ackedFcnt = w.acked_fcnt;
        out.encrypted.ciphertext.assign(data + off, data + off + blen);
        out.encrypted.tag.assign(data + len - macwire::kTagBytes, data + len);
        return out;
    }
    if (!op_from_body(w, data + off, blen, out))
        return std::nullopt;
    return out;
}

std::optional<LoraClientOperationMessage> open_op(const std::vector<uint8_t> &frame,
                                                  uint32_t session_id, uint32_t node_nonce)
{
    macwire::WireHdr w;
    auto pt = v1_open_session_frame(session_id, node_nonce, frame.size() > 3 ? frame[3] : 0,
                                    frame.size() > 2 ? frame[2] : 0, /*downlink=*/true, frame, &w);
    if (!pt)
        return std::nullopt;
    LoraClientOperationMessage out;
    hdr_from(w, out.header);
    if (!op_from_body(w, pt->data(), pt->size(), out))
        return std::nullopt;
    return out;
}

namespace {
bool resp_from_body(const macwire::WireHdr &w, const uint8_t *body, size_t len,
                    LoraClientResponseMessage &out)
{
    using Proto = LoraClientResponseMessage::Proto;
    if (w.port == macwire::kPortSession)
    {
        ::ClientRegister *r = client_register__unpack(nullptr, len, body);
        if (!r) return false;
        out.proto            = Proto::Register;
        out.reg.mac_addr     = r->mac_addr;
        out.reg.needs_config = r->needs_config;
        client_register__free_unpacked(r, nullptr);
        return true;
    }
    if (w.port == macwire::kPortMac0Mgmt)
    {
        ::NodeMacPdu *p = node_mac_pdu__unpack(nullptr, len, body);
        if (!p) return false;
        if (p->pdu_case == NODE_MAC_PDU__PDU_ACK && p->ack)
        {
            out.proto          = Proto::Ack;
            out.ack.ack_msg_id = w.ack() ? w.acked_fcnt : 0;
            out.ack.status     = static_cast<AckStatus>(p->ack->status);
            out.ack.phasePresent = p->ack->phase != nullptr;
            if (p->ack->phase) read_phase(out.ack.phase, p->ack->phase);
        }
        node_mac_pdu__free_unpacked(p, nullptr);
        return true;
    }
    ::NodeAppPdu *p = node_app_pdu__unpack(nullptr, len, body);
    if (!p) return false;
    switch (p->pdu_case)
    {
    case NODE_APP_PDU__PDU_AVAIL:
        out.proto = Proto::Avail;
        if (p->avail) out.avail.available = p->avail->available;
        break;
    case NODE_APP_PDU__PDU_STATE:
        out.proto = Proto::State;
        if (p->state) out.state.voltage = p->state->voltage;
        break;
    case NODE_APP_PDU__PDU_POSITION:
        out.proto = Proto::Position;
        if (p->position)
        {
            out.position.position = p->position->position;
            out.position.voltage  = p->position->voltage;
            out.position.current  = p->position->current;
        }
        break;
    case NODE_APP_PDU__PDU_BEACON:
        out.proto = Proto::Beacon;
        if (p->beacon)
        {
            out.beacon.reason         = static_cast<WakeReason>(p->beacon->reason);
            out.beacon.schedVersion   = p->beacon->schedversion;
            out.beacon.nodeEpoch      = p->beacon->nodeepoch;
            out.beacon.mode           = static_cast<NodeMode>(p->beacon->mode);
            out.beacon.voltage        = p->beacon->voltage;
            out.beacon.position       = p->beacon->position;
            out.beacon.awakeWindow_ms = p->beacon->awakewindow_ms;
            out.beacon.nextEventEpoch = p->beacon->nexteventepoch;
            out.beacon.sessionResume  = p->beacon->sessionresume;
            out.beacon.clockValid     = p->beacon->clockvalid;
            out.beacon.fwVersion      = p->beacon->fwversion;
            out.beacon.phasePresent   = p->beacon->phase != nullptr;
            if (p->beacon->phase) read_phase(out.beacon.phase, p->beacon->phase);
        }
        break;
    default:
        out.proto = Proto::NotSet;
        break;
    }
    node_app_pdu__free_unpacked(p, nullptr);
    return true;
}
}  // namespace

void route_payload(const LoraClientResponseMessage &inner, EncryptedPayload *e)
{
    macwire::WireHdr w;
    Bytes body;
    resp_body(inner, &w, &body);
    e->port      = w.port;
    e->ack       = w.ack();
    e->ackedFcnt = w.acked_fcnt;
}

std::optional<LoraClientResponseMessage> deserialize_resp_payload(const EncryptedPayload &e,
                                                                  const uint8_t *data, size_t len)
{
    macwire::WireHdr w;
    w.port = e.port;
    if (e.ack)
    {
        w.flags     |= macwire::kFlagAck;
        w.acked_fcnt = e.ackedFcnt;
    }
    LoraClientResponseMessage out;
    if (!resp_from_body(w, data, len, out))
        return std::nullopt;
    return out;
}

std::optional<LoraClientResponseMessage> deserialize_resp(const uint8_t *data, size_t len)
{
    macwire::WireHdr w;
    size_t off = 0, blen = 0;
    if (data == nullptr || !macwire::decode(data, len, &w, &off, &blen))
        return std::nullopt;
    LoraClientResponseMessage out;
    hdr_from(w, out.header);
    if (w.auth() == macwire::Auth::Session)
    {
        out.proto = LoraClientResponseMessage::Proto::Encrypted;
        out.encrypted.port      = w.port;
        out.encrypted.ack       = w.ack();
        out.encrypted.ackedFcnt = w.acked_fcnt;
        out.encrypted.ciphertext.assign(data + off, data + off + blen);
        out.encrypted.tag.assign(data + len - macwire::kTagBytes, data + len);
        return out;
    }
    if (!resp_from_body(w, data + off, blen, out))
        return std::nullopt;
    return out;
}

AirFrame make_op_frame(const LoraClientOperationMessage &m)
{
    return AirFrame{AirFrame::Dir::HubToNode, serialize_op(m)};
}

AirFrame make_resp_frame(const LoraClientResponseMessage &m)
{
    return AirFrame{AirFrame::Dir::NodeToHub, serialize_resp(m)};
}

std::optional<LoraClientOperationMessage> as_op(const AirFrame &f)
{
    if (f.dir != AirFrame::Dir::HubToNode) return std::nullopt;
    return deserialize_op(f.bytes.data(), f.bytes.size());
}

std::optional<LoraClientResponseMessage> as_resp(const AirFrame &f)
{
    if (f.dir != AirFrame::Dir::NodeToHub) return std::nullopt;
    return deserialize_resp(f.bytes.data(), f.bytes.size());
}

} // namespace proto_sim

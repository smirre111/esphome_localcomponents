// Phase-2 codec implementation. Translates proto_sim::* (our internal,
// ergonomic structs) to and from real protobuf-c wire bytes using the
// generated stubs the production firmware itself compiles.
//
// Catches schema drift: if blinds.proto, the generated blinds.pb-c.{h,c},
// or our internal struct shapes ever diverge, the tests fail at the
// codec layer instead of silently passing on hand-rolled structs.

#include "sim/wire_codec.h"

extern "C" {
#include "blinds.pb-c.h"
}

#include "sim/messages.h"

#include <cstring>

namespace proto_sim {

// ---------------------------------------------------------------------------
// Struct → proto-c (operation/hub→node)
// ---------------------------------------------------------------------------
static void fill_header_pb(const LoraHeader& src, ::LoraHeader& dst) {
    lora_header__init(&dst);
    dst.destaddress   = src.destAddress;
    dst.destsubnet    = src.destSubnet;
    dst.senderaddress = src.senderAddress;
    dst.msgid         = src.msgId;
    // Header field 5 (`encrypted`) no longer exists — encryption is inferred
    // from the oneof case.
    dst.burstindex    = src.burstIndex;
    dst.burstcount    = src.burstCount;
}

static void fill_header_from_pb(const ::LoraHeader& src, LoraHeader& dst) {
    dst.destAddress   = src.destaddress;
    dst.destSubnet    = src.destsubnet;
    dst.senderAddress = src.senderaddress;
    dst.msgId         = src.msgid;
    dst.burstIndex    = src.burstindex;
    dst.burstCount    = src.burstcount;
}

// Slim AEAD envelope: only tag + ciphertext travel.
static void fill_enc_pb(const EncryptedPayload& src, ::EncryptedPayload& dst) {
    encrypted_payload__init(&dst);
    dst.tag.data            = const_cast<uint8_t*>(src.tag.data());
    dst.tag.len             = src.tag.size();
    dst.ciphertext.data     = const_cast<uint8_t*>(src.ciphertext.data());
    dst.ciphertext.len      = src.ciphertext.size();
}

static void fill_enc_from_pb(const ::EncryptedPayload& src, EncryptedPayload& dst) {
    dst.tag.assign(src.tag.data, src.tag.data + src.tag.len);
    dst.ciphertext.assign(src.ciphertext.data,
                          src.ciphertext.data + src.ciphertext.len);
}

static std::vector<uint8_t> serialize_op_impl(const LoraClientOperationMessage& m,
                                              bool with_header) {
    ::LoraClientOperationMessage pb;
    lora_client_operation_message__init(&pb);

    ::LoraHeader pb_header;
    fill_header_pb(m.header, pb_header);
    if (with_header) pb.header = &pb_header;

    ::LoraCoverOperation pb_covop;
    ::ClientConfig       pb_cc;
    ::CoverConfig        pb_cv;
    ::LoginMsg           pb_login;
    ::BaseNonceExchange  pb_basen;
    ::EncryptedPayload   pb_enc;
    ::TimeSync           pb_ts;
    ::ScheduleConfig     pb_sched;
    std::vector<uint8_t> name_buf;
    std::vector<uint8_t> bn_buf;
    // Storage for the repeated ScheduleEntry array must outlive the pack call
    // below, so it is declared here rather than inside the switch case.
    std::vector<::ScheduleEntry>  sched_entries;
    std::vector<::ScheduleEntry*> sched_ptrs;

    using Cmd = LoraClientOperationMessage::Cmd;
    switch (m.cmd) {
    case Cmd::Operation:
        lora_cover_operation__init(&pb_covop);
        if (m.operation.kind == LoraCoverOperation::Kind::Operation) {
            pb_covop.covop_case = LORA_COVER_OPERATION__COVOP_OPERATION;
            pb_covop.operation  = static_cast<::CovOperation>(m.operation.operation);
        } else {
            pb_covop.covop_case = LORA_COVER_OPERATION__COVOP_POSITION;
            pb_covop.position   = m.operation.position;
        }
        pb.cmd_case  = LORA_CLIENT_OPERATION_MESSAGE__CMD_OPERATION;
        pb.operation = &pb_covop;
        break;
    case Cmd::Sysop:
        pb.cmd_case = LORA_CLIENT_OPERATION_MESSAGE__CMD_SYSOP;
        pb.sysop    = static_cast<::ClientOperation>(m.sysop);
        break;
    case Cmd::ClientConfig:
        client_config__init(&pb_cc);
        pb_cc.mac_addr      = m.clientconfig.mac_addr;
        pb_cc.addr          = m.clientconfig.addr;
        pb_cc.subnt         = m.clientconfig.subnt;
        name_buf.assign(m.clientconfig.name.begin(), m.clientconfig.name.end());
        pb_cc.name.data     = name_buf.data();
        pb_cc.name.len      = name_buf.size();
        pb_cc.sleepduration = m.clientconfig.sleepDuration;
        pb_cc.batteryinterval = m.clientconfig.batteryInterval;
        pb.cmd_case      = LORA_CLIENT_OPERATION_MESSAGE__CMD_CLIENTCONFIG;
        pb.clientconfig  = &pb_cc;
        break;
    case Cmd::CoverConfig:
        cover_config__init(&pb_cv);
        pb_cv.opentime         = m.coverconfig.openTime;
        pb_cv.closetime        = m.coverconfig.closeTime;
        pb_cv.blindheightmm    = m.coverconfig.blindHeightMm;
        pb_cv.axlediametermm   = m.coverconfig.axleDiameterMm;
        pb_cv.blindthicknessmm = m.coverconfig.blindThicknessMm;
        pb_cv.openslack        = m.coverconfig.openSlack;
        pb_cv.closeslack       = m.coverconfig.closeSlack;
        pb.cmd_case     = LORA_CLIENT_OPERATION_MESSAGE__CMD_COVERCONFIG;
        pb.coverconfig  = &pb_cv;
        break;
    case Cmd::Login:
        login_msg__init(&pb_login);
        pb_login.nonce            = m.login.nonce;
        pb_login.request_register = m.login.request_register;
        pb.cmd_case = LORA_CLIENT_OPERATION_MESSAGE__CMD_LOGIN;
        pb.login    = &pb_login;
        break;
    case Cmd::TimeSync:
        time_sync__init(&pb_ts);
        pb_ts.epoch     = m.timesync.epoch;
        pb_ts.utcoffset = m.timesync.utcOffset;
        pb_ts.dstnext   = m.timesync.dstNext;
        pb.cmd_case = LORA_CLIENT_OPERATION_MESSAGE__CMD_TIMESYNC;
        pb.timesync = &pb_ts;
        break;
    case Cmd::Schedule:
        schedule_config__init(&pb_sched);
        pb_sched.version              = m.schedule.version;
        pb_sched.mode                 = static_cast<::NodeMode>(m.schedule.mode);
        pb_sched.interactivetimeout_s = m.schedule.interactiveTimeout_s;
        pb_sched.checkininterval_s    = m.schedule.checkinInterval_s;
        pb_sched.beaconlead_s         = m.schedule.beaconLead_s;
        pb_sched.posteventwindow_s    = m.schedule.postEventWindow_s;
        pb_sched.catchupwindow_s      = m.schedule.catchupWindow_s;
        sched_entries.resize(m.schedule.entries.size());
        sched_ptrs.resize(m.schedule.entries.size());
        for (size_t i = 0; i < m.schedule.entries.size(); ++i) {
            schedule_entry__init(&sched_entries[i]);
            sched_entries[i].minuteofday = m.schedule.entries[i].minuteOfDay;
            sched_entries[i].daymask     = m.schedule.entries[i].dayMask;
            sched_entries[i].action      =
                static_cast<::SchedAction>(m.schedule.entries[i].action);
            sched_entries[i].positionpct = m.schedule.entries[i].positionPct;
            sched_entries[i].kind        = m.schedule.entries[i].kind;
            sched_ptrs[i] = &sched_entries[i];
        }
        pb_sched.n_entries = sched_ptrs.size();
        pb_sched.entries   = sched_ptrs.empty() ? nullptr : sched_ptrs.data();
        pb.cmd_case = LORA_CLIENT_OPERATION_MESSAGE__CMD_SCHEDULE;
        pb.schedule = &pb_sched;
        break;
    case Cmd::BaseNonce:
        base_nonce_exchange__init(&pb_basen);
        // Encode base nonce as 4-byte big-endian (production wire format).
        bn_buf.resize(4);
        bn_buf[0] = (m.basenonce.base_nonce >> 24) & 0xFF;
        bn_buf[1] = (m.basenonce.base_nonce >> 16) & 0xFF;
        bn_buf[2] = (m.basenonce.base_nonce >>  8) & 0xFF;
        bn_buf[3] = (m.basenonce.base_nonce >>  0) & 0xFF;
        pb_basen.base_nonce.data = bn_buf.data();
        pb_basen.base_nonce.len  = bn_buf.size();
        pb.cmd_case  = LORA_CLIENT_OPERATION_MESSAGE__CMD_BASENONCE;
        pb.basenonce = &pb_basen;
        break;
    case Cmd::Encrypted:
        fill_enc_pb(m.encrypted, pb_enc);
        pb.cmd_case  = LORA_CLIENT_OPERATION_MESSAGE__CMD_ENCRYPTED;
        pb.encrypted = &pb_enc;
        break;
    case Cmd::NotSet:
        break;
    }

    const size_t len = lora_client_operation_message__get_packed_size(&pb);
    std::vector<uint8_t> out(len);
    lora_client_operation_message__pack(&pb, out.data());
    return out;
}

std::vector<uint8_t> serialize_op(const LoraClientOperationMessage& m) {
    return serialize_op_impl(m, /*with_header=*/true);
}

std::vector<uint8_t> serialize_op_payload(const LoraClientOperationMessage& m) {
    return serialize_op_impl(m, /*with_header=*/false);
}

static std::vector<uint8_t> serialize_resp_impl(const LoraClientResponseMessage& m,
                                                bool with_header) {
    ::LoraClientResponseMessage pb;
    lora_client_response_message__init(&pb);

    ::LoraHeader pb_header;
    fill_header_pb(m.header, pb_header);
    if (with_header) pb.header = &pb_header;

    ::ClientAvailable pb_avail;
    ::ClientRegister  pb_reg;
    ::ClientBattery   pb_bat;
    ::CoverPosition   pb_pos;
    ::LoginMsg        pb_login;
    ::EncryptedPayload pb_enc;
    ::CommandAck      pb_ack;
    ::NodeWakeBeacon  pb_beacon;

    using Proto = LoraClientResponseMessage::Proto;
    switch (m.proto) {
    case Proto::Avail:
        client_available__init(&pb_avail);
        pb_avail.available = m.avail.available;
        pb.proto_case = LORA_CLIENT_RESPONSE_MESSAGE__PROTO_AVAIL;
        pb.avail      = &pb_avail;
        break;
    case Proto::Register:
        client_register__init(&pb_reg);
        pb_reg.mac_addr     = m.reg.mac_addr;
        pb_reg.needs_config = m.reg.needs_config;
        pb.proto_case = LORA_CLIENT_RESPONSE_MESSAGE__PROTO_REGISTER;
        pb.register_  = &pb_reg;
        break;
    case Proto::State:
        client_battery__init(&pb_bat);
        pb_bat.voltage = m.state.voltage;
        pb.proto_case = LORA_CLIENT_RESPONSE_MESSAGE__PROTO_STATE;
        pb.state      = &pb_bat;
        break;
    case Proto::Position:
        cover_position__init(&pb_pos);
        pb_pos.position = m.position.position;
        pb_pos.voltage  = m.position.voltage;
        pb_pos.current  = m.position.current;
        pb.proto_case = LORA_CLIENT_RESPONSE_MESSAGE__PROTO_POSITION;
        pb.position   = &pb_pos;
        break;
    case Proto::Login:
        login_msg__init(&pb_login);
        pb_login.nonce            = m.login.nonce;
        pb_login.request_register = m.login.request_register;
        pb.proto_case = LORA_CLIENT_RESPONSE_MESSAGE__PROTO_LOGIN;
        pb.login      = &pb_login;
        break;
    case Proto::Ack:
        command_ack__init(&pb_ack);
        pb_ack.ack_msg_id = m.ack.ack_msg_id;
        pb_ack.status     = static_cast<::AckStatus>(m.ack.status);
        pb.proto_case = LORA_CLIENT_RESPONSE_MESSAGE__PROTO_ACK;
        pb.ack        = &pb_ack;
        break;
    case Proto::Beacon:
        node_wake_beacon__init(&pb_beacon);
        pb_beacon.reason         = static_cast<::WakeReason>(m.beacon.reason);
        pb_beacon.schedversion   = m.beacon.schedVersion;
        pb_beacon.nodeepoch      = m.beacon.nodeEpoch;
        pb_beacon.mode           = static_cast<::NodeMode>(m.beacon.mode);
        pb_beacon.voltage        = m.beacon.voltage;
        pb_beacon.position       = m.beacon.position;
        pb_beacon.awakewindow_ms = m.beacon.awakeWindow_ms;
        pb_beacon.nexteventepoch = m.beacon.nextEventEpoch;
        pb_beacon.sessionresume  = m.beacon.sessionResume;
        pb_beacon.clockvalid     = m.beacon.clockValid;
        pb_beacon.fwversion      = m.beacon.fwVersion;
        pb.proto_case = LORA_CLIENT_RESPONSE_MESSAGE__PROTO_BEACON;
        pb.beacon     = &pb_beacon;
        break;
    case Proto::Encrypted:
        fill_enc_pb(m.encrypted, pb_enc);
        pb.proto_case = LORA_CLIENT_RESPONSE_MESSAGE__PROTO_ENCRYPTED;
        pb.encrypted  = &pb_enc;
        break;
    case Proto::NotSet:
        break;
    }

    const size_t len = lora_client_response_message__get_packed_size(&pb);
    std::vector<uint8_t> out(len);
    lora_client_response_message__pack(&pb, out.data());
    return out;
}

std::vector<uint8_t> serialize_resp(const LoraClientResponseMessage& m) {
    return serialize_resp_impl(m, /*with_header=*/true);
}

std::vector<uint8_t> serialize_resp_payload(const LoraClientResponseMessage& m) {
    return serialize_resp_impl(m, /*with_header=*/false);
}

// ---------------------------------------------------------------------------
// proto-c → struct
// ---------------------------------------------------------------------------
std::optional<LoraClientOperationMessage> deserialize_op(const uint8_t* data, size_t len) {
    ::LoraClientOperationMessage* pb =
        lora_client_operation_message__unpack(nullptr, len, data);
    if (!pb) return std::nullopt;

    LoraClientOperationMessage out;
    if (pb->header) fill_header_from_pb(*pb->header, out.header);

    using Cmd = LoraClientOperationMessage::Cmd;
    switch (pb->cmd_case) {
    case LORA_CLIENT_OPERATION_MESSAGE__CMD_OPERATION:
        out.cmd = Cmd::Operation;
        if (pb->operation) {
            if (pb->operation->covop_case == LORA_COVER_OPERATION__COVOP_OPERATION) {
                out.operation.kind = LoraCoverOperation::Kind::Operation;
                out.operation.operation = static_cast<CovOperation>(pb->operation->operation);
            } else if (pb->operation->covop_case == LORA_COVER_OPERATION__COVOP_POSITION) {
                out.operation.kind = LoraCoverOperation::Kind::Position;
                out.operation.position = pb->operation->position;
            }
        }
        break;
    case LORA_CLIENT_OPERATION_MESSAGE__CMD_SYSOP:
        out.cmd   = Cmd::Sysop;
        out.sysop = static_cast<ClientOperation>(pb->sysop);
        break;
    case LORA_CLIENT_OPERATION_MESSAGE__CMD_CLIENTCONFIG:
        out.cmd = Cmd::ClientConfig;
        if (pb->clientconfig) {
            out.clientconfig.mac_addr      = pb->clientconfig->mac_addr;
            out.clientconfig.addr          = pb->clientconfig->addr;
            out.clientconfig.subnt         = pb->clientconfig->subnt;
            out.clientconfig.name.assign(
                reinterpret_cast<const char*>(pb->clientconfig->name.data),
                pb->clientconfig->name.len);
            out.clientconfig.sleepDuration = pb->clientconfig->sleepduration;
            out.clientconfig.batteryInterval = pb->clientconfig->batteryinterval;
        }
        break;
    case LORA_CLIENT_OPERATION_MESSAGE__CMD_COVERCONFIG:
        out.cmd = Cmd::CoverConfig;
        if (pb->coverconfig) {
            out.coverconfig.openTime         = pb->coverconfig->opentime;
            out.coverconfig.closeTime        = pb->coverconfig->closetime;
            out.coverconfig.blindHeightMm    = pb->coverconfig->blindheightmm;
            out.coverconfig.axleDiameterMm   = pb->coverconfig->axlediametermm;
            out.coverconfig.blindThicknessMm = pb->coverconfig->blindthicknessmm;
            out.coverconfig.openSlack        = pb->coverconfig->openslack;
            out.coverconfig.closeSlack       = pb->coverconfig->closeslack;
        }
        break;
    case LORA_CLIENT_OPERATION_MESSAGE__CMD_LOGIN:
        out.cmd = Cmd::Login;
        if (pb->login) {
            out.login.nonce            = pb->login->nonce;
            out.login.request_register = pb->login->request_register;
        }
        break;
    case LORA_CLIENT_OPERATION_MESSAGE__CMD_TIMESYNC:
        out.cmd = Cmd::TimeSync;
        if (pb->timesync) {
            out.timesync.epoch     = pb->timesync->epoch;
            out.timesync.utcOffset = pb->timesync->utcoffset;
            out.timesync.dstNext   = pb->timesync->dstnext;
        }
        break;
    case LORA_CLIENT_OPERATION_MESSAGE__CMD_SCHEDULE:
        out.cmd = Cmd::Schedule;
        if (pb->schedule) {
            out.schedule.version              = pb->schedule->version;
            out.schedule.mode                 = static_cast<NodeMode>(pb->schedule->mode);
            out.schedule.interactiveTimeout_s = pb->schedule->interactivetimeout_s;
            out.schedule.checkinInterval_s    = pb->schedule->checkininterval_s;
            out.schedule.beaconLead_s         = pb->schedule->beaconlead_s;
            out.schedule.postEventWindow_s    = pb->schedule->posteventwindow_s;
            out.schedule.catchupWindow_s      = pb->schedule->catchupwindow_s;
            out.schedule.entries.clear();
            for (size_t i = 0; i < pb->schedule->n_entries; ++i) {
                const ::ScheduleEntry* e = pb->schedule->entries[i];
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
    case LORA_CLIENT_OPERATION_MESSAGE__CMD_BASENONCE:
        out.cmd = Cmd::BaseNonce;
        if (pb->basenonce && pb->basenonce->base_nonce.len == 4) {
            const uint8_t* b = pb->basenonce->base_nonce.data;
            out.basenonce.base_nonce =
                (static_cast<uint32_t>(b[0]) << 24) |
                (static_cast<uint32_t>(b[1]) << 16) |
                (static_cast<uint32_t>(b[2]) <<  8) |
                (static_cast<uint32_t>(b[3]));
        }
        break;
    case LORA_CLIENT_OPERATION_MESSAGE__CMD_ENCRYPTED:
        out.cmd = Cmd::Encrypted;
        if (pb->encrypted) fill_enc_from_pb(*pb->encrypted, out.encrypted);
        break;
    default:
        // Includes CMD__NOT_SET and any field number this build does not know
        // (i.e. a newer peer's message reaching an older parser).  Must stay a
        // quiet, non-fatal fallthrough — see the P0 forward-compatibility gate.
        out.cmd = Cmd::NotSet;
        break;
    }

    lora_client_operation_message__free_unpacked(pb, nullptr);
    return out;
}

std::optional<LoraClientResponseMessage> deserialize_resp(const uint8_t* data, size_t len) {
    ::LoraClientResponseMessage* pb =
        lora_client_response_message__unpack(nullptr, len, data);
    if (!pb) return std::nullopt;

    LoraClientResponseMessage out;
    if (pb->header) fill_header_from_pb(*pb->header, out.header);

    using Proto = LoraClientResponseMessage::Proto;
    switch (pb->proto_case) {
    case LORA_CLIENT_RESPONSE_MESSAGE__PROTO_AVAIL:
        out.proto = Proto::Avail;
        if (pb->avail) out.avail.available = pb->avail->available;
        break;
    case LORA_CLIENT_RESPONSE_MESSAGE__PROTO_REGISTER:
        out.proto = Proto::Register;
        if (pb->register_) {
            out.reg.mac_addr     = pb->register_->mac_addr;
            out.reg.needs_config = pb->register_->needs_config;
        }
        break;
    case LORA_CLIENT_RESPONSE_MESSAGE__PROTO_STATE:
        out.proto = Proto::State;
        if (pb->state) out.state.voltage = pb->state->voltage;
        break;
    case LORA_CLIENT_RESPONSE_MESSAGE__PROTO_POSITION:
        out.proto = Proto::Position;
        if (pb->position) {
            out.position.position = pb->position->position;
            out.position.voltage  = pb->position->voltage;
            out.position.current  = pb->position->current;
        }
        break;
    case LORA_CLIENT_RESPONSE_MESSAGE__PROTO_LOGIN:
        out.proto = Proto::Login;
        if (pb->login) {
            out.login.nonce            = pb->login->nonce;
            out.login.request_register = pb->login->request_register;
        }
        break;
    case LORA_CLIENT_RESPONSE_MESSAGE__PROTO_ACK:
        out.proto = Proto::Ack;
        if (pb->ack) {
            out.ack.ack_msg_id = pb->ack->ack_msg_id;
            out.ack.status     = static_cast<AckStatus>(pb->ack->status);
        }
        break;
    case LORA_CLIENT_RESPONSE_MESSAGE__PROTO_BEACON:
        out.proto = Proto::Beacon;
        if (pb->beacon) {
            out.beacon.reason         = static_cast<WakeReason>(pb->beacon->reason);
            out.beacon.schedVersion   = pb->beacon->schedversion;
            out.beacon.nodeEpoch      = pb->beacon->nodeepoch;
            out.beacon.mode           = static_cast<NodeMode>(pb->beacon->mode);
            out.beacon.voltage        = pb->beacon->voltage;
            out.beacon.position       = pb->beacon->position;
            out.beacon.awakeWindow_ms = pb->beacon->awakewindow_ms;
            out.beacon.nextEventEpoch = pb->beacon->nexteventepoch;
            out.beacon.sessionResume  = pb->beacon->sessionresume;
            out.beacon.clockValid     = pb->beacon->clockvalid;
            out.beacon.fwVersion      = pb->beacon->fwversion;
        }
        break;
    case LORA_CLIENT_RESPONSE_MESSAGE__PROTO_ENCRYPTED:
        out.proto = Proto::Encrypted;
        if (pb->encrypted) fill_enc_from_pb(*pb->encrypted, out.encrypted);
        break;
    default:
        // See the note in deserialize_op: unknown/newer field numbers land here
        // and must remain a quiet fallthrough.
        out.proto = Proto::NotSet;
        break;
    }

    lora_client_response_message__free_unpacked(pb, nullptr);
    return out;
}

AirFrame make_op_frame(const LoraClientOperationMessage& m) {
    return AirFrame{AirFrame::Dir::HubToNode, serialize_op(m)};
}

AirFrame make_resp_frame(const LoraClientResponseMessage& m) {
    return AirFrame{AirFrame::Dir::NodeToHub, serialize_resp(m)};
}

std::optional<LoraClientOperationMessage> as_op(const AirFrame& f) {
    if (f.dir != AirFrame::Dir::HubToNode) return std::nullopt;
    return deserialize_op(f.bytes.data(), f.bytes.size());
}

std::optional<LoraClientResponseMessage> as_resp(const AirFrame& f) {
    if (f.dir != AirFrame::Dir::NodeToHub) return std::nullopt;
    return deserialize_resp(f.bytes.data(), f.bytes.size());
}

} // namespace proto_sim

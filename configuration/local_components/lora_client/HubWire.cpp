// HubWire.cpp -- see HubWire.h.
#include "esphome/components/lora_client/HubWire.h"

#include <cstring>

namespace hubwire
{

// The port table of spec section 3, as the hub's producers see it. PORT 0 is
// MAC management (HubMacPdu): GridSync is the Mac channel, Acked. PORT 2 is the
// application (HubAppPdu): Control (cover op, sysop) and Config (ScheduleConfig)
// are Acked; TimeSync, ClientConfig, CoverConfig are Fire.
struct RouteRow
{
    hubframes::DownCase cmd;
    uint8_t                             port;
    bool                                ack_req;
};
static constexpr RouteRow kRoutes[] = {
    {hubframes::DL_GRIDSYNC,     macwire::kPortMac0Mgmt, true},
    {hubframes::DL_DRIFTTEST,    macwire::kPortMac0Mgmt, false},
    {hubframes::DL_MACCONTROL,   macwire::kPortMac0Mgmt, false},
    {hubframes::DL_MODETEST,     macwire::kPortMac0Mgmt, false},
    {hubframes::DL_GRIDBEACON,   macwire::kPortMac0Mgmt, false},
    {hubframes::DL_GRIDDEMOTE,   macwire::kPortMac0Mgmt, false},
    {hubframes::DL_OPERATION,    macwire::kPortApp,      true},
    {hubframes::DL_SYSOP,        macwire::kPortApp,      true},
    {hubframes::DL_SCHEDULE,     macwire::kPortApp,      true},
    {hubframes::DL_CLIENTCONFIG, macwire::kPortApp,      false},
    {hubframes::DL_COVERCONFIG,  macwire::kPortApp,      false},
    {hubframes::DL_TIMESYNC,     macwire::kPortApp,      false},
};

Route routeDownlink(hubframes::DownCase c)
{
    for (const RouteRow &r : kRoutes)
        if (r.cmd == c)
            return {true, r.port, r.ack_req};
    return {};
}

// HubMacPdu: the command's body is moved into the same-numbered PDU case.
static bool s_mac_pdu(const hubframes::Down *op, ::HubMacPdu *pdu)
{
    switch (op->cmd_case)
    {
    case hubframes::DL_DRIFTTEST:
        pdu->pdu_case = HUB_MAC_PDU__PDU_DRIFTTEST;  pdu->drifttest  = op->drifttest;  break;
    case hubframes::DL_MACCONTROL:
        pdu->pdu_case = HUB_MAC_PDU__PDU_MACCONTROL; pdu->maccontrol = op->maccontrol; break;
    case hubframes::DL_GRIDSYNC:
        pdu->pdu_case = HUB_MAC_PDU__PDU_GRIDSYNC;   pdu->gridsync   = op->gridsync;   break;
    case hubframes::DL_MODETEST:
        pdu->pdu_case = HUB_MAC_PDU__PDU_MODETEST;   pdu->modetest   = op->modetest;   break;
    case hubframes::DL_GRIDBEACON:
        pdu->pdu_case = HUB_MAC_PDU__PDU_GRIDBEACON; pdu->gridbeacon = op->gridbeacon; break;
    case hubframes::DL_GRIDDEMOTE:
        pdu->pdu_case = HUB_MAC_PDU__PDU_GRIDDEMOTE; pdu->griddemote = op->griddemote; break;
    default:
        return false;
    }
    return pdu->drifttest != nullptr;   // every case's pointer shares the union
}

// HubAppPdu. The Control commands get their identity (section 7) here, in
// copies, so the caller's command is never modified.
static bool s_app_pdu(const hubframes::Down *op, ::HubAppPdu *pdu,
                      ::LoraCoverOperation *covop, ::SysopCmd *sysop, uint32_t cmd_id)
{
    switch (op->cmd_case)
    {
    case hubframes::DL_OPERATION:
        if (op->operation == nullptr)
            return false;
        *covop        = *op->operation;
        covop->cmd_id = cmd_id;
        pdu->pdu_case = HUB_APP_PDU__PDU_OPERATION; pdu->operation = covop; return true;
    case hubframes::DL_SYSOP:
        sysop->op     = op->sysop;
        sysop->cmd_id = cmd_id;
        pdu->pdu_case = HUB_APP_PDU__PDU_SYSOP; pdu->sysop = sysop; return true;
    case hubframes::DL_CLIENTCONFIG:
        pdu->pdu_case = HUB_APP_PDU__PDU_CLIENTCONFIG; pdu->clientconfig = op->clientconfig; break;
    case hubframes::DL_COVERCONFIG:
        pdu->pdu_case = HUB_APP_PDU__PDU_COVERCONFIG; pdu->coverconfig = op->coverconfig; break;
    case hubframes::DL_TIMESYNC:
        pdu->pdu_case = HUB_APP_PDU__PDU_TIMESYNC; pdu->timesync = op->timesync; break;
    case hubframes::DL_SCHEDULE:
        pdu->pdu_case = HUB_APP_PDU__PDU_SCHEDULE; pdu->schedule = op->schedule; break;
    default:
        return false;
    }
    return pdu->clientconfig != nullptr;
}

static bool s_pack(const ProtobufCMessage *m, uint8_t *out, size_t cap, size_t *len)
{
    const size_t n = protobuf_c_message_get_packed_size(m);
    if (n > cap)
        return false;
    *len = protobuf_c_message_pack(m, out);
    return *len == n;
}

bool packDownlinkBody(const hubframes::Down *op, uint32_t cmd_id, uint8_t *out,
                      size_t cap, size_t *len)
{
    if (op == nullptr || out == nullptr || len == nullptr)
        return false;
    const Route r = routeDownlink(op->cmd_case);
    if (!r.ok)
        return false;
    if (r.port == macwire::kPortMac0Mgmt)
    {
        ::HubMacPdu pdu = HUB_MAC_PDU__INIT;
        return s_mac_pdu(op, &pdu) && s_pack(&pdu.base, out, cap, len);
    }
    ::HubAppPdu          pdu   = HUB_APP_PDU__INIT;
    ::LoraCoverOperation covop = LORA_COVER_OPERATION__INIT;
    ::SysopCmd           sysop = SYSOP_CMD__INIT;
    return s_app_pdu(op, &pdu, &covop, &sysop, cmd_id) && s_pack(&pdu.base, out, cap, len);
}

bool assemble(const macwire::WireHdr &h, const uint8_t *body, size_t body_len,
              std::vector<uint8_t> *out)
{
    const size_t len = frameLen(h, body_len);
    if (out == nullptr || len > macwire::kMaxFrameBytes || (body_len > 0 && body == nullptr))
        return false;
    out->assign(len, 0);
    const size_t hlen = macwire::encodeHeader(h, out->data(), len);
    if (hlen == 0)
    {
        out->clear();
        return false;
    }
    if (body_len)
        std::memcpy(out->data() + hlen, body, body_len);
    return true;
}

void toHdr(const macwire::WireHdr &h, hubframes::Hdr *out)
{
    *out               = hubframes::Hdr{};
    out->destaddress   = h.dst;
    out->destsubnet    = h.subnet;
    out->senderaddress = h.src;
    out->msgid         = h.fcnt;
    out->burstindex    = h.burst_idx;
    out->burstcount    = h.burst_cnt;
    out->onmark        = h.onMark();
    out->firestamped   = h.stamped();
    out->fireround     = h.fire_round;
    out->fireoffsetus  = h.fire_offset_us;
    out->sessionnonce  = h.sessionOpening() ? h.node_nonce : 0;
}

// ---------------------------------------------------------------------------
// Uplink
// ---------------------------------------------------------------------------

Uplink::Uplink() = default;

Uplink::~Uplink() { this->reset_(); }

void Uplink::reset_()
{
    if (this->mac_) node_mac_pdu__free_unpacked(this->mac_, nullptr);
    if (this->app_) node_app_pdu__free_unpacked(this->app_, nullptr);
    if (this->reg_) client_register__free_unpacked(this->reg_, nullptr);
    this->mac_ = nullptr;
    this->app_ = nullptr;
    this->reg_ = nullptr;
    this->msg_ = hubframes::Up{};
}

void Uplink::sealed(const macwire::WireHdr &h, const uint8_t *body, size_t len, const uint8_t *tag)
{
    this->reset_();
    toHdr(h, &this->hdr_);
    this->sealed_.ciphertext.data = const_cast<uint8_t *>(body);
    this->sealed_.ciphertext.len  = len;
    this->sealed_.tag.data        = const_cast<uint8_t *>(tag);
    this->sealed_.tag.len         = macwire::kTagBytes;
    this->msg_.header     = &this->hdr_;
    this->msg_.proto_case = hubframes::UL_SEALED;
    this->msg_.encrypted  = &this->sealed_;
}

// MacAck + ACKED_FCNT is the UpAck; the other cases keep their message.
bool Uplink::viewMac_(const macwire::WireHdr &h)
{
    switch (this->mac_->pdu_case)
    {
    case NODE_MAC_PDU__PDU_ACK:
        if (!h.ack() || this->mac_->ack == nullptr)
            return false;
        this->ack_.ack_msg_id = h.acked_fcnt;
        this->ack_.status     = this->mac_->ack->status;
        this->ack_.phase      = this->mac_->ack->phase;
        this->msg_.proto_case = hubframes::UL_ACK;
        this->msg_.ack        = &this->ack_;
        return true;
    case NODE_MAC_PDU__PDU_MACCONTROL:
        this->msg_.proto_case = hubframes::UL_MACCONTROL;
        this->msg_.maccontrol = this->mac_->maccontrol;
        break;
    case NODE_MAC_PDU__PDU_MODETESTREPORT:
        this->msg_.proto_case     = hubframes::UL_MODETESTREPORT;
        this->msg_.modetestreport = this->mac_->modetestreport;
        break;
    case NODE_MAC_PDU__PDU_GRIDSYNCREQUEST:
        this->msg_.proto_case      = hubframes::UL_GRIDSYNCREQUEST;
        this->msg_.gridsyncrequest = this->mac_->gridsyncrequest;
        break;
    default:
        return false;
    }
    return this->msg_.maccontrol != nullptr;
}

bool Uplink::viewApp_()
{
    switch (this->app_->pdu_case)
    {
    case NODE_APP_PDU__PDU_AVAIL:
        this->msg_.proto_case = hubframes::UL_AVAIL;    this->msg_.avail = this->app_->avail; break;
    case NODE_APP_PDU__PDU_STATE:
        this->msg_.proto_case = hubframes::UL_STATE;    this->msg_.state = this->app_->state; break;
    case NODE_APP_PDU__PDU_POSITION:
        this->msg_.proto_case = hubframes::UL_POSITION; this->msg_.position = this->app_->position; break;
    case NODE_APP_PDU__PDU_BEACON:
        this->msg_.proto_case = hubframes::UL_BEACON;   this->msg_.beacon = this->app_->beacon; break;
    default:
        return false;
    }
    return this->msg_.avail != nullptr;
}

bool Uplink::parse(const macwire::WireHdr &h, const uint8_t *body, size_t len)
{
    this->reset_();
    toHdr(h, &this->hdr_);
    this->msg_.header = &this->hdr_;
    bool ok = false;
    if (h.port == macwire::kPortMac0Mgmt)
    {
        this->mac_ = node_mac_pdu__unpack(nullptr, len, body);
        ok = this->mac_ != nullptr && this->viewMac_(h);
    }
    else if (h.port == macwire::kPortSession)
    {
        this->reg_ = client_register__unpack(nullptr, len, body);
        ok = this->reg_ != nullptr;
        this->msg_.proto_case = hubframes::UL_REGISTER;
        this->msg_.register_  = this->reg_;
    }
    else if (h.port == macwire::kPortApp)
    {
        this->app_ = node_app_pdu__unpack(nullptr, len, body);
        ok = this->app_ != nullptr && this->viewApp_();
    }
    if (!ok)
        this->reset_();
    return ok;
}

}  // namespace hubwire

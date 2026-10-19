// v1_bridge.cpp -- see v1_bridge.h.
#include "sim/v1_bridge.h"

#include "sim/crypto.h"
#include "blinds.pb-c.h"
#include "FrameCrypto.h"
#include "MacWire.h"

#include <cstring>
#include <map>

namespace proto_sim {
namespace v1bridge {

namespace {
SessionLookup             g_lookup = nullptr;
std::map<uint8_t, uint32_t> g_nonce;

using Bytes = std::vector<uint8_t>;

bool sessionOf(uint8_t node, uint32_t *sid) {
    *sid = 0;
    return g_lookup != nullptr && g_lookup(node, sid);
}

template <typename M, typename F, typename G>
Bytes packWith(const M *m, F size_fn, G pack_fn) {
    Bytes b(size_fn(m));
    if (!b.empty()) pack_fn(m, b.data());
    return b;
}

void flipTag(Bytes &f) {
    if (!f.empty()) f.back() ^= 0x01;
}

// ---- uplink: v0 response oneof -> (PORT, v1 body, ACK flag) ------------------
bool mapUplinkBody(const LoraClientResponseMessage *r, macwire::WireHdr *h, Bytes *body) {
    switch (r->proto_case) {
    case LORA_CLIENT_RESPONSE_MESSAGE__PROTO_ACK: {
        if (r->ack == nullptr) return false;
        MacAck a = MAC_ACK__INIT;
        a.status = r->ack->status;
        a.phase  = r->ack->phase;
        NodeMacPdu p = NODE_MAC_PDU__INIT;
        p.pdu_case = NODE_MAC_PDU__PDU_ACK;
        p.ack = &a;
        h->port = macwire::kPortMac0Mgmt;
        h->flags |= macwire::kFlagAck;
        h->acked_fcnt = r->ack->ack_msg_id;
        *body = packWith(&p, node_mac_pdu__get_packed_size, node_mac_pdu__pack);
        return true;
    }
    case LORA_CLIENT_RESPONSE_MESSAGE__PROTO_MACCONTROL:
    case LORA_CLIENT_RESPONSE_MESSAGE__PROTO_MODETESTREPORT:
    case LORA_CLIENT_RESPONSE_MESSAGE__PROTO_GRIDSYNCREQUEST: {
        NodeMacPdu p = NODE_MAC_PDU__INIT;
        p.pdu_case = (NodeMacPdu__PduCase) r->proto_case;   // same field numbers
        p.maccontrol = r->maccontrol;                        // one pointer union
        h->port = macwire::kPortMac0Mgmt;
        *body = packWith(&p, node_mac_pdu__get_packed_size, node_mac_pdu__pack);
        return true;
    }
    case LORA_CLIENT_RESPONSE_MESSAGE__PROTO_AVAIL:
    case LORA_CLIENT_RESPONSE_MESSAGE__PROTO_STATE:
    case LORA_CLIENT_RESPONSE_MESSAGE__PROTO_POSITION:
    case LORA_CLIENT_RESPONSE_MESSAGE__PROTO_BEACON: {
        NodeAppPdu p = NODE_APP_PDU__INIT;
        p.pdu_case = (NodeAppPdu__PduCase) r->proto_case;
        p.avail = r->avail;
        h->port = macwire::kPortApp;
        *body = packWith(&p, node_app_pdu__get_packed_size, node_app_pdu__pack);
        return true;
    }
    default:
        return false;
    }
}

macwire::WireHdr headerFromV0(const LoraHeader *lh) {
    macwire::WireHdr h;
    if (lh == nullptr) return h;
    h.dst       = (uint8_t) lh->destaddress;
    h.src       = (uint8_t) lh->senderaddress;
    h.subnet    = (uint8_t) lh->destsubnet;
    h.fcnt      = lh->msgid;
    h.burst_idx = (uint8_t) lh->burstindex;
    h.burst_cnt = (uint8_t) lh->burstcount;
    if (lh->onmark) h.flags |= macwire::kFlagOnMark;
    if (lh->firestamped) {
        h.flags |= macwire::kFlagStamped;
        h.fire_round     = lh->fireround & macwire::kU24Max;
        h.fire_offset_us = lh->fireoffsetus & macwire::kU24Max;
    }
    if (lh->sessionnonce != 0) {
        h.flags |= macwire::kFlagSessionOpening;
        h.node_nonce = lh->sessionnonce;
    }
    return h;
}

framecrypto::EtmHeaderFields etmFields(bool downlink, uint32_t sid, const LoraHeader *lh) {
    framecrypto::EtmHeaderFields f{};
    f.downlink       = downlink;
    f.session_id     = sid;
    f.dest_address   = lh->destaddress;
    f.dest_subnet    = lh->destsubnet;
    f.sender_address = lh->senderaddress;
    f.msgid          = lh->msgid;
    f.burst_index    = lh->burstindex;
    f.burst_count    = lh->burstcount;
    f.on_mark        = lh->onmark;
    f.fire_stamped   = lh->firestamped;
    f.fire_round     = lh->fireround;
    f.fire_offset_us = lh->fireoffsetus;
    return f;
}

Bytes uplinkEncrypted(const LoraClientResponseMessage *m) {
    const LoraHeader *lh = m->header;
    const uint8_t node = (uint8_t) lh->senderaddress;
    uint32_t sid = 0;
    sessionOf(node, &sid);
    // The nonce a session-opening frame names, else the one learnt; learnt only
    // from a frame that verifies under it (a forged nonce must not poison it).
    const uint32_t nonce = lh->sessionnonce != 0 ? lh->sessionnonce : nodeNonce(node);
    const EncryptedPayload *e = m->encrypted;
    auto pt = encrypt_then_cmac_open(sid, nonce, (uint8_t) lh->destaddress, node, false,
                                     etmFields(false, sid, lh), e->ciphertext.data,
                                     e->ciphertext.len, e->tag.data, e->tag.len);
    macwire::WireHdr h = headerFromV0(lh);
    Bytes body;
    bool mapped = false;
    if (pt && lh->sessionnonce != 0) noteNodeNonce(node, lh->sessionnonce);
    if (pt) {
        LoraClientResponseMessage *inner =
            lora_client_response_message__unpack(nullptr, pt->size(), pt->data());
        if (inner) {
            mapped = mapUplinkBody(inner, &h, &body);
            lora_client_response_message__free_unpacked(inner, nullptr);
        }
    }
    if (!mapped) {
        // Authentic but not a message: the same plaintext under a valid v1 tag.
        // Not authentic: the v1 frame must fail its tag too.
        h.port = macwire::kPortApp;
        if (pt) body = *pt;
        else body.assign(e->ciphertext.data, e->ciphertext.data + e->ciphertext.len);
    }
    h.flags = macwire::withAuth(h.flags, macwire::Auth::Session);
    Bytes f = v1_session_frame(sid, nonce, (uint8_t) lh->destaddress, node, false, h, body);
    if (!pt) flipTag(f);
    return f;
}

Bytes uplinkRegister(const LoraClientResponseMessage *m) {
    const LoraHeader *lh = m->header;
    LoraHeader zero = LORA_HEADER__INIT;
    if (lh == nullptr) lh = &zero;
    const ClientRegister *r = m->register_;
    uint8_t mic[framecrypto::kSessionCmacTagBytes];
    compute_register_mic(r->mac_addr, r->needs_config, lh->destaddress, lh->destsubnet,
                         lh->senderaddress, lh->msgid, mic);
    const bool ok = r->mic.len == sizeof(mic) && std::memcmp(r->mic.data, mic, sizeof(mic)) == 0;
    ClientRegister body_msg = *r;
    body_msg.mic.data = nullptr;
    body_msg.mic.len  = 0;
    macwire::WireHdr h = headerFromV0(lh);
    h.flags = macwire::withAuth(h.flags & ~macwire::kFlagSessionOpening, macwire::Auth::KAuth);
    h.port  = macwire::kPortSession;
    Bytes f = v1_kauth_frame(false, h,
                             packWith(&body_msg, client_register__get_packed_size, client_register__pack));
    if (!ok) flipTag(f);
    return f;
}
}  // namespace

void setSessionLookup(SessionLookup f) { g_lookup = f; }
void noteNodeNonce(uint8_t node, uint32_t nonce) { g_nonce[node] = nonce; }
uint32_t nodeNonce(uint8_t node) {
    auto it = g_nonce.find(node);
    return it == g_nonce.end() ? kDefaultNodeNonce : it->second;
}
void reset() { g_nonce.clear(); }

Bytes uplinkToV1(const uint8_t *data, size_t len) {
    Bytes same(data, data + len);
    if (len == 0 || data[0] != macwire::kV0FirstByte) return same;
    LoraClientResponseMessage *m = lora_client_response_message__unpack(nullptr, len, data);
    if (m == nullptr) return same;
    Bytes out = same;
    if (m->proto_case == LORA_CLIENT_RESPONSE_MESSAGE__PROTO_ENCRYPTED && m->encrypted && m->header)
        out = uplinkEncrypted(m);
    else if (m->proto_case == LORA_CLIENT_RESPONSE_MESSAGE__PROTO_REGISTER && m->register_)
        out = uplinkRegister(m);
    else {
        macwire::WireHdr h = headerFromV0(m->header);
        h.flags &= (uint8_t) ~macwire::kFlagSessionOpening;   // plaintext: no session
        Bytes body;
        if (mapUplinkBody(m, &h, &body))
            out = v1_plain_frame(h, body);
    }
    lora_client_response_message__free_unpacked(m, nullptr);
    return out;
}

// ---- downlink: v1 -> the v0 envelope a test inspects ---------------------------
namespace {
bool v0OpFromBody(const macwire::WireHdr &h, const Bytes &body, LoraClientOperationMessage *op,
                  ProtobufCMessage **owned) {
    *owned = nullptr;
    if (h.port == macwire::kPortSession) {
        LoginMsg *lm = login_msg__unpack(nullptr, body.size(), body.data());
        if (!lm) return false;
        *owned = &lm->base;
        op->cmd_case = LORA_CLIENT_OPERATION_MESSAGE__CMD_LOGIN;
        op->login = lm;
        return true;
    }
    if (h.port == macwire::kPortMac0Mgmt) {
        HubMacPdu *p = hub_mac_pdu__unpack(nullptr, body.size(), body.data());
        if (!p) return false;
        *owned = &p->base;
        op->cmd_case = (LoraClientOperationMessage__CmdCase) p->pdu_case;
        op->drifttest = p->drifttest;   // one pointer union
        return p->pdu_case != HUB_MAC_PDU__PDU__NOT_SET;
    }
    HubAppPdu *p = hub_app_pdu__unpack(nullptr, body.size(), body.data());
    if (!p) return false;
    *owned = &p->base;
    op->cmd_case = (LoraClientOperationMessage__CmdCase) p->pdu_case;
    if (p->pdu_case == HUB_APP_PDU__PDU_SYSOP)
        op->sysop = p->sysop ? p->sysop->op : CLIENT_OPERATION__CMD_ENABLE_WIFI;
    else
        op->clientconfig = p->clientconfig;
    return p->pdu_case != HUB_APP_PDU__PDU__NOT_SET;
}

LoraHeader v0HeaderFrom(const macwire::WireHdr &h) {
    LoraHeader lh = LORA_HEADER__INIT;
    lh.destaddress   = h.dst;
    lh.destsubnet    = h.subnet;
    lh.senderaddress = h.src;
    lh.msgid         = h.fcnt;
    lh.burstindex    = h.burst_idx;
    lh.burstcount    = h.burst_cnt;
    lh.onmark        = h.onMark();
    lh.firestamped   = h.stamped();
    lh.fireround     = h.fire_round;
    lh.fireoffsetus  = h.fire_offset_us;
    return lh;
}
}  // namespace

Bytes downlinkToV0(const uint8_t *data, size_t len) {
    Bytes same(data, data + len);
    macwire::WireHdr h;
    size_t off = 0, blen = 0;
    if (len == 0 || data[0] != macwire::kVersion || !macwire::decode(data, len, &h, &off, &blen))
        return same;
    uint32_t sid = 0;
    const uint32_t nonce = nodeNonce(h.dst);
    Bytes body(data + off, data + off + blen);
    if (h.auth() == macwire::Auth::Session) {
        sessionOf(h.dst, &sid);
        auto pt = v1_open_session_frame(sid, nonce, h.src, h.dst, true, same);
        if (!pt) {
            // Keys no longer known (a re-key since): the header still reads, as
            // a v0 encrypted envelope around the raw ciphertext and tag.
            LoraHeader lh = v0HeaderFrom(h);
            EncryptedPayload e = ENCRYPTED_PAYLOAD__INIT;
            e.ciphertext.data = body.data();
            e.ciphertext.len  = body.size();
            e.tag.data = const_cast<uint8_t *>(data + len - macwire::kTagBytes);
            e.tag.len  = macwire::kTagBytes;
            LoraClientOperationMessage w = LORA_CLIENT_OPERATION_MESSAGE__INIT;
            w.header = &lh;
            w.cmd_case = LORA_CLIENT_OPERATION_MESSAGE__CMD_ENCRYPTED;
            w.encrypted = &e;
            return packWith(&w, lora_client_operation_message__get_packed_size,
                            lora_client_operation_message__pack);
        }
        body = *pt;
    }
    LoraClientOperationMessage op = LORA_CLIENT_OPERATION_MESSAGE__INIT;
    ProtobufCMessage *owned = nullptr;
    if (!v0OpFromBody(h, body, &op, &owned)) {
        if (owned) protobuf_c_message_free_unpacked(owned, nullptr);
        return same;
    }
    LoraHeader lh = v0HeaderFrom(h);
    uint8_t mic[framecrypto::kSessionCmacTagBytes];
    if (op.cmd_case == LORA_CLIENT_OPERATION_MESSAGE__CMD_LOGIN) {
        compute_login_mic(lh.destaddress, lh.destsubnet, lh.senderaddress, lh.msgid,
                          op.login->nonce, op.login->request_register, op.login->hub_rebooted, mic);
        op.login->mic.data = mic;
        op.login->mic.len  = sizeof(mic);
    }
    Bytes out;
    if (h.auth() == macwire::Auth::Session) {
        Bytes inner = packWith(&op, lora_client_operation_message__get_packed_size,
                               lora_client_operation_message__pack);
        auto enc = encrypt_then_cmac_seal(sid, nonce, h.src, h.dst, true, etmFields(true, sid, &lh),
                                          inner.data(), inner.size());
        EncryptedPayload e = ENCRYPTED_PAYLOAD__INIT;
        e.tag.data = enc.tag;
        e.tag.len  = sizeof(enc.tag);
        e.ciphertext.data = enc.ciphertext.data();
        e.ciphertext.len  = enc.ciphertext.size();
        LoraClientOperationMessage w = LORA_CLIENT_OPERATION_MESSAGE__INIT;
        w.header    = &lh;
        w.cmd_case  = LORA_CLIENT_OPERATION_MESSAGE__CMD_ENCRYPTED;
        w.encrypted = &e;
        out = packWith(&w, lora_client_operation_message__get_packed_size,
                       lora_client_operation_message__pack);
    } else {
        op.header = &lh;
        out = packWith(&op, lora_client_operation_message__get_packed_size,
                       lora_client_operation_message__pack);
    }
    if (op.cmd_case == LORA_CLIENT_OPERATION_MESSAGE__CMD_LOGIN) {
        op.login->mic.data = nullptr;   // owned by `mic`, not by the unpacked message
        op.login->mic.len  = 0;
    }
    protobuf_c_message_free_unpacked(owned, nullptr);
    return out;
}

}  // namespace v1bridge
}  // namespace proto_sim

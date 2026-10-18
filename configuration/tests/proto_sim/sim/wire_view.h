#pragma once
// wire_view.h -- read a frame the way a test needs to, in either wire format.
//
// S8 / W3: the seam and e2e scenarios run the REAL hub against the REAL node,
// and a handful of their assertions peek at a frame (is this the node's
// REGISTER, and which MAC does it carry; does this LOGIN ask for a REGISTER;
// who sent it). These helpers answer that for a v0 envelope or, in a
// MAC_WIRE_V1 build, for a v1 frame (fixed header + the PORT's body; REGISTER
// and LOGIN bodies are plaintext under K_AUTH, so no key is needed).

#include <cstddef>
#include <cstdint>
#include <vector>

#include "blinds.pb-c.h"
#include "MacWire.h"

namespace proto_sim {
namespace wire {

#if defined(MAC_WIRE_V1) && MAC_WIRE_V1
constexpr bool kV1 = true;
#else
constexpr bool kV1 = false;
#endif

// The frame is the node's REGISTER; *mac (optional) gets its MAC.
inline bool registerMac(const std::vector<uint8_t> &f, uint64_t *mac = nullptr) {
    if (kV1) {
        macwire::WireHdr h;
        size_t off = 0, blen = 0;
        if (!macwire::decode(f.data(), f.size(), &h, &off, &blen) ||
            h.port != macwire::kPortSession || h.auth() != macwire::Auth::KAuth)
            return false;
        ClientRegister *r = client_register__unpack(nullptr, blen, f.data() + off);
        if (r == nullptr) return false;
        if (mac) *mac = r->mac_addr;
        client_register__free_unpacked(r, nullptr);
        return true;
    }
    LoraClientResponseMessage *r = lora_client_response_message__unpack(nullptr, f.size(), f.data());
    if (r == nullptr) return false;
    const bool is = r->proto_case == LORA_CLIENT_RESPONSE_MESSAGE__PROTO_REGISTER && r->register_;
    if (is && mac) *mac = r->register_->mac_addr;
    lora_client_response_message__free_unpacked(r, nullptr);
    return is;
}

// The frame is a LOGIN; *request_register gets its flag.
inline bool loginRequestRegister(const std::vector<uint8_t> &f, bool *request_register) {
    if (kV1) {
        macwire::WireHdr h;
        size_t off = 0, blen = 0;
        if (!macwire::decode(f.data(), f.size(), &h, &off, &blen) || h.port != macwire::kPortSession)
            return false;
        LoginMsg *l = login_msg__unpack(nullptr, blen, f.data() + off);
        if (l == nullptr) return false;
        *request_register = l->request_register;
        login_msg__free_unpacked(l, nullptr);
        return true;
    }
    LoraClientOperationMessage *m = lora_client_operation_message__unpack(nullptr, f.size(), f.data());
    if (m == nullptr) return false;
    const bool is = m->cmd_case == LORA_CLIENT_OPERATION_MESSAGE__CMD_LOGIN && m->login;
    if (is) *request_register = m->login->request_register;
    lora_client_operation_message__free_unpacked(m, nullptr);
    return is;
}

// The frame's sender and destination addresses (false: not a frame).
inline bool addresses(const std::vector<uint8_t> &f, uint32_t *src, uint32_t *dst) {
    if (kV1) {
        macwire::WireHdr h;
        size_t off = 0, blen = 0;
        if (!macwire::decode(f.data(), f.size(), &h, &off, &blen)) return false;
        *src = h.src;
        *dst = h.dst;
        return true;
    }
    LoraClientResponseMessage *r = lora_client_response_message__unpack(nullptr, f.size(), f.data());
    if (r == nullptr || r->header == nullptr) {
        if (r) lora_client_response_message__free_unpacked(r, nullptr);
        return false;
    }
    *src = r->header->senderaddress;
    *dst = r->header->destaddress;
    lora_client_response_message__free_unpacked(r, nullptr);
    return true;
}

}  // namespace wire
}  // namespace proto_sim

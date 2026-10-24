#pragma once
// wire_view.h -- read a MAC wire v1 frame the way a test needs to.
//
// The seam and e2e scenarios run the REAL hub against the REAL node, and a
// handful of their assertions peek at a frame (is this the node's REGISTER,
// and which MAC does it carry; does this LOGIN ask for a REGISTER; who sent
// it). These helpers answer that from the fixed header and the PORT's body;
// REGISTER and LOGIN bodies are plaintext under K_AUTH, so no key is needed.

#include <cstddef>
#include <cstdint>
#include <vector>

#include "blinds.pb-c.h"
#include "MacWire.h"

namespace proto_sim {
namespace wire {

// The frame is the node's REGISTER; *mac (optional) gets its MAC.
inline bool registerMac(const std::vector<uint8_t> &f, uint64_t *mac = nullptr) {
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

// The frame is a LOGIN; *request_register gets its flag.
inline bool loginRequestRegister(const std::vector<uint8_t> &f, bool *request_register) {
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

// The frame's sender and destination addresses (false: not a frame).
inline bool addresses(const std::vector<uint8_t> &f, uint32_t *src, uint32_t *dst) {
    macwire::WireHdr h;
    size_t off = 0, blen = 0;
    if (!macwire::decode(f.data(), f.size(), &h, &off, &blen)) return false;
    *src = h.src;
    *dst = h.dst;
    return true;
}

}  // namespace wire
}  // namespace proto_sim

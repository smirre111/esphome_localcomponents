#pragma once
// HubWire.h -- the hub's protobuf side of MAC wire format v1 (stage S8,
// docs/mac-wire-format.md sections 2, 3 and 7).
//
// MacWire.h (shared with the node) owns the fixed header; this unit owns what
// the hub puts behind it and reads out of it:
//   * routeDownlink: which PORT (and whether ACK_REQ) a downlink's command goes
//     under -- the port table of section 3 as the hub's producers see it;
//   * packDownlinkBody: the ONE body message of section 3 (HubMacPdu or
//     HubAppPdu), with cmd_id for the Control commands (section 7);
//   * Uplink: a received v1 frame as the hub's internal Up (HubFrames.h):
//     the body decoded as the message its (direction, PORT) names, the MacAck +
//     ACKED_FCNT as an UpAck, or (AUTH = SESSION, not yet opened) the sealed
//     ciphertext and tag.
//
// HUB-ONLY (not vendored). No crypto here (HubLinkSecurity), no logging.

#include <cstddef>
#include <cstdint>
#include <vector>

#include "esphome/components/blindsproto/blinds.pb-c.h"
#include "esphome/components/lora_client/HubFrames.h"
#include "esphome/components/lora_client/MacWire.h"

namespace hubwire
{


struct Route
{
    bool    ok{false};        // false: this command has no v1 downlink body
    uint8_t port{0};          // macwire::kPort*
    bool    ack_req{false};   // FLAGS.ACK_REQ: an Acked delivery (spec 2.5)
};

// LOGIN is not routed here (send_login builds it: PORT 1, AUTH = K_AUTH).
Route routeDownlink(hubframes::DownCase c);

// The body for `op`'s command, packed into out[0..cap). cmd_id goes into
// LoraCoverOperation.cmd_id / SysopCmd.cmd_id and is ignored for every other
// command. False when the command has no v1 body or it does not fit.
bool packDownlinkBody(const hubframes::Down *op, uint32_t cmd_id, uint8_t *out,
                      size_t cap, size_t *len);

// Header + body + (tag room when AUTH != NONE), in bytes.
inline size_t frameLen(const macwire::WireHdr &h, size_t body_len)
{
    return macwire::headerLen(h) + body_len + macwire::tagLen(h.flags);
}

// header || body || (8 zero bytes of tag room when AUTH != NONE) into *out.
// False when the header is not encodable or the frame would exceed 255 B.
bool assemble(const macwire::WireHdr &h, const uint8_t *body, size_t body_len,
              std::vector<uint8_t> *out);

// The internal header of a wire header (the fields dispatch reads).
void toHdr(const macwire::WireHdr &h, hubframes::Hdr *out);

// A received v1 frame. Owns the unpacked body; view() is the internal Up
// dispatch consumes (valid while this object lives).
class Uplink
{
public:
    Uplink();
    ~Uplink();
    Uplink(const Uplink &) = delete;
    Uplink &operator=(const Uplink &) = delete;

    // Decodes `body` as the node->hub message PORT names (NodeMacPdu, the
    // ClientRegister of PORT 1, NodeAppPdu) and builds the view. False when it
    // does not decode, carries no case, or a MacAck arrives without FLAGS.ACK.
    bool parse(const macwire::WireHdr &h, const uint8_t *body, size_t len);

    // An AUTH = SESSION frame before it is opened: the header, and the
    // ciphertext and tag pointing into the caller's bytes (which must outlive
    // this object).
    void sealed(const macwire::WireHdr &h, const uint8_t *body, size_t len, const uint8_t *tag);

    hubframes::Up *view() { return &this->msg_; }

private:
    bool viewMac_(const macwire::WireHdr &h);
    bool viewApp_();
    void reset_();

    ::NodeMacPdu     *mac_{nullptr};
    ::NodeAppPdu     *app_{nullptr};
    ::ClientRegister *reg_{nullptr};
    hubframes::Hdr    hdr_{};
    hubframes::UpAck  ack_{};
    hubframes::Sealed sealed_{};
    hubframes::Up     msg_{};
};

}  // namespace hubwire

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
//   * Uplink: a received v1 body decoded as the message its (direction, PORT)
//     names, plus a v0-shaped view of it (LoraClientResponseMessage with a
//     LoraHeader made from the wire header, and a CommandAck made from MacAck +
//     ACKED_FCNT) so the hub's dispatch keeps one shape until W5 deletes v0.
//
// Compiled in BOTH configurations (MAC_WIRE_V1 on or off): only the listener's
// and the tracker's wiring is switched, so this unit's tests always run.
//
// HUB-ONLY (not vendored). No crypto here (HubLinkSecurity), no logging.

#include <cstddef>
#include <cstdint>
#include <vector>

#include "esphome/components/blindsproto/blinds.pb-c.h"
#include "esphome/components/lora_client/MacWire.h"

namespace hubwire
{

// The hub's own short address (SRC of every downlink).
static constexpr uint8_t kHubAddr = 0x01;

struct Route
{
    bool    ok{false};        // false: this command has no v1 downlink body
    uint8_t port{0};          // macwire::kPort*
    bool    ack_req{false};   // FLAGS.ACK_REQ: an Acked delivery (spec 2.5)
};

// LOGIN is not routed here (send_login builds it: PORT 1, AUTH = K_AUTH), nor
// is ENCRYPTED (a v0 envelope, never a v1 body).
Route routeDownlink(LoraClientOperationMessage__CmdCase c);

// The body for `op`'s command, packed into out[0..cap). cmd_id goes into
// LoraCoverOperation.cmd_id / SysopCmd.cmd_id and is ignored for every other
// command. False when the command has no v1 body or it does not fit.
bool packDownlinkBody(const LoraClientOperationMessage *op, uint32_t cmd_id, uint8_t *out,
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

// The v0 LoraHeader view of a wire header (the fields dispatch reads).
void toLoraHeader(const macwire::WireHdr &h, ::LoraHeader *out);

// An AUTH = SESSION uplink as the v0 outer envelope the encrypted pipeline
// walks (header from the wire, EncryptedPayload{ciphertext = body, tag}),
// protobuf-c owned: free with lora_client_response_message__free_unpacked.
::LoraClientResponseMessage *sealedOuterV0(const macwire::WireHdr &h, const uint8_t *body,
                                           size_t body_len, const uint8_t *tag);

// A received v1 body, decoded. Owns the unpacked message; view() is the v0
// envelope dispatch consumes (valid while this object lives).
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

    ::LoraClientResponseMessage *view() { return &this->msg_; }
    // The view as a protobuf-c owned message (pack + unpack), for the paths
    // that take ownership (handle_register_, set_response's tail).
    ::LoraClientResponseMessage *cloneV0() const;

private:
    bool viewMac_(const macwire::WireHdr &h);
    bool viewApp_();
    void reset_();

    ::NodeMacPdu               *mac_{nullptr};
    ::NodeAppPdu               *app_{nullptr};
    ::ClientRegister           *reg_{nullptr};
    ::LoraHeader                hdr_;
    ::CommandAck                ack_;
    ::LoraClientResponseMessage msg_;
};

}  // namespace hubwire

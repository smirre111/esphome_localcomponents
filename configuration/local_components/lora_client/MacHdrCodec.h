#pragma once
// MacHdrCodec.h -- the MAC header decoders: the v1 wire header (fromWireHdr)
// and the hub's internal header (fromHdr, HubFrames.h) into a mac::MacHdr.
//
// HUB-ONLY (not vendored): the node decodes its own frames.

#include "esphome/components/lora_client/HubFrames.h"
#include "esphome/components/lora_client/MacSap.h"
#include "esphome/components/lora_client/MacWire.h"

namespace machdr
{

// MAC wire v1 (S8): the fixed header, already decoded by macwire::decode. The
// one function spec section 10 asks for; every field fits by construction.
inline void fromWireHdr(const macwire::WireHdr &h, mac::MacHdr *out)
{
    *out               = mac::MacHdr{};
    out->dst           = h.dst;
    out->src           = h.src;
    out->subnet        = h.subnet;
    out->fcnt          = h.fcnt;
    out->port          = static_cast<mac::Port>(h.port);
    out->burst_idx     = h.burst_idx;
    out->burst_cnt     = h.burst_cnt;
    out->on_mark       = h.onMark();
    out->stamped       = h.stamped();
    out->fire_round    = h.fire_round;
    out->fire_off_us   = h.fire_offset_us;
    out->session_nonce = h.sessionOpening() ? h.node_nonce : 0;
}

// The internal header (HubFrames.h) as the MacHdr a child or MAC-2 reads.
// Every field came from the one-byte wire fields, so this cannot fail; the
// width check stays as a guard for hand-built headers.
inline bool fromHdr(const hubframes::Hdr *h, mac::MacHdr *out)
{
    *out = mac::MacHdr{};
    if (h == nullptr)
        return true;
    if (h->destaddress > 0xFF || h->senderaddress > 0xFF || h->destsubnet > 0xFF ||
        h->burstindex > 0xFF || h->burstcount > 0xFF)
        return false;
    out->dst           = static_cast<mac::Addr>(h->destaddress);
    out->src           = static_cast<mac::Addr>(h->senderaddress);
    out->subnet        = static_cast<uint8_t>(h->destsubnet);
    out->fcnt          = h->msgid;
    out->burst_idx     = static_cast<uint8_t>(h->burstindex);
    out->burst_cnt     = static_cast<uint8_t>(h->burstcount);
    out->on_mark       = h->onmark;
    out->stamped       = h->firestamped;
    out->fire_round    = h->fireround;
    out->fire_off_us   = h->fireoffsetus;
    out->session_nonce = h->sessionnonce;
    return true;
}

}  // namespace machdr

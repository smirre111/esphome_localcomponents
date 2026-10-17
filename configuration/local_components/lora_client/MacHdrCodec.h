#pragma once
// MacHdrCodec.h -- decode today's protobuf LoraHeader into a mac::MacHdr.
//
// Stage S4 of the MAC separation refactor (docs/mac-separation-layering-plan.md).
// One decoder for the two places that hand a header to MAC-2: the listener
// (seal, uplink verify) and the tracker's per-copy CopySealer call. At S8 the
// header becomes fixed-offset bytes and this file goes away.
//
// HUB-ONLY (not vendored): the node decodes its own frames.
//
// Refuses (returns false) a header whose address, subnet or burst field does not
// fit MacHdr's one-byte width. Neither side ever sends one, and truncating it
// instead would map two different wire headers onto one CMAC input. A missing
// header decodes as all-zero.

#include "esphome/components/blindsproto/blinds.pb-c.h"
#include "esphome/components/lora_client/MacSap.h"

namespace machdr
{

inline bool fromLoraHeader(const ::LoraHeader *h, mac::MacHdr *out)
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

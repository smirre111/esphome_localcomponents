#pragma once
// UplinkDemux.h -- MAC-0's uplink demultiplexer (review HB23). HUB-ONLY.
//
// The tracker used to hand every received frame to EVERY listener, and each
// one decoded the v1 header, allocated an Uplink view, ran the protobuf unpack
// and only then discarded the frame on address: N decodes per frame at N nodes.
// The header is decoded ONCE here and the frame goes to the listener(s) whose
// short address is the frame's SRC. The one exception is a REGISTER (PORT 1,
// AUTH = K_AUTH): it comes from a node whose address may be new or changed and
// is matched on the factory MAC inside the body, which only the listeners read,
// so it still goes to every listener. A frame that is not v1 goes nowhere.
//
// Pure: no heap, no logging, no ESPHome dependency (host-tested).

#include <cstddef>
#include <cstdint>

#include "esphome/components/lora_client/MacWire.h"

namespace uplinkdemux
{

enum class Route : uint8_t
{
    Drop,   // not a v1 frame: nobody can use it
    One,    // to the listener(s) whose short address is `src`
    All,    // REGISTER: every listener (matched on the MAC in the body)
};

struct Decision
{
    Route   route{Route::Drop};
    uint8_t src{0};
};

inline Decision route(const uint8_t *frame, size_t len)
{
    macwire::WireHdr h;
    size_t off = 0, blen = 0;
    if (frame == nullptr || !macwire::decode(frame, len, &h, &off, &blen))
        return Decision{Route::Drop, 0};
    if (h.port == macwire::kPortSession && h.auth() == macwire::Auth::KAuth)
        return Decision{Route::All, h.src};
    return Decision{Route::One, h.src};
}

// Hand the frame to the listeners the decision names. `addressOf(c)` gives a
// listener's short address, `deliver(c)` hands it the frame. Returns how many
// listeners got it.
template <typename Container, typename AddrFn, typename DeliverFn>
size_t dispatch(const uint8_t *frame, size_t len, const Container &listeners, AddrFn addressOf,
                DeliverFn deliver)
{
    const Decision d = route(frame, len);
    size_t n = 0;
    if (d.route == Route::Drop)
        return 0;
    for (const auto &l : listeners)
    {
        if (d.route == Route::One && addressOf(l) != d.src)
            continue;
        deliver(l);
        ++n;
    }
    return n;
}

}  // namespace uplinkdemux

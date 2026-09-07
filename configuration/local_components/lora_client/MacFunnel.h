#pragma once

#include <stdint.h>

// ---------------------------------------------------------------------------
// MacFunnel — the MAC-layer frame funnel and the rates derived from it.
//
// See configuration/docs/mac-layer.md section 6. Each stage is counted
// separately because each has a different owner and a different fix:
//
//   0 offered          hub log        (not here — only the sender knows)
//   1 on air           witness radio  (not here — needs a third receiver)
//   2 detected         RxDone or RxTimeout fired
//   3 crcValid         RegIrqFlags said the payload checked out
//   4 parsed           protobuf decoded at all
//   5 addressed        the MAC-0 address filter passed
//   6 counterAccepted  MAC-1 replay window admitted it
//   7 micValid         MAC-2 authenticated it
//
// Stages 4-7 are NOT errors when they drop a frame: on a shared broadcast
// channel a node hears its neighbours constantly, and discarding their traffic
// is the filter working. Only 2->3 is loss.
//
// THE THREE RATES ARE NAMED APART ON PURPOSE. All three get called "frame
// error rate" in conversation and they have different fixes:
//
//   FER_air  = 1 - crcValid / onAir      channel: interference, range
//   FER_link = 1 - crcValid / offered    channel PLUS the transmitter's own
//                                        failures — the only one that catches a
//                                        frame that was never emitted, e.g. a
//                                        RegOpMode=TX write silently skipped on
//                                        a one-tick semaphore timeout
//   WMR      = 1 - windowsHit / windowsArmed    TIMING, and mode-specific
//
// FER_link - FER_air is the transmitter's own loss, and it is observable ONLY
// with the witness receiver. Without one the two collapse into a single number
// and "the hub did not send it" is indistinguishable from "the node did not
// hear it" — opposite fixes, same symptom.
//
// WMR is what actually distinguishes the three modes. FER is a property of the
// channel and should be within noise across modes at equal payload and power;
// if it is not, the modes are not being compared fairly and no WMR comparison
// between them means anything.
//
// Deliberately NOT here: command success rate. It mixes MAC loss with
// application retry and idempotency — the right number for judging the product
// and the wrong one for judging a mode.
//
// Dependency-free, so the arithmetic is verified on the host.
// ---------------------------------------------------------------------------

namespace macfunnel
{

struct Counters
{
    // Stage 2-3: what the radio saw.
    uint32_t detected{0};
    uint32_t crc_valid{0};
    uint32_t crc_errors{0};

    // Stage 4-7: what the MAC did with it. A frame dropped here is usually a
    // neighbour's, not a fault.
    uint32_t parsed{0};
    uint32_t parse_failures{0};   // unpack said no: corrupt, or a foreign protocol
    uint32_t addressed{0};
    uint32_t foreign{0};          // addressed to someone else
    uint32_t counter_accepted{0}; // MAC-1
    uint32_t duplicates{0};       // MAC-1 rejected: replay or retransmission
    uint32_t mic_valid{0};        // MAC-2
    uint32_t mic_failures{0};

    // Windows: the mode's own decisions, reported as VALUES UNDER TEST rather
    // than used as ground truth.
    uint32_t windows_armed{0};
    uint32_t windows_hit{0};

    void reset() { *this = Counters{}; }

    void noteDetected(bool crc_ok)
    {
        detected++;
        if (crc_ok) crc_valid++; else crc_errors++;
    }
    void noteParsed(bool ok)        { if (ok) parsed++;           else parse_failures++; }
    void noteAddressed(bool mine)   { if (mine) addressed++;      else foreign++; }
    void noteCounter(bool accepted) { if (accepted) counter_accepted++; else duplicates++; }
    void noteMic(bool ok)           { if (ok) mic_valid++;        else mic_failures++; }
    void noteWindow(bool hit)       { windows_armed++; if (hit) windows_hit++; }
};

// Rates in parts per million, so they are exact integers on a device with no
// FPU worth using and can be compared without worrying about float formatting.
// A denominator of zero yields 0 rather than a division fault: "no frames
// offered" is not "no errors", and the caller must check the denominator — the
// counters travel with the rate for exactly that reason.
constexpr uint32_t kPpmScale = 1000000;

constexpr uint32_t lossPpm(uint32_t good, uint32_t total)
{
    return (total == 0 || good >= total)
               ? 0u
               : (uint32_t) (((uint64_t) (total - good) * kPpmScale) / total);
}

// Needs the witness receiver's count. Not derivable on either endpoint.
constexpr uint32_t ferAirPpm(const Counters &c, uint32_t on_air)
{
    return lossPpm(c.crc_valid, on_air);
}

// Needs the hub's offered count, joined on seq. The node can never compute
// this: it cannot know what it did not hear.
constexpr uint32_t ferLinkPpm(const Counters &c, uint32_t offered)
{
    return lossPpm(c.crc_valid, offered);
}

constexpr uint32_t wmrPpm(const Counters &c)
{
    return lossPpm(c.windows_hit, c.windows_armed);
}

// Duplicates as a fraction of what the address filter passed. In Mode A this is
// large and HEALTHY — a 17-copy burst is sixteen intentional duplicates.
constexpr uint32_t dupPpm(const Counters &c)
{
    const uint32_t seen = c.counter_accepted + c.duplicates;
    return seen == 0 ? 0u : (uint32_t) (((uint64_t) c.duplicates * kPpmScale) / seen);
}

// Non-zero is a session bug, never a channel effect: a corrupted frame fails
// CRC long before it reaches the MIC.
constexpr uint32_t micFailPpm(const Counters &c)
{
    const uint32_t checked = c.mic_valid + c.mic_failures;
    return checked == 0 ? 0u : (uint32_t) (((uint64_t) c.mic_failures * kPpmScale) / checked);
}

}  // namespace macfunnel

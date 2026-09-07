#pragma once

#include <stdint.h>

#include "LoraTiming.h"

// ---------------------------------------------------------------------------
// AckCache — answering a replayed command instead of dropping it (B4).
//
// THE GAP IT CLOSES. The protocol cannot currently express "arrived, but the
// ack was lost". The node's replay filter requires msgid > rx_id_, so a
// retransmitted command is dropped in silence; the hub, having heard no ack,
// retries; the node drops it again. The command HAS been executed — the blind
// already moved — and the hub reports failure. Required before single-copy
// downlink becomes normal, because that is when retries stop being rare.
//
// THE TRAP, AND IT IS THE WHOLE DESIGN. Mode A sends SEVENTEEN COPIES of every
// command. The node accepts copy 1 and sees copies 2..17 as duplicates. A naive
// cached ack answers all sixteen — sixteen uplinks per command, on a battery
// node, on a shared channel, for a command that already succeeded. That is
// worse than the bug it fixes.
//
// So a duplicate is only worth answering once the ORIGINAL ACK CAN NO LONGER BE
// IN FLIGHT. Copies of the same burst arrive within one burst span; a genuine
// hub retry arrives a retry interval later. The two are far apart in time —
// 1.45 s against 3 s — and that gap is the discriminator:
//
//   duplicate within kBurstSpanUs of first acceptance  -> silent, it is a copy
//   duplicate later, and not too soon after the last   -> re-ack, the ack was lost
//   more than kMaxReAcks                               -> give up, something
//                                                          else is wrong
//
// Dependency-free.
// ---------------------------------------------------------------------------

namespace ackcache
{

// A 17-copy burst spans 16 * 88 ms + one time on air. Anything inside this of
// the first acceptance is another copy of the same burst, not a retry.
static constexpr int64_t kBurstSpanUs =
    (int64_t) (loratiming::kBurstCopies - 1) * loratiming::kBurstCopyStrideUs +
    (int64_t) loratiming::timeOnAirUs(60);

// Never re-ack faster than this, whatever arrives. Bounds the uplink cost of a
// hub that has gone haywire.
static constexpr int64_t kMinReAckIntervalUs = 2000000;

// After this many, stop. If the hub still has not heard us, the problem is not
// a lost ack and more uplinks will not fix it.
static constexpr uint8_t kMaxReAcks = 3;

enum class Decision : uint8_t {
    NotADuplicate,   // caller proceeds with normal admission
    SilentCopy,      // another copy of the same burst: drop, say nothing
    ReAck,           // the ack was probably lost: send the cached one again
    Exhausted,       // re-acked enough; drop
};

struct Cache
{
    bool     valid{false};
    uint32_t msgid{0};
    int64_t  first_seen_us{0};
    int64_t  last_reack_us{0};
    uint8_t  reacks{0};

    void reset() { *this = Cache{}; }

    // Remember a command we have just accepted and answered.
    void note(uint32_t accepted_msgid, int64_t now_us)
    {
        this->valid         = true;
        this->msgid         = accepted_msgid;
        this->first_seen_us = now_us;
        this->last_reack_us = 0;
        this->reacks        = 0;
    }
};

// What to do with a frame whose msgid the replay filter rejected.
//
// `now_us` is monotonic microseconds. A clock that jumps BACKWARDS (this
// happens) must not turn a copy into a retry, so negative elapsed times are
// treated as "still inside the burst" — the conservative direction, because
// staying silent costs nothing while a spurious uplink costs battery.
inline Decision classify(const Cache &c, uint32_t msgid, int64_t now_us)
{
    if (!c.valid || msgid != c.msgid)
        return Decision::NotADuplicate;

    const int64_t since_first = now_us - c.first_seen_us;
    if (since_first < kBurstSpanUs)
        return Decision::SilentCopy;

    if (c.reacks >= kMaxReAcks)
        return Decision::Exhausted;

    if (c.last_reack_us != 0 && (now_us - c.last_reack_us) < kMinReAckIntervalUs)
        return Decision::SilentCopy;

    return Decision::ReAck;
}

// Record that a re-ack was sent.
inline void noteReAck(Cache &c, int64_t now_us)
{
    c.last_reack_us = now_us;
    if (c.reacks < 0xFF) c.reacks++;
}

}  // namespace ackcache

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

// One remembered command. The cache holds several, which is not a size tweak:
// a SINGLE entry is evicted by the next acked command, and the hub routinely
// sends one. Found end to end (e2e_test, ALostAckIsRecoveredByTheRetry...):
//
//   1. the node accepts a cover op as msgid 2 and acks it — the ack is lost;
//   2. the login-confirm path delivers ScheduleConfig as msgid 4, which the
//      node also acks, and that OVERWRITES the entry for msgid 2;
//   3. the hub retries the cover op, byte-identical, still msgid 2;
//   4. classify() finds msgid 4 in the cache, answers NotADuplicate, and the
//      frame is refused with no re-ack.
//
// So B4's whole purpose — making a lost ack recoverable — failed whenever any
// other acked command arrived in between, which is most of the time. Four
// entries cover every frame the hub can have in flight to one node.
struct Entry
{
    bool     valid{false};
    uint32_t msgid{0};
    int64_t  first_seen_us{0};
    int64_t  last_reack_us{0};
    uint8_t  reacks{0};
    uint32_t stamp{0};        // insertion order, for eviction
};

struct Cache
{
    static constexpr uint8_t kEntries = 4;

    Entry    entries[kEntries]{};
    uint32_t next_stamp{1};

    void reset() { *this = Cache{}; }

    // The entry for a msgid, or nullptr. Const and non-const, because classify
    // reads and noteReAck writes.
    const Entry *find(uint32_t id) const
    {
        for (uint8_t i = 0; i < kEntries; ++i)
            if (entries[i].valid && entries[i].msgid == id) return &entries[i];
        return nullptr;
    }
    Entry *find(uint32_t id)
    {
        for (uint8_t i = 0; i < kEntries; ++i)
            if (entries[i].valid && entries[i].msgid == id) return &entries[i];
        return nullptr;
    }

    // The most recently noted entry, or an empty one. What a log line and a
    // "has anything been acked" check want.
    const Entry &newest() const
    {
        static const Entry none{};
        const Entry *best = nullptr;
        for (uint8_t i = 0; i < kEntries; ++i)
            if (entries[i].valid && (!best || entries[i].stamp > best->stamp))
                best = &entries[i];
        return best ? *best : none;
    }

    // Convenience readers, so a caller that only cares about the last command
    // does not have to reach through newest() every time.
    bool     valid() const        { return newest().valid; }
    uint32_t msgid() const        { return newest().msgid; }
    uint8_t  reacks() const       { return newest().reacks; }
    int64_t  firstSeenUs() const  { return newest().first_seen_us; }
    int64_t  lastReackUs() const  { return newest().last_reack_us; }

    // Remember a command we have just accepted and answered. Re-noting an id
    // already held REFRESHES nothing — the first acceptance is what bounds the
    // re-ack budget, and resetting it here would let an attacker mint fresh
    // budget by replaying.
    void note(uint32_t accepted_msgid, int64_t now_us)
    {
        if (find(accepted_msgid) != nullptr) return;

        Entry *slot = nullptr;
        for (uint8_t i = 0; i < kEntries; ++i)
            if (!entries[i].valid) { slot = &entries[i]; break; }
        if (slot == nullptr)
        {
            // Evict the oldest. A command old enough to be the oldest of four
            // is one whose retry ladder has long since run out.
            slot = &entries[0];
            for (uint8_t i = 1; i < kEntries; ++i)
                if (entries[i].stamp < slot->stamp) slot = &entries[i];
        }
        *slot = Entry{};
        slot->valid         = true;
        slot->msgid         = accepted_msgid;
        slot->first_seen_us = now_us;
        slot->stamp         = this->next_stamp++;
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
    const Entry *e = c.find(msgid);
    if (e == nullptr)
        return Decision::NotADuplicate;

    const int64_t since_first = now_us - e->first_seen_us;
    if (since_first < kBurstSpanUs)
        return Decision::SilentCopy;

    if (e->reacks >= kMaxReAcks)
        return Decision::Exhausted;

    if (e->last_reack_us != 0 && (now_us - e->last_reack_us) < kMinReAckIntervalUs)
        return Decision::SilentCopy;

    return Decision::ReAck;
}

// Record that a re-ack was sent. Takes the msgid because the cache holds
// several commands and the budget belongs to the one being re-acked.
inline void noteReAck(Cache &c, uint32_t msgid, int64_t now_us)
{
    Entry *e = c.find(msgid);
    if (e == nullptr) return;
    e->last_reack_us = now_us;
    if (e->reacks < 0xFF) e->reacks++;
}

}  // namespace ackcache

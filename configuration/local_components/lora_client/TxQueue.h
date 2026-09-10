#pragma once

#include <stdint.h>
#include <stddef.h>

// ---------------------------------------------------------------------------
// TxQueue — a reordering, time-scheduled transmit queue (B1a).
//
// WHAT IT REPLACES. Downlinks reach the radio through ONE FIFO: send() pushes a
// buffer, sendTask pops it, sendPacketBurst blocks that task for the whole
// burst. B-1 made the per-frame TX POLICY expressible (how many copies, what
// stride) but placement is still "whenever the queue drains". Two things the
// design needs cannot be said at all:
//
//   * "not before round n+2" — section 4.5's deferral. A burst occupies
//     1450 ms and sendTask then blocks a further 400 ms, so a frame deferred
//     to round n+1 lands inside the same burst's response window on the same
//     serialised task. Deferral must be by TWO rounds.
//   * "behind nothing else" — the deferred frame must not then queue behind
//     unrelated traffic that arrived while it waited.
//
// Both are ORDERING statements, and a FIFO has no way to express either.
//
// WHAT THIS IS NOT. It does not transmit, own buffers, or know about radios: it
// orders SLOT INDICES into the existing buffer pool. That keeps it
// dependency-free and testable, and keeps the memory pool exactly as it is.
//
// THE ORDERING RULE, in one place:
//
//   1. an entry is ELIGIBLE only once now >= earliest_us
//   2. among eligible entries, LOWEST priority value goes first
//   3. ties broken by insertion order — FIFO within a priority, always
//
// Rule 3 is not a detail. Without a stable tiebreak, two frames queued for the
// same node at the same priority can swap, and the second overwrites the first
// at the far end — the retry ladder assumes a command and its retry arrive in
// the order they were built.
//
// Dependency-free.
// ---------------------------------------------------------------------------

namespace txqueue
{

// Lower goes first. Named rather than numbered at call sites so the intent
// survives: "behind nothing else" is a priority, not a magic 0.
enum class Priority : uint8_t {
    Immediate = 0,   // deferred traffic that has waited its two rounds
    Normal    = 1,   // ordinary commands
    Background = 2,  // beacons, grid publication, measurement traffic
};

static constexpr uint8_t kMaxEntries = 16;
static constexpr uint8_t kInvalidSlot = 0xFF;

struct Entry
{
    uint8_t  slot{kInvalidSlot};   // index into the caller's buffer pool
    int64_t  earliest_us{0};       // not before this instant
    Priority priority{Priority::Normal};
    uint32_t seq{0};               // insertion order; the stable tiebreak
    bool     used{false};
};

class Queue
{
  public:
    void clear()
    {
        for (uint8_t i = 0; i < kMaxEntries; ++i) entries_[i] = Entry{};
        next_seq_ = 0;
        count_    = 0;
    }

    uint8_t size() const { return this->count_; }
    bool    empty() const { return this->count_ == 0; }
    bool    full() const  { return this->count_ >= kMaxEntries; }

    // Returns false when full. The caller keeps its buffer and can retry or
    // drop with a log — silently discarding a downlink is how a command
    // disappears with no trace.
    bool push(uint8_t slot, int64_t earliest_us, Priority priority)
    {
        if (slot == kInvalidSlot) return false;
        for (uint8_t i = 0; i < kMaxEntries; ++i)
        {
            if (entries_[i].used) continue;
            entries_[i] = Entry{slot, earliest_us, priority, this->next_seq_++, true};
            this->count_++;
            return true;
        }
        return false;
    }

    // The best entry eligible at `now`, or kInvalidSlot if none is.
    //
    // "None eligible" is NOT the same as "empty": a queue holding only deferred
    // frames returns nothing now and something later, and a caller that treated
    // those alike would spin.
    uint8_t pop(int64_t now_us)
    {
        int64_t ignored = 0;
        return popDue(now_us, /*lead_us=*/0, ignored);
    }

    // pop(), but willing to release a frame `lead_us` BEFORE its instant, and
    // reporting the instant it was scheduled for.
    //
    // Both halves are needed to place a frame precisely. A queue that releases
    // a frame only once its instant has passed can never fire ON it — the
    // caller inherits the whole prepare cost (idle, preamble, FIFO clock-in)
    // plus a task wake, and B5's prepare/fire split has nothing to hold back.
    // Releasing early hands the caller the slack to prepare in, and
    // earliest_us_out is what it then fires against.
    //
    // earliest_us_out is 0 for a frame with no placement, which is the signal
    // to fire immediately rather than to wait for instant zero.
    uint8_t popDue(int64_t now_us, int64_t lead_us, int64_t &earliest_us_out)
    {
        earliest_us_out = 0;
        int8_t best = -1;
        for (uint8_t i = 0; i < kMaxEntries; ++i)
        {
            const Entry &e = entries_[i];
            if (!e.used || e.earliest_us > now_us + lead_us) continue;
            if (best < 0 || better_(e, entries_[best])) best = (int8_t) i;
        }
        if (best < 0) return kInvalidSlot;

        const uint8_t slot = entries_[best].slot;
        earliest_us_out    = entries_[best].earliest_us;
        entries_[best] = Entry{};
        this->count_--;
        return slot;
    }

    // When the next entry becomes eligible, for a caller that must wait rather
    // than poll. Returns `now_us` if something is already eligible, and
    // INT64_MAX if the queue is empty.
    int64_t nextEligibleUs(int64_t now_us) const
    {
        int64_t soonest = INT64_MAX;
        for (uint8_t i = 0; i < kMaxEntries; ++i)
        {
            const Entry &e = entries_[i];
            if (!e.used) continue;
            if (e.earliest_us <= now_us) return now_us;
            if (e.earliest_us < soonest) soonest = e.earliest_us;
        }
        return soonest;
    }

    // Peek without removing, for diagnostics and tests.
    const Entry *peek(uint8_t index) const
    {
        return index < kMaxEntries && entries_[index].used ? &entries_[index] : nullptr;
    }

  private:
    // Priority first, then insertion order. Never `earliest_us`: two frames
    // that became eligible in the same instant must still go out in the order
    // they were built.
    static bool better_(const Entry &a, const Entry &b)
    {
        if (a.priority != b.priority)
            return (uint8_t) a.priority < (uint8_t) b.priority;
        return a.seq < b.seq;
    }

    Entry    entries_[kMaxEntries]{};
    uint32_t next_seq_{0};
    uint8_t  count_{0};
};

// Section 4.5's deferral, as a function so the "two, not one" reasoning lives
// with the number rather than at a call site.
//
// A burst occupies 1450 ms and sendTask then blocks a further 400 ms — 1850 ms
// against a 1500 ms round — so one round is not enough to clear it.
//
// SUPERSEDED IN PRACTICE, and kept for the reasoning rather than the number.
// LORATracker::nextClearT0ForSlotUs() answers the same question from the
// MEASURED busy instant — burst_busy_until_us_, which already includes the last
// copy's air time and the response window — and lands the frame on the node's
// own next clear mark instead of on a fixed two-round approximation of it.
// send_aligned_() calls that, so every placed downlink is already deferred past
// a running burst.
//
// Deliberately NOT given a caller to close the §11a row: a producer that
// deferred by a fixed two rounds where a measured instant is available would be
// worse than the code it replaced, and "has a caller" was never the point of
// that list. The rule is implemented; this is the derivation.
static constexpr uint32_t kDeferRounds = 2;

constexpr int64_t deferUntilUs(int64_t now_us, uint32_t round_us)
{
    return now_us + (int64_t) kDeferRounds * (int64_t) round_us;
}

}  // namespace txqueue

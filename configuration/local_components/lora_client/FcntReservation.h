#pragma once
// FcntReservation.h -- a block of downlink FCNTs reserved on the ESPHome loop
// and spent from another task. HUB-ONLY (not vendored).
//
// Why: the esp_timer streams (MAC ping, ModeTest marks, drift ticks) build a
// frame per tick on the esp_timer task. They must not call the Sequencer there:
// allocating persists the counter through ESPHome preferences, which only the
// loop task may touch. So the loop reserves a whole run's FCNTs once
// (LORAListener::reserve_fcnt_block_) and the timer takes them from here.
//
// A block belongs to the session it was reserved in. Every FCNT is a CTR
// counter-block input: sealing a reserved FCNT under the NEXT session's keys
// would reuse keystream, because the next session counts up from 1 through the
// same numbers (review P1). So a block is
//   * invalidated explicitly whenever the counters restart or the keys change
//     (send_login, clearSessionKeys_, installSessionKeys_), and
//   * bound to the key epoch it was reserved under: take() refuses once the
//     peer's key epoch has moved, even if an invalidation were missed.
// The caller also drops a frame whose seal reports another epoch.
//
// Not thread-safe by itself: the caller holds the lock that guards the
// sequencer (LORAListener::session_tx_mutex_) around grant/take/invalidate.
// Pure: no heap, no logging, no ESPHome dependency (host-tested).

#include <cstdint>

namespace mac1
{

class FcntReservation
{
public:
    // Reserve [first, last] under `key_epoch`. An empty or inverted range
    // leaves the reservation invalid.
    void grant(uint32_t first, uint32_t last, uint32_t key_epoch)
    {
        valid_ = first != 0 && first <= last;
        next_  = valid_ ? first : 0;
        last_  = valid_ ? last : 0;
        epoch_ = key_epoch;
    }

    void invalidate()
    {
        valid_ = false;
        next_  = 0;
        last_  = 0;
    }

    // The next reserved FCNT, or 0 when there is none: never granted,
    // invalidated, exhausted, or the peer's key epoch has moved since the grant
    // (which also invalidates, so a later epoch match cannot revive it).
    uint32_t take(uint32_t key_epoch_now)
    {
        if (!valid_)
            return 0;
        if (key_epoch_now != epoch_)
        {
            invalidate();
            return 0;
        }
        const uint32_t fcnt = next_;
        if (fcnt == last_)
            invalidate();   // that was the last one
        else
            ++next_;
        return fcnt;
    }

    bool     valid() const { return valid_; }
    uint32_t keyEpoch() const { return epoch_; }
    uint32_t remaining() const { return valid_ ? last_ - next_ + 1 : 0; }

private:
    bool     valid_{false};
    uint32_t next_{0};
    uint32_t last_{0};
    uint32_t epoch_{0};
};

}  // namespace mac1

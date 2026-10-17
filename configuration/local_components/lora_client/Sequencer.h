#pragma once

#include <stdint.h>

// ---------------------------------------------------------------------------
// Sequencer.h -- MAC-1 (sequencing), stage S3 of the MAC-layer separation
// (mac-review.md sections 5.2 / 6).
//
// ONE implementation of the frame counter for both ends of the link: the tx
// counter (allocation, with persist-before-handout), the rx replay filter and
// the "last accepted" stamp the MAC-1 freshness query reads. One instance per
// link (the node has one, toward its hub; the hub has one per node).
//
// TWO-PHASE RECEIVE. MAC-1 cannot simply sit above or below MAC-2: the replay
// window may only move for a frame MAC-2 has AUTHENTICATED, yet MAC-0's timing
// bookkeeping needs MAC-1's verdict before that commit. So:
//     check(fcnt)            read-only: would this id be accepted?
//     ... MAC-2 verifies, MAC-0 consults the freshness class ...
//     commit(fcnt, now, persist)  admit it and move the window
//
// TWO POLICIES, one class:
//   SlidingWindow   (node) -- high-water mark + 64-bit bitmap of ids seen
//                   below it, so a retried command (pack-once: byte-identical,
//                   same id) that was overtaken by later frames is still
//                   accepted exactly once, and a replay never is (the DTLS /
//                   IPsec window).
//   ForwardRatchet  (hub)  -- accept only id in (rx, rx + window]; commit sets
//                   rx := id. The hub's uplinks are never retransmitted
//                   byte-identically, so it has not needed the bitmap. Moving
//                   the hub to SlidingWindow is a separate decision.
//
// PERSISTENCE stays with the caller (the node's SessionManager owns the NVS
// blob, unchanged). This class only decides WHEN a snapshot must be written
// and calls the caller's PersistHook -- a plain function pointer + context, no
// virtual call, no heap. The caller writes (txId(), rxId()) and, when the
// write landed, calls notePersisted().
//
// SHARED, NODE OWNS IT. Dependency-free (<stdint.h> only), so the GoogleTest
// harness and the hub's ESPHome build compile it as is. The hub vendors a
// byte-identical copy into local_components/lora_client/ under a drift gate;
// edit THIS file, never the vendored one.
// ---------------------------------------------------------------------------

namespace mac1
{

enum class Policy : uint8_t
{
    SlidingWindow,    // node: high-water mark + replay bitmap
    ForwardRatchet,   // hub: strictly forward
};

// The forward jump accepted beyond the high-water mark. msgid increments by one
// per frame (plus a reboot margin), so a larger jump is a corrupt or spurious
// frame -- ratcheting to it would wedge the link until the next login.
static constexpr uint32_t kForwardWindow = 1024;
// How far below the mark a frame may still be accepted once (SlidingWindow).
// One word of bitmap; an attacker replaying something old gets 64 ids of reach.
static constexpr uint32_t kReplayWindow = 64;
// The CTR counter block holds 31 bits of the frame counter (FrameCrypto.h
// kMsgidCtrLimit): an id >= 2^31 would reuse a keystream.
static constexpr uint32_t kFcntLimit = 0x80000000u;

struct Config
{
    Policy   policy{Policy::SlidingWindow};
    // tx: an id at last_persisted + margin is persisted BEFORE it is handed
    // out (N-2); the restore adds the same margin, so ids handed out since the
    // last write are never reused after a reboot.
    uint32_t tx_persist_margin{64};
    // rx: persisted every margin ids advanced (N-3); the restore adds the margin
    // and marks it all seen. Must stay below kReplayWindow.
    uint32_t rx_persist_margin{16};
};

// The node's configuration (what SessionManager has always used).
static constexpr Config kNodeConfig{Policy::SlidingWindow, 64, 16};

// "Write the current (txId, rxId) snapshot; return true if it landed."
struct PersistHook
{
    bool (*fn)(void *ctx){nullptr};
    void *ctx{nullptr};
    bool operator()() const { return fn != nullptr && fn(ctx); }
};

enum class AllocStatus : uint8_t
{
    Ok,
    Spent,           // the 31-bit space is exhausted: a fresh session is needed
    PersistFailed,   // the margin id could not be persisted: send nothing
};

struct Alloc
{
    uint32_t    fcnt;     // 0 unless status == Ok
    AllocStatus status;
};

enum class Commit : uint8_t
{
    Rejected,    // not admissible (replay, too old, or a jump beyond the window)
    Advanced,    // a new high-water mark
    Reordered,   // SlidingWindow only: an id below the mark, seen for the first time
};

class Sequencer
{
public:
    explicit Sequencer(const Config &cfg = kNodeConfig) : cfg_(cfg) {}

    const Config &config() const { return cfg_; }

    // ---- transmit -------------------------------------------------------------

    uint32_t txId() const { return tx_; }

    // The next frame counter. Refused BEFORE advancing when the space is spent;
    // when the candidate crosses the persist margin, `persist` runs first and a
    // failed write rolls the counter back (fail closed: an id a reboot could
    // hand out again must never go on the air).
    Alloc allocate(const PersistHook &persist)
    {
        if (tx_ >= kFcntLimit - 1u)
            return Alloc{0, AllocStatus::Spent};
        const uint32_t candidate = tx_ + 1;
        tx_ = candidate;
        if (candidate < last_persisted_tx_ + cfg_.tx_persist_margin)
            return Alloc{candidate, AllocStatus::Ok};
        if (persist() && last_persisted_tx_ == candidate)
            return Alloc{candidate, AllocStatus::Ok};
        tx_ = candidate - 1;
        return Alloc{0, AllocStatus::PersistFailed};
    }

    // ---- receive ----------------------------------------------------------------

    // Read-only: would commit() accept this id? Shares its arithmetic with
    // commit(), so the two can never disagree.
    bool check(uint32_t fcnt) const
    {
        if (fcnt > rx_ && fcnt <= rx_ + kForwardWindow)
            return true;                               // a forward jump inside the window
        if (cfg_.policy == Policy::ForwardRatchet)
            return false;
        // At or below the mark: a reorder is accepted once, a replay never.
        const uint32_t behind = rx_ - fcnt;            // fcnt <= rx_ here
        if (behind >= kReplayWindow)
            return false;
        return (seen_ & (1ull << behind)) == 0;
    }

    // Admit `fcnt` and move the window -- only for a frame MAC-2 has
    // authenticated (or, before any session exists, the only sequencing there
    // is). Persists through `persist` when the rx margin is crossed, then if a
    // tx snapshot is overdue. `now_us` is stamped into lastAcceptedUs().
    Commit commit(uint32_t fcnt, int64_t now_us, const PersistHook &persist)
    {
        if (!check(fcnt))
            return Commit::Rejected;
        Commit result;
        if (fcnt > rx_)
        {
            const uint32_t advance = fcnt - rx_;
            seen_ = (advance >= 64u) ? 0u : (seen_ << advance);
            seen_ |= 1ull;                             // bit 0 is the mark itself
            rx_ = fcnt;
            if (rx_ - last_persisted_rx_ >= cfg_.rx_persist_margin)
                persist();
            if (txPersistDue())
                persist();
            result = Commit::Advanced;
        }
        else
        {
            // Behind the mark: the mark does NOT move (moving it backwards would
            // let the next replay in); only the id's bit is recorded.
            seen_ |= (1ull << (rx_ - fcnt));
            result = Commit::Reordered;
        }
        last_accepted_ = fcnt;
        last_accepted_us_ = now_us;
        return result;
    }

    uint32_t rxId() const { return rx_; }
    // The id / instant commit() last accepted (RAM only). The freshness query's
    // CurrentBurstCopy class: a copy of the burst being accepted, heard again
    // within that burst's span, is not a cold replay.
    uint32_t lastAcceptedFcnt() const { return last_accepted_; }
    int64_t  lastAcceptedUs() const { return last_accepted_us_; }

    // ---- lifecycle --------------------------------------------------------------

    // A new session starts both directions over. The bitmap goes with the mark:
    // bits under the OLD numbering would refuse the new session's first frames.
    // The persisted marks are deliberately left alone (unchanged behaviour).
    void reset()
    {
        tx_ = 0;
        rx_ = 0;
        seen_ = 0;
    }

    // Install a persisted snapshot: both margins added, the rx margin marked
    // fully SEEN (an empty bitmap would let every id at or below the restored
    // mark be replayed once after a reboot), and the result counts as persisted.
    void restore(uint32_t saved_tx, uint32_t saved_rx)
    {
        rx_ = saved_rx + cfg_.rx_persist_margin;
        seen_ = ~0ull;
        last_persisted_rx_ = rx_;
        tx_ = saved_tx + cfg_.tx_persist_margin;
        last_persisted_tx_ = tx_;
    }

    // The caller's write of (txId(), rxId()) landed.
    void notePersisted()
    {
        last_persisted_tx_ = tx_;
        last_persisted_rx_ = rx_;
    }

    // A full tx margin has passed since the last snapshot (throttled save).
    bool txPersistDue() const { return tx_ >= last_persisted_tx_ + cfg_.tx_persist_margin; }

    // Test seam: put the tx counter near a boundary. Production never calls it.
    void seedTxForTest(uint32_t id) { tx_ = id; }

private:
    Config   cfg_;
    uint32_t tx_{0};
    uint32_t rx_{0};
    // Bit n set: id (rx_ - n) has been accepted. Bit 0 is rx_ itself.
    uint64_t seen_{0};
    uint32_t last_accepted_{0};
    int64_t  last_accepted_us_{0};
    uint32_t last_persisted_tx_{0};
    uint32_t last_persisted_rx_{0};
};

}  // namespace mac1

#pragma once

#include <stddef.h>
#include <stdint.h>

// ---------------------------------------------------------------------------
// ModeBSupervisor — keep a node that SHOULD be in Mode B actually in Mode B.
// Hub-only (not vendored from the node repository), dependency-free, host-tested.
//
// THE BEHAVIOUR THIS REPLACES (measured 2026-09-19 .. 2026-10-07, see
// docs/bench-runbook.md "A SWEEP POINT CANNOT BOOTSTRAP ITSELF" and the Mode B
// soak): switching "Timed Mode (Mode B)" ON publishes the grid, and that is all.
// A node adopts the grid but NEVER promotes by itself: promotion needs >= 8
// phase samples from >= 2 CAUGHT frames, an idle Mode-A node (6 % duty) catches
// too few, it demotes after resync_max_s (352 s) and re-requests the grid every
// ~6 min. Waiting for it is a deadlock. A short stretch of marks (a ModeTest B,
// one single-copy mark per 1.5 s round) bootstraps promotion in ~13.6-16 s, and
// afterwards the node HOLDS Mode B on its own (hub beacons every 349.5 s;
// 14.5 h and 9 h holds, zero demotions).
//
// So "stay in Mode B across node sleep / reboot / hub reboot" = after any event
// that costs the node its promotion, the HUB runs a short warm-up again. This
// header is the policy; the glue (what a "warm-up" physically is, where the
// inputs come from) lives in lora_client.cpp.
//
// THE RULES
//   * Only a node whose Timed Mode switch is ON, that is not an auto-mode node,
//     that holds a confirmed session, is awake, and whose GridSync has been
//     ACKNOWLEDGED and has SETTLED, is ever warmed up. (Runbook: a warm-up
//     pressed 46 s after adoption measured windows 0/0; at 73 s and 171 s it
//     measured 193/192 and 191/191, hence kGridSettleS.)
//   * A node the hub already believes promoted is left alone.
//   * ONE warm-up on the air at a time, hub-wide (Arbiter, FIFO). A start is
//     preceded by a random jitter so 32 nodes waking together do not all ask in
//     the same tick; the FIFO is what actually guarantees non-overlap.
//   * It yields to cover work: it never starts while THIS node has an op in
//     flight, it aborts the moment one is issued for it, and ANY op issued hub-
//     wide holds the arbiter off for kOpHoldOffS so a queued warm-up does not
//     start into a fresh command.
//   * A warm-up that did not end in a promotion is retried on a capped backoff
//     (kBackoffS: 2, 5, 15 min, then hourly) - never in a tight loop.
//   * Success is the node's own word: its PhaseReport says timed RX is active
//     (the node announces a mode change within kModeAnnounceMinS = 60 s), or the
//     ModeTest REPORT says it ran Mode B with armed windows.
// ---------------------------------------------------------------------------

namespace modebsup
{

// Supervisor tick. The listener calls Supervisor::tick() this often.
static constexpr uint32_t kTickS = 2;

// Seconds after the node acknowledged our GridSync before a warm-up may start.
static constexpr uint32_t kGridSettleS = 75;

// Warm-up length (the ModeTest duration). Promotion was measured at 13.6 s
// (runbook) and ~13-16 s (2026-10-07 soak); 60 s leaves a ~4x margin and costs
// 40 single-copy marks.
static constexpr uint32_t kWarmupS = 60;

// After the warm-up ends: how long to wait for the node's announcement that it
// is promoted. The node announces a mode change at most once per
// kModeAnnounceMinS (60 s), so 90 s covers one full floor plus airtime.
static constexpr uint32_t kVerifyGraceS = 90;

// A warm-up that yielded (op in flight) or found the node busy tries again
// after this. Not a failure: the backoff is untouched.
static constexpr uint32_t kYieldRetryS = 30;

// Random start delay, 0..kJitterMaxS, applied each time a node becomes due.
static constexpr uint32_t kJitterMaxS = 20;

// A hub-wide op (any node) postpones the next grant by this long.
static constexpr uint32_t kOpHoldOffS = 10;

// After a SUCCESS: no new warm-up for this long (flap guard).
static constexpr uint32_t kRearmHoldS = 300;

// The token can be held no longer than a warm-up (+ start burst, hub stop timer
// at duration + 5 s, one tick of hand-over): a holder that never releases cannot
// stall the fleet.
static constexpr uint32_t kLeaseS = kWarmupS + 30;

// Failure backoff by consecutive failures (1-based), capped at the last entry.
static constexpr uint32_t kBackoffS[4] = {120, 300, 900, 3600};

constexpr uint32_t backoffS(uint32_t failures)
{
    return failures == 0 ? 0
         : failures > 4  ? kBackoffS[3]
                         : kBackoffS[failures - 1];
}

// Worst-case time for `n` nodes that all need re-establishing at once, counting
// only the serialised part: one warm-up occupies the token for the hub's stop
// timer (kWarmupS + 5) plus up to two ticks of hand-over; the settle and the
// jitter of every node run in parallel, once, in front of the queue.
constexpr uint32_t worstCaseAllNodesS(uint32_t n)
{
    return kGridSettleS + kJitterMaxS + n * (kWarmupS + 5 + 2 * kTickS);
}

// An uptime-seconds comparison that survives uint32 wrap.
constexpr bool reached(uint32_t now_s, uint32_t t_s)
{
    return static_cast<int32_t>(now_s - t_s) >= 0;
}

// The ModeTest report shows the node genuinely ran timed windows.
constexpr bool reportShowsModeB(uint32_t mode, uint32_t windows_armed, uint32_t windows_hit)
{
    return mode == 2u && windows_armed > 0u && windows_hit > 0u;
}

// --- Hub-wide token --------------------------------------------------------
//
// FIFO of requesters + at most one holder with a lease. Pure and passive: it
// never sleeps or times anything itself, the caller supplies now_s.
class Arbiter
{
public:
    static constexpr size_t kCap = 64;

    // Idempotent. False when the queue is full.
    bool request(uint32_t id)
    {
        if (this->queued(id) || (this->has_ && this->holder_ == id))
            return true;
        if (this->n_ >= kCap)
            return false;
        this->q_[this->n_++] = id;
        return true;
    }

    // True when `id` holds the token now. Only the head of the queue is ever
    // promoted, and not while an op hold-off is running.
    bool granted(uint32_t id, uint32_t now_s)
    {
        this->expire(now_s);
        if (!this->has_ && this->n_ > 0 && this->q_[0] == id &&
            reached(now_s, this->hold_until_))
        {
            this->popFront();
            this->has_       = true;
            this->holder_    = id;
            this->lease_end_ = now_s + kLeaseS;
        }
        return this->has_ && this->holder_ == id;
    }

    bool holds(uint32_t id, uint32_t now_s)
    {
        this->expire(now_s);
        return this->has_ && this->holder_ == id;
    }

    // Holder gives it up, or a waiter leaves the queue.
    void cancel(uint32_t id)
    {
        if (this->has_ && this->holder_ == id)
        {
            this->has_ = false;
            return;
        }
        this->remove(id);
    }

    // Any op issued anywhere: postpone the next grant.
    void holdOff(uint32_t now_s, uint32_t secs)
    {
        const uint32_t until = now_s + secs;
        if (!reached(this->hold_until_, until))
            this->hold_until_ = until;
    }

    void reset()
    {
        this->n_          = 0;
        this->has_        = false;
        this->hold_until_ = 0;
    }

    size_t waiting() const { return this->n_; }
    bool   held() const    { return this->has_; }
    uint32_t holder() const { return this->holder_; }

private:
    bool queued(uint32_t id) const
    {
        for (size_t i = 0; i < this->n_; ++i)
            if (this->q_[i] == id)
                return true;
        return false;
    }
    void remove(uint32_t id)
    {
        size_t w = 0;
        for (size_t i = 0; i < this->n_; ++i)
            if (this->q_[i] != id)
                this->q_[w++] = this->q_[i];
        this->n_ = w;
    }
    void popFront()
    {
        for (size_t i = 1; i < this->n_; ++i)
            this->q_[i - 1] = this->q_[i];
        --this->n_;
    }
    void expire(uint32_t now_s)
    {
        if (this->has_ && reached(now_s, this->lease_end_))
            this->has_ = false;
    }

    uint32_t q_[kCap]{};
    size_t   n_{0};
    bool     has_{false};
    uint32_t holder_{0};
    uint32_t lease_end_{0};
    uint32_t hold_until_{0};
};

// --- Per-node policy --------------------------------------------------------

struct Inputs
{
    bool     supervised      = false;  // Timed Mode ON, not an auto-mode node, grid running
    bool     session_ok      = false;  // confirmed session, no relogin pending
    bool     awake           = false;  // the hub believes the node is awake
    uint32_t s_since_grid_ack = 0xFFFFFFFFu;  // 0xFFFFFFFF = GridSync not acknowledged
    bool     promoted        = false;  // the node reported timed RX active (fresh)
    bool     busy            = false;  // THIS node has a cover op / sysop in flight
    bool     other_test      = false;  // a manual ModeTest / MAC ping / drift test runs
    bool     warmup_running  = false;  // OUR ModeTest is on the air
    bool     report_ok       = false;  // the ModeTest report showed Mode B windows
};

enum class Gate : uint8_t
{
    Ok = 0, NotSupervised, NoSession, Asleep, GridNotAcked, GridSettling,
    Promoted, Busy, OtherTest,
};

constexpr Gate commonGate(const Inputs &in)
{
    if (!in.supervised) return Gate::NotSupervised;
    if (!in.session_ok) return Gate::NoSession;
    if (!in.awake)      return Gate::Asleep;
    return Gate::Ok;
}

constexpr Gate startGate(const Inputs &in)
{
    if (in.s_since_grid_ack == 0xFFFFFFFFu) return Gate::GridNotAcked;
    if (in.s_since_grid_ack < kGridSettleS)  return Gate::GridSettling;
    if (in.promoted)                         return Gate::Promoted;
    if (in.busy)                             return Gate::Busy;
    if (in.other_test)                       return Gate::OtherTest;
    return Gate::Ok;
}

enum class Phase : uint8_t { Idle = 0, Queued, Warming, Verifying };

// What the glue must do. Queued/Yield/DropOut/Succeeded/Failed are also the log
// events; Start and (Yield|DropOut while a warm-up runs) change the radio.
enum class Event : uint8_t
{
    None = 0,
    Start,       // start the ModeTest now
    Yield,       // gave way to a cover op / busy node; abort a running warm-up
    DropOut,     // node left (asleep / session lost / token lost); abort
    Succeeded,   // promoted
    Failed,      // verify window expired without a promotion
};

class Supervisor
{
public:
    Event tick(const Inputs &in, uint32_t now_s, uint32_t rnd, Arbiter &arb, uint32_t id)
    {
        if (commonGate(in) != Gate::Ok)
            return this->dropOut(arb, id);
        switch (this->phase_)
        {
            case Phase::Idle:      return this->idle(in, now_s, rnd, arb, id);
            case Phase::Queued:    return this->queuedTick(in, now_s, arb, id);
            case Phase::Warming:   return this->warming(in, now_s, arb, id);
            default:               return this->verifying(in, now_s);
        }
    }

    // The node rebooted / re-logged-in / woke, or the switch was turned on:
    // whatever happened before no longer predicts anything. The caller aborts a
    // running warm-up.
    void restart(Arbiter &arb, uint32_t id)
    {
        arb.cancel(id);
        this->phase_     = Phase::Idle;
        this->failures_  = 0;
        this->not_before_ = 0;
        this->armed_     = false;
    }

    // A cover op / sysop was just issued for THIS node. Returns true when a
    // warm-up was on the air and has been given up (the caller aborts the
    // ModeTest). Not a failure: the backoff is untouched.
    bool yieldToOp(Arbiter &arb, uint32_t id, uint32_t now_s)
    {
        if (this->phase_ != Phase::Warming)
            return false;
        arb.cancel(id);
        this->phase_      = Phase::Idle;
        this->armed_      = false;
        this->not_before_ = now_s + kYieldRetryS;
        return true;
    }

    Phase    phase() const    { return this->phase_; }
    uint32_t failures() const { return this->failures_; }
    uint32_t notBefore() const { return this->not_before_; }
    bool     warmupInFlight() const
    {
        return this->phase_ == Phase::Warming || this->phase_ == Phase::Verifying;
    }

private:
    Event dropOut(Arbiter &arb, uint32_t id)
    {
        const bool was_warming = this->phase_ == Phase::Warming;
        if (this->phase_ != Phase::Idle)
            arb.cancel(id);
        this->phase_ = Phase::Idle;
        this->armed_ = false;
        return was_warming ? Event::DropOut : Event::None;
    }

    Event idle(const Inputs &in, uint32_t now_s, uint32_t rnd, Arbiter &arb, uint32_t id)
    {
        if (in.promoted)
        {
            this->failures_ = 0;
            this->armed_    = false;
            return Event::None;
        }
        if (!this->armed_)
        {
            this->armed_ = true;
            const uint32_t due = now_s + rnd % (kJitterMaxS + 1);
            if (!reached(this->not_before_, due))
                this->not_before_ = due;
        }
        if (!reached(now_s, this->not_before_) || startGate(in) != Gate::Ok)
            return Event::None;
        if (!arb.request(id))
            return Event::None;          // queue full: try again next tick
        this->phase_ = Phase::Queued;
        return this->queuedTick(in, now_s, arb, id);
    }

    Event queuedTick(const Inputs &in, uint32_t now_s, Arbiter &arb, uint32_t id)
    {
        const Gate g = startGate(in);
        if (g == Gate::Busy || g == Gate::OtherTest)
        {
            arb.cancel(id);
            this->phase_      = Phase::Idle;
            this->not_before_ = now_s + kYieldRetryS;
            return Event::Yield;
        }
        if (g != Gate::Ok)               // grid changed under us, or promoted meanwhile
        {
            arb.cancel(id);
            this->phase_ = Phase::Idle;
            return Event::None;
        }
        if (!arb.granted(id, now_s))
            return Event::None;
        this->phase_     = Phase::Warming;
        this->started_s_ = now_s;
        return Event::Start;
    }

    Event warming(const Inputs &in, uint32_t now_s, Arbiter &arb, uint32_t id)
    {
        if (!arb.holds(id, now_s))
        {
            this->phase_ = Phase::Idle;  // lease lost: someone else may start
            this->armed_ = false;
            return Event::DropOut;
        }
        if (in.warmup_running)
            return Event::None;
        arb.cancel(id);                  // the air is free for the next node
        this->phase_        = Phase::Verifying;
        this->verify_until_ = now_s + kVerifyGraceS;
        return Event::None;
    }

    Event verifying(const Inputs &in, uint32_t now_s)
    {
        if (in.promoted || in.report_ok)
        {
            this->failures_   = 0;
            this->phase_      = Phase::Idle;
            this->armed_      = false;
            this->not_before_ = now_s + kRearmHoldS;
            return Event::Succeeded;
        }
        if (!reached(now_s, this->verify_until_))
            return Event::None;
        ++this->failures_;
        this->phase_      = Phase::Idle;
        this->armed_      = false;
        this->not_before_ = now_s + backoffS(this->failures_);
        return Event::Failed;
    }

    Phase    phase_{Phase::Idle};
    bool     armed_{false};
    uint32_t failures_{0};
    uint32_t not_before_{0};
    uint32_t started_s_{0};
    uint32_t verify_until_{0};
};

}  // namespace modebsup

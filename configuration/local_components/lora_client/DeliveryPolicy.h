#pragma once

#include <stdint.h>

// Qualified when the build provides it (ESPHome and the hub test targets), the
// plain name otherwise. Same pattern as SingleShotAckWait.h itself.
#if __has_include("esphome/components/lora_client/SingleShotAckWait.h")
#include "esphome/components/lora_client/SingleShotAckWait.h"
#else
#include "SingleShotAckWait.h"
#endif

// ---------------------------------------------------------------------------
// The DECISIONS of hub->node command delivery, with no I/O and no clock.
//
// Step 1 of the "unified MAC delivery path" refactor. Everything here used to be
// an inline branch inside LORAListener's tracked-op code (on_op_retry_timeout_,
// op_retries_exhausted_, handle_command_ack_, ack_gridsync_); it is lifted out
// unchanged so it can be unit-tested (and mutation-tested) on its own, and so
// the later steps can hang the schedule / GridSync / config pushes on the same
// rules instead of their three bespoke timers.
//
// What a delivery attempt is, in one paragraph. A command is sent, then waited
// for. The first wait is either the NORMAL one (a burst's worth) or, for an
// optimistic single shot, the SHORT adaptive one (SingleShotAckWait.h). When the
// wait elapses nothing has been heard: the command is either given up, retried,
// or (short wait only) bursted without charging the budget. A retry that lands
// inside a relogin is deferred, uncharged. An ack names one msgid; a request may
// have been sent under several (a SET), and any of them completes it.
// ---------------------------------------------------------------------------

namespace deliverypolicy {

// ---- which first wait ------------------------------------------------------

// The FIRST wait of a command is the short one only for an optimistic single
// shot, before any retry was spent, and only once per command. (Thin name over
// singleshotwait::useShortFirstWait so callers speak one vocabulary.)
constexpr bool isShortFirstWait(bool first_shot_optimistic, uint8_t retries_spent,
                                bool short_wait_already_spent)
{
    return singleshotwait::useShortFirstWait(first_shot_optimistic, retries_spent,
                                             short_wait_already_spent);
}

// ---- what to do when a wait elapses ---------------------------------------

enum class TimeoutAction : uint8_t
{
    Stale,            // acked (or cancelled) already: nothing to do, do not re-arm
    DeferRelogin,     // a session is being rebuilt: keep waiting, charge nothing
    ShortWaitMissed,  // the optimistic guess missed: burst now, charge nothing
    GiveUp,           // the counted budget is spent
    Retransmit,       // a counted retry
};

struct TimeoutState
{
    bool    awaiting;          // a command is in flight and unacked
    bool    relogin_pending;   // the session is being rebuilt
    bool    short_wait;        // THIS timer was armed as the short first wait
    uint8_t retries_spent;     // counted retries so far
    uint8_t max_retries;       // the counted budget
};

struct TimeoutDecision
{
    TimeoutAction action;
    uint8_t       retries_spent;   // the counter AFTER this decision
};

// Order matters and is part of the contract: staleness first (a timer that
// outlived its command must do nothing at all), then the relogin deferral (even
// a short wait is deferred, not missed, while there is no session to answer on),
// then the uncharged short-wait miss, and only then the counted ladder. The
// counter is incremented BEFORE the comparison, so the retry that exceeds the
// budget is itself counted: with max_retries = 4 the 5th expiry is the give-up
// and leaves the counter at 5.
constexpr TimeoutDecision onAckTimeout(const TimeoutState &s)
{
    if (!s.awaiting)
        return {TimeoutAction::Stale, s.retries_spent};
    if (s.relogin_pending)
        return {TimeoutAction::DeferRelogin, s.retries_spent};
    if (s.short_wait)
        return {TimeoutAction::ShortWaitMissed, s.retries_spent};
    const uint8_t spent = (uint8_t) (s.retries_spent + 1);
    return {spent > s.max_retries ? TimeoutAction::GiveUp : TimeoutAction::Retransmit, spent};
}

// Whether an action spent from the counted budget.
constexpr bool isCountedRetry(TimeoutAction a)
{
    return a == TimeoutAction::Retransmit || a == TimeoutAction::GiveUp;
}

// Rule 4: a single shot that went unacked puts the node back on bursts. It is
// recorded once: only a command that went out as a single shot, and only if the
// belief does not already say so.
constexpr bool marksSingleShotUnacked(bool sent_single_shot, bool already_unacked)
{
    return sent_single_shot && !already_unacked;
}

// ---- what giving up does ---------------------------------------------------

enum class GiveUpAction : uint8_t
{
    FailRelogin,   // the session is presumed dead: mark failed, clear it, log in again
    FailKeep,      // the outcome is ambiguous (the node may have slept): keep the session
};

// `outcome_ambiguous`: silence proves nothing about the node's state, as for a
// command that makes the node go to sleep.
constexpr GiveUpAction giveUpAction(bool outcome_ambiguous)
{
    return outcome_ambiguous ? GiveUpAction::FailKeep : GiveUpAction::FailRelogin;
}

// ---- which acks complete a request ----------------------------------------

// A request is sent under one msgid, or under a RANGE of them when each
// retransmission mints a fresh one: an ack for any msgid in [first, last].
constexpr bool msgIdInRange(uint32_t id, uint32_t first, uint32_t last)
{
    return id >= first && id <= last;
}

// ...or under an explicit SET (GridSync keeps every published id). Linear: sets
// are a handful of entries.
constexpr bool msgIdInSet(const uint32_t *ids, unsigned count, uint32_t id)
{
    for (unsigned i = 0; i < count; ++i)
        if (ids[i] == id)
            return true;
    return false;
}

// An ack counts only while the command is still awaited, and only if it names a
// msgid of this command.
constexpr bool acceptsRangeAck(bool awaiting, uint32_t id, uint32_t first, uint32_t last)
{
    return awaiting && msgIdInRange(id, first, last);
}

}  // namespace deliverypolicy

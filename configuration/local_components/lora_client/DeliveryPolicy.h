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

// Position of `id` in the set, -1 when it is not a member.
constexpr int msgIdIndex(const uint32_t *ids, unsigned count, uint32_t id)
{
    for (unsigned i = 0; i < count; ++i)
        if (ids[i] == id)
            return (int) i;
    return -1;
}

// ---- the ack-id set of one request (step 3) -------------------------------

// Every msgid a request has been sent under, each with a TAG: whatever the
// attempt that carried it stood for (a ScheduleConfig's version). A request is
// re-built on every resend, so a late ack for an EARLIER attempt must apply what
// THAT attempt carried, not what the latest one does. The set is bounded: a full
// set forgets its oldest entry. Id 0 is "no id" and is never stored.
struct AckIds
{
    static constexpr unsigned kCap = 8;
    uint32_t id[kCap]{};
    uint32_t tag[kCap]{};
    uint8_t  n{0};

    void add(uint32_t new_id, uint32_t new_tag)
    {
        if (new_id == 0)
            return;
        if (this->n == kCap)
        {
            for (unsigned i = 1; i < kCap; ++i)
            {
                this->id[i - 1]  = this->id[i];
                this->tag[i - 1] = this->tag[i];
            }
            --this->n;
        }
        this->id[this->n]  = new_id;
        this->tag[this->n] = new_tag;
        ++this->n;
    }
    int find(uint32_t wanted) const { return msgIdIndex(this->id, this->n, wanted); }
    void clear() { this->n = 0; }
    uint32_t last() const { return this->n != 0 ? this->id[this->n - 1] : 0; }
};

// ---- what a request IS ------------------------------------------------------

enum class Mode : uint8_t { Acked, Fire };            // wait for an ack, or send and forget
// How the frame is put on the air. Auto: placed on the node's next mark, a single shot
// when the node has earned it. Burst: placed, as a full burst. ClassA: one copy aimed
// at the node's next open Class A window, a burst when the hub cannot place the node
// and nothing at all when the node is known to be asleep. Bare: a plain send, not
// placed on any mark (the transmit queue releases it when it drains).
enum class Shape : uint8_t { Auto, Burst, ClassA, Bare };
enum class Resend : uint8_t { SameBytes, Rebuild };   // a retry re-sends the stored frame / builds a new one

struct Policy
{
    Mode         mode;
    Shape        shape;
    Resend       resend;
    uint8_t      max_retries;           // the counted budget AFTER the first send
    uint32_t     base_wait_ms;          // the ack wait, before the transmit backlog is added
    GiveUpAction give_up;
    bool         at_most_once;          // never re-sent after it may have been heard (OTA)
    bool         supersedes_same_kind;  // a newer request of this kind replaces the one in flight
    bool         yields_warmup;         // a Mode B warm-up on the air is stopped for it
    // Fire only: with no session to encrypt under (a relogin in progress) the frame is
    // NOT sent -- a plaintext one is refused by a node that still holds its old session
    // -- and whoever confirms the session sends it again. A frame that must go out in
    // the clear to reach an unprovisioned node leaves this false.
    bool         defer_in_relogin = false;
};

// ScheduleConfig: a ladder of one send plus three rebuilds, 5 s each (plus the
// backlog). A failed push is only logged: the node keeps its old schedule, the
// session is fine, and the next beacon starts a fresh ladder.
constexpr Policy kSchedulePolicy = {Mode::Acked, Shape::Auto, Resend::Rebuild, 3, 5000,
                                    GiveUpAction::FailKeep, false, true, true};

// GridSync (enable): a full burst PLACED on a pre-planned grid mark, because a node
// being told about the grid is by definition not yet on it. Re-published up to five
// times, each one REBUILT so it re-declares the position it will actually occupy on
// the grid (a stale position would anchor the node wrong). An ack for any of the
// ids published (a SET) confirms the grid. A failed publish is only logged: the node
// stays in Mode A, the session is fine, and the next login or node request starts a
// fresh ladder.
constexpr Policy kGridSyncPolicy = {Mode::Acked, Shape::Burst, Resend::Rebuild, 5, 6000,
                                    GiveUpAction::FailKeep, false, true, false};

// ---- Fire requests (step 6): one transmission, no ack ----------------------------
//
// A Fire request is sent ONCE and forgotten: no retry ladder, no ack matching, no
// give-up. It occupies no channel -- it never touches a DeliverySlot, so it can
// neither cancel nor delay a ScheduleConfig / GridSync / cover-op ladder in flight
// on the channel its traffic class shares.

// TimeSync: one copy in the node's Class A window when the hub can place it, the
// burst it always was when it cannot (a TimeSync carries no ack, so a lost single
// copy is silent and airtime is the cheaper side of that trade), and nothing for a
// node whose windows have both closed. Deferred while the session is rebuilt:
// confirm_session_() sends it again.
constexpr Policy kTimeSyncPolicy = {Mode::Fire, Shape::ClassA, Resend::SameBytes, 0, 0,
                                    GiveUpAction::FailKeep, false, false, false, true};
// ClientConfig / CoverConfig: a bare send, encrypted whenever a session exists and in
// the clear only for the unprovisioned bootstrap. NOT deferred in a relogin: the
// REGISTER that provokes them arrives exactly when no session exists.
constexpr Policy kConfigPushPolicy = {Mode::Fire, Shape::Bare, Resend::SameBytes, 0, 0,
                                      GiveUpAction::FailKeep, false, false, false, false};

// Whether a Fire request is held back now. An Acked request is never held here (its
// own ladder defers its retries, see onAckTimeout).
constexpr bool fireDeferred(const Policy &p, bool relogin_pending)
{
    return p.mode == Mode::Fire && p.defer_in_relogin && relogin_pending;
}

// ---- the Control channel (step 5): cover operations and sysops --------------
//
// One command per node at a time, the ladder the hardware verified: the stored
// frame is re-sent byte for byte (same msgid, so the node re-acks it from its
// AckCache instead of running it twice), 3 s apart plus the transmit backlog,
// 4 counted retries. Out of retries the session is presumed dead (FailRelogin)
// -- except SLEEP, whose silence is ambiguous (the node may have slept and lost
// its ack): that one keeps the session (FailKeep).
constexpr uint8_t  kControlMaxRetries = 4;
constexpr uint32_t kControlWaitMs     = 3000;

// A cover operation: the latest one wins, so a newer one replaces an older one.
constexpr Policy kCoverPolicy = {Mode::Acked, Shape::Auto, Resend::SameBytes, kControlMaxRetries,
                                 kControlWaitMs, GiveUpAction::FailRelogin, false, true, true};
// A sysop (mode switch, debug toggles, ...): never replaced by a newer request.
constexpr Policy kSysopPolicy = {Mode::Acked, Shape::Auto, Resend::SameBytes, kControlMaxRetries,
                                 kControlWaitMs, GiveUpAction::FailRelogin, false, false, true};
// CMD_SLEEP: as a sysop, but giving up keeps the session.
constexpr Policy kSleepPolicy = {Mode::Acked, Shape::Auto, Resend::SameBytes, kControlMaxRetries,
                                 kControlWaitMs, GiveUpAction::FailKeep, false, false, true};
// CMD_OTA: as a sysop, but AT MOST ONCE -- it is never re-packed under a fresh
// msgid, because the node may already have run the first one.
constexpr Policy kOtaPolicy = {Mode::Acked, Shape::Auto, Resend::SameBytes, kControlMaxRetries,
                               kControlWaitMs, GiveUpAction::FailRelogin, true, false, true};

// Which of the four a Control command is. The sysop values are the caller's
// (ClientOperation), so this header stays free of the generated protobuf header.
constexpr const Policy &controlPolicy(bool is_cover, int32_t sysop, int32_t sleep_op, int32_t ota_op)
{
    return is_cover        ? kCoverPolicy
           : sysop == sleep_op ? kSleepPolicy
           : sysop == ota_op   ? kOtaPolicy
                               : kSysopPolicy;
}

// ---- one Control command at a time, the rest in line (step 5d) ---------------
//
// A node has ONE Control command in flight (a cover op or a sysop). A newer
// request never silently overwrites it:
//   * a newer COVER op replaces an older cover op, in flight or still waiting
//     ("the latest cover op wins": a blind only needs to end up where the user
//     last said);
//   * everything else WAITS, in order: a sysop (SLEEP, OTA, a mode switch, a
//     debug toggle) is never superseded, and a cover op never cancels one.
struct ControlCmd
{
    bool     is_cover;
    uint32_t covop_case;   // cover: which oneof arm
    int32_t  operation;    // cover: CovOperation
    float    position;     // cover: 0..1
    int32_t  sysop;        // sysop: ClientOperation
};

enum class ControlAdmit : uint8_t
{
    StartNow,   // nothing in flight
    Replace,    // a cover op over the cover op in flight
    Queue,      // behind the command in flight
};

constexpr ControlAdmit admitControl(bool busy, bool inflight_is_cover, bool new_is_cover)
{
    if (!busy)
        return ControlAdmit::StartNow;
    return (new_is_cover && inflight_is_cover) ? ControlAdmit::Replace : ControlAdmit::Queue;
}

// What waits behind the command in flight: a small FIFO, oldest first.
//
// At most ONE cover op is ever in it (a newer one replaces it, and moves to the
// back: the order is the order the user's latest intents were given in). When it
// is full a request is REFUSED -- the newest is dropped, never an older one: a
// queued SLEEP/OTA/mode switch was asked for and must not vanish silently.
struct ControlQueue
{
    static constexpr unsigned kDepth = 4;
    enum class Push : uint8_t { Queued, ReplacedCover, Full };

    ControlCmd q[kDepth]{};
    uint8_t    n{0};

    Push push(const ControlCmd &c)
    {
        bool replaced = false;
        if (c.is_cover)
            replaced = this->removeCover();
        if (this->n == kDepth)
            return Push::Full;
        this->q[this->n++] = c;
        return replaced ? Push::ReplacedCover : Push::Queued;
    }
    // The oldest, removed. False when nothing waits.
    bool pop(ControlCmd *out)
    {
        if (this->n == 0)
            return false;
        *out = this->q[0];
        this->erase(0);
        return true;
    }
    bool empty() const { return this->n == 0; }
    unsigned size() const { return this->n; }
    void clear() { this->n = 0; }
    bool hasSysop(int32_t sysop) const { return this->find(false, sysop) >= 0; }
    // Drop a waiting sysop. True when there was one.
    bool removeSysop(int32_t sysop)
    {
        const int i = this->find(false, sysop);
        if (i < 0)
            return false;
        this->erase((unsigned) i);
        return true;
    }
    bool hasCover() const { return this->find(true, 0) >= 0; }

private:
    int find(bool cover, int32_t sysop) const
    {
        for (unsigned i = 0; i < this->n; ++i)
            if (this->q[i].is_cover == cover && (cover || this->q[i].sysop == sysop))
                return (int) i;
        return -1;
    }
    bool removeCover()
    {
        const int i = this->find(true, 0);
        if (i < 0)
            return false;
        this->erase((unsigned) i);
        return true;
    }
    void erase(unsigned i)
    {
        for (; i + 1 < this->n; ++i)
            this->q[i] = this->q[i + 1];
        --this->n;
    }
};

// A ladder is live and still has a counted retry to spend: the push is "owed"
// to the pending-shadow and holds the onboarding gate. (The final wait, after
// the last retry, no longer does.)
constexpr bool ladderHasRetryLeft(bool awaiting, uint8_t retries_spent, uint8_t max_retries)
{
    return awaiting && retries_spent < max_retries;
}

}  // namespace deliverypolicy

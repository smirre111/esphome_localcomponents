#pragma once

#include <stdint.h>

#include "LoraTiming.h"
#include "TimedGrid.h"

// ---------------------------------------------------------------------------
// MAC-1 freshness query (mac-separation-implementation-plan.md section 2(b),
// "MAC-1 freshness query, final form"). Classifies an AUTHENTICATED downlink
// into one of four classes, each licensing a different subset of timing
// bookkeeping in CmdDispatcher::admitFrame — see that call site for which
// class may do what.
//
// Scope: only frames whose CMAC verified under the current session. LOGIN,
// CLIENTCONFIG, GRIDBEACON and GRIDDEMOTE are out of this query's scope —
// each has its own MIC/rate-limit rule elsewhere.
//
// Deliberately NOT a dependency on SessionManager: this header classifies
// from plain values the caller already has (or reads from SessionManager's
// own read-only accessors), so it stays as host-testable as every other
// policy header in this directory (see the project convention: decisions
// go in dependency-free headers).
// ---------------------------------------------------------------------------

namespace mac1freshness
{

enum class Class
{
    New,              // session_.wouldAcceptRxId(msgid) — genuinely new traffic
    CurrentBurstCopy,  // a duplicate of the burst currently being accepted
    Unsequenced,       // a MAC-ping whose own seq check passed, or MAC-1 is off
    Stale,             // everything else — no timing bookkeeping may trust it
};

// Worst-case time a `burst_count`-copy burst of `frame_len`-byte downlinks
// can still be arriving, measured from copy 0's accepted instant. This is
// what AckCache.h's kBurstSpanUs approximates with a FIXED 60-byte price —
// correct for a routine command, but too short for anything longer (e.g. a
// 152-byte ScheduleConfig takes ~96 ms on air per copy, not ~43 ms), so a
// late genuine copy of a long frame could misclassify as Stale rather than
// CurrentBurstCopy. This computes the real price from the frame actually
// seen instead of assuming one.
inline int64_t spanUs(uint32_t burst_count, uint32_t frame_len)
{
    const uint32_t copies = (burst_count == 0)
        ? 1u
        : (burst_count > loratiming::kBurstCopies ? loratiming::kBurstCopies : burst_count);
    const uint32_t on_air_us = loratiming::downlinkTimeOnAirUs(frame_len);
    const uint32_t stride_us = (loratiming::kBurstCopyStrideUs > on_air_us)
        ? loratiming::kBurstCopyStrideUs : on_air_us;
    return (int64_t) (copies - 1) * (int64_t) stride_us
         + (int64_t) on_air_us
         + (int64_t) timedgrid::kGuardUs;
}

struct Inputs
{
    // Whether this frame's CMAC verified under the current session
    // (was_encrypted at the call site). Gates New AND CurrentBurstCopy —
    // security review finding H1: msgid is a PLAINTEXT header field, so an
    // attacker can read the last-accepted one straight off the air and
    // replay it in a forged, unauthenticated frame; without this gate that
    // would misclassify as CurrentBurstCopy exactly as readily as a forged
    // LOGIN misclassified as New. Deliberately does NOT gate Unsequenced:
    // the MAC-ping-during-test exemption is intentionally plaintext (it
    // measures MAC-0 without MAC-2 overhead), so Unsequenced must still
    // reach a plaintext ping.
    bool     authenticated{false};
    // session_.wouldAcceptRxId(msgid) — read-only, must never itself consume
    // the id (that's acceptRxId's job, called separately). classify() ANDs
    // this with `authenticated` above, so the caller may pass the raw
    // result through unconditionally.
    bool     would_be_new{false};
    uint32_t msgid{0};
    // session_.lastAcceptedMsgid()/lastAcceptedUs() — the id and instant
    // acceptRxId() last returned true for.
    uint32_t last_accepted_msgid{0};
    int64_t  last_accepted_us{0};
    int64_t  now_us{0};
    uint32_t burst_count{0};
    uint32_t frame_len{0};
    // Unsequenced is scoped to MAC-CONTROL frames only, matching
    // MacSublayers.h's own documented scope — application traffic is never
    // exempt from MAC-1, switch on or off.
    bool     is_mac_control{false};
    // Already-evaluated `seq > mac_ping_seq_hw_`, read-only (the actual
    // counter update stays the caller's job, same ordering as today).
    bool     mac_ping_seq_ok{false};
    bool     mac1_counter_enabled{true};
};

inline Class classify(const Inputs &in)
{
    // MAC-CONTROL frames are sequenced by their OWN `seq` (checked by the
    // caller into mac_ping_seq_ok), never by the session's msgid/replay
    // window — admitFrame() never runs a MAC-control frame through
    // acceptRxId() at all (see MacSublayers.h's scope). New and
    // CurrentBurstCopy are both derived from that msgid window, so letting
    // a MAC-control frame reach either one is a category error, not just a
    // layering nicety: wouldAcceptRxId()'s window never advances for a
    // MAC-control msgid (nothing ever consumes it), so a REPLAYED MAC-ping
    // or MAC_CONFIG with a msgid ahead of the application stream's high-
    // water mark would read as New on every single replay. Checked first,
    // unconditionally — security review finding H1.
    if (in.is_mac_control)
        return (in.mac_ping_seq_ok || !in.mac1_counter_enabled) ? Class::Unsequenced : Class::Stale;

    // Security review finding H1: New and CurrentBurstCopy both require
    // `authenticated` — an unauthenticated frame (the concrete case: a
    // bad-MIC LOGIN, checked here before handleLogin() ever verifies it)
    // must never classify as either, or it holds off SyncStale demotion
    // indefinitely and feeds the unbounded anchor re-solve. msgid is a
    // PLAINTEXT header field, so CurrentBurstCopy needs the same gate: an
    // attacker can read the last-accepted one straight off the air and
    // replay it in a forged frame.
    if (!in.authenticated)
        return Class::Stale;

    if (in.would_be_new)
        return Class::New;

    if (in.msgid == in.last_accepted_msgid &&
        (in.now_us - in.last_accepted_us) < spanUs(in.burst_count, in.frame_len))
        return Class::CurrentBurstCopy;

    return Class::Stale;
}

}  // namespace mac1freshness

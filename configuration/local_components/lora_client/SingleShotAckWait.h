#pragma once

#include <stdint.h>

// Qualified when the build provides it (ESPHome, and the hub test targets that
// pick the node copies through the shims): a bare quote include would find the
// hub copy beside this file and clash with the node copy already included.
#if __has_include("esphome/components/lora_client/LoraTiming.h")
#include "esphome/components/lora_client/LoraTiming.h"
#else
#include "LoraTiming.h"
#endif

// ---------------------------------------------------------------------------
// How long the hub waits for the FIRST ack of an OPTIMISTIC single shot before
// it falls back to a burst. Hub-only (not vendored from the node repository).
//
// WHY IT IS DIFFERENT FROM kOpRetryIntervalMs (3000 ms). That interval is sized
// for a burst: ~1.5 s of copies plus a post-burst RX window, so the node's
// deferred ack has a quiet gap to land in (lora_client.h). An optimistic single
// shot is ONE frame on the node's mark. If the guess was wrong (the node is not
// in Mode B any more, or its clock drifted off the grid) nothing will ever
// answer, and waiting 3 s to find that out is 3 s of user-visible latency added
// on top of the burst that then has to be sent.
//
// THE FIRST VERSION OF THIS HEADER WAS WRONG, AND MEASURED SO (2026-10-08, node
// 2, Mode B, 8 optimistic ops). It derived the wait from geometry: "the node
// answers at T0 + 60 ms (+ its 290 ms worst backoff)", so mark + 500 ms. The
// hardware says the ack does not arrive then. Across ~47 clean single placed copies
// (43 non-optimistic, 4 optimistic, same node) the time from the MARK to the
// ack on the hub is 1.62 .. 2.17 s, whatever the distance to the mark: about
// one round (1.5 s) + the 60 ms reply offset + 0.1 .. 0.5 s. The signature is
// an ack that misses the SAME-round reply instant and is aimed at the NEXT
// round's (the node-side cause is not established from the hub log; the node
// repo is outside this change). 4 of 8 optimistic commands therefore burst
// (17 copies, ~715 ms of shared air) on top of a command that was about to be
// answered, and the hub, half-duplex, was deaf to the answer while it did.
//
// There was a second error: the caller then ran the result through
// ack_wait_ms_(), which ADDS the transmit queue's drain time, and that already
// contains the time to the mark. The time to the mark was counted twice.
//
// So the wait is no longer a constant derived on paper. It is
//
//     wait = max(time to the mark, queue drain) + tail          (all from now)
//     tail = clamp(max recent mark->ack delay of THIS node * 1.25,
//                  kFloorTailMs, kCeilingTailMs)
//
// and kDefaultTailMs (= the ceiling = the normal wait) until the node has any
// sample. The geometry survives only as the FLOOR's justification. The samples
// are what the hub actually saw: the mark->ack delay of every tracked op that
// went out as ONE placed copy and was acked without a burst fallback,
// optimistic or not (so the history exists before the switch is ever turned
// on), plus, on a MISS, the tail that was just proven too short (a lower bound;
// it ramps the next wait up instead of repeating the same mistake).
//
// Per node (one AckLatency per listener): a slow node must not slow 31 fast
// ones, and a fast node must not make a slow one burst. Samples age out
// (kSampleMaxAgeMs): a firmware update or a moved node must not be judged by
// last week's latency.
//
// A MISS IS NOT A FAILURE. The caller must not count the short wait against the
// retry budget (kOpMaxRetries): a wrong guess is the thing optimism accepts, and
// four of them in a row would otherwise tear down a healthy session via the
// "command failed after retries" relogin.
//
// WHY A LATE ACK IS NOT CANCELLED. When the deadline passes the burst goes out
// and the hub's receiver is off for its whole length (~1.5 s, half-duplex), so
// an ack that lands meanwhile is not heard at all; there is nothing to cancel
// the copies with. (Observed: the ack was due inside the burst, the one that was
// heard came 3.3 s after the mark.) The fix is not to start the burst early.
// ---------------------------------------------------------------------------

namespace singleshotwait {

// LORAClient::kUplinkOffsetUs, restated because this header is dependency-free.
// Asserted equal to the real one in the tests.
static constexpr uint32_t kNodeReplyOffsetUs      = 60000;
static constexpr uint32_t kLargestTrackedOpBytes  = 100;
static constexpr uint32_t kMeasuredTurnaroundUs   = 12000;
static constexpr uint32_t kAckPayloadBytes        = 25;
static constexpr uint32_t kNodePreCadBackoffMaxUs = 290000;
static constexpr uint32_t kHubReceiveSlackUs      = 100000;

constexpr uint32_t maxU32(uint32_t a, uint32_t b) { return a > b ? a : b; }

// The geometric bound for a SAME-round ack: used only to justify the floor.
// The measured ack is a round later; see above.
static constexpr uint32_t kTailBudgetUs =
      maxU32(kNodeReplyOffsetUs,
             loratiming::t0ToRxDoneUs(kLargestTrackedOpBytes) + kMeasuredTurnaroundUs)
    + loratiming::uplinkTimeOnAirUs(kAckPayloadBytes)
    + kNodePreCadBackoffMaxUs
    + kHubReceiveSlackUs;

// Tail limits, in ms AFTER the mark.
//   floor   : never below the same-round geometry with room to spare, even for
//             a node that has always answered instantly.
//   ceiling : the normal retry interval. This is an optimisation of the MISS
//             case, never a way to wait longer than the non-optimistic path.
//   default : no history -> the normal wait (measured max 2.17 s, 0 retransmits
//             in 43 ops at 3 s). Nothing is gained, nothing is risked.
static constexpr uint32_t kFloorTailMs   = 1000;
static constexpr uint32_t kCeilingTailMs = 3000;
static constexpr uint32_t kDefaultTailMs = kCeilingTailMs;
static_assert(kTailBudgetUs <= kFloorTailMs * 1000u,
              "the floor must cover the same-round geometry");
static_assert(kFloorTailMs <= kDefaultTailMs && kDefaultTailMs <= kCeilingTailMs,
              "floor <= default <= ceiling");

// Margin over the worst recent delay: 25 % (jitter in the node's random backoff
// and the hub's 10 ms poll). No separate minimum: below ~800 ms the floor
// already dominates, so a minimum margin could never show.
static constexpr uint32_t kMarginPercent = 25;

// Samples older than this are ignored (the node may have been reflashed).
static constexpr uint32_t kSampleMaxAgeMs = 12u * 60u * 60u * 1000u;

// A delay above this is not a latency, it is a bug (a stale mark, a stalled
// loop): it is dropped rather than clamped, so it cannot move the wait at all.
static constexpr uint32_t kPlausibleMaxDelayMs = 10000;

// Rolling window per node. Small on purpose: 8 samples is 8 commands, and 32
// nodes cost 32 * 8 * 12 bytes.
static constexpr unsigned kSamples = 8;

// Rolling record of one node's mark->ack delays. Time is a wrapping uint32 ms
// clock supplied by the caller (differences are wrap-safe for < 49 days).
struct AckLatency
{
    struct Sample { uint32_t at_ms; uint32_t delay_ms; bool used; };
    Sample   s[kSamples] = {};
    unsigned next = 0;

    // One delay, clamped to the ceiling so a single freak value (a retry's ack,
    // a stalled hub loop) cannot do more than push the wait to the normal one.
    // Implausible values (> kPlausibleMaxDelayMs) are dropped. Returns whether
    // the sample was kept.
    bool note(uint32_t now_ms, uint32_t delay_ms)
    {
        if (delay_ms > kPlausibleMaxDelayMs)
            return false;
        s[next] = Sample{now_ms, delay_ms > kCeilingTailMs ? kCeilingTailMs : delay_ms, true};
        next = (next + 1) % kSamples;
        return true;
    }

    bool fresh(const Sample &x, uint32_t now_ms) const
    {
        return x.used && (uint32_t) (now_ms - x.at_ms) <= kSampleMaxAgeMs;
    }

    unsigned count(uint32_t now_ms) const
    {
        unsigned n = 0;
        for (const Sample &x : s) if (fresh(x, now_ms)) ++n;
        return n;
    }

    // The largest fresh delay, 0 when there is none.
    uint32_t maxDelayMs(uint32_t now_ms) const
    {
        uint32_t m = 0;
        for (const Sample &x : s) if (fresh(x, now_ms) && x.delay_ms > m) m = x.delay_ms;
        return m;
    }

    // Ms after the mark this node's first ack is waited for.
    uint32_t tailMs(uint32_t now_ms) const
    {
        if (count(now_ms) == 0)
            return kDefaultTailMs;
        const uint32_t m      = maxDelayMs(now_ms);
        uint32_t t = m + (uint32_t) (((uint64_t) m * kMarginPercent) / 100u);
        if (t < kFloorTailMs)   t = kFloorTailMs;
        if (t > kCeilingTailMs) t = kCeilingTailMs;
        return t;
    }
};

// The first-ack wait for an optimistic single shot, measured from NOW.
// `us_until_t0` : time to the placed mark (<= 0: already due).
// `drain_ms`    : the transmit queue's drain time (frames ahead of this one can
//                 push the real air time past the mark; it includes the mark
//                 when the frame is placed). Whichever is later governs, NOT
//                 their sum: they are two estimates of the same instant.
// `tail_ms`     : AckLatency::tailMs().
constexpr uint32_t firstAckWaitMs(int64_t us_until_t0, uint32_t drain_ms, uint32_t tail_ms)
{
    const uint32_t to_mark_ms =
        (us_until_t0 <= 0) ? 0u : (uint32_t) ((us_until_t0 + 999) / 1000);
    return maxU32(to_mark_ms, drain_ms) + tail_ms;
}

// Whether the first wait of a tracked op is the short one: only when the first
// shot was an optimistic single shot, only before any retry has been spent, and
// only once per command.
constexpr bool useShortFirstWait(bool first_shot_optimistic, uint8_t retries_spent,
                                 bool short_wait_already_spent)
{
    return first_shot_optimistic && retries_spent == 0 && !short_wait_already_spent;
}

// Whether an ack is a CLEAN latency sample: the command went out as ONE placed
// copy, nothing was retransmitted and no fallback burst was started (a burst
// blinds the hub and defers the ack, so its delay says nothing about the
// single-shot path), and the mark is known.
constexpr bool isCleanSample(bool sent_single_shot, uint8_t retries_spent,
                             bool burst_fallback_spent, bool mark_known)
{
    return sent_single_shot && retries_spent == 0 && !burst_fallback_spent && mark_known;
}

}  // namespace singleshotwait

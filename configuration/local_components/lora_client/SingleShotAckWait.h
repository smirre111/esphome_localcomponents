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
// on top of the burst that then has to be sent. The wait only has to cover
// "frame lands, node answers", so it can be much shorter.
//
// THE WAIT IS MEASURED FROM PLACEMENT, NOT FROM FIRE. send_aligned_ hands the
// frame to the transmit queue with earliest_us = the node's next mark clear of
// the hub's own burst, which can be up to a whole round (1.5 s) away. The retry
// timer is armed at that same instant. A flat 500 ms would therefore expire
// BEFORE the frame was even on the air for roughly two thirds of commands and
// burst on top of a command still waiting for its mark. So:
//
//     wait = time until the mark (T0) + kTailMs        (capped at the normal wait)
//
// kTailMs covers what happens AFTER T0:
//     node reply           max(60 ms, RxDone(100 B) + 12 ms)
//                                                the node answers at T0 + 60 ms
//                                                (LORAClient::kUplinkOffsetUs), or
//                                                after the frame has been received
//                                                and turned around if that is
//                                                later; 100 B is a generous bound
//                                                for a tracked cover op / sysop
//                                                and 12 ms the measured p99 MAC
//                                                turnaround (11.6 ms, 2026-10-04)
//     ack on the air       uplinkTimeOnAirUs(25 B)
//     node pre-CAD backoff 290 ms               worst case of the node's 20-290 ms
//                                                backoff (implementation-plan.md,
//                                                section 12 / M1 row)
//     hub receive slack    100 ms                loop() polls with a 10 ms delay and
//                                                the ack is decrypted before it is
//                                                matched
// The sum is asserted below to fit inside 500 ms, so the constant cannot drift
// away from the geometry it was derived from. It is ~487 ms: the 500 ms is not
// padded, it is dominated by the node's worst-case backoff, which is the term
// to revisit if the node ever drops its unconditional pre-CAD backoff in Mode B.
//
// A MISS IS NOT A FAILURE. The caller must not count the short wait against the
// retry budget (kOpMaxRetries): a wrong guess is the thing optimism accepts, and
// four of them in a row would otherwise tear down a healthy session via the
// "command failed after retries" relogin.
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

static constexpr uint32_t kTailMs = 500;

constexpr uint32_t maxU32(uint32_t a, uint32_t b) { return a > b ? a : b; }

static constexpr uint32_t kTailBudgetUs =
      maxU32(kNodeReplyOffsetUs,
             loratiming::t0ToRxDoneUs(kLargestTrackedOpBytes) + kMeasuredTurnaroundUs)
    + loratiming::uplinkTimeOnAirUs(kAckPayloadBytes)
    + kNodePreCadBackoffMaxUs
    + kHubReceiveSlackUs;

static_assert(kTailBudgetUs <= kTailMs * 1000u,
              "kTailMs must cover everything that happens after the mark");

// The first-ack wait for an optimistic single shot placed at `us_until_t0`
// from now (negative or zero: the mark is already due). Never longer than the
// normal wait: this is an optimisation of the miss case, not a new way to wait
// longer.
constexpr uint32_t firstAckWaitMs(int64_t us_until_t0, uint32_t normal_ms)
{
    const uint32_t to_mark_ms =
        (us_until_t0 <= 0) ? 0u : (uint32_t) ((us_until_t0 + 999) / 1000);
    const uint32_t wait = to_mark_ms + kTailMs;
    return (wait < normal_ms) ? wait : normal_ms;
}

// Whether the first wait of a tracked op is the short one: only when the first
// shot was an optimistic single shot, only before any retry has been spent, and
// only once per command.
constexpr bool useShortFirstWait(bool first_shot_optimistic, uint8_t retries_spent,
                                 bool short_wait_already_spent)
{
    return first_shot_optimistic && retries_spent == 0 && !short_wait_already_spent;
}

}  // namespace singleshotwait

#pragma once

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

// See SingleShotAckWait.h for why these are qualified when possible.
#if __has_include("esphome/components/lora_client/PendingData.h")
#include "esphome/components/lora_client/PendingData.h"
#else
#include "PendingData.h"
#endif
#if __has_include("esphome/components/lora_client/TimedGrid.h")
#include "esphome/components/lora_client/TimedGrid.h"
#else
#include "TimedGrid.h"
#endif

// ---------------------------------------------------------------------------
// SHADOW MODE for the pending-data bitmap (Tier 3, implementation-plan.md
// section 4.4) — the HUB's half of the decision, computed and LOGGED but never
// transmitted.
//
// PendingData.h is the node's half (and the vendored, shared one): how a node
// reads a mask. This header is the hub-only counterpart: which bits the hub
// WOULD clear if it were allowed to. Kept separate because PendingData.h is
// vendored byte-for-byte from the node repository (regen_stubs.sh), so a
// hub-only decision cannot live in it without drifting from the node copy.
//
// WHY SHADOW. A clear bit is a promise that this hub has nothing for the node
// for up to one beacon interval (~5.8 min), made to a node that will then not
// listen. Breaking it costs a command's latency. Before the hub is allowed to
// make it, the policy has to be observed against real traffic: how often would
// a bit have been cleared, and — the important half — was anything addressed to
// that node inside the interval a clear bit would have covered? serviceBeacon
// logs the answer once per beacon and still transmits allListening().
//
// ONE DECISION, LISTEN BY DEFAULT. A node's bit is SET (listen) if ANY reason
// below holds; it is CLEAR only when none does. Every new condition is
// therefore a new reason to listen, and a state this code was not told about
// leaves the bit set. The reasons that can be wrong in the dangerous direction
// (a command is on its way) are the first four.
//
// SLOT SHARING. The mask has one bit per SLOT and several nodes can share a
// slot (login_slot_ % 32 wraps), so a slot's bit is the OR of its nodes'.
// A slot nobody on this hub owns stays SET: it may belong to a node this hub
// has not been told about, and unknown means listen (PendingData.h's principle).
//
// Dependency-free apart from PendingData.h / TimedGrid.h.
// ---------------------------------------------------------------------------

namespace pendingshadow {

// Reasons a node's bit must be SET. A bit per reason so a log line can name
// them all, and so a test can assert exactly which one held.
enum Reason : uint32_t {
    kOpAwaitingAck        = 1u << 0,   // a tracked command/sysop (OTA included) unacked
    kOpDeferredUntilLogin = 1u << 1,   // a command parked behind a relogin
    kReloginPending       = 1u << 2,   // session being (re)built
    kPushAwaitingAck      = 1u << 3,   // schedule / config / GridSync not yet confirmed
    kTimeSyncDue          = 1u << 4,
    kQueuedFrames         = 1u << 5,   // something placed or queued on the hub's air
    kTimedModeOff         = 1u << 6,   // not on the grid: the bit means nothing to it
    kNotLatencyTolerant   = 1u << 7,   // HA can command it at any instant
    kRecentTraffic        = 1u << 8,   // heard / addressed inside the last 3 intervals
};

// How many beacon intervals of silence before a node counts as quiet.
static constexpr int64_t kQuietIntervals = 3;

// Everything the decision reads about ONE node. Plain values so a test can
// build any state by hand and so the listener fills it in one place.
struct NodeInputs {
    uint8_t slot{0};                    // already reduced modulo the slot count
    bool    op_awaiting_ack{false};
    bool    op_deferred_until_login{false};
    bool    relogin_pending{false};
    bool    push_awaiting_ack{false};
    bool    timesync_due{false};
    bool    queued_frames{false};
    bool    timed_mode_off{true};       // default: listen
    bool    latency_tolerant{false};    // default: listen
    // Microseconds since the node was last heard from or addressed; negative =
    // never. Never reads as NOT recent, which would clear the bit of a node the
    // hub has no history for — so the caller must leave the node otherwise
    // listening; see recentTraffic().
    int64_t since_last_traffic_us{-1};
};

// A node with no history at all is not "quiet", it is unknown, and unknown
// listens. So "never" is treated as recent.
constexpr bool recentTraffic(int64_t since_us, int64_t beacon_interval_us)
{
    if (since_us < 0) return true;
    return since_us < kQuietIntervals * beacon_interval_us;
}

constexpr uint32_t listenReasons(const NodeInputs &n, int64_t beacon_interval_us)
{
    return (n.op_awaiting_ack          ? kOpAwaitingAck        : 0u)
         | (n.op_deferred_until_login  ? kOpDeferredUntilLogin : 0u)
         | (n.relogin_pending          ? kReloginPending       : 0u)
         | (n.push_awaiting_ack        ? kPushAwaitingAck      : 0u)
         | (n.timesync_due             ? kTimeSyncDue          : 0u)
         | (n.queued_frames            ? kQueuedFrames         : 0u)
         | (n.timed_mode_off           ? kTimedModeOff         : 0u)
         | (!n.latency_tolerant        ? kNotLatencyTolerant   : 0u)
         | (recentTraffic(n.since_last_traffic_us, beacon_interval_us)
                                       ? kRecentTraffic        : 0u);
}

static constexpr size_t kMaxNodes = 32;

struct Verdict {
    uint32_t mask{pending::allListening()};   // the would-be pending mask
    uint32_t cleared_slots{0};                // bits that differ from allListening
    uint32_t cleared_nodes{0};                // bit i = nodes[i] would have been cleared
    bool     any_cleared{false};
};

// `reasons_out` (nullable) receives each node's own reasons, whether or not
// its slot ended up cleared — a node can have none and still share a listening
// slot, and the log should say so.
inline Verdict shadowMask(const NodeInputs *nodes, size_t n,
                          int64_t beacon_interval_us, uint32_t *reasons_out = nullptr)
{
    uint32_t owned  = 0;   // slots with at least one known node
    uint32_t listen = 0;   // of those, slots with at least one reason to listen
    for (size_t i = 0; i < n; ++i) {
        const uint32_t r = listenReasons(nodes[i], beacon_interval_us);
        if (reasons_out != nullptr) reasons_out[i] = r;
        const uint32_t slot = nodes[i].slot;
        if (slot >= timedgrid::kSlotCount) continue;   // unassigned: owns nothing
        const uint32_t bit = 1u << slot;
        owned |= bit;
        // Past kMaxNodes the per-node bookkeeping below cannot name the node,
        // so refuse to clear anything for it rather than clear unaccounted.
        if (r != 0 || i >= kMaxNodes) listen |= bit;
    }
    Verdict v;
    const uint32_t cleared = owned & ~listen;
    v.mask          = pending::allListening() & ~cleared;
    v.cleared_slots = cleared;
    v.any_cleared   = cleared != 0;
    for (size_t i = 0; i < n && i < kMaxNodes; ++i) {
        const uint32_t slot = nodes[i].slot;
        if (slot < timedgrid::kSlotCount && (cleared & (1u << slot)) != 0)
            v.cleared_nodes |= (1u << i);
    }
    return v;
}

// Short names for the log line, in Reason order.
inline const char *reasonName(uint32_t reason_bit)
{
    switch (reason_bit) {
    case kOpAwaitingAck:        return "op-ack";
    case kOpDeferredUntilLogin: return "op-deferred";
    case kReloginPending:       return "relogin";
    case kPushAwaitingAck:      return "push";
    case kTimeSyncDue:          return "timesync";
    case kQueuedFrames:         return "queued";
    case kTimedModeOff:         return "timed-off";
    case kNotLatencyTolerant:   return "not-tolerant";
    case kRecentTraffic:        return "recent";
    default:                    return "?";
    }
}

// Appends one reason name (comma-separated after the first) at buf + used.
// Returns false when the caller must stop: the buffer is full or snprintf
// failed. `used` always stays below `cap`, so the buffer stays terminated.
inline bool appendReason(char *buf, size_t cap, size_t &used, uint32_t reason_bit)
{
    if (used + 1 >= cap) return false;
    const int w = snprintf(buf + used, cap - used, "%s%s", used ? "," : "",
                           reasonName(reason_bit));
    if (w < 0) return false;
    used += (size_t) w;
    if (used >= cap) { used = cap - 1; return false; }
    return true;
}

// "op-ack,push" for a reason set; "none" for an empty one. Always terminates.
inline void describeReasons(uint32_t reasons, char *buf, size_t cap)
{
    if (buf == nullptr || cap == 0) return;
    buf[0] = '\0';
    if (reasons == 0) { snprintf(buf, cap, "none"); return; }
    size_t used = 0;
    for (uint32_t b = 1; b != 0 && b <= kRecentTraffic; b <<= 1) {
        if ((reasons & b) == 0) continue;
        if (!appendReason(buf, cap, used, b)) break;
    }
}

}  // namespace pendingshadow
